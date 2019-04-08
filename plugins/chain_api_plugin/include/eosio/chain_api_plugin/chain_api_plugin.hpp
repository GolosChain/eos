/**
 *  @file
 *  @copyright defined in eos/LICENSE
 */
#pragma once

#include <eosio/http_plugin/http_plugin.hpp>

#include <memory>

#include <appbase/application.hpp>
#include <eosio/chain/controller.hpp>

namespace eosio {
   class chain_api_plugin : public plugin<chain_api_plugin> {
      public:
        APPBASE_PLUGIN_REQUIRES((http_plugin))

        chain_api_plugin();
        virtual ~chain_api_plugin();

        virtual void set_program_options(options_description&, options_description&) override;

        void plugin_initialize(const variables_map&);
        void plugin_startup();
        void plugin_shutdown() {}

      private:
        fc::unique_ptr<class chain_api_plugin_impl> my;
   };

}


