/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rec_update_save --
 *     Save a WT_UPDATE list for later restoration.
 */
static int
__rec_update_save(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_UPDATE *onpage_upd, size_t upd_memsize)
{
    WT_RET(__wt_realloc_def(session, &r->supd_allocated, r->supd_next + 1, &r->supd));
    r->supd[r->supd_next].ins = ins;
    r->supd[r->supd_next].ripcip = ripcip;
    r->supd[r->supd_next].onpage_upd = onpage_upd;
    ++r->supd_next;
    r->supd_memsize += upd_memsize;
    return (0);
}

/*
 * __rec_append_orig_value --
 *     Append the key's original value to its update list.
 */
static int
__rec_append_orig_value(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd, WT_CELL_UNPACK *unpack)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_UPDATE *append;
    size_t size;

    /* Done if at least one self-contained update is globally visible. */
    for (;; upd = upd->next) {
        if (WT_UPDATE_DATA_VALUE(upd) && __wt_txn_upd_visible_all(session, upd))
            return (0);

        /* Add the original value after birthmarks. */
        if (upd->type == WT_UPDATE_BIRTHMARK) {
            WT_ASSERT(session, unpack != NULL && unpack->type != WT_CELL_DEL);
            break;
        }

        /* Leave reference at the last item in the chain. */
        if (upd->next == NULL)
            break;
    }

    /*
     * We need the original on-page value for some reader: get a copy and
     * append it to the end of the update list with a transaction ID that
     * guarantees its visibility.
     *
     * If we don't have a value cell, it's an insert/append list key/value
     * pair which simply doesn't exist for some reader; place a deleted
     * record at the end of the update list.
     */
    append = NULL; /* -Wconditional-uninitialized */
    size = 0;      /* -Wconditional-uninitialized */
    if (unpack == NULL || unpack->type == WT_CELL_DEL)
        WT_RET(__wt_update_alloc(session, NULL, &append, &size, WT_UPDATE_TOMBSTONE));
    else {
        WT_RET(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_page_cell_data_ref(session, page, unpack, tmp));
        WT_ERR(__wt_update_alloc(session, tmp, &append, &size, WT_UPDATE_STANDARD));
    }

    /*
     * If we're saving the original value for a birthmark, transfer over
     * the transaction ID and clear out the birthmark update.
     *
     * Else, set the entry's transaction information to the lowest possible
     * value. Cleared memory matches the lowest possible transaction ID and
     * timestamp, do nothing.
     */
    if (upd->type == WT_UPDATE_BIRTHMARK) {
        append->txnid = upd->txnid;
        append->start_ts = upd->start_ts;
        append->durable_ts = upd->durable_ts;
        append->next = upd->next;
    }

    /* Append the new entry into the update list. */
    WT_PUBLISH(upd->next, append);
    __wt_cache_page_inmem_incr(session, page, size);

    if (upd->type == WT_UPDATE_BIRTHMARK) {
        upd->type = WT_UPDATE_STANDARD;
        upd->txnid = WT_TXN_ABORTED;
    }

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __get_next_rec_upd --
 *     Return the next update in a list, considering both lookaside and in-memory updates.
 */
static int
__get_next_rec_upd(WT_SESSION_IMPL *session, WT_UPDATE **inmem_upd_pos, bool *las_positioned,
  WT_CURSOR *las_cursor, uint32_t btree_id, WT_ITEM *key, WT_UPDATE **upd)
{
    WT_DECL_ITEM(las_key);
    WT_DECL_ITEM(las_value);
    WT_DECL_RET;
    wt_timestamp_t durable_timestamp, las_timestamp, las_txnid;
    size_t not_used;
    uint32_t las_btree_id;
    uint8_t prepare_state, upd_type;
    int cmp;
    bool use_las_rec;

    use_las_rec = false;

    /* Free an external update to start with */
    if (*upd != NULL && (*upd)->ext == 1)
        __wt_free_update_list(session, *upd);

    /* Determine whether to use lookaside or an in-memory update. */
    if (*las_positioned) {
        WT_RET(__wt_scr_alloc(session, 0, &las_key));

        /* Check if lookaside cursor is still positioned on an update for the given key. */
        WT_ERR(las_cursor->get_key(las_cursor, &las_btree_id, las_key, &las_timestamp, &las_txnid));
        if (las_btree_id != btree_id) {
            *las_positioned = false;
            goto inmem;
        } else {
            WT_ERR(__wt_compare(session, NULL, las_key, key, &cmp));
            if (cmp != 0) {
                *las_positioned = false;
                goto inmem;
            }
        }

        if (*inmem_upd_pos == NULL) {
            /* There are no more in-memory updates to consider. */
            use_las_rec = true;
        } else {
            if (las_timestamp > (*inmem_upd_pos)->start_ts)
                use_las_rec = true;
            else if (las_timestamp == (*inmem_upd_pos)->start_ts) {
                if (las_txnid > (*inmem_upd_pos)->txnid)
                    use_las_rec = true;
            }
        }
    }

    if (use_las_rec) {
        WT_ERR(__wt_scr_alloc(session, 0, &las_value));

        /* Create an update from the lookaside, mark it external and reposition lookaside cursor.*/
        WT_ERR(las_cursor->get_value(
          las_cursor, &durable_timestamp, &prepare_state, &upd_type, las_value));
        WT_ASSERT(session, upd_type != WT_UPDATE_BIRTHMARK);

        /* Allocate an update structure for the record found. */
        WT_ERR(__wt_update_alloc(session, las_value, upd, &not_used, upd_type));
        (*upd)->txnid = las_txnid;
        (*upd)->durable_ts = durable_timestamp;
        (*upd)->start_ts = las_timestamp;
        (*upd)->prepare_state = prepare_state;
        /*
         * Mark this update as external and to be discarded when not needed.
         */
        (*upd)->ext = 1;

        ret = las_cursor->prev(las_cursor);
        if (ret == WT_NOTFOUND) {
            *las_positioned = false;
            ret = 0;
        }
        WT_ERR(ret);
    } else {
inmem:
        *upd = *inmem_upd_pos;
        if (*inmem_upd_pos != NULL)
            *inmem_upd_pos = (*inmem_upd_pos)->next;
    }

err:
    __wt_scr_free(session, &las_key);
    __wt_scr_free(session, &las_value);
    return (ret);
}

/*
 * __wt_rec_upd_select --
 *     Return the update in a list that should be written (or NULL if none can be written).
 */
int
__wt_rec_upd_select(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_CELL_UNPACK *vpack, WT_UPDATE_SELECT *upd_select)
{
    WT_CACHE *cache;
    WT_CURSOR *las_cursor;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_UPDATE *first_ts_upd, *first_txn_upd, *first_inmem_upd, *inmem_upd_pos, *upd;
    wt_timestamp_t timestamp, ts;
    size_t upd_memsize;
    uint64_t max_txn, txnid;
    uint32_t session_flags;
    uint8_t *p;
    bool all_visible, las_cursor_open, las_positioned, list_prepared, list_uncommitted;
    bool skipped_birthmark, sweep_locked;

    /*
     * The "saved updates" return value is used independently of returning an update we can write,
     * both must be initialized.
     */
    upd_select->upd = NULL;
    upd_select->upd_saved = false;

    cache = S2C(session)->cache;
    page = r->page;
    first_ts_upd = first_txn_upd = NULL;
    upd = NULL;
    upd_memsize = 0;
    max_txn = WT_TXN_NONE;
    session_flags = 0; /* [-Werror=maybe-uninitialized] */
    las_cursor_open = false;
    las_positioned = false;
    list_prepared = list_uncommitted = skipped_birthmark = false;

    if (__wt_page_las_active(session, r->ref)) {
        /* Obtain the key to iterate the lookaside */
        WT_RET(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &key));
        switch (page->type) {
        case WT_PAGE_COL_FIX:
        case WT_PAGE_COL_VAR:
            p = key->mem;
            WT_ERR(__wt_vpack_uint(&p, 0, WT_INSERT_RECNO(ins)));
            key->size = WT_PTRDIFF(p, key->data);
            break;
        case WT_PAGE_ROW_LEAF:
            if (ins == NULL) {
                WT_WITH_BTREE(session, S2BT(session),
                  ret = __wt_row_leaf_key(session, page, ripcip, key, false));
                WT_ERR(ret);
            } else {
                key->data = WT_INSERT_KEY(ins);
                key->size = WT_INSERT_KEY_SIZE(ins);
            }
            break;
        default:
            WT_ERR(__wt_illegal_value(session, page->type));
        }

        /* Open a lookaside cursor, position at the latest update for this key */
        /* Remember to unlock */
        __wt_las_cursor(session, &las_cursor, &session_flags);
        las_cursor_open = true;
        cache->las_reader = true;
        //__wt_readlock(session, &cache->las_sweepwalk_lock);
        cache->las_reader = false;
        sweep_locked = true;

        /*
         * Position one past the last update for this key, then step back one update. The updates
         * are sorted oldest to newest in the lookaside.
         */
        WT_ERR(__wt_las_cursor_position(session, las_cursor, S2BT(session)->id, key, UINT64_MAX));
        ret = las_cursor->prev(las_cursor);
        if (ret == 0)
            las_positioned = true;
        WT_ERR_NOTFOUND_OK(ret);
    }

    /*
     * If called with a WT_INSERT item, use its WT_UPDATE list (which must exist), otherwise check
     * for an on-page row-store WT_UPDATE list (which may not exist). Also, check for any updates
     * in the lookaside. Return immediately if the item has no updates.
     */
    if (ins != NULL)
        first_inmem_upd = ins->upd;
    else
        first_inmem_upd = WT_ROW_UPDATE(page, ripcip);

    inmem_upd_pos = first_inmem_upd;
    WT_ERR(__get_next_rec_upd(
      session, &inmem_upd_pos, &las_positioned, las_cursor, S2BT(session)->id, key, &upd));
    if (upd == NULL)
        goto err;

    for (; ret == 0 && upd != NULL; ret = __get_next_rec_upd(session, &inmem_upd_pos,
                                      &las_positioned, las_cursor, S2BT(session)->id, key, &upd)) {
        if ((txnid = upd->txnid) == WT_TXN_ABORTED)
            continue;

        ++r->updates_seen;
        upd_memsize += WT_UPDATE_MEMSIZE(upd);

        /*
         * Track the first update in the chain that is not aborted and the maximum transaction ID.
         */
        if (first_txn_upd == NULL)
            first_txn_upd = upd;

        /* Track the largest transaction ID seen. */
        if (WT_TXNID_LT(max_txn, txnid))
            max_txn = txnid;

        /*
         * Track if all the updates are not with in-progress prepare state.
         */
        if (upd->prepare_state == WT_PREPARE_RESOLVED)
            r->all_upd_prepare_in_prog = false;

        /*
         * Check whether the update was committed before reconciliation started. The global commit
         * point can move forward during reconciliation so we use a cached copy to avoid races when
         * a concurrent transaction commits or rolls back while we are examining its updates. As
         * prepared transaction IDs are globally visible, need to check the update state as well.
         */
        if (F_ISSET(r, WT_REC_EVICT)) {
            if (upd->prepare_state == WT_PREPARE_LOCKED ||
              upd->prepare_state == WT_PREPARE_INPROGRESS) {
                list_prepared = true;
                continue;
            }
            if (F_ISSET(r, WT_REC_VISIBLE_ALL) ? WT_TXNID_LE(r->last_running, txnid) :
                                                 !__txn_visible_id(session, txnid)) {
                r->update_uncommitted = list_uncommitted = true;
                continue;
            }
        }

        /* Track the first update with non-zero timestamp. */
        if (first_ts_upd == NULL && upd->start_ts != WT_TS_NONE)
            first_ts_upd = upd;

        /*
         * Select the update to write to the disk image.
         *
         * Lookaside and update/restore eviction try to choose the same
         * version as a subsequent checkpoint, so that checkpoint can
         * skip over pages with lookaside entries.  If the application
         * has supplied a stable timestamp, we assume (a) that it is
         * old, and (b) that the next checkpoint will use it, so we wait
         * to see a stable update.  If there is no stable timestamp, we
         * assume the next checkpoint will write the most recent version
         * (but we save enough information that checkpoint can fix
         * things up if we choose an update that is too new).
         */
        if (upd_select->upd == NULL && r->las_skew_newest)
            upd_select->upd = upd;

        if (F_ISSET(r, WT_REC_VISIBLE_ALL) ? !__wt_txn_upd_visible_all(session, upd) :
                                             !__wt_txn_upd_durable(session, upd)) {
            if (F_ISSET(r, WT_REC_EVICT))
                ++r->updates_unstable;

            /*
             * Rare case: when applications run at low isolation levels, update/restore eviction may
             * see a stable update followed by an uncommitted update. Give up in that case: we need
             * to discard updates from the stable update and older for correctness and we can't
             * discard an uncommitted update.
             */
            if (F_ISSET(r, WT_REC_UPDATE_RESTORE) && upd_select->upd != NULL &&
              (list_prepared || list_uncommitted)) {
                r->leave_dirty = true;
                ret = __wt_set_return(session, EBUSY);
                goto err;
            }

            if (upd->type == WT_UPDATE_BIRTHMARK)
                skipped_birthmark = true;

            continue;
        }

        /*
         * Lookaside without stable timestamp was taken care of above
         * (set to the first uncommitted transaction). Lookaside with
         * stable timestamp always takes the first stable update.
         */
        if (upd_select->upd == NULL)
            upd_select->upd = upd;

        if (!F_ISSET(r, WT_REC_EVICT))
            break;
    }
    WT_ERR(ret);

    /* Keep track of the selected update. */
    upd = upd_select->upd;

    /* Reconciliation should never see an aborted or reserved update. */
    WT_ASSERT(
      session, upd == NULL || (upd->txnid != WT_TXN_ABORTED && upd->type != WT_UPDATE_RESERVE));

    /*
     * The checkpoint transaction is special. Make sure we never write metadata updates from a
     * checkpoint in a concurrent session.
     */
    WT_ASSERT(session, !WT_IS_METADATA(session->dhandle) || upd == NULL ||
        upd->txnid == WT_TXN_NONE || upd->txnid != S2C(session)->txn_global.checkpoint_state.id ||
        WT_SESSION_IS_CHECKPOINT(session));

    /* If all of the updates were aborted, quit. */
    if (first_txn_upd == NULL) {
        WT_ASSERT(session, upd == NULL);
        goto err;
    }

    /* If no updates were skipped, record that we're making progress. */
    if (upd == first_txn_upd)
        r->update_used = true;

    /*
     * TIMESTAMP-FIXME The start timestamp is determined by the commit timestamp when the key is
     * first inserted (or last updated). The end timestamp is set when a key/value pair becomes
     * invalid, either because of a remove or a modify/update operation on the same key.
     */
    if (upd != NULL) {
        /*
         * TIMESTAMP-FIXME This is waiting on the WT_UPDATE structure's start/stop
         * timestamp/transaction work. For now, if we don't have a timestamp/transaction, just
         * pretend it's durable. If we do have a timestamp/transaction, make the durable and start
         * timestamps equal to the start timestamp and the start transaction equal to the
         * transaction, and again, pretend it's durable.
         */
        upd_select->durable_ts = WT_TS_NONE;
        upd_select->start_ts = WT_TS_NONE;
        upd_select->start_txn = WT_TXN_NONE;
        upd_select->stop_ts = WT_TS_MAX;
        upd_select->stop_txn = WT_TXN_MAX;
        if (upd_select->upd->start_ts != WT_TS_NONE)
            upd_select->durable_ts = upd_select->start_ts = upd_select->upd->start_ts;
        if (upd_select->upd->txnid != WT_TXN_NONE)
            upd_select->start_txn = upd_select->upd->txnid;

        /*
         * Finalize the timestamps and transactions, checking if the update is globally visible and
         * nothing needs to be written.
         */
        if ((upd_select->stop_ts == WT_TS_MAX && upd_select->stop_txn == WT_TXN_MAX) &&
          ((upd_select->start_ts == WT_TS_NONE && upd_select->start_txn == WT_TXN_NONE) ||
              __wt_txn_visible_all(session, upd_select->start_txn, upd_select->start_ts))) {
            upd_select->start_ts = WT_TS_NONE;
            upd_select->start_txn = WT_TXN_NONE;
            upd_select->stop_ts = WT_TS_MAX;
            upd_select->stop_txn = WT_TXN_MAX;
        }
    }

    /*
     * Track the most recent transaction in the page. We store this in the tree at the end of
     * reconciliation in the service of checkpoints, it is used to avoid discarding trees from
     * memory when they have changes required to satisfy a snapshot read.
     */
    if (WT_TXNID_LT(r->max_txn, max_txn))
        r->max_txn = max_txn;

    /* Update the maximum timestamp. */
    if (first_ts_upd != NULL && r->max_timestamp < first_ts_upd->durable_ts)
        r->max_timestamp = first_ts_upd->durable_ts;

    /*
     * If the update we chose was a birthmark, or we are doing update-restore and we skipped a
     * birthmark, the original on-page value must be retained.
     */
    if (upd != NULL && (upd->type == WT_UPDATE_BIRTHMARK ||
                         (F_ISSET(r, WT_REC_UPDATE_RESTORE) && skipped_birthmark))) {
        /*
         * Resolve the birthmark now regardless of whether the update being written to the data file
         * is the same as it was the previous reconciliation. Otherwise lookaside can end up with
         * two birthmark records in the same update chain.
         */
        WT_ERR(__rec_append_orig_value(session, page, first_inmem_upd, vpack));
        upd_select->upd = NULL;
    }

    /*
     * Check if all updates on the page are visible.  If not, it must stay
     * dirty unless we are saving updates to the lookaside table.
     *
     * Updates can be out of transaction ID order (but not out of timestamp
     * order), so we track the maximum transaction ID and the newest update
     * with a timestamp (if any).
     */
    timestamp = first_ts_upd == NULL ? 0 : first_ts_upd->durable_ts;
    all_visible = upd == first_txn_upd && !list_prepared && !list_uncommitted &&
      (F_ISSET(r, WT_REC_VISIBLE_ALL) ? __wt_txn_visible_all(session, max_txn, timestamp) :
                                        __wt_txn_visible(session, max_txn, timestamp));

    if (all_visible)
        goto check_original_value;

    r->leave_dirty = true;

    if (F_ISSET(r, WT_REC_VISIBILITY_ERR))
        WT_PANIC_ERR(session, EINVAL, "reconciliation error, update not visible");

    /*
     * If not trying to evict the page, we know what we'll write and we're done.
     */
    if (!F_ISSET(r, WT_REC_EVICT))
        goto check_original_value;

    /*
     * We are attempting eviction with changes that are not yet stable
     * (i.e. globally visible).  There are two ways to continue, the
     * save/restore eviction path or the lookaside table eviction path.
     * Both cannot be configured because the paths track different
     * information. The update/restore path can handle uncommitted changes,
     * by evicting most of the page and then creating a new, smaller page
     * to which we re-attach those changes. Lookaside eviction writes
     * changes into the lookaside table and restores them on demand if and
     * when the page is read back into memory.
     *
     * Both paths are configured outside of reconciliation: the save/restore
     * path is the WT_REC_UPDATE_RESTORE flag, the lookaside table path is
     * the WT_REC_LOOKASIDE flag.
     */
    if (!F_ISSET(r, WT_REC_LOOKASIDE | WT_REC_UPDATE_RESTORE)) {
        ret = __wt_set_return(session, EBUSY);
        goto err;
    }
    if (list_uncommitted && !F_ISSET(r, WT_REC_UPDATE_RESTORE)) {
        ret = __wt_set_return(session, EBUSY);
        goto err;
    }

    WT_ASSERT(session, r->max_txn != WT_TXN_NONE);

    /*
     * The order of the updates on the list matters, we can't move only the unresolved updates, move
     * the entire update list.
     */
    WT_ERR(__rec_update_save(session, r, ins, ripcip, upd_select->upd, upd_memsize));
    upd_select->upd_saved = true;

    /*
     * Track the first off-page update when saving history in the lookaside table. When skewing
     * newest, we want the first (non-aborted) update after the one stored on the page. Otherwise,
     * we want the update before the on-page update.
     */
    if (F_ISSET(r, WT_REC_LOOKASIDE) && r->las_skew_newest) {
        if (WT_TXNID_LT(r->unstable_txn, first_inmem_upd->txnid))
            r->unstable_txn = first_inmem_upd->txnid;
        if (first_ts_upd != NULL) {
            WT_ASSERT(session, first_ts_upd->prepare_state == WT_PREPARE_INPROGRESS ||
                first_ts_upd->start_ts <= first_ts_upd->durable_ts);

            if (r->unstable_timestamp < first_ts_upd->start_ts)
                r->unstable_timestamp = first_ts_upd->start_ts;

            if (r->unstable_durable_timestamp < first_ts_upd->durable_ts)
                r->unstable_durable_timestamp = first_ts_upd->durable_ts;
        }
    } else if (F_ISSET(r, WT_REC_LOOKASIDE)) {
        for (upd = first_inmem_upd; upd != upd_select->upd; upd = upd->next) {
            if (upd->txnid == WT_TXN_ABORTED)
                continue;

            if (upd->txnid != WT_TXN_NONE && WT_TXNID_LT(upd->txnid, r->unstable_txn))
                r->unstable_txn = upd->txnid;

            /*
             * The durable timestamp is always set by commit, and usually the same as the start
             * timestamp, which makes it OK to use the two independently and be confident both will
             * be set.
             */
            WT_ASSERT(session,
              upd->prepare_state == WT_PREPARE_INPROGRESS || upd->durable_ts >= upd->start_ts);

            if (r->unstable_timestamp > upd->start_ts)
                r->unstable_timestamp = upd->start_ts;

            /*
             * An in-progress prepared update will always have a zero durable timestamp. Checkpoints
             * can only skip reading lookaside history if all updates are in the future, including
             * the prepare, so including the prepare timestamp instead.
             */
            ts = upd->prepare_state == WT_PREPARE_INPROGRESS ? upd->start_ts : upd->durable_ts;
            if (r->unstable_durable_timestamp > ts)
                r->unstable_durable_timestamp = ts;
        }
    }

check_original_value:
    /*
     * Paranoia: check that we didn't choose an update that has since been rolled back.
     */
    WT_ASSERT(session, upd_select->upd == NULL || upd_select->upd->txnid != WT_TXN_ABORTED);

    /*
     * Returning an update means the original on-page value might be lost, and that's a problem if
     * there's a reader that needs it. This call makes a copy of the on-page value and if there is a
     * birthmark in the update list, replaces it. We do that any time there are saved updates and
     * during reconciliation of a backing overflow record that will be physically removed once it's
     * no longer needed
     */
    if (upd_select->upd != NULL &&
      (upd_select->upd_saved ||
          (vpack != NULL && vpack->ovfl && vpack->raw != WT_CELL_VALUE_OVFL_RM)))
        WT_ERR(__rec_append_orig_value(session, page, first_inmem_upd, vpack));

err:
    __wt_scr_free(session, &key);
    if (las_cursor_open)
        WT_TRET(__wt_las_cursor_close(session, &las_cursor, session_flags));

    if (sweep_locked) {
        //__wt_readunlock(session, &cache->las_sweepwalk_lock);
    }

    return (ret);
}
