/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "rpc/connection.h"
#include "rpc/types.h"
#include "utils/hdr_hist.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/semaphore.hh>

#include <boost/intrusive/list.hpp>

#include <list>
#include <type_traits>
#include <vector>

namespace rpc {

class server {
public:
    // always guaranteed non-null
    class resources final {
    public:
        resources(server* s, ss::lw_shared_ptr<connection> c)
          : conn(std::move(c))
          , _s(s) {}

        // NOLINTNEXTLINE
        ss::lw_shared_ptr<connection> conn;

        server_probe& probe() { return _s->_probe; }
        ss::semaphore& memory() { return _s->_memory; }
        hdr_hist& hist() { return _s->_hist; }
        ss::gate& conn_gate() { return _s->_conn_gate; }
        ss::abort_source& abort_source() { return _s->_as; }
        bool abort_requested() const { return _s->_as.abort_requested(); }

    private:
        server* _s;
    };
    struct protocol {
        protocol() noexcept = default;
        protocol(protocol&&) noexcept = default;
        protocol& operator=(protocol&&) noexcept = default;
        protocol(const protocol&) = delete;
        protocol& operator=(const protocol&) = delete;

        virtual ~protocol() noexcept = default;
        virtual const char* name() const = 0;
        // the lifetime of all references here are guaranteed to live
        // until the end of the server (container/parent)
        virtual ss::future<> apply(server::resources) = 0;
    };

    explicit server(server_configuration);
    explicit server(ss::sharded<server_configuration>* s);
    server(server&&) noexcept = default;
    server& operator=(server&&) noexcept = delete;
    server(const server&) = delete;
    server& operator=(const server&) = delete;
    ~server();

    void set_protocol(std::unique_ptr<protocol> proto) {
        _proto = std::move(proto);
    }
    void start();

    /**
     * The RPC server can be shutdown in two phases. First phase initiated with
     * `shutdown_input` prevents server from accepting new requests and
     * connections. In second phases `wait_for_shutdown` caller waits for all
     * pending requests to finish. This interface is convinient as it allows
     * stopping the server without waiting for downstream services to stop
     * requests processing
     */
    void shutdown_input();
    ss::future<> wait_for_shutdown();
    /**
     * Stop function is a nop when `shutdown_input` was previously called. Left
     * here for convenience when dealing with `seastar::sharded` type
     */
    ss::future<> stop();

    const server_configuration cfg; // NOLINT
    const hdr_hist& histogram() const { return _hist; }

private:
    struct listener {
        ss::sstring name;
        ss::server_socket socket;

        listener(ss::sstring name, ss::server_socket socket)
          : name(std::move(name))
          , socket(std::move(socket)) {}
    };

    friend resources;
    ss::future<> accept(listener&);
    void setup_metrics();

    std::unique_ptr<protocol> _proto;
    ss::semaphore _memory;
    std::vector<std::unique_ptr<listener>> _listeners;
    boost::intrusive::list<connection> _connections;
    ss::abort_source _as;
    ss::gate _conn_gate;
    hdr_hist _hist;
    server_probe _probe;
    ss::metrics::metric_groups _metrics;
};

} // namespace rpc
