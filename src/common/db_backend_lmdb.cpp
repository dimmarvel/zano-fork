// Copyright (c) 2014-2024 Zano Project
// Copyright (c) 2014-2018 The Louisdor Project 
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "db_backend_lmdb.h"
#include "misc_language.h"
#include "string_coding.h"
#include "profile_tools.h"
#include "util.h"

#define BUF_SIZE 1024
#define MAX_DBS 15

#define CHECK_AND_ASSERT_MESS_LMDB_DB(rc, ret, mess) CHECK_AND_ASSERT_MES(rc == MDB_SUCCESS, ret, "[DB ERROR]:(" << rc << ")" << mdb_strerror(rc) << ", [message]: " << mess);
#define CHECK_AND_ASSERT_THROW_MESS_LMDB_DB(rc, mess) CHECK_AND_ASSERT_THROW_MES(rc == MDB_SUCCESS, "[DB ERROR]:(" << rc << ")" << mdb_strerror(rc) << ", [message]: " << mess);
#define ASSERT_MES_AND_THROW_LMDB(rc, mess) ASSERT_MES_AND_THROW("[DB ERROR]:(" << rc << ")" << mdb_strerror(rc) << ", [message]: " << mess);

#undef LOG_DEFAULT_CHANNEL 
#define LOG_DEFAULT_CHANNEL "lmdb"
// 'lmdb' channel is disabled by default

namespace tools
{
  namespace db
  {
    lmdb_db_backend::lmdb_db_backend()= default;
    lmdb_db_backend::~lmdb_db_backend()
    {
      NESTED_TRY_ENTRY();
      close();
      NESTED_CATCH_ENTRY(__func__);
    }

    bool lmdb_db_backend::open(const std::string& path, uint64_t cache_sz)
    {
      int res = mdb_env_create(&m_env_.get());
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_env_create");

      res = mdb_env_set_maxdbs(m_env_.get(), MAX_DBS);
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_env_set_maxdbs");
      
      res = mdb_env_set_mapsize(m_env_.get(), cache_sz);
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_env_set_mapsize");
      
      m_path_ = path;
      CHECK_AND_ASSERT_MES(tools::create_directories_if_necessary(m_path_), false, "create_directories_if_necessary failed: " << m_path);
      
      res = mdb_env_open(m_env_.get(), m_path_.c_str(), MDB_NORDAHEAD, 0644);
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_env_open, m_path=" << m_path);
      
      return true;
    }

    bool lmdb_db_backend::open_container(const std::string& name, container_handle& h)
    {
      MDB_dbi dbi{};
      begin_transaction();
      int res = mdb_dbi_open(get_current_tx(), name.c_str(), MDB_CREATE, &dbi);
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_dbi_open with container name: " << name);
      commit_transaction();
      h = static_cast<container_handle>(dbi);
      return true;
    }

    bool lmdb_db_backend::close_container(container_handle& h)
    {
      static const container_handle null_handle = AUTO_VAL_INIT(null_handle);
      CHECK_AND_ASSERT_MES(h != null_handle, false, "close_container is called for null container handle");
      MDB_dbi dbi = static_cast<MDB_dbi>(h);
      begin_transaction();
      mdb_dbi_close(m_env_.get(), dbi);
      commit_transaction();
      h = null_handle;
      return true;
    }

    bool lmdb_db_backend::close()
    {
      std::lock_guard<boost::recursive_mutex> lock(m_cs);
      for (auto& [tx_thread, txs] : m_txs)
      {
        for (auto& tx_guard : txs)
        {
          if (!tx_guard.m_finalized && tx_guard.txn)
          {
            int res = tx_guard.finalize();
            if (res != MDB_SUCCESS) 
            {
              LOG_ERROR("[DB ERROR]: On close transactions: " << mdb_strerror(res));
            }
          }
        }
      }
      m_txs.clear();
      m_env_.reset();  // env_deleter closes
      return true;
    }

    bool lmdb_db_backend::begin_transaction(bool read_only)
    {
      if (!read_only)
      {
        LOG_PRINT_CYAN("[DB " << m_path_ << "] WRITE LOCKED", LOG_LEVEL_3);
        CRITICAL_SECTION_LOCK(m_write_exclusive_lock);
      }
      PROFILE_FUNC("lmdb_db_backend::begin_transaction");
      std::lock_guard<boost::recursive_mutex> lock(m_cs);
      CHECK_AND_ASSERT_THROW_MES(m_penv_, "m_penv==null, db closed");

      auto& txs = m_txs[std::this_thread::get_id()];
      MDB_txn* pparent = nullptr;
      bool parent_read_only = false;
      if(!txs.empty())
      {
        pparent = txs.back().txn;
        parent_read_only = txs.back().read_only;
      }

      if (!txs.empty() && read_only && txs.back().read_only)
      {
        ++txs.back().nesting;
      }
      else
      {
        MDB_txn* parent = nullptr;
        if (txs.empty())
        {
          parent = nullptr;
        }
        else {
          //don't use parent tx in write transactions if parent tx was read-only (restriction in lmdb) 
          //see "Nested transactions: Max 1 child, write txns only, no writemap"
          if (txs.back().read_only)
            parent = nullptr;
          else
            parent = txs.back().txn;
        }

        txn_guard guard(*this, read_only ? txn_mode::read_only : txn_mode::read_write, parent);
        txs.emplace_back(std::move(guard));
      }
      LOG_PRINT_L4("[DB] Transaction started");
      return true;
    }

    MDB_txn* lmdb_db_backend::get_current_tx()
    {
      std::lock_guard<boost::recursive_mutex> lock(m_cs);
      auto& rtxlist = m_txs[std::this_thread::get_id()];
      CHECK_AND_ASSERT_MES(rtxlist.size(), nullptr, "Unable to find active tx for thread " << std::this_thread::get_id());
      return rtxlist.back().ptx;
    }

    bool lmdb_db_backend::pop_tx_guard(txn_guard& g)
    {
      std::lock_guard<boost::recursive_mutex> lock(m_cs);
      auto it = m_txs.find(std::this_thread::get_id());
      CHECK_AND_ASSERT_MES(it != m_txs.end(), false, "[DB] Unable to find id cor current thread");
      CHECK_AND_ASSERT_MES(it->second.size(), false, "[DB] No active tx for current thread");

      txe = it->second.back();

      if (it->second.back().read_only &&  it->second.back().nesting == 0)
      {
        LOG_ERROR("Internal db tx state error: read_only and nesting readers == 0");
      }

      if ((it->second.back().read_only && it->second.back().nesting < 2) || (!it->second.back().read_only && it->second.back().nesting < 1))
      {
        it->second.pop_back();
        if (!it->second.size())
          m_txs.erase(it);       
      }
      else
      {
        --it->second.back().nesting;
      }
      return true;
    }

    bool lmdb_db_backend::commit_transaction()
    {
      PROFILE_FUNC("lmdb_db_backend::commit_transaction");
      tx_entry txe{};
      bool r = pop_tx_entry(txe);
      CHECK_AND_ASSERT_MES(r, false, "Unable to pop_tx_entry");
      if (txe.nesting == 0 || (txe.read_only && txe.nesting == 1))
      {
        if (txe.read_only)
        {
          mdb_txn_abort(txe.txn);
        }
        else
        {
          int res = mdb_txn_commit(txe.txn);
          CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_txn_commit (error " << res << ")");

          if (!txe.read_only && !txe.count)
          {
            CRITICAL_SECTION_UNLOCK(m_write_exclusive_lock);
            LOG_PRINT_CYAN("[DB " << m_path << "] WRITE UNLOCKED", LOG_LEVEL_3);
          }
        }
      }
      LOG_PRINT_L4("[DB] Transaction committed");
      return true;
    }

    void lmdb_db_backend::abort_transaction() noexcept
    {
      {
        tx_entry txe{};
        bool r = pop_tx_entry(txe);
        CHECK_AND_ASSERT_MES(r, void(), "Unable to pop_tx_entry");
        if (txe.count == 0 || (txe.read_only && txe.count == 1))
        {
          mdb_txn_abort(txe.txn);
          if (!txe.read_only)
          {
            CRITICAL_SECTION_UNLOCK(m_write_exclusive_lock);
            LOG_PRINT_CYAN("[DB " << m_path_ << "] WRITE UNLOCKED(ABORTED)", LOG_LEVEL_3);
          }
        }
      }
      LOG_PRINT_L4("[DB] Transaction aborted");
    }

    bool lmdb_db_backend::erase(container_handle h, const char* k, size_t ks)
    {
      MDB_val key = AUTO_VAL_INIT(key);
      key.mv_data = (void*)k;
      key.mv_size = ks;

      int res = mdb_del(get_current_tx(), static_cast<MDB_dbi>(h), &key, nullptr);
      if (res == MDB_NOTFOUND)
        return false;
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_del");
      return true;
    }

    bool lmdb_db_backend::have_tx()
    {
      std::lock_guard<boost::recursive_mutex> lock(m_cs);
      auto it = m_txs.find(std::this_thread::get_id());
      if (it == m_txs.end())
        return false;
      return it->second.size() ? true : false;
    }

    bool lmdb_db_backend::get(container_handle h, const char* k, size_t ks, std::string& res_buff)
    {
      PROFILE_FUNC("lmdb_db_backend::get");
      MDB_val key{}, data{};
      key.mv_data = static_cast<void*>(const_cast<char*>(k));
      key.mv_size = ks;

      bool need_to_commit = !have_tx();
      if (need_to_commit)
        begin_transaction(true);

      int res = mdb_get(get_current_tx(), static_cast<MDB_dbi>(h), &key, &data);

      if (need_to_commit)
        commit_transaction();

      if (res == MDB_NOTFOUND || res != MDB_SUCCESS) 
        return false;
  
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_get, h: " << h << ", ks: " << ks);
      res_buff.assign(static_cast<char*>(data.mv_data), data.mv_size);
      return true;
    }

    bool lmdb_db_backend::clear(container_handle h)
    {
      int res = mdb_drop(get_current_tx(), static_cast<MDB_dbi>(h), 0);
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_drop");
      return true;
    }

    uint64_t lmdb_db_backend::size(container_handle h)
    {
      PROFILE_FUNC("lmdb_db_backend::size");
      MDB_stat container_stat = AUTO_VAL_INIT(container_stat);
      bool need_to_commit = !have_tx();
      if (need_to_commit)
        begin_transaction(true);
      int res = mdb_stat(get_current_tx(), static_cast<MDB_dbi>(h), &container_stat);
      if (need_to_commit)
        commit_transaction();
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_stat");
      return container_stat.ms_entries;
    }

    bool lmdb_db_backend::set(container_handle h, const char* k, size_t ks, const char* v, size_t vs)
    {
      PROFILE_FUNC("lmdb_db_backend::set");
      int res = 0;
      MDB_val key{};
      MDB_val data[2] = {}; // mdb_put may access data[1] if some flags are set, this may trigger static code analizers, so here we allocate two elements to avoid it
      key.mv_data = static_cast<void*>(const_cast<char*>(k));
      key.mv_size = ks;
      data[0].mv_data = static_cast<void*>(const_cast<char*>(v));
      data[0].mv_size = vs;

      res = mdb_put(get_current_tx(), static_cast<MDB_dbi>(h), &key, data, 0);
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_put");
      return true;
    }

    bool lmdb_db_backend::enumerate(container_handle h, i_db_callback* pcb)
    {
      CHECK_AND_ASSERT_MES(pcb, false, "null capback ptr passed to enumerate");
      MDB_val key{}, data{};

      bool need_to_commit = !have_tx();
      if (need_to_commit)
        begin_transaction(true);
      MDB_cursor* cursor_ptr = nullptr;
      int res = mdb_cursor_open(get_current_tx(), static_cast<MDB_dbi>(h), &cursor_ptr);
      CHECK_AND_ASSERT_MESS_LMDB_DB(res, false, "Unable to mdb_cursor_open");
      CHECK_AND_ASSERT_MES(cursor_ptr, false, "cursor_ptr is null after mdb_cursor_open");

      while ((rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT)) == MDB_SUCCESS)
      {
        if (!cb->on_enum_item(count++, key.mv_data, key.mv_size, data.mv_data, data.mv_size))
          break;
      }
      mdb_cursor_close(cursor_ptr);
      if (need_to_commit)
        commit_transaction();
      return true;
    }

    bool lmdb_db_backend::get_stat_info(tools::db::stat_info& si)
    {
      si = {};

      MDB_envinfo ei{};
      mdb_env_info(m_penv, &ei);
      si.map_size = ei.me_mapsize;
      
      std::lock_guard<boost::recursive_mutex> lock(m_cs);
      for (auto& e : m_txs)
      {
        for (auto& pr : e.second)
        {
          ++si.tx_count;
          if(!pr.read_only)
            ++si.write_tx_count;
        }
      }
      return true;
    }

    const char* lmdb_db_backend::name()
    {
      return "lmdb";
    }

    bool lmdb_db_backend::convert_db_4kb_page_to_16kb_page(const std::string& source_path, const std::string& destination_path)
    {
      #define MDB_CHECK(x, msg) {int rc = x; CHECK_AND_ASSERT_MES(rc == MDB_SUCCESS, false, "LMDB 4k->16k error: " << msg << ": " << mdb_strerror(rc));}

      MDB_env *env_src = nullptr, *env_dst = nullptr;

      // source
      MDB_CHECK(mdb_env_create(&env_src), "failed to create LMDB environment");
      MDB_CHECK(mdb_env_set_mapsize(env_src, 4 * 1024 * 1024), "failed to set mapsize"); // mapsize ?
      MDB_CHECK(mdb_env_open(env_src, source_path.c_str(), 0, 0664), "failed to open source LMDB");

      // destination (16k page size)
      MDB_CHECK(mdb_env_create(&env_dst), "failed to create LMDB environment");
      MDB_CHECK(mdb_env_set_mapsize(env_dst, 16 * 1024 * 1024), "failed to set mapsize"); // mapsize ?
      
      // TODO uncomment after mdb_env_set_pagesize is supported
      // MDB_CHECK(mdb_env_set_pagesize(env_dst, 16 * 1024), "failed to set page size to 16K");
      
      MDB_CHECK(mdb_env_open(env_dst, destination_path.c_str(), 0, 0664), "failed to open destination LMDB");

      // begin transactions
      MDB_txn *txn_src = nullptr, *txn_dst = nullptr;
      MDB_dbi dbi_src, dbi_dst;
      MDB_CHECK(mdb_txn_begin(env_src, nullptr, MDB_RDONLY, &txn_src), "failed to begin source transaction");
      MDB_CHECK(mdb_dbi_open(txn_src, nullptr, 0, &dbi_src), "failed to open source database");
      MDB_CHECK(mdb_txn_begin(env_dst, nullptr, 0, &txn_dst), "failed to begin destination transaction");
      MDB_CHECK(mdb_dbi_open(txn_dst, nullptr, MDB_CREATE, &dbi_dst), "failed to open destination database");

      MDB_cursor *cursor;
      MDB_val key, data;

      // Iterate over the source database and copy all key-value pairs to the destination database
      MDB_CHECK(mdb_cursor_open(txn_src, dbi_src, &cursor), "failed to open cursor");

      while (mdb_cursor_get(cursor, &key, &data, MDB_NEXT) == MDB_SUCCESS)
      {
        MDB_CHECK(mdb_put(txn_dst, dbi_dst, &key, &data, 0), "failed to put data in destination database");
      }

      mdb_cursor_close(cursor);

      // commit transactions
      MDB_CHECK(mdb_txn_commit(txn_src), "failed to commit source transaction");
      MDB_CHECK(mdb_txn_commit(txn_dst), "failed to commit destination transaction");

      mdb_dbi_close(env_src, dbi_src);
      mdb_dbi_close(env_dst, dbi_dst);
      mdb_env_close(env_src);
      mdb_env_close(env_dst);

      return true;

      #undef MDB_CHECK
    }

    lmdb_db_backend::txn_guard::txn_guard(lmdb_db_backend& db, txn_mode mode, MDB_txn* parent)
      : m_db(db), read_only(mode == txn_mode::read_only)
    {
      unsigned flags = read_only ? MDB_RDONLY : 0;
      int res = mdb_txn_begin(m_db.m_env_.get(), parent, flags, &txn);
      if (res != MDB_SUCCESS)
      {
        if (!read_only)
        {
          CRITICAL_SECTION_UNLOCK(m_db.m_write_exclusive_lock);
        }
        ASSERT_MES_AND_THROW_LMDB(res, "mdb_txn_begin failed");
      }
      // nesting starts at 0; nested read-only increments in begin_transaction()
    }

    lmdb_db_backend::lmdb_txn::lmdb_txn(lmdb_db_backend& db, bool read_only, MDB_txn* parent_tx)
      : m_db(db), m_finalized(false)
    {
      unsigned int flags = read_only ? MDB_RDONLY : 0;
      MDB_txn* new_tx = nullptr;
      int res = mdb_txn_begin(m_db.m_penv, parent_tx, flags, &new_tx);
      if (res != MDB_SUCCESS)
      {
        if (!read_only)
        {
          //Important: if mdb_txn_begin is failed need to unlock previously locked mutex
          CRITICAL_SECTION_UNLOCK(m_db.m_write_exclusive_lock);
        }
        //throw exception to avoid regular code execution 
        ASSERT_MES_AND_THROW_LMDB(res, "Unable to mdb_txn_begin");
      }
    }

    void begin()
    {
      unsigned flags = read_only ? MDB_RDONLY : 0;
      int rc = mdb_txn_begin(m_db.m_env_.get(), parent_tx, flags, &txn);
      if (rc != MDB_SUCCESS) {
        if (!read_only) 
          CRITICAL_SECTION_UNLOCK(m_db.m_write_exclusive_lock);
        ASSERT_MES_AND_THROW_LMDB(rc, "mdb_txn_begin failed");
      }
    }

    bool lmdb_db_backend::txn_guard::finalize()
    {
      if (!txn)
        return true;
      if (read_only)
      {
        mdb_txn_abort(txn);
      }
      else
      {
        int res = mdb_txn_commit(txn);
        ASSERT_MES_AND_THROW_LMDB(res, "mdb_txn_commit finalize failed");
      }
      m_finalized = true;
      return m_finalized;
    }

    void lmdb_db_backend::txn_guard::commit_or_abort()
    {
      if (read_only) 
      {
        mdb_txn_abort(txn);
      }
      else
      {
        int res = mdb_txn_commit(txn);
        ASSERT_MES_AND_THROW_LMDB(res, "mdb_txn_commit commit_or_abort failed");
        CRITICAL_SECTION_UNLOCK(m_db.m_write_exclusive_lock);
      }
      m_finalized = true;
    }

    lmdb_db_backend::txn_guard::~txn_guard() noexcept
    {
      if (!m_finalized && txn)
      {
        mdb_txn_abort(txn);
        if (!read_only)
        {
          CRITICAL_SECTION_UNLOCK(m_db.m_write_exclusive_lock);
        }
        LOG_PRINT_L4("[DB] Transaction aborted in destructor");
      }
    }
  }
}

#undef LOG_DEFAULT_CHANNEL 
#define LOG_DEFAULT_CHANNEL NULL
