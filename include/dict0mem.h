/****************************************************************************
Copyright (c) 1996, 2009, Innobase Oy. All Rights Reserved.

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

/** @file include/dict0mem.h
Data dictionary memory object creation

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#pragma once

#include "btr0types.h"
#include "data0type.h"
#include "dict0types.h"
#include "hash0hash.h"
#include "innodb0types.h"
#include "lock0types.h"
#include "mem0mem.h"
#include "que0types.h"
#include "rem0types.h"
#include "sync0rw.h"
#include "trx0types.h"
#include "ut0byte.h"
#include "ut0lst.h"
#include "ut0mem.h"
#include "ut0rnd.h"

/** Number of flag bits */
constexpr ulint DICT_TF_BITS = 6;

/** Type flags of an index: OR'ing of the flags is allowed to define a
combination of types */
/* @{ */

/** Clustered index */
constexpr ulint DICT_CLUSTERED = 1;

/** Unique index */
constexpr ulint DICT_UNIQUE = 2;

/* @} */

/** Types for a table object */

/** ordinary table */
constexpr ulint DICT_TABLE_ORDINARY = 1;

/** Table flags.  All unused bits must be 0. */
/* @{ */

/** Compact page format.  This must be set for new file formats (later than
 * DICT_TF_FORMAT_51). */
constexpr ulint DICT_TF_COMPACT = 1;

/* @} */

/** File format */
/* @{ */

/** File format */
constexpr ulint DICT_TF_FORMAT_SHIFT = 5;

constexpr ulint DICT_TF_FORMAT_MASK = ((~(~0UL << (DICT_TF_BITS - DICT_TF_FORMAT_SHIFT))) << DICT_TF_FORMAT_SHIFT);

/** InnoDBL up to 0.1 */
constexpr ulint DICT_TF_FORMAT_51 = 0;

/** InnoDB 0.1: new BLOB treatment */
constexpr ulint DICT_TF_FORMAT_V1 = 1;

/** Maximum supported file format */
constexpr ulint DICT_TF_FORMAT_MAX = DICT_TF_FORMAT_V1;

/* @} */

static_assert(
  (1 << (DICT_TF_BITS - DICT_TF_FORMAT_SHIFT)) > DICT_TF_FORMAT_MAX, "error DICT_TF_BITS is insufficient for DICT_TF_FORMAT_MAX"
);

/* @} */

/** @brief Additional table flags.

These flags will be stored in SYS_TABLES.MIX_LEN.  All unused flags
will be written as 0.  The column may contain garbage for tables
created with old versions of InnoDB that only implemented ROW_FORMAT=REDUNDANT.
*/

/* @{ */
constexpr ulint DICT_TF2_SHIFT = DICT_TF_BITS;

/** Shift value for table->flags. */
/** true for tables from CREATE TEMPORARY TABLE. */
constexpr ulint DICT_TF2_TEMPORARY = 1;

/** Total number of bits in table->flags. */
constexpr ulint DICT_TF2_BITS = DICT_TF2_SHIFT + 1;

/* @} */

/** Creates a table memory object.
@return	own: table object */
dict_table_t *dict_mem_table_create(
  const char *name, /*!< in: table name */
  ulint space,      /*!< in: space where the clustered index
                                        of the table is placed; this parameter
                                        is ignored if the table is made
                                        a member of a cluster */
  ulint n_cols,     /*!< in: number of columns */
  ulint flags
); /*!< in: table flags */

/** Free a table memory object. */
void dict_mem_table_free(dict_table_t *table); /*!< in: table */

/** Adds a column definition to a table. */
void dict_mem_table_add_col(
  dict_table_t *table, /*!< in: table */
  mem_heap_t *heap,    /*!< in: temporary memory heap, or NULL */
  const char *name,    /*!< in: column name, or NULL */
  ulint mtype,         /*!< in: main datatype */
  ulint prtype,        /*!< in: precise type */
  ulint len
); /*!< in: precision */

/** Creates an index memory object.
@return	own: index object */
dict_index_t *dict_mem_index_create(
  const char *table_name, /*!< in: table name */
  const char *index_name, /*!< in: index name */
  ulint space,            /*!< in: space where the index tree is
                                       placed, ignored if the index is of
                                       the clustered type */
  ulint type,             /*!< in: DICT_UNIQUE,
                                       DICT_CLUSTERED, ... ORed */
  ulint n_fields
); /*!< in: number of fields */

/** Adds a field definition to an index. NOTE: does not take a copy
of the column name if the field is a column. The memory occupied
by the column name may be released only after publishing the index. */
void dict_mem_index_add_field(
  dict_index_t *index, /*!< in: index */
  const char *name,    /*!< in: column name */
  ulint prefix_len
); /*!< in: 0 or the column prefix
                                                 length in a column prefix index
                                                 like INDEX (textcol(25)) */

/** Frees an index memory object. */
void dict_mem_index_free(dict_index_t *index); /*!< in: index */

/** Creates and initializes a foreign constraint memory object.
@return	own: foreign constraint struct */
dict_foreign_t *dict_mem_foreign_create(void);

/** Data structure for a column in a table */
struct dict_col_struct {
  /* @{ */
  DTYPE_FIELDS
  /* @} */

  /** Table column position (starting from 0) */
  unsigned ind : 10;

  /** nonzero if this column appears in the ordering fields of an index */
  unsigned ord_part : 1;
};

/** @brief DICT_MAX_INDEX_COL_LEN is measured in bytes and is the maximum
indexed column length (or indexed prefix length).

It is set to 3*256, so that one can create a column prefix index on
256 characters of a TEXT or VARCHAR column also in the UTF-8
charset. In that charset, a character may take at most 3 bytes.  This
constant MUST NOT BE CHANGED, or the compatibility of InnoDB data
files would be at risk! */
constexpr auto DICT_MAX_INDEX_COL_LEN = REC_MAX_INDEX_COL_LEN;

#ifdef UNIV_DEBUG
/** Value of dict_index_struct::magic_n */
const ulint DICT_INDEX_MAGIC_N = 76789786;
#endif /* UNIV_DEBUG */

/** Data structure for a field in an index */
struct dict_field_struct {
  /** pointer to the table column */
  dict_col_t *col;

  /** Name of the column */
  const char *name;

  /** 0 or the length of the column prefix in bytes e.g., for
  INDEX (textcol(25)); must be smaller than
  DICT_MAX_INDEX_COL_LEN; NOTE that in the UTF-8 charset,
  MySQL sets this to 3 * the prefix len in UTF-8 chars */
  unsigned prefix_len : 10;

  /** 0 or the fixed length of the column if smaller than
  DICT_MAX_INDEX_COL_LEN */
  unsigned fixed_len : 10;
};

/** Data structure for an index.  Most fields will be
initialized to 0, NULL or false in dict_mem_index_create(). */
struct dict_index_struct {
  /** Id of the index */
  uint64_t id;

  /** memory heap */
  mem_heap_t *heap;

  /** index name */
  const char *name;

  /** table name */
  const char *table_name;

  /** back pointer to table */
  dict_table_t *table;

  /** Space where the index tree is placed */
  unsigned space : 32;

  /** index tree root page number */
  unsigned page : 32;

  /** index type (DICT_CLUSTERED, DICT_UNIQUE, DICT_UNIVERSAL) */
  unsigned type : 4;

  /* position of the trx id column in a clustered index
  record, if the fields before it are known to be of a
  fixed size, 0 otherwise */
  unsigned trx_id_offset : 10;

  /** Number of columns the user defined to be in the
  index: in the internal representation we add more columns */
  unsigned n_user_defined_cols : 10;

  /** Number of fields from the beginning which are enough to determine
  an index entry uniquely */
  unsigned n_uniq : 10;

  /** Number of fields defined so far */
  unsigned n_def : 10;

  /** number of fields in the index */
  unsigned n_fields : 10;

  /** number of nullable fields */
  unsigned n_nullable : 10;

  /** true if the index object is in the dictionary cache */
  unsigned cached : 1;

  /** true if this index is marked to be dropped in
   * ha_innobase::prepare_drop_index(), otherwise false */
  unsigned to_be_dropped : 1;

  /** Array of field descriptions */
  dict_field_t *fields;

  /* List of indexes of the table */
  UT_LIST_NODE_T(dict_index_t) indexes;

  /** Statistics for query optimization */

  /* @{ */

  /* Approximate number of different key values for this index, for
  each n-column prefix where n <= dict_get_n_unique(index); we
  periodically calculate new estimates */
  int64_t *stat_n_diff_key_vals;

  /** approximate index size in database pages */
  ulint stat_index_size;

  /** Approximate number of leaf pages in the index tree */
  ulint stat_n_leaf_pages;

  /** read-write lock protecting the upper levels of the index tree */
  rw_lock_t lock;

  /** Client compare context. For use defined column types and BLOBs
  the client is responsible for comparing the column values. This field
  is the argument for the callback compare function. */
  void *cmp_ctx;

  /** Id of the transaction that created this index, or 0 if the index existed
   * when InnoDB was started up */
  uint64_t trx_id;
  /* @} */

#ifdef UNIV_DEBUG
  /** Magic number */
  ulint magic_n;
#endif
};

/** Data structure for a foreign key constraint; an example:
FOREIGN KEY (A, B) REFERENCES TABLE2 (C, D).  Most fields will be
initialized to 0, NULL or false in dict_mem_foreign_create(). */
struct dict_foreign_struct {
  /** This object is allocated from this memory heap */
  mem_heap_t *heap;

  /** id of the constraint as a null-terminated string */
  char *id;

  /** Number of indexes' first fields for which the foreign
  key constraint is defined: we allow the indexes to contain
  more fields than mentioned in the constraint, as long as
  the first fields are as mentioned */
  unsigned n_fields : 10;

  /** 0 or DICT_FOREIGN_ON_DELETE_CASCADE or DICT_FOREIGN_ON_DELETE_SET_NULL */
  unsigned type : 6;

  /** foreign table name */
  char *foreign_table_name;

  /** Table where the foreign key is */
  dict_table_t *foreign_table;

  /** Names of the columns in the foreign key */
  const char **foreign_col_names;

  /** Referenced table name */
  char *referenced_table_name;

  /** Table where the referenced key is */
  dict_table_t *referenced_table;

  /** Names of the referenced columns in the referenced table */
  const char **referenced_col_names;

  /** Foreign index; we require that both tables contain explicitly
  defined indexes for the constraint: InnoDB does not generate new
  indexes implicitly */
  dict_index_t *foreign_index;

  /** Referenced index */
  dict_index_t *referenced_index;

  /** List node for foreign keys of the table */
  UT_LIST_NODE_T(dict_foreign_t) foreign_list;

  /** List node for referenced keys of the table */
  UT_LIST_NODE_T(dict_foreign_t) referenced_list;
};

/** The flags for ON_UPDATE and ON_DELETE can be ORed; the default is that
a foreign key constraint is enforced, therefore RESTRICT just means no flag */
/* @{ */
/** On delete cascade */
constexpr ulint DICT_FOREIGN_ON_DELETE_CASCADE = 1;

/** On update set null */
constexpr ulint DICT_FOREIGN_ON_DELETE_SET_NULL = 2;

/** On delete cascade */
constexpr ulint DICT_FOREIGN_ON_UPDATE_CASCADE = 4;

/** On update set null */
constexpr ulint DICT_FOREIGN_ON_UPDATE_SET_NULL = 8;

/** On delete no action */
constexpr ulint DICT_FOREIGN_ON_DELETE_NO_ACTION = 16;

/** On update no action */
constexpr ulint DICT_FOREIGN_ON_UPDATE_NO_ACTION = 32;
/* @} */

#ifdef UNIV_DEBUG
constexpr ulint DICT_TABLE_MAGIC_N = 76333786;
#endif /* UNIV_DEBUG */

/** Data structure for a database table.  Most fields will be
initialized to 0, NULL or false in dict_mem_table_create(). */
struct dict_table_struct {
  /** Id of the table */
  uint64_t id;

  /** Memory heap */
  mem_heap_t *heap;

  /** Table name */
  const char *name;

  /** NULL or the directory path where a TEMPORARY table that was explicitly
  created by a user should be placed if innodb_file_per_table is defined; in
  Unix this is usually /tmp/... */
  const char *dir_path_of_temp_table;

  /*!< space where the clustered index of the
  table is placed */
  unsigned space : 32;

  unsigned flags : DICT_TF2_BITS; /*!< DICT_TF_COMPACT, ... */

  /** true if this is in a single-table tablespace and the .ibd file is
  missing; then we must return in ha_innodb.cc an error if the user tries
  to query such an orphaned table */
  unsigned ibd_file_missing : 1;

  /** This flag is set true when the user calls DISCARD TABLESPACE on this
  table, and reset to false in IMPORT TABLESPACE */
  unsigned tablespace_discarded : 1;

  /** true if the table object has been added to the dictionary cache */
  unsigned cached : 1;

  /** Number of columns defined so far */
  unsigned n_def : 10;

  /** Number of columns */
  unsigned n_cols : 10;

  /** Array of column descriptions */
  dict_col_t *cols;

  /** Column names packed in a character string "name1\0name2\0...nameN\0".
  Until the string contains n_cols, it will be allocated from a temporary heap.
  The final string will be allocated from table->heap. */
  const char *col_names;

  /** Hash chain node */
  hash_node_t name_hash;

  /** Hash chain node */
  hash_node_t id_hash;

  /** List of indexes of the table */
  UT_LIST_BASE_NODE_T(dict_index_t, indexes) indexes;

  /** List of foreign key constraints in the table; these refer to columns
  in other tables */
  UT_LIST_BASE_NODE_T(dict_foreign_t, foreign_list) foreign_list;

  /** List of foreign key constraints which refer to this table */
  UT_LIST_BASE_NODE_T(dict_foreign_t, referenced_list) referenced_list;

  /** Node of the LRU list of tables */
  UT_LIST_NODE_T(dict_table_t) table_LRU;

  /** Count of how many handles the user has opened to this table; dropping
  of the table is NOT allowed until this count gets to zero */
  ulint n_handles_opened;

  /** Count of how many foreign key check operations are currently being performed
  on the table: we cannot drop the table while there are foreign key checks running
  on it! */
  ulint n_foreign_key_checks_running;

  /** List of locks on the table */
  UT_LIST_BASE_NODE_T_EXTERN(lock_t, trx_locks) locks;

#ifdef UNIV_DEBUG
  /** This field is used to specify in simulations tables which are so big
  that disk should be accessed: disk access is simulated by putting the
  thread to sleep for a while; NOTE that this flag is not stored to the data
  dictionary on disk, and the database will forget about value true if it has
  to reload the table definition from disk */
  bool does_not_fit_in_memory;
#endif /* UNIV_DEBUG */

  /** flag: true if the maximum length of a single row exceeds BIG_ROW_SIZE;
  initialized in dict_table_add_to_cache() */
  unsigned big_rows : 1;

  /** Statistics for query optimization */
  /* @{ */
  /** true if statistics have been calculated the first time after database
  startup or table creation */
  unsigned stat_initialized : 1;

  /** Approximate number of rows in the table; we periodically calculate
  new estimates */
  int64_t stat_n_rows;

  /** Approximate clustered index size in database pages */
  ulint stat_clustered_index_size;

  /** Other indexes in database pages */
  ulint stat_sum_of_other_index_sizes;

  /** When a row is inserted, updated, or deleted, we add 1 to this
  number; we calculate new estimates for the stat_...  values for
  the table and the indexes at an interval of 2 GB or when about 1 / 16
  of table has been modified; also when an estimate operation is called
  for; the counter is reset to zero at statistics calculation; this
  counter is not protected by any latch, because this is only used
  for heuristics */
  ulint stat_modified_counter;
  /* @} */

#ifdef UNIV_DEBUG
  /** Value of dict_table_struct::magic_n */
  ulint magic_n;
#endif /* UNIV_DEBUG */
};
