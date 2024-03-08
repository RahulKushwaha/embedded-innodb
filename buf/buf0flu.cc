/****************************************************************************
Copyright (c) 1995, 2010, Innobase Oy. All Rights Reserved.

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

/** @file buf/buf0flu.c
The database buffer buf_pool flush algorithm

Created 11/11/1995 Heikki Tuuri
*******************************************************/

#include "buf0flu.h"

#ifdef UNIV_NONINL
#include "buf0flu.ic"
#endif

#include "buf0buf.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "fil0fil.h"
#include "log0log.h"
#include "os0file.h"
#include "page0page.h"
#include "srv0srv.h"
#include "trx0sys.h"
#include "ut0lst.h"
#include "ut0rbt.h"

/** These statistics are generated for heuristics used in estimating the
rate at which we should flush the dirty blocks to avoid bursty IO
activity. Note that the rate of flushing not only depends on how many
dirty pages we have in the buffer pool but it is also a fucntion of
how much redo the workload is generating and at what rate. */
/* @{ */

/** Number of intervals for which we keep the history of these stats.
Each interval is 1 second, defined by the rate at which
srv_error_monitor_thread() calls buf_flush_stat_update(). */
#define BUF_FLUSH_STAT_N_INTERVAL 20

/** Sampled values buf_flush_stat_cur.
Not protected by any mutex.  Updated by buf_flush_stat_update(). */
static buf_flush_stat_t buf_flush_stat_arr[BUF_FLUSH_STAT_N_INTERVAL];

/** Cursor to buf_flush_stat_arr[]. Updated in a round-robin fashion. */
static ulint buf_flush_stat_arr_ind;

/** Values at start of the current interval. Reset by
buf_flush_stat_update(). */
static buf_flush_stat_t buf_flush_stat_cur;

/** Running sum of past values of buf_flush_stat_cur.
Updated by buf_flush_stat_update(). Not protected by any mutex. */
static buf_flush_stat_t buf_flush_stat_sum;

/** Number of pages flushed through non flush_list flushes. */
static ulint buf_lru_flush_page_count = 0;

/* @} */

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Validates the flush list.
@return	true if ok */
static bool buf_flush_validate_low(void);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/** Insert a block in the flush_rbt and returns a pointer to its
predecessor or nullptr if no predecessor. The ordering is maintained
on the basis of the <oldest_modification, space, offset> key.
@return pointer to the predecessor or nullptr if no predecessor. */
static buf_page_t *buf_flush_insert_in_flush_rbt(
    buf_page_t *bpage) /*!< in: bpage to be inserted. */
{
  buf_page_t *prev = nullptr;
  const ib_rbt_node_t *c_node;
  const ib_rbt_node_t *p_node;

  ut_ad(buf_pool_mutex_own());

  /* Insert this buffer into the rbt. */
  c_node = rbt_insert(buf_pool->flush_rbt, &bpage, &bpage);
  ut_a(c_node != nullptr);

  /* Get the predecessor. */
  p_node = rbt_prev(buf_pool->flush_rbt, c_node);

  if (p_node != nullptr) {
    prev = *rbt_value(buf_page_t *, p_node);
    ut_a(prev != nullptr);
  }

  return (prev);
}

/** Delete a bpage from the flush_rbt. */
static void buf_flush_delete_from_flush_rbt(
    buf_page_t *bpage) /*!< in: bpage to be removed. */
{

#ifdef UNIV_DEBUG
  bool ret = false;
#endif /* UNIV_DEBUG */

  ut_ad(buf_pool_mutex_own());
#ifdef UNIV_DEBUG
  ret =
#endif /* UNIV_DEBUG */
      rbt_delete(buf_pool->flush_rbt, &bpage);
  ut_ad(ret);
}

/** Compare two modified blocks in the buffer pool. The key for comparison
is:
key = <oldest_modification, space, offset>
This comparison is used to maintian ordering of blocks in the
buf_pool->flush_rbt.
Note that for the purpose of flush_rbt, we only need to order blocks
on the oldest_modification. The other two fields are used to uniquely
identify the blocks.
@return < 0 if b2 < b1, 0 if b2 == b1, > 0 if b2 > b1 */
static int buf_flush_block_cmp(const void *p1, /*!< in: block1 */
                               const void *p2) /*!< in: block2 */
{
  int ret;
  const buf_page_t *b1;
  const buf_page_t *b2;

  ut_ad(p1 != nullptr);
  ut_ad(p2 != nullptr);

  b1 = *(const buf_page_t **) p1;
  b2 = *(const buf_page_t **) p2;

  ut_ad(b1 != nullptr);
  ut_ad(b2 != nullptr);

  ut_ad(b1->in_flush_list);
  ut_ad(b2->in_flush_list);

  if (b2->oldest_modification > b1->oldest_modification) {
    return (1);
  }

  if (b2->oldest_modification < b1->oldest_modification) {
    return (-1);
  }

  /* If oldest_modification is same then decide on the space. */
  ret = (int) (b2->space - b1->space);

  /* Or else decide ordering on the offset field. */
  return (ret ? ret : (int) (b2->offset - b1->offset));
}

void buf_flush_init_flush_rbt() {
  ut_ad(buf_pool_mutex_own());

  /* Create red black tree for speedy insertions in flush list. */
  buf_pool->flush_rbt = rbt_create(sizeof(buf_page_t *), buf_flush_block_cmp);
}

void buf_flush_free_flush_rbt() {
  buf_pool_mutex_enter();

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
  ut_a(buf_flush_validate_low());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

  rbt_free(buf_pool->flush_rbt);
  buf_pool->flush_rbt = nullptr;

  buf_pool_mutex_exit();
}

void buf_flush_insert_into_flush_list(buf_block_t *block) {
  ut_ad(buf_pool_mutex_own());
  ut_ad((UT_LIST_GET_FIRST(buf_pool->flush_list) == nullptr) ||
      (UT_LIST_GET_FIRST(buf_pool->flush_list)->oldest_modification <=
          block->page.oldest_modification));

  /* If we are in the recovery then we need to update the flush
  red-black tree as well. */
  if (buf_pool->flush_rbt != nullptr) {
    buf_flush_insert_sorted_into_flush_list(block);
    return;
  }

  ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
  ut_ad(block->page.in_LRU_list);
  ut_ad(block->page.in_page_hash);
  ut_ad(!block->page.in_flush_list);
  ut_d(block->page.in_flush_list = true);
  UT_LIST_ADD_FIRST(buf_pool->flush_list, &block->page);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
  ut_a(buf_flush_validate_low());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
}

void buf_flush_insert_sorted_into_flush_list(buf_block_t *block) {
  ut_ad(buf_pool_mutex_own());
  ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

  ut_ad(block->page.in_LRU_list);
  ut_ad(block->page.in_page_hash);
  ut_ad(!block->page.in_flush_list);
  ut_d(block->page.in_flush_list = true);

  buf_page_t *prev_b{};

  /* For the most part when this function is called the flush_rbt
  should not be nullptr. In a very rare boundary case it is possible
  that the flush_rbt has already been freed by the recovery thread
  before the last page was hooked up in the flush_list by the
  io-handler thread. In that case we'll  just do a simple
  linear search in the else block. */
  if (buf_pool->flush_rbt != nullptr) {

    prev_b = buf_flush_insert_in_flush_rbt(&block->page);

  } else {

    auto b = UT_LIST_GET_FIRST(buf_pool->flush_list);

    while (b != nullptr
        && b->oldest_modification > block->page.oldest_modification) {
      ut_ad(b->in_flush_list);
      prev_b = b;
      b = UT_LIST_GET_NEXT(list, b);
    }
  }

  if (prev_b == nullptr) {
    UT_LIST_ADD_FIRST(buf_pool->flush_list, &block->page);
  } else {
    UT_LIST_INSERT_AFTER(buf_pool->flush_list, prev_b, &block->page);
  }

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
  ut_a(buf_flush_validate_low());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
}

bool buf_flush_ready_for_replace(buf_page_t *bpage) {
  ut_ad(buf_pool_mutex_own());
  ut_ad(mutex_own(buf_page_get_mutex(bpage)));
  ut_ad(bpage->in_LRU_list);

  if (likely(buf_page_in_file(bpage))) {

    return (bpage->oldest_modification == 0 &&
        buf_page_get_io_fix(bpage) == BUF_IO_NONE &&
        bpage->buf_fix_count == 0);
  }

  ut_print_timestamp(ib_stream);
  ib_logger(ib_stream,
            "  Error: buffer block state %lu"
            " in the LRU list!\n",
            (ulong) buf_page_get_state(bpage));
  ut_print_buf(ib_stream, bpage, sizeof(buf_page_t));
  ib_logger(ib_stream, "\n");

  return (false);
}

/** Returns true if the block is modified and ready for flushing.
@return	true if can flush immediately */
inline bool buf_flush_ready_for_flush(
    buf_page_t *bpage,         /*!< in: buffer control block, must be
                               buf_page_in_file(bpage) */
    enum buf_flush flush_type) /*!< in: BUF_FLUSH_LRU or BUF_FLUSH_LIST */
{
  ut_a(buf_page_in_file(bpage));
  ut_ad(buf_pool_mutex_own());
  ut_ad(mutex_own(buf_page_get_mutex(bpage)));
  ut_ad(flush_type == to_int(BUF_FLUSH_LRU) || flush_type == BUF_FLUSH_LIST);

  if (bpage->oldest_modification != 0 &&
      buf_page_get_io_fix(bpage) == BUF_IO_NONE) {
    ut_ad(bpage->in_flush_list);

    if (flush_type != BUF_FLUSH_LRU) {

      return (true);

    } else if (bpage->buf_fix_count == 0) {

      /* If we are flushing the LRU list, to avoid deadlocks
      we require the block not to be bufferfixed, and hence
      not latched. */

      return (true);
    }
  }

  return (false);
}

/** Remove a block from the flush list of modified blocks. */

void buf_flush_remove(
    buf_page_t *bpage) /*!< in: pointer to the block in question */
{
  ut_ad(buf_pool_mutex_own());
  ut_ad(mutex_own(buf_page_get_mutex(bpage)));
  ut_ad(bpage->in_flush_list);

  switch (buf_page_get_state(bpage)) {
    case BUF_BLOCK_NOT_USED:
    case BUF_BLOCK_READY_FOR_USE:
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_REMOVE_HASH:
      ut_error;
      break;
    case BUF_BLOCK_FILE_PAGE:
      UT_LIST_REMOVE(buf_pool->flush_list, bpage);
      break;
  }

  /* If the flush_rbt is active then delete from it as well. */
  if (likely_null(buf_pool->flush_rbt)) {
    buf_flush_delete_from_flush_rbt(bpage);
  }

  /* Must be done after we have removed it from the flush_rbt
  because we assert on in_flush_list in comparison function. */
  ut_d(bpage->in_flush_list = false);

  bpage->oldest_modification = 0;

  auto check = [](const buf_page_t *ptr) { ut_ad(ptr->in_flush_list); };
  ut_list_validate(buf_pool->flush_list, check);
}

/** Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage has already been
copied to dpage. */

void buf_flush_relocate_on_flush_list(
    buf_page_t *bpage, /*!< in/out: control block being moved */
    buf_page_t *dpage) /*!< in/out: destination block */
{
  buf_page_t *prev;
  buf_page_t *prev_b = nullptr;

  ut_ad(buf_pool_mutex_own());

  ut_ad(mutex_own(buf_page_get_mutex(bpage)));

  ut_ad(bpage->in_flush_list);
  ut_ad(dpage->in_flush_list);

  /* If recovery is active we must swap the control blocks in
  the flush_rbt as well. */
  if (likely_null(buf_pool->flush_rbt)) {
    buf_flush_delete_from_flush_rbt(bpage);
    prev_b = buf_flush_insert_in_flush_rbt(dpage);
  }

  /* Must be done after we have removed it from the flush_rbt
  because we assert on in_flush_list in comparison function. */
  ut_d(bpage->in_flush_list = false);

  prev = UT_LIST_GET_PREV(list, bpage);
  UT_LIST_REMOVE(buf_pool->flush_list, bpage);

  if (prev) {
    ut_ad(prev->in_flush_list);
    UT_LIST_INSERT_AFTER(buf_pool->flush_list, prev, dpage);
  } else {
    UT_LIST_ADD_FIRST(buf_pool->flush_list, dpage);
  }

  /* Just an extra check. Previous in flush_list
  should be the same control block as in flush_rbt. */
  ut_a(!buf_pool->flush_rbt || prev_b == prev);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
  ut_a(buf_flush_validate_low());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
}

/** Updates the flush system data structures when a write is completed. */

void buf_flush_write_complete(
    buf_page_t *bpage) /*!< in: pointer to the block in question */
{
  enum buf_flush flush_type;

  ut_ad(bpage);

  buf_flush_remove(bpage);

  flush_type = buf_page_get_flush_type(bpage);
  buf_pool->n_flush[flush_type]--;

  if (flush_type == BUF_FLUSH_LRU) {
    /* Put the block to the end of the LRU list to wait to be
    moved to the free list */

    buf_LRU_make_block_old(bpage);

    buf_pool->LRU_flush_ended++;
  }

  /* ib_logger(ib_stream, "n pending flush %lu\n",
  buf_pool->n_flush[flush_type]); */

  if ((buf_pool->n_flush[flush_type] == 0) &&
      (buf_pool->init_flush[flush_type] == false)) {

    /* The running flush batch has ended */

    os_event_set(buf_pool->no_flush[flush_type]);
  }
}

/** Flush a batch of writes to the datafiles that have already been
written by the OS. */
static void buf_flush_sync_datafiles(void) {
  /* Wake possible simulated aio thread to actually post the
  writes to the operating system */
  os_aio_simulated_wake_handler_threads();

  /* Wait that all async writes to tablespaces have been posted to
  the OS */
  os_aio_wait_until_no_pending_writes();

  /* Now we flush the data to disk (for example, with fsync) */
  fil_flush_file_spaces(FIL_TABLESPACE);

  return;
}

/** Flushes possible buffered writes from the doublewrite memory buffer to disk,
and also wakes up the aio thread if simulated aio is used. It is very
important to call this function after a batch of writes has been posted,
and also when we may have to wait for a page latch! Otherwise a deadlock
of threads can occur. */
static void buf_flush_buffered_writes(void) {
  byte *write_buf;
  ulint len;
  ulint len2;
  ulint i;

  if (!srv_use_doublewrite_buf || trx_doublewrite == nullptr) {
    /* Sync the writes to the disk. */
    buf_flush_sync_datafiles();
    return;
  }

  mutex_enter(&(trx_doublewrite->mutex));

  /* Write first to doublewrite buffer blocks. We use synchronous
  aio and thus know that file write has been completed when the
  control returns. */

  if (trx_doublewrite->first_free == 0) {

    mutex_exit(&(trx_doublewrite->mutex));

    return;
  }

  for (i = 0; i < trx_doublewrite->first_free; i++) {

    const buf_block_t *block;

    block = (buf_block_t *) trx_doublewrite->buf_block_arr[i];

    if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {
      /* No simple validate for compressed pages exists. */
      continue;
    }

    if (unlikely(memcmp(block->frame + (FIL_PAGE_LSN + 4),
                        block->frame +
                            (UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
                        4))) {
      ut_print_timestamp(ib_stream);
      ib_logger(ib_stream, "  ERROR: The page to be written"
                           " seems corrupt!\n"
                           "The lsn fields do not match!"
                           " Noticed in the buffer pool\n"
                           "before posting to the"
                           " doublewrite buffer.\n");
    }

    if (!block->check_index_page_at_flush) {
    } else if (page_is_comp(block->frame)) {
      if (unlikely(!page_simple_validate_new(block->frame))) {
        corrupted_page:
        buf_page_print(block->frame, 0);

        ut_print_timestamp(ib_stream);
        ib_logger(ib_stream,
                  "  Apparent corruption of an"
                  " index page n:o %lu in space %lu\n"
                  "to be written to data file."
                  " We intentionally crash server\n"
                  "to prevent corrupt data"
                  " from ending up in data\n"
                  "files.\n",
                  (ulong) buf_block_get_page_no(block),
                  (ulong) buf_block_get_space(block));

        ut_error;
      }
    } else if (unlikely(!page_simple_validate_old(block->frame))) {

      goto corrupted_page;
    }
  }

  /* increment the doublewrite flushed pages counter */
  srv_dblwr_pages_written += trx_doublewrite->first_free;
  srv_dblwr_writes++;

  len = ut_min(TRX_SYS_DOUBLEWRITE_BLOCK_SIZE, trx_doublewrite->first_free) *
      UNIV_PAGE_SIZE;

  write_buf = trx_doublewrite->write_buf;
  i = 0;

  fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, trx_doublewrite->block1, 0, len,
         (void *) write_buf, nullptr);

  for (len2 = 0; len2 + UNIV_PAGE_SIZE <= len; len2 += UNIV_PAGE_SIZE, i++) {
    const buf_block_t
        *block = (buf_block_t *) trx_doublewrite->buf_block_arr[i];

    if (likely(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE) &&
        unlikely(memcmp(write_buf + len2 + (FIL_PAGE_LSN + 4),
                        write_buf + len2 +
                            (UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
                        4))) {
      ut_print_timestamp(ib_stream);
      ib_logger(ib_stream, "  ERROR: The page to be written"
                           " seems corrupt!\n"
                           "The lsn fields do not match!"
                           " Noticed in the doublewrite block1.\n");
    }
  }

  if (trx_doublewrite->first_free <= TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
    goto flush;
  }

  len = (trx_doublewrite->first_free - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) *
      UNIV_PAGE_SIZE;

  write_buf = trx_doublewrite->write_buf +
      TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;
  ut_ad(i == TRX_SYS_DOUBLEWRITE_BLOCK_SIZE);

  fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, trx_doublewrite->block2, 0, len,
         (void *) write_buf, nullptr);

  for (len2 = 0; len2 + UNIV_PAGE_SIZE <= len; len2 += UNIV_PAGE_SIZE, i++) {
    const buf_block_t
        *block = (buf_block_t *) trx_doublewrite->buf_block_arr[i];

    if (likely(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE) &&
        unlikely(memcmp(write_buf + len2 + (FIL_PAGE_LSN + 4),
                        write_buf + len2 +
                            (UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
                        4))) {
      ut_print_timestamp(ib_stream);
      ib_logger(ib_stream, "  ERROR: The page to be"
                           " written seems corrupt!\n"
                           "The lsn fields do not match!"
                           " Noticed in"
                           " the doublewrite block2.\n");
    }
  }

  flush:
  /* Now flush the doublewrite buffer data to disk */

  fil_flush(TRX_SYS_SPACE);

  /* We know that the writes have been flushed to disk now
  and in recovery we will find them in the doublewrite buffer
  blocks. Next do the writes to the intended positions. */

  for (i = 0; i < trx_doublewrite->first_free; i++) {
    const buf_block_t
        *block = (buf_block_t *) trx_doublewrite->buf_block_arr[i];

    ut_a(buf_page_in_file(&block->page));

    ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

    if (unlikely(memcmp(block->frame + (FIL_PAGE_LSN + 4),
                        block->frame +
                            (UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
                        4))) {
      ut_print_timestamp(ib_stream);
      ib_logger(ib_stream,
                "  ERROR: The page to be written"
                " seems corrupt!\n"
                "The lsn fields do not match!"
                " Noticed in the buffer pool\n"
                "after posting and flushing"
                " the doublewrite buffer.\n"
                "Page buf fix count %lu,"
                " io fix %lu, state %lu\n",
                (ulong) block->page.buf_fix_count,
                (ulong) buf_block_get_io_fix(block),
                (ulong) buf_block_get_state(block));
    }

    fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER, false,
           buf_block_get_space(block), buf_block_get_page_no(block), 0,
           UNIV_PAGE_SIZE, (void *) block->frame, (void *) block);

    /* Increment the counter of I/O operations used
    for selecting LRU policy. */
    buf_LRU_stat_inc_io();
  }

  /* Sync the writes to the disk. */
  buf_flush_sync_datafiles();

  /* We can now reuse the doublewrite memory buffer: */
  trx_doublewrite->first_free = 0;

  mutex_exit(&(trx_doublewrite->mutex));
}

/** Posts a buffer page for writing. If the doublewrite memory buffer is
full, calls buf_flush_buffered_writes and waits for for free space to
appear. */
static void buf_flush_post_to_doublewrite_buf(
    buf_page_t *bpage) /*!< in: buffer block to write */
{
  try_again:
  mutex_enter(&(trx_doublewrite->mutex));

  ut_a(buf_page_in_file(bpage));

  if (trx_doublewrite->first_free >= 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
    mutex_exit(&(trx_doublewrite->mutex));

    buf_flush_buffered_writes();

    goto try_again;
  }

  ut_a(buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);

  memcpy(trx_doublewrite->write_buf +
             UNIV_PAGE_SIZE * trx_doublewrite->first_free,
         ((buf_block_t *) bpage)->frame, UNIV_PAGE_SIZE);

  trx_doublewrite->buf_block_arr[trx_doublewrite->first_free] = bpage;

  trx_doublewrite->first_free++;

  if (trx_doublewrite->first_free >= 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
    mutex_exit(&(trx_doublewrite->mutex));

    buf_flush_buffered_writes();

    return;
  }

  mutex_exit(&(trx_doublewrite->mutex));
}

void buf_flush_init_for_writing(byte *page, uint64_t newest_lsn) {
  ut_ad(page != nullptr);

  /* Write the newest modification lsn to the page header and trailer */
  mach_write_to_8(page + FIL_PAGE_LSN, newest_lsn);

  mach_write_to_8(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
                  newest_lsn);

  /* Store the new formula checksum */

  mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
                  srv_use_checksums ? buf_calc_page_new_checksum(page)
                                    : BUF_NO_CHECKSUM_MAGIC);

  /* We overwrite the first 4 bytes of the end lsn field to store
  the old formula checksum. Since it depends also on the field
  FIL_PAGE_SPACE_OR_CHKSUM, it has to be calculated after storing the
  new formula checksum. */

  mach_write_to_4(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
                  srv_use_checksums ? buf_calc_page_old_checksum(page)
                                    : BUF_NO_CHECKSUM_MAGIC);
}

/** Does an asynchronous write of a buffer page. NOTE: in simulated aio and
also when the doublewrite buffer is used, we must call
buf_flush_buffered_writes after we have posted a batch of writes! */
static void
buf_flush_write_block_low(buf_page_t *bpage) /*!< in: buffer block to write */
{
  page_t *frame = nullptr;
#ifdef UNIV_LOG_DEBUG
  static bool univ_log_debug_warned;
#endif /* UNIV_LOG_DEBUG */

  ut_ad(buf_page_in_file(bpage));

  /* We are not holding buf_pool_mutex or block_mutex here.
  Nevertheless, it is safe to access bpage, because it is
  io_fixed and oldest_modification != 0.  Thus, it cannot be
  relocated in the buffer pool or removed from flush_list or
  LRU_list. */
  ut_ad(!buf_pool_mutex_own());
  ut_ad(!mutex_own(buf_page_get_mutex(bpage)));
  ut_ad(buf_page_get_io_fix(bpage) == BUF_IO_WRITE);
  ut_ad(bpage->oldest_modification != 0);
  ut_ad(bpage->newest_modification != 0);

#ifdef UNIV_LOG_DEBUG
  if (!univ_log_debug_warned) {
    univ_log_debug_warned = true;
    ib_logger(ib_stream, "Warning: cannot force log to disk if"
                         " UNIV_LOG_DEBUG is defined!\n"
                         "Crash recovery will not work!\n");
  }
#else
  /* Force the log to the disk before writing the modified block */
  log_write_up_to(bpage->newest_modification, LOG_WAIT_ALL_GROUPS, true);
#endif
  switch (buf_page_get_state(bpage)) {
    case BUF_BLOCK_NOT_USED:
    case BUF_BLOCK_READY_FOR_USE:
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_REMOVE_HASH:
      ut_error;
      break;
    case BUF_BLOCK_FILE_PAGE:
      frame = ((buf_block_t *) bpage)->frame;
      buf_flush_init_for_writing(((buf_block_t *) bpage)->frame,
                                 bpage->newest_modification);
      break;
  }

  if (!srv_use_doublewrite_buf || !trx_doublewrite) {
    fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER, false,
           buf_page_get_space(bpage), buf_page_get_page_no(bpage), 0,
           UNIV_PAGE_SIZE, frame, bpage);
  } else {
    buf_flush_post_to_doublewrite_buf(bpage);
  }
}

/** Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: in simulated aio we must call
os_aio_simulated_wake_handler_threads after we have posted a batch of
writes! NOTE: buf_pool_mutex and buf_page_get_mutex(bpage) must be
held upon entering this function, and they will be released by this
function. */
static void buf_flush_page(buf_page_t *bpage, /*!< in: buffer control block */
                           enum buf_flush flush_type) /*!< in: BUF_FLUSH_LRU
                                                      or BUF_FLUSH_LIST */
{
  mutex_t *block_mutex;

  ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
  ut_ad(buf_pool_mutex_own());
  ut_ad(buf_page_in_file(bpage));

  block_mutex = buf_page_get_mutex(bpage);
  ut_ad(mutex_own(block_mutex));

  ut_ad(buf_flush_ready_for_flush(bpage, flush_type));

  buf_page_set_io_fix(bpage, BUF_IO_WRITE);

  buf_page_set_flush_type(bpage, flush_type);

  if (buf_pool->n_flush[flush_type] == 0) {

    os_event_reset(buf_pool->no_flush[flush_type]);
  }

  buf_pool->n_flush[flush_type]++;

  switch (flush_type) {
    bool is_s_latched;
    case BUF_FLUSH_LIST:
      /* If the simulated aio thread is not running, we must
      not wait for any latch, as we may end up in a deadlock:
      if buf_fix_count == 0, then we know we need not wait */

      is_s_latched = (bpage->buf_fix_count == 0);
      if (is_s_latched) {
        rw_lock_s_lock_gen(&((buf_block_t *) bpage)->lock, BUF_IO_WRITE);
      }

      mutex_exit(block_mutex);
      buf_pool_mutex_exit();

      /* Even though bpage is not protected by any mutex at
      this point, it is safe to access bpage, because it is
      io_fixed and oldest_modification != 0.  Thus, it
      cannot be relocated in the buffer pool or removed from
      flush_list or LRU_list. */

      if (!is_s_latched) {
        buf_flush_buffered_writes();

        rw_lock_s_lock_gen(&((buf_block_t *) bpage)->lock, BUF_IO_WRITE);
      }

      break;

    case BUF_FLUSH_LRU:
      /* VERY IMPORTANT:
      Because any thread may call the LRU flush, even when owning
      locks on pages, to avoid deadlocks, we must make sure that the
      s-lock is acquired on the page without waiting: this is
      accomplished because buf_flush_ready_for_flush() must hold,
      and that requires the page not to be bufferfixed. */

      rw_lock_s_lock_gen(&((buf_block_t *) bpage)->lock, BUF_IO_WRITE);

      /* Note that the s-latch is acquired before releasing the
      buf_pool mutex: this ensures that the latch is acquired
      immediately. */

      mutex_exit(block_mutex);
      buf_pool_mutex_exit();
      break;

    default:
      ut_error;
  }

  /* Even though bpage is not protected by any mutex at this
  point, it is safe to access bpage, because it is io_fixed and
  oldest_modification != 0.  Thus, it cannot be relocated in the
  buffer pool or removed from flush_list or LRU_list. */

#ifdef UNIV_DEBUG
  if (buf_debug_prints) {
    ib_logger(ib_stream, "Flushing %u space %u page %u\n", flush_type,
              bpage->space, bpage->offset);
  }
#endif /* UNIV_DEBUG */
  buf_flush_write_block_low(bpage);
}

/** Flushes to disk all flushable pages within the flush area.
@return	number of pages flushed */
static ulint
buf_flush_try_neighbors(ulint space,               /*!< in: space id */
                        ulint offset,              /*!< in: page offset */
                        enum buf_flush flush_type) /*!< in: BUF_FLUSH_LRU or
                                                   BUF_FLUSH_LIST */
{
  buf_page_t *bpage;
  ulint low, high;
  ulint count = 0;
  ulint i;

  ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

  if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN) {
    /* If there is little space, it is better not to flush any
    block except from the end of the LRU list */

    low = offset;
    high = offset + 1;
  } else {
    /* When flushed, dirty blocks are searched in neighborhoods of
    this size, and flushed along with the original page. */

    ulint buf_flush_area =
        ut_min(BUF_READ_AHEAD_AREA, buf_pool->curr_size / 16);

    low = (offset / buf_flush_area) * buf_flush_area;
    high = (offset / buf_flush_area + 1) * buf_flush_area;
  }

  /* ib_logger(ib_stream, "Flush area: low %lu high %lu\n", low, high); */

  if (high > fil_space_get_size(space)) {
    high = fil_space_get_size(space);
  }

  buf_pool_mutex_enter();

  for (i = low; i < high; i++) {

    bpage = buf_page_hash_get(space, i);

    if (!bpage) {

      continue;
    }

    ut_a(buf_page_in_file(bpage));

    /* We avoid flushing 'non-old' blocks in an LRU flush,
    because the flushed blocks are soon freed */

    if (flush_type != BUF_FLUSH_LRU || i == offset || buf_page_is_old(bpage)) {
      mutex_t *block_mutex = buf_page_get_mutex(bpage);

      mutex_enter(block_mutex);

      if (buf_flush_ready_for_flush(bpage, flush_type) &&
          (i == offset || !bpage->buf_fix_count)) {
        /* We only try to flush those
        neighbors != offset where the buf fix count is
        zero, as we then know that we probably can
        latch the page without a semaphore wait.
        Semaphore waits are expensive because we must
        flush the doublewrite buffer before we start
        waiting. */

        buf_flush_page(bpage, flush_type);
        ut_ad(!mutex_own(block_mutex));
        count++;

        buf_pool_mutex_enter();
      } else {
        mutex_exit(block_mutex);
      }
    }
  }

  buf_pool_mutex_exit();

  return (count);
}

/** This utility flushes dirty blocks from the end of the LRU list or
flush_list. NOTE 1: in the case of an LRU flush the calling thread may own
latches to pages: to avoid deadlocks, this function must be written so that it
cannot end up waiting for these latches! NOTE 2: in the case of a flush list
flush, the calling thread is not allowed to own any latches on pages!
@return number of blocks for which the write request was queued;
ULINT_UNDEFINED if there was a flush of the same type already running */

ulint buf_flush_batch(
    enum buf_flush flush_type, /*!< in: BUF_FLUSH_LRU or
                               BUF_FLUSH_LIST; if BUF_FLUSH_LIST,
                               then the caller must not own any
                               latches on pages */
    ulint min_n,               /*!< in: wished minimum mumber of blocks
                               flushed (it is not guaranteed that the
                               actual number is that big, though) */
    uint64_t lsn_limit)        /*!< in the case BUF_FLUSH_LIST all
                                  blocks whose oldest_modification is
                                  smaller than this should be flushed
                                  (if their number does not exceed
                                  min_n), otherwise ignored */
{
  buf_page_t *bpage;
  ulint page_count = 0;
  ulint space;
  ulint offset;

  ut_ad((flush_type == BUF_FLUSH_LRU) || (flush_type == BUF_FLUSH_LIST));
#ifdef UNIV_SYNC_DEBUG
  ut_ad((flush_type != BUF_FLUSH_LIST) || sync_thread_levels_empty_gen(true));
#endif /* UNIV_SYNC_DEBUG */
  buf_pool_mutex_enter();

  if ((buf_pool->n_flush[flush_type] > 0) ||
      (buf_pool->init_flush[flush_type] == true)) {

    /* There is already a flush batch of the same type running */

    buf_pool_mutex_exit();

    return (ULINT_UNDEFINED);
  }

  buf_pool->init_flush[flush_type] = true;

  for (;;) {
    flush_next:
    /* If we have flushed enough, leave the loop */
    if (page_count >= min_n) {

      break;
    }

    /* Start from the end of the list looking for a suitable
    block to be flushed. */

    if (flush_type == BUF_FLUSH_LRU) {
      bpage = UT_LIST_GET_LAST(buf_pool->LRU);
    } else {
      ut_ad(flush_type == BUF_FLUSH_LIST);

      bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
      if (!bpage || bpage->oldest_modification >= lsn_limit) {
        /* We have flushed enough */

        break;
      }
      ut_ad(bpage->in_flush_list);
    }

    /* Note that after finding a single flushable page, we try to
    flush also all its neighbors, and after that start from the
    END of the LRU list or flush list again: the list may change
    during the flushing and we cannot safely preserve within this
    function a pointer to a block in the list! */

    do {
      mutex_t *block_mutex = buf_page_get_mutex(bpage);
      bool ready;

      ut_a(buf_page_in_file(bpage));

      mutex_enter(block_mutex);
      ready = buf_flush_ready_for_flush(bpage, flush_type);
      mutex_exit(block_mutex);

      if (ready) {
        space = buf_page_get_space(bpage);
        offset = buf_page_get_page_no(bpage);

        buf_pool_mutex_exit();

        /* Try to flush also all the neighbors */
        page_count += buf_flush_try_neighbors(space, offset, flush_type);

        buf_pool_mutex_enter();
        goto flush_next;

      } else if (flush_type == BUF_FLUSH_LRU) {
        bpage = UT_LIST_GET_PREV(LRU, bpage);
      } else {
        ut_ad(flush_type == BUF_FLUSH_LIST);

        bpage = UT_LIST_GET_PREV(list, bpage);
        ut_ad(!bpage || bpage->in_flush_list);
      }
    } while (bpage != nullptr);

    /* If we could not find anything to flush, leave the loop */

    break;
  }

  buf_pool->init_flush[flush_type] = false;

  if (buf_pool->n_flush[flush_type] == 0) {

    /* The running flush batch has ended */

    os_event_set(buf_pool->no_flush[flush_type]);
  }

  buf_pool_mutex_exit();

  buf_flush_buffered_writes();

#ifdef UNIV_DEBUG
  if (buf_debug_prints && page_count > 0) {
    ut_a(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
    ib_logger(ib_stream,
              flush_type == BUF_FLUSH_LRU
              ? "Flushed %lu pages in LRU flush\n"
              : "Flushed %lu pages in flush list flush\n",
              (ulong) page_count);
  }
#endif /* UNIV_DEBUG */

  srv_buf_pool_flushed += page_count;

  /* We keep track of all flushes happening as part of LRU
  flush. When estimating the desired rate at which flush_list
  should be flushed we factor in this value. */
  if (flush_type == BUF_FLUSH_LRU) {
    buf_lru_flush_page_count += page_count;
  }

  return (page_count);
}

/** Waits until a flush batch of the given type ends */

void buf_flush_wait_batch_end(
    enum buf_flush type) /*!< in: BUF_FLUSH_LRU or BUF_FLUSH_LIST */
{
  ut_ad((type == BUF_FLUSH_LRU) || (type == BUF_FLUSH_LIST));

  os_event_wait(buf_pool->no_flush[type]);
}

/** Gives a recommendation of how many blocks should be flushed to establish
a big enough margin of replaceable blocks near the end of the LRU list
and in the free list.
@return number of blocks which should be flushed from the end of the
LRU list */
static ulint buf_flush_LRU_recommendation(void) {
  buf_page_t *bpage;
  ulint n_replaceable;
  ulint distance = 0;

  buf_pool_mutex_enter();

  n_replaceable = UT_LIST_GET_LEN(buf_pool->free);

  bpage = UT_LIST_GET_LAST(buf_pool->LRU);

  while (
      (bpage != nullptr) &&
          (n_replaceable < BUF_FLUSH_FREE_BLOCK_MARGIN + BUF_FLUSH_EXTRA_MARGIN)
          &&
              (distance < BUF_LRU_FREE_SEARCH_LEN)) {

    mutex_t *block_mutex = buf_page_get_mutex(bpage);

    mutex_enter(block_mutex);

    if (buf_flush_ready_for_replace(bpage)) {
      n_replaceable++;
    }

    mutex_exit(block_mutex);

    distance++;

    bpage = UT_LIST_GET_PREV(LRU, bpage);
  }

  buf_pool_mutex_exit();

  if (n_replaceable >= BUF_FLUSH_FREE_BLOCK_MARGIN) {

    return (0);
  }

  return (BUF_FLUSH_FREE_BLOCK_MARGIN + BUF_FLUSH_EXTRA_MARGIN - n_replaceable);
}

/** Flushes pages from the end of the LRU list if there is too small a margin
of replaceable pages there or in the free list. VERY IMPORTANT: this function
is called also by threads which have locks on pages. To avoid deadlocks, we
flush only pages such that the s-lock required for flushing can be acquired
immediately, without waiting. */

void buf_flush_free_margin(void) {
  ulint n_to_flush;
  ulint n_flushed;

  n_to_flush = buf_flush_LRU_recommendation();

  if (n_to_flush > 0) {
    n_flushed = buf_flush_batch(BUF_FLUSH_LRU, n_to_flush, 0);
    if (n_flushed == ULINT_UNDEFINED) {
      /* There was an LRU type flush batch already running;
      let us wait for it to end */

      buf_flush_wait_batch_end(BUF_FLUSH_LRU);
    }
  }
}

/** Update the historical stats that we are collecting for flush rate
heuristics at the end of each interval.
Flush rate heuristic depends on (a) rate of redo log generation and
(b) the rate at which LRU flush is happening. */

void buf_flush_stat_update(void) {
  buf_flush_stat_t *item;
  uint64_t lsn_diff;
  uint64_t lsn;
  ulint n_flushed;

  lsn = log_get_lsn();
  if (buf_flush_stat_cur.redo == 0) {
    /* First time around. Just update the current LSN
    and return. */
    buf_flush_stat_cur.redo = lsn;
    return;
  }

  item = &buf_flush_stat_arr[buf_flush_stat_arr_ind];

  /* values for this interval */
  lsn_diff = lsn - buf_flush_stat_cur.redo;
  n_flushed = buf_lru_flush_page_count - buf_flush_stat_cur.n_flushed;

  /* add the current value and subtract the obsolete entry. */
  buf_flush_stat_sum.redo += lsn_diff - item->redo;
  buf_flush_stat_sum.n_flushed += n_flushed - item->n_flushed;

  /* put current entry in the array. */
  item->redo = lsn_diff;
  item->n_flushed = n_flushed;

  /* update the index */
  buf_flush_stat_arr_ind++;
  buf_flush_stat_arr_ind %= BUF_FLUSH_STAT_N_INTERVAL;

  /* reset the current entry. */
  buf_flush_stat_cur.redo = lsn;
  buf_flush_stat_cur.n_flushed = buf_lru_flush_page_count;
}

/** Determines the fraction of dirty pages that need to be flushed based
on the speed at which we generate redo log. Note that if redo log
is generated at a significant rate without corresponding increase
in the number of dirty pages (for example, an in-memory workload)
it can cause IO bursts of flushing. This function implements heuristics
to avoid this burstiness.
@return	number of dirty pages to be flushed / second */

ulint buf_flush_get_desired_flush_rate(void) {
  ulint redo_avg;
  ulint lru_flush_avg;
  ulint n_dirty;
  ulint n_flush_req;
  lint rate;
  uint64_t lsn = log_get_lsn();
  ulint log_capacity = log_get_capacity();

  /* log_capacity should never be zero after the initialization
  of log subsystem. */
  ut_ad(log_capacity != 0);

  /* Get total number of dirty pages. It is OK to access
  flush_list without holding any mtex as we are using this
  only for heuristics. */
  n_dirty = UT_LIST_GET_LEN(buf_pool->flush_list);

  /* An overflow can happen if we generate more than 2^32 bytes
  of redo in this interval i.e.: 4G of redo in 1 second. We can
  safely consider this as infinity because if we ever come close
  to 4G we'll start a synchronous flush of dirty pages. */
  /* redo_avg below is average at which redo is generated in
  past BUF_FLUSH_STAT_N_INTERVAL + redo generated in the current
  interval. */
  redo_avg = (ulint) (buf_flush_stat_sum.redo / BUF_FLUSH_STAT_N_INTERVAL +
      (lsn - buf_flush_stat_cur.redo));

  /* An overflow can happen possibly if we flush more than 2^32
  pages in BUF_FLUSH_STAT_N_INTERVAL. This is a very very
  unlikely scenario. Even when this happens it means that our
  flush rate will be off the mark. It won't affect correctness
  of any subsystem. */
  /* lru_flush_avg below is rate at which pages are flushed as
  part of LRU flush in past BUF_FLUSH_STAT_N_INTERVAL + the
  number of pages flushed in the current interval. */
  lru_flush_avg = buf_flush_stat_sum.n_flushed / BUF_FLUSH_STAT_N_INTERVAL +
      (buf_lru_flush_page_count - buf_flush_stat_cur.n_flushed);

  n_flush_req = (n_dirty * redo_avg) / log_capacity;

  /* The number of pages that we want to flush from the flush
  list is the difference between the required rate and the
  number of pages that we are historically flushing from the
  LRU list */
  rate = n_flush_req - lru_flush_avg;
  return (rate > 0 ? (ulint) rate : 0);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Validates the flush list.
@return	true if ok */
static bool buf_flush_validate_low() {
  const ib_rbt_node_t *rnode{};

  UT_LIST_CHECK(buf_pool->flush_list);

  auto bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);

  /* If we are in recovery mode i.e.: flush_rbt != nullptr
  then each block in the flush_list must also be present
  in the flush_rbt. */
  if (buf_pool->flush_rbt != nullptr) {
    rnode = rbt_first(buf_pool->flush_rbt);
  }

  while (bpage != nullptr) {
    const uint64_t om = bpage->oldest_modification;
    ut_ad(bpage->in_flush_list);
    ut_a(buf_page_in_file(bpage));
    ut_a(om > 0);

    if (buf_pool->flush_rbt != nullptr) {
      ut_a(rnode != nullptr);

      auto rpage = *rbt_value(buf_page_t *, rnode);

      ut_a(rpage != nullptr);
      ut_a(rpage == bpage);

      rnode = rbt_next(buf_pool->flush_rbt, rnode);
    }

    bpage = UT_LIST_GET_NEXT(list, bpage);

    ut_a(!bpage || om >= bpage->oldest_modification);
  }

  /* By this time we must have exhausted the traversal of
  flush_rbt (if active) as well. */
  ut_a(rnode == nullptr);

  return (true);
}

/** Validates the flush list.
@return	true if ok */

bool buf_flush_validate(void) {
  bool ret;

  buf_pool_mutex_enter();

  ret = buf_flush_validate_low();

  buf_pool_mutex_exit();

  return (ret);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
