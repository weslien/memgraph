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

#include "storage/v2/disk/rocksdb_storage.hpp"
#include "storage/v2/indices/label_property_index.hpp"
#include "utils/synchronized.hpp"

namespace memgraph::storage {

class DiskLabelPropertyIndex : public storage::LabelPropertyIndex {
 public:
  explicit DiskLabelPropertyIndex(const Config &config);

  bool CreateIndex(LabelId label, PropertyId property,
                   const std::vector<std::pair<std::string, std::string>> &vertices);

  std::unique_ptr<rocksdb::Transaction> CreateRocksDBTransaction() const;

  std::unique_ptr<rocksdb::Transaction> CreateAllReadingRocksDBTransaction() const;

  [[nodiscard]] bool SyncVertexToLabelPropertyIndexStorage(const Vertex &vertex, uint64_t commit_timestamp) const;

  [[nodiscard]] bool ClearDeletedVertex(std::string_view gid, uint64_t transaction_commit_timestamp) const;

  [[nodiscard]] bool DeleteVerticesWithRemovedIndexingLabel(uint64_t transaction_start_timestamp,
                                                            uint64_t transaction_commit_timestamp);

  void UpdateOnAddLabel(LabelId added_label, Vertex *vertex_after_update, const Transaction &tx) override;

  void UpdateOnRemoveLabel(LabelId removed_label, Vertex *vertex_after_update, const Transaction &tx) override;

  void UpdateOnSetProperty(PropertyId property, const PropertyValue &value, Vertex *vertex,
                           const Transaction &tx) override{};

  bool DropIndex(LabelId label, std::vector<PropertyPath> const &properties) override;

  bool IndexExists(LabelId label, std::span<PropertyPath const> properties) const override;

  auto RelevantLabelPropertiesIndicesInfo(std::span<LabelId const> labels,
                                          std::span<PropertyPath const> properties) const
      -> std::vector<LabelPropertiesIndicesInfo> override;

  std::vector<std::pair<LabelId, std::vector<PropertyPath>>> ListIndices() const override;

  uint64_t ApproximateVertexCount(LabelId label, std::span<PropertyPath const> properties) const override;

  uint64_t ApproximateVertexCount(LabelId label, std::span<PropertyPath const> properties,
                                  std::span<PropertyValue const> values) const override;

  uint64_t ApproximateVertexCount(LabelId label, std::span<PropertyPath const> properties,
                                  std::span<PropertyValueRange const> bounds) const override;

  RocksDBStorage *GetRocksDBStorage() const;

  void LoadIndexInfo(const std::vector<std::string> &keys);

  std::set<std::pair<LabelId, PropertyId>> GetInfo() const;

  void DropGraphClearIndices() override{};

  void AbortEntries(AbortableInfo const &info, uint64_t start_timestamp) override;

 private:
  utils::Synchronized<std::map<uint64_t, std::map<Gid, std::vector<std::pair<LabelId, PropertyId>>>>>
      entries_for_deletion;
  std::set<std::pair<LabelId, PropertyId>> index_;
  std::unique_ptr<RocksDBStorage> kvstore_;
};

}  // namespace memgraph::storage
