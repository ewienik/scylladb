/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#pragma once

#include <cql3/statements/select_statement.hh>
#include <schema/schema.hh>
#include <seastar/core/shared_future.hh>

namespace seastar::http::experimental {
    class client;
}

namespace service {

struct vector_store final {
    using primary_key = cql3::statements::select_statement::primary_key;

    vector_store();
    ~vector_store();

    [[nodiscard]] auto ann(sstring const& keyspace, sstring const& name, schema_ptr schema, std::vector<float> const& embedding, std::size_t limit) const
            -> future<std::optional<std::vector<primary_key>>>;

private:
    std::unique_ptr<http::experimental::client> _client;
    sstring _host;
};

} // namespace service

