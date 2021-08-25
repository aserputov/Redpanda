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
#include "kafka/server/protocol.h"
#include "kafka/server/response.h"
#include "rpc/server.h"
#include "seastarx.h"
#include "security/acl.h"
#include "security/sasl_authentication.h"
#include "utils/hdr_hist.h"
#include "utils/named_type.h"

#include <seastar/core/future.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>

#include <absl/container/flat_hash_map.h>

#include <memory>

namespace kafka {

struct request_header;
class request_context;

class connection_context final
  : public ss::enable_lw_shared_from_this<connection_context> {
public:
    connection_context(
      protocol& p,
      rpc::server::resources&& r,
      security::sasl_server sasl,
      bool enable_authorizer) noexcept
      : _proto(p)
      , _rs(std::move(r))
      , _sasl(std::move(sasl))
      // tests may build a context without a live connection
      , _client_addr(_rs.conn ? _rs.conn->addr.addr() : ss::net::inet_address{})
      , _enable_authorizer(enable_authorizer) {}

    ~connection_context() noexcept = default;
    connection_context(const connection_context&) = delete;
    connection_context(connection_context&&) = delete;
    connection_context& operator=(const connection_context&) = delete;
    connection_context& operator=(connection_context&&) = delete;

    protocol& server() { return _proto; }
    const ss::sstring& listener() const { return _rs.conn->name(); }
    security::sasl_server& sasl() { return _sasl; }

    template<typename T>
    bool authorized(security::acl_operation operation, const T& name) {
        if (!_enable_authorizer) {
            return true;
        }
        auto user = sasl().principal();
        security::acl_principal principal(
          security::principal_type::user, std::move(user));
        return _proto.authorizer().authorized(
          name,
          operation,
          std::move(principal),
          security::acl_host(_client_addr));
    }

    ss::future<> process_one_request();
    bool is_finished_parsing() const;
    ss::net::inet_address client_host() const { return _client_addr; }

private:
    // used to track number of pending requests
    class request_tracker {
    public:
        explicit request_tracker(rpc::server_probe& probe)
          : _probe(probe) {
            _probe.request_received();
        }
        request_tracker(const request_tracker&) = delete;
        request_tracker(request_tracker&&) = delete;
        request_tracker& operator=(const request_tracker&) = delete;
        request_tracker& operator=(request_tracker&&) = delete;

        ~request_tracker() noexcept { _probe.request_completed(); }

    private:
        rpc::server_probe& _probe;
    };
    // used to pass around some internal state
    struct session_resources {
        ss::lowres_clock::duration backpressure_delay;
        ss::semaphore_units<> memlocks;
        ss::semaphore_units<> queue_units;
        std::unique_ptr<hdr_hist::measurement> method_latency;
    };

    /// called by throttle_request
    ss::future<ss::semaphore_units<>> reserve_request_units(size_t size);

    /// apply correct backpressure sequence
    ss::future<session_resources>
    throttle_request(const request_header&, size_t sz);

    ss::future<> dispatch_method_once(request_header, size_t sz);
    ss::future<> process_next_response();
    ss::future<> do_process(request_context);

    ss::future<> handle_auth_v0(size_t);

private:
    using sequence_id = named_type<uint64_t, struct kafka_protocol_sequence>;
    using map_t = absl::flat_hash_map<sequence_id, response_ptr>;

    protocol& _proto;
    rpc::server::resources _rs;
    sequence_id _next_response;
    sequence_id _seq_idx;
    map_t _responses;
    security::sasl_server _sasl;
    const ss::net::inet_address _client_addr;
    const bool _enable_authorizer;
};

} // namespace kafka
