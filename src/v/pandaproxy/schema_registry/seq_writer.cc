//// Copyright 2021 Vectorized, Inc.
////
//// Use of this software is governed by the Business Source License
//// included in the file licenses/BSL.md
////
//// As of the Change Date specified in that file, in accordance with
//// the Business Source License, use of this software will be governed
//// by the Apache License, Version 2.

#include "pandaproxy/schema_registry/seq_writer.h"

#include "pandaproxy/error.h"
#include "pandaproxy/logger.h"
#include "pandaproxy/schema_registry/client_fetch_batch_reader.h"
#include "pandaproxy/schema_registry/storage.h"
#include "random/simple_time_jitter.h"
#include "vlog.h"

#include <seastar/core/coroutine.hh>

using namespace std::chrono_literals;

namespace pandaproxy::schema_registry {

// TODO: hook consume_to_store into advance_offset

/// Call this before reading from the store, if servicing
/// a REST API endpoint that requires global knowledge of latest
/// data (i.e. any listings)
ss::future<> seq_writer::read_sync() {
    auto offsets = co_await _client.local().list_offsets(
      model::schema_registry_internal_tp);

    const auto& topics = offsets.data.topics;
    if (topics.size() != 1 || topics.front().partitions.size() != 1) {
        auto ec = kafka::error_code::unknown_topic_or_partition;
        throw kafka::exception(ec, make_error_code(ec).message());
    }

    const auto& partition = topics.front().partitions.front();
    if (partition.error_code != kafka::error_code::none) {
        auto ec = partition.error_code;
        throw kafka::exception(ec, make_error_code(ec).message());
    }

    co_await wait_for(partition.offset - model::offset{1});
}

ss::future<> seq_writer::wait_for(model::offset offset) {
    auto fn = [offset](seq_writer& seq) {
        auto sem_units = ss::get_units(seq._wait_for_sem, 1);
        if (offset > seq._loaded_offset) {
            vlog(
              plog.debug,
              "wait_for dirty!  Reading {}..{}",
              seq._loaded_offset,
              offset);

            return make_client_fetch_batch_reader(
                     seq._client.local(),
                     model::schema_registry_internal_tp,
                     seq._loaded_offset + model::offset{1},
                     offset + model::offset{1})
              .consume(consume_to_store{seq._store, seq}, model::no_timeout);
        } else {
            vlog(plog.debug, "wait_for clean (offset  {})", offset);
            return ss::make_ready_future<>();
        }
    };

    co_await container().invoke_on(0, _smp_opts, fn);
}

/// Helper for write methods that need to check + retry if their
/// write landed where they expected it to.
///
/// \param write_at Offset at which caller expects their write to land
/// \param batch Message to write
/// \return true if the write landed at `write_at`, else false
ss::future<bool> seq_writer::produce_and_check(
  model::offset write_at, model::record_batch batch) {
    kafka::partition_produce_response res
      = co_await _client.local().produce_record_batch(
        model::schema_registry_internal_tp, std::move(batch));

    // TODO(Ben): Check the error reporting here
    if (res.error_code != kafka::error_code::none) {
        throw kafka::exception(res.error_code, *res.error_message);
    }

    auto wrote_at = res.base_offset;
    if (wrote_at == write_at) {
        vlog(plog.debug, "seq_writer: Successful write at {}", wrote_at);

        co_return true;
    } else {
        vlog(
          plog.debug,
          "seq_writer: Failed write at {} (wrote at {})",
          write_at,
          wrote_at);
        co_return false;
    }
};

ss::future<> seq_writer::advance_offset(model::offset offset) {
    auto remote = [offset](seq_writer& s) { s.advance_offset_inner(offset); };

    return container().invoke_on(0, _smp_opts, remote);
}

void seq_writer::advance_offset_inner(model::offset offset) {
    if (_loaded_offset < offset) {
        vlog(
          plog.debug,
          "seq_writer::advance_offset {}->{}",
          _loaded_offset,
          offset);
        _loaded_offset = offset;
    } else {
        vlog(
          plog.debug,
          "seq_writer::advance_offset ignoring {} (have {})",
          offset,
          _loaded_offset);
    }
}

ss::future<schema_id> seq_writer::write_subject_version(
  subject sub, schema_definition def, schema_type type) {
    auto do_write = [sub, def, type](
                      model::offset write_at,
                      seq_writer& seq) -> ss::future<std::optional<schema_id>> {
        // Check if store already contains this data: if
        // so, we do no I/O and return the schema ID.
        auto projected = co_await seq._store.project_ids(sub, def, type);

        if (!projected.inserted) {
            vlog(plog.debug, "write_subject_version: no-op");
            co_return projected.id;
        } else {
            vlog(
              plog.debug,
              "seq_writer::write_subject_version project offset={} subject={} "
              "schema={} "
              "version={}",
              write_at,
              sub,
              projected.id,
              projected.version);

            auto my_node_id = config::shard_local_cfg().node_id();

            auto key = schema_key{
              .seq{write_at},
              .node{my_node_id},
              .sub{sub},
              .version{projected.version}};
            auto value = schema_value{
              .sub{std::move(sub)},
              .version{projected.version},
              .type = type,
              .id{projected.id},
              .schema{std::move(def)},
              .deleted = is_deleted::no};

            auto batch = as_record_batch(key, value);

            auto success = co_await seq.produce_and_check(
              write_at, std::move(batch));
            if (success) {
                auto applier = consume_to_store(seq._store, seq);
                co_await applier.apply(write_at, key, value);
                seq.advance_offset_inner(write_at);
                co_return projected.id;
            } else {
                co_return std::nullopt;
            }
        }
    };

    return sequenced_write(do_write);
}

ss::future<bool> seq_writer::write_config(
  std::optional<subject> sub, compatibility_level compat) {
    auto do_write = [sub, compat](
                      model::offset write_at,
                      seq_writer& seq) -> ss::future<std::optional<bool>> {
        vlog(
          plog.debug,
          "write_config sub={} compat={} offset={}",
          sub,
          to_string_view(compat),
          write_at);

        // Check for no-op case
        compatibility_level existing;
        if (sub.has_value()) {
            existing = co_await seq._store.get_compatibility(sub.value());
        } else {
            existing = co_await seq._store.get_compatibility();
        }
        if (existing == compat) {
            co_return false;
        }

        auto my_node_id = config::shard_local_cfg().node_id();
        auto key = config_key{
          .seq{write_at}, .node{my_node_id}, .sub{std::move(sub)}};
        auto value = config_value{.compat = compat};
        auto batch = as_record_batch(key, value);

        auto success = co_await seq.produce_and_check(
          write_at, std::move(batch));
        if (success) {
            auto applier = consume_to_store(seq._store, seq);
            co_await applier.apply(write_at, key, value);
            seq.advance_offset_inner(write_at);
            co_return true;
        } else {
            // Pass up a None, our caller's cue to retry
            co_return std::nullopt;
        }
    };

    co_return co_await sequenced_write(do_write);
}

/// Impermanent delete: update a version with is_deleted=true
ss::future<bool>
seq_writer::delete_subject_version(subject sub, schema_version version) {
    auto do_write = [sub, version](
                      model::offset write_at,
                      seq_writer& seq) -> ss::future<std::optional<bool>> {
        auto s_res = co_await seq._store.get_subject_schema(
          sub, version, include_deleted::yes);
        subject_schema ss = std::move(s_res);

        auto my_node_id = config::shard_local_cfg().node_id();
        auto key = schema_key{
          .seq{write_at}, .node{my_node_id}, .sub{sub}, .version{version}};
        vlog(plog.debug, "seq_writer::delete_subject_version {}", key);
        auto value = schema_value{
          .sub{sub},
          .version{version},
          .type = ss.type,
          .id{ss.id},
          .schema{std::move(ss.definition)},
          .deleted{is_deleted::yes}};

        auto batch = as_record_batch(key, value);

        auto success = co_await seq.produce_and_check(
          write_at, std::move(batch));
        if (success) {
            auto applier = consume_to_store(seq._store, seq);
            co_await applier.apply(write_at, key, value);
            seq.advance_offset_inner(write_at);
            co_return true;
        } else {
            // Pass up a None, our caller's cue to retry
            co_return std::nullopt;
        }
    };

    co_return co_await sequenced_write(do_write);
}

ss::future<std::vector<schema_version>>
seq_writer::delete_subject_impermanent(subject sub) {
    vlog(plog.debug, "delete_subject_impermanent sub={}", sub);
    auto do_write = [sub](model::offset write_at, seq_writer& seq)
      -> ss::future<std::optional<std::vector<schema_version>>> {
        // Grab the versions before they're gone.
        std::vector<schema_version> versions = co_await seq._store.get_versions(
          sub, include_deleted::yes);

        // Inspect the subject to see if its already deleted
        if (co_await seq._store.is_subject_deleted(sub)) {
            co_return std::make_optional(versions);
        }

        // Proceed to write
        auto my_node_id = config::shard_local_cfg().node_id();
        auto version = versions.back();
        auto key = delete_subject_key{
          .seq{write_at}, .node{my_node_id}, .sub{sub}};
        auto value = delete_subject_value{.sub{sub}, .version{version}};
        auto batch = as_record_batch(key, value);

        auto success = co_await seq.produce_and_check(
          write_at, std::move(batch));
        if (success) {
            auto applier = consume_to_store(seq._store, seq);
            co_await applier.apply(write_at, key, value);
            seq.advance_offset_inner(write_at);
            co_return versions;
        } else {
            // Pass up a None, our caller's cue to retry
            co_return std::nullopt;
        }
    };

    co_return co_await sequenced_write(do_write);
}

/// Permanent deletions (i.e. writing tombstones for previous sequenced
/// records) do not themselves need sequence numbers.
/// Include a version if we are only to hard delete that version, otherwise
/// will hard-delete the whole subject.
ss::future<std::vector<schema_version>> seq_writer::delete_subject_permanent(
  subject sub, std::optional<schema_version> version) {
    return container().invoke_on(0, _smp_opts, [sub, version](seq_writer& seq) {
        auto units = ss::get_units(seq._write_sem, 1);
        return seq.delete_subject_permanent_inner(sub, version);
    });
}

ss::future<std::vector<schema_version>>
seq_writer::delete_subject_permanent_inner(
  subject sub, std::optional<schema_version> version) {
    std::vector<seq_marker> sequences;
    /// Check for whether our victim is already soft-deleted happens
    /// within these store functions (will throw a 404-equivalent if so)
    vlog(plog.debug, "delete_subject_permanent sub={}", sub);
    if (version.has_value()) {
        sequences = co_await _store.get_subject_version_written_at(
          sub, version.value());
    } else {
        sequences = co_await _store.get_subject_written_at(sub);
    }

    storage::record_batch_builder rb{
      model::record_batch_type::raft_data, model::offset{0}};

    std::vector<std::variant<schema_key, delete_subject_key, config_key>> keys;
    for (auto s : sequences) {
        vlog(
          plog.debug,
          "Delete subject_permanent: tombstoning sub={} at {}",
          sub,
          s);

        // Assumption: magic is the same as it was when key was
        // originally read.
        switch (s.key_type) {
        case seq_marker_key_type::schema: {
            auto key = schema_key{
              .seq{s.seq}, .node{s.node}, .sub{sub}, .version{s.version}};
            keys.push_back(key);
            rb.add_raw_kv(to_json_iobuf(std::move(key)), std::nullopt);
        } break;
        case seq_marker_key_type::delete_subject: {
            auto key = delete_subject_key{
              .seq{s.seq}, .node{s.node}, .sub{sub}};
            keys.push_back(key);
            rb.add_raw_kv(to_json_iobuf(std::move(key)), std::nullopt);
        } break;
        case seq_marker_key_type::config: {
            auto key = config_key{.seq{s.seq}, .node{s.node}, .sub{sub}};
            keys.push_back(key);
            rb.add_raw_kv(to_json_iobuf(std::move(key)), std::nullopt);
        } break;
        default:
            assert(false);
        }
    }

    // If a subject is in the store, it must have been replayed some somewhere,
    // so there must be some entries in the list of keys to tombstone.
    assert(!keys.empty());

    // Produce tombstones.  We do not need to check where they landed, because
    // these can arrive in any order and be safely repeated.
    auto batch = std::move(rb).build();
    assert(batch.record_count() > 0);

    kafka::partition_produce_response res
      = co_await _client.local().produce_record_batch(
        model::schema_registry_internal_tp, std::move(batch));
    if (res.error_code != kafka::error_code::none) {
        vlog(
          plog.error,
          "Error writing to schema topic: {} {}",
          res.error_code,
          res.error_message);
        throw kafka::exception(res.error_code, *res.error_message);
    }

    // Replay the persisted deletions into our store
    auto applier = consume_to_store(_store, *this);
    auto offset = res.base_offset;
    for (auto k : keys) {
        if (auto skey = std::get_if<schema_key>(&k)) {
            co_await applier.apply(offset, *skey, std::nullopt);
        } else if (auto dkey = std::get_if<delete_subject_key>(&k)) {
            co_await applier.apply(offset, *dkey, std::nullopt);
        } else if (auto ckey = std::get_if<config_key>(&k)) {
            co_await applier.apply(offset, *ckey, std::nullopt);
        } else {
            // Unreachable!
            assert(false);
        }
        advance_offset_inner(offset);
        offset++;
    }

    co_return std::vector<schema_version>();
}

} // namespace pandaproxy::schema_registry