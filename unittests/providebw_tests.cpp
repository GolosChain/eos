#include <boost/test/unit_test.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/chain/abi_serializer.hpp>
#include <eosio/chain/resource_limits.hpp>

#include <eosio.system/eosio.system.wast.hpp>
#include <eosio.system/eosio.system.abi.hpp>
// These contracts are still under dev
#include <eosio.bios/eosio.bios.wast.hpp>
#include <eosio.bios/eosio.bios.abi.hpp>
#include <eosio.token/eosio.token.wast.hpp>
#include <eosio.token/eosio.token.abi.hpp>
#include <eosio.msig/eosio.msig.wast.hpp>
#include <eosio.msig/eosio.msig.abi.hpp>

#include <Runtime/Runtime.h>

#include <fc/variant_object.hpp>

#ifdef NON_VALIDATING_TEST
#define TESTER tester
#else
#define TESTER validating_tester
#endif


using namespace eosio;
using namespace eosio::chain;
using namespace eosio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

class system_contract_tester : public TESTER {
public:

   fc::variant get_global_state() {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, N(global), N(global) );
      if (data.empty()) std::cout << "\nData is empty\n" << std::endl;
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "eosio_global_state", data, abi_serializer_max_time );

   }

    auto buyram( name payer, name receiver, asset ram ) {
       auto r = base_tester::push_action(config::system_account_name, N(buyram), payer, mvo()
                    ("payer", payer)
                    ("receiver", receiver)
                    ("quant", ram)
                    );
       produce_block();
       return r;
    }

    auto delegate_bandwidth( name from, name receiver, asset net, asset cpu, uint8_t transfer = 1) {
       auto r = base_tester::push_action(config::system_account_name, N(delegatebw), from, mvo()
                    ("from", from )
                    ("receiver", receiver)
                    ("stake_net_quantity", net)
                    ("stake_cpu_quantity", cpu)
                    ("transfer", transfer)
                    );
       produce_block();
       return r;
    }

    void create_currency( name contract, name manager, asset maxsupply, const private_key_type* signer = nullptr ) {
        auto act =  mutable_variant_object()
                ("issuer",       manager )
                ("maximum_supply", maxsupply );

        base_tester::push_action(contract, N(create), contract, act );
    }

    auto issue( name contract, name manager, name to, asset amount ) {
       auto r = base_tester::push_action( contract, N(issue), manager, mutable_variant_object()
                ("to",      to )
                ("quantity", amount )
                ("memo", "")
        );
        produce_block();
        return r;
    }

    auto set_privileged( name account ) {
       auto r = base_tester::push_action(config::system_account_name, N(setpriv), config::system_account_name,  mvo()("account", account)("is_priv", 1));
       produce_block();
       return r;
    }

    asset get_balance( const account_name& act ) {
         return get_currency_balance(N(eosio.token), symbol(CORE_SYMBOL), act);
    }

    void set_code_abi(const account_name& account, const char* wast, const char* abi, const private_key_type* signer = nullptr) {
       wdump((account));
        set_code(account, wast, signer);
        set_abi(account, abi, signer);
        if (account == config::system_account_name) {
           const auto& accnt = control->db().get<account_object,by_name>( account );
           abi_def abi_definition;
           BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi_definition), true);
           abi_ser.set_abi(abi_definition, abi_serializer_max_time);
        }
        produce_blocks();
    }

    abi_serializer abi_ser;
};

BOOST_AUTO_TEST_SUITE(providebw_tests)

BOOST_FIXTURE_TEST_CASE( providebw_test, system_contract_tester ) {
    try {
        // Create eosio.msig and eosio.token
        create_accounts({N(eosio.msig), N(eosio.token), N(eosio.ram), N(eosio.ramfee), N(eosio.stake), N(eosio.vpay), N(eosio.bpay), N(eosio.saving) });

        // Set code for the following accounts:
        //  - eosio (code: eosio.bios) (already set by tester constructor)
        //  - eosio.msig (code: eosio.msig)
        //  - eosio.token (code: eosio.token)
        set_code_abi(N(eosio.msig), eosio_msig_wast, eosio_msig_abi);//, &eosio_active_pk);
        set_code_abi(N(eosio.token), eosio_token_wast, eosio_token_abi); //, &eosio_active_pk);

        // Set privileged for eosio.msig and eosio.token
        set_privileged(N(eosio.msig));
        set_privileged(N(eosio.token));

        // Verify eosio.msig and eosio.token is privileged
        const auto& eosio_msig_acc = get<account_object, by_name>(N(eosio.msig));
        BOOST_TEST(eosio_msig_acc.privileged == true);
        const auto& eosio_token_acc = get<account_object, by_name>(N(eosio.token));
        BOOST_TEST(eosio_token_acc.privileged == true);


        // Create SYS tokens in eosio.token, set its manager as eosio
        auto max_supply = core_from_string("10000000000.0000"); /// 1x larger than 1B initial tokens
        auto initial_supply = core_from_string("1000000000.0000"); /// 1x larger than 1B initial tokens
        create_currency(N(eosio.token), config::system_account_name, max_supply);
        // Issue the genesis supply of 1 billion SYS tokens to eosio.system
        issue(N(eosio.token), config::system_account_name, config::system_account_name, initial_supply);

        auto actual = get_balance(config::system_account_name);
        BOOST_REQUIRE_EQUAL(initial_supply, actual);

        create_accounts({N(provider), N(user)});

        // Set eosio.system to eosio
        set_code_abi(config::system_account_name, eosio_system_wast, eosio_system_abi);

        {
            auto r = buyram(config::system_account_name, N(provider), asset(1000));
            BOOST_REQUIRE( !r->except_ptr );

            r = delegate_bandwidth(N(eosio.stake), N(provider), asset(1000000), asset(100000));
            BOOST_REQUIRE( !r->except_ptr );

            r = buyram(config::system_account_name, N(user), asset(1000));
            BOOST_REQUIRE( !r->except_ptr );
        }

        auto& rlm = control->get_resource_limits_manager();
        auto provider_cpu = rlm.get_account_cpu_limit_ex(N(provider));
        auto provider_net = rlm.get_account_net_limit_ex(N(provider));

        BOOST_CHECK_GT(provider_cpu.available, 0);
        BOOST_CHECK_GT(provider_net.available, 0);

        auto user_cpu = rlm.get_account_cpu_limit_ex(N(user));
        auto user_net = rlm.get_account_net_limit_ex(N(user));

        BOOST_CHECK_EQUAL(user_cpu.available, 0);
        BOOST_CHECK_EQUAL(user_net.available, 0);

        // Check that user can't send transaction due missing bandwidth
        variant pretty_trx = fc::mutable_variant_object()
           ("actions", fc::variants({
              fc::mutable_variant_object()
                 ("account", name(config::system_account_name))
                 ("name", "reqauth")
                 ("authorization", fc::variants({
                    fc::mutable_variant_object()
                       ("actor", "user")
                       ("permission", name(config::active_name))
                 }))
                 ("data", fc::mutable_variant_object()
                    ("from", "user")
                 )
              })
          );
        signed_transaction trx;
        abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer_max_time);
        set_transaction_headers(trx);
        trx.sign( get_private_key("user", "active"), control->get_chain_id() );
        BOOST_REQUIRE_EXCEPTION( push_transaction(trx), tx_net_usage_exceeded, [](auto&){return true;});

        trx.actions.emplace_back( vector<permission_level>{{"provider", config::active_name}},
                                  providebw(N(provider)));
        set_transaction_headers(trx);

        // Check that user can publish transaction using provider bandwidth
        BOOST_TEST_MESSAGE("user public key: " <<  get_public_key("user", "active"));
        BOOST_TEST_MESSAGE("provider public key: " <<  get_public_key("provider", "active"));
        trx.signatures.clear();
        trx.sign( get_private_key("user", "active"), control->get_chain_id() );
        trx.sign( get_private_key("provider", "active"), control->get_chain_id() );
        auto r = push_transaction( trx );

        BOOST_REQUIRE( !r->except_ptr );

        auto provider_cpu2 = rlm.get_account_cpu_limit_ex(N(provider));
        auto provider_net2 = rlm.get_account_net_limit_ex(N(provider));

        auto user_cpu2 = rlm.get_account_cpu_limit_ex(N(user));
        auto user_net2 = rlm.get_account_net_limit_ex(N(user));

        BOOST_CHECK_EQUAL(user_cpu2.used, user_cpu.used);
        BOOST_CHECK_EQUAL(user_net2.used, user_net.used);

        BOOST_CHECK_GT(provider_cpu2.used, provider_cpu.used);
        BOOST_CHECK_GT(provider_net2.used, provider_net.used);

    } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()