#include <iostream>
#include <string>
#include <cstring>

#include "misc_log_ex.h"
#include "storages/parserse_base_utils.h"
#include "storages/portable_storage.h"
#include "serialization/keyvalue_serialization.h"
#include "crypto/hash.h"
#include "rpc/core_rpc_server_commands_defs.h"

int main() {
    currency::COMMAND_RPC_GET_TX_POOL::request req;
    std::string buf;
    epee::serialization::store_t_to_binary(req, buf);
    std::ofstream("get_tx_pool.bin", std::ios::binary).write(buf.data(), buf.size());

    return 0;
}
