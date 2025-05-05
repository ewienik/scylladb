/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include "vector_store.hh"
#include <dht/i_partitioner.hh>
#include <keys.hh>
#include <schema/schema.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/client.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/short_streams.hh>
#include <utils/big_decimal.hh>
#include <utils/rjson.hh>
#include <charconv>
#include <cstdlib>

using namespace seastar;

logging::logger logger("vector_store");

namespace {

bytes get_key_column_value(const rjson::value& item, std::size_t idx, const column_definition& column) {
    auto const& column_name = column.name_as_text();
    // TODO: fix error checking
    ::logger.info("ppery: get_key_column_value: {}, {}, {}", column_name, idx, item);
    auto const* keys_obj = rjson::find(item, column_name);
    ::logger.info("ppery: get_key_column_value: {}", *keys_obj);
    auto const& keys_arr = keys_obj->GetArray();
    ::logger.info("ppery: get_key_column_value: {}", keys_arr);
    auto const& key = keys_arr[idx];
    ::logger.info("ppery: get_key_column_value: {}", key);
    return column.type->from_string(rjson::print(key));
}

partition_key pk_from_json(rjson::value const& item, std::size_t idx, schema_ptr const& schema) {
    std::vector<bytes> raw_pk;
    // FIXME: this is a loop, but we really allow only one partition key column.
    for (const column_definition& cdef : schema->partition_key_columns()) {
        ::logger.info("ppery: pk_from_json: {}", cdef.name());
        bytes raw_value = get_key_column_value(item, idx, cdef);
        ::logger.info("ppery: pk_from_json: {}", raw_value);
        raw_pk.push_back(std::move(raw_value));
    }
    return partition_key::from_exploded(raw_pk);
}

clustering_key_prefix ck_from_json(rjson::value const& item, std::size_t idx, schema_ptr const& schema) {
    if (schema->clustering_key_size() == 0) {
        return clustering_key_prefix::make_empty();
    }
    std::vector<bytes> raw_ck;
    // FIXME: this is a loop, but we really allow only one clustering key column.
    for (const column_definition& cdef : schema->clustering_key_columns()) {
        bytes raw_value = get_key_column_value(item, idx, cdef);
        raw_ck.push_back(std::move(raw_value));
    }

    return clustering_key_prefix::from_exploded(raw_ck);
}
} // namespace

namespace service {

vector_store::vector_store() {
    constexpr auto DEFAULT_IP = "127.0.0.1";
    constexpr uint16_t DEFAULT_PORT = 6080U;

    _host = DEFAULT_IP;
    auto port = DEFAULT_PORT;
    if (auto const* host_env = std::getenv("VECTOR_STORE_IP"); host_env != nullptr) {
        _host = host_env;
    }
    if (auto const* port_env = std::getenv("VECTOR_STORE_PORT"); port_env != nullptr) {
    }
    _client = std::make_unique<http::experimental::client>(socket_address(net::inet_address(_host), port));
}

vector_store::~vector_store() = default;

auto vector_store::ann(sstring const& keyspace, sstring const& name, schema_ptr schema, std::vector<float> const& embedding, std::size_t limit) const
        -> future<std::optional<std::vector<primary_key>>> {
    auto req = http::request::make("POST", _host, format("/api/v1/indexes/{}/{}/ann", keyspace, name));
    req.write_body("json", [limit, embedding](output_stream<char> out) -> future<> {
        std::exception_ptr ex;
        try {
            co_await out.write(R"({"embedding":[)");
            auto first = true;
            for (auto value: embedding) {
                if (!first) {
                    co_await out.write(",");
                } else {
                    first = false;
                }
                co_await out.write(format("{}", value));
            }
            co_await out.write(format(R"(],"limit":{}}})", limit));
            co_await out.flush();
        } catch (...) {
            ex = std::current_exception();
        }
        co_await out.close();
        if (ex) {
            co_await coroutine::return_exception_ptr(std::move(ex));
        }
    });
    auto resp_status = http::reply::status_type::ok;
    auto resp_content = std::vector<temporary_buffer<char>>{};
    // dht::decorated_key partition;
    // clustering_key_prefix clustering;
    co_await _client->make_request(std::move(req), [&](http::reply const& reply, input_stream<char> body) -> future<> {
        resp_status = reply._status;
        resp_content = co_await util::read_entire_stream(body);
    });
    ::logger.info("ppery: make_request: status {}, content {}", resp_status, resp_content);
    auto const resp = rjson::parse(std::move(resp_content));
    ::logger.info("ppery: resp: {}", resp);
    auto const& primary_keys = resp["primary_keys"];
    ::logger.info("ppery: primary_keys: {}", primary_keys);
    // TODO: fix error checking
    auto const distances = resp["distances"].GetArray();
    ::logger.info("ppery: distances: {}", distances);
    auto size = distances.Size();
    ::logger.info("ppery: size: {}", size);
    auto keys = std::vector<primary_key>{};
    for (auto idx = 0U; idx < size; ++idx) {
        ::logger.info("ppery: loop: {}", idx);
        auto pk = pk_from_json(primary_keys, idx, schema);
        auto ck = ck_from_json(primary_keys, idx, schema);
        keys.push_back(primary_key{dht::decorate_key(*schema, std::move(pk)), ck});
    }
    ::logger.info("ppery: keys.size: {}", keys.size());
    co_return std::optional{keys};
}

} // namespace service

