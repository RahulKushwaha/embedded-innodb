/** Copyright (c) 2024 Sunny Bains. All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <trx0rseg.h>

#include <vector>

#include "innodb0types.h"

#include "lock0lock.h"
#include "srv0srv.h"
#include "trx0trx.h"

constexpr int N_TRXS = 8;
constexpr int N_ROW_LOCKS = 1;
constexpr int REC_BITMAP_SIZE = 104;

#define kernel_mutex_enter()        \
  do {                              \
    mutex_enter(kernel_mutex_temp); \
  } while (false)

#define kernel_mutex_exit()        \
  do {                             \
    mutex_exit(kernel_mutex_temp); \
  } while (false)

namespace test {

/** Creates and initializes a transaction instance
@return	own: the transaction */
Trx *trx_create() {
#if 0
  auto trx = reinterpret_cast<Trx*>(::malloc(sizeof(Trx)));

  trx_init(trx);
#else
  auto trx = srv_trx_sys->create_user_trx(nullptr);
#endif

  return trx;
}

/** Free the transaction object.
@param[in,own] trx              Free the transaction. */
void trx_free(Trx *&trx) {
  srv_trx_sys->destroy_user_trx(trx);
  ut_a(trx == nullptr);
}

/** Setup the test transaction for the simulation.
@param[in,out] trx              Transaction to setup.
@param[in] n_row_locks          Number of row locks to create. */
void trx_setup(Trx *trx, int n_row_locks) {
  for (int i = 0; i < n_row_locks; ++i) {
    auto mode = LOCK_S;
    space_id_t space = random() % 100;
    page_no_t page_no = random() % 1000;
    auto heap_no = random() % REC_BITMAP_SIZE;

    if (!(i % 50)) {
      mode = LOCK_X;
    }

    kernel_mutex_enter();

    std::cout << "REC LOCK CREATE: " << i << "\n";

    /* Pass nullptr index handle. */
    (void)srv_lock_sys->rec_create_low({space, page_no}, mode, heap_no, REC_BITMAP_SIZE, nullptr, trx);

    kernel_mutex_exit();
  }
}

DTuple *create_search_tuple() {
  auto heap = mem_heap_create(200);
  auto tuple = dtuple_create(heap, 1);
  tuple->fields = new dfield_t[2];
  tuple->fields[0].data = new int(5);
  tuple->fields[0].len = sizeof(int);
  tuple->fields[0].type = dtype_t{DATA_INT};

  return tuple;
}

DTuple *create_tuple() {
  auto heap = mem_heap_create(200);
  auto tuple = dtuple_create(heap, 2);
  tuple->fields = new dfield_t[2];
  tuple->fields[0].data = new int(5);
  tuple->fields[0].len = sizeof(int);
  tuple->fields[0].type = dtype_t{DATA_INT};

  tuple->fields[1].data = new int(6);
  tuple->fields[1].len = sizeof(int);
  tuple->fields[1].type = dtype_t{DATA_INT};

  return tuple;
}

/** Create N_TRXS transactions and create N_ROW_LOCKS rec locks on
random space/page_no/heap_no. Set the wait bit for locks that
clash. Select a random transaction and check if there are any
other transactions that are waiting on its locks. */
void run_1(Btree *btree) {
  ulint num_cols{2};
  space_id_t space_id{0};
  std::string table_name{"test/t1"};
  {
    std::string file_path = std::format("{}{}.ibd", srv_config.m_data_home, table_name);
    if (std::filesystem::exists(file_path)) {
      std::filesystem::remove(file_path);
    }
  }
  srv_fil->create_new_single_table_tablespace(&space_id, table_name.c_str(), false, 0, FIL_IBD_FILE_INITIAL_SIZE);

  mtr_t mtr{};

  mtr.start();
  srv_fsp->header_init(space_id, FIL_IBD_FILE_INITIAL_SIZE, &mtr);
  mtr.commit();

  auto table = Table::create(table_name.c_str(), space_id, num_cols, 0, false, Current_location());
  for (ulint i = 0; i < num_cols; i++) {
    table->add_col(std::format("col_{}", i).c_str(), DATA_INT, 0, 4);
  }

  std::string primary_index{"primary_idx"};
  auto index = Index::create(table, primary_index.c_str(), Page_id(space_id, 0), DICT_CLUSTERED, 1);
  index->add_col(table, table->get_nth_col(0), 0);
  index->m_n_uniq = 1;

  mtr.start();
  page_no_t page_no = btree->create(1, space_id, index->m_id, index, &mtr);
  index->m_page_id.m_page_no = page_no;
  std::cout << "space_id: " << index->m_page_id.m_space_id << " page_id: " << index->m_page_id.m_page_no << std::endl;

  Btree_cursor cursor(srv_fsp, btree);
  cursor.m_index = index;
  auto tuple = create_tuple();
  rec_t *rec{nullptr};
  big_rec_t *big_rec{nullptr};

  que_thr_t que_thr{};
  que_thr.graph = new que_t{};
  que_thr.graph->trx = trx_create();
  que_thr.graph->trx->m_rseg = new trx_rseg_t{};

  rw_lock_create(&index->m_lock, SYNC_INDEX_TREE);
  index->m_cached = true;

  auto search_tuple = create_search_tuple();
  cursor.search_to_nth_level(nullptr, index, 0, search_tuple, PAGE_CUR_LE, BTR_MODIFY_LEAF | BTR_INSERT, &mtr, Current_location());

  auto insert_flags{BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG};
  auto optimistic_insert = cursor.optimistic_insert(insert_flags, tuple, &rec, &big_rec, 0, &que_thr, &mtr);
  std::cout << "insert result: " << optimistic_insert << std::endl;
  // page_t *root = btree->root_get(index->m_page_id, &mtr);
  // btree->print_size(index);
  btree->print_index(index, 100);
  std::cout << page_no << std::endl;

  mtr.commit();
}

}  // namespace test

int main() {
  srandom(time(nullptr));

  /* Note: The order of initializing and close of the sub-systems is very important. */

  // Startup
  ut_mem_init();

  os_sync_init();

  srv_config.m_max_n_threads = N_TRXS;

  sync_init();

  kernel_mutex_temp = static_cast<mutex_t *>(mem_alloc(sizeof(mutex_t)));

  mutex_create(&kernel_mutex, IF_DEBUG("kernel_mutex", ) IF_SYNC_DEBUG(SYNC_KERNEL, ) Current_location());

  {
    srv_config.m_buf_pool_size = 64 * 1024 * 1024;

    srv_buf_pool = new (std::nothrow) Buf_pool();
    ut_a(srv_buf_pool != nullptr);

    auto success = srv_buf_pool->open(srv_config.m_buf_pool_size);
    ut_a(success);
  }

  srv_config.m_log_buffer_size = 1024;
  auto log = Log::create();
  auto fil = new Fil(100);
  srv_fil = fil;
  auto fsp = FSP::create(log, fil, srv_buf_pool);
  srv_fsp = fsp;

  auto aio = AIO::create(2, 2, 2);
  srv_aio = aio;

  srv_lock_timeout_thread_event = os_event_create(nullptr);

  srv_trx_sys = Trx_sys::create(srv_fsp);
  srv_trx_sys->m_fsp = fsp;
  ut_a(srv_trx_sys->m_fsp);

  srv_lock_sys = Lock_sys::create(srv_trx_sys, 1024 * 1024);

  kernel_mutex_enter();

  UT_LIST_INIT(srv_trx_sys->m_client_trx_list);

  kernel_mutex_exit();

  auto btree = Btree::create(srv_lock_sys, fsp, srv_buf_pool);

  // Run the test
  char data_home[]{"/tmp/"};
  srv_config.m_data_home = data_home;
  test::run_1(btree);

  // Shutdown
  Lock_sys::destroy(srv_lock_sys);

  Trx_sys::destroy(srv_trx_sys);

  Btree::destroy(btree);
  FSP::destroy(fsp);
  delete fil;
  Log::destroy(log);

  AIO::destroy(aio);

  mutex_free(&kernel_mutex);

  mem_free(kernel_mutex_temp);
  kernel_mutex_temp = nullptr;

  os_event_free(srv_lock_timeout_thread_event);
  srv_lock_timeout_thread_event = nullptr;

  srv_buf_pool->close();

  sync_close();

  os_sync_free();

  delete srv_buf_pool;

  ut_delete_all_mem();

  exit(EXIT_SUCCESS);
}
