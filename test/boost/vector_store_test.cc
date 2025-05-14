/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include <cstdint>
#include <optional>
#include <random>
#include <seastar/core/shared_ptr.hh>
#include <seastar/http/httpd.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include "service/vector_store.hh"
#include "test/lib/cql_test_env.hh"


namespace {

using namespace seastar;

using http_server = httpd::http_server;
using routes = httpd::routes;
using vector_store = service::vector_store;

auto new_ephemeral_port() {
    constexpr auto MIN_PORT = 49152;
    constexpr auto MAX_PORT = 65535;

    auto rd = std::random_device{};
    return std::uniform_int_distribution<uint16_t>(MIN_PORT, MAX_PORT)(rd);
}

auto listen_on_ephemeral_port(lw_shared_ptr<http_server> server) -> future<socket_address> {
    constexpr auto const* LOCALHOST = "127.0.0.1";
    auto inaddr = net::inet_address(LOCALHOST);

    while (true) {
        auto addr = socket_address(inaddr, new_ephemeral_port());
        try {
            co_await server->listen(addr);
            co_return addr;
        } catch (const std::system_error& e) {
            continue;
        }
    }
}

auto new_http_server() -> future<std::tuple<lw_shared_ptr<http_server>, socket_address>> {
    auto server = make_lw_shared<http_server>("test_server");
    auto port = co_await listen_on_ephemeral_port(server);
    co_return std::make_tuple(server, port);
}

} // namespace

BOOST_AUTO_TEST_SUITE(vector_store_test)

SEASTAR_TEST_CASE(test_vector_store) {
    auto [server, addr] = co_await new_http_server();

    auto cfg = cql_test_config();
    *cfg.vector_store_addr = addr;
    co_await do_with_cql_env([](cql_test_env& env) -> future<> {
        auto& vs = env.local_qp().vector_store();

        auto& db = env.local_db();
        co_await env.execute_cql("create table ks.vs (id int, embedding vec<float, 3>, primary key (id))");
        auto schema = db.find_schema("ks", "vs");

        auto keys = co_await vs.ann("ks", "idx", schema, std::vector<float>{0.1, 0.2, 0.3}, 1);
        BOOST_CHECK(keys == std::nullopt);

        BOOST_CHECK(true);
    }, std::move(cfg));

    co_await server->stop();
}

BOOST_AUTO_TEST_SUITE_END()

