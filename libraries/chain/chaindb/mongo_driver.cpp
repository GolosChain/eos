#include <cyberway/chaindb/mongo_bigint_converter.hpp>
#include <cyberway/chaindb/mongo_driver.hpp>
#include <cyberway/chaindb/exception.hpp>
#include <cyberway/chaindb/names.hpp>
#include <cyberway/chaindb/mongo_driver_utils.hpp>
#include <cyberway/chaindb/journal.hpp>
#include <cyberway/chaindb/abi_info.hpp>
#include <cyberway/chaindb/noscope_tables.hpp>

#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/exception/exception.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/exception/logic_error.hpp>

namespace cyberway { namespace chaindb {

    using fc::variant_object;

    using bsoncxx::builder::basic::make_document;
    using bsoncxx::builder::basic::document;
    using bsoncxx::builder::basic::sub_document;
    using bsoncxx::builder::basic::kvp;

    using mongocxx::database;
    using mongocxx::collection;

    namespace model   = mongocxx::model;
    namespace options = mongocxx::options;

    using document_view = bsoncxx::document::view;

    enum class direction: int {
        Forward  =  1,
        Backward = -1
    }; // enum direction

    enum class mongo_code: int {
        Unknown        = -1,
        EmptyBulk      = 22,
        DuplicateValue = 11000,
        NoServer       = 13053
    }; // enum mongo_code

    namespace { namespace _detail {
        static const string pk_index_postfix = "_pk";
        static const string mongodb_id_index = "_id_";
        static const string mongodb_system   = "system.";

        template <typename Exception>
        mongo_code get_mongo_code(const Exception& e) {
            auto value = static_cast<mongo_code>(e.code().value());

            switch (value) {
                case mongo_code::EmptyBulk:
                case mongo_code::DuplicateValue:
                case mongo_code::NoServer:
                    return value;

                default:
                    return mongo_code::Unknown;
            }
        }

        bool is_asc_order(const string& name) {
            return name.size() == names::asc_order.size();
        }

        int get_field_order(const int order, const string& name) {
            if (is_asc_order(name)) {
                return  order;
            } else {
                return -order;
            }
        }

        field_name get_order_field(const order_def& order) {
            field_name field = order.field;
            if (order.type == "uint128" || order.type == "int128") {
                field.append(1, '.').append(mongo_bigint_converter::BINARY_FIELD);
            }
            return field;
        }

        variant get_order_value(const variant_object& row, const index_info& index, const order_def& order) try {
            auto* object = &row;
            auto pos = order.path.size();
            for (auto& key: order.path) {
                auto itr = object->find(key);
                CYBERWAY_ASSERT(object->end() != itr, driver_absent_field_exception,
                    "Can't find the part ${key} for the field ${field} in the row ${row} from the table ${table}",
                    ("key", key)("field", order.field)("table", get_full_table_name(index))("row", row));

                --pos;
                if (0 == pos) {
                    return itr->value();
                } else {
                    object = &itr->value().get_object();
                }
            }
            CYBERWAY_THROW(driver_absent_field_exception,
                "Wrong logic on parsing of the field ${field} in the row ${row} from the table ${table}",
                ("table", get_full_table_name(index))("field", order.field)("row", row));
        } catch (const driver_absent_field_exception&) {
            throw;
        } catch (...) {
            CYBERWAY_THROW(driver_absent_field_exception,
                "External database can't read the field ${field} in the row ${row} from the table ${table}",
                ("field", order.field)("table", get_full_table_name(index))("row", row));
        }

        template <typename Lambda>
        void auto_reconnect(Lambda&& lambda) {
            // TODO: move to program options
            constexpr int max_iters = 12;
            constexpr int sleep_seconds = 5;

            for (int i = 0; i < max_iters; ++i) try {
                if (i > 0) {
                    wlog("Fail to connect to MongoDB server, wait ${sec} seconds...", ("sec", sleep_seconds));
                    sleep(sleep_seconds);
                    wlog("Try again...");
                }
                lambda();
                return;
            } catch (const mongocxx::exception& e) {
                elog("MongoDB error on reconnect: ${code}, ${what}", ("code", e.code().value())("what", e.what()));

                CYBERWAY_ASSERT(_detail::get_mongo_code(e) == mongo_code::NoServer,
                    driver_open_exception, "MongoDB driver error: ${code}, ${what}",
                    ("code", e.code().value())("what", e.what()));

                continue; // try again
            }

            CYBERWAY_THROW(driver_open_exception, "Fail to connect to MongoDB server");
        }

        collection get_db_table(const mongodb_driver_impl&, const table_info&);

    } } // namespace _detail

    class mongodb_cursor_info: public cursor_info {
    public:
        mongodb_cursor_info(cursor_t id, index_info index, mongodb_driver_impl& driver)
        : cursor_info{id, std::move(index)},
          driver_(driver) {
        }

        mongodb_cursor_info() = default;
        mongodb_cursor_info(mongodb_cursor_info&&) = default;

        mongodb_cursor_info(const mongodb_cursor_info&) = delete;

        mongodb_cursor_info clone(cursor_t id) {
            mongodb_cursor_info dst(id, index, driver_);

            if (source_) {
                // it is faster to get object from exist cursor then to open a new cursor, locate, and get object
                dst.object     = get_object_value();
                dst.find_key_  = dst.object.value;
                dst.find_pk_   = get_pk_value();
                // don't copy direction, because direction::Backward starts from previous, not from currently
                dst.direction_ = direction::Forward;
            } else {
                dst.find_key_  = find_key_;
                dst.find_pk_   = find_pk_;
                dst.object     = object;
                dst.direction_ = direction_;
            }

            dst.pk     = pk;
            dst.scope_ = index.scope;

            return dst;
        }

        // open() allows to reuse the same cursor for different cases
        mongodb_cursor_info& open(const direction dir, variant key, const primary_key_t locate_pk) {
            reset_object();
            source_.reset();

            pk         = locate_pk;
            scope_     = index.scope;
            direction_ = dir;

            find_pk_   = locate_pk;
            find_key_  = std::move(key);

            return *this;
        }

        mongodb_cursor_info& next() {
            if (direction::Backward == direction_) {
                // we at the last record of a range - we should get its key for correct locating
                lazy_open();
                auto was_end = is_end();
                change_direction(direction::Forward);
                if (was_end) {
                    lazy_open();
                    return *this;
                }
            }
            lazy_next();
            return *this;
        }

        mongodb_cursor_info& prev() {
            if (direction::Forward == direction_) {
                change_direction(direction::Backward);
                lazy_open();
            } else if (primary_key::End == pk) {
                lazy_open();
            } else {
                lazy_next();
            }
            return *this;
        }

        mongodb_cursor_info& current() {
            if (primary_key::Unset == pk) {
                lazy_open();
            }
            return *this;
        }

        const object_value& get_object_value(bool with_decors = false) {
            lazy_open();
            if (!object.value.is_null()) {
                return object;
            }

            if (is_end()) {
                object.clear();
                object.service.pk    = pk;
                object.service.code  = index.code;
                object.service.scope = index.scope;
                object.service.table = index.table_name();
            } else {
                auto& view = *source_.value().begin();
                object = build_object(index, view, with_decors);
                pk     = object.service.pk;
            }

            return object;
        }

        bool is_opened() const {
            return !!source_;
        }

        void skip_pk(const primary_key_t pk) {
            if (is_opened()) {
                if (!skipped_pk_tree_.capacity()) {
                    skipped_pk_tree_.reserve(64);
                }
                skipped_pk_tree_.insert(pk);
            }
        }

    private:
        mongodb_driver_impl& driver_;

        direction     direction_ = direction::Forward;
        primary_key_t find_pk_   = primary_key::Unset;
        variant       find_key_;

        std::optional<mongocxx::cursor> source_;
        account_name_t scope_ = 0;
        fc::flat_set<primary_key_t> skipped_pk_tree_;

        void change_direction(const direction dir) {
            if (!source_) {
                get_object_value();
            }
            if (source_) {
                find_key_ = get_object_value().value;
                find_pk_  = get_pk_value();
            }
            source_.reset();
            direction_ = dir;
        }

        void reset_object() {
            pk = primary_key::Unset;
            if (!object.is_null()) {
                object.clear();
            }
        }

        document create_bound_document() {
            document bound;
            const variant_object* find_object = nullptr;
            auto order = static_cast<int>(direction_);

            if (find_key_.is_object() && find_key_.get_object().size()) {
                find_object = &find_key_.get_object();
            }

            if (!is_noscope_table(index)) {
                append_scope_value(bound, index);
            }

            auto& orders = index.index->orders;
            for (auto& o: orders) {
                auto field = _detail::get_order_field(o);
                if (find_object) {
                    build_document(bound, field, _detail::get_order_value(*find_object, index, o));
                } else {
                    build_bound_document(bound, field, _detail::get_field_order(order, o.order));
                }
            }

            if (!index.index->unique) {
                if (primary_key::is_good(find_pk_)) {
                    append_pk_value(bound, index, find_pk_);
                } else {
                    build_bound_document(bound, index.pk_order->field, order);
                }
            }

            return bound;
        }

        document create_sort_document() const {
            document sort;
            auto order = static_cast<int>(direction_);

            if (!is_noscope_table(index)) {
                sort.append(kvp(names::scope_path, order));
            }

            auto& orders = index.index->orders;
            for (auto& o: orders) {
                sort.append(kvp(_detail::get_order_field(o), _detail::get_field_order(order, o.order)));
            }

            if (!index.index->unique) {
                sort.append(kvp(index.pk_order->field, order));
            }

            return sort;
        }

        void lazy_open() {
            if (source_) return;

            auto bound = create_bound_document();
            auto sort  = create_sort_document();

            find_pk_ = primary_key::Unset;

            auto opts = options::find()
                .hint(mongocxx::hint(db_name_to_string(index.index->name)))
                .sort(sort.view());

            if (direction_ == direction::Forward) {
                opts.min(bound.view());
            } else {
                opts.max(bound.view());
            }


            _detail::auto_reconnect([&]() {
                skipped_pk_tree_.clear();
                source_.emplace(_detail::get_db_table(driver_, index).find({}, opts));
                try_to_init_pk_value();
            });
        }

        bool is_end() {
            auto& src = source_.value();
            if (src.begin() == src.end()) {
                return true;
            } else if (!is_noscope_table(index)) {
                return !ignore_scope(index) && scope_ != index.scope ;
            }
            return false;
        }

        void lazy_next() {
            lazy_open();

            while (!is_end()) {
                try {
                    ++source_.value().begin();
                } catch (const mongocxx::exception& e) {
                    elog("MongoDB error on iterate to next object: ${code}, ${what}",
                        ("code", e.code().value())("what", e.what()));
                    CYBERWAY_THROW(driver_open_exception, "MongoDB error on iterate to next object: ${code}, ${what}",
                        ("code", e.code().value())("what", e.what()));
                }

                try_to_init_pk_value();
                if (!skipped_pk_tree_.count(pk)) {
                    break;
                }
            }
        }

        void try_to_init_pk_value() {
            init_scope_value();
            if (!is_end() || direction::Forward == direction_) {
                reset_object();
                init_pk_value();
            }
        }

        primary_key_t get_pk_value() {
            if (primary_key::Unset == pk) {
                init_pk_value();
            }
            return pk;
        }

        void init_scope_value() {
            auto itr = source_.value().begin();
            if (source_.value().end() != itr) {
                scope_ = chaindb::get_scope_value(index, *itr);
            }
        }

        void init_pk_value() {
            if (is_end()) {
                pk = primary_key::End;
            } else {
                pk = chaindb::get_pk_value(index, *source_.value().begin());
            }
        }

    }; // class mongodb_cursor_info

    using cursor_map = std::map<cursor_t, mongodb_cursor_info>;
    using code_cursor_map = std::map<account_name /* code */, cursor_map>;

    struct cursor_location {
        code_cursor_map::iterator code_itr_;
        cursor_map::iterator cursor_itr_;

        mongodb_cursor_info& cursor() {
            return cursor_itr_->second;
        }

        cursor_map& map() {
            return code_itr_->second;
        }
    }; // struct cursor_location

    ///----

    struct mongodb_driver_impl {
        journal& journal_;
        string sys_code_name_;
        mongocxx::client mongo_conn_;
        code_cursor_map code_cursor_map_;
        bool skip_op_cnt_checking_ = false;

        // https://github.com/cyberway/cyberway/issues/1094
        bool update_pk_with_revision_ = false;

        mongodb_driver_impl(journal& jrnl, string address, string sys_name)
        : journal_(jrnl),
          sys_code_name_(std::move(sys_name)) {
            init_instance();
            mongocxx::uri uri{address};
            mongo_conn_ = mongocxx::client{uri};
        }

        ~mongodb_driver_impl() = default;

        mongodb_cursor_info& get_unapplied_cursor(const cursor_request& request) {
            return get_cursor(request).cursor();
        }

        mongodb_cursor_info& get_applied_cursor(const cursor_info& info) {
            return get_applied_cursor(const_cast<cursor_info&>(info));
        }

        mongodb_cursor_info& get_applied_cursor(const cursor_request& request) {
            auto loc = get_cursor(request);
            return get_applied_cursor(loc.cursor());
        }

        void apply_code_changes(const account_name& code) {
            journal_.apply_code_changes(write_ctx_t_(*this), code);
        }

        void apply_all_changes() {
            journal_.apply_all_changes(write_ctx_t_(*this));
        }

        void skip_pk(const table_info& table, const primary_key_t pk) {
            auto itr = code_cursor_map_.find(table.code);
            if (code_cursor_map_.end() == itr) {
                return;
            }

            for (auto& id_cursor: itr->second) if (id_cursor.second.index.scope == table.scope) {
                id_cursor.second.skip_pk(pk);
            }
        }

        void close_cursor(const cursor_request& request) {
            auto loc = get_cursor(request);
            auto& map = loc.map();

            map.erase(loc.cursor_itr_);
            if (map.empty()) {
                code_cursor_map_.erase(loc.code_itr_);
            }
        }

        void close_code_cursors(const account_name& code) {
            auto itr = code_cursor_map_.find(code);
            if (code_cursor_map_.end() == itr) return;

            code_cursor_map_.erase(itr);
        }

        std::vector<index_def> get_db_indexes(collection& db_table) const {
            std::vector<index_def> result;
            auto indexes = db_table.list_indexes();

            result.reserve(abi_info::MaxIndexCnt * 2);
            for (auto& info: indexes) {
                index_def index;

                auto itr = info.find("name");
                if (info.end() == itr) continue;
                auto iname = itr->get_utf8().value;

                if (iname.ends_with(_detail::pk_index_postfix)) continue;
                if (!iname.compare( _detail::mongodb_id_index)) continue;

                try {
                    index.name = db_string_to_name(iname.data());
                } catch (const eosio::chain::name_type_exception&) {
                    db_table.indexes().drop_one(iname);
                    continue;
                }

                itr = info.find("unique");
                if (info.end() != itr) index.unique = itr->get_bool().value;

                itr = info.find("key");
                if (info.end() != itr) {
                    auto fields = itr->get_document().value;
                    for (auto& field: fields) {
                        order_def order;

                        auto key = field.key();
                        if (!key.compare(names::scope_path)) continue;

                        order.field = key.to_string();

                        // <FIELD_NAME>.binary - we should remove ".binary"
                        if (order.field.size() > mongo_bigint_converter::BINARY_FIELD.size() &&
                            key.ends_with(mongo_bigint_converter::BINARY_FIELD)
                        ) {
                            order.field.erase(order.field.size() - 1 - mongo_bigint_converter::BINARY_FIELD.size());
                        }

                        order.order = field.get_int32().value == 1 ? names::asc_order : names::desc_order;
                        index.orders.emplace_back(std::move(order));
                    }

                    // see create_index()
                    if (!index.unique) index.orders.pop_back();
                }
                result.emplace_back(std::move(index));
            }
            return result;
        }

        std::vector<table_def> db_tables(const account_name& code) const {
            static constexpr std::chrono::milliseconds max_time(10);
            std::vector<table_def> tables;

            tables.reserve(abi_info::MaxTableCnt * 2);
            _detail::auto_reconnect([&]() {
                tables.clear();
                auto db = mongo_conn_.database(get_code_name(sys_code_name_, code));
                auto names = db.list_collection_names();
                for (auto& tname: names) {
                    table_def table;

                    if (!tname.compare(0, _detail::mongodb_system.size(), _detail::mongodb_system)) {
                        continue;
                    }

                    try {
                        table.name = db_string_to_name(tname.c_str());
                    } catch (const eosio::chain::name_type_exception& e) {
                        db.collection(tname).drop();
                        continue;
                    }
                    auto db_table = db.collection(tname);
                    table.row_count = db_table.estimated_document_count(
                        options::estimated_document_count().max_time(max_time));
                    table.indexes = get_db_indexes(db_table);

                    tables.emplace_back(std::move(table));
                }
            });

            return tables;
        }

        void drop_index(const index_info& info) const {
            get_db_table(info).indexes().drop_one(get_index_name(info));
        }

        void drop_table(const table_info& info) const {
            get_db_table(info).drop();
        }

        void create_index(const index_info& info) const {
            document idx_doc;
            auto& index = *info.index;

            if (!is_noscope_table(info)) {
                idx_doc.append(kvp(names::scope_path, 1));
            }
            for (auto& order: index.orders) {
                auto field = _detail::get_order_field(order);
                if (_detail::is_asc_order(order.order)) {
                    idx_doc.append(kvp(field, 1));
                } else {
                    idx_doc.append(kvp(field, -1));
                }
            }

            if (!index.unique) {
                // when index is not unique, we add unique pk for deterministic order of records
                idx_doc.append(kvp(info.pk_order->field, 1));
            }

            auto idx_name = get_index_name(info);
            auto db_table = get_db_table(info);
            db_table.create_index(idx_doc.view(), options::index().name(idx_name).unique(index.unique));

            // for available primary key
            if (!is_noscope_table(info) && info.pk_order == &index.orders.front()) {
                document id_doc;
                idx_name.append(_detail::pk_index_postfix);
                id_doc.append(kvp(info.pk_order->field, 1));
                db_table.create_index(id_doc.view(), options::index().name(idx_name));
            }
        }

        template <typename... Args>
        mongodb_cursor_info& create_cursor(index_info index, Args&&... args) {
            auto code = index.code;
            auto itr = code_cursor_map_.find(code);
            auto id = get_next_cursor_id(itr);
            auto db_table = get_db_table(index);
            mongodb_cursor_info new_cursor(id, std::move(index), *this, std::forward<Args>(args)...);
            // apply_table_changes(index);
            return add_cursor(std::move(itr), code, std::move(new_cursor));
        }

        template <typename... Args>
        mongodb_cursor_info& create_applied_cursor(index_info index, Args&&... args) {
            apply_table_changes(index);
            return create_cursor(std::move(index), std::forward<Args>(args)...);
        }

        mongodb_cursor_info& clone_cursor(const cursor_request& request) {
            auto loc = get_cursor(request);
            auto next_id = get_next_cursor_id(loc.code_itr_);

            auto cloned_cursor = loc.cursor().clone(next_id);
            return add_cursor(loc.code_itr_, request.code, std::move(cloned_cursor));
        }

        void drop_db() {
            CYBERWAY_ASSERT(code_cursor_map_.empty(), driver_opened_cursors_exception, "ChainDB has opened cursors");

            code_cursor_map_.clear(); // close all opened cursors

            auto db_list = mongo_conn_.list_databases();
            for (auto& db: db_list) {
                auto db_name = db["name"].get_utf8().value;
                if (!db_name.starts_with(sys_code_name_)) continue;

                mongo_conn_.database(db_name).drop();
            }
        }

        primary_key_t available_pk(const table_info& table) {
            primary_key_t pk = 0;
            auto& pk_index = table.table->indexes.front();
            auto  pk_order = table.pk_order;
            auto  hint = db_name_to_string(pk_index.name);
            document sort;
            document bound;

            apply_table_changes(table);

            build_bound_document(bound, pk_order->field, -1);

            sort.append(kvp(table.pk_order->field, -1));

            if (!is_noscope_table(table)) {
                hint.append(_detail::pk_index_postfix);
            }

            auto opts = options::find()
                .hint(mongocxx::hint(hint))
                .sort(sort.view())
                .max(bound.view())
                .limit(1);

            _detail::auto_reconnect([&] {
                auto doc = get_db_table(table).find_one(make_document(), opts);

                if (!!doc) {
                    pk = chaindb::get_pk_value(table, *doc) + 1;
                }
            });

            return pk;
        }

        object_value object_by_pk(const table_info& table, const primary_key_t pk) {
            object_value obj;
            auto& pk_index = table.table->indexes.front();
            auto  pk_order = table.pk_order;
            document bound;
            document sort;

            apply_table_changes(table);

            if (!is_noscope_table(table)) {
                append_scope_value(bound, table);
                sort.append(kvp(names::scope_path, 1));
            }

            append_pk_value(bound, table, pk);
            sort.append(kvp(table.pk_order->field, 1));

            auto opts = options::find()
                .hint(mongocxx::hint(db_name_to_string(pk_index.name)))
                .sort(sort.view())
                .min(bound.view())
                .limit(1);

            obj.service.pk = pk;

            _detail::auto_reconnect([&] {
                auto doc = get_db_table(table).find_one(make_document(), opts);

                if (!!doc) {
                    auto dpk = chaindb::get_pk_value(table, *doc);
                    auto scope = chaindb::get_scope_value(table, *doc);
                    if (dpk == pk && scope == table.scope) {
                        obj = build_object(table, doc->view(), false);
                        return;
                    }
                }

                obj.clear();
                obj.service.pk    = primary_key::End;
                obj.service.code  = table.code;
                obj.service.scope = table.scope;
                obj.service.table = table.table_name();
            });

            return obj;
        }

        collection get_db_table(const table_info& table) const {
            return get_db_table(table.code, table.table_name());
        }

    private:
        static mongocxx::instance& init_instance() {
            static mongocxx::instance instance;
            return instance;
        }

        collection get_db_table(const account_name_t code, const table_name_t table) const {
            return mongo_conn_.database(get_code_name(sys_code_name_, code)).collection(get_table_name(table));
        }

        cursor_t get_next_cursor_id(code_cursor_map::iterator itr) {
            if (itr != code_cursor_map_.end() && !itr->second.empty()) {
                return itr->second.rbegin()->second.id + 1;
            }
            return 1;
        }

        mongodb_cursor_info& add_cursor(
            code_cursor_map::iterator itr, const account_name& code, mongodb_cursor_info cursor
        ) {
            if (code_cursor_map_.end() == itr) {
                itr = code_cursor_map_.emplace(code, cursor_map()).first;
            }
            return itr->second.emplace(cursor.id, std::move(cursor)).first->second;
        }

        void apply_table_changes(const table_info& table) {
            journal_.apply_table_changes(write_ctx_t_(*this), table);
        }

        mongodb_cursor_info& get_applied_cursor(cursor_info& info) {
            auto& cursor = static_cast<mongodb_cursor_info&>(info);
            if (!cursor.is_opened()) {
                apply_table_changes(cursor.index);
            }
            return cursor;
        }

        cursor_location get_cursor(const cursor_request& request) {
            auto code_itr = code_cursor_map_.find(request.code);
            CYBERWAY_ASSERT(code_cursor_map_.end() != code_itr, driver_invalid_cursor_exception,
                "The map for the cursor ${code}.${id} doesn't exist", ("code", get_code_name(request))("id", request.id));

            auto& map = code_itr->second;
            auto  cursor_itr = map.find(request.id);
            CYBERWAY_ASSERT(map.end() != cursor_itr, driver_invalid_cursor_exception,
                "The cursor ${code}.${id} doesn't exist", ("code", get_code_name(request))("id", request.id));

            return cursor_location{code_itr, cursor_itr};
        }

        class write_ctx_t_ final {
            struct bulk_info_t_ final {
                document pk;
                document data;
            }; // struct bulk_info_t_

            struct bulk_group_t_ final {
                const account_name_t code  = account_name_t();
                const table_name_t   table = table_name_t();

                std::deque<bulk_info_t_> remove;
                std::deque<bulk_info_t_> update;
                std::deque<bulk_info_t_> revision;
                std::deque<bulk_info_t_> insert;

                bulk_group_t_() = default;

                bulk_group_t_(const table_info& info)
                : code(info.code),
                  table(info.table_name()) {
                }

                bulk_group_t_(const table_name_t& name)
                : table(name) {
                }
            }; // struct bulk_group_t_;

        public:
            write_ctx_t_(mongodb_driver_impl& impl)
            : impl_(impl),
              complete_undo_bulk_(N(undo)),
              prepare_undo_bulk_(N(undo)) {
            }

            void start_table(const table_info& table) {
                auto old_table = table_;
                table_ = &table;

                if (old_table == nullptr ||
                    table.code != old_table->code ||
                    table.table_name() != old_table->table_name()
                ) {
                    bulk_list_.emplace_back(table);
                }
            }

            void add_data(const write_operation& op) {
                append_bulk(build_find_pk_document, build_service_document, bulk_list_.back(), op);
            }

            void add_prepare_undo(const write_operation& op) {
                append_bulk(build_find_undo_pk_document, build_undo_document, prepare_undo_bulk_, op);
            }

            void add_complete_undo(const write_operation& op) {
                append_bulk(build_find_undo_pk_document, build_undo_document, complete_undo_bulk_, op);
            }

            void write() {
                execute_bulk(prepare_undo_bulk_);

                for (auto& group: bulk_list_) {
                    execute_bulk(group);
                }

                execute_bulk(complete_undo_bulk_);

                CYBERWAY_ASSERT(error_.empty(), driver_duplicate_exception, error_);
            }

        private:
            mongodb_driver_impl& impl_;
            std::deque<bulk_group_t_> bulk_list_;
            bulk_group_t_ complete_undo_bulk_;
            bulk_group_t_ prepare_undo_bulk_;

            std::string error_;
            const table_info* table_ = nullptr;

            template <typename BuildFindDocument, typename BuildServiceDocument>
            void append_bulk(
                BuildFindDocument&& build_find_document, BuildServiceDocument&& build_service_document,
                bulk_group_t_& group, const write_operation& op
            ) {
                bulk_info_t_& dst = [&]() -> bulk_info_t_& {
                    switch(op.operation) {
                        case write_operation::Unknown:
                        case write_operation::Insert:
                            return group.insert.emplace_back();

                        case write_operation::Update:
                            return group.update.emplace_back();

                        case write_operation::Revision:
                            return group.revision.emplace_back();

                        case write_operation::Remove:
                            return group.remove.emplace_back();
                    }
                }();

                switch (op.operation) {
                    case write_operation::Insert:
                    case write_operation::Update:
                        build_document(dst.data, op.object);

                    case write_operation::Revision:
                        build_service_document(dst.data, *table_, op.object);

                    case write_operation::Remove:
                        build_find_document(dst.pk, *table_, op.object);

                        // https://github.com/cyberway/cyberway/issues/1094
                        if (impl_.update_pk_with_revision_ && op.find_revision >= start_revision) {
                            dst.pk.append(kvp(names::revision_path, op.find_revision));
                        }
                        break;

                    case write_operation::Unknown:
                        CYBERWAY_THROW(driver_write_exception,
                            "Wrong operation type on writing into the table ${table}:${scope} "
                            "with the revision (find: ${find_rev}, set: ${set_rev}) and with the primary key ${pk}",
                            ("table", get_full_table_name(*table_))("scope", table_->scope)
                            ("find_rev", op.find_revision)("set_rev", op.object.service.revision)
                            ("pk", op.object.pk()));
                        return;
                }
            }

            void execute_bulk(bulk_group_t_& group) {
                static options::bulk_write opts(options::bulk_write().ordered(false));
                auto remove_bulk = impl_.get_db_table(group.code, group.table).create_bulk_write(opts);
                int  remove_cnt  = 0;
                auto update_bulk = impl_.get_db_table(group.code, group.table).create_bulk_write(opts);
                int  update_cnt  = 0;

                for (auto& src: group.remove) {
                    remove_bulk.append(model::delete_one(src.pk.view()));
                    ++remove_cnt;
                }

                for (auto& src: group.update) {
                    update_bulk.append(model::replace_one(src.pk.view(), src.data.view()));
                    ++update_cnt;
                }

                for (auto& src: group.revision) {
                    update_bulk.append(model::update_one(
                        src.pk.view(), make_document(kvp("$set", src.data))));
                    ++update_cnt;
                }

                for (auto& src: group.insert) {
                    update_bulk.append(model::insert_one(src.data.view()));
                    ++update_cnt;
                }

                execute_bulk(group, remove_cnt, remove_bulk);
                execute_bulk(group, update_cnt, update_bulk);
            }

            void execute_bulk(bulk_group_t_& group, const int op_cnt, mongocxx::bulk_write& bulk) {
                if (!op_cnt) {
                    return;
                }

                // no reasons to do reconnect, exception can happen after writing and it will fail whole writing-process
                try {
                    auto res = bulk.execute();
                    CYBERWAY_ASSERT(res, driver_open_exception,
                        "MongoDB driver returns empty result on bulk execution");

                    CYBERWAY_ASSERT(
                        impl_.skip_op_cnt_checking_ ||
                        (res->matched_count() + res->inserted_count()) == op_cnt ||
                        res->deleted_count()  == op_cnt,
                        driver_open_exception,
                        "MongoDB driver returns bad result on bulk execution to the table ${table}",
                        ("table", get_full_table_name(group.code, group.table))
                            ("op_cnt", op_cnt)("matched", res->matched_count())
                            ("inserted", res->inserted_count())("modified", res->modified_count())
                            ("deleted", res->deleted_count())("upserted", res->upserted_count()));

                } catch (const mongocxx::bulk_write_exception& e) {
                    elog("MongoDB error on bulk write: ${code}, ${what}", ("code", e.code().value())("what", e.what()));

                    CYBERWAY_ASSERT(_detail::get_mongo_code(e) == mongo_code::DuplicateValue,
                        driver_open_exception, "MongoDB driver error: ${code}, ${what}",
                        ("code", e.code().value())("what", e.what()));

                    error_ = e.what();
                }
            }
        }; // class write_ctx_t_

    }; // struct mongodb_driver_impl

    namespace { namespace _detail {
        collection get_db_table(const mongodb_driver_impl& driver, const table_info& info) {
            return driver.get_db_table(info);
        }
    } } // namespace _detail

    ///----

    mongodb_driver::mongodb_driver(journal& jrnl, string address, string sys_name)
    : impl_(std::make_unique<mongodb_driver_impl>(jrnl, std::move(address), std::move(sys_name))) {
    }

    mongodb_driver::~mongodb_driver() = default;

    void mongodb_driver::enable_rev_bad_update() const {
        // https://github.com/cyberway/cyberway/issues/1094
        impl_->update_pk_with_revision_ = true;
        enable_undo_restore();
    }

    void mongodb_driver::disable_rev_bad_update() const {
        // https://github.com/cyberway/cyberway/issues/1094
        impl_->update_pk_with_revision_ = false;
        disable_undo_restore();
    }

    void mongodb_driver::enable_undo_restore() const {
        impl_->skip_op_cnt_checking_ = true;
    }

    void mongodb_driver::disable_undo_restore() const {
        impl_->skip_op_cnt_checking_ = false;
    }

    std::vector<table_def> mongodb_driver::db_tables(const account_name& code) const {
        return impl_->db_tables(code);
    }

    void mongodb_driver::create_index(const index_info& index) const {
        impl_->create_index(index);
    }

    void mongodb_driver::drop_index(const index_info& index) const {
        impl_->drop_index(index);
    }

    void mongodb_driver::drop_table(const table_info& table) const {
        impl_->drop_table(table);
    }

    void mongodb_driver::drop_db() const {
        impl_->drop_db();
    }

    const cursor_info& mongodb_driver::clone(const cursor_request& request) const {
        return impl_->clone_cursor(request);
    }

    void mongodb_driver::close(const cursor_request& request) const {
        impl_->close_cursor(request);
    }

    void mongodb_driver::close_code_cursors(const account_name& code) const {
        impl_->close_code_cursors(code);
    }

    void mongodb_driver::apply_code_changes(const account_name& code) const {
        impl_->apply_code_changes(code);
    }

    void mongodb_driver::apply_all_changes() const {
        impl_->apply_all_changes();
    }

    void mongodb_driver::skip_pk(const table_info& table, const primary_key_t pk) const {
        impl_->skip_pk(table, pk);
    }

    cursor_info& mongodb_driver::lower_bound(index_info index, variant key) const {
        return impl_->create_cursor(std::move(index))
            .open(direction::Forward, std::move(key), primary_key::Unset);
    }

    cursor_info& mongodb_driver::upper_bound(index_info index, variant key) const {
        // upper_bound() in C++ returns next field after key
        //   when MongoDB returns first field before key

        // main problem: does exist the key in the collection or not? ...

        auto& cursor = impl_->create_applied_cursor(std::move(index))
            // 1. open at the max(), which exclude current value
            .open(direction::Backward, key, primary_key::Unset)
            // 2. return to the value which was excluded by max()
            .next();

        // now check - is it the key or not ...
        auto& obj = cursor.get_object_value();
        if (obj.value.is_object() && obj.value.has_value(key)) {
            cursor.next();
        }
        return cursor;
    }

    cursor_info& mongodb_driver::locate_to(index_info index, variant key, primary_key_t pk) const {
        return impl_->create_cursor(std::move(index))
            .open(direction::Forward, std::move(key), pk);
    }

    cursor_info& mongodb_driver::begin(index_info index) const {
        return impl_->create_cursor(std::move(index))
            .open(direction::Forward, {}, primary_key::Unset);
    }

    cursor_info& mongodb_driver::end(index_info index) const {
        return impl_->create_cursor(std::move(index))
            .open(direction::Backward, {}, primary_key::End);
    }

    cursor_info& mongodb_driver::cursor(const cursor_request& request) const {
        return impl_->get_unapplied_cursor(request);
    }

    cursor_info& mongodb_driver::current(const cursor_info& info) const {
        return impl_->get_applied_cursor(info)
            .current();
    }

    cursor_info& mongodb_driver::next(const cursor_info& info) const {
        return impl_->get_applied_cursor(info)
            .next();
    }

    cursor_info& mongodb_driver::prev(const cursor_info& info) const {
        return impl_->get_applied_cursor(info)
            .prev();
    }

    primary_key_t mongodb_driver::available_pk(const table_info& table) const {
        return impl_->available_pk(table);
    }

    object_value mongodb_driver::object_by_pk(const table_info& table, const primary_key_t pk) const {
        return impl_->object_by_pk(table, pk);
    }

    const object_value& mongodb_driver::object_at_cursor(const cursor_info& info, bool with_decors) const {
        return impl_->get_applied_cursor(info)
            .get_object_value(with_decors);
    }

} } // namespace cyberway::chaindb
