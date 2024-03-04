/****************************************************************************
Copyright (c) 1996, 2010, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/** @file trx/trx0trx.c
The transaction

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0trx.h"

#ifdef UNIV_NONINL
#include "trx0trx.ic"
#endif

#include "api0ucode.h"

#include "lock0lock.h"
#include "log0log.h"
#include "os0proc.h"
#include "que0que.h"
#include "read0read.h"
#include "srv0srv.h"
#include "thr0loc.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "trx0xa.h"
#include "usr0sess.h"

/* TODO: Can we remove this? */
/* Dummy session used currently in client interface */
sess_t *trx_dummy_sess = nullptr;

/* Number of transactions currently allocated for the client: protected by
the kernel mutex */
ulint trx_n_transactions = 0;

/* Threads with unknown id. */
os_thread_id_t NULL_THREAD_ID;

/** Reset global variables. */

void trx_var_init(void) {
  trx_dummy_sess = nullptr;
  trx_n_transactions = 0;
}

bool trx_is_strict(trx_t *trx) { return false; }

void trx_set_detailed_error(trx_t *trx, const char *msg) {
  ut_strlcpy(trx->detailed_error, msg, sizeof(trx->detailed_error));
}

trx_t *trx_create(sess_t *sess) {
  ut_ad(mutex_own(&kernel_mutex));
  ut_ad(sess);

  auto trx = static_cast<trx_t *>(mem_alloc(sizeof(trx_t)));

  trx->magic_n = TRX_MAGIC_N;

  trx->op_info = "";

  trx->is_purge = 0;
  trx->is_recovered = 0;
  trx->conc_state = TRX_NOT_STARTED;
  trx->start_time = time(nullptr);

  trx->isolation_level = TRX_ISO_REPEATABLE_READ;

  trx->id = 0;
  trx->no = LSN_MAX;

#ifdef WITH_XOPEN
  trx->support_xa = false;
  trx->flush_log_later = false;
  trx->must_flush_log_later = false;
#endif /* WITH_XOPEN */

  trx->check_foreigns = true;
  trx->check_unique_secondary = true;

  trx->dict_operation = TRX_DICT_OP_NONE;
  trx->table_id = 0;

  trx->client_thd = nullptr;
  trx->client_query_str = nullptr;
  trx->duplicates = 0;

  trx->n_client_tables_in_use = 0;
  trx->client_n_tables_locked = 0;

  mutex_create(&trx->undo_mutex, SYNC_TRX_UNDO);

  trx->rseg = nullptr;

  trx->undo_no = 0;
  trx->last_sql_stat_start.least_undo_no = 0;
  trx->insert_undo = nullptr;
  trx->update_undo = nullptr;
  trx->undo_no_arr = nullptr;

  trx->error_state = DB_SUCCESS;
  trx->error_key_num = 0;
  trx->detailed_error[0] = '\0';

  trx->sess = sess;
  trx->que_state = TRX_QUE_RUNNING;
  trx->n_active_thrs = 0;

  trx->handling_signals = false;

  UT_LIST_INIT(trx->signals);
  UT_LIST_INIT(trx->reply_signals);

  trx->graph = nullptr;

  trx->wait_lock = nullptr;
  trx->was_chosen_as_deadlock_victim = false;
  UT_LIST_INIT(trx->wait_thrs);

  trx->lock_heap = mem_heap_create_in_buffer(256);
  UT_LIST_INIT(trx->trx_locks);

  UT_LIST_INIT(trx->trx_savepoints);

  trx->dict_operation_lock_mode = 0;

  trx->global_read_view_heap = mem_heap_create(256);
  trx->global_read_view = nullptr;
  trx->read_view = nullptr;

#ifdef WITH_XOPEN
  /* Set X/Open XA transaction identification to nullptr */
  memset(&trx->xid, 0, sizeof(trx->xid));
  trx->xid.formatID = -1;
#endif /* WITH_XOPEN */

  return trx;
}

trx_t *trx_allocate_for_client(void *arg) {
  trx_t *trx;

  mutex_enter(&kernel_mutex);

  trx = trx_create(trx_dummy_sess);

  trx_n_transactions++;

  UT_LIST_ADD_FIRST(client_trx_list, trx_sys->client_trx_list, trx);

  mutex_exit(&kernel_mutex);

  trx->client_thread_id = os_thread_get_curr_id();

  trx->client_process_no = os_proc_get_number();

  return trx;
}

trx_t *trx_allocate_for_background(void) {
  trx_t *trx;

  mutex_enter(&kernel_mutex);

  trx = trx_create(trx_dummy_sess);

  mutex_exit(&kernel_mutex);

  trx->client_process_no = 0;

  trx->client_thread_id = NULL_THREAD_ID;

  return trx;
}

/** Frees a transaction object. */
static void trx_free(trx_t *trx) /*!< in, own: trx object */
{
  ut_ad(mutex_own(&kernel_mutex));

  if (trx->n_client_tables_in_use != 0 || trx->client_n_tables_locked != 0) {

    ut_print_timestamp(ib_stream);
    ib_logger(ib_stream,
              "  Error: Client is freeing a trx instance\n"
              "though trx->n_client_tables_in_use is %lu\n"
              "and trx->client_n_tables_locked is %lu.\n",
              (ulong)trx->n_client_tables_in_use,
              (ulong)trx->client_n_tables_locked);

    trx_print(ib_stream, trx, 600);

    ut_print_buf(ib_stream, trx, sizeof(trx_t));
    ib_logger(ib_stream, "\n");
  }

  ut_a(trx->magic_n == TRX_MAGIC_N);

  trx->magic_n = 11112222;

  ut_a(trx->conc_state == TRX_NOT_STARTED);

  mutex_free(&(trx->undo_mutex));

  ut_a(trx->insert_undo == nullptr);
  ut_a(trx->update_undo == nullptr);

  if (trx->undo_no_arr) {
    trx_undo_arr_free(trx->undo_no_arr);
  }

  ut_a(UT_LIST_GET_LEN(trx->signals) == 0);
  ut_a(UT_LIST_GET_LEN(trx->reply_signals) == 0);

  ut_a(trx->wait_lock == nullptr);
  ut_a(UT_LIST_GET_LEN(trx->wait_thrs) == 0);

  ut_a(trx->dict_operation_lock_mode == 0);

  if (trx->lock_heap) {
    mem_heap_free(trx->lock_heap);
  }

  ut_a(UT_LIST_GET_LEN(trx->trx_locks) == 0);

  if (trx->global_read_view_heap) {
    mem_heap_free(trx->global_read_view_heap);
  }

  trx->global_read_view = nullptr;

  ut_a(trx->read_view == nullptr);

  mem_free(trx);
}

void trx_free_for_client(trx_t *trx) {
  mutex_enter(&kernel_mutex);

  UT_LIST_REMOVE(client_trx_list, trx_sys->client_trx_list, trx);

  trx_free(trx);

  ut_a(trx_n_transactions > 0);

  trx_n_transactions--;

  mutex_exit(&kernel_mutex);
}

void trx_free_for_background(trx_t *trx) {
  mutex_enter(&kernel_mutex);

  trx_free(trx);

  mutex_exit(&kernel_mutex);
}

/** Inserts the trx handle in the trx system trx list in the right position.
The list is sorted on the trx id so that the biggest id is at the list
start. This function is used at the database startup to insert incomplete
transactions to the list. */
static void trx_list_insert_ordered(trx_t *trx) /*!< in: trx handle */
{
  ut_ad(mutex_own(&kernel_mutex));

  auto trx2 = UT_LIST_GET_FIRST(trx_sys->trx_list);

  while (trx2 != nullptr) {
    if (trx->id >= trx2->id) {

      ut_ad(trx->id > trx2->id);
      break;
    }
    trx2 = UT_LIST_GET_NEXT(trx_list, trx2);
  }

  if (trx2 != nullptr) {
    trx2 = UT_LIST_GET_PREV(trx_list, trx2);

    if (trx2 == nullptr) {
      UT_LIST_ADD_FIRST(trx_list, trx_sys->trx_list, trx);
    } else {
      UT_LIST_INSERT_AFTER(trx_list, trx_sys->trx_list, trx2, trx);
    }
  } else {
    UT_LIST_ADD_LAST(trx_list, trx_sys->trx_list, trx);
  }
}

void trx_lists_init_at_db_start(ib_recovery_t recovery) {
  ut_ad(mutex_own(&kernel_mutex));

  UT_LIST_INIT(trx_sys->trx_list);

  /* Look from the rollback segments if there exist undo logs for
  transactions */

  auto rseg = UT_LIST_GET_FIRST(trx_sys->rseg_list);

  while (rseg != nullptr) {
    auto undo = UT_LIST_GET_FIRST(rseg->insert_undo_list);

    while (undo != nullptr) {

      auto trx = trx_create(trx_dummy_sess);

      trx->is_recovered = true;
      trx->id = undo->trx_id;
#ifdef WITH_XOPEN
      trx->xid = undo->xid;
#endif /* WITH_XOPEN */
      trx->insert_undo = undo;
      trx->rseg = rseg;

      if (undo->state != TRX_UNDO_ACTIVE) {

        /* Prepared transactions are left in the prepared state waiting for a commit or abort decision from the client. */

        if (undo->state == TRX_UNDO_PREPARED) {

          ib_logger(ib_stream,
                    "Transaction %lu was in the XA prepared state.\n",
                    TRX_ID_PREP_PRINTF(trx->id));

          if (recovery == IB_RECOVERY_DEFAULT) {

            trx->conc_state = TRX_PREPARED;
          } else {
            ib_logger(ib_stream, "Since force_recovery > 0, we will do a rollback anyway.\n");

            trx->conc_state = TRX_ACTIVE;
          }
        } else {
          trx->conc_state = TRX_COMMITTED_IN_MEMORY;
        }

        /* We give a dummy value for the trx no;
        this should have no relevance since purge
        is not interested in committed transaction
        numbers, unless they are in the history
        list, in which case it looks the number
        from the disk based undo log structure */

        trx->no = trx->id;
      } else {
        trx->conc_state = TRX_ACTIVE;

        /* A running transaction always has the number field inited to LSN_MAX */

        trx->no = LSN_MAX;
      }

      if (undo->dict_operation) {
        trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);
        trx->table_id = undo->table_id;
      }

      if (!undo->empty) {
        trx->undo_no = undo->top_undo_no + 1;
      }

      trx_list_insert_ordered(trx);

      undo = UT_LIST_GET_NEXT(undo_list, undo);
    }

    undo = UT_LIST_GET_FIRST(rseg->update_undo_list);

    while (undo != nullptr) {
      auto trx = trx_get_on_id(undo->trx_id);

      if (nullptr == trx) {
        trx = trx_create(trx_dummy_sess);

        trx->is_recovered = true;
        trx->id = undo->trx_id;
#ifdef WITH_XOPEN
        trx->xid = undo->xid;
#endif /* WITH_XOPEN */

        if (undo->state != TRX_UNDO_ACTIVE) {

          /* Prepared transactions are left in
          the prepared state waiting for a
          commit or abort decision from
          the client. */

          if (undo->state == TRX_UNDO_PREPARED) {
            ib_logger(ib_stream,
                      "Transaction %lu was in the XA prepared state.\n",
                      TRX_ID_PREP_PRINTF(trx->id));

            if (recovery == IB_RECOVERY_DEFAULT) {

              trx->conc_state = TRX_PREPARED;
            } else {
              ib_logger(ib_stream, "Since"
                                   " force_recovery"
                                   " > 0, we will"
                                   " do a rollback"
                                   " anyway.\n");

              trx->conc_state = TRX_ACTIVE;
            }
          } else {
            trx->conc_state = TRX_COMMITTED_IN_MEMORY;
          }

          /* We give a dummy value for the trx
          number */

          trx->no = trx->id;
        } else {
          trx->conc_state = TRX_ACTIVE;

          /* A running transaction always has the number field inited to LSN_MAX */

          trx->no = LSN_MAX;
        }

        trx->rseg = rseg;
        trx_list_insert_ordered(trx);

        if (undo->dict_operation) {
          trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);
          trx->table_id = undo->table_id;
        }
      }

      trx->update_undo = undo;

      if (!undo->empty && undo->top_undo_no >= trx->undo_no) {

        trx->undo_no = undo->top_undo_no + 1;
      }

      undo = UT_LIST_GET_NEXT(undo_list, undo);
    }

    rseg = UT_LIST_GET_NEXT(rseg_list, rseg);
  }
}

/** Assigns a rollback segment to a transaction in a round-robin fashion.
Skips the SYSTEM rollback segment if another is available.
@return	assigned rollback segment id */
inline ulint trx_assign_rseg(void) {
  trx_rseg_t *rseg = trx_sys->latest_rseg;

  ut_ad(mutex_own(&kernel_mutex));
loop:
  /* Get next rseg in a round-robin fashion */

  rseg = UT_LIST_GET_NEXT(rseg_list, rseg);

  if (rseg == nullptr) {
    rseg = UT_LIST_GET_FIRST(trx_sys->rseg_list);
  }

  /* If it is the SYSTEM rollback segment, and there exist others, skip
  it */

  if ((rseg->id == TRX_SYS_SYSTEM_RSEG_ID) &&
      (UT_LIST_GET_LEN(trx_sys->rseg_list) > 1)) {
    goto loop;
  }

  trx_sys->latest_rseg = rseg;

  return rseg->id;
}

/** Starts a new transaction.
@return	true */

bool trx_start_low(
    trx_t *trx,    /*!< in: transaction */
    ulint rseg_id) /*!< in: rollback segment id; if ULINT_UNDEFINED
                   is passed, the system chooses the rollback segment
                   automatically in a round-robin fashion */
{
  trx_rseg_t *rseg;

  ut_ad(mutex_own(&kernel_mutex));
  ut_ad(trx->rseg == nullptr);
  ut_ad(trx->magic_n == TRX_MAGIC_N);

  if (trx->is_purge) {
    trx->id = 0;
    trx->conc_state = TRX_ACTIVE;
    trx->start_time = time(nullptr);

    return true;
  }

  ut_ad(trx->conc_state != TRX_ACTIVE);

  if (rseg_id == ULINT_UNDEFINED) {

    rseg_id = trx_assign_rseg();
  }

  rseg = trx_sys_get_nth_rseg(trx_sys, rseg_id);

  trx->id = trx_sys_get_new_trx_id();

  /* The initial value for trx->no: LSN_MAX is used in read_view_open_now: */

  trx->no = LSN_MAX;

  trx->rseg = rseg;

  trx->conc_state = TRX_ACTIVE;
  trx->start_time = time(nullptr);

#ifdef WITH_XOPEN
  trx->flush_log_later = false;
  trx->must_flush_log_later = false;
#endif /* WITH_XOPEN */

  UT_LIST_ADD_FIRST(trx_list, trx_sys->trx_list, trx);

  return true;
}

bool trx_start(trx_t *trx, ulint rseg_id) {
  bool ret;

  /* Update the info whether we should skip XA steps that eat CPU time
  For the duration of the transaction trx->support_xa is not reread
  from thd so any changes in the value take effect in the next
  transaction. This is to avoid a scenario where some undo
  generated by a transaction, has XA stuff, and other undo,
  generated by the same transaction, doesn't. */

  /* FIXME: This requires an API change to support */
  /* trx->support_xa = ib_supports_xa(trx->client_thd); */

  mutex_enter(&kernel_mutex);

  ret = trx_start_low(trx, rseg_id);

  mutex_exit(&kernel_mutex);

  return ret;
}

void trx_commit_off_kernel(trx_t *trx) {
  page_t *update_hdr_page;
  uint64_t lsn = 0;
  trx_rseg_t *rseg;
  trx_undo_t *undo;
  mtr_t mtr;

  ut_ad(mutex_own(&kernel_mutex));

  rseg = trx->rseg;

  if (trx->insert_undo != nullptr || trx->update_undo != nullptr) {

    mutex_exit(&kernel_mutex);

    mtr_start(&mtr);

    /* Change the undo log segment states from TRX_UNDO_ACTIVE
    to some other state: these modifications to the file data
    structure define the transaction as committed in the file
    based world, at the serialization point of the log sequence
    number lsn obtained below. */

    mutex_enter(&(rseg->mutex));

    if (trx->insert_undo != nullptr) {
      trx_undo_set_state_at_finish(rseg, trx, trx->insert_undo, &mtr);
    }

    undo = trx->update_undo;

    if (undo) {
      mutex_enter(&kernel_mutex);
      trx->no = trx_sys_get_new_trx_no();

      mutex_exit(&kernel_mutex);

      /* It is not necessary to obtain trx->undo_mutex here
      because only a single OS thread is allowed to do the
      transaction commit for this transaction. */

      update_hdr_page = trx_undo_set_state_at_finish(rseg, trx, undo, &mtr);

      /* We have to do the cleanup for the update log while
      holding the rseg mutex because update log headers
      have to be put to the history list in the order of
      the trx number. */

      trx_undo_update_cleanup(trx, update_hdr_page, &mtr);
    }

    mutex_exit(&(rseg->mutex));

    /* The following call commits the mini-transaction, making the
    whole transaction committed in the file-based world, at this
    log sequence number. The transaction becomes 'durable' when
    we write the log to disk, but in the logical sense the commit
    in the file-based data structures (undo logs etc.) happens
    here.

    NOTE that transaction numbers, which are assigned only to
    transactions with an update undo log, do not necessarily come
    in exactly the same order as commit lsn's, if the transactions
    have different rollback segments. To get exactly the same
    order we should hold the kernel mutex up to this point,
    adding to the contention of the kernel mutex. However, if
    a transaction T2 is able to see modifications made by
    a transaction T1, T2 will always get a bigger transaction
    number and a bigger commit lsn than T1. */

    /*--------------*/
    mtr_commit(&mtr);
    /*--------------*/
    lsn = mtr.end_lsn;

    mutex_enter(&kernel_mutex);
  }

  ut_ad(trx->conc_state == TRX_ACTIVE || trx->conc_state == TRX_PREPARED);
  ut_ad(mutex_own(&kernel_mutex));

  /* The following assignment makes the transaction committed in memory
  and makes its changes to data visible to other transactions.
  NOTE that there is a small discrepancy from the strict formal
  visibility rules here: a human user of the database can see
  modifications made by another transaction T even before the necessary
  log segment has been flushed to the disk. If the database happens to
  crash before the flush, the user has seen modifications from T which
  will never be a committed transaction. However, any transaction T2
  which sees the modifications of the committing transaction T, and
  which also itself makes modifications to the database, will get an lsn
  larger than the committing transaction T. In the case where the log
  flush fails, and T never gets committed, also T2 will never get
  committed. */

  /*--------------------------------------*/
  trx->conc_state = TRX_COMMITTED_IN_MEMORY;
  /*--------------------------------------*/

  /* If we release kernel_mutex below and we are still doing
  recovery i.e.: back ground rollback thread is still active
  then there is a chance that the rollback thread may see
  this trx as COMMITTED_IN_MEMORY and goes adhead to clean it
  up calling trx_cleanup_at_db_startup(). This can happen
  in the case we are committing a trx here that is left in
  PREPARED state during the crash. Note that commit of the
  rollback of a PREPARED trx happens in the recovery thread
  while the rollback of other transactions happen in the
  background thread. To avoid this race we unconditionally
  unset the is_recovered flag from the trx. */

  trx->is_recovered = false;

  lock_release_off_kernel(trx);

  if (trx->global_read_view) {
    read_view_close(trx->global_read_view);
    mem_heap_empty(trx->global_read_view_heap);
    trx->global_read_view = nullptr;
  }

  trx->read_view = nullptr;

  if (lsn) {

    mutex_exit(&kernel_mutex);

    if (trx->insert_undo != nullptr) {

      trx_undo_insert_cleanup(trx);
    }

    /* NOTE that we could possibly make a group commit more
    efficient here: call os_thread_yield here to allow also other
    trxs to come to commit! */

    /*-------------------------------------*/

    /* Depending on the config options, we may now write the log
    buffer to the log files, making the transaction durable if
    the OS does not crash. We may also flush the log files to
    disk, making the transaction durable also at an OS crash or a
    power outage.

    The idea in InnoDB's group commit is that a group of
    transactions gather behind a trx doing a physical disk write
    to log files, and when that physical write has been completed,
    one of those transactions does a write which commits the whole
    group. Note that this group commit will only bring benefit if
    there are > 2 users in the database. Then at least 2 users can
    gather behind one doing the physical log write to disk. */

    /* If we are calling trx_commit() under prepare_commit_mutex, we
    will delay possible log write and flush to a separate function
    trx_commit_flush_log(), which is only called when the
    thread has released the mutex. This is to make the
    group commit algorithm to work. Otherwise, the prepare_commit
    mutex would serialize all commits and prevent a group of
    transactions from gathering. */
#ifdef WITH_XOPEN
    if (trx->flush_log_later) {
      /* Do nothing yet */
      trx->must_flush_log_later = true;
    } else
#endif /* WITH_XOPEN */
      if (srv_flush_log_at_trx_commit == 0) {
        /* Do nothing */
      } else if (srv_flush_log_at_trx_commit == 1) {
        if (srv_unix_file_flush_method == SRV_UNIX_NOSYNC) {
          /* Write the log but do not flush it to disk */

          log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, false);
        } else {
          /* Write the log to the log files AND flush
          them to disk */

          log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, true);
        }
      } else if (srv_flush_log_at_trx_commit == 2) {

        /* Write the log but do not flush it to disk */

        log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, false);
      } else {
        ut_error;
      }

    trx->commit_lsn = lsn;

    /*-------------------------------------*/

    mutex_enter(&kernel_mutex);
  }

  /* Free all savepoints */
  trx_roll_free_all_savepoints(trx);

  trx->conc_state = TRX_NOT_STARTED;
  trx->rseg = nullptr;
  trx->undo_no = 0;
  trx->last_sql_stat_start.least_undo_no = 0;
  trx->client_query_str = nullptr;

  ut_ad(UT_LIST_GET_LEN(trx->wait_thrs) == 0);
  ut_ad(UT_LIST_GET_LEN(trx->trx_locks) == 0);

  UT_LIST_REMOVE(trx_list, trx_sys->trx_list, trx);
}

void trx_cleanup_at_db_startup(trx_t *trx) {
  if (trx->insert_undo != nullptr) {

    trx_undo_insert_cleanup(trx);
  }

  trx->conc_state = TRX_NOT_STARTED;
  trx->rseg = nullptr;
  trx->undo_no = 0;
  trx->last_sql_stat_start.least_undo_no = 0;

  UT_LIST_REMOVE(trx_list, trx_sys->trx_list, trx);
}

read_view_t *trx_assign_read_view(trx_t *trx) {
  ut_ad(trx->conc_state == TRX_ACTIVE);

  if (trx->read_view) {
    return trx->read_view;
  }

  mutex_enter(&kernel_mutex);

  if (!trx->read_view) {
    trx->read_view = read_view_open_now(trx->id, trx->global_read_view_heap);
    trx->global_read_view = trx->read_view;
  }

  mutex_exit(&kernel_mutex);

  return trx->read_view;
}

/** Commits a transaction. NOTE that the kernel mutex is temporarily released.
 */
static void trx_handle_commit_sig_off_kernel(
    trx_t *trx,           /*!< in: transaction */
    que_thr_t **next_thr) /*!< in/out: next query thread to run;
                          if the value which is passed in is
                          a pointer to a nullptr pointer, then the
                          calling function can start running
                          a new query thread */
{
  trx_sig_t *sig;
  trx_sig_t *next_sig;

  ut_ad(mutex_own(&kernel_mutex));

  trx->que_state = TRX_QUE_COMMITTING;

  trx_commit_off_kernel(trx);

  ut_ad(UT_LIST_GET_LEN(trx->wait_thrs) == 0);

  /* Remove all TRX_SIG_COMMIT signals from the signal queue and send
  reply messages to them */

  sig = UT_LIST_GET_FIRST(trx->signals);

  while (sig != nullptr) {
    next_sig = UT_LIST_GET_NEXT(signals, sig);

    if (sig->type == TRX_SIG_COMMIT) {

      trx_sig_reply(sig, next_thr);
      trx_sig_remove(trx, sig);
    }

    sig = next_sig;
  }

  trx->que_state = TRX_QUE_RUNNING;
}

void trx_end_lock_wait(trx_t *trx) {
  que_thr_t *thr;

  ut_ad(mutex_own(&kernel_mutex));
  ut_ad(trx->que_state == TRX_QUE_LOCK_WAIT);

  thr = UT_LIST_GET_FIRST(trx->wait_thrs);

  while (thr != nullptr) {
    que_thr_end_wait_no_next_thr(thr);

    UT_LIST_REMOVE(trx_thrs, trx->wait_thrs, thr);

    thr = UT_LIST_GET_FIRST(trx->wait_thrs);
  }

  trx->que_state = TRX_QUE_RUNNING;
}

/** Moves the query threads in the lock wait list to the SUSPENDED state and
puts the transaction to the TRX_QUE_RUNNING state. */
static void trx_lock_wait_to_suspended(
    trx_t *trx) /*!< in: transaction in the TRX_QUE_LOCK_WAIT state */
{
  que_thr_t *thr;

  ut_ad(mutex_own(&kernel_mutex));
  ut_ad(trx->que_state == TRX_QUE_LOCK_WAIT);

  thr = UT_LIST_GET_FIRST(trx->wait_thrs);

  while (thr != nullptr) {
    thr->state = QUE_THR_SUSPENDED;

    UT_LIST_REMOVE(trx_thrs, trx->wait_thrs, thr);

    thr = UT_LIST_GET_FIRST(trx->wait_thrs);
  }

  trx->que_state = TRX_QUE_RUNNING;
}

/** Moves the query threads in the sig reply wait list of trx to the SUSPENDED
state. */
static void trx_sig_reply_wait_to_suspended(trx_t *trx) /*!< in: transaction */
{
  trx_sig_t *sig;
  que_thr_t *thr;

  ut_ad(mutex_own(&kernel_mutex));

  sig = UT_LIST_GET_FIRST(trx->reply_signals);

  while (sig != nullptr) {
    thr = sig->receiver;

    ut_ad(thr->state == QUE_THR_SIG_REPLY_WAIT);

    thr->state = QUE_THR_SUSPENDED;

    sig->receiver = nullptr;

    UT_LIST_REMOVE(reply_signals, trx->reply_signals, sig);

    sig = UT_LIST_GET_FIRST(trx->reply_signals);
  }
}

/** Checks the compatibility of a new signal with the other signals in the
queue.
@return	true if the signal can be queued */
static bool trx_sig_is_compatible(
    trx_t *trx,   /*!< in: trx handle */
    ulint type,   /*!< in: signal type */
    ulint sender) /*!< in: TRX_SIG_SELF or TRX_SIG_OTHER_SESS */
{
  trx_sig_t *sig;

  ut_ad(mutex_own(&kernel_mutex));

  if (UT_LIST_GET_LEN(trx->signals) == 0) {

    return true;
  }

  if (sender == TRX_SIG_SELF) {
    if (type == TRX_SIG_ERROR_OCCURRED) {

      return true;

    } else if (type == TRX_SIG_BREAK_EXECUTION) {

      return true;
    } else {
      return false;
    }
  }

  ut_ad(sender == TRX_SIG_OTHER_SESS);

  sig = UT_LIST_GET_FIRST(trx->signals);

  if (type == TRX_SIG_COMMIT) {
    while (sig != nullptr) {

      if (sig->type == TRX_SIG_TOTAL_ROLLBACK) {

        return false;
      }

      sig = UT_LIST_GET_NEXT(signals, sig);
    }

    return true;

  } else if (type == TRX_SIG_TOTAL_ROLLBACK) {
    while (sig != nullptr) {

      if (sig->type == TRX_SIG_COMMIT) {

        return false;
      }

      sig = UT_LIST_GET_NEXT(signals, sig);
    }

    return true;

  } else if (type == TRX_SIG_BREAK_EXECUTION) {

    return true;
  } else {
    ut_error;

    return false;
  }
}

void trx_sig_send(trx_t *trx, ulint type, ulint sender, que_thr_t *receiver_thr,
                  trx_savept_t *savept, que_thr_t **next_thr) {
  trx_sig_t *sig;
  trx_t *receiver_trx;

  ut_ad(trx);
  ut_ad(mutex_own(&kernel_mutex));

  if (!trx_sig_is_compatible(trx, type, sender)) {
    /* The signal is not compatible with the other signals in
    the queue: die */

    ut_error;
  }

  /* Queue the signal object */

  if (UT_LIST_GET_LEN(trx->signals) == 0) {

    /* The signal list is empty: the 'sig' slot must be unused
    (we improve performance a bit by avoiding mem_alloc) */
    sig = &(trx->sig);
  } else {
    /* It might be that the 'sig' slot is unused also in this
    case, but we choose the easy way of using mem_alloc */

    sig = static_cast<trx_sig_t *>(mem_alloc(sizeof(trx_sig_t)));
  }

  UT_LIST_ADD_LAST(signals, trx->signals, sig);

  sig->type = type;
  sig->sender = sender;
  sig->receiver = receiver_thr;

  if (savept) {
    sig->savept = *savept;
  }

  if (receiver_thr) {
    receiver_trx = thr_get_trx(receiver_thr);

    UT_LIST_ADD_LAST(reply_signals, receiver_trx->reply_signals, sig);
  }

  if (trx->sess->state == SESS_ERROR) {

    trx_sig_reply_wait_to_suspended(trx);
  }

  if ((sender != TRX_SIG_SELF) || (type == TRX_SIG_BREAK_EXECUTION)) {
    ut_error;
  }

  /* If there were no other signals ahead in the queue, try to start
  handling of the signal */

  if (UT_LIST_GET_FIRST(trx->signals) == sig) {

    trx_sig_start_handle(trx, next_thr);
  }
}

void trx_end_signal_handling(trx_t *trx) {
  ut_ad(mutex_own(&kernel_mutex));
  ut_ad(trx->handling_signals == true);

  trx->handling_signals = false;

  trx->graph = trx->graph_before_signal_handling;

  if (trx->graph && (trx->sess->state == SESS_ERROR)) {

    que_fork_error_handle(trx, trx->graph);
  }
}

void trx_sig_start_handle(trx_t *trx, que_thr_t **next_thr) {
  trx_sig_t *sig;
  ulint type;
loop:
  /* We loop in this function body as long as there are queued signals
  we can process immediately */

  ut_ad(trx);
  ut_ad(mutex_own(&kernel_mutex));

  if (trx->handling_signals && (UT_LIST_GET_LEN(trx->signals) == 0)) {

    trx_end_signal_handling(trx);

    return;
  }

  if (trx->conc_state == TRX_NOT_STARTED) {

    trx_start_low(trx, ULINT_UNDEFINED);
  }

  /* If the trx is in a lock wait state, moves the waiting query threads
  to the suspended state */

  if (trx->que_state == TRX_QUE_LOCK_WAIT) {

    trx_lock_wait_to_suspended(trx);
  }

  /* If the session is in the error state and this trx has threads
  waiting for reply from signals, moves these threads to the suspended
  state, canceling wait reservations; note that if the transaction has
  sent a commit or rollback signal to itself, and its session is not in
  the error state, then nothing is done here. */

  if (trx->sess->state == SESS_ERROR) {
    trx_sig_reply_wait_to_suspended(trx);
  }

  /* If there are no running query threads, we can start processing of a
  signal, otherwise we have to wait until all query threads of this
  transaction are aware of the arrival of the signal. */

  if (trx->n_active_thrs > 0) {

    return;
  }

  if (trx->handling_signals == false) {
    trx->graph_before_signal_handling = trx->graph;

    trx->handling_signals = true;
  }

  sig = UT_LIST_GET_FIRST(trx->signals);
  type = sig->type;

  if (type == TRX_SIG_COMMIT) {

    trx_handle_commit_sig_off_kernel(trx, next_thr);

  } else if ((type == TRX_SIG_TOTAL_ROLLBACK) ||
             (type == TRX_SIG_ROLLBACK_TO_SAVEPT)) {

    trx_rollback(trx, sig, next_thr);

    /* No further signals can be handled until the rollback
    completes, therefore we return */

    return;

  } else if (type == TRX_SIG_ERROR_OCCURRED) {

    trx_rollback(trx, sig, next_thr);

    /* No further signals can be handled until the rollback
    completes, therefore we return */

    return;

  } else if (type == TRX_SIG_BREAK_EXECUTION) {

    trx_sig_reply(sig, next_thr);
    trx_sig_remove(trx, sig);
  } else {
    ut_error;
  }

  goto loop;
}

void trx_sig_reply(trx_sig_t *sig, que_thr_t **next_thr) {
  trx_t *receiver_trx;

  ut_ad(sig);
  ut_ad(mutex_own(&kernel_mutex));

  if (sig->receiver != nullptr) {
    ut_ad((sig->receiver)->state == QUE_THR_SIG_REPLY_WAIT);

    receiver_trx = thr_get_trx(sig->receiver);

    UT_LIST_REMOVE(reply_signals, receiver_trx->reply_signals, sig);
    ut_ad(receiver_trx->sess->state != SESS_ERROR);

    que_thr_end_wait(sig->receiver, next_thr);

    sig->receiver = nullptr;
  }
}

void trx_sig_remove(trx_t *trx, trx_sig_t *sig) {
  ut_ad(trx && sig);
  ut_ad(mutex_own(&kernel_mutex));

  ut_ad(sig->receiver == nullptr);

  UT_LIST_REMOVE(signals, trx->signals, sig);
  sig->type = 0; /* reset the field to catch possible bugs */

  if (sig != &(trx->sig)) {
    mem_free(sig);
  }
}

commit_node_t *commit_node_create(mem_heap_t *heap) {
  auto node = reinterpret_cast<commit_node_t *>(
      mem_heap_alloc(heap, sizeof(commit_node_t)));

  node->common.type = QUE_NODE_COMMIT;
  node->state = COMMIT_NODE_SEND;

  return node;
}

que_thr_t *trx_commit_step(que_thr_t *thr) {
  auto node = static_cast<commit_node_t *>(thr->run_node);

  ut_ad(que_node_get_type(node) == QUE_NODE_COMMIT);

  if (thr->prev_node == que_node_get_parent(node)) {
    node->state = COMMIT_NODE_SEND;
  }

  if (node->state == COMMIT_NODE_SEND) {
    mutex_enter(&kernel_mutex);

    node->state = COMMIT_NODE_WAIT;

    que_thr_t *next_thr{};

    thr->state = QUE_THR_SIG_REPLY_WAIT;

    /* Send the commit signal to the transaction */

    trx_sig_send(thr_get_trx(thr), TRX_SIG_COMMIT, TRX_SIG_SELF, thr, nullptr,
                 &next_thr);

    mutex_exit(&kernel_mutex);

    return next_thr;
  }

  ut_ad(node->state == COMMIT_NODE_WAIT);

  node->state = COMMIT_NODE_SEND;

  thr->run_node = que_node_get_parent(node);

  return thr;
}

db_err trx_commit(trx_t *trx) {
  /* Because we do not do the commit by sending an Innobase
  sig to the transaction, we must here make sure that trx has been
  started. */

  ut_a(trx);

  trx->op_info = "committing";

  mutex_enter(&kernel_mutex);

  trx_commit_off_kernel(trx);

  mutex_exit(&kernel_mutex);

  trx->op_info = "";

  return DB_SUCCESS;
}

#ifdef WITH_XOPEN
ulint trx_commit_flush_log(trx_t *trx) {
  uint64_t lsn = trx->commit_lsn;

  ut_a(trx);

  trx->op_info = "flushing log";

  if (!trx->must_flush_log_later) {
    /* Do nothing */
  } else if (srv_flush_log_at_trx_commit == 0) {
    /* Do nothing */
  } else if (srv_flush_log_at_trx_commit == 1) {
    if (srv_unix_file_flush_method == SRV_UNIX_NOSYNC) {
      /* Write the log but do not flush it to disk */

      log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, false);
    } else {
      /* Write the log to the log files AND flush them to
      disk */

      log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, true);
    }
  } else if (srv_flush_log_at_trx_commit == 2) {

    /* Write the log but do not flush it to disk */

    log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, false);
  } else {
    ut_error;
  }

  trx->must_flush_log_later = false;

  trx->op_info = "";

  return 0;
}
#endif

/** Marks the latest SQL statement ended. */

void trx_mark_sql_stat_end(trx_t *trx) /*!< in: trx handle */
{
  ut_a(trx);

  if (trx->conc_state == TRX_NOT_STARTED) {
    trx->undo_no = 0;
  }

  trx->last_sql_stat_start.least_undo_no = trx->undo_no;
}

void trx_print(ib_stream_t ib_stream, trx_t *trx, ulint max_query_len) {
  bool newline;

  ib_logger(ib_stream, "TRANSACTION %lu", TRX_ID_PREP_PRINTF(trx->id));

  switch (trx->conc_state) {
  case TRX_NOT_STARTED:
    ib_logger(ib_stream, ", not started");
    break;
  case TRX_ACTIVE:
    ib_logger(ib_stream, ", ACTIVE %lu sec",
              (ulong)difftime(time(nullptr), trx->start_time));
    break;
  case TRX_PREPARED:
    ib_logger(ib_stream, ", ACTIVE (PREPARED) %lu sec",
              (ulong)difftime(time(nullptr), trx->start_time));
    break;
  case TRX_COMMITTED_IN_MEMORY:
    ib_logger(ib_stream, ", COMMITTED IN MEMORY");
    break;
  default:
    ib_logger(ib_stream, " state %lu", (ulong)trx->conc_state);
  }

#ifdef UNIV_LINUX
  ib_logger(ib_stream, ", process no %lu", trx->client_process_no);
#endif
  ib_logger(ib_stream, ", OS thread id %lu",
            (ulong)os_thread_pf(trx->client_thread_id));

  if (*trx->op_info) {
    ib_logger(ib_stream, " %s", trx->op_info);
  }

  if (trx->is_recovered) {
    ib_logger(ib_stream, " recovered trx");
  }

  if (trx->is_purge) {
    ib_logger(ib_stream, " purge trx");
  }

  ib_logger(ib_stream, "\n");

  if (trx->n_client_tables_in_use > 0 || trx->client_n_tables_locked > 0) {

    ib_logger(ib_stream, "Client tables in use %lu, locked %lu\n",
              (ulong)trx->n_client_tables_in_use,
              (ulong)trx->client_n_tables_locked);
  }

  newline = true;

  switch (trx->que_state) {
  case TRX_QUE_RUNNING:
    newline = false;
    break;
  case TRX_QUE_LOCK_WAIT:
    ib_logger(ib_stream, "LOCK WAIT ");
    break;
  case TRX_QUE_ROLLING_BACK:
    ib_logger(ib_stream, "ROLLING BACK ");
    break;
  case TRX_QUE_COMMITTING:
    ib_logger(ib_stream, "COMMITTING ");
    break;
  default:
    ib_logger(ib_stream, "que state %lu ", (ulong)trx->que_state);
  }

  if (0 < UT_LIST_GET_LEN(trx->trx_locks) ||
      mem_heap_get_size(trx->lock_heap) > 400) {
    newline = true;

    ib_logger(ib_stream,
              "%lu lock struct(s), heap size %lu,"
              " %lu row lock(s)",
              (ulong)UT_LIST_GET_LEN(trx->trx_locks),
              (ulong)mem_heap_get_size(trx->lock_heap),
              (ulong)lock_number_of_rows_locked(trx));
  }

  if (trx->undo_no > 0) {
    newline = true;
    ib_logger(ib_stream, ", undo log entries %lu",
              (ulong)trx->undo_no);
  }

  if (newline) {
    ib_logger(ib_stream, "\n");
  }
}

int trx_weight_cmp(const trx_t *a, const trx_t *b) {
  /* We compare the number of altered/locked rows. */

  return TRX_WEIGHT(a) - TRX_WEIGHT(b);
}

void trx_prepare_off_kernel(trx_t *trx) {
  trx_rseg_t *rseg;
  uint64_t lsn = 0;
  mtr_t mtr;

  ut_ad(mutex_own(&kernel_mutex));

  rseg = trx->rseg;

  if (trx->insert_undo != nullptr || trx->update_undo != nullptr) {

    mutex_exit(&kernel_mutex);

    mtr_start(&mtr);

    /* Change the undo log segment states from TRX_UNDO_ACTIVE
    to TRX_UNDO_PREPARED: these modifications to the file data
    structure define the transaction as prepared in the
    file-based world, at the serialization point of lsn. */

    mutex_enter(&(rseg->mutex));

    if (trx->insert_undo != nullptr) {

      /* It is not necessary to obtain trx->undo_mutex here
      because only a single OS thread is allowed to do the
      transaction prepare for this transaction. */

      trx_undo_set_state_at_prepare(trx, trx->insert_undo, &mtr);
    }

    if (trx->update_undo) {
      trx_undo_set_state_at_prepare(trx, trx->update_undo, &mtr);
    }

    mutex_exit(&(rseg->mutex));

    /*--------------*/
    mtr_commit(&mtr); /* This mtr commit makes the
                      transaction prepared in the file-based
                      world */
    /*--------------*/
    lsn = mtr.end_lsn;

    mutex_enter(&kernel_mutex);
  }

  ut_ad(mutex_own(&kernel_mutex));

  /*--------------------------------------*/
  trx->conc_state = TRX_PREPARED;
  /*--------------------------------------*/

  if (lsn) {
    /* Depending on the config options, we may now write the log
    buffer to the log files, making the prepared state of the
    transaction durable if the OS does not crash. We may also
    flush the log files to disk, making the prepared state of the
    transaction durable also at an OS crash or a power outage.

    The idea in InnoDB's group prepare is that a group of
    transactions gather behind a trx doing a physical disk write
    to log files, and when that physical write has been completed,
    one of those transactions does a write which prepares the whole
    group. Note that this group prepare will only bring benefit if
    there are > 2 users in the database. Then at least 2 users can
    gather behind one doing the physical log write to disk. */

    mutex_exit(&kernel_mutex);

    if (srv_flush_log_at_trx_commit == 0) {
      /* Do nothing */
    } else if (srv_flush_log_at_trx_commit == 1) {
      if (srv_unix_file_flush_method == SRV_UNIX_NOSYNC) {
        /* Write the log but do not flush it to disk */

        log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, false);
      } else {
        /* Write the log to the log files AND flush
        them to disk */

        log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, true);
      }
    } else if (srv_flush_log_at_trx_commit == 2) {

      /* Write the log but do not flush it to disk */

      log_write_up_to(lsn, LOG_WAIT_ONE_GROUP, false);
    } else {
      ut_error;
    }

    mutex_enter(&kernel_mutex);
  }
}

ulint trx_prepare(trx_t *trx) {
  /* Because we do not do the prepare by sending an Innobase
  sig to the transaction, we must here make sure that trx has been
  started. */

  ut_a(trx);

  trx->op_info = "preparing";

  mutex_enter(&kernel_mutex);

  trx_prepare_off_kernel(trx);

  mutex_exit(&kernel_mutex);

  trx->op_info = "";

  return 0;
}

int trx_recover(XID *xid_list, ulint len) {
  trx_t *trx;
  ulint count = 0;

  ut_ad(xid_list);
  ut_ad(len);

  /* We should set those transactions which are in the prepared state
  to the xid_list */

  mutex_enter(&kernel_mutex);

  trx = UT_LIST_GET_FIRST(trx_sys->trx_list);

  while (trx) {
    if (trx->conc_state == TRX_PREPARED) {
#ifdef WITH_XOPEN
      xid_list[count] = trx->xid;
#endif /* WITH_XOPEN */

      if (count == 0) {
        ut_print_timestamp(ib_stream);
        ib_logger(ib_stream, "  Starting recovery for"
                             " XA transactions...\n");
      }

      ut_print_timestamp(ib_stream);
      ib_logger(ib_stream,
                "  Transaction %lu in"
                " prepared state after recovery\n",
                TRX_ID_PREP_PRINTF(trx->id));

      ut_print_timestamp(ib_stream);
      ib_logger(ib_stream,
                "  Transaction contains changes"
                " to %lu rows\n",
                (ulong)trx->undo_no);

      count++;

      if (count == len) {
        break;
      }
    }

    trx = UT_LIST_GET_NEXT(trx_list, trx);
  }

  mutex_exit(&kernel_mutex);

  if (count > 0) {
    ut_print_timestamp(ib_stream);
    ib_logger(ib_stream,
              "  %lu transactions in prepared state"
              " after recovery\n",
              (ulong)count);
  }

  return (int)count;
}

#ifdef WITH_XOPEN
trx_t *trx_get_trx_by_xid(XID *xid) {
  trx_t *trx;

  if (xid == nullptr) {

    return nullptr;
  }

  mutex_enter(&kernel_mutex);

  trx = UT_LIST_GET_FIRST(trx_sys->trx_list);

  while (trx) {
    /* Compare two X/Open XA transaction id's: their
    length should be the same and binary comparison
    of gtrid_lenght+bqual_length bytes should be
    the same */

    if (xid->gtrid_length == trx->xid.gtrid_length &&
        xid->bqual_length == trx->xid.bqual_length &&
        memcmp(xid->data, trx->xid.data,
               xid->gtrid_length + xid->bqual_length) == 0) {
      break;
    }

    trx = UT_LIST_GET_NEXT(trx_list, trx);
  }

  mutex_exit(&kernel_mutex);

  if (trx) {
    if (trx->conc_state != TRX_PREPARED) {

      return nullptr;
    }

    return trx;
  } else {
    return nullptr;
  }
}
#endif /* WITH_XOPEN */
