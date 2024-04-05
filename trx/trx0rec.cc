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

/** @file trx/trx0rec.c
Transaction undo log record

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0rec.h"

#ifdef UNIV_NONINL
#include "trx0rec.ic"
#endif

#include "dict0dict.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "que0que.h"
#include "row0ext.h"
#include "row0row.h"
#include "row0upd.h"
#include "trx0purge.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "ut0mem.h"

/** Writes the mtr log entry of the inserted undo log record on the undo log
page. */
inline void trx_undof_page_add_undo_rec_log(
  page_t *undo_page, /*!< in: undo log page */
  ulint old_free,    /*!< in: start offset of the inserted entry */
  ulint new_free,    /*!< in: end offset of the entry */
  mtr_t *mtr
) /*!< in: mtr */
{
  byte *log_ptr;
  const byte *log_end;
  ulint len;

  log_ptr = mlog_open(mtr, 11 + 13 + MLOG_BUF_MARGIN);

  if (log_ptr == nullptr) {

    return;
  }

  log_end = &log_ptr[11 + 13 + MLOG_BUF_MARGIN];
  log_ptr = mlog_write_initial_log_record_fast(undo_page, MLOG_UNDO_INSERT, log_ptr, mtr);
  len = new_free - old_free - 4;

  mach_write_to_2(log_ptr, len);
  log_ptr += 2;

  if (log_ptr + len <= log_end) {
    memcpy(log_ptr, undo_page + old_free + 2, len);
    mlog_close(mtr, log_ptr + len);
  } else {
    mlog_close(mtr, log_ptr);
    mlog_catenate_string(mtr, undo_page + old_free + 2, len);
  }
}

byte *trx_undo_parse_add_undo_rec(byte *ptr, byte *end_ptr, page_t *page) {
  ulint len;
  byte *rec;
  ulint first_free;

  if (end_ptr < ptr + 2) {

    return nullptr;
  }

  len = mach_read_from_2(ptr);
  ptr += 2;

  if (end_ptr < ptr + len) {

    return nullptr;
  }

  if (page == nullptr) {

    return ptr + len;
  }

  first_free = mach_read_from_2(page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
  rec = page + first_free;

  mach_write_to_2(rec, first_free + 4 + len);
  mach_write_to_2(rec + 2 + len, first_free);

  mach_write_to_2(page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE, first_free + 4 + len);
  memcpy(rec + 2, ptr, len);

  return ptr + len;
}

/** Calculates the free space left for extending an undo log record.
@return	bytes left */
inline ulint trx_undo_left(
  const page_t *page, /*!< in: undo log page */
  const byte *ptr
) /*!< in: pointer to page */
{
  /* The '- 10' is a safety margin, in case we have some small
  calculation error below */

  return UNIV_PAGE_SIZE - (ptr - page) - 10 - FIL_PAGE_DATA_END;
}

/** Set the next and previous pointers in the undo page for the undo record
that was written to ptr. Update the first free value by the number of bytes
written for this undo record.
@return	offset of the inserted entry on the page if succeeded, 0 if fail */
static ulint trx_undo_page_set_next_prev_and_add(
  page_t *undo_page, /*!< in/out: undo log page */
  byte *ptr,         /*!< in: ptr up to where data has been
                       written on this undo page. */
  mtr_t *mtr
) /*!< in: mtr */
{
  ulint first_free; /*!< offset within undo_page */
  ulint end_of_rec; /*!< offset within undo_page */
  byte *ptr_to_first_free;
  /* pointer within undo_page
  that points to the next free
  offset value within undo_page.*/

  ut_ad(ptr > undo_page);
  ut_ad(ptr < undo_page + UNIV_PAGE_SIZE);

  if (unlikely(trx_undo_left(undo_page, ptr) < 2)) {

    return 0;
  }

  ptr_to_first_free = undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE;

  first_free = mach_read_from_2(ptr_to_first_free);

  /* Write offset of the previous undo log record */
  mach_write_to_2(ptr, first_free);
  ptr += 2;

  end_of_rec = ptr - undo_page;

  /* Write offset of the next undo log record */
  mach_write_to_2(undo_page + first_free, end_of_rec);

  /* Update the offset to first free undo record */
  mach_write_to_2(ptr_to_first_free, end_of_rec);

  /* Write this log entry to the UNDO log */
  trx_undof_page_add_undo_rec_log(undo_page, first_free, end_of_rec, mtr);

  return first_free;
}

/** Reports in the undo log of an insert of a clustered index record.
@return	offset of the inserted entry on the page if succeed, 0 if fail */
static ulint trx_undo_page_report_insert(
  page_t *undo_page,           /*!< in: undo log page */
  trx_t *trx,                  /*!< in: transaction */
  dict_index_t *index,         /*!< in: clustered index */
  const dtuple_t *clust_entry, /*!< in: index entry which will be
                                 inserted to the clustered index */
  mtr_t *mtr
) /*!< in: mtr */
{
  ulint first_free;
  byte *ptr;
  ulint i;

  ut_ad(dict_index_is_clust(index));
  ut_ad(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_INSERT);

  first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
  ptr = undo_page + first_free;

  ut_ad(first_free <= UNIV_PAGE_SIZE);

  if (trx_undo_left(undo_page, ptr) < 2 + 1 + 11 + 11) {

    /* Not enough space for writing the general parameters */

    return 0;
  }

  /* Reserve 2 bytes for the pointer to the next undo log record */
  ptr += 2;

  /* Store first some general parameters to the undo log */
  *ptr = TRX_UNDO_INSERT_REC;
  ++ptr;

  ptr += mach_uint64_write_much_compressed(ptr, trx->undo_no);
  ptr += mach_uint64_write_much_compressed(ptr, index->table->id);
  /*----------------------------------------*/
  /* Store then the fields required to uniquely determine the record
  to be inserted in the clustered index */

  for (i = 0; i < dict_index_get_n_unique(index); i++) {

    const dfield_t *field = dtuple_get_nth_field(clust_entry, i);
    ulint flen = dfield_get_len(field);

    if (trx_undo_left(undo_page, ptr) < 5) {

      return 0;
    }

    ptr += mach_write_compressed(ptr, flen);

    if (flen != UNIV_SQL_NULL) {
      if (trx_undo_left(undo_page, ptr) < flen) {

        return 0;
      }

      memcpy(ptr, dfield_get_data(field), flen);
      ptr += flen;
    }
  }

  return trx_undo_page_set_next_prev_and_add(undo_page, ptr, mtr);
}

byte *trx_undo_rec_get_pars(
  trx_undo_rec_t *undo_rec, ulint *type, ulint *cmpl_info, bool *updated_extern, undo_no_t *undo_no, uint64_t *table_id
) {
  byte *ptr;
  ulint type_cmpl;

  ptr = undo_rec + 2;

  type_cmpl = mach_read_from_1(ptr);
  ptr++;

  if (type_cmpl & TRX_UNDO_UPD_EXTERN) {
    *updated_extern = true;
    type_cmpl -= TRX_UNDO_UPD_EXTERN;
  } else {
    *updated_extern = false;
  }

  *type = type_cmpl & (TRX_UNDO_CMPL_INFO_MULT - 1);
  *cmpl_info = type_cmpl / TRX_UNDO_CMPL_INFO_MULT;

  *undo_no = mach_uint64_read_much_compressed(ptr);
  ptr += mach_uint64_get_much_compressed_size(*undo_no);

  *table_id = mach_uint64_read_much_compressed(ptr);
  ptr += mach_uint64_get_much_compressed_size(*table_id);

  return ptr;
}

/** Reads from an undo log record a stored column value.
@return	remaining part of undo log record after reading these values */
static byte *trx_undo_rec_get_col_val(
  byte *ptr,    /*!< in: pointer to remaining part of undo log record */
  byte **field, /*!< out: pointer to stored field */
  ulint *len,   /*!< out: length of the field, or UNIV_SQL_NULL */
  ulint *orig_len
) /*!< out: original length of the locally
                    stored part of an externally stored column, or 0 */
{
  *len = mach_read_compressed(ptr);
  ptr += mach_get_compressed_size(*len);

  *orig_len = 0;

  switch (*len) {
    case UNIV_SQL_NULL:
      *field = nullptr;
      break;
    case UNIV_EXTERN_STORAGE_FIELD:
      *orig_len = mach_read_compressed(ptr);
      ptr += mach_get_compressed_size(*orig_len);
      *len = mach_read_compressed(ptr);
      ptr += mach_get_compressed_size(*len);
      *field = ptr;
      ptr += *len;

      ut_ad(*orig_len >= BTR_EXTERN_FIELD_REF_SIZE);
      ut_ad(*len > *orig_len);
      /* @see dtuple_convert_big_rec() */
      ut_ad(*len >= BTR_EXTERN_FIELD_REF_SIZE * 2);
      /* we do not have access to index->table here
    ut_ad(dict_table_get_format(index->table) >= DICT_TF_FORMAT_V1
          || *len >= REC_MAX_INDEX_COL_LEN
          + BTR_EXTERN_FIELD_REF_SIZE);
    */

      *len += UNIV_EXTERN_STORAGE_FIELD;
      break;
    default:
      *field = ptr;
      if (*len >= UNIV_EXTERN_STORAGE_FIELD) {
        ptr += *len - UNIV_EXTERN_STORAGE_FIELD;
      } else {
        ptr += *len;
      }
  }

  return ptr;
}

byte *trx_undo_rec_get_row_ref(byte *ptr, dict_index_t *index, dtuple_t **ref, mem_heap_t *heap) {
  ulint ref_len;
  ulint i;

  ut_ad(index && ptr && ref && heap);
  ut_a(dict_index_is_clust(index));

  ref_len = dict_index_get_n_unique(index);

  *ref = dtuple_create(heap, ref_len);

  dict_index_copy_types(*ref, index, ref_len);

  for (i = 0; i < ref_len; i++) {
    dfield_t *dfield;
    byte *field;
    ulint len;
    ulint orig_len;

    dfield = dtuple_get_nth_field(*ref, i);

    ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

    dfield_set_data(dfield, field, len);
  }

  return ptr;
}

byte *trx_undo_rec_skip_row_ref(byte *ptr, dict_index_t *index) {
  ulint ref_len;
  ulint i;

  ut_ad(index && ptr);
  ut_a(dict_index_is_clust(index));

  ref_len = dict_index_get_n_unique(index);

  for (i = 0; i < ref_len; i++) {
    byte *field;
    ulint len;
    ulint orig_len;

    ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);
  }

  return ptr;
}

/** Fetch a prefix of an externally stored column, for writing to the undo log
of an update or delete marking of a clustered index record.
@return	ext_buf */
static byte *trx_undo_page_fetch_ext(
  byte *ext_buf,     /*!< in: a buffer of
                       REC_MAX_INDEX_COL_LEN
                       + BTR_EXTERN_FIELD_REF_SIZE */
  const byte *field, /*!< in: an externally stored column */
  ulint *len
) /*!< in: length of field;
                       out: used length of ext_buf */
{
  /* Fetch the BLOB. */
  ulint ext_len = btr_copy_externally_stored_field_prefix(ext_buf, REC_MAX_INDEX_COL_LEN, field, *len);
  /* BLOBs should always be nonempty. */
  ut_a(ext_len);
  /* Append the BLOB pointer to the prefix. */
  memcpy(ext_buf + ext_len, field + *len - BTR_EXTERN_FIELD_REF_SIZE, BTR_EXTERN_FIELD_REF_SIZE);
  *len = ext_len + BTR_EXTERN_FIELD_REF_SIZE;
  return ext_buf;
}

/** Writes to the undo log a prefix of an externally stored column.
@return	undo log position */
static byte *trx_undo_page_report_modify_ext(
  byte *ptr,          /*!< in: undo log position,
                        at least 15 bytes must be available */
  byte *ext_buf,      /*!< in: a buffer of
                        REC_MAX_INDEX_COL_LEN
                        + BTR_EXTERN_FIELD_REF_SIZE,
                        or nullptr when should not fetch
                        a longer prefix */
  const byte **field, /*!< in/out: the locally stored part of
                        the externally stored column */
  ulint *len
) /*!< in/out: length of field, in bytes */
{
  if (ext_buf) {
    /* If an ordering column is externally stored, we will
    have to store a longer prefix of the field.  In this
    case, write to the log a marker followed by the
    original length and the real length of the field. */
    ptr += mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD);

    ptr += mach_write_compressed(ptr, *len);

    *field = trx_undo_page_fetch_ext(ext_buf, *field, len);

    ptr += mach_write_compressed(ptr, *len);
  } else {
    ptr += mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD + *len);
  }

  return ptr;
}

/** Reports in the undo log of an update or delete marking of a clustered index
record.
@return byte offset of the inserted undo log entry on the page if
succeed, 0 if fail */
static ulint trx_undo_page_report_modify(
  page_t *undo_page,    /*!< in: undo log page */
  trx_t *trx,           /*!< in: transaction */
  dict_index_t *index,  /*!< in: clustered index where update or
                          delete marking is done */
  const rec_t *rec,     /*!< in: clustered index record which
                          has NOT yet been modified */
  const ulint *offsets, /*!< in: rec_get_offsets(rec, index) */
  const upd_t *update,  /*!< in: update vector which tells the
                          columns to be updated; in the case of
                          a delete, this should be set to nullptr */
  ulint cmpl_info,      /*!< in: compiler info on secondary
                          index updates */
  mtr_t *mtr
) /*!< in: mtr */
{
  dict_table_t *table;
  ulint first_free;
  byte *ptr;
  const byte *field;
  ulint flen;
  ulint col_no;
  ulint type_cmpl;
  byte *type_cmpl_ptr;
  ulint i;
  trx_id_t trx_id;
  bool ignore_prefix = false;
  byte ext_buf[REC_MAX_INDEX_COL_LEN + BTR_EXTERN_FIELD_REF_SIZE];

  ut_a(dict_index_is_clust(index));
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_UPDATE);
  table = index->table;

  first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
  ptr = undo_page + first_free;

  ut_ad(first_free <= UNIV_PAGE_SIZE);

  if (trx_undo_left(undo_page, ptr) < 50) {

    /* NOTE: the value 50 must be big enough so that the general
    fields written below fit on the undo log page */

    return 0;
  }

  /* Reserve 2 bytes for the pointer to the next undo log record */
  ptr += 2;

  /* Store first some general parameters to the undo log */

  if (!update) {
    type_cmpl = TRX_UNDO_DEL_MARK_REC;
  } else if (rec_get_deleted_flag(rec, dict_table_is_comp(table))) {
    type_cmpl = TRX_UNDO_UPD_DEL_REC;
    /* We are about to update a delete marked record.
    We don't typically need the prefix in this case unless
    the delete marking is done by the same transaction
    (which we check below). */
    ignore_prefix = true;
  } else {
    type_cmpl = TRX_UNDO_UPD_EXIST_REC;
  }

  type_cmpl |= cmpl_info * TRX_UNDO_CMPL_INFO_MULT;
  type_cmpl_ptr = ptr;

  *ptr++ = (byte)type_cmpl;
  ptr += mach_uint64_write_much_compressed(ptr, trx->undo_no);

  ptr += mach_uint64_write_much_compressed(ptr, table->id);

  /*----------------------------------------*/
  /* Store the state of the info bits */

  *ptr++ = (byte)rec_get_info_bits(rec, dict_table_is_comp(table));

  /* Store the values of the system columns */
  field = rec_get_nth_field(rec, offsets, dict_index_get_sys_col_pos(index, DATA_TRX_ID), &flen);
  ut_ad(flen == DATA_TRX_ID_LEN);

  trx_id = trx_read_trx_id(field);

  /* If it is an update of a delete marked record, then we are
  allowed to ignore blob prefixes if the delete marking was done
  by some other trx as it must have committed by now for us to
  allow an over-write. */
  if (ignore_prefix) {
    ignore_prefix = trx_id != trx->m_id;
  }

  ptr += mach_uint64_write_compressed(ptr, trx_id);

  field = rec_get_nth_field(rec, offsets, dict_index_get_sys_col_pos(index, DATA_ROLL_PTR), &flen);
  ut_ad(flen == DATA_ROLL_PTR_LEN);

  const auto roll_ptr = trx_read_roll_ptr(field);

  ptr += mach_uint64_write_compressed(ptr, roll_ptr);

  /*----------------------------------------*/
  /* Store then the fields required to uniquely determine the
  record which will be modified in the clustered index */

  for (i = 0; i < dict_index_get_n_unique(index); i++) {

    field = rec_get_nth_field(rec, offsets, i, &flen);

    /* The ordering columns must not be stored externally. */
    ut_ad(!rec_offs_nth_extern(offsets, i));
    ut_ad(dict_index_get_nth_col(index, i)->ord_part);

    if (trx_undo_left(undo_page, ptr) < 5) {

      return 0;
    }

    ptr += mach_write_compressed(ptr, flen);

    if (flen != UNIV_SQL_NULL) {
      if (trx_undo_left(undo_page, ptr) < flen) {

        return 0;
      }

      memcpy(ptr, field, flen);
      ptr += flen;
    }
  }

  /*----------------------------------------*/
  /* Save to the undo log the old values of the columns to be updated. */

  if (update) {
    if (trx_undo_left(undo_page, ptr) < 5) {

      return 0;
    }

    ptr += mach_write_compressed(ptr, upd_get_n_fields(update));

    for (i = 0; i < upd_get_n_fields(update); i++) {

      ulint pos = upd_get_nth_field(update, i)->field_no;

      /* Write field number to undo log */
      if (trx_undo_left(undo_page, ptr) < 5) {

        return 0;
      }

      ptr += mach_write_compressed(ptr, pos);

      /* Save the old value of field */
      field = rec_get_nth_field(rec, offsets, pos, &flen);

      if (trx_undo_left(undo_page, ptr) < 15) {

        return 0;
      }

      if (rec_offs_nth_extern(offsets, pos)) {
        ptr = trx_undo_page_report_modify_ext(
          ptr,
          dict_index_get_nth_col(index, pos)->ord_part && !ignore_prefix && flen < REC_MAX_INDEX_COL_LEN ? ext_buf : nullptr,
          &field,
          &flen
        );

        /* Notify purge that it eventually has to
        free the old externally stored field */

        trx->update_undo->del_marks = true;

        *type_cmpl_ptr |= TRX_UNDO_UPD_EXTERN;
      } else {
        ptr += mach_write_compressed(ptr, flen);
      }

      if (flen != UNIV_SQL_NULL) {
        if (trx_undo_left(undo_page, ptr) < flen) {

          return 0;
        }

        memcpy(ptr, field, flen);
        ptr += flen;
      }
    }
  }

  /*----------------------------------------*/
  /* In the case of a delete marking, and also in the case of an update
  where any ordering field of any index changes, store the values of all
  columns which occur as ordering fields in any index. This info is used
  in the purge of old versions where we use it to build and search the
  delete marked index records, to look if we can remove them from the
  index tree. Note that starting from 4.0.14 also externally stored
  fields can be ordering in some index. Starting from 5.2, we no longer
  store REC_MAX_INDEX_COL_LEN first bytes to the undo log record,
  but we can construct the column prefix fields in the index by
  fetching the first page of the BLOB that is pointed to by the
  clustered index. This works also in crash recovery, because all pages
  (including BLOBs) are recovered before anything is rolled back. */

  if (!update || !(cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
    byte *old_ptr = ptr;

    trx->update_undo->del_marks = true;

    if (trx_undo_left(undo_page, ptr) < 5) {

      return 0;
    }

    /* Reserve 2 bytes to write the number of bytes the stored
    fields take in this undo record */

    ptr += 2;

    for (col_no = 0; col_no < dict_table_get_n_cols(table); col_no++) {

      const dict_col_t *col = dict_table_get_nth_col(table, col_no);

      if (col->ord_part) {
        ulint pos;

        /* Write field number to undo log */
        if (trx_undo_left(undo_page, ptr) < 5 + 15) {

          return 0;
        }

        pos = dict_index_get_nth_col_pos(index, col_no);
        ptr += mach_write_compressed(ptr, pos);

        /* Save the old value of field */
        field = rec_get_nth_field(rec, offsets, pos, &flen);

        if (rec_offs_nth_extern(offsets, pos)) {
          ptr =
            trx_undo_page_report_modify_ext(ptr, flen < REC_MAX_INDEX_COL_LEN && !ignore_prefix ? ext_buf : nullptr, &field, &flen);
        } else {
          ptr += mach_write_compressed(ptr, flen);
        }

        if (flen != UNIV_SQL_NULL) {
          if (trx_undo_left(undo_page, ptr) < flen) {

            return 0;
          }

          memcpy(ptr, field, flen);
          ptr += flen;
        }
      }
    }

    mach_write_to_2(old_ptr, ptr - old_ptr);
  }

  /*----------------------------------------*/
  /* Write pointers to the previous and the next undo log records */
  if (trx_undo_left(undo_page, ptr) < 2) {

    return 0;
  }

  mach_write_to_2(ptr, first_free);
  ptr += 2;
  mach_write_to_2(undo_page + first_free, ptr - undo_page);

  mach_write_to_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE, ptr - undo_page);

  /* Write to the REDO log about this change in the UNDO log */

  trx_undof_page_add_undo_rec_log(undo_page, first_free, ptr - undo_page, mtr);
  return first_free;
}

byte *trx_undo_update_rec_get_sys_cols(byte *ptr, trx_id_t *trx_id, roll_ptr_t *roll_ptr, ulint *info_bits) {
  /* Read the state of the info bits */
  *info_bits = mach_read_from_1(ptr);
  ptr += 1;

  /* Read the values of the system columns */

  *trx_id = mach_uint64_read_compressed(ptr);
  ptr += mach_uint64_get_compressed_size(*trx_id);

  *roll_ptr = mach_uint64_read_compressed(ptr);
  ptr += mach_uint64_get_compressed_size(*roll_ptr);

  return ptr;
}

/** Reads from an update undo log record the number of updated fields.
@return	remaining part of undo log record after reading this value */
inline byte *trx_undo_update_rec_get_n_upd_fields(
  byte *ptr, /*!< in: pointer to remaining part of undo log record */
  ulint *n
) /*!< out: number of fields */
{
  *n = mach_read_compressed(ptr);
  ptr += mach_get_compressed_size(*n);

  return ptr;
}

/** Reads from an update undo log record a stored field number.
@return	remaining part of undo log record after reading this value */
inline byte *trx_undo_update_rec_get_field_no(
  byte *ptr, /*!< in: pointer to remaining part of undo log record */
  ulint *field_no
) /*!< out: field number */
{
  *field_no = mach_read_compressed(ptr);
  ptr += mach_get_compressed_size(*field_no);

  return ptr;
}

byte *trx_undo_update_rec_get_update(
  byte *ptr, dict_index_t *index, ulint type, trx_id_t trx_id, roll_ptr_t roll_ptr, ulint info_bits, trx_t *trx, mem_heap_t *heap,
  upd_t **upd
) {
  upd_field_t *upd_field;
  upd_t *update;
  ulint n_fields;
  byte *buf;
  ulint i;

  ut_a(dict_index_is_clust(index));

  if (type != TRX_UNDO_DEL_MARK_REC) {
    ptr = trx_undo_update_rec_get_n_upd_fields(ptr, &n_fields);
  } else {
    n_fields = 0;
  }

  update = upd_create(n_fields + 2, heap);

  update->info_bits = info_bits;

  /* Store first trx id and roll ptr to update vector */

  upd_field = upd_get_nth_field(update, n_fields);
  buf = mem_heap_alloc(heap, DATA_TRX_ID_LEN);
  trx_write_trx_id(buf, trx_id);

  upd_field_set_field_no(upd_field, dict_index_get_sys_col_pos(index, DATA_TRX_ID), index, trx);
  dfield_set_data(&(upd_field->new_val), buf, DATA_TRX_ID_LEN);

  upd_field = upd_get_nth_field(update, n_fields + 1);
  buf = mem_heap_alloc(heap, DATA_ROLL_PTR_LEN);
  trx_write_roll_ptr(buf, roll_ptr);

  upd_field_set_field_no(upd_field, dict_index_get_sys_col_pos(index, DATA_ROLL_PTR), index, trx);
  dfield_set_data(&(upd_field->new_val), buf, DATA_ROLL_PTR_LEN);

  /* Store then the updated ordinary columns to the update vector */

  for (i = 0; i < n_fields; i++) {

    byte *field;
    ulint len;
    ulint field_no;
    ulint orig_len;

    ptr = trx_undo_update_rec_get_field_no(ptr, &field_no);

    if (field_no >= dict_index_get_n_fields(index)) {
      ib_logger(
        ib_stream,
        "Error: trying to access"
        " update undo rec field %lu in ",
        (ulong)field_no
      );
      dict_index_name_print(ib_stream, trx, index);
      ib_logger(
        ib_stream,
        "\n"
        "but index has only %lu fields\n"
        "Submit a detailed bug report, "
        "check the InnoDB website for details\n"
        "Run also CHECK TABLE ",
        (ulong)dict_index_get_n_fields(index)
      );
      ut_print_name(ib_stream, trx, true, index->table_name);
      ib_logger(
        ib_stream,
        "\n"
        "n_fields = %lu, i = %lu, ptr %p\n",
        (ulong)n_fields,
        (ulong)i,
        ptr
      );
      return nullptr;
    }

    upd_field = upd_get_nth_field(update, i);

    upd_field_set_field_no(upd_field, field_no, index, trx);

    ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

    upd_field->orig_len = orig_len;

    if (len == UNIV_SQL_NULL) {
      dfield_set_null(&upd_field->new_val);
    } else if (len < UNIV_EXTERN_STORAGE_FIELD) {
      dfield_set_data(&upd_field->new_val, field, len);
    } else {
      len -= UNIV_EXTERN_STORAGE_FIELD;

      dfield_set_data(&upd_field->new_val, field, len);
      dfield_set_ext(&upd_field->new_val);
    }
  }

  *upd = update;

  return ptr;
}

byte *trx_undo_rec_get_partial_row(byte *ptr, dict_index_t *index, dtuple_t **row, bool ignore_prefix, mem_heap_t *heap) {
  ut_ad(index);
  ut_ad(ptr);
  ut_ad(row);
  ut_ad(heap);
  ut_ad(dict_index_is_clust(index));

  auto row_len = dict_table_get_n_cols(index->table);

  *row = dtuple_create(heap, row_len);

  dict_table_copy_types(*row, index->table);

  auto end_ptr = ptr + mach_read_from_2(ptr);

  ptr += 2;

  while (ptr != end_ptr) {
    dfield_t *dfield;
    byte *field;
    ulint field_no;
    const dict_col_t *col;
    ulint col_no;
    ulint len;
    ulint orig_len;

    ptr = trx_undo_update_rec_get_field_no(ptr, &field_no);

    col = dict_index_get_nth_col(index, field_no);

    col_no = dict_col_get_no(col);

    ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

    dfield = dtuple_get_nth_field(*row, col_no);

    dfield_set_data(dfield, field, len);

    if (len != UNIV_SQL_NULL && len >= UNIV_EXTERN_STORAGE_FIELD) {
      dfield_set_len(dfield, len - UNIV_EXTERN_STORAGE_FIELD);
      dfield_set_ext(dfield);
      /* If the prefix of this column is indexed,
      ensure that enough prefix is stored in the
      undo log record. */
      if (!ignore_prefix && col->ord_part) {
        ut_a(dfield_get_len(dfield) >= 2 * BTR_EXTERN_FIELD_REF_SIZE);
        ut_a(
          dict_table_get_format(index->table) >= DICT_TF_FORMAT_V1 ||
          dfield_get_len(dfield) >= REC_MAX_INDEX_COL_LEN + BTR_EXTERN_FIELD_REF_SIZE
        );
      }
    }
  }

  return ptr;
}

/** Erases the unused undo log page end. */
static void trx_undo_erase_page_end(
  page_t *undo_page, /*!< in: undo page whose end to erase */
  mtr_t *mtr
) /*!< in: mtr */
{
  ulint first_free;

  first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
  memset(undo_page + first_free, 0xff, (UNIV_PAGE_SIZE - FIL_PAGE_DATA_END) - first_free);

  mlog_write_initial_log_record(undo_page, MLOG_UNDO_ERASE_END, mtr);
}

byte *trx_undo_parse_erase_page_end(byte *ptr, byte *end_ptr __attribute__((unused)), page_t *page, mtr_t *mtr) {
  ut_ad(ptr && end_ptr);

  if (page == nullptr) {

    return ptr;
  }

  trx_undo_erase_page_end(page, mtr);

  return ptr;
}

db_err trx_undo_report_row_operation(
  ulint flags, ulint op_type, que_thr_t *thr, dict_index_t *index, const dtuple_t *clust_entry, const upd_t *update,
  ulint cmpl_info, const rec_t *rec, roll_ptr_t *roll_ptr
) {
  trx_t *trx;
  trx_undo_t *undo;
  ulint page_no;
  trx_rseg_t *rseg;
  mtr_t mtr;
  db_err err = DB_SUCCESS;
  mem_heap_t *heap = nullptr;
  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  ulint *offsets = offsets_;
  rec_offs_init(offsets_);

  ut_a(dict_index_is_clust(index));

  if (flags & BTR_NO_UNDO_LOG_FLAG) {

    *roll_ptr = 0;

    return DB_SUCCESS;
  }

  ut_ad(thr);
  ut_ad((op_type != TRX_UNDO_INSERT_OP) || (clust_entry && !update && !rec));

  trx = thr_get_trx(thr);
  rseg = trx->rseg;

  mutex_enter(&(trx->undo_mutex));

  /* If the undo log is not assigned yet, assign one */

  if (op_type == TRX_UNDO_INSERT_OP) {

    if (trx->insert_undo == nullptr) {

      err = trx_undo_assign_undo(trx, TRX_UNDO_INSERT);
    }

    undo = trx->insert_undo;

    if (unlikely(!undo)) {
      /* Did not succeed */
      mutex_exit(&(trx->undo_mutex));

      return err;
    }
  } else {
    ut_ad(op_type == TRX_UNDO_MODIFY_OP);

    if (trx->update_undo == nullptr) {

      err = trx_undo_assign_undo(trx, TRX_UNDO_UPDATE);
    }

    undo = trx->update_undo;

    if (unlikely(!undo)) {
      /* Did not succeed */
      mutex_exit(&(trx->undo_mutex));
      return err;
    }

    offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED, &heap);
  }

  page_no = undo->last_page_no;

  mtr_start(&mtr);

  for (;;) {

    Buf_pool::Request req {
      .m_rw_latch = RW_X_LATCH,
      .m_page_id = { undo->space, page_no },
      .m_mode = BUF_GET,
      .m_file = __FILE__,
      .m_line = __LINE__,
      .m_mtr = &mtr
    };

    auto undo_block = buf_pool->get(req, undo->guess_block);

    buf_block_dbg_add_level(IF_SYNC_DEBUG(undo_block, SYNC_TRX_UNDO_PAGE));

    auto undo_page = undo_block->get_frame();

    ulint offset;

    if (op_type == TRX_UNDO_INSERT_OP) {
      offset = trx_undo_page_report_insert(undo_page, trx, index, clust_entry, &mtr);
    } else {
      offset = trx_undo_page_report_modify(undo_page, trx, index, rec, offsets, update, cmpl_info, &mtr);
    }

    if (unlikely(offset == 0)) {
      /* The record did not fit on the page. We erase the
      end segment of the undo log page and write a log
      record of it: this is to ensure that in the debug
      version the replicate page constructed using the log
      records stays identical to the original page */

      trx_undo_erase_page_end(undo_page, &mtr);
      mtr_commit(&mtr);
    } else {
      /* Success */

      mtr_commit(&mtr);

      undo->empty = false;
      undo->top_page_no = page_no;
      undo->top_offset = offset;
      undo->top_undo_no = trx->undo_no;
      undo->guess_block = undo_block;

      ++trx->undo_no;

      mutex_exit(&trx->undo_mutex);

      *roll_ptr = trx_undo_build_roll_ptr(op_type == TRX_UNDO_INSERT_OP, rseg->id, page_no, offset);
      if (likely_null(heap)) {
        mem_heap_free(heap);
      }
      return DB_SUCCESS;
    }

    ut_ad(page_no == undo->last_page_no);

    /* We have to extend the undo log by one page */

    mtr_start(&mtr);

    /* When we add a page to an undo log, this is analogous to
    a pessimistic insert in a B-tree, and we must reserve the
    counterpart of the tree latch, which is the rseg mutex. */

    mutex_enter(&(rseg->mutex));

    page_no = trx_undo_add_page(trx, undo, &mtr);

    mutex_exit(&(rseg->mutex));

    if (unlikely(page_no == FIL_NULL)) {
      /* Did not succeed: out of space */

      mutex_exit(&(trx->undo_mutex));
      mtr_commit(&mtr);
      if (likely_null(heap)) {
        mem_heap_free(heap);
      }
      return DB_OUT_OF_FILE_SPACE;
    }
  }
}

trx_undo_rec_t *trx_undo_get_undo_rec_low(roll_ptr_t roll_ptr, mem_heap_t *heap) {
  trx_undo_rec_t *undo_rec;
  ulint rseg_id;
  ulint page_no;
  ulint offset;
  const page_t *undo_page;
  trx_rseg_t *rseg;
  bool is_insert;
  mtr_t mtr;

  trx_undo_decode_roll_ptr(roll_ptr, &is_insert, &rseg_id, &page_no, &offset);
  rseg = trx_rseg_get_on_id(rseg_id);

  mtr_start(&mtr);

  undo_page = trx_undo_page_get_s_latched(rseg->space, page_no, &mtr);

  undo_rec = trx_undo_rec_copy(undo_page + offset, heap);

  mtr_commit(&mtr);

  return undo_rec;
}

db_err trx_undo_get_undo_rec(roll_ptr_t roll_ptr, trx_id_t trx_id, trx_undo_rec_t **undo_rec, mem_heap_t *heap) {
#ifdef UNIV_SYNC_DEBUG
  ut_ad(rw_lock_own(&(purge_sys->latch), RW_LOCK_SHARED));
#endif /* UNIV_SYNC_DEBUG */

  if (!trx_purge_update_undo_must_exist(trx_id)) {

    /* It may be that the necessary undo log has already been
    deleted */

    return DB_MISSING_HISTORY;
  }

  *undo_rec = trx_undo_get_undo_rec_low(roll_ptr, heap);

  return DB_SUCCESS;
}

db_err trx_undo_prev_version_build(
  const rec_t *index_rec, mtr_t *index_mtr __attribute__((unused)), const rec_t *rec, dict_index_t *index, ulint *offsets,
  mem_heap_t *heap, rec_t **old_vers
) {
  trx_undo_rec_t *undo_rec = nullptr;
  dtuple_t *entry;
  trx_id_t rec_trx_id;
  ulint type;
  undo_no_t undo_no;
  uint64_t table_id;
  trx_id_t trx_id;
  roll_ptr_t roll_ptr;
  roll_ptr_t old_roll_ptr;
  upd_t *update;
  byte *ptr;
  ulint info_bits;
  ulint cmpl_info;
  bool dummy_extern;
  byte *buf;
#ifdef UNIV_SYNC_DEBUG
  ut_ad(rw_lock_own(&(purge_sys->latch), RW_LOCK_SHARED));
#endif /* UNIV_SYNC_DEBUG */
  ut_ad(
    mtr_memo_contains_page(index_mtr, index_rec, MTR_MEMO_PAGE_S_FIX) ||
    mtr_memo_contains_page(index_mtr, index_rec, MTR_MEMO_PAGE_X_FIX)
  );
  ut_ad(rec_offs_validate(rec, index, offsets));

  if (!dict_index_is_clust(index)) {
    ib_logger(
      ib_stream,
      "Error: trying to access"
      " update undo rec for non-clustered index %s\n"
      "Submit a detailed bug report, "
      "check the InnoDB website for details\n"
      "index record ",
      index->name
    );
    rec_print(ib_stream, index_rec, index);
    ib_logger(ib_stream, "\nrecord version ");
    rec_print_new(ib_stream, rec, offsets);
    ib_logger(ib_stream, "\n");
    return DB_ERROR;
  }

  roll_ptr = row_get_rec_roll_ptr(rec, index, offsets);
  old_roll_ptr = roll_ptr;

  *old_vers = nullptr;

  if (trx_undo_roll_ptr_is_insert(roll_ptr)) {

    /* The record rec is the first inserted version */

    return DB_SUCCESS;
  }

  rec_trx_id = row_get_rec_trx_id(rec, index, offsets);

  auto err = trx_undo_get_undo_rec(roll_ptr, rec_trx_id, &undo_rec, heap);

  if (unlikely(err != DB_SUCCESS)) {
    /* The undo record may already have been purged.
    This should never happen in InnoDB. */

    return err;
  }

  ptr = trx_undo_rec_get_pars(undo_rec, &type, &cmpl_info, &dummy_extern, &undo_no, &table_id);

  ptr = trx_undo_update_rec_get_sys_cols(ptr, &trx_id, &roll_ptr, &info_bits);

  /* (a) If a clustered index record version is such that the
  trx id stamp in it is bigger than purge_sys->view, then the
  BLOBs in that version are known to exist (the purge has not
  progressed that far);

  (b) if the version is the first version such that trx id in it
  is less than purge_sys->view, and it is not delete-marked,
  then the BLOBs in that version are known to exist (the purge
  cannot have purged the BLOBs referenced by that version
  yet).

  This function does not fetch any BLOBs.  The callers might, by
  possibly invoking row_ext_create() via row_build().  However,
  they should have all needed information in the *old_vers
  returned by this function.  This is because *old_vers is based
  on the transaction undo log records.  The function
  trx_undo_page_fetch_ext() will write BLOB prefixes to the
  transaction undo log that are at least as long as the longest
  possible column prefix in a secondary index.  Thus, secondary
  index entries for *old_vers can be constructed without
  dereferencing any BLOB pointers. */

  ptr = trx_undo_rec_skip_row_ref(ptr, index);

  ptr = trx_undo_update_rec_get_update(ptr, index, type, trx_id, roll_ptr, info_bits, nullptr, heap, &update);

  if (table_id != index->table->id) {
    ptr = nullptr;

    ib_logger(
      ib_stream,
      "Error: trying to access update undo rec"
      " for table %s\n"
      "but the table id in the"
      " undo record is wrong\n"
      "Submit a detailed bug report, "
      "check InnoDB website for details\n"
      "Run also CHECK TABLE %s\n",
      index->table_name,
      index->table_name
    );
  }

  if (ptr == nullptr) {
    /* The record was corrupted, return an error; these printfs
    should catch an elusive bug in row_vers_old_has_index_entry */

    ib_logger(
      ib_stream,
      "table %s, index %s, n_uniq %lu\n"
      "undo rec address %p, type %lu cmpl_info %lu\n"
      "undo rec table id %lu,"
      " index table id %lu\n"
      "dump of 150 bytes in undo rec: ",
      index->table_name,
      index->name,
      (ulong)dict_index_get_n_unique(index),
      undo_rec,
      (ulong)type,
      (ulong)cmpl_info,
      (ulong)table_id,
      (ulong)index->table->id
    );
    ut_print_buf(ib_stream, undo_rec, 150);
    ib_logger(ib_stream, "\nindex record ");
    rec_print(ib_stream, index_rec, index);
    ib_logger(ib_stream, "\nrecord version ");
    rec_print_new(ib_stream, rec, offsets);
    ib_logger(
      ib_stream,
      "\n"
      "Record trx id %lu, update rec trx id %lu\n"
      "Roll ptr in rec %lu, in update rec %lu\n",
      TRX_ID_PREP_PRINTF(rec_trx_id),
      TRX_ID_PREP_PRINTF(trx_id),
      (ulong)old_roll_ptr,
      (ulong)roll_ptr
    );

    trx_purge_sys_print();
    return DB_ERROR;
  }

  if (row_upd_changes_field_size_or_external(index, offsets, update)) {
    ulint n_ext;

    /* We have to set the appropriate extern storage bits in the
    old version of the record: the extern bits in rec for those
    fields that update does NOT update, as well as the bits for
    those fields that update updates to become externally stored
    fields. Store the info: */

    entry = row_rec_to_index_entry(ROW_COPY_DATA, rec, index, offsets, &n_ext, heap);
    n_ext += btr_push_update_extern_fields(entry, update, heap);
    /* The page containing the clustered index record
    corresponding to entry is latched in mtr.  Thus the
    following call is safe. */
    row_upd_index_replace_new_col_vals(entry, index, update, heap);

    buf = mem_heap_alloc(heap, rec_get_converted_size(index, entry, n_ext));

    *old_vers = rec_convert_dtuple_to_rec(buf, index, entry, n_ext);
  } else {
    buf = mem_heap_alloc(heap, rec_offs_size(offsets));
    *old_vers = rec_copy(buf, rec, offsets);
    rec_offs_make_valid(*old_vers, index, offsets);
    row_upd_rec_in_place(*old_vers, index, offsets, update);
  }

  return DB_SUCCESS;
}
