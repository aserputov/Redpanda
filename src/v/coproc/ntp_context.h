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

#include "cluster/partition.h"
#include "cluster/partition_manager.h"
#include "config/configuration.h"
#include "coproc/sys_refs.h"
#include "coproc/types.h"
#include "random/simple_time_jitter.h"
#include "rpc/reconnect_transport.h"
#include "utils/mutex.h"

#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>

#include <absl/container/btree_map.h>
#include <absl/container/node_hash_map.h>

#include <chrono>
#include <exception>

using namespace std::chrono_literals;

namespace coproc {

/// Structure representing state about input topics that scripts will subscribe
/// to. ss::shared_ptrs to ntp_contexts will be used as there may be many
/// subscribers to an input ntp
struct ntp_context {
    struct offset_pair {
        model::offset last_read{};
        model::offset last_acked{};
    };

    using offset_tracker = absl::btree_map<script_id, offset_pair>;

    explicit ntp_context(ss::lw_shared_ptr<cluster::partition> p)
      : partition(std::move(p)) {}

    const model::ntp& ntp() const { return partition->ntp(); }

    /// Reference to a partition for reading from an input source
    ss::lw_shared_ptr<cluster::partition> partition;

    /// Interested scripts write their last read offset of the input ntp
    offset_tracker offsets;
};

using ntp_context_cache
  = absl::node_hash_map<model::ntp, ss::lw_shared_ptr<ntp_context>>;

/// One instance of this struct exists per shard (held by the pacemaker) and a
/// reference will be passed to all script_contexts on that shard. The data
/// members represent resources that will be shared across all script_contexts.
struct shared_script_resources {
    /// So that there is some variability between the sleep values across all
    /// scripts during their fibers call to ss::sleep_abortable
    simple_time_jitter<ss::lowres_clock> jitter{1s};

    /// Max amount of requests allowed to concurrently hold data in memory
    ss::semaphore read_sem{
      config::shard_local_cfg().coproc_max_ingest_bytes.value()};

    /// Underlying transport connection to the wasm engine
    rpc::reconnect_transport transport;

    /// A mutex per materialized log is required as concurrency is not
    /// guaranteed across script contexts. Two scripts writing to the same
    /// underlying log must not have portions of the writes be ordered within
    /// the seastar reactor
    /// NOTE: Pointer stability is explicity requested due to the way iterators
    /// to elements within the collection are used
    absl::node_hash_map<model::ntp, mutex> log_mtx;

    /// References to other redpanda components
    sys_refs& rs;

    shared_script_resources(rpc::reconnect_transport t, sys_refs& rs)
      : transport(std::move(t))
      , rs(rs) {}
};

} // namespace coproc
