/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include "service/vector_store_client.hh"
#include "db/config.hh"
#include "exceptions/exceptions.hh"
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/short_streams.hh>


namespace {

using namespace seastar;

using vector_store_client = service::vector_store_client;
using vector_store_client_tester = service::vector_store_client_tester;
using config = vector_store_client::config;
using configuration_exception = exceptions::configuration_exception;
using inet_address = seastar::net::inet_address;
using port_number = vector_store_client::port_number;

} // namespace

BOOST_AUTO_TEST_CASE(vector_store_client_test_ctor) {
    {
        auto cfg = config();
        auto vs = vector_store_client{cfg};
        BOOST_CHECK(vs.is_disabled());
        BOOST_CHECK(!vs.host());
        BOOST_CHECK(!vs.port());
    }
    {
        auto cfg = config();
        cfg.vector_store_uri.set("http://good.authority.com:6080");
        auto vs = vector_store_client{cfg};
        BOOST_CHECK(!vs.is_disabled());
        BOOST_CHECK_EQUAL(*vs.host(), "good.authority.com");
        BOOST_CHECK_EQUAL(*vs.port(), 6080);
    }
    {
        auto cfg = config();
        cfg.vector_store_uri.set("http://bad,authority.com:6080");
        BOOST_CHECK_THROW(vector_store_client{cfg}, configuration_exception);
        cfg.vector_store_uri.set("bad-schema://authority.com:6080");
        BOOST_CHECK_THROW(vector_store_client{cfg}, configuration_exception);
        cfg.vector_store_uri.set("http://bad.port.com:a6080");
        BOOST_CHECK_THROW(vector_store_client{cfg}, configuration_exception);
        cfg.vector_store_uri.set("http://bad.port.com:60806080");
        BOOST_CHECK_THROW(vector_store_client{cfg}, configuration_exception);
        cfg.vector_store_uri.set("http://bad.format.com:60:80");
        BOOST_CHECK_THROW(vector_store_client{cfg}, configuration_exception);
        cfg.vector_store_uri.set("http://authority.com:6080/bad/path");
        BOOST_CHECK_THROW(vector_store_client{cfg}, configuration_exception);
    }
}

SEASTAR_TEST_CASE(vector_store_client_test_dns_refresh) {
    auto cfg = config();
    cfg.vector_store_uri.set("http://good.authority.here:6080");

    {
        // Resolving of the hostname is started in start_background_tasks()
        auto vs = vector_store_client{cfg};
        BOOST_CHECK(!vs.is_disabled());

        vector_store_client_tester::set_dns_refresh_interval(vs, std::chrono::milliseconds(2000));
        vector_store_client_tester::set_wait_for_client_timeout(vs, std::chrono::milliseconds(100));
        vector_store_client_tester::set_dns_resolver(vs, [](auto const& host) -> future<inet_address> {
            BOOST_CHECK_EQUAL(host, "good.authority.here");
            co_return inet_address("127.0.0.1");
        });

        co_await vs.start_background_tasks();

        BOOST_CHECK(co_await vector_store_client_tester::can_resolve_hostname(vs));

        co_await vs.stop();
    }

    {
        // Unable to resolve the hostname
        auto vs = vector_store_client{cfg};
        BOOST_CHECK(!vs.is_disabled());

        vector_store_client_tester::set_dns_refresh_interval(vs, std::chrono::milliseconds(2000));
        vector_store_client_tester::set_wait_for_client_timeout(vs, std::chrono::milliseconds(100));
        vector_store_client_tester::set_dns_resolver(vs, [](auto const& host) -> future<inet_address> {
            BOOST_CHECK_EQUAL(host, "good.authority.here");
            throw std::system_error();
            co_return inet_address("127.0.0.1");
        });

        co_await vs.start_background_tasks();

        BOOST_CHECK(!co_await vector_store_client_tester::can_resolve_hostname(vs));

        co_await vs.stop();
    }

    {
        // Resolving of the hostname is repeated after errors
        auto vs = vector_store_client{cfg};
        BOOST_CHECK(!vs.is_disabled());

        vector_store_client_tester::set_dns_refresh_interval(vs, std::chrono::milliseconds(100));
        vector_store_client_tester::set_wait_for_client_timeout(vs, std::chrono::milliseconds(500));
        auto count = 0;
        vector_store_client_tester::set_dns_resolver(vs, [&count](auto const& host) -> future<inet_address> {
            BOOST_CHECK_EQUAL(host, "good.authority.here");
            if (count++ < 2) {
                throw std::system_error();
            }
            co_return inet_address("127.0.0.1");
        });

        co_await vs.start_background_tasks();

        BOOST_CHECK(co_await vector_store_client_tester::can_resolve_hostname(vs));

        co_await vs.stop();
    }

    {
        // Minimal interval between DNS refreshes is respected
        auto vs = vector_store_client{cfg};
        BOOST_CHECK(!vs.is_disabled());

        vector_store_client_tester::set_dns_refresh_interval(vs, std::chrono::milliseconds(10));
        vector_store_client_tester::set_wait_for_client_timeout(vs, std::chrono::milliseconds(100));
        auto count = 0;
        vector_store_client_tester::set_dns_resolver(vs, [&count](auto const& host) -> future<inet_address> {
            BOOST_CHECK_EQUAL(host, "good.authority.here");
            count++;
            co_return inet_address("127.0.0.1");
        });

        co_await vs.start_background_tasks();

        BOOST_CHECK(co_await vector_store_client_tester::can_resolve_hostname(vs));
        BOOST_CHECK_EQUAL(count, 1);
        count = 0;
        vector_store_client_tester::trigger_dns_resolver(vs);
        vector_store_client_tester::trigger_dns_resolver(vs);
        vector_store_client_tester::trigger_dns_resolver(vs);
        vector_store_client_tester::trigger_dns_resolver(vs);
        vector_store_client_tester::trigger_dns_resolver(vs);
        co_await sleep(std::chrono::milliseconds(20)); // wait for the next DNS refresh
        BOOST_CHECK(co_await vector_store_client_tester::can_resolve_hostname(vs));
        BOOST_CHECK_GE(count, 1);
        BOOST_CHECK_LE(count, 2);

        co_await vs.stop();
    }
}

