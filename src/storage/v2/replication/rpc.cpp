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

#include "storage/v2/replication/rpc.hpp"
#include <cstdint>
#include <rpc/version.hpp>

#include "slk/streams.hpp"
#include "utils/enum.hpp"
#include "utils/typeinfo.hpp"

namespace memgraph {

namespace storage::replication {

void AppendDeltasReq::Save(const AppendDeltasReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self, builder);
}
void AppendDeltasReq::Load(AppendDeltasReq *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void AppendDeltasRes::Save(const AppendDeltasRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self, builder);
}
void AppendDeltasRes::Load(AppendDeltasRes *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }

void HeartbeatReq::Save(const HeartbeatReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self, builder);
}

void HeartbeatReq::Load(HeartbeatReq *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void HeartbeatRes::Save(const HeartbeatRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self, builder);
}
void HeartbeatRes::Load(HeartbeatRes *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void SnapshotReq::Save(const SnapshotReq &self, memgraph::slk::Builder *builder) { memgraph::slk::Save(self, builder); }
void SnapshotReq::Load(SnapshotReq *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void SnapshotRes::Save(const SnapshotRes &self, memgraph::slk::Builder *builder) { memgraph::slk::Save(self, builder); }
void SnapshotRes::Load(SnapshotRes *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void WalFilesReq::Save(const WalFilesReq &self, memgraph::slk::Builder *builder) { memgraph::slk::Save(self, builder); }
void WalFilesReq::Load(WalFilesReq *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void WalFilesRes::Save(const WalFilesRes &self, memgraph::slk::Builder *builder) { memgraph::slk::Save(self, builder); }
void WalFilesRes::Load(WalFilesRes *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void CurrentWalReq::Save(const CurrentWalReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self, builder);
}
void CurrentWalReq::Load(CurrentWalReq *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }
void CurrentWalRes::Save(const CurrentWalRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self, builder);
}
void CurrentWalRes::Load(CurrentWalRes *self, memgraph::slk::Reader *reader) { memgraph::slk::Load(self, reader); }

}  // namespace storage::replication

constexpr utils::TypeInfo storage::replication::AppendDeltasReq::kType{utils::TypeId::REP_APPEND_DELTAS_REQ,
                                                                       "AppendDeltasReq", nullptr};

constexpr utils::TypeInfo storage::replication::AppendDeltasRes::kType{utils::TypeId::REP_APPEND_DELTAS_RES,
                                                                       "AppendDeltasRes", nullptr};

constexpr utils::TypeInfo storage::replication::InProgressRes::kType{utils::TypeId::REP_IN_PROGRESS_RES,
                                                                     "InProgressRes", nullptr};

constexpr utils::TypeInfo storage::replication::HeartbeatReq::kType{utils::TypeId::REP_HEARTBEAT_REQ, "HeartbeatReq",
                                                                    nullptr};

constexpr utils::TypeInfo storage::replication::HeartbeatRes::kType{utils::TypeId::REP_HEARTBEAT_RES, "HeartbeatRes",
                                                                    nullptr};

constexpr utils::TypeInfo storage::replication::SnapshotReq::kType{utils::TypeId::REP_SNAPSHOT_REQ, "SnapshotReq",
                                                                   nullptr};

constexpr utils::TypeInfo storage::replication::SnapshotRes::kType{utils::TypeId::REP_SNAPSHOT_RES, "SnapshotRes",
                                                                   nullptr};

constexpr utils::TypeInfo storage::replication::WalFilesReq::kType{utils::TypeId::REP_WALFILES_REQ, "WalFilesReq",
                                                                   nullptr};

constexpr utils::TypeInfo storage::replication::WalFilesRes::kType{utils::TypeId::REP_WALFILES_RES, "WalFilesRes",
                                                                   nullptr};

constexpr utils::TypeInfo storage::replication::CurrentWalReq::kType{utils::TypeId::REP_CURRENT_WAL_REQ,
                                                                     "CurrentWalReq", nullptr};

constexpr utils::TypeInfo storage::replication::CurrentWalRes::kType{utils::TypeId::REP_CURRENT_WAL_RES,
                                                                     "CurrentWalRes", nullptr};

// Autogenerated SLK serialization code
namespace slk {

// Serialize code for CurrentWalRes

void Save(const memgraph::storage::replication::CurrentWalRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.current_commit_timestamp, builder);
}

void Load(memgraph::storage::replication::CurrentWalRes *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->current_commit_timestamp, reader);
}

// Serialize code for CurrentWalReq

void Save(const memgraph::storage::replication::CurrentWalReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.main_uuid, builder);
  memgraph::slk::Save(self.uuid, builder);
  memgraph::slk::Save(self.reset_needed, builder);
}

void Load(memgraph::storage::replication::CurrentWalReq *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->main_uuid, reader);
  memgraph::slk::Load(&self->uuid, reader);
  memgraph::slk::Load(&self->reset_needed, reader);
}

// Serialize code for WalFilesRes

void Save(const memgraph::storage::replication::WalFilesRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.current_commit_timestamp, builder);
}

void Load(memgraph::storage::replication::WalFilesRes *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->current_commit_timestamp, reader);
}

// Serialize code for WalFilesReq

void Save(const memgraph::storage::replication::WalFilesReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.main_uuid, builder);
  memgraph::slk::Save(self.uuid, builder);
  memgraph::slk::Save(self.file_number, builder);
  memgraph::slk::Save(self.reset_needed, builder);
}

void Load(memgraph::storage::replication::WalFilesReq *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->main_uuid, reader);
  memgraph::slk::Load(&self->uuid, reader);
  memgraph::slk::Load(&self->file_number, reader);
  memgraph::slk::Load(&self->reset_needed, reader);
}

// Serialize code for SnapshotRes

void Save(const memgraph::storage::replication::SnapshotRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.current_commit_timestamp, builder);
}

void Load(memgraph::storage::replication::SnapshotRes *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->current_commit_timestamp, reader);
}

// Serialize code for SnapshotReq

void Save(const memgraph::storage::replication::SnapshotReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.main_uuid, builder);
  memgraph::slk::Save(self.storage_uuid, builder);
}

void Load(memgraph::storage::replication::SnapshotReq *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->main_uuid, reader);
  memgraph::slk::Load(&self->storage_uuid, reader);
}

// Serialize code for HeartbeatRes

void Save(const memgraph::storage::replication::HeartbeatRes &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.success, builder);
  memgraph::slk::Save(self.current_commit_timestamp, builder);
  memgraph::slk::Save(self.epoch_id, builder);
}

void Load(memgraph::storage::replication::HeartbeatRes *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->success, reader);
  memgraph::slk::Load(&self->current_commit_timestamp, reader);
  memgraph::slk::Load(&self->epoch_id, reader);
}

// Serialize code for HeartbeatReq

void Save(const memgraph::storage::replication::HeartbeatReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.main_uuid, builder);
  memgraph::slk::Save(self.uuid, builder);
  memgraph::slk::Save(self.main_commit_timestamp, builder);
  memgraph::slk::Save(self.epoch_id, builder);
}

void Load(memgraph::storage::replication::HeartbeatReq *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->main_uuid, reader);
  memgraph::slk::Load(&self->uuid, reader);
  memgraph::slk::Load(&self->main_commit_timestamp, reader);
  memgraph::slk::Load(&self->epoch_id, reader);
}

// Serialize code for AppendDeltasRes

void Save(const memgraph::storage::replication::AppendDeltasRes &self, memgraph::slk::Builder *builder) {
  slk::Save(self.success, builder);
}

void Load(memgraph::storage::replication::AppendDeltasRes *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->success, reader);
}

// Serialize code for AppendDeltasReq

void Save(const memgraph::storage::replication::AppendDeltasReq &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.main_uuid, builder);
  memgraph::slk::Save(self.uuid, builder);
  memgraph::slk::Save(self.previous_commit_timestamp, builder);
  memgraph::slk::Save(self.seq_num, builder);
}

void Load(memgraph::storage::replication::AppendDeltasReq *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->main_uuid, reader);
  memgraph::slk::Load(&self->uuid, reader);
  memgraph::slk::Load(&self->previous_commit_timestamp, reader);
  memgraph::slk::Load(&self->seq_num, reader);
}

// Serialize SalientConfig
void Save(const memgraph::storage::SalientConfig &self, memgraph::slk::Builder *builder) {
  memgraph::slk::Save(self.name, builder);
  memgraph::slk::Save(self.uuid, builder);
  memgraph::slk::Save(utils::EnumToNum<3, uint8_t>(self.storage_mode), builder);
  memgraph::slk::Save(self.items.properties_on_edges, builder);
  memgraph::slk::Save(self.items.enable_schema_metadata, builder);
}

void Load(memgraph::storage::SalientConfig *self, memgraph::slk::Reader *reader) {
  memgraph::slk::Load(&self->name, reader);
  memgraph::slk::Load(&self->uuid, reader);
  uint8_t sm = 0;
  memgraph::slk::Load(&sm, reader);
  if (!utils::NumToEnum<3>(sm, self->storage_mode)) {
    throw SlkReaderException("Unexpected result line:{}!", __LINE__);
  }
  memgraph::slk::Load(&self->items.properties_on_edges, reader);
  memgraph::slk::Load(&self->items.enable_schema_metadata, reader);
}

}  // namespace slk
}  // namespace memgraph
