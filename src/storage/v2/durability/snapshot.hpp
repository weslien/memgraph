// Copyright 2025 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "replication/epoch.hpp"
#include "storage/v2/config.hpp"
#include "storage/v2/durability/metadata.hpp"
#include "storage/v2/edge.hpp"
#include "storage/v2/enum_store.hpp"
#include "storage/v2/indices/indices.hpp"
#include "storage/v2/name_id_mapper.hpp"
#include "storage/v2/schema_info.hpp"
#include "storage/v2/transaction.hpp"
#include "storage/v2/vertex.hpp"
#include "utils/file_locker.hpp"
#include "utils/observer.hpp"
#include "utils/skip_list.hpp"

namespace memgraph::storage::durability {

/// Structure used to hold information about a snapshot.
struct SnapshotInfo {
  uint64_t offset_edges;
  uint64_t offset_vertices;
  uint64_t offset_indices;
  uint64_t offset_edge_indices;
  uint64_t offset_constraints;
  uint64_t offset_mapper;
  uint64_t offset_enums;
  uint64_t offset_epoch_history;
  uint64_t offset_metadata;
  uint64_t offset_edge_batches;
  uint64_t offset_vertex_batches;

  std::string uuid;
  std::string epoch_id;
  uint64_t start_timestamp;
  uint64_t durable_timestamp;
  uint64_t edges_count;
  uint64_t vertices_count;
};

/// Structure used to hold information about the snapshot that has been
/// recovered.
struct RecoveredSnapshot {
  SnapshotInfo snapshot_info;
  RecoveryInfo recovery_info;
  RecoveredIndicesAndConstraints indices_constraints;
};

/// Function used to read information about the snapshot file.
/// @throw RecoveryFailure
SnapshotInfo ReadSnapshotInfo(const std::filesystem::path &path);

void OverwriteSnapshotUUID(std::filesystem::path const &path, utils::UUID const &uuid);

/// Function used to load the snapshot data into the storage.
/// @throw RecoveryFailure
RecoveredSnapshot LoadSnapshot(std::filesystem::path const &path, utils::SkipList<Vertex> *vertices,
                               utils::SkipList<Edge> *edges, utils::SkipList<EdgeMetadata> *edges_metadata,
                               std::deque<std::pair<std::string, uint64_t>> *epoch_history,
                               NameIdMapper *name_id_mapper, std::atomic<uint64_t> *edge_count, Config const &config,
                               memgraph::storage::EnumStore *enum_store,
                               memgraph::storage::SharedSchemaTracking *schema_info,
                               std::optional<SnapshotObserverInfo> const &snapshot_info = std::nullopt);

bool CreateSnapshot(Storage *storage, Transaction *transaction, const std::filesystem::path &snapshot_directory,
                    const std::filesystem::path &wal_directory, utils::SkipList<Vertex> *vertices,
                    utils::SkipList<Edge> *edges, utils::UUID const &uuid,
                    const memgraph::replication::ReplicationEpoch &epoch,
                    const std::deque<std::pair<std::string, uint64_t>> &epoch_history,
                    utils::FileRetainer *file_retainer, std::atomic_bool *abort_snapshot = nullptr);

}  // namespace memgraph::storage::durability
