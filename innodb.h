/***********************************************************************
Copyright (c) 2008, 2009 Innobase Oy. All rights reserved.
Copyright (c) 2008, 2009 Oracle. All rights reserved.
Copyright (c) 2010 Stewart Smith 
Copyright (c) 2024 Sunny Bains

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

************************************************************************/

/*!< @file innodb.h */

#pragma once

#include <cstdint>
#include <cstdio>

#include <functional>
#include <vector>

struct ib_trx_struct;
struct ib_crsr_struct;
struct ib_tpl_struct;
struct ib_tbl_sch_struct;
struct ib_idx_sch_struct;

/** Type of callback in the event of InnoDB panicing. Your callback should
 *  call exit() rather soon, as continuing after a panic will lead to errors
 *  returned from every API function. We have also not fully tested
 *  every possible outcome from not immediately calling exit().*/
typedef void (*ib_panic_handler_t)(void*, int, char*, ...);

/** The first argument to the InnoDB message logging function. By default
 * it's set to stderr. You should treat ib_msg_stream_t as a void*, since
 * it will probably change in the future. */
using ib_msg_stream_t = FILE*;

/** All log messages are written to this function.It should have the same
 * behavior as fprintf(3). */
typedef int (*ib_msg_log_t)(ib_msg_stream_t, const char*, ...);

/** \enum db_err InnoDB error codes.
 * Most of the error codes are internal to the engine
 * and will not be seen by user applications. The partial error codes reflect
 * the sub-state of an operation within InnoDB. Some of the error codes are
 * deprecated and are no longer used. */
enum db_err {
  /** A successult result */
  DB_SUCCESS = 10,    

  /* We can't proceed, internal error. */
  DB_PANIC,

  /* The following are error codes */

  /** This is a generic error code. It is used to classify
  error conditions that can't be represented by other codes */
  DB_ERROR,      

  /** An operation was interrupted by a user. */
  DB_INTERRUPTED,      

  /** Operation caused an out of memory error. Within InnoDB
  core code this is normally a fatal error */
  DB_OUT_OF_MEMORY,    

  /** The operating system returned an out of file space error
   when trying to do an IO operation. */
  DB_OUT_OF_FILE_SPACE,    

  /** A lock request by transaction resulted in a lock wait.
  The thread is suspended internally by InnoDB and is put on a
         lock wait queue. */
  DB_LOCK_WAIT,      

  /** A lock request by a transaction resulted in a deadlock.
  The transaction was rolled back */
  DB_DEADLOCK,      

  /*!< Not used */
  DB_ROLLBACK,      

  /** A record insert or update violates a unique contraint. */
  DB_DUPLICATE_KEY,    
  
  /** A query thread should be in state suspended but is
  trying to acquire a lock. Currently this is treated as a hard
  error and a violation of an invariant. */
  DB_QUE_THR_SUSPENDED,    

  /** Required history data has been deleted due to lack of space
  in rollback segment */
  DB_MISSING_HISTORY,    

  /** This error is not used */
  DB_CLUSTER_NOT_FOUND = 30,

  /** The table could not be found */
  DB_TABLE_NOT_FOUND,    

  /** The database has to be stopped and restarted with more
  file space */
  DB_MUST_GET_MORE_FILE_SPACE,  

  /** The user is trying to create a table in the InnoDB data
  dictionary but a table with that name already exists */
  DB_TABLE_EXISTS,    

  /** A record in an index would not fit on a compressed page,
  or it would become bigger than 1/2 free space in an uncompressed
  page frame */
  DB_TOO_BIG_RECORD,    

  /** Lock wait lasted too long */
  DB_LOCK_WAIT_TIMEOUT,    

  /** Referenced key value not found fonnic_r a foreign key in an
  insert or update of a row */
  DB_NO_REFERENCED_ROW,    

  /** Cannot delete or update a row because it contains a key
  value which is referenced */
  DB_ROW_IS_REFERENCED,    

  /** Adding a foreign key constraint to a table failed */
  DB_CANNOT_ADD_CONSTRAINT,  

  /** Data structure corruption noticed */
  DB_CORRUPTION,      

  /** InnoDB cannot handle an index where same column appears
  twice */
  DB_COL_APPEARS_TWICE_IN_INDEX,  

  /** Dropping a foreign key constraint from a table failed */
  DB_CANNOT_DROP_CONSTRAINT,  

  /** No savepoint exists with the given name */
  DB_NO_SAVEPOINT,    

  /** We cannot create a new single-table tablespace because a
  file of the same name already exists */
  DB_TABLESPACE_ALREADY_EXISTS,  

  /** Tablespace does not exist or is being dropped right now */
  DB_TABLESPACE_DELETED,    

  /** Lock structs have exhausted the buffer pool (for big
  transactions, InnoDB stores the lock structs in the buffer pool) */
  DB_LOCK_TABLE_FULL,    

  /** Foreign key constraints activated but the operation would
  lead to a duplicate key in some table */
  DB_FOREIGN_DUPLICATE_KEY,  

  /** When InnoDB runs out of the preconfigured undo slots,
  this can only happen when there are too many concurrent transactions */
  DB_TOO_MANY_CONCURRENT_TRXS,  

  /** When InnoDB sees any artefact or a feature that it can't
  recoginize or work with e.g., FT indexes created by a later version of the engine. */
  DB_UNSUPPORTED,      

  /** A column in the PRIMARY KEY was found to be nullptr */
  DB_PRIMARY_KEY_IS_NULL,    

  /** The application should clean up and quite ASAP. Fatal error,
  InnoDB cannot continue operation without risking database corruption. */
  DB_FATAL,      

  /* The following are partial failure codes */

  /** Partial failure code. */
  DB_FAIL = 1000,

  /** If an update or insert of a record doesn't fit in a Btree page */
  DB_OVERFLOW,      

  /** If an update or delete of a record causes a Btree page
  to be below a minimum threshold */
  DB_UNDERFLOW,      

  /** Failure to insert a secondary index entry to the insert buffer */
  DB_STRONG_FAIL,      

  /** Record not found */
  DB_RECORD_NOT_FOUND = 1500,

  /** A cursor operation or search operation scanned to the end
  of the index. */
  DB_END_OF_INDEX,    

  /* The following are API only error codes. */

  /** Generic schema error */
  DB_SCHEMA_ERROR = 2000,

  /** Column update or read failed because the types mismatch */
  DB_DATA_MISMATCH,    

  /** If an API function expects the schema to be locked in
  exclusive mode and if it's not then that API function will
  return this error code */
  DB_SCHEMA_NOT_LOCKED,    

  /** Generic error code for "Not found" type of errors */
  DB_NOT_FOUND,      

  /** Generic error code for "Readonly" type of errors */
  DB_READONLY,      

  /** Generic error code for "Invalid input" type of errors */
  DB_INVALID_INPUT,    

  /** Out of a resource, could be memory, threads, file descriptors etc. */
  DB_OUT_OF_RESOURCES,

  /** Index is corrupted. */
  DB_INDEX_CORRUPT,

  /** DDL is in progress. */
  DB_DDL_IN_PROGRESS,
};

using dberr_t = db_err;

/* Basic types used by the InnoDB API. */

/** All InnoDB error codes are represented by ib_err_t. See \ref db_err for
a complete list of possible error codes. */
using ib_err_t = dberr_t;

/** Representation of a byte within InnoDB */
using ib_byte_t = unsigned char;

/** Representation of an unsigned long int within InnoDB */
using ulint = unsigned long int;

static_assert(sizeof(ulint) == sizeof(uintptr_t), "ulint must be == sizeof(uintptr_t) bytes");

/** Representation of a void* within InnoDB */
using ib_opaque_t = void*;

/** A character set pointer */
using ib_charset_t = ib_opaque_t;

/** The integral type that represents internal table and index ids. */
using ib_id_t = uint64_t;

/** @enum ib_cfg_type_t Possible types for a configuration variable. */
enum ib_cfg_type_t {
  /** The configuration parameter is of type ibool */
  IB_CFG_IBOOL,

   /** The configuration parameter is of type ulint */
  IB_CFG_ULINT,

   /** The configuration parameter is of type ulong */
  IB_CFG_ULONG,

  /** The configuration parameter is of type char* */
  IB_CFG_TEXT,

  /** The configuration parameter is a callback parameter */
  IB_CFG_CB
};

/** @enum ib_col_type_t  column types that are supported. */
enum ib_col_type_t : int {
  /** Character varying length. The column is not padded. */
  IB_VARCHAR =  1,

  /** Fixed length character string. The column is padded to the right. */
  IB_CHAR =  2,

  /** Fixed length binary, similar to IB_CHAR but the column is not padded to the right. */
  IB_BINARY =  3,

  /** Variable length binary */
  IB_VARBINARY =  4,

  /** Binary large object, or a TEXT type */
  IB_BLOB  =  5,

  /** Integer: can be any size from 1 - 8 bytes. If the size is
  1, 2, 4 and 8 bytes then you can use the typed read and write functions. For
  other sizes you will need to use the ib_col_get_value() function and do the
  conversion yourself. */
  IB_INT =  6,

  /** System column, this column can be one of DATA_TRX_ID, DATA_ROLL_PTR or DATA_ROW_ID. */
  IB_SYS =  8,

  /** C (float)  floating point value. */
  IB_FLOAT =  9,

  /** C (double) floating point value. */
  IB_DOUBLE =  10,

  /** Decimal stored as an ASCII string */
  IB_DECIMAL =  11,

  /** Any charset, varying length */
  IB_VARCHAR_ANYCHARSET =  12,

  /** Any charset, fixed length */
  IB_CHAR_ANYCHARSET =  13
};

/** @enum ib_tbl_fmt_t InnoDB table format types */
enum ib_tbl_fmt_t {
  IB_TBL_UNKNOWN = 0,

  /** Default row format. */
  IB_TBL_V1,
};

/** @enum ib_col_attr_t InnoDB column attributes */
enum ib_col_attr_t {
  /** No special attributes. */
  IB_COL_NONE = 0,

  /** Column data can't be nullptr. */
  IB_COL_NOT_NULL = 1,

  /** Column is IB_INT and unsigned. */
  IB_COL_UNSIGNED = 2,

  /** Future use, reserved. */
  IB_COL_NOT_USED = 4,

  /** Custom precision type, this is a bit that is ignored by InnoDB and so
   * can be set and queried by users. */
  IB_COL_CUSTOM1 = 8,

  /** Custom precision type, this is a bit that is ignored by InnoDB and so
   * can be set and queried by users. */
  IB_COL_CUSTOM2 = 16,

  /** Custom precision type, this is a bit that is ignored by InnoDB and
   * so can be set and queried by users. */
  IB_COL_CUSTOM3 = 32
};

/* Note: must match lock0types.h */
/** @enum ib_lck_mode_t InnoDB lock modes. */
enum ib_lck_mode_t {
  /** Intention shared, an intention lock should be used to lock tables */
  IB_LOCK_IS = 0,

  /** Intention exclusive, an intention lock should be used to lock tables */
  IB_LOCK_IX,

  /** Shared locks should be used to lock rows */
  IB_LOCK_S,

  /** Exclusive locks should be used to lock rows*/
  IB_LOCK_X,

  /** Future use, reserved */
  IB_LOCK_NOT_USED,

  /** This is used internally to note consistent read */
  IB_LOCK_NONE,

  /** number of lock modes */
  IB_LOCK_NUM = IB_LOCK_NONE
};

/** @enum ib_srch_mode_t InnoDB cursor search modes for ib_cursor_moveto().
Note: Values must match those found in page0cur.h */
enum ib_srch_mode_t {
    /** Unsupported. */
    IB_CUR_UNSUPP = 0,

  /** If search key is not found then position the cursor on the row
   * that is greater than the search key */
  IB_CUR_G = 1,

  /** If the search key not found then position the cursor on the row
   * that is greater than or equal to the search key */
  IB_CUR_GE = 2,

  /** If search key is not found then position the cursor on the row
   * that is less than the search key */
  IB_CUR_L = 3,

  /** If search key is not found then position the cursor on the row
   * that is less than or equal to the search key */
  IB_CUR_LE = 4
};

/** @enum ib_match_mode_t Various match modes used by ib_cursor_moveto() */
enum ib_match_mode_t {
  /** Closest match possible */
  IB_CLOSEST_MATCH,

  /** Search using a complete key value */
  IB_EXACT_MATCH,

  /** Search using a key prefix which must match to rows: the prefix may
   * contain an incomplete field (the last field in prefix may be just a
   * prefix of a fixed length column) */
  IB_EXACT_PREFIX
};

/** @struct ib_col_meta_t InnoDB column meta data. */
struct ib_col_meta_t {
  /** Type of the column */
  ib_col_type_t  type;

  /** Column attributes */
  ib_col_attr_t  attr;

  /** Length of type in bytes. */
  uint32_t  type_len;

  /** 16 bits of data relevant only to the client. InnoDB doesn't care */
  uint16_t  client_type;

  /** Column charset */
  ib_charset_t*  charset;
};

/* Note: Must be in sync with trx0trx.h */
/** @enum ib_trx_state_t The transaction state can be queried using the
 * ib_trx_state() function. The InnoDB deadlock monitor can roll back a
 * transaction and users should be prepared for this, especially where there
 * is high contention. The way to determine the state of the transaction is to
 * query it's state and check. */
enum ib_trx_state_t {
  /** Has not started yet, the transaction has not ben started yet.*/
  IB_TRX_NOT_STARTED,

  /** The transaction is currently active and needs to be either
   * committed or rolled back. */
  IB_TRX_ACTIVE,

  /** Not committed to disk yet */
  IB_TRX_COMMITTED_IN_MEMORY,

  /** Support for 2PC/XA */
  IB_TRX_PREPARED
};

/* Note: Must be in sync with Trx_isolation in trx0types.h */
/** @enum ib_trx_level_t Transaction isolation levels */
enum ib_trx_level_t {
  /** Dirty read: non-locking SELECTs are performed so that we do not
   * look at a possible earlier version of a record; thus they are not
   * 'consistent' reads under this isolation level; otherwise like level 2 */
  IB_TRX_READ_UNCOMMITTED = 0,

  /** Somewhat Oracle-like isolation, except that in range UPDATE and DELETE
   * we must block phantom rows with next-key locks; SELECT ... FOR UPDATE
   * and ...  LOCK IN SHARE MODE only lock the index records, NOT the gaps
   * before them, and thus allow free inserting; each consistent read reads
   * its own snapshot */
  IB_TRX_READ_COMMITTED = 1,

  /** All consistent reads in the same trx read the same snapshot; full next-key
   * locking used in locking reads to block insertions into gaps */
  IB_TRX_REPEATABLE_READ = 2,

  /** All plain SELECTs are converted to LOCK IN SHARE MODE reads */
  IB_TRX_SERIALIZABLE = 3
};

/** @enum ib_shutdown_t When ib_shutdown() is called InnoDB may take a long
 * time to shutdown because of background tasks e.g., purging deleted records.
 * The following flags allow the user to control the shutdown behavior. */
enum ib_shutdown_t {
  /** Normal shutdown, do insert buffer merge and purge before complete shutdown. */
  IB_SHUTDOWN_NORMAL,

  /** Do not do a purge and index buffer merge at shutdown. */
  IB_SHUTDOWN_NO_IBUFMERGE_PURGE,

  /** Same as NO_IBUFMERGE_PURGE and in addition do not even flush the buffer
   * pool to data files. No committed transactions are lost */
  IB_SHUTDOWN_NO_BUFPOOL_FLUSH
};

/** Generical InnoDB callback prototype. */
using ib_cb_t = void(*)();

/* Note: This is to make it easy for API users to have type
 * checking for arguments to our functions. Making it ib_opaque_t
 * by itself will result in pointer decay resulting in subverting
 * of the compiler's type checking. */

/** InnoDB tuple handle. This handle can refer to either a cluster index
 * tuple or a secondary index tuple. There are two types of tuples for each
 * type of index, making a total of four types of tuple handles. There
 * is a tuple for reading the entire row contents and another for searching
 * on the index key. */
using ib_tpl_t = ib_tpl_struct*;

/** InnoDB transaction handle, all database operations need to be covered
 * by transactions. This handle represents a transaction. The handle can be
 * created with ib_trx_begin(), you commit your changes with ib_trx_commit()
 * and undo your changes using ib_trx_rollback(). If the InnoDB deadlock
 * monitor rolls back the transaction then you need to free the transaction
 * using the function ib_trx_release(). You can query the state of an InnoDB
 * transaction by calling ib_trx_state(). */
using ib_trx_t = ib_trx_struct*;

/** InnoDB cursor handle */
using ib_crsr_t = ib_crsr_struct*;

/** InnoDB table schema handle */
using ib_tbl_sch_t = ib_tbl_sch_struct*; 

/** InnoDB index schema handle */
using ib_idx_sch_t = ib_idx_sch_struct*;

/** Visit all tables in the InnoDB schem. */
/**
 * @brief Function pointer type for visiting all tables in a schema.
 *
 * This function pointer type is used as a callback for visiting all tables in a schema.
 * It takes an argument `arg` which is a user-defined callback argument.
 * The `name` parameter represents the name of the table being visited.
 * The `name_len` parameter represents the length of the table name in bytes.
 *
 * @param arg User callback argument.
 * @param name Table name.
 * @param name_len Length of the table name in bytes.
 * @return 0 on success, nonzero on failure (abort traversal)
 */
using ib_schema_visitor_table_all_t = std::function<int(void* arg, const char* name, int name_len)>;

/** Callback functions to traverse the schema of a table. */
struct ib_schema_visitor_t {
  /** Currently, this is also the number of callback functions in the struct. */
  enum Version {
    TABLE = 1,
    TABLE_COL = 2,
    TABLE_AND_INDEX = 3,
    TABLE_AND_INDEX_COL = 4
  };

  /** Table visitor */
  /**
  * @brief Function pointer type for visiting schema tables.
  *
  * This function pointer type is used for visiting schema tables in the embedded InnoDB library.
  * The visitor function should return 0 on success or a nonzero value on failure to abort the traversal.
  *
  * @param arg User callback argument.
  * @param name The name of the table being visited.
  * @param tbl_fmt The type of the table.
  * @param page_size The page size of the table.
  * @param n_cols The number of columns defined in the table.
  * @param n_indexes The number of indexes defined in the table.
  * @return 0 on success, nonzero on failure.
  */
  using table_t = std::function<int(void* arg, const char* name, ib_tbl_fmt_t tbl_fmt, ulint page_size, int n_cols, int n_indexes)>;
  
  /** Table column visitor */
  /**
  * @brief Function pointer type for visiting table columns in a schema.
  *
  * This function pointer type is used for traversing and visiting table columns in a schema.
  * The visitor function should return 0 on success and a nonzero value on failure to abort the traversal.
  *
  * @param arg User callback argument.
  * @param name The name of the column.
  * @param col_type The type of the column.
  * @param len The length of the column.
  * @param attr The attributes of the column.
  * @return 0 on success, nonzero on failure.
  */
  using table_col_t = std::function<int(void* arg, const char* name, ib_col_type_t col_type, ulint len, ib_col_attr_t attr)>;
  
  /** Index visitor */
  /**
  * @brief Function pointer type for visiting indexes in the schema.
  *
  * This function pointer type is used for visiting indexes in the schema.
  * The visitor function should return 0 on success and nonzero on failure
  * to abort the traversal.
  *
  * @param arg       User callback argument.
  * @param name      The name of the index.
  * @param clustered True if the index is clustered.
  * @param unique    True if the index is unique.
  * @param n_cols    The number of columns defined in the index.
  *
  * @return 0 on success, nonzero on failure.
  */
  using index_t = std::function<int(void* arg, const char* name, bool clustered, bool unique, int n_cols)>;
  
  /** Index column visitor */
  /**
  * @brief Function pointer type for visiting index columns in the InnoDB schema.
  *
  * This function pointer type is used to define a callback function for visiting index columns in the InnoDB schema.
  * The callback function should return 0 on success and a nonzero value on failure to abort the traversal.
  *
  * @param arg The user callback argument.
  * @param name The name of the column being visited.
  * @param prefix_len The length of the prefix.
  *
  * @return 0 on success, nonzero on failure.
  */
  using index_col_t = std::function<int(void* arg, const char* name, ulint prefix_len)>;

  /** Visitor version */
  Version  version;

  /** For travesing table info */
  table_t  table;

  /** For travesing table column info */
  table_col_t  table_col;

  /** For travesing index info */
  index_t  index;

  /** For travesing index column info */
  index_col_t  index_col;
};

/**
 * @brief Function pointer type for client comparison function.
 *
 * This function pointer type represents a client comparison function that compares two keys.
 * The function takes in column meta data, two keys, and their respective lengths.
 * It returns an integer value indicating the comparison result: 1 if the first key is greater,
 * 0 if the keys are equal, and -1 if the first key is less than the second key.
 *
 * @param col_meta The column meta data.
 * @param p1 The first key.
 * @param p1_len The length of the first key.
 * @param p2 The second key.
 * @param p2_len The length of the second key.
 * @return An integer value indicating the comparison result.
 */
using ib_client_cmp_t = std::function<int(const ib_col_meta_t* col_meta, const ib_byte_t* p1, ulint p1_len, const ib_byte_t* p2, ulint p2_len)>;

/* This should be the same as innodb0types.h */
/** Represents SQL_NULL length */
constexpr uint32_t IB_SQL_NULL = 0xFFFFFFFF;

/** The number of system columns in a row. */
constexpr ulint IB_N_SYS_COLS = 3;

/** The maximum length of a text column. */
constexpr ulint MAX_TEXT_LEN = 4096;

/* MySQL uses 3 byte UTF-8 encoding. */
/** The maximum length of a column name in a table schema. */
constexpr ulint IB_MAX_COL_NAME_LEN   = 64 * 3;

/** The maximum length of a table name (plus database name). */
constexpr ulint IB_MAX_TABLE_NAME_LEN = 64 * 3;

/** @def ib_tbl_sch_add_blob_col(s, n)
 * Add a BLOB column to a table schema.
 * @param s is the the schema handle
 * @param n is the column name  */
#define ib_tbl_sch_add_blob_col(s, n) \
  ib_table_schema_add_col(s, n, IB_BLOB, IB_COL_NONE, 0, 0)

/** @def ib_tbl_sch_add_text_col(s, n)
 * Add a BLOB column to a table schema.
 * @param s is the the schema handle
 * @param n is the column name
 * Add a TEXT column to a table schema. */
 #define ib_tbl_sch_add_text_col(s, n) \
  ib_table_schema_add_col(s, n, IB_VARCHAR, IB_COL_NONE, 0, MAX_TEXT_LEN)

/** @def ib_tbl_sch_add_varchar_col(s, n, l)
 * Add a VARCHAR column to a table schema.
 * @param s is the schema handle
 * @param n is the column name
 * @param l the max length of the VARCHAR column
 * @return DB_SUCCESS or error code */
#define ib_tbl_sch_add_varchar_col(s, n, l) \
  ib_table_schema_add_col(s, n, IB_VARCHAR, IB_COL_NONE, 0, l)

/** @def ib_tbl_sch_add_u32_col(s, n)
 * Add an UNSIGNED INT column to a table schema.
 * @param s is the schema handle
 * @param n is the column name
 * @return DB_SUCCESS or error code */
#define ib_tbl_sch_add_u32_col(s, n) \
  ib_table_schema_add_col(s, n, IB_INT, IB_COL_UNSIGNED, 0, 4)

/** @def ib_tbl_sch_add_u64_col(s, n)
 * Add an UNSIGNED BIGINT column to a table schema.
 * @param s is the schema handle
 * @param n is the column name
 * @return DB_SUCCESS or error code */
#define ib_tbl_sch_add_u64_col(s, n) \
  ib_table_schema_add_col(s, n, IB_INT, IB_COL_UNSIGNED, 0, 8)

/** @def ib_tbl_sch_add_u64_notnull_col(s, n)
 * Add an UNSIGNED BIGINT NOT nullptr column to a table schema.
 * @param s is the schema handle
 * @param n is the column name
 * @return DB_SUCCESS or error code */
#define ib_tbl_sch_add_u64_notnull_col(s, n) \
  ib_table_schema_add_col(s, n, IB_INT, IB_COL_NOT_NULL | IB_COL_UNSIGNED, 0, 8)

/** @def ib_cfg_set_int(name, value)
 * Set an int configuration variable.
 * @ingroup config
 * @param name is the config variable name
 * @param value is the integer value of the variable
 * @return DB_SUCCESS or error code */
#define ib_cfg_set_int(name, value)  ib_cfg_set(name, value)

/** @def ib_cfg_set_text(name, value)
 * Set a text configuration variable.
 * @ingroup config
 * @param name is the config variable name
 * @param value is the char* value of the variable
 * @return DB_SUCCESS or error code */
#define ib_cfg_set_text(name, value)  ib_cfg_set(name, value)

/*! @def ib_cfg_set_bool_on(name)
Set a boolean configuration variable to true.
@ingroup config
@param name is the config variable name
@return DB_SUCCESS or error code */
#define ib_cfg_set_bool_on(name)  ib_cfg_set(name, true)

/** @def ib_cfg_set_bool_off(name)
 * Set a boolean configuration variable to false.
 * @ingroup config
 * @param name is the config variable name
 * @return DB_SUCCESS or error code */
#define ib_cfg_set_bool_off(name)  ib_cfg_set(name, false)

/** @def ib_cfg_set_callback(name, value)
 * Set a generic ib_cb_t callback function.
 * @ingroup config
 * @param name is the config variable name
 * @param value is a pointer to a callback function
 * @return DB_SUCCESS or error code */
#define ib_cfg_set_callback(name, value) ib_cfg_set(name, value)

/** Callback function to compare InnoDB key columns in an index. */
extern ib_client_cmp_t  ib_client_compare;

/** Define the Doxygen groups:
   @defgroup init Startup/Shutdown functions
   @defgroup cursor Cursor functions
   @defgroup trx Transaction functions
   @defgroup config Configuration functions
   @defgroup ddl DDL functions
   @defgroup misc Miscellaneous functions
   @defgroup tuple Tuple functions
   @defgroup sql SQL functions
   @defgroup dml DML functions
   @defgroup debug Debug and Testing functions
*/
/** Return the API version number, the version number format is:
| 16 bits future use | 16 bits current | 16 bits revision | 16 bits age |

- If the library source code has changed at all since the last release,
  then revision will be incremented (`c:r:a' becomes `c:r+1:a').
- If any interfaces have been added, removed, or changed since the last
  update, current will be incremented, and revision will be set to 0.
- If any interfaces have been added (but not changed or removed) since
  the last release, then age will be incremented.
- If any interfaces have been changed or removed since the last release,
  then age will be set to 0.

@ingroup misc
@return  API version number */
[[nodiscard]] uint64_t ib_api_version();

/** Initialize the InnoDB engine. This must be called prior to calling
 * any other InnoDB API function. You can call only the ib_cfg_*() functions
 * between calls to ib_init() and ib_startup(). No other InnoDB
 * functions should be called.
 * 
 * @ingroup init
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_init();

/** Startup the InnoDB engine. If this function is called on a non-existent
 * database then based on the default or user specified configuration
 * settings it will create all the necessary files. If the database was
 * shutdown cleanly but the user deleted the REDO log files then it
 * will recreate the REDO log files.
 * 
 * @ingroup init
 * @param format is the max file format name that the engine supports.
 * Currently this is either Antelope or Barracuda although more may be
 * added in the future without API changes.
 * @return  DB_SUCCESS or error code
 * @see DB_SUCCESS */
[[nodiscard]] ib_err_t ib_startup(const char* format);

/** Shutdown the InnoDB engine. Call this function when they are no 
 * active transactions. It will close all files and release all memory
 * on successful completion. All internal variables will be reset to their
 * default values.
 * 
 * @ingroup init
 * @param flag is the shutdown flag
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_shutdown(ib_shutdown_t flag);

/** Start a transaction that's been rolled back. This special function
 * exists for the case when InnoDB's deadlock detector has rolledack
 * a transaction. While the transaction has been rolled back the handle
 * is still valid and can be reused by calling this function. If you
 * don't want to reuse the transaction handle then you can free the handle
 * by calling ib_trx_release().
 * 
 * @ingroup trx
 * @param trx is the transaction to restart
 * @param trx_level is the transaction isolation level
 * @return  innobase txn handle */
[[nodiscard]] ib_err_t ib_trx_start(ib_trx_t trx, ib_trx_level_t trx_level);

/** Begin a transaction. This will allocate a new transaction handle and
 * put the transaction in the active state.
 * 
 * @ingroup trx
 * @param trx_level is the transaction isolation level
 * @return  innobase txn handle */
[[nodiscard]] ib_trx_t ib_trx_begin(ib_trx_level_t  trx_level);

/** Set client data for a transaction. This is passed back to the client
 * in the trx_is_interrupted callback. InnoDB will only ever pass this
 * around, it will never dereference it.
 * 
 * @ingroup trx
 * @param trx is the transaction to set the client data for
 * @param client_data is client program's data about this transaction */
void ib_trx_set_client_data(ib_trx_t trx, void* client_data);

/** Query the transaction's state. This function can be used to check for
* the state of the transaction in case it has been rolled back by the
* InnoDB deadlock detector. Note that when a transaction is selected as
* a victim for rollback, InnoDB will always return an appropriate error
* code indicating this. @see DB_DEADLOCK, @see DB_LOCK_TABLE_FULL and
* @see DB_LOCK_WAIT_TIMEOUT
* 
* @ingroup trx
* @param trx is the transaction handle
* @return  transaction state */
[[nodiscard]] ib_trx_state_t ib_trx_state(ib_trx_t  trx);

/** Release the resources of the transaction. If the transaction was
* selected as a victim by InnoDB and rolled back then use this function
* to free the transaction handle.
* 
* @ingroup trx
* @param trx is the transaction handle
* @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_trx_release(ib_trx_t trx);

/** Commit a transaction. This function will release the schema latches too.
* It will also free the transaction handle.
* 
* @ingroup trx
* @param trx is thr transaction handle
* @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_trx_commit(ib_trx_t trx);

/** Rollback a transaction. This function will release the schema latches too.
* It will also free the transaction handle.
* 
* @ingroup trx
* @param trx is the transaction handle
* @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_trx_rollback(ib_trx_t trx);

/** Add columns to a table schema. Tables are created in InnoDB by first
* creating a table schema which is identified by a handle. Then you
* add the column definitions to the table schema.
* 
* @ingroup ddl
* @param ib_tbl_sch is the table schema instance
* @param name is the name of the column to add
* @param col_type is the type of the column
* @param col_attr are the attributes of the column, including constraints
* @param client_type is any 16 bit number relevant only to the client
* @param len is the maximum length of the column
* @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_table_schema_add_col(
  ib_tbl_sch_t ib_tbl_sch,
  const char* name,
  ib_col_type_t col_type,
  ib_col_attr_t col_attr,
  uint16_t client_type,
  ulint len);

/** Create and add an index key definition to a table schema. The index
* schema is owned by the table schema instance and will be freed when
* the table schema instance is freed.
* 
* @ingroup ddl
* @param[in,out] tbl_sch is the schema instance
* @param name name of the key definition to create
* @param[out] idx_sch is the key definition schema instance
* @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_table_schema_add_index(ib_tbl_sch_t tbl_sch, const char*  name, ib_idx_sch_t* idx_sch);

/** Destroy a schema. The handle is freed by this function.
* 
* @ingroup ddl
* @param ib_tbl_sch is the table schema to delte*/
void ib_table_schema_delete(ib_tbl_sch_t ib_tbl_sch);

/** Create a table schema.
 *  
 * @ingroup ddl
 * @param name is the table name for which to create the schema
 * @param[out] tbl_sch is the schema instance that is created
 * @param tbl_fmt is the format of the table to be created
 * @param page_size is the page size for the table or 0 for default
 * 
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_table_schema_create(const char* name, ib_tbl_sch_t* tbl_sch, ib_tbl_fmt_t tbl_fmt, ulint page_size);

/** Add columns to an index schema definition.
 * 
 * @ingroup ddl
 * @param[in,out] idx_sch is the index schema instance
 * @param name is the name of the column to add to the index schema
 * @param prefix_len is the prefix length of the index or 0 if no prefix
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_index_schema_add_col(ib_idx_sch_t idx_sch, const char* name, ulint  prefix_len);

/** Create an index schema instance.
 * 
 * @ingroup ddl
 * @param ib_usr_trx is the current user transaction
 * @param name is the name of the index to create
 * @param table_name is the name of the table the index belongs to
 * @param[out] idx_sch is the newly created index schema instance
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_index_schema_create(ib_trx_t ib_usr_trx, const char* name, const char* table_name, ib_idx_sch_t* idx_sch);

/** Set index as clustered index. Implies UNIQUE.
 * 
 * @ingroup ddl
 * @param[in,out] idx_sch is the index schema to update
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_index_schema_set_clustered(ib_idx_sch_t idx_sch);

/** Set to true if it's a simple select.
 * 
 * @ingroup sql
 * @param[in, out] crsr is the cursor to update */
void ib_cursor_set_simple_select(ib_crsr_t crsr);

/** Set index as a unique index.
 * 
 * @ingroup ddl
 * @param[in,out] idx_sch is the index schema to update
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_index_schema_set_unique(ib_idx_sch_t idx_sch);

/** Destroy an index schema.
 *  
 * @ingroup ddl
 * @param idx_sch is the index schema to delete */
void ib_index_schema_delete(ib_idx_sch_t idx_sch);

/** Create a table in the InnoDB data dictionary using the schema definition.
* If the table exists in the database then this function will return
* DB_TABLE_IS_BEING_USED and id will contain that table's id.
* 
* @ingroup ddl
* @param[in,out] trx the current user transaction
* @param tbl_sch the the schema for the table to create
* @param[out] id table id that was created
* @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_table_create(ib_trx_t trx, const ib_tbl_sch_t tbl_sch, ib_id_t* id);

/** Rename a table. Ensure that you have acquired the schema lock in
 * exclusive mode.
 * 
 * @ingroup ddl
 * @param[in,out] trx is the current user transaction
 * @param old_name the current name of the table
 * @param new_name the new name for the table
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_table_rename(ib_trx_t trx, const char* old_name, const char* new_name);

/** Create a secondary index. The index id encodes the table id in the high
 * 4 bytes and the index id in the lower 4 bytes.
 * 
 * @ingroup ddl
 * @param[in,out] ib_idx_sch the schema for the index
 * @param[out] index_id is the new index id that was created
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_index_create(ib_idx_sch_t ib_idx_sch, ib_id_t* index_id);

/** Drop a table. Ensure that you have acquired the schema lock in
 * exclusive mode.
 * 
 * @ingroup ddl
 * @param trx is the covering transaction.
 * @param name is the name of the table to drop
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_table_drop(ib_trx_t trx, const char* name);

/** Drop a secondary index. Ensure that you have acquired the schema lock in
 * exclusive mode.
 * 
 * @ingroup ddl
 * @param trx is the covering transaction.
 * @param index_id is the id of the index to drop
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_index_drop(ib_trx_t trx, ib_id_t index_id);

/** Open an InnoDB table and return a cursor handle to it.
 * 
 * @ingroup cursor
 * @param table_id is the id of the table to open
 * @param trx is the current transaction handle, can be nullptr
 * @param[out] crsr is the new cursor
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_open_table_using_id(ib_id_t table_id, ib_trx_t trx, ib_crsr_t*  crsr);

/** Open an InnoDB index and return a cursor handle to it.
 * 
 * @ingroup cursor
 * @param index_id is the id of the index to open
 * @param trx is the current transaction handlem can be nullptr
 * @param[out] crsr is the new cursor
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_open_index_using_id(ib_id_t index_id, ib_trx_t trx, ib_crsr_t* crsr);

/** Open an InnoDB secondary index cursor and return a cursor handle to it.
 * 
 * @ingroup cursor
 * @param ib_open_crsr is an open cursor
 * @param index_name is the name of the index
 * @param[out] crsr is the new cursor
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_open_index_using_name(ib_crsr_t ib_open_crsr, const char* index_name, ib_crsr_t* crsr);

/** Open an InnoDB table by name and return a cursor handle to it.
 * 
 * @ingroup cursor
 * @param name is the table name to open
 * @param trx is the current transactionm, can be nullptr
 * @param crsr is the new cursor
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_open_table(const char*  name, ib_trx_t trx, ib_crsr_t* crsr);

/** Reset the cursor.
 * 
 * @ingroup cursor
 * @param crsr is an open cursor
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_reset(ib_crsr_t crsr);

/** Close an InnoDB table and free the cursor.
 * 
 * @ingroup cursor
 * @param crsr is an open cursor
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_close(ib_crsr_t crsr);

/** Insert a row to a table.
 * 
 * @ingroup dml
 * @param crsr is an open cursor
 * @param tpl is the tuple to insert
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_insert_row(ib_crsr_t  crsr, const ib_tpl_t tpl);

/** Update a row in a table.
 * 
 * @ingroup dml
 * @param crsr is the cursor instance
 * @param old_tpl is the old tuple in the table
 * @param new_tpl is the new tuple with the updated values
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_update_row(ib_crsr_t crsr, const ib_tpl_t old_tpl, const ib_tpl_t  new_tpl);

/** Delete a row in a table.
 * 
 * @ingroup dml
 * @param crsr is the cursor instance
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_delete_row(ib_crsr_t crsr);

/** Read current row.
 * 
 * @ingroup dml
 * @param crsr is the cursor instance
 * @param[out] tpl is the tuple to read the column values
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_read_row(ib_crsr_t crsr, ib_tpl_t tpl);

/** Move cursor to the prev user record in the table.
 * 
 * @ingroup cursor
 * @param crsr is the cursor instance
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_prev(ib_crsr_t crsr);

/** Move cursor to the next user record in the table.
 * 
 * @ingroup cursor
 * @param crsr is the cursor instance
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_next(ib_crsr_t crsr);

/** Move cursor to the first record in the table.
 * 
 * @ingroup cursor
 * @param crsr is the cursor instance
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_first(ib_crsr_t crsr);

/** Move cursor to the last record in the table.
 * 
 * @ingroup cursor
 * @param crsr is the cursor instance
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_last(ib_crsr_t crsr);

/** Search for key.
 * 
 * @ingroup cursor
 * @param crsr is an open cursor instance
 * @param tpl is a key to search for
 * @param ib_srch_mode is the search mode
 * @param[out] result is -1, 0 or 1 depending on tuple eq or gt than the current row
 * @return  DB_SUCCESS or err code */
[[nodiscard]] ib_err_t ib_cursor_moveto(ib_crsr_t crsr, ib_tpl_t tpl, ib_srch_mode_t srch_mode, int* result);

/** Attach the cursor to the transaction. The cursor must not already be
 * attached to another transaction.
 *
 * @ingroup cursor
 * @param crsr is the cursor instance
 * @param trx is the transaction to attach to the cursor */
void ib_cursor_attach_trx(ib_crsr_t crsr, ib_trx_t trx);

/** Set the match mode for ib_cursor_move().
 *
 * @ingroup cursor
 * @param crsr is the cursor instance
 * @param match_mode is the match mode to set */
void ib_cursor_set_match_mode(ib_crsr_t crsr, ib_match_mode_t match_mode);

/** Set a column of the tuple. Make a copy using the tuple's heap.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param col_no is the column index in the tuple
 * @param src is the data value to set
 * @param len is the data value (src) length in bytes
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_col_set_value(ib_tpl_t tpl, ulint col_no, const void* src, ulint len);

/** Get the size of the data available in the column the tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @return  bytes avail or IB_SQL_NULL */
[[nodiscard]] ulint ib_col_get_len(ib_tpl_t tpl, ulint i);

/** Copy a column value from the tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] dst is where the data will be copied
 * @param len is the maximum number of bytes that can be copied to dst
 * @return  bytes copied or IB_SQL_NULL */
[[nodiscard]] ulint ib_col_copy_value(ib_tpl_t tpl, ulint i, void* dst, ulint len);

/** Read a signed int 8 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_i8(ib_tpl_t tpl, ulint i, int8_t* ival);

/** Read an unsigned int 8 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_u8(ib_tpl_t tpl, ulint i, uint8_t* ival);

/** Read a signed int 16 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_i16(ib_tpl_t tpl, ulint i, int16_t* ival);

/** Read an unsigned int 16 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_u16(ib_tpl_t tpl, ulint i, uint16_t* ival);

/** Read a signed int 32 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_i32(ib_tpl_t tpl, ulint i, int32_t* ival);

/** Read an unsigned int 32 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_u32(ib_tpl_t tpl, ulint i, uint32_t* ival);

/** Read a signed int 64 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_i64(ib_tpl_t tpl, ulint i, int64_t* ival);

/** Read an unsigned int 64 bit column from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] ival is the integer value
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_u64(ib_tpl_t tpl, ulint i, uint64_t* ival);

/** Get a column value pointer from the tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @return  nullptr or pointer to buffer */
[[nodiscard]] const void* ib_col_get_value(ib_tpl_t tpl, ulint i);

/** Get a column type, length and attributes from the tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple instance
 * @param i is the index (ordinal position) of the column within the tuple
 * @param[out] col_meta the column meta data
 * @return  len of column data */
ulint ib_col_get_meta(ib_tpl_t tpl, ulint i, ib_col_meta_t* col_meta);

/** "Clear" or reset an InnoDB tuple. We free the heap and recreate the tuple.
 * 
 * @ingroup tuple
 * @param tpl is the tuple to be freed
 * @return  new tuple, or nullptr */

[[nodiscard]] ib_tpl_t ib_tuple_clear(ib_tpl_t tpl);

/** Create a new cluster key search tuple and copy the contents of  the
 * secondary index key tuple columns that refer to the cluster index record
 * to the cluster key. It does a deep copy of the column data.
 * 
 * @ingroup tuple
 * @param crsr is a cursor opened on a secondary index
 * @param[out] dst_tpl is the tuple where the key data will be copied
 * @param src_tpl is the source secondary index tuple to copy from
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_tuple_get_cluster_key(ib_crsr_t crsr, ib_tpl_t* dst_tpl, const ib_tpl_t src_tpl);

/** Copy the contents of  source tuple to destination tuple. The tuples
 * must be of the same type and belong to the same table/index.
 * 
 * @ingroup tuple
 * @param dst_tpl is the destination tuple
 * @param src_tpl is the source tuple
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_tuple_copy(ib_tpl_t dst_tpl, const ib_tpl_t src_tpl);

/** Create an InnoDB tuple used for index/table search.
 * 
 * @ingroup tuple
 * @param crsr is the cursor instance
 * @return tuple for current index */

[[nodiscard]] ib_tpl_t ib_sec_search_tuple_create(ib_crsr_t crsr);

/** Create an InnoDB tuple used for index/table search.
 * 
 * @ingroup tuple
 * @param crsr is the cursor instance
 * @return  tuple for current index */
[[nodiscard]] ib_tpl_t ib_sec_read_tuple_create(ib_crsr_t crsr);

/** Create an InnoDB tuple used for table key operations.
 * 
 * @ingroup tuple
 * @param crsr is the cursor instance
 * @return  tuple for current table */
[[nodiscard]] ib_tpl_t ib_clust_search_tuple_create(ib_crsr_t crsr);

/** Create an InnoDB tuple for table row operations.
 * 
 * @ingroup tuple
 * @param crsr is the cursor instance
 * @return  tTuple for current table */
[[nodiscard]] ib_tpl_t ib_clust_read_tuple_create(ib_crsr_t crsr);

/** Return the number of user columns in the tuple definition.
 * 
 * @ingroup tuple
 * @param tpl is a tuple
 * @return  number of user columns */

[[nodiscard]] ulint ib_tuple_get_n_user_cols(const ib_tpl_t tpl);

/** Return the number of columns in the tuple definition.
 * 
 * @ingroup tuple
 * @param tpl is a tuple
 * @return  number of columns */
[[nodiscard]] ulint ib_tuple_get_n_cols(const ib_tpl_t tpl);

/** Destroy an InnoDB tuple.
 * 
 * @ingroup tuple
 * @param tpl is the tuple instance to delete */
void ib_tuple_delete(ib_tpl_t tpl);

/** Truncate a table. The cursor handle will be closed and set to nullptr
 * on success.
 * 
 * @ingroup ddl
 * @param[out] crsr is the cursor for table to truncate
 * @param[out] table_id is the new table id
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_cursor_truncate(ib_crsr_t* crsr, ib_id_t* table_id);

/** Truncate a table.
 * 
 * @ingroup ddl
 * @param table_name is the name of the table to truncate
 * @param[out] table_id is the new table id
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_table_truncate(const char* table_name, ib_id_t* table_id);

/** Get a table id.
 * 
 * @ingroup ddl
 * @param table_name is the name of the table to lookup
 * @param[out] table_id is the new table id if found
 * @return  DB_SUCCESS if found */
[[nodiscard]] ib_err_t ib_table_get_id(const char* table_name, ib_id_t* table_id);

/** Get an index id.
 * 
 * @ingroup ddl
 * @param table_name is the name of the table that contains the index
 * @param index_name is the name of the index to lookup
 * @param[out] index_id contains the index id if found
 * @return  DB_SUCCESS if found */
[[nodiscard]] ib_err_t ib_index_get_id(const char* table_name, const char* index_name, ib_id_t* index_id);

/** Create a database if it doesn't exist.
 * 
 * @ingroup ddl
 * @param dbname is the name of the database to create
 * @return  true on success */
[[nodiscard]] bool ib_database_create(const char* dbname);

/** Drop a database if it exists. This function will also drop all tables
 * within the database.
 * 
 * @ingroup ddl
 * @param dbname is the name of the database to drop
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_database_drop(const char* dbname);

/** Check if cursor is positioned.
 * 
 * @ingroup cursor
 * @param crsr is the cursor instance to check
 * @return  true if positioned */
[[nodiscard]] bool ib_cursor_is_positioned(const ib_crsr_t crsr);

/** Latches the data dictionary in shared mode.
 * 
 * @ingroup ddl
 * @param trx is the transaction instance
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_schema_lock_shared(ib_trx_t trx);

/** Latches the data dictionary in exclusive mode.
 * 
 * @ingroup ddl
 * @param trx is the transaction instance
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_schema_lock_exclusive(ib_trx_t trx);

/** Checks if the data dictionary is latched in exclusive mode by a
 * user transaction.
 * 
 * @ingroup ddl
 * @param trx is a transaction instance
 * @return  true if exclusive latch */
[[nodiscard]] bool ib_schema_lock_is_exclusive(const ib_trx_t trx);

/** Checks if the data dictionary is latched in shared mode.
 * @param trx is a transaction instance
 * @return  true if shared latch */
[[nodiscard]] bool ib_schema_lock_is_shared(const ib_trx_t trx);

/** Unlocks the data dictionary.
 * 
 * @ingroup ddl
 * @param trx is a transaction instance
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_schema_unlock(ib_trx_t  trx);

/** Lock an InnoDB cursor/table.
 * 
 * @ingroup trx
 * @param crsr is the cursor instance
 * @param lck_mode is the lock mode
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_cursor_lock(ib_crsr_t crsr, ib_lck_mode_t lck_mode);

/** Set the Lock an InnoDB table using the table id.
 * 
 * @ingroup trx
 * @param trx is a transaction instance
 * @param table_id is the table to lock
 * @param lck_mode is the lock mode
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_table_lock(ib_trx_t trx, ib_id_t table_id, ib_lck_mode_t lck_mode);

/** Set the Lock mode of the cursor.
 * 
 * @ingroup trx
 * @param crsr is the cursor instance for which we want to set the lock mode
 * @param  lck_mode is the lock mode
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_cursor_set_lock_mode(ib_crsr_t crsr, ib_lck_mode_t lck_mode);

/** Set need to access clustered index record flag.
 * 
 * @ingroup dml
 * @param crsr is the cursor instance for which we want to set the flag */
void ib_cursor_set_cluster_access(ib_crsr_t crsr);

/** Read a table's schema using the visitor pattern. It will make the
 * following sequence of calls:
 * 
 * visitor->table()
 * visitor->table_col() for each user column
 * visitor->index() for each user index
 * visitor->index_col() for each column in user index
 *
 * It will stop if any of the above functions returns a non-zero value.
 * The caller must have an exclusive lock on the InnoDB data dictionary
 * 
 * @ingroup ddl
 * @param trx transaction that owns the schema lock
 * @param name is the table name to read
 * @param visitor visitor functions to invoke on each definition
 * @param arg is the argument passed to the visitor functions.
 * @return  DB_SUCCESS or DB_ERROR */
[[nodiscard]] ib_err_t ib_table_schema_visit(ib_trx_t trx, const char* name, const ib_schema_visitor_t* visitor, void* arg);

/** List all the tables in the InnoDB's data dictionary. It will abort
 * if visitor returns a non-zero value.
 * 
 * It will call the function:
 *   visitor.tables(arg, const char* name, int name_len);
 * 
 * The function will abort if visitor.tables() returns non-zero.
 * 
 * @ingroup ddl
 * @param trx is the transaction that owns the schema lock
 * @param visitor is the visitor function
 * @param arg argument passed to the visitor function
 * @return DB_SUCCESS if successful */
[[nodiscard]] ib_err_t ib_schema_tables_iterate(ib_trx_t trx, ib_schema_visitor_table_all_t visitor, void* arg);

/** Get the type of a configuration variable. Returns DB_SUCCESS if the
 * variable with name "name" was found and "type" was set.
 * 
 * @ingroup config
 * @param name is the variable name to look up
 * @param[out] type is the type of the variable name if found
 * @return  DB_SUCCESS if successful */
[[nodiscard]] ib_err_t ib_cfg_var_get_type(const char* name, ib_cfg_type_t* type);

/** Set a configuration variable. The second argument's type depends on the
 * type of the variable with the given "name". Returns DB_SUCCESS if the
 * variable with name "name" was found and if its value was set.
 * 
 * @ingroup config
 * @param name is the config variable name whose value is to be set
 * @return  DB_SUCCESS if set */
[[nodiscard]] ib_err_t ib_cfg_set(const char* name, ...);

/** Get the value of a configuration variable. The type of the returned value
 * depends on the type of the configuration variable. DB_SUCCESS is returned
 * if the variable with name "name" was found and "value" was set.
 * 
 * @ingroup config
 * @param name is the variable name whose value needs to be accessed
 * @param[out] value is the value of the variable if found
 * @return  DB_SUCCESS if retrieved successfully */
[[nodiscard]] ib_err_t ib_cfg_get(const char* name, void* value);

/** Get a list of the names of all configuration variables.
 * The caller is responsible for free(3)ing the returned array of strings
 * when it is not needed anymore and for not modifying the individual strings.
 * 
 * @ingroup config
 * @param[out] names pointer to array of strings
 * @param[out] names_num number of strings returned
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_cfg_get_all(const char*** names, uint32_t* names_num);

/** Creates a named savepoint. The transaction must be started. If there is
 * already a savepoint of the same name, this call erases that old savepoint
 * and replaces it with a new. Savepoints are deleted in a transaction
 * commit or rollback.
 * 
 * @ingroup trx
 * @param trx is the transaction instance
 * @param name is the name of the savepoint
 * @param name_len is the length of name in bytes */
void ib_savepoint_take(ib_trx_t trx, const void* name, ulint name_len);

/** Releases only the named savepoint. Savepoints which were set after this
 * savepoint are left as is.
 * 
 * @ingroup trx
 * @param trx is the active transaction
 * @param name is the name of the savepoint
 * @param name_len is the length of name in bytes
 * @return  if no savepoint of the name found then DB_NO_SAVEPOINT,
 * otherwise DB_SUCCESS */
[[nodiscard]] ib_err_t ib_savepoint_release(ib_trx_t trx, const void* name, ulint name_len);

/** Rolls back a transaction back to a named savepoint. Modifications after the
 * savepoint are undone but InnoDB does NOT release the corresponding locks
 * which are stored in memory. If a lock is 'implicit', that is, a new inserted
 * row holds a lock where the lock information is carried by the trx id stored in
 * the row, these locks are naturally released in the rollback. Savepoints which
 * were set after this savepoint are deleted. If name equals nullptr then all the
 * savepoints are rolled back.
 * 
 * @ingroup trx
 * @param trx is the active transaction
 * @param name is the savepoint name  can be nullptr
 * @param name_len is the length of name in bytes
 * @return  if no savepoint of the name found then DB_NO_SAVEPOINT,
 *         otherwise DB_SUCCESS */
[[nodiscard]] ib_err_t ib_savepoint_rollback(ib_trx_t trx, const void* name, ulint name_len);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_i8(ib_tpl_t tpl, int col_no, int8_t val);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_i16(ib_tpl_t tpl, int col_no, int16_t val);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_i32(ib_tpl_t tpl, int col_no, int32_t val);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_i64(ib_tpl_t tpl, int col_no, int64_t val);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_u8(ib_tpl_t tpl, int col_no, uint8_t val);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_u16(ib_tpl_t tpl, int col_no, uint16_t val);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_u32(ib_tpl_t tpl, int col_no, uint32_t val);

/** Write an integer value to a column. Integers are stored in big-endian
 * format and will need to be converted from the host format.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_u64(ib_tpl_t tpl, int col_no, uint64_t val);

/** Inform the cursor that it's the start of an SQL statement.
 * 
 * @ingroup cursor
 * @param crsr is the cursor instance */
void ib_cursor_stmt_begin(ib_crsr_t crsr);

/** Write a double value to a column.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_double(ib_tpl_t tpl, int col_no, double val);

/** Read a double column value from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple to read from
 * @param col_no is the column number to read
 * @param[out] dval is where the value is copied
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_double(ib_tpl_t tpl, ulint col_no, double* dval);

/** Write a float value to a column.
 * 
 * @ingroup dml
 * @param[in,out] tpl is the tuple to write to
 * @param col_no is the column number to update
 * @param val is the value to write
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_write_float(ib_tpl_t tpl, int col_no, float val);

/** Read a float value from an InnoDB tuple.
 * 
 * @ingroup dml
 * @param tpl is the tuple to read from
 * @param col_no is the column number to read
 * @param[out] fval is where the value is copied
 * @return  DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_tuple_read_float(ib_tpl_t tpl, ulint col_no, float* fval);

/** Set the message logging function.
 * 
 * @ingroup misc
 * @param msg_log is the message logging function
 * @param msg_stream is the message stream, this is the first argument
 *         to the loggingfunction */
void ib_logger_set(ib_msg_log_t msg_log, ib_msg_stream_t msg_stream);

/** Convert an error number to a human readable text message. The
* returned string is static and should not be freed or modified.
* 
* @ingroup misc
* @param db_errno is the error number
* @return  string, describing the error */
[[nodiscard]] const char* ib_strerror(ib_err_t db_errno);

/** Get the value of an INT status variable. 
 * 
 * @ingroup misc
 * @param name is the status variable name
 * @param[out] dst is where the output value is copied if name is found
 * @return DB_SUCCESS if found and type is INT,
 *         DB_DATA_MISMATCH if found but type is not INT,
 *         DB_NOT_FOUND otherwise. */
[[nodiscard]] ib_err_t ib_status_get_i64(const char* name, int64_t* dst);

/** Get a list of the names of all status variables.
 * The caller is responsible for free(3)ing the returned array of strings
 * when it is not needed anymore and for not modifying the individual strings.
 * 
 * @ingroup config
 * @param[out] names pointer to array of strings
 * @param[out] names_num number of strings returned
 * @return  DB_SUCCESS or error code */
[[nodiscard]] ib_err_t ib_status_get_all(const char***  names, uint32_t*  names_num);

/**
 * Set panic handler.
 * 
 * InnoDB will "panic" upon finding certain forms of corruption.
 * By setting a panic handler, you can implement your own notification
 * to the end user of this corruption (e.g. popping up a dialog box).
 * 
 * @ingroup config
 * @param new_panic_handler your panic handler */
void ib_set_panic_handler(ib_panic_handler_t new_panic_handler);

/** Callback for checking if a transaction has been interrupted.
 * This callback lets you implement the MySQL KILL command kind of
 * functionality.
 * A transaction may block in the thread it's running in (for example, while
 * acquiring row locks or doing IO) but other threads may do something that
 * causes ib_trx_is_interrupted_handler_t to return true.  */
using ib_trx_is_interrupted_handler_t = std::function<int(void*)>;

/** Set trx_is_interrupted_handler.
 * 
 * You may specificy a callback that InnoDB will check during certain wait
 * situations to see if it should abort the operation or not. This lets
 * you implement MySQL/Drizzle KILL command style functionality.
 * 
 * @ingroup config
 * @param handler the trx_is_interrupted callback */
void ib_set_trx_is_interrupted_handler(ib_trx_is_interrupted_handler_t handler);

/** Get which key caused a duplicate key error.
 * 
 * In the event of \ref DB_DUPLICATE_KEY error, you can call this function
 * immediately after to get the name of the table and index that caused
 * the error.
 * 
 * @ingroup trx
 * @param trx Transaction where error occured
 * @param table_name pointer to be set to table_name. Valid until next ib_ function
 * call. If you would like to keep it, make a copy.
 * @param index_name pointer to be set to the index name. Valid until next ib_ function 
 * all. If you would like to keep it, make a copy.  */
ib_err_t ib_get_duplicate_key(ib_trx_t trx, const char **table_name, const char **index_name);

/** @struct ib_table_stats_t InnoDB Table and index statistics. */
struct ib_table_stats_t {
  /** Approximate number of rows in the table; we periodically calculate new estimates */
  int64_t  stat_n_rows;

  /** Approximate clustered index size in bytes. */
  uint64_t  stat_clustered_index_size;

  /** Other indexes in bytes */
  uint64_t  stat_sum_of_other_index_sizes;

  /** When a row is inserted, updated, or deleted, we add 1 to this number; we calculate
   * new estimates for the stat_...  values for the table and the indexes at an interval
   * of 2 GB or when about 1 / 16 of table has been modified; also when an estimate operation
   * is called for; the counter is reset to zero at statistics calculation; this counter is
   * not protected by any latch, because this is only used for heuristics */
  uint64_t  stat_modified_counter;
};

/** Get table statistics.
 * 
 * This function will fill out the provided \ref ib_table_stats_t with
 * statistics about the table on the currently opened \ref ib_crsr_t
 * 
 * @ingroup misc
 * @param crsr A Cursor that is opened to a table
 * @param table_stats a \ref ib_table_stats_t to be filled out by InnoDB
 * @param sizeof_ib_table_stats_t sizeof(ib_table_stats_t). This allows for ABI compatible changes to the size of ib_table_stats_t.
 * @returns \ref DB_SUCCESS or error */
[[nodiscard]] ib_err_t ib_get_table_statistics(ib_crsr_t crsr, ib_table_stats_t *table_stats, size_t sizeof_ib_table_stats_t);

/** Get statistics on number of different key values per index part
 * 
 * This function returns the approximate different key
 * values for this index. They are periodically recalculated.
 * 
 * @ingroup misc
 * @param crsr A Cursor that is opened to a table
 * @param index_name name of the index
 * @param ncols returns the number of elements in n_diff
 * @param n_diff An array allocated with malloc() (user needs to free()) containing the statistics
 * @returns \ref DB_SUCCESS or error. \ref DB_NOT_FOUND if index is not found */
[[nodiscard]] ib_err_t ib_get_index_stat_n_diff_key_vals(ib_crsr_t crsr, const char* index_name, uint64_t *ncols, int64_t **n_diff);

/** Force an update of table and index statistics
 * 
 * This function forces an update to the table and index statistics for the table crsr is opened on.
 * 
 * @ingroup misc
 * @param crsr A Cursor that is opened to a table
 * @returns \ref DB_SUCCESS or error.  */
[[nodiscard]] ib_err_t ib_update_table_statistics(ib_crsr_t crsr);

/** Inject an error into InnoDB
 * 
 * This function will simulate an error condition inside InnoDB.
 * You should not rely on this function. It is for InnoDB test suite use only,
 * parts may only be compiled into debug libraries and this function
 * can quite legitimately just return DB_ERROR and cause Voldemort to pay
 * you a visit.
 * 
 * @ingroup debug
 * @param error_to_inject The error inject code to insert.  */
[[nodiscard]] ib_err_t ib_error_inject(int error_to_inject);

/** Do a parallel select count(*) from T;
 * @param[in] trx InnoDB transaction
 * @param[in] crsrs InnoDB cursors
 * @param[in] n_threads Number of threads to use
 * @param[out] n_rows Number of rows in the table */
[[nodiscard]] ib_err_t ib_parallel_select_count_star(ib_trx_t trx, std::vector<ib_crsr_t> &crsrs, size_t n_threads, uint64_t &n_rows);

/**
 * Checks the table for errors using the given transaction and cursor.
 *
 * @param trx The InnoDB transaction to use for checking the table.
 * @param crsr The InnoDB cursor to use for checking the table.
 * @param n_threads The number of threads to use for checking the table.
 * @return The error code indicating the result of the table check.
 */
[[nodiscard]] ib_err_t ib_check_table(ib_trx_t trx, ib_crsr_t crsr, size_t n_threads);

namespace logger {

extern const char* Progname;

enum Level {
  Debug,
  Info,
  Warn,
  Error,
  Fatal
};

extern int level;

} // logger

// FIXME: Change to proper C++ style streams
/** The first argument to the InnoDB error logging function. */
using ib_stream_t = FILE *;

extern ib_stream_t ib_stream;
