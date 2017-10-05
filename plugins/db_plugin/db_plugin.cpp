#include <eos/db_plugin/db_plugin.hpp>
#include <eos/chain/chain_controller.hpp>
#include <eos/chain/config.hpp>
#include <eos/chain/exceptions.hpp>
#include <eos/chain/transaction.hpp>
#include <eos/chain/types.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>
#include <fc/variant.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <eos/chain/multi_index_includes.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/algorithm_ext.hpp>
#include <boost/thread/thread.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>

namespace fc { class variant; }

namespace eos {

using chain::AccountName;
using chain::FuncName;
using chain::block_id_type;
using chain::PermissionName;
using chain::ProcessedTransaction;
using chain::signed_block;
using chain::transaction_id_type;

class db_plugin_impl {
public:
   db_plugin_impl();
   ~db_plugin_impl();

   void applied_irreversible_block(const signed_block&);
   void process_irreversible_block(const signed_block&);

   void init();
   void wipe_database();

   bool wipe_database_on_startup{false};
   mongocxx::instance mongo_inst;
   mongocxx::client mongo_conn;
   std::set<AccountName> filter_on;
   std::unique_ptr<boost::lockfree::spsc_queue<signed_block>> queue;
   boost::thread consum_thread;
   boost::atomic<bool> startup{true};
   boost::atomic<bool> done{false};

   void consum_blocks();

   bool is_scope_relevant(const eos::types::Vector<AccountName>& scope);
   void update_account(const chain::Message& msg);

   static const FuncName newaccount;
   static const FuncName transfer;

   static const std::string db_name;
   static const std::string blocks_col;
   static const std::string trans_col;
   static const std::string msgs_col;
   static const std::string accounts_col;
};

const FuncName db_plugin_impl::newaccount = "newaccount";
const FuncName db_plugin_impl::transfer = "transfer";

const std::string db_plugin_impl::db_name = "EOS";
const std::string db_plugin_impl::blocks_col = "Blocks";
const std::string db_plugin_impl::trans_col = "Transactions";
const std::string db_plugin_impl::msgs_col = "Messages";
const std::string db_plugin_impl::accounts_col = "Accounts";


void db_plugin_impl::applied_irreversible_block(const signed_block& block) {
   if (startup) {
      // on startup we don't want to queue, instead push back on caller
      process_irreversible_block(block);
   } else {
      if (!queue->push(block)) {
         // TODO what to do if full
         elog("queue is full!!!!!");
         FC_ASSERT(false, "queue is full");
      }
   }
}

void db_plugin_impl::consum_blocks() {
   signed_block block;
   while (!done) {
      while (queue->pop(block)) {
         ilog("queue size: ${q}", ("q", queue->read_available()+1));
         process_irreversible_block(block);
      }
   }
   while (queue->pop(block)) {
      process_irreversible_block(block);
   }
   ilog("db_plugin consum thread shutdown gracefully");
}

//
// Blocks
//    block_num         int32
//    block_id          sha256
//    prev_block_id     sha256
//    timestamp         sec_since_epoch
//    transaction_merkle_root  sha256
//    producer_account_id      string
void db_plugin_impl::process_irreversible_block(const signed_block& block)
{
   using namespace bsoncxx::types;
   using namespace bsoncxx::builder;

   bool transactions_in_block = false;
   mongocxx::options::bulk_write bulk_opts;
   bulk_opts.ordered(false);
   mongocxx::bulk_write bulk_trans{bulk_opts};

   auto blocks = mongo_conn[db_name][blocks_col]; // Blocks
   auto trans = mongo_conn[db_name][trans_col]; // Transactions
   auto msgs = mongo_conn[db_name][msgs_col]; // Messages

   stream::document block_doc{};
   const auto block_id = block.id();
   const auto block_id_str = block_id.str();

   block_doc << "block_num" << b_int32{static_cast<int32_t>(block.block_num())}
       << "block_id" << block_id_str
       << "prev_block_id" << block.previous.str()
       << "timestamp" << b_date{std::chrono::milliseconds{std::chrono::seconds{block.timestamp.sec_since_epoch()}}}
       << "transaction_merkle_root" << block.transaction_merkle_root.str()
       << "producer_account_id" << block.producer.toString();
   if (!blocks.insert_one(block_doc.view())) {
      elog("Failed to insert block ${bid}", ("bid", block_id));
   }

   int32_t trx_num = -1;
   const bool check_relevance = !filter_on.empty();
   for (const auto& cycle : block.cycles) {
      for (const auto& thread : cycle) {
         for (const auto& trx : thread.user_input) {
            ++trx_num;
            if (check_relevance && !is_scope_relevant(trx.scope))
               continue;

            stream::document doc{};
            const auto trans_id_str = trx.id().str();
            auto trx_doc = doc
                  << "transaction_id" << trans_id_str
                  << "sequence_num" << b_int32{trx_num}
                  << "block_id" << block_id_str
                  << "ref_block_num" << b_int32{static_cast<int32_t >(trx.refBlockNum)}
                  << "ref_block_prefix" << trx.refBlockPrefix.str()
                  << "scope" << stream::open_array;
            for (const auto& account_name : trx.scope)
               trx_doc = trx_doc << account_name.toString();
            trx_doc = trx_doc
                  << stream::close_array
                  << "read_scope" << stream::open_array;
            for (const auto& account_name : trx.readscope)
               trx_doc = trx_doc << account_name.toString();
            trx_doc = trx_doc
                  << stream::close_array
                  << "expiration" << b_date{std::chrono::milliseconds{std::chrono::seconds{trx.expiration.sec_since_epoch()}}}
                  << "signatures" << stream::open_array;
            for (const auto& sig : trx.signatures) {
               trx_doc = trx_doc << fc::json::to_string(sig);
            }
            trx_doc = trx_doc
                  << stream::close_array
                  << "messages" << stream::open_array;

            mongocxx::bulk_write bulk_msgs{bulk_opts};
            int32_t i = 0;
            for (const chain::Message& msg : trx.messages) {
               auto msg_oid = bsoncxx::oid{};
               trx_doc = trx_doc << msg_oid; // add to transaction.messages array
               stream::document msg_builder{};
               auto msg_doc = msg_builder
                  << "_id" << b_oid{msg_oid}
                  << "message_id" << b_int32{i}
                  << "transaction_id" << trans_id_str
                  << "authorization" << stream::open_array;
               for (const auto& auth : msg.authorization) {
                  msg_doc = msg_doc << stream::open_document
                        << "account" << auth.account.toString()
                        << "permission" << auth.permission.toString()
                        << stream::close_document;
               }
               std::string data_str;
               if (msg.type == newaccount) {
                  data_str = fc::json::to_string(msg.as<eos::types::newaccount>());
               } else {
                  data_str = fc::json::to_string(msg.data);
               }
               auto msg_complete = msg_doc
                     << stream::close_array
                     << "type" << msg.type.toString()
                     // TODO: unpack from binary
                     << "data" << data_str
                     << stream::finalize;
               mongocxx::model::insert_one insert_msg{msg_complete.view()};
               bulk_msgs.append(insert_msg);

               // eos account update
               if (msg.code == config::EosContractName) {
                  update_account(msg);
               }

               ++i;
            }

            if (!trx.messages.empty()) {
               auto result = msgs.bulk_write(bulk_msgs);
               if (!result) {
                  elog("Bulk message insert failed for block: ${bid}, transaction: ${trx}", ("bid", block_id)("trx", trx.id()));
               }
            }

            auto complete_doc = trx_doc << stream::close_array << stream::finalize;
            mongocxx::model::insert_one insert_op{complete_doc.view()};
            transactions_in_block = true;
            bulk_trans.append(insert_op);

         }
      }
   }


   if (transactions_in_block) {
      auto result = trans.bulk_write(bulk_trans);
      if (!result) {
         elog("Bulk transaction insert failed for block: ${bid}", ("bid", block_id));
      }
   }

}

void db_plugin_impl::update_account(const chain::Message& msg) {
   using bsoncxx::builder::stream::document;
   using bsoncxx::builder::stream::open_document;
   using bsoncxx::builder::stream::close_document;
   using bsoncxx::builder::stream::finalize;

   if (msg.code != config::EosContractName)
      return;

   if (msg.type == transfer) {
      auto transfer = msg.as<types::transfer>();
      auto from_name = transfer.from.toString();
      auto to_name = transfer.to.toString();
      auto accounts = mongo_conn[db_name][accounts_col]; // Accounts
      document find_from{};
      find_from << "name" << from_name;
      auto from_account = accounts.find_one(find_from.view());
      if (!from_account) {
         elog("Unable to find account ${n}", ("n", transfer.from));
         return;
      }
      document find_to{};
      find_to << "name" << to_name;
      auto to_account = accounts.find_one(find_to.view());
      if (!to_account) {
         elog("Unable to find account ${n}", ("n", transfer.to));
         return;
      }
      auto from_view = from_account->view();
      Asset from_balance = Asset::fromString(from_view["eos_balance"].get_utf8().value.to_string());
      auto to_view = to_account->view();
      Asset to_balance = Asset::fromString(to_view["eos_balance"].get_utf8().value.to_string());
      from_balance -= eos::types::ShareType(transfer.amount);
      to_balance += eos::types::ShareType(transfer.amount);

      document update_from{};
      update_from << "$set" << open_document << "eos_balance" << from_balance.toString() << close_document;
      document update_to{};
      update_to << "$set" << open_document << "eos_balance" << to_balance.toString() << close_document;

      accounts.update_one(find_from.view(), update_from.view());
      accounts.update_one(find_to.view(), update_to.view());

   } else if (msg.type == newaccount) {
      auto newaccount = msg.as<types::newaccount>();
      auto accounts = mongo_conn[db_name][accounts_col]; // Accounts
      bsoncxx::builder::stream::document doc{};
      doc << "name" << newaccount.name.toString()
          << "eos_balance" << newaccount.deposit.toString();
      if (!accounts.insert_one(doc.view())) {
         elog("Failed to insert account ${n}", ("n", newaccount.name));
      }
   }
}

bool db_plugin_impl::is_scope_relevant(const eos::types::Vector<AccountName>& scope)
{
   for (const AccountName& account_name : scope)
      if (filter_on.count(account_name))
         return true;

   return false;
}

db_plugin_impl::db_plugin_impl()
: mongo_inst{}
, mongo_conn{}
{
}

db_plugin_impl::~db_plugin_impl() {
   try {
      done = true;
      consum_thread.join();
   } catch (std::exception& e) {
      elog("Exception on db_plugin shutdown of consum thread: ${e}", ("e", e.what()));
   }
}

void db_plugin_impl::wipe_database() {
   ilog("db wipe_database");
   mongo_conn[db_name].drop();
}

void db_plugin_impl::init() {
   // Create the native contract accounts manually; sadly, we can't run their contracts to make them create themselves
   auto accounts = mongo_conn[db_name][accounts_col]; // Accounts
   bsoncxx::builder::stream::document doc{};
   if (accounts.count(doc.view()) == 0) {
      doc << "name" << config::EosContractName.toString()
          << "eos_balance" << Asset(config::InitialTokenSupply).toString();
      if (!accounts.insert_one(doc.view())) {
         elog("Failed to insert account ${n}", ("n", config::EosContractName.toString()));
      }
   }
}

//////////
// db_plugin
//////////

db_plugin::db_plugin()
:my(new db_plugin_impl)
{
}

db_plugin::~db_plugin()
{
}

void db_plugin::set_program_options(options_description& cli, options_description& cfg)
{
   cfg.add_options()
         ("filter_on_accounts,f", bpo::value<std::vector<std::string>>()->composing(),
          "Track only transactions whose scopes involve the listed accounts. Default is to track all transactions.")
         ("queue_size,q", bpo::value<uint>()->default_value(1024),
         "The block queue size")
         ;
}

void db_plugin::wipe_database() {
   if (!my->startup) {
      elog("ERROR: db_plugin::wipe_database() called before configuration or after startup. Ignoring.");
   } else {
      my->wipe_database_on_startup = true;
   }
}

void db_plugin::applied_irreversible_block(const signed_block& block) {
   my->applied_irreversible_block(block);
}


void db_plugin::plugin_initialize(const variables_map& options)
{
   ilog("initializing db plugin");
   if(options.count("filter_on_accounts")) {
      auto foa = options.at("filter_on_accounts").as<std::vector<std::string>>();
      for(auto filter_account : foa)
         my->filter_on.emplace(filter_account);
   } else if (options.count("queue_size")) {
      auto size = options.at("queue_size").as<uint>();
      my->queue = std::make_unique<boost::lockfree::spsc_queue<signed_block>>(size);
   }

   // TODO: add command line for uri
   my->mongo_conn = mongocxx::client{mongocxx::uri{}};

   if (my->wipe_database_on_startup) {
      my->wipe_database();
   }
   my->init();
}

void db_plugin::plugin_startup()
{
   ilog("starting db plugin");
   // TODO: assert that last irreversible in db is one less than received (on startup only?, every so often?)

   my->consum_thread = boost::thread([this]{ my->consum_blocks(); });

   // chain_controller is created and has resynced or replayed if needed
   my->startup = false;
}

void db_plugin::plugin_shutdown()
{
   my.reset();
}

} // namespace eos