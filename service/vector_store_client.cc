/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include "vector_store_client.hh"
#include "db/config.hh"
#include "exceptions/exceptions.hh"
#include <charconv>
#include <regex>
#include <seastar/http/client.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>

namespace {

using configuration_exception = exceptions::configuration_exception;
using host_name = service::vector_store_client::host_name;
using http_client = http::experimental::client;
using inet_address = seastar::net::inet_address;
using milliseconds = std::chrono::milliseconds;
using port_number = service::vector_store_client::port_number;
using time_point = lowres_clock::time_point;

// Minimum interval between cleanup tasks
constexpr auto CLEANUP_INTERVAL = std::chrono::seconds(5);

// Minimum interval between dns name refreshes
constexpr auto DNS_REFRESH_INTERVAL = std::chrono::seconds(5);

/// Timeout for waiting for a new client to be available
constexpr auto WAIT_FOR_CLIENT_TIMEOUT = std::chrono::seconds(5);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
logging::logger vslogger("vector_store_client");

auto parse_port(std::string const& port_txt) -> std::optional<port_number> {
    auto port = port_number{};
    auto [ptr, ec] = std::from_chars(&*port_txt.begin(), &*port_txt.end(), port);
    if (*ptr != '\0' || ec != std::errc{}) {
        return std::nullopt;
    }
    return port;
}

auto parse_service_uri(std::string_view uri) -> std::optional<std::tuple<host_name, port_number>> {
    constexpr auto URI_REGEX = R"(^http:\/\/([a-z0-9._-]+):([0-9]+)$)";
    auto const uri_regex = std::regex(URI_REGEX);
    auto uri_match = std::smatch{};
    auto uri_txt = std::string(uri);
    if (!std::regex_match(uri_txt, uri_match, uri_regex) || uri_match.size() != 3) {
        return {};
    }
    auto host = uri_match[1].str();
    auto port = parse_port(uri_match[2].str());
    if (!port) {
        return {};
    }
    return {{host, *port}};
}

} // namespace

namespace service {

struct vector_store_client::impl {
    lw_shared_ptr<http_client> current_client;           ///< The current http client
    std::vector<lw_shared_ptr<http_client>> old_clients; ///< The current http client
    host_name host;                                      ///< The host name for the vector-store service.
    port_number port{};                                  ///< The port number for the vector-store service.
    inet_address addr;                                   ///< The address for the vector-store service.
    time_point last_dns_refresh;                         ///< The last time the DNS service was refreshed to get the vector-store service address.
    gate tasks_gate;                                     ///< Gate to control tasks
    condition_variable refresh_cv;                       ///< Condition variable to signal that the service address should be refreshed.
    condition_variable refresh_client_cv;                ///< Condition variable to signal that a new client is available.
    condition_variable refresh_stop_cv;                  ///< Condition variable to wait for the refresh task to stop.
    condition_variable cleanup_stop_cv;                  ///< Condition variable to wait for the cleanup task to stop.
    bool stop_refreshing{};                              ///< Flag to stop refreshing the service address.
    milliseconds dns_refresh_interval = DNS_REFRESH_INTERVAL;
    milliseconds wait_for_client_timeout = WAIT_FOR_CLIENT_TIMEOUT;
    std::function<future<inet_address>(sstring const&)> dns_resolver;

    impl(host_name host_, port_number port_)
        : host(std::move(host_))
        , port(port_)
        , dns_resolver([](auto const& host) {
            return net::dns::resolve_name(host);
        }) {
    }

    auto refresh_addr() -> future<> {
        auto new_addr = inet_address{};
        try {
            new_addr = co_await dns_resolver(host);
        } catch (std::system_error const&) {
            // addr should be empty if the resolution failed
        }
        if (new_addr.is_addr_any()) {
            current_client = nullptr;
            co_return;
        }

        // Check if the new address is the same as the current one
        if (current_client && new_addr == addr) {
            co_return;
        }

        addr = new_addr;
        old_clients.emplace_back(current_client);
        current_client = make_lw_shared<http_client>(socket_address(addr, port));
    }

    auto refresh_stop_wait(time_point timeout) -> future<> {
        try {
            co_await refresh_stop_cv.wait(timeout);
        } catch (condition_variable_timed_out&) {
        }
    }

    /// Refresh the vector store service IP address from the dns name. Returns true if the address was refreshed.
    auto refresh_addr_task() -> future<> {
        auto requested_refresh = false;
        for (;;) {
            if (stop_refreshing) {
                break;
            }

            // Do not refresh the service address too often
            auto now = lowres_clock::now();
            if (now - last_dns_refresh > dns_refresh_interval) {
                last_dns_refresh = now;
                co_await refresh_addr();
            }

            if (requested_refresh || !current_client) {
                // Wait till the end of the refreshing interval
                co_await refresh_stop_wait(last_dns_refresh + dns_refresh_interval);
                continue;
            }

            if (stop_refreshing) {
                break;
            }

            // new client is available
            refresh_client_cv.broadcast();

            requested_refresh = false;
            co_await refresh_cv.when();
            requested_refresh = true;
        }
    }

    void trigger_dns_refresh() {
        refresh_cv.signal();
    }

    auto cleanup() -> future<> {
        for (auto& client : old_clients) {
            if (client && client.owned()) {
                co_await client->close();
                client = nullptr;
            }
        }
        std::erase_if(old_clients, [](auto const& client) {
            return !client;
        });
    }

    /// Refresh the vector store service IP address from the dns name. Returns true if the address was refreshed.
    auto cleanup_task() -> future<> {
        for (;;) {
            try {
                co_await cleanup_stop_cv.wait(lowres_clock::now() + CLEANUP_INTERVAL);
                break;
            } catch (condition_variable_timed_out&) {
            }
            co_await cleanup();
        }
        co_await cleanup();
        if (current_client) {
            co_await current_client->close();
        }
        current_client = nullptr;
    }

    struct get_client_response {
        lw_shared_ptr<http_client> client; ///< The http client.
        host_name host;                    ///< The host name for the vector-store service.
    };

    auto get_client() -> future<std::expected<get_client_response, addr_unavailable>> {
        if (current_client) {
            co_return get_client_response{.client = current_client, .host = host};
        }

        trigger_dns_refresh();

        try {
            co_await refresh_client_cv.wait(lowres_clock::now() + wait_for_client_timeout);
        } catch (condition_variable_timed_out&) {
        }

        if (!current_client || stop_refreshing) {
            co_return std::unexpected{addr_unavailable{}};
        }
        co_return get_client_response{.client = current_client, .host = host};
    }
};

vector_store_client::vector_store_client(config const& cfg) {
    auto config_uri = cfg.vector_store_uri();
    if (config_uri.empty()) {
        vslogger.info("Vector Store service URI is not configured.");
        return;
    }

    auto parsed_uri = parse_service_uri(config_uri);
    if (!parsed_uri) {
        throw configuration_exception(format("Invalid Vector Store service URI: {}", config_uri));
    }

    auto [host, port] = *parsed_uri;
    _impl = std::make_unique<impl>(std::move(host), port);
    vslogger.info("Vector Store service uri = {}:{}.", _impl->host, _impl->port);
}

vector_store_client::~vector_store_client() = default;

auto vector_store_client::start_background_tasks() -> future<> {
    if (is_disabled()) {
        co_return;
    }

    /// start the background task to refresh the service address
    (void)try_with_gate(_impl->tasks_gate, [this] {
        return _impl->refresh_addr_task();
    }).handle_exception([](std::exception_ptr eptr) {
        vslogger.error("Failed to start a Vector Store Client refresh task: {}", eptr);
    });

    /// start the background task to cleanup
    (void)try_with_gate(_impl->tasks_gate, [this] {
        return _impl->cleanup_task();
    }).handle_exception([](std::exception_ptr eptr) {
        vslogger.error("Failed to start a Vector Store Client cleanup task: {}", eptr);
    });
}

auto vector_store_client::stop() -> future<> {
    if (is_disabled()) {
        co_return;
    }

    _impl->stop_refreshing = true;
    _impl->refresh_stop_cv.signal();
    _impl->refresh_cv.signal();
    _impl->cleanup_stop_cv.signal();
    co_await _impl->tasks_gate.close();
}

auto vector_store_client::host() const -> std::expected<host_name, disabled> {
    if (is_disabled()) {
        return std::unexpected{disabled{}};
    }
    return {_impl->host};
}

auto vector_store_client::port() const -> std::expected<port_number, disabled> {
    if (is_disabled()) {
        return std::unexpected{disabled{}};
    }
    return {_impl->port};
}

void vector_store_client_tester::set_dns_refresh_interval(vector_store_client& vsc, std::chrono::milliseconds interval) {
    if (vsc.is_disabled()) {
        on_internal_error(vslogger, "Cannot set dns_refresh_interval on a disabled vector store client");
    }
    vsc._impl->dns_refresh_interval = interval;
}

void vector_store_client_tester::set_wait_for_client_timeout(vector_store_client& vsc, std::chrono::milliseconds timeout) {
    if (vsc.is_disabled()) {
        on_internal_error(vslogger, "Cannot set wait_for_client_timeout on a disabled vector store client");
    }
    vsc._impl->wait_for_client_timeout = timeout;
}

void vector_store_client_tester::set_dns_resolver(vector_store_client& vsc, std::function<future<inet_address>(sstring const&)> resolver) {
    if (vsc.is_disabled()) {
        on_internal_error(vslogger, "Cannot set dns_resolver on a disabled vector store client");
    }
    vsc._impl->dns_resolver = std::move(resolver);
}

void vector_store_client_tester::trigger_dns_resolver(vector_store_client& vsc) {
    if (vsc.is_disabled()) {
        on_internal_error(vslogger, "Cannot trigger a dns resolver on a disabled vector store client");
    }
    vsc._impl->trigger_dns_refresh();
}

auto vector_store_client_tester::can_resolve_hostname(vector_store_client& vsc) -> future<bool> {
    if (vsc.is_disabled()) {
        on_internal_error(vslogger, "Cannot check hostname resolving on a disabled vector store client");
    }
    co_return co_await vsc._impl->get_client();
}

} // namespace service

