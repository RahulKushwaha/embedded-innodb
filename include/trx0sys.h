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

/** @file include/trx0sys.h
Transaction system

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#pragma once

#include "innodb0types.h"

// FIXME: This dependency needs to be fixed in mtr0log.ic
/** Set to true when the doublewrite buffer is being created */
extern bool trx_doublewrite_buf_is_being_created;

#include "buf0buf.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "page0types.h"
#include "read0types.h"
#include "sync0sync.h"
#include "trx0types.h"
#include "ut0byte.h"
#include "ut0lst.h"
#include "data0type.h"
#include "mtr0log.h"
#include "srv0srv.h"
#include "trx0trx.h"

/** The automatically created system rollback segment has this id */
constexpr ulint TRX_SYS_SYSTEM_RSEG_ID = 0;

/** The transaction system tablespace.
Space id and page no where the trx system file copy resides */
constexpr auto TRX_SYS_SPACE = SYS_TABLESPACE;

/** Page numnber of the transaction system meta data. */
constexpr auto TRX_SYS_PAGE_NO = FSP_TRX_SYS_PAGE_NO;

/** The offset of the transaction system header on the page */
constexpr auto TRX_SYS = FSEG_PAGE_DATA;

/** Transaction system header */
/*@{ */

/** The maximum trx id or trx number modulo TRX_SYS_TRX_ID_UPDATE_MARGIN
written to a file page by any transaction; the assignment of transaction
ids continues from this number rounded up by TRX_SYS_TRX_ID_UPDATE_MARGIN
plus TRX_SYS_TRX_ID_UPDATE_MARGIN when the database is started */
constexpr ulint TRX_SYS_TRX_ID_STORE = 0;

/** segment header for the tablespace segment the trx system is created into */
constexpr ulint TRX_SYS_FSEG_HEADER = 8;

/** The start of the array of rollback segment specification slots */
constexpr ulint TRX_SYS_RSEGS = 8 + FSEG_HEADER_SIZE;

/*@} */

/** Maximum number of rollback segments: the number of segment
specification slots in the transaction system array; rollback segment
id must fit in one byte, therefore 256; each slot is currently 8 bytes
in size */
constexpr ulint TRX_SYS_N_RSEGS = 256;

static_assert(UNIV_PAGE_SIZE >= 4096, "error UNIV_PAGE_SIZE < 4096");

/** Doublewrite buffer */
/* @{ */
/** The offset of the doublewrite buffer header on the trx system header page */
constexpr ulint TRX_SYS_DOUBLEWRITE = UNIV_PAGE_SIZE - 200;

/** fseg header of the fseg containing the doublewrite buffer */
constexpr ulint TRX_SYS_DOUBLEWRITE_FSEG = 0;

/** 4-byte magic number which shows if we already have created the
doublewrite buffer */
constexpr auto TRX_SYS_DOUBLEWRITE_MAGIC = FSEG_HEADER_SIZE;

/** Page number of the first page in the first sequence of 64
(= FSP_EXTENT_SIZE) consecutive pages in the doublewrite buffer */
constexpr auto TRX_SYS_DOUBLEWRITE_BLOCK1 = 4 + FSEG_HEADER_SIZE;

/** Page number of the first page in the second sequence of 64
consecutive pages in the doublewrite buffer */
constexpr auto TRX_SYS_DOUBLEWRITE_BLOCK2 = 8 + FSEG_HEADER_SIZE;

/** We repeat TRX_SYS_DOUBLEWRITE_MAGIC, TRX_SYS_DOUBLEWRITE_BLOCK1,
TRX_SYS_DOUBLEWRITE_BLOCK2 so that if the trx sys header is half-written
to disk, we still may be able to recover the information */
constexpr auto TRX_SYS_DOUBLEWRITE_REPEAT = 12;

/** If this is not yet set to TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N,
we must reset the doublewrite buffer, because starting from 4.1.x the
space id of a data page is stored into
FIL_PAGE_ARCH_LOG_NO_OR_SPACE_NO. */
constexpr auto TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED = 24 + FSEG_HEADER_SIZE;

/** Contents of TRX_SYS_DOUBLEWRITE_MAGIC */
constexpr ulint TRX_SYS_DOUBLEWRITE_MAGIC_N = 536853855;

/** Contents of TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED */
constexpr ulint TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N = 1783657386;

/** Size of the doublewrite block in pages */
constexpr auto TRX_SYS_DOUBLEWRITE_BLOCK_SIZE = FSP_EXTENT_SIZE;

/* @} */

/** File format tag */
/* @{ */

/** The offset of the file format tag on the trx system header page
(TRX_SYS_PAGE_NO of TRX_SYS_SPACE) */
constexpr auto TRX_SYS_FILE_FORMAT_TAG = UNIV_PAGE_SIZE - 16;

/** Contents of TRX_SYS_FILE_FORMAT_TAG when valid.  The file format
identifier is added to this constant. */
constexpr ulint TRX_SYS_FILE_FORMAT_TAG_MAGIC_N_LOW = 3645922177UL;

/** Contents of TRX_SYS_FILE_FORMAT_TAG+4 when valid */
constexpr ulint TRX_SYS_FILE_FORMAT_TAG_MAGIC_N_HIGH = 2745987765UL;

/* @} */

/** When a trx id which is zero modulo this number (which must be a power of
two) is assigned, the field TRX_SYS_TRX_ID_STORE on the transaction system
page is updated */
constexpr ulint TRX_SYS_TRX_ID_WRITE_MARGIN = 256;

/** The transaction system */
extern trx_sys_t *trx_sys;

/** The following is set to true when we are upgrading from pre-4.1
format data files to the multiple tablespaces format data files */
extern bool trx_doublewrite_must_reset_space_ids;

/** Doublewrite system */
extern trx_doublewrite_t *trx_doublewrite;

/** Doublewrite system */
extern trx_doublewrite_t *trx_doublewrite;
/** The following is true when we are using the database in the
post-4.1 format, i.e., we have successfully upgraded, or have created
a new database installation */
extern bool trx_sys_multiple_tablespace_format;

/** Creates the doublewrite buffer to a new InnoDB installation. The header of
the doublewrite buffer is placed on the trx system header page. */

db_err trx_sys_create_doublewrite_buf();

/** At a database startup initializes the doublewrite buffer memory structure if
we already have a doublewrite buffer created in the data files. If we are
upgrading to an InnoDB version which supports multiple tablespaces, then this
function performs the necessary update operations. If we are in a crash
recovery, this function uses a possible doublewrite buffer to restore
half-written pages in the data files. */
void trx_sys_doublewrite_init_or_restore_pages(bool restore_corrupt_pages); /** in: true=restore pages */

/** Marks the trx sys header when we have successfully upgraded to the >= 4.1.x
multiple tablespace format. */
void trx_sys_mark_upgraded_to_multiple_tablespaces();

/** Determines if a page number is located inside the doublewrite buffer.
@return true if the location is inside the two blocks of the
doublewrite buffer */
bool trx_doublewrite_page_inside(ulint page_no); /** in: page number */

/** Checks if a page address is the trx sys header page.
@return	true if trx sys header page */
inline bool trx_sys_hdr_page(
  ulint space, /** in: space */
  ulint page_no
); /** in: page number */

/** Creates and initializes the central memory structures for the transaction
system. This is called when the database is started. */
void trx_sys_init_at_db_start(ib_recovery_t recovery); /** in: recovery flag */

/** Creates and initializes the transaction system at the database creation. */
void trx_sys_create(ib_recovery_t recovery); /** in: recovery flag */

/** Looks for a free slot for a rollback segment in the trx system file copy.
@return	slot index or ULINT_UNDEFINED if not found */
ulint trx_sysf_rseg_find_free(mtr_t *mtr); /** in: mtr */

/** Gets the pointer in the nth slot of the rseg array.
@return	pointer to rseg object, NULL if slot not in use */
inline trx_rseg_t *trx_sys_get_nth_rseg(
  trx_sys_t *sys, /** in: trx system */
  ulint n
); /** in: index of slot */

/** Sets the pointer in the nth slot of the rseg array. */
inline void trx_sys_set_nth_rseg(
  trx_sys_t *sys, /** in: trx system */
  ulint n,        /** in: index of slot */
  trx_rseg_t *rseg
); /** in: pointer to rseg object,
                                        NULL if slot not in use */

/** Gets a pointer to the transaction system file copy and x-locks its page.
@return	pointer to system file copy, page x-locked */
inline trx_sysf_t *trx_sysf_get(mtr_t *mtr); /** in: mtr */

/** Gets the space of the nth rollback segment slot in the trx system
file copy.
@return	space id */
inline ulint trx_sysf_rseg_get_space(
  trx_sysf_t *sys_header, /** in: trx sys file copy */
  ulint i,                /** in: slot index == rseg id */
  mtr_t *mtr
); /** in: mtr */

/** Gets the page number of the nth rollback segment slot in the trx system
file copy.
@return	page number, FIL_NULL if slot unused */
inline ulint trx_sysf_rseg_get_page_no(
  trx_sysf_t *sys_header, /** in: trx sys file copy */
  ulint i,                /** in: slot index == rseg id */
  mtr_t *mtr
); /** in: mtr */

/** Sets the space id of the nth rollback segment slot in the trx system
file copy. */
inline void trx_sysf_rseg_set_space(
  trx_sysf_t *sys_header, /** in: trx sys file copy */
  ulint i,                /** in: slot index == rseg id */
  ulint space,            /** in: space id */
  mtr_t *mtr
); /** in: mtr */

/** Sets the page number of the nth rollback segment slot in the trx system
file copy. */
inline void trx_sysf_rseg_set_page_no(
  trx_sysf_t *sys_header, /** in: trx sys file copy */
  ulint i,                /** in: slot index == rseg id */
  ulint page_no,          /** in: page number, FIL_NULL if
                                         the slot is reset to unused */
  mtr_t *mtr
); /** in: mtr */

/** Allocates a new transaction id.
@return	new, allocated trx id */
inline trx_id_t trx_sys_get_new_trx_id(void);

/** Allocates a new transaction number.
@return	new, allocated trx number */
inline trx_id_t trx_sys_get_new_trx_no(void);

/** Writes a trx id to an index page. In case that the id size changes in
some future version, this function should be used instead of
mach_write_... */
inline void trx_write_trx_id(
  byte *ptr, /** in: pointer to memory where written */
  trx_id_t id
); /** in: id */

/** Reads a trx id from an index page. In case that the id size changes in
some future version, this function should be used instead of
mach_read_...
@return	id */
inline trx_id_t trx_read_trx_id(const byte *ptr); /** in: pointer to memory from where to read */

/** Looks for the trx handle with the given id in trx_list.
@return	the trx handle or NULL if not found */
inline trx_t *trx_get_on_id(trx_id_t trx_id); /** in: trx id to search for */

/** Returns the minumum trx id in trx list. This is the smallest id for which
the trx can possibly be active. (But, you must look at the trx->conc_state to
find out if the minimum trx id transaction itself is active, or already
committed.)
@return	the minimum trx id, or trx_sys->max_trx_id if the trx list is empty */
inline trx_id_t trx_list_get_min_trx_id(void);

/** Checks if a transaction with the given id is active.
@return	true if active */
inline bool trx_is_active(trx_id_t trx_id); /** in: trx id of the transaction */

/** Checks that trx is in the trx list.
@return	true if is in */
bool trx_in_trx_list(trx_t *in_trx); /** in: trx */
/** Initializes the tablespace tag system. */

void trx_sys_file_format_init(void);

/** Closes the tablespace tag system. */
void trx_sys_file_format_close(void);

/** Shutdown/Close the transaction system. */
void trx_sys_close(void);

/** Tags the system table space with minimum format id if it has not been
tagged yet.
WARNING: This function is only called during the startup and AFTER the
redo log application during recovery has finished. */
void trx_sys_file_format_tag_init(void);

/** Shutdown/Close the transaction system. */
void trx_sys_close(void);

/** Get the name representation of the file format from its id.
@return	pointer to the name */
const char *trx_sys_file_format_id_to_name(const ulint id); /** in: id of the file format */

/** Validate the file format name and return its corresponding id.
@return	valid file format id or DICT_TF_FORMAT_MAX + 1 */
ulint trx_sys_file_format_name_to_id(const char *format_name); /** in: pointer to file format name */

/** Set the file format id unconditionally except if it's already the
same value.
@return	true if value updated */
bool trx_sys_file_format_max_set(
  ulint format_id, /** in: file format id */
  const char **name
); /** out: max file format name or
                        NULL if not needed. */

/** Get the name representation of the file format from its id.
@return	pointer to the max format name */
const char *trx_sys_file_format_max_get(void);

/** Check for the max file format tag stored on disk.
@return	DB_SUCCESS or error code */
db_err trx_sys_file_format_max_check(ulint max_format_id); /** in: the max format id to check */

/** Update the file format tag in the system tablespace only if the given
format id is greater than the known max id.
@return	true if format_id was bigger than the known max id */

bool trx_sys_file_format_max_upgrade(
  const char **name, /** out: max file format name */
  ulint format_id
); /** in: file format identifier */

/** Reset the variables. */
void trx_sys_var_init(void);

/** Reads the file format id from the first system table space file.
Even if the call succeeds and returns true, the returned format id
may be ULINT_UNDEFINED signalling that the format id was not present
in the data file.
@return true if call succeeds */
bool trx_sys_read_file_format_id(
  const char *pathname, /** in: pathname of the first system
                          table space file */
  ulint *format_id
); /** out: file format of the system table
                          space */

/** Reads the file format id from the given per-table data file.
@return true if call succeeds */
bool trx_sys_read_pertable_file_format_id(
  const char *pathname, /** in: pathname of a per-table
                          datafile */
  ulint *format_id
); /** out: file format of the per-table
                          data file */

/** Doublewrite control struct */
struct trx_doublewrite_t {
  mutex_t mutex;    /** mutex protecting the first_free field and
                    write_buf */
  ulint block1;     /** the page number of the first
                    doublewrite block (64 pages) */
  ulint block2;     /** page number of the second block */
  ulint first_free; /** first free position in write_buf measured
                    in units of UNIV_PAGE_SIZE */
  byte *write_buf;  /** write buffer used in writing to the
                    doublewrite buffer, aligned to an
                    address divisible by UNIV_PAGE_SIZE
                    (which is required by Windows aio) */
  byte *write_buf_unaligned;
  /** pointer to write_buf, but unaligned */
  buf_page_t **buf_block_arr; /** array to store pointers to the buffer
                              blocks which have been cached to write_buf */
};

/** The transaction system central memory data structure; protected by the
kernel mutex */
struct trx_sys_t {
  /** The smallest number not yet assigned as a transaction
  id or transaction number */
  trx_id_t max_trx_id;

  /** List of active and committed in memory transactions,
  sorted on trx id, biggest first */
  UT_LIST_BASE_NODE_T_EXTERN(trx_t, trx_list) trx_list;

  /** List of transactions created for users */
  UT_LIST_BASE_NODE_T_EXTERN(trx_t, client_trx_list) client_trx_list;

  /** List of rollback segment objects */
  UT_LIST_BASE_NODE_T_EXTERN(trx_rseg_t, rseg_list) rseg_list;

  /** Latest rollback segment in the round-robin assignment
  of rollback segments to transactions */
  trx_rseg_t *latest_rseg;

  /** Pointer array to rollback segments; NULL if slot not in use */
  trx_rseg_t *rseg_array[TRX_SYS_N_RSEGS];

  /** Length of the TRX_RSEG_HISTORY list (update undo logs for
  committed transactions), protected by rseg->mutex */
  ulint rseg_history_len;

  /** List of read views sorted on trx no, biggest first */
  UT_LIST_BASE_NODE_T_EXTERN(read_view_t, view_list) view_list;
};

/* The typedef for rseg slot in the file copy */
typedef byte trx_sysf_rseg_t;

/* Rollback segment specification slot offsets */
/*-------------------------------------------------------------*/

/** Tablespace where the segment header is placed. */
constexpr auto TRX_SYS_RSEG_SPACE = SYS_TABLESPACE;

/** Page number where the segment header is placed; this is FIL_NULL if the slot is unused */
constexpr page_no_t TRX_SYS_RSEG_PAGE_NO = 4;

/*-------------------------------------------------------------*/
/* Size of a rollback segment specification slot */
constexpr ulint TRX_SYS_RSEG_SLOT_SIZE = 8;

/** Writes the value of max_trx_id to the file based trx system header. */
void trx_sys_flush_max_trx_id(void);

/** Checks if a page address is the trx sys header page.
@return	true if trx sys header page */
inline bool trx_sys_hdr_page(
  ulint space, /*!< in: space */
  ulint page_no
) /*!< in: page number */
{
  return space == TRX_SYS_SPACE && page_no == TRX_SYS_PAGE_NO;
}

/** Gets the pointer in the nth slot of the rseg array.
@return	pointer to rseg object, NULL if slot not in use */
inline trx_rseg_t *trx_sys_get_nth_rseg(
  trx_sys_t *sys, /*!< in: trx system */
  ulint n
) /*!< in: index of slot */
{
  ut_ad(mutex_own(&(kernel_mutex)));
  ut_ad(n < TRX_SYS_N_RSEGS);

  return (sys->rseg_array[n]);
}

/** Sets the pointer in the nth slot of the rseg array. */
inline void trx_sys_set_nth_rseg(
  trx_sys_t *sys, /*!< in: trx system */
  ulint n,        /*!< in: index of slot */
  trx_rseg_t *rseg
) /*!< in: pointer to rseg object,
                                       NULL if slot not in use */
{
  ut_ad(n < TRX_SYS_N_RSEGS);

  sys->rseg_array[n] = rseg;
}

/** Gets a pointer to the transaction system header and x-latches its page.
@return	pointer to system header, page x-latched. */
inline trx_sysf_t *trx_sysf_get(mtr_t *mtr) /*!< in: mtr */
{
  ut_ad(mtr != nullptr);

  Buf_pool::Request req {
    .m_rw_latch = RW_X_LATCH,
    .m_page_id = { TRX_SYS_SPACE, TRX_SYS_PAGE_NO },
    .m_mode = BUF_GET,
    .m_file = __FILE__,
    .m_line = __LINE__,
    .m_mtr = mtr
  };

  auto block = srv_buf_pool->get(req, nullptr);
  buf_block_dbg_add_level(IF_SYNC_DEBUG(block, SYNC_TRX_SYS_HEADER));

  auto header = TRX_SYS + block->get_frame();

  return header;
}

/** Gets the space of the nth rollback segment slot in the trx system
file copy.
@return	space id */
inline ulint trx_sysf_rseg_get_space(
  trx_sysf_t *sys_header, /*!< in: trx sys header */
  ulint i,                /*!< in: slot index == rseg id */
  mtr_t *mtr
) /*!< in: mtr */
{
  ut_ad(mutex_own(&(kernel_mutex)));
  ut_ad(sys_header);
  ut_ad(i < TRX_SYS_N_RSEGS);

  return (mtr_read_ulint(sys_header + TRX_SYS_RSEGS + i * TRX_SYS_RSEG_SLOT_SIZE + TRX_SYS_RSEG_SPACE, MLOG_4BYTES, mtr));
}

/** Gets the page number of the nth rollback segment slot in the trx system
header.
@return	page number, FIL_NULL if slot unused */
inline ulint trx_sysf_rseg_get_page_no(
  trx_sysf_t *sys_header, /*!< in: trx system header */
  ulint i,                /*!< in: slot index == rseg id */
  mtr_t *mtr
) /*!< in: mtr */
{
  ut_ad(sys_header);
  ut_ad(mutex_own(&(kernel_mutex)));
  ut_ad(i < TRX_SYS_N_RSEGS);

  return (mtr_read_ulint(sys_header + TRX_SYS_RSEGS + i * TRX_SYS_RSEG_SLOT_SIZE + TRX_SYS_RSEG_PAGE_NO, MLOG_4BYTES, mtr));
}

/** Sets the space id of the nth rollback segment slot in the trx system
file copy. */
inline void trx_sysf_rseg_set_space(
  trx_sysf_t *sys_header, /*!< in: trx sys file copy */
  ulint i,                /*!< in: slot index == rseg id */
  ulint space,            /*!< in: space id */
  mtr_t *mtr
) /*!< in: mtr */
{
  ut_ad(mutex_own(&(kernel_mutex)));
  ut_ad(sys_header);
  ut_ad(i < TRX_SYS_N_RSEGS);

  mlog_write_ulint(sys_header + TRX_SYS_RSEGS + i * TRX_SYS_RSEG_SLOT_SIZE + TRX_SYS_RSEG_SPACE, space, MLOG_4BYTES, mtr);
}

/** Sets the page number of the nth rollback segment slot in the trx system
header. */
inline void trx_sysf_rseg_set_page_no(
  trx_sysf_t *sys_header, /*!< in: trx sys header */
  ulint i,                /*!< in: slot index == rseg id */
  ulint page_no,          /*!< in: page number, FIL_NULL if the
                                         slot is reset to unused */
  mtr_t *mtr
) /*!< in: mtr */
{
  ut_ad(mutex_own(&(kernel_mutex)));
  ut_ad(sys_header);
  ut_ad(i < TRX_SYS_N_RSEGS);

  mlog_write_ulint(sys_header + TRX_SYS_RSEGS + i * TRX_SYS_RSEG_SLOT_SIZE + TRX_SYS_RSEG_PAGE_NO, page_no, MLOG_4BYTES, mtr);
}

/** Writes a trx id to an index page. In case that the id size changes in
some future version, this function should be used instead of
mach_write_... */
inline void trx_write_trx_id(
  byte *ptr, /*!< in: pointer to memory where written */
  trx_id_t id
) /*!< in: id */
{
  static_assert(DATA_TRX_ID_LEN == 6, "error DATA_TRX_ID_LEN != 6");

  mach_write_to_6(ptr, id);
}

/** Reads a trx id from an index page. In case that the id size changes in
some future version, this function should be used instead of
mach_read_...
@return	id */
inline trx_id_t trx_read_trx_id(const byte *ptr) /*!< in: pointer to memory from where to read */
{
  static_assert(DATA_TRX_ID_LEN == 6, "error DATA_TRX_ID_LEN != 6");

  return (mach_read_from_6(ptr));
}

/** Looks for the trx handle with the given id in trx_list.
@return	the trx handle or NULL if not found */
inline trx_t *trx_get_on_id(trx_id_t trx_id) /*!< in: trx id to search for */
{
  trx_t *trx;

  ut_ad(mutex_own(&(kernel_mutex)));

  trx = UT_LIST_GET_FIRST(trx_sys->trx_list);

  while (trx != nullptr) {
    if (trx_id == trx->m_id) {

      return (trx);
    }

    trx = UT_LIST_GET_NEXT(trx_list, trx);
  }

  return (nullptr);
}

/** Returns the minumum trx id in trx list. This is the smallest id for which
the trx can possibly be active. (But, you must look at the trx->conc_state to
find out if the minimum trx id transaction itself is active, or already
committed.)
@return	the minimum trx id, or trx_sys->max_trx_id if the trx list is empty */
inline trx_id_t trx_list_get_min_trx_id(void) {
  trx_t *trx;

  ut_ad(mutex_own(&(kernel_mutex)));

  trx = UT_LIST_GET_LAST(trx_sys->trx_list);

  if (trx == nullptr) {

    return (trx_sys->max_trx_id);
  }

  return (trx->m_id);
}

/** Checks if a transaction with the given id is active.
@return	true if active */
inline bool trx_is_active(trx_id_t trx_id) /*!< in: trx id of the transaction */
{
  trx_t *trx;

  ut_ad(mutex_own(&(kernel_mutex)));

  if (trx_id < trx_list_get_min_trx_id()) {

    return (false);
  }

  if (trx_id >= trx_sys->max_trx_id) {

    /* There must be corruption: we return true because this
    function is only called by lock_clust_rec_some_has_impl()
    and row_vers_impl_x_locked_off_kernel() and they have
    diagnostic prints in this case */

    return (true);
  }

  trx = trx_get_on_id(trx_id);
  if (trx && (trx->m_conc_state == TRX_ACTIVE || trx->m_conc_state == TRX_PREPARED)) {

    return (true);
  }

  return (false);
}

/** Allocates a new transaction id.
@return	new, allocated trx id */
inline trx_id_t trx_sys_get_new_trx_id(void) {
  trx_id_t id;

  ut_ad(mutex_own(&kernel_mutex));

  /* VERY important: after the database is started, max_trx_id value is
  divisible by TRX_SYS_TRX_ID_WRITE_MARGIN, and the following if
  will evaluate to true when this function is first time called,
  and the value for trx id will be written to disk-based header!
  Thus trx id values will not overlap when the database is
  repeatedly started! */

  if (trx_sys->max_trx_id % TRX_SYS_TRX_ID_WRITE_MARGIN == 0) {

    trx_sys_flush_max_trx_id();
  }

  id = trx_sys->max_trx_id;

  ++trx_sys->max_trx_id;

  return id;
}

/** Allocates a new transaction number.
@return	new, allocated trx number */
inline trx_id_t trx_sys_get_new_trx_no(void) {
  ut_ad(mutex_own(&kernel_mutex));

  return (trx_sys_get_new_trx_id());
}
