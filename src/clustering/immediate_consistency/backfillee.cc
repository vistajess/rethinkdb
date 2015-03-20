// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/immediate_consistency/backfillee.hpp"

#include "clustering/immediate_consistency/history.hpp"

/* `PRE_ATOM_PIPELINE_SIZE` is the maximum combined size of the pre-atoms that we send to
the backfiller that it hasn't consumed yet. So the other server may use up to this size
for its pre-atom queue. `PRE_ATOM_CHUNK_SIZE` is the typical size of a pre-atom message
we send over the network. */
static const size_t PRE_ATOM_PIPELINE_SIZE = 4 * MEGABYTE;
static const size_t PRE_ATOM_CHUNK_SIZE = 100 * KILOBYTE;

backfillee_t::backfillee_t(
        mailbox_manager_t *_mailbox_manager,
        branch_history_manager_t *_branch_history_manager,
        store_view_t *_store,
        const backfiller_bcard_t &backfiller,
        signal_t *interruptor) :
    mailbox_manager(_mailbox_manager),
    branch_history_manager(_branch_history_manager),
    store(_store),
    range_to_backfill(store->get_region().inner),
    pre_atom_throttler(PRE_ATOM_PIPELINE_SIZE),
    ack_pre_atoms_mailbox(mailbox_manager,
        std::bind(&backfillee::on_ack_pre_atoms, this, ph::_1, ph::_2, ph::_3));
{
    guarantee(region_is_superset(backfiller.region, store->get_region()));
    guarantee(store->get_region().beg == backfiller.region.beg);
    guarantee(store->get_region().end == backfiller.region.end);

    backfiller_bcard_t::intro_1_t our_intro;
    our_intro.ack_pre_atoms_mailbox = ack_pre_atoms_mailbox.get_address();
    our_intro.atoms_mailbox = atoms_mailbox.get_address();

    /* Fetch the `initial_version` and `initial_version_history` fields for the
    `intro_1_t` that we'll send to the backfiller */
    {
        region_map_t<binary_blob_t> initial_state_blob;
        read_token_t read_token;
        store->new_read_token(&read_token);
        store->do_get_metainfo(order_token_t::ignore.with_read_mode(), &read_token,
            interruptor, &initial_state_blob);
        our_intro.initial_version = to_version_map(initial_state_blob);
        branch_history_manager->export_branch_history(
            our_intro.initial_version,
            &our_intro.initial_version_history);
    }

    /* Set up a mailbox to receive the `intro_2_t` from the backfiller */
    cond_t got_intro;
    mailbox_t<void(backfiller_bcard_t::intro_2_t)> intro_mailbox(
        mailbox_manager,
        [&](signal_t *, const backfiller_bcard_t::intro_2_t &i) {
            intro = i;
            got_intro.pulse();
        });
    our_intro.intro_mailbox = intro_mailbox.get_address();

    /* Send the `intro_1_t` to the backfiller and wait for it to send back the
    `intro_2_t` */
    registrant.init(new registrant_t<backfiller_bcard_t::intro_1_t>(
        mailbox_manager,
        backfiller.registrar,
        backfillee_bcard_t {
            intro_mailbox.get_address()
        }));
    wait_interruptible(&got_intro, interruptor);

    /* Spawn the coroutine that will stream pre-atoms to the backfiller. */
    coro_t::spawn_sometime(
        std::bind(&backfillee_t::send_pre_atoms, this, drainer.lock()));
}

bool backfillee_t::go(callback_t *callback, signal_t *interruptor) {
    guarantee(current_session == nullptr);
    session_info_t info;
    info.callback = callback;
    info.callback_returned_false = false;
    info.session_id = generate_uuid();
    assignment_sentry_t<session_info_t *> sentry(&current_session, &info);

    send(mailbox_manager, intro.start_mailbox,
        fifo_source.enter_write(), info.session_id, to_backfill);

    wait_interruptible(&info.stop, interruptor);

    if (info.callback_returned_false) {
        send(mailbox_manager, intro.stop_mailbox,
            fifo_source.enter_write(), info.session_id);
        return false;
    } else {
        guarantee(range_to_backfill.is_empty());
        return true;
    }
}

void backfillee_t::on_ack_pre_atoms(
        signal_t *interruptor,
        const fifo_enforcer_write_token_t &fifo_token,
        size_t pre_atoms_size) [
    /* Ordering of these messages is actually irrelevant; we just pass through the fifo
    sink for consistency */
    fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, fifo_token);
    wait_interruptible(&exit_write, interruptor);

    /* Shrink `pre_atom_throttler` so that `send_pre_atoms()` can acquire the semaphore
    again. */
    guarantee(pre_atoms_size <= pre_atom_throttler_acq.count());
    if (pre_atoms_size == pre_atom_throttler_acq.count()) {
        /* It's illegal to `set_count(0)`, so we need to do this instead */
        pre_atom_throttler_acq.reset();
    } else {
        pre_atom_throttler_acq.set_count(
            pre_atom_throttler_acq.count() - pre_atoms_size);
    }
}

void backfillee_t::on_atoms(
        signal_t *interruptor,
        const fifo_enforcer_write_token_t &fifo_token,
        const session_id_t &session,
        const region_map_t<version_t> &version,
        const std::deque<backfill_atom_t> &chunk) {
    /* Find the session that this chunk is a part of */
    session_info_t *session = current_session;
    if (session == nullptr) {
        /* The session was aborted */
        return;
    }
    auto_drainer_t::lock_t keepalive = session->drainer.lock();

    /* Get into line for the underlying store and the session callback mutex */
    write_token_t write_token;
    object_buffer_t<new_mutex_in_line_t> mutex_in_line;
    {
        fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, fifo_token);
        wait_interruptible(&exit_write, interruptor);
        store->new_write_token(&write_token);
        mutex_in_line.create(&session->mutex);
    }

    /* Actually record the data in the underlying store */
    store->receive_backfill(
        from_version_map(version),
        chunk,
        &write_token,
        interruptor);

    /* Wait until it's our turn to access the session callback. This ordering is
    important because `callback->on_progress()` must be called in order from left to
    right. */
    wait_interruptible(mutex_in_line->acq_signal(), interruptor);

    if (session->callback_returned_false) {
        /* Ensure that we don't call `on_progress()` again if it returned `false` */
        return;
    }

    region_t region_done = version.get_domain().inner;
    guarantee(region_done.beg == store->get_region().beg);
    guarantee(region_done.end == store->get_region().end);
    guarantee(region_done.inner.left == range_to_backfill.left);
    guarantee(region_done.inner.right <= range_to_backfill.right);

    if (session->callback->on_progress(version)) {
        /* Tell the backfiller that the chunk was accepted. This has two purposes: it
        lets the backfiller forget the pre-atoms for that range, and it tells the
        backfiller that it's OK to send more atoms. */
        size_t chunk_size = 0;
        for (const backfill_atom_t &atom : chunk) {
            chunk_size += atom.size();
        }
        send(mailbox_manager, intro.ack_atoms_mailbox,
            fifo_source.enter_write(), session_id, region_done, chunk_size);

        if (range_done.inner == range_to_backfill) {
            /* This was the last chunk. We're done with the backfill. */
            range_to_backfill = key_range_t::empty();
            session->stop.pulse();
        } else {
            range_to_backfill.left = region_done.inner.right.key;
        }
    } else {
        /* End the session prematurely */
        session->callback_returned_false = true;
        session->stop.pulse();
    }
}

void backfillee_t::send_pre_atoms(auto_drainer_t::lock_t keepalive) {
    try {
        key_range_t range_to_do = store->get_region().inner;
        bool done = false;
        while (!done) {
            /* Wait until there's room in the semaphore for the chunk we're about to
            process */
            new_semaphore_acq_t sem_acq(&pre_atom_throttler, PRE_ATOM_CHUNK_SIZE);
            wait_interruptible(&sem_acq, keepalive.get_drain_signal());

            /* Copy pre-atoms from the store into `callback_t::atoms` until the total
            size hits `PRE_ATOM_CHUNK_SIZE` or we finish the range */
            class callback_t : public store_view_t::backfill_pre_callback_t {
            public:
                callback_t(const store_key_t &start) :
                    size(0), range { start, key_range_t::right_bound_t(start) }  { }
                bool on_pre_atom(backfill_pre_atom_t &&atom) {
                    atoms.push_back(atom);
                    size += atom.size();
                    return (size < PRE_ATOM_CHUNK_SIZE);
                }
                void on_done_range(const key_range_t &r) {
                    rassert(range.right == key_range_t::right_bound_t(r.left));
                    range.right = r.right;
                }
                std::deque<backfill_pre_atom_t> pre_atoms;
                key_range_t range;
                size_t size;
            } callback(range_to_do.left);

            store->send_backfill_pre(
                intro.common_version, &callback, keepalive.get_drain_signal());

            /* Transfer the semaphore ownership */
            sem_acq.set_count(callback.size);   /* this should be a small change */
            pre_atom_throttler_acq.transfer_in(std::move(sem_acq));

            /* Send the chunk over the network */
            send(mailbox_manager, intro.pre_atoms_mailbox,
                fifo_source.enter(), callback.range, callback.pre_atoms);

            /* Update `range_to_do` */
            guarantee(region_is_superset(to_do, callback.range));
            guarantee(range_to_do.left == callback.range.left);
            if (callback.range.right == range_to_do.right) {
                /* We did the whole range so now we're done. */
                done = true;
            } else {
                guarantee(callback.range.right < range_to_do.right);
                range_to_do.left = callback.range.right.key;
            }
        }
    } catch (const interrupted_exc_t &) {
        /* The `backfillee_t` was deleted; the backfill is being aborted. */
    }
}
