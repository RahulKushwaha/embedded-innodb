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

/** @file dyn/dyn0dyn.c
The dynamically allocated array

Created 2/5/1996 Heikki Tuuri
*******************************************************/

#include "dyn0dyn.h"

dyn_block_t *dyn_array_add_block(dyn_array_t *arr) {
  ut_ad(arr);
  ut_ad(arr->magic_n == DYN_BLOCK_MAGIC_N);

  if (arr->heap == nullptr) {
    UT_LIST_INIT(arr->base);
    UT_LIST_ADD_FIRST(arr->base, arr);

    arr->heap = mem_heap_create(sizeof(dyn_block_t));
  }

  auto block = dyn_array_get_last_block(arr);

  block->used = block->used | DYN_BLOCK_FULL_FLAG;

  auto heap = arr->heap;

  block = (dyn_block_t *)mem_heap_alloc(heap, sizeof(dyn_block_t));

  block->used = 0;

  UT_LIST_ADD_LAST(arr->base, block);

  return block;
}
