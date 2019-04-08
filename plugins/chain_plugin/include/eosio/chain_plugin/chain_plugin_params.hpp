#pragma once

#include <boost/container/flat_set.hpp>

#include <eosio/chain/types.hpp>
#include <eosio/chain/symbol.hpp>

#include <fc/optional.hpp>

namespace eosio {

    struct get_account_params {
       chain::name                 account_name;
       fc::optional<chain::symbol> expected_core_symbol;
    };


    struct get_info_params{};

    struct get_block_params {
       std::string block_num_or_id;
    };

    struct get_block_header_state_params {
       std::string block_num_or_id;
    };

    using push_transaction_params = fc::variant_object;

    using push_block_params = chain::signed_block;

    using push_transactions_params  = std::vector<push_transaction_params>;

}

FC_REFLECT( eosio::get_account_params, (account_name)(expected_core_symbol) )
FC_REFLECT_EMPTY(eosio::get_info_params )
FC_REFLECT(eosio::get_block_params, (block_num_or_id))
FC_REFLECT(eosio::get_block_header_state_params, (block_num_or_id))
