// Copyright (c) 2014-2024 Zano Project
// Copyright (c) 2014-2018 The Louisdor Project 
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include  <thread>

#include "include_base_utils.h"

#include "db_backend_base.h"
#include "db/liblmdb/lmdb.h"


namespace tools
{
  namespace db
  {
    enum class txn_mode { read_only, read_write };
    class lmdb_db_backend : public i_db_backend
    {
      using txn_list = std::list<txn_guard>;
      using txn_map  = std::unordered_map<std::thread::id, txn_list>;

      struct env_deleter
      { 
        void operator()(MDB_env* e) const noexcept 
        {
          if (e)
          {
            mdb_env_close(e);
            e = nullptr;
          }
        }
      };

      std::string m_path_;
      std::unique_ptr<MDB_env, env_deleter> m_env_;
      mutable boost::recursive_mutex m_cs;
      boost::recursive_mutex m_write_exclusive_lock;
      txn_map m_txs; // size_t -> count of nested read_only transactions

      bool pop_tx_guard(txn_guard& g) noexcept;

      struct txn_guard
      {
        txn_guard(lmdb_db_backend& db, txn_mode mode, MDB_txn* parent);
        ~txn_guard();  // abort if not committed

        bool begin();
        bool finalize();        // commit (or abort for read-only) without unlocking
        void commit_or_abort(); // commit (or abort for read-only) + unlock

        MDB_txn* txn{nullptr};
        bool read_only;
        size_t nesting{0};
      private:
        lmdb_db_backend& m_db;
        bool m_finalized{false};
      };

    public:
      lmdb_db_backend();
      lmdb_db_backend(const lmdb_db_backend&) = delete;
      lmdb_db_backend& operator=(const lmdb_db_backend&) = delete;
      lmdb_db_backend(lmdb_db_backend&&) = default;
      lmdb_db_backend& operator=(lmdb_db_backend&&) = default;
      ~lmdb_db_backend() override;

      //----------------- i_db_backend -----------------------------------------------------
      bool close() noexcept override;
      bool begin_transaction(bool read_only = false) override;
      bool commit_transaction() override;
      void abort_transaction() noexcept override;
      bool open(const std::string& path, uint64_t cache_sz = CACHE_SIZE) override;
      bool open_container(const std::string& name, container_handle& h) override;
      bool close_container(container_handle& h) override;
      bool erase(container_handle h, const char* k, size_t s) override;
      bool get(container_handle h, const char* k, size_t s, std::string& res_buff) override;
      bool clear(container_handle h) override;
      uint64_t size(container_handle h) override;
      bool set(container_handle h, const char* k, size_t s, const char* v, size_t vs) override;
      bool enumerate(container_handle h, i_db_callback* pcb) override;
      bool get_stat_info(tools::db::stat_info& si) override;
      const char* name() noexcept override;
      //-------------------------------------------------------------------------------------
      bool have_tx() const noexcept;
      MDB_txn* get_current_tx() const noexcept;

      static bool convert_db_4kb_page_to_16kb_page(const std::string& source_path, const std::string& destination_path);

    };
  }
}
