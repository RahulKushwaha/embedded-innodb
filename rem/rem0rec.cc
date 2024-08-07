/****************************************************************************
Copyright (c) 1994, 2010, Innobase Oy. All Rights Reserved.

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

/** @file rem/rem0rec.c
Record manager

Created 5/30/1994 Heikki Tuuri
*************************************************************************/

#include <sstream>
#include <format>

#include "btr0types.h"
#include "mtr0log.h"
#include "mtr0mtr.h"
#include "rem0rec.h"

/*			PHYSICAL RECORD (OLD STYLE)
                        ===========================

The physical record, which is the data type of all the records
found in index pages of the database, has the following format
(lower addresses and more significant bits inside a byte are below
represented on a higher text line):

| offset of the end of the last field of data, the most significant
  bit is set to 1 if and only if the field is SQL-null,
  if the offset is 2-byte, then the second most significant
  bit is set to 1 if the field is stored on another page:
  mostly this will occur in the case of big BLOB fields |
...
| offset of the end of the first field of data + the SQL-null bit |
| 4 bits used to delete mark a record, and mark a predefined
  minimum record in alphabetical order |
| 4 bits giving the number of records owned by this record
  (this term is explained in page0page.h) |
| 13 bits giving the order number of this record in the
  heap of the index page |
| 10 bits giving the number of fields in this record |
| 1 bit which is set to 1 if the offsets above are given in
  one byte format, 0 if in two byte format |
| two bytes giving an absolute pointer to the next record in the page |
ORIGIN of the record
| first field of data |
...
| last field of data |

The origin of the record is the start address of the first field
of data. The offsets are given relative to the origin.
The offsets of the data fields are stored in an inverted
order because then the offset of the first fields are near the
origin, giving maybe a better processor cache hit rate in searches.

The offsets of the data fields are given as one-byte
(if there are less than 127 bytes of data in the record)
or two-byte unsigned integers. The most significant bit
is not part of the offset, instead it indicates the SQL-null
if the bit is set to 1. */

/*			PHYSICAL RECORD (NEW STYLE)
                        ===========================

The physical record, which is the data type of all the records
found in index pages of the database, has the following format
(lower addresses and more significant bits inside a byte are below
represented on a higher text line):

| length of the last non-null variable-length field of data:
  if the maximum length is 255, one byte; otherwise,
  0xxxxxxx (one byte, length=0..127), or 1exxxxxxxxxxxxxx (two bytes,
  length=128..16383, extern storage flag) |
...
| length of first variable-length field of data |
| SQL-null flags (1 bit per nullable field), padded to full bytes |
| 4 bits used to delete mark a record, and mark a predefined
  minimum record in alphabetical order |
| 4 bits giving the number of records owned by this record
  (this term is explained in page0page.h) |
| 13 bits giving the order number of this record in the
  heap of the index page |
| 3 bits record type: 000=conventional, 001=node pointer (inside B-tree),
  010=infimum, 011=supremum, 1xx=reserved |
| two bytes giving a relative pointer to the next record in the page |
ORIGIN of the record
| first field of data |
...
| last field of data |

The origin of the record is the start address of the first field
of data. The offsets are given relative to the origin.
The offsets of the data fields are stored in an inverted
order because then the offset of the first fields are near the
origin, giving maybe a better processor cache hit rate in searches.

The offsets of the data fields are given as one-byte
(if there are less than 127 bytes of data in the record)
or two-byte unsigned integers. The most significant bit
is not part of the offset, instead it indicates the SQL-null
if the bit is set to 1. */

/* CANONICAL COORDINATES. A record can be seen as a single
string of 'characters' in the following way: catenate the bytes
in each field, in the order of fields. An SQL-null field
is taken to be an empty sequence of bytes. Then after
the position of each field insert in the string
the 'character' <FIELD-END>, except that after an SQL-null field
insert <NULL-FIELD-END>. Now the ordinal position of each
byte in this canonical string is its canonical coordinate.
So, for the record ("AA", SQL-NULL, "BB", ""), the canonical
string is "AA<FIELD_END><NULL-FIELD-END>BB<FIELD-END><FIELD-END>".
We identify prefixes (= initial segments) of a record
with prefixes of the canonical string. The canonical
length of the prefix is the length of the corresponding
prefix of the canonical string. The canonical length of
a record is the length of its canonical string.

For example, the maximal common prefix of records
("AA", SQL-NULL, "BB", "C") and ("AA", SQL-NULL, "B", "C")
is "AA<FIELD-END><NULL-FIELD-END>B", and its canonical
length is 5.

A complete-field prefix of a record is a prefix which ends at the
end of some field (containing also <FIELD-END>).
A record is a complete-field prefix of another record, if
the corresponding canonical strings have the same property. */

/** Validates the consistency of an old-style physical record.
@return	true if ok */
static bool rec_validate_old(const rec_t *rec); /*!< in: physical record */

ulint rec_get_n_extern_new(const rec_t *rec, dict_index_t *index, ulint n) {
  const byte *nulls;
  const byte *lens;
  dict_field_t *field;
  ulint null_mask;
  ulint n_extern;
  ulint i;

  ut_ad(dict_table_is_comp(index->table));
  ut_ad(rec_get_status(rec) == REC_STATUS_ORDINARY);
  ut_ad(n == ULINT_UNDEFINED || n <= dict_index_get_n_fields(index));

  if (n == ULINT_UNDEFINED) {
    n = dict_index_get_n_fields(index);
  }

  nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
  lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);
  null_mask = 1;
  n_extern = 0;
  i = 0;

  /* read the lengths of fields 0..n */
  do {
    ulint len;

    field = dict_index_get_nth_field(index, i);
    if (!(dict_field_get_col(field)->prtype & DATA_NOT_NULL)) {
      /* nullable field => read the null flag */

      if (unlikely(!(byte)null_mask)) {
        nulls--;
        null_mask = 1;
      }

      if (*nulls & null_mask) {
        null_mask <<= 1;
        /* No length is stored for NULL fields. */
        continue;
      }
      null_mask <<= 1;
    }

    if (unlikely(!field->fixed_len)) {
      /* Variable-length field: read the length */
      const dict_col_t *col = dict_field_get_col(field);
      len = *lens--;
      /* If the maximum length of the field is up
      to 255 bytes, the actual length is always
      stored in one byte. If the maximum length is
      more than 255 bytes, the actual length is
      stored in one byte for 0..127.  The length
      will be encoded in two bytes when it is 128 or
      more, or when the field is stored externally. */
      if (unlikely(col->len > 255) || unlikely(col->mtype == DATA_BLOB)) {
        if (len & 0x80) {
          /* 1exxxxxxx xxxxxxxx */
          if (len & 0x40) {
            n_extern++;
          }
          lens--;
        }
      }
    }
  } while (++i < n);

  return (n_extern);
}

void rec_init_offsets_comp_ordinary(const rec_t *rec, ulint extra, const dict_index_t *index, ulint *offsets) {
  ulint i = 0;
  ulint offs = 0;
  ulint any_ext = 0;
  const byte *nulls = rec - (extra + 1);
  const byte *lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);
  dict_field_t *field;
  ulint null_mask = 1;

#ifdef UNIV_DEBUG
  /* We cannot invoke rec_offs_make_valid() here, because it can hold
  that extra != REC_N_NEW_EXTRA_BYTES.  Similarly, rec_offs_validate()
  will fail in that case, because it invokes rec_get_status(). */
  offsets[2] = (ulint)rec;
  offsets[3] = (ulint)index;
#endif /* UNIV_DEBUG */

  /* read the lengths of fields 0..n */
  do {
    ulint len;

    field = dict_index_get_nth_field(index, i);
    if (!(dict_field_get_col(field)->prtype & DATA_NOT_NULL)) {
      /* nullable field => read the null flag */

      if (unlikely(!(byte)null_mask)) {
        nulls--;
        null_mask = 1;
      }

      if (*nulls & null_mask) {
        null_mask <<= 1;
        /* No length is stored for NULL fields.
        We do not advance offs, and we set
        the length to zero and enable the
        SQL NULL flag in offsets[]. */
        len = offs | REC_OFFS_SQL_NULL;
        goto resolved;
      }
      null_mask <<= 1;
    }

    if (unlikely(!field->fixed_len)) {
      /* Variable-length field: read the length */
      const dict_col_t *col = dict_field_get_col(field);
      len = *lens--;
      /* If the maximum length of the field is up
      to 255 bytes, the actual length is always
      stored in one byte. If the maximum length is
      more than 255 bytes, the actual length is
      stored in one byte for 0..127.  The length
      will be encoded in two bytes when it is 128 or
      more, or when the field is stored externally. */
      if (unlikely(col->len > 255) || unlikely(col->mtype == DATA_BLOB)) {
        if (len & 0x80) {
          /* 1exxxxxxx xxxxxxxx */
          len <<= 8;
          len |= *lens--;

          offs += len & 0x3fff;
          if (unlikely(len & 0x4000)) {
            ut_ad(dict_index_is_clust(index));
            any_ext = REC_OFFS_EXTERNAL;
            len = offs | REC_OFFS_EXTERNAL;
          } else {
            len = offs;
          }

          goto resolved;
        }
      }

      len = offs += len;
    } else {
      len = offs += field->fixed_len;
    }
  resolved:
    rec_offs_base(offsets)[i + 1] = len;
  } while (++i < rec_offs_n_fields(offsets));

  *rec_offs_base(offsets) = (rec - (lens + 1)) | REC_OFFS_COMPACT | any_ext;
}

/**
 * The following function determines the offsets to each field in the
 * record.	 The offsets are written to a previously allocated array of
 * ulint, where rec_offs_n_fields(offsets) has been initialized to the
 * number of fields in the record.	 The rest of the array will be
 * initialized by this function.  rec_offs_base(offsets)[0] will be set
 * to the extra size (if REC_OFFS_COMPACT is set, the record is in the
 * new format; if REC_OFFS_EXTERNAL is set, the record contains externally
 * stored columns), and rec_offs_base(offsets)[1..n_fields] will be set to
 * offsets past the end of fields 0..n_fields, or to the beginning of
 * fields 1..n_fields+1.  When the high-order bit of the offset at [i+1]
 * is set (REC_OFFS_SQL_NULL), the field i is NULL.  When the second
 * high-order bit of the offset at [i+1] is set (REC_OFFS_EXTERNAL), the
 * field i is being stored externally.
 * 
 * @param[in] rec physical record
 * @param[in] index record descriptor
 * @param[in] offsets array of offsets; n=rec_offs_n_fields(offsets)
 */
static void rec_init_offsets(const rec_t *rec, const dict_index_t *index, ulint *offsets) {
  ulint i = 0;
  ulint offs;

  rec_offs_make_valid(rec, index, offsets);

  if (dict_table_is_comp(index->table)) {
    const byte *nulls;
    const byte *lens;
    dict_field_t *field;
    ulint null_mask;
    ulint status = rec_get_status(rec);
    ulint n_node_ptr_field = ULINT_UNDEFINED;

    switch (expect(status, REC_STATUS_ORDINARY)) {
      case REC_STATUS_INFIMUM:
      case REC_STATUS_SUPREMUM:
        /* the field is 8 bytes long */
        rec_offs_base(offsets)[0] = REC_N_NEW_EXTRA_BYTES | REC_OFFS_COMPACT;
        rec_offs_base(offsets)[1] = 8;
        return;
      case REC_STATUS_NODE_PTR:
        n_node_ptr_field = dict_index_get_n_unique_in_tree(index);
        break;
      case REC_STATUS_ORDINARY:
        rec_init_offsets_comp_ordinary(rec, REC_N_NEW_EXTRA_BYTES, index, offsets);
        return;
    }

    nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
    lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);
    offs = 0;
    null_mask = 1;

    /* read the lengths of fields 0..n */
    do {
      ulint len;
      if (unlikely(i == n_node_ptr_field)) {
        len = offs += 4;
        goto resolved;
      }

      field = dict_index_get_nth_field(index, i);
      if (!(dict_field_get_col(field)->prtype & DATA_NOT_NULL)) {
        /* nullable field => read the null flag */

        if (unlikely(!(byte)null_mask)) {
          nulls--;
          null_mask = 1;
        }

        if (*nulls & null_mask) {
          null_mask <<= 1;
          /* No length is stored for NULL fields.
          We do not advance offs, and we set
          the length to zero and enable the
          SQL NULL flag in offsets[]. */
          len = offs | REC_OFFS_SQL_NULL;
          goto resolved;
        }
        null_mask <<= 1;
      }

      if (unlikely(!field->fixed_len)) {
        /* Variable-length field: read the length */
        const dict_col_t *col = dict_field_get_col(field);
        len = *lens--;
        /* If the maximum length of the field
        is up to 255 bytes, the actual length
        is always stored in one byte. If the
        maximum length is more than 255 bytes,
        the actual length is stored in one
        byte for 0..127.  The length will be
        encoded in two bytes when it is 128 or
        more, or when the field is stored
        externally. */
        if (unlikely(col->len > 255) || unlikely(col->mtype == DATA_BLOB)) {
          if (len & 0x80) {
            /* 1exxxxxxx xxxxxxxx */

            len <<= 8;
            len |= *lens--;

            /* B-tree node pointers
            must not contain externally
            stored columns.  Thus
            the "e" flag must be 0. */
            ut_a(!(len & 0x4000));
            offs += len & 0x3fff;
            len = offs;

            goto resolved;
          }
        }

        len = offs += len;
      } else {
        len = offs += field->fixed_len;
      }
    resolved:
      rec_offs_base(offsets)[i + 1] = len;
    } while (++i < rec_offs_n_fields(offsets));

    *rec_offs_base(offsets) = (rec - (lens + 1)) | REC_OFFS_COMPACT;
  } else {
    /* Old-style record: determine extra size and end offsets */
    offs = REC_N_OLD_EXTRA_BYTES;
    if (rec_get_1byte_offs_flag(rec)) {
      offs += rec_offs_n_fields(offsets);
      *rec_offs_base(offsets) = offs;
      /* Determine offsets to fields */
      do {
        offs = rec_1_get_field_end_info(rec, i);
        if (offs & REC_1BYTE_SQL_NULL_MASK) {
          offs &= ~REC_1BYTE_SQL_NULL_MASK;
          offs |= REC_OFFS_SQL_NULL;
        }
        rec_offs_base(offsets)[1 + i] = offs;
      } while (++i < rec_offs_n_fields(offsets));
    } else {
      offs += 2 * rec_offs_n_fields(offsets);
      *rec_offs_base(offsets) = offs;
      /* Determine offsets to fields */
      do {
        offs = rec_2_get_field_end_info(rec, i);
        if (offs & REC_2BYTE_SQL_NULL_MASK) {
          offs &= ~REC_2BYTE_SQL_NULL_MASK;
          offs |= REC_OFFS_SQL_NULL;
        }
        if (offs & REC_2BYTE_EXTERN_MASK) {
          offs &= ~REC_2BYTE_EXTERN_MASK;
          offs |= REC_OFFS_EXTERNAL;
          *rec_offs_base(offsets) |= REC_OFFS_EXTERNAL;
        }
        rec_offs_base(offsets)[1 + i] = offs;
      } while (++i < rec_offs_n_fields(offsets));
    }
  }
}

ulint *rec_get_offsets_func(const rec_t *rec, const dict_index_t *index, ulint *offsets, ulint n_fields, mem_heap_t **heap, const char *file, ulint line) {
  ulint n;
  ulint size;

  ut_ad(rec);
  ut_ad(index);
  ut_ad(heap);

  if (dict_table_is_comp(index->table)) {
    switch (expect(rec_get_status(rec), REC_STATUS_ORDINARY)) {
      case REC_STATUS_ORDINARY:
        n = dict_index_get_n_fields(index);
        break;
      case REC_STATUS_NODE_PTR:
        n = dict_index_get_n_unique_in_tree(index) + 1;
        break;
      case REC_STATUS_INFIMUM:
      case REC_STATUS_SUPREMUM:
        /* infimum or supremum record */
        n = 1;
        break;
      default:
        ut_error;
        return (nullptr);
    }
  } else {
    n = rec_get_n_fields_old(rec);
  }

  if (unlikely(n_fields < n)) {
    n = n_fields;
  }

  size = n + (1 + REC_OFFS_HEADER_SIZE);

  if (unlikely(!offsets) || unlikely(rec_offs_get_n_alloc(offsets) < size)) {
    if (unlikely(!*heap)) {
      *heap = mem_heap_create_func(size * sizeof(ulint), MEM_HEAP_DYNAMIC, file, line);
    }
    offsets = reinterpret_cast<ulint *>(mem_heap_alloc(*heap, size * sizeof(ulint)));
    rec_offs_set_n_alloc(offsets, size);
  }

  rec_offs_set_n_fields(offsets, n);
  rec_init_offsets(rec, index, offsets);
  return (offsets);
}

void rec_get_offsets_reverse(const byte *extra, const dict_index_t *index, ulint node_ptr, ulint *offsets) {
  ulint n;
  ulint i;
  ulint offs;
  ulint any_ext;
  const byte *nulls;
  const byte *lens;
  dict_field_t *field;
  ulint null_mask;
  ulint n_node_ptr_field;

  ut_ad(extra);
  ut_ad(index);
  ut_ad(offsets);
  ut_ad(dict_table_is_comp(index->table));

  if (unlikely(node_ptr)) {
    n_node_ptr_field = dict_index_get_n_unique_in_tree(index);
    n = n_node_ptr_field + 1;
  } else {
    n_node_ptr_field = ULINT_UNDEFINED;
    n = dict_index_get_n_fields(index);
  }

  ut_a(rec_offs_get_n_alloc(offsets) >= n + (1 + REC_OFFS_HEADER_SIZE));
  rec_offs_set_n_fields(offsets, n);

  nulls = extra;
  lens = nulls + UT_BITS_IN_BYTES(index->n_nullable);
  i = offs = 0;
  null_mask = 1;
  any_ext = 0;

  /* read the lengths of fields 0..n */
  do {
    ulint len;
    if (unlikely(i == n_node_ptr_field)) {
      len = offs += 4;
      goto resolved;
    }

    field = dict_index_get_nth_field(index, i);
    if (!(dict_field_get_col(field)->prtype & DATA_NOT_NULL)) {
      /* nullable field => read the null flag */

      if (unlikely(!(byte)null_mask)) {
        nulls++;
        null_mask = 1;
      }

      if (*nulls & null_mask) {
        null_mask <<= 1;
        /* No length is stored for NULL fields.
        We do not advance offs, and we set
        the length to zero and enable the
        SQL NULL flag in offsets[]. */
        len = offs | REC_OFFS_SQL_NULL;
        goto resolved;
      }
      null_mask <<= 1;
    }

    if (unlikely(!field->fixed_len)) {
      /* Variable-length field: read the length */
      const dict_col_t *col = dict_field_get_col(field);
      len = *lens++;
      /* If the maximum length of the field is up
      to 255 bytes, the actual length is always
      stored in one byte. If the maximum length is
      more than 255 bytes, the actual length is
      stored in one byte for 0..127.  The length
      will be encoded in two bytes when it is 128 or
      more, or when the field is stored externally. */
      if (unlikely(col->len > 255) || unlikely(col->mtype == DATA_BLOB)) {
        if (len & 0x80) {
          /* 1exxxxxxx xxxxxxxx */
          len <<= 8;
          len |= *lens++;

          offs += len & 0x3fff;
          if (unlikely(len & 0x4000)) {
            any_ext = REC_OFFS_EXTERNAL;
            len = offs | REC_OFFS_EXTERNAL;
          } else {
            len = offs;
          }

          goto resolved;
        }
      }

      len = offs += len;
    } else {
      len = offs += field->fixed_len;
    }
  resolved:
    rec_offs_base(offsets)[i + 1] = len;
  } while (++i < rec_offs_n_fields(offsets));

  ut_ad(lens >= extra);
  *rec_offs_base(offsets) = (lens - extra + REC_N_NEW_EXTRA_BYTES) | REC_OFFS_COMPACT | any_ext;
}

ulint rec_get_nth_field_offs_old(const rec_t *rec, ulint n, ulint *len) {
  ulint os;
  ulint next_os;

  ut_ad(len);
  ut_a(rec);
  ut_a(n < rec_get_n_fields_old(rec));

  if (rec_get_1byte_offs_flag(rec)) {
    os = rec_1_get_field_start_offs(rec, n);

    next_os = rec_1_get_field_end_info(rec, n);

    if (next_os & REC_1BYTE_SQL_NULL_MASK) {
      *len = UNIV_SQL_NULL;

      return os;
    }

    next_os = next_os & ~REC_1BYTE_SQL_NULL_MASK;
  } else {
    os = rec_2_get_field_start_offs(rec, n);

    next_os = rec_2_get_field_end_info(rec, n);

    if (next_os & REC_2BYTE_SQL_NULL_MASK) {
      *len = UNIV_SQL_NULL;

      return os;
    }

    next_os = next_os & ~(REC_2BYTE_SQL_NULL_MASK | REC_2BYTE_EXTERN_MASK);
  }

  *len = next_os - os;

  ut_ad(*len < UNIV_PAGE_SIZE);

  return os;
}

ulint rec_get_converted_size_comp_prefix(const dict_index_t *index, const dfield_t *fields, ulint n_fields, ulint *extra) {
  ulint extra_size;
  ulint data_size;
  ulint i;
  ut_ad(index);
  ut_ad(fields);
  ut_ad(n_fields > 0);
  ut_ad(n_fields <= dict_index_get_n_fields(index));

  extra_size = REC_N_NEW_EXTRA_BYTES + UT_BITS_IN_BYTES(index->n_nullable);
  data_size = 0;

  /* read the lengths of fields 0..n */
  for (i = 0; i < n_fields; i++) {
    const dict_field_t *field;
    ulint len;
    const dict_col_t *col;

    field = dict_index_get_nth_field(index, i);
    len = dfield_get_len(&fields[i]);
    col = dict_field_get_col(field);

    ut_ad(dict_col_type_assert_equal(col, dfield_get_type(&fields[i])));

    if (dfield_is_null(&fields[i])) {
      /* No length is stored for NULL fields. */
      ut_ad(!(col->prtype & DATA_NOT_NULL));
      continue;
    }

    ut_ad(len <= col->len || col->mtype == DATA_BLOB || col->mtype == DATA_DECIMAL);

    /* If the maximum length of a variable-length field
    is up to 255 bytes, the actual length is always stored
    in one byte. If the maximum length is more than 255
    bytes, the actual length is stored in one byte for
    0..127.  The length will be encoded in two bytes when
    it is 128 or more, or when the field is stored externally. */

    if (field->fixed_len) {
      ut_ad(len == field->fixed_len);
      /* dict_index_add_col() should guarantee this */
      ut_ad(!field->prefix_len || field->fixed_len == field->prefix_len);
    } else if (dfield_is_ext(&fields[i])) {
      ut_ad(col->len >= 256 || col->mtype == DATA_BLOB);
      extra_size += 2;
    } else if (len < 128 || (col->len < 256 && col->mtype != DATA_BLOB)) {
      extra_size++;
    } else {
      /* For variable-length columns, we look up the
      maximum length from the column itself.  If this
      is a prefix index column shorter than 256 bytes,
      this will waste one byte. */
      extra_size += 2;
    }
    data_size += len;
  }

  if (likely_null(extra)) {
    *extra = extra_size;
  }

  return extra_size + data_size;
}

ulint rec_get_converted_size_comp(const dict_index_t *index, ulint status, const dfield_t *fields, ulint n_fields, ulint *extra) {
  ulint size;

  ut_ad(index);
  ut_ad(fields);
  ut_ad(n_fields > 0);

  switch (expect(status, REC_STATUS_ORDINARY)) {
    case REC_STATUS_ORDINARY:
      ut_ad(n_fields == dict_index_get_n_fields(index));
      size = 0;
      break;
    case REC_STATUS_NODE_PTR:
      n_fields--;
      ut_ad(n_fields == dict_index_get_n_unique_in_tree(index));
      ut_ad(dfield_get_len(&fields[n_fields]) == REC_NODE_PTR_SIZE);
      size = REC_NODE_PTR_SIZE; /* child page number */
      break;
    case REC_STATUS_INFIMUM:
    case REC_STATUS_SUPREMUM:
      /* infimum or supremum record, 8 data bytes */
      if (likely_null(extra)) {
        *extra = REC_N_NEW_EXTRA_BYTES;
      }
      return (REC_N_NEW_EXTRA_BYTES + 8);
    default:
      ut_error;
      return (ULINT_UNDEFINED);
  }

  return (size + rec_get_converted_size_comp_prefix(index, fields, n_fields, extra));
}

void rec_set_nth_field_null_bit(rec_t *rec, ulint i, bool val) {
  ulint info;

  if (rec_get_1byte_offs_flag(rec)) {

    info = rec_1_get_field_end_info(rec, i);

    if (val) {
      info = info | REC_1BYTE_SQL_NULL_MASK;
    } else {
      info = info & ~REC_1BYTE_SQL_NULL_MASK;
    }

    rec_1_set_field_end_info(rec, i, info);

    return;
  }

  info = rec_2_get_field_end_info(rec, i);

  if (val) {
    info = info | REC_2BYTE_SQL_NULL_MASK;
  } else {
    info = info & ~REC_2BYTE_SQL_NULL_MASK;
  }

  rec_2_set_field_end_info(rec, i, info);
}

void rec_set_nth_field_sql_null(rec_t *rec, ulint n) {
  auto offset = rec_get_field_start_offs(rec, n);

  data_write_sql_null(rec + offset, rec_get_nth_field_size(rec, n));

  rec_set_nth_field_null_bit(rec, n, true);
}

/**
 * Builds an old-style physical record out of a data tuple and
 * stores it beginning from the start of the given buffer.
 * 
 * @param[in] buf start address of the physical record
 * @param[in] dtuple data tuple
 * @param[in] n_ext number of externally stored columns
 *  
 * @return	pointer to the origin of physical record
 */
static rec_t *rec_convert_dtuple_to_rec_old(byte *buf, const dtuple_t *dtuple, ulint n_ext) {
  const dfield_t *field;
  ulint n_fields;
  ulint data_size;
  rec_t *rec;
  ulint end_offset;
  ulint ored_offset;
  ulint len;
  ulint i;

  ut_ad(buf && dtuple);
  ut_ad(dtuple_validate(dtuple));
  ut_ad(dtuple_check_typed(dtuple));

  n_fields = dtuple_get_n_fields(dtuple);
  data_size = dtuple_get_data_size(dtuple, 0);

  ut_ad(n_fields > 0);

  /* Calculate the offset of the origin in the physical record */

  rec = buf + rec_get_converted_extra_size(data_size, n_fields, n_ext);
#ifdef UNIV_DEBUG
  /* Suppress Valgrind warnings of ut_ad()
  in mach_write_to_1(), mach_write_to_2() et al. */
  memset(buf, 0xff, rec - buf + data_size);
#endif /* UNIV_DEBUG */
  /* Store the number of fields */
  rec_set_n_fields_old(rec, n_fields);

  /* Set the info bits of the record */
  rec_set_info_bits_old(rec, dtuple_get_info_bits(dtuple) & REC_INFO_BITS_MASK);

  /* Store the data and the offsets */

  end_offset = 0;

  if (!n_ext && data_size <= REC_1BYTE_OFFS_LIMIT) {

    rec_set_1byte_offs_flag(rec, true);

    for (i = 0; i < n_fields; i++) {

      field = dtuple_get_nth_field(dtuple, i);

      if (dfield_is_null(field)) {
        len = dtype_get_sql_null_size(dfield_get_type(field), 0);
        data_write_sql_null(rec + end_offset, len);

        end_offset += len;
        ored_offset = end_offset | REC_1BYTE_SQL_NULL_MASK;
      } else {
        /* If the data is not SQL null, store it */
        len = dfield_get_len(field);

        memcpy(rec + end_offset, dfield_get_data(field), len);

        end_offset += len;
        ored_offset = end_offset;
      }

      rec_1_set_field_end_info(rec, i, ored_offset);
    }
  } else {
    rec_set_1byte_offs_flag(rec, false);

    for (i = 0; i < n_fields; i++) {

      field = dtuple_get_nth_field(dtuple, i);

      if (dfield_is_null(field)) {
        len = dtype_get_sql_null_size(dfield_get_type(field), 0);
        data_write_sql_null(rec + end_offset, len);

        end_offset += len;
        ored_offset = end_offset | REC_2BYTE_SQL_NULL_MASK;
      } else {
        /* If the data is not SQL null, store it */
        len = dfield_get_len(field);

        memcpy(rec + end_offset, dfield_get_data(field), len);

        end_offset += len;
        ored_offset = end_offset;

        if (dfield_is_ext(field)) {
          ored_offset |= REC_2BYTE_EXTERN_MASK;
        }
      }

      rec_2_set_field_end_info(rec, i, ored_offset);
    }
  }

  return (rec);
}

void rec_convert_dtuple_to_rec_comp(rec_t *rec, ulint extra, const dict_index_t *index, ulint status, const dfield_t *fields, ulint n_fields) {
  const dfield_t *field;
  const dtype_t *type;
  byte *end;
  byte *nulls;
  byte *lens;
  ulint len;
  ulint i;
  ulint n_node_ptr_field;
  ulint fixed_len;
  ulint null_mask = 1;
  ut_ad(extra == 0 || dict_table_is_comp(index->table));
  ut_ad(extra == 0 || extra == REC_N_NEW_EXTRA_BYTES);
  ut_ad(n_fields > 0);

  switch (expect(status, REC_STATUS_ORDINARY)) {
    case REC_STATUS_ORDINARY:
      ut_ad(n_fields <= dict_index_get_n_fields(index));
      n_node_ptr_field = ULINT_UNDEFINED;
      break;
    case REC_STATUS_NODE_PTR:
      ut_ad(n_fields == dict_index_get_n_unique_in_tree(index) + 1);
      n_node_ptr_field = n_fields - 1;
      break;
    case REC_STATUS_INFIMUM:
    case REC_STATUS_SUPREMUM:
      ut_ad(n_fields == 1);
      n_node_ptr_field = ULINT_UNDEFINED;
      break;
    default:
      ut_error;
      return;
  }

  end = rec;
  nulls = rec - (extra + 1);
  lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);
  /* clear the SQL-null flags */
  memset(lens + 1, 0, nulls - lens);

  /* Store the data and the offsets */

  for (i = 0, field = fields; i < n_fields; i++, field++) {
    const dict_field_t *ifield;

    type = dfield_get_type(field);
    len = dfield_get_len(field);

    if (unlikely(i == n_node_ptr_field)) {
      ut_ad(dtype_get_prtype(type) & DATA_NOT_NULL);
      ut_ad(len == 4);
      memcpy(end, dfield_get_data(field), len);
      end += 4;
      break;
    }

    if (!(dtype_get_prtype(type) & DATA_NOT_NULL)) {
      /* nullable field */
      ut_ad(index->n_nullable > 0);

      if (unlikely(!(byte)null_mask)) {
        nulls--;
        null_mask = 1;
      }

      ut_ad(*nulls < null_mask);

      /* set the null flag if necessary */
      if (dfield_is_null(field)) {
        *nulls |= null_mask;
        null_mask <<= 1;
        continue;
      }

      null_mask <<= 1;
    }
    /* only nullable fields can be null */
    ut_ad(!dfield_is_null(field));

    ifield = dict_index_get_nth_field(index, i);
    fixed_len = ifield->fixed_len;
    /* If the maximum length of a variable-length field
    is up to 255 bytes, the actual length is always stored
    in one byte. If the maximum length is more than 255
    bytes, the actual length is stored in one byte for
    0..127.  The length will be encoded in two bytes when
    it is 128 or more, or when the field is stored externally. */
    if (fixed_len) {
      ut_ad(len == fixed_len);
      ut_ad(!dfield_is_ext(field));
    } else if (dfield_is_ext(field)) {
      ut_ad(ifield->col->len >= 256 || ifield->col->mtype == DATA_BLOB);
      ut_ad(len <= REC_MAX_INDEX_COL_LEN + BTR_EXTERN_FIELD_REF_SIZE);
      *lens-- = (byte)(len >> 8) | 0xc0;
      *lens-- = (byte)len;
    } else {
      ut_ad(len <= dtype_get_len(type) || dtype_get_mtype(type) == DATA_BLOB || dtype_get_mtype(type) == DATA_DECIMAL);
      if (len < 128 || (dtype_get_len(type) < 256 && dtype_get_mtype(type) != DATA_BLOB)) {

        *lens-- = (byte)len;
      } else {
        ut_ad(len < 16384);
        *lens-- = (byte)(len >> 8) | 0x80;
        *lens-- = (byte)len;
      }
    }

    memcpy(end, dfield_get_data(field), len);
    end += len;
  }
}

/**
 * Builds a new-style physical record out of a data tuple and
 * stores it beginning from the start of the given buffer.
 * 
 * @param[in] buf start address of the physical record
 * @param[in] index record descriptor
 * @param[in] dtuple data tuple
 * @return	pointer to the origin of physical record
 */
static rec_t *rec_convert_dtuple_to_rec_new(byte *buf, const dict_index_t *index, const dtuple_t *dtuple) {
  ulint extra_size;
  ulint status;
  rec_t *rec;

  status = dtuple_get_info_bits(dtuple) & REC_NEW_STATUS_MASK;
  rec_get_converted_size_comp(index, status, dtuple->fields, dtuple->n_fields, &extra_size);
  rec = buf + extra_size;

  rec_convert_dtuple_to_rec_comp(rec, REC_N_NEW_EXTRA_BYTES, index, status, dtuple->fields, dtuple->n_fields);

  /* Set the info bits of the record */
  rec_set_info_and_status_bits(rec, dtuple_get_info_bits(dtuple));

  return rec;
}

rec_t *rec_convert_dtuple_to_rec(byte *buf, const dict_index_t *index, const dtuple_t *dtuple, ulint n_ext) {
  rec_t *rec;

  ut_ad(buf && index && dtuple);
  ut_ad(dtuple_validate(dtuple));
  ut_ad(dtuple_check_typed(dtuple));

  if (dict_table_is_comp(index->table)) {
    rec = rec_convert_dtuple_to_rec_new(buf, index, dtuple);
  } else {
    rec = rec_convert_dtuple_to_rec_old(buf, dtuple, n_ext);
  }

#ifdef UNIV_DEBUG
  {
    mem_heap_t *heap = nullptr;
    ulint offsets_[REC_OFFS_NORMAL_SIZE];
    const ulint *offsets;
    ulint i;
    rec_offs_init(offsets_);

    offsets = rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED, &heap);
    ut_ad(rec_validate(rec, offsets));
    ut_ad(dtuple_get_n_fields(dtuple) == rec_offs_n_fields(offsets));

    for (i = 0; i < rec_offs_n_fields(offsets); i++) {
      ut_ad(!dfield_is_ext(dtuple_get_nth_field(dtuple, i)) == !rec_offs_nth_extern(offsets, i));
    }

    if (likely_null(heap)) {
      mem_heap_free(heap);
    }
  }
#endif /* UNIV_DEBUG */
  return (rec);
}

void rec_copy_prefix_to_dtuple(dtuple_t *tuple, const rec_t *rec, const dict_index_t *index, ulint n_fields, mem_heap_t *heap) {
  ulint i;
  ulint offsets_[REC_OFFS_NORMAL_SIZE];
  ulint *offsets = offsets_;
  rec_offs_init(offsets_);

  offsets = rec_get_offsets(rec, index, offsets, n_fields, &heap);

  ut_ad(rec_validate(rec, offsets));
  ut_ad(dtuple_check_typed(tuple));

  dtuple_set_info_bits(tuple, rec_get_info_bits(rec, dict_table_is_comp(index->table)));

  for (i = 0; i < n_fields; i++) {
    dfield_t *field;
    const byte *data;
    ulint len;

    field = dtuple_get_nth_field(tuple, i);
    data = rec_get_nth_field(rec, offsets, i, &len);

    if (len != UNIV_SQL_NULL) {
      dfield_set_data(field, mem_heap_dup(heap, data, len), len);
      ut_ad(!rec_offs_nth_extern(offsets, i));
    } else {
      dfield_set_null(field);
    }
  }
}

/**
 * Copies the first n fields of an old-style physical record
 * to a new physical record in a buffer.
 * @param[in] rec                  Physical record
 * @param[in] n_fields             Number of fields to copy
 * @param[in] area_end             End of the prefix data
 * @param[out] buf                 Memory buffer for the copied prefix, or NULL.
 *                                 Memory wll be allocated using std::new, free using delete [] buf.
 * @param[in] buf_size             Buffer size
 * @return	own: copied record
 */
static rec_t *rec_copy_prefix_to_buf_old(const rec_t *rec, ulint n_fields, ulint area_end, byte *&buf, ulint &buf_size) {
  ulint area_start;

  if (rec_get_1byte_offs_flag(rec)) {
    area_start = REC_N_OLD_EXTRA_BYTES + n_fields;
  } else {
    area_start = REC_N_OLD_EXTRA_BYTES + 2 * n_fields;
  }

  auto prefix_len = area_start + area_end;

  if (buf == nullptr || buf_size < prefix_len) {
    if (buf != nullptr) {
      mem_free(buf);
    }

    buf = static_cast<byte *>(mem_alloc2(prefix_len, &buf_size));
  }

  memcpy(buf, rec - area_start, prefix_len);

  auto copy_rec = buf + area_start;

  rec_set_n_fields_old(copy_rec, n_fields);

  return copy_rec;
}

rec_t *rec_copy_prefix_to_buf(const rec_t *rec, const dict_index_t *index, ulint n_fields, byte *&buf, ulint &buf_size) {
  prefetch_rw(buf);

  if (!dict_table_is_comp(index->table)) {
    ut_ad(rec_validate_old(rec));
    return rec_copy_prefix_to_buf_old(rec, n_fields, rec_get_field_start_offs(rec, n_fields), buf, buf_size);
  }

  auto status = rec_get_status(rec);

  switch (status) {
    case REC_STATUS_ORDINARY:
      ut_ad(n_fields <= dict_index_get_n_fields(index));
      break;
    case REC_STATUS_NODE_PTR:
      /* it doesn't make sense to copy the child page number field */
      ut_ad(n_fields <= dict_index_get_n_unique_in_tree(index));
      break;
    case REC_STATUS_INFIMUM:
    case REC_STATUS_SUPREMUM:
      /* infimum or supremum record: no sense to copy anything */
    default:
      ut_error;
      return nullptr;
  }

  auto nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
  auto lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);

  prefetch_r(lens);

  ulint prefix_len{};
  ulint null_mask{1};

  /* Read the lengths of fields 0..n */
  for (ulint i = 0; i < n_fields; i++) {
    const auto field = dict_index_get_nth_field(index, i);
    const auto col = dict_field_get_col(field);

    if (!(col->prtype & DATA_NOT_NULL)) {
      /* nullable field => read the null flag */
      if (unlikely(!(byte)null_mask)) {
        nulls--;
        null_mask = 1;
      }

      if (*nulls & null_mask) {
        null_mask <<= 1;
        continue;
      }

      null_mask <<= 1;
    }

    if (field->fixed_len) {
      prefix_len += field->fixed_len;
    } else {
      ulint len = *lens--;
      /* If the maximum length of the column is up
      to 255 bytes, the actual length is always
      stored in one byte. If the maximum length is
      more than 255 bytes, the actual length is
      stored in one byte for 0..127.  The length
      will be encoded in two bytes when it is 128 or
      more, or when the column is stored externally. */
      if (col->len > 255 || col->mtype == DATA_BLOB) {
        if (len & 0x80) {
          /* 1exxxxxx */
          len &= 0x3f;
          len <<= 8;
          len |= *lens--;
          prefetch_r(lens);
        }
      }
      prefix_len += len;
    }
  }

  prefetch_r(rec + prefix_len);

  prefix_len += rec - (lens + 1);

  if (buf == nullptr || buf_size < prefix_len) {
    if (buf != nullptr) {
      mem_free(buf);
    }

    buf = static_cast<byte *>(mem_alloc2(prefix_len, &buf_size));
  }

  memcpy(buf, lens + 1, prefix_len);

  return buf + (rec - (lens + 1));
}

/**
 * Validates the consistency of an old-style physical record.
 * 
 * @param[in] rec  Record to validate.
 * 
 * @return	true if ok
 */
static bool rec_validate_old(const rec_t *rec) {
  ulint len_sum = 0;

  auto n_fields = rec_get_n_fields_old(rec);

  if (n_fields == 0 || n_fields > REC_MAX_N_FIELDS) {
    ib_logger(ib_stream, "Error: record has %lu fields\n", (ulong)n_fields);
    return false;
  }

  for (ulint i = 0; i < n_fields; i++) {
    ulint len;
    auto data = rec_get_nth_field_old(rec, i, &len);

    if (!(len < UNIV_PAGE_SIZE || len == UNIV_SQL_NULL)) {
      log_err(std::format("Record field {} len {}", i, len));
      return false;
    }

    if (len != UNIV_SQL_NULL) {
      ulint sum{};

      len_sum += len;

      /* Dereference the end of the field to cause a memory trap if possible */
      sum += *(data + len - 1);
      (void)sum;

    } else {
      len_sum += rec_get_nth_field_size(rec, i);
    }
  }

  if (len_sum != rec_get_data_size_old(rec)) {
    log_err(std::format("Record len should be {}, len {}", len_sum, rec_get_data_size_old(rec)));
    return false;
  } else {
    return true;
  }
}

bool rec_validate(const rec_t *rec, const ulint *offsets) {
  ulint len_sum = 0;
  auto n_fields = rec_offs_n_fields(offsets);

  if (n_fields == 0 || n_fields > REC_MAX_N_FIELDS) {
    ib_logger(ib_stream, "Error: record has %lu fields\n", (ulong)n_fields);
    return false;
  }

  ut_a(rec_offs_comp(offsets) || n_fields <= rec_get_n_fields_old(rec));

  for (ulint i = 0; i < n_fields; i++) {
    ulint len;
    auto data = rec_get_nth_field(rec, offsets, i, &len);

    if (!((len < UNIV_PAGE_SIZE) || (len == UNIV_SQL_NULL))) {
      ib_logger(ib_stream, "Error: record field %lu len %lu\n", (ulong)i, (ulong)len);
      return false;
    }

    if (len != UNIV_SQL_NULL) {
      ulint sum{};

      len_sum += len;

      /* Dereference the end of the field to cause a memory trap if possible */
      sum += *(data + len - 1);
      (void)sum;

    } else if (!rec_offs_comp(offsets)) {
      len_sum += rec_get_nth_field_size(rec, i);
    }
  }

  if (len_sum != rec_offs_data_size(offsets)) {
    log_err("Record len should be %lu, len %lu\n", len_sum, rec_offs_data_size(offsets));
    return false;
  }

  if (!rec_offs_comp(offsets)) {
    ut_a(rec_validate_old(rec));
  }

  return true;
}

std::ostream &rec_print_old(std::ostream &os, const rec_t *rec) {
  auto n = rec_get_n_fields_old(rec);

  os << std::format("PHYSICAL RECORD: n_fields {}; {}-byte offsets; info bits {}\n",
		    n,
		    (rec_get_1byte_offs_flag(rec) ? 1 : 2),
		    rec_get_info_bits(rec, false));

  for (ulint i = 0; i < n; i++) {
    ulint len;
    auto data = rec_get_nth_field_old(rec, i, &len);

    os << std::format(" {}:", i);

    if (len != UNIV_SQL_NULL) {
      ut_print_buf(os, data, std::max(len, 30UL));
      os << std::format("; len: {} ", len);
    } else {
      os << std::format(" SQL NULL, size {} ", rec_get_nth_field_size(rec, i));
    }

    os << ";\n";
  }

  rec_validate_old(rec);

  return os;
}

void rec_print_old(ib_stream_t ib_stream, const rec_t *rec) {
  std::ostringstream os{};

  rec_print_old(os, rec);

  ib_logger(ib_stream, "%s", os.str().c_str());
}

std::ostream &rec_print_comp(std::ostream &os, const rec_t *rec, const ulint *offsets) {
  for (ulint i = 0; i < rec_offs_n_fields(offsets); i++) {
    ulint len;
    auto data = rec_get_nth_field(rec, offsets, i, &len);

    os << std::format(" {}:", i);

    if (len != UNIV_SQL_NULL) {
      ut_print_buf(os, data, std::max(len, 30UL));
      os << std::format("; len: {} ", len);
    } else {
      os << " SQL NULL";
    }

    os << ";\n";
  }

  return os;
}

void rec_print_comp(ib_stream_t ib_stream, const rec_t *rec, const ulint *offsets) {
  std::stringstream os{};

  rec_print_comp(os, rec, offsets);

  ib_logger(ib_stream, "%s", os.str().c_str());
}

std::ostream &rec_print_new(std::ostream& os, const rec_t *rec, const ulint *offsets) {
  ut_a(rec_offs_comp(offsets));
  ut_ad(rec_offs_validate(rec, nullptr, offsets));

  os << std::format("PHYSICAL RECORD: n_fields ; {} compact format; info bits {}",
		    rec_offs_n_fields(offsets), rec_get_info_bits(rec, true));

  rec_print_comp(os, rec, offsets);
  rec_validate(rec, offsets);

  return os;
}

void rec_print_new(ib_stream_t ib_stream, const rec_t *rec, const ulint *offsets) {
  std::ostringstream os{};

  rec_print_comp(os, rec, offsets);
  ib_logger(ib_stream, "%s", os.str().c_str());
}

std::ostream &rec_print(std::ostream &os, const rec_t *rec, dict_index_t *index) {
  if (!dict_table_is_comp(index->table)) {
    rec_print_old(os, rec);
  } else {
    mem_heap_t *heap{};
    ulint offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);

    rec_print_new(os, rec, rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED, &heap));

    if (likely_null(heap)) {
      mem_heap_free(heap);
    }
  }

  return os;
}

void rec_print(ib_stream_t ib_stream, const rec_t *rec, dict_index_t *index) {
  std::ostringstream os{};

  rec_print(os, rec, index);
  ib_logger(ib_stream, "%s\n", os.str().c_str());
}
