/****************************************************************************
Copyright (c) 1996, 2009, Innobase Oy. All Rights Reserved.
Copyright (c) 2024 Sunny Bains. All rights reserved.

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

/** @file include/dict0load.h
Loads to the memory cache database object definitions
from dictionary tables

Created 4/24/1996 Heikki Tuuri
*******************************************************/

#pragma once

#include "dict0types.h"
#include "mem0mem.h"
#include "srv0srv.h"
#include "ut0byte.h"

/** In a crash recovery we already have all the tablespace objects created.
This function compares the space id information in the InnoDB data dictionary
to what we already read with srv_fil->load_single_table_tablespaces().

In a normal startup, we create the tablespace objects for every table in
InnoDB's data dictionary, if the corresponding .ibd file exists.
We also scan the biggest space id, and store it to srv_fil->system. */
void dict_check_tablespaces_and_store_max_id(bool in_crash_recovery); /*!< in: are we doing a crash recovery */

/** Finds the first table name in the given database.
@return own: table name, nullptr if does not exist; the caller must free
the memory in the string! */
char *dict_get_first_table_name_in_db(const char *name); /*!< in: database name which ends to '/' */

/** Loads a table definition and also all its index definitions, and also
the cluster definition if the table is a member in a cluster. Also loads
all foreign key constraints where the foreign key is in the table or where
a foreign key references columns in this table.
@return table, nullptr if does not exist; if the table is stored in an
.ibd file, but the file does not exist, then we set the
ibd_file_missing flag true in the table object we return */
dict_table_t *dict_load_table(
  ib_recovery_t recovery, /*!< in: recovery flag */
  const char *name
); /*!< in: table name in the
                                        databasename/tablename format */

/** Loads a table object based on the table id.
@return	table; nullptr if table does not exist */
dict_table_t *dict_load_table_on_id(
  ib_recovery_t recovery, /*!< in: recovery flag */
  uint64_t table_id
); /*!< in: table id */

/** This function is called when the database is booted.
Loads system table index definitions except for the clustered index which
is added to the dictionary cache at booting before calling this function. */
void dict_load_sys_table(dict_table_t *table); /*!< in: system table */

/** Loads foreign key constraints where the table is either the foreign key
holder or where the table is referenced by a foreign key. Adds these
constraints to the data dictionary. Note that we know that the dictionary
cache already contains all constraints where the other relevant table is
already in the dictionary cache.
@return	DB_SUCCESS or error code */
db_err dict_load_foreigns(
  const char *table_name, /*!< in: table name */
  bool check_charsets
); /*!< in: true=check charsets
                                                   compatibility */

/** Prints to the standard output information on all tables found in the data
dictionary system table. */
void dict_print();
