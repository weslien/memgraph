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

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dbms/constants.hpp"
#include "query/edge_accessor.hpp"
#include "query/query_user.hpp"
#include "query/vertex_accessor.hpp"
#include "storage/v2/id_types.hpp"

namespace memgraph::query {

class FineGrainedAuthChecker;

class DbAccessor;

class AuthChecker {
 public:
  virtual ~AuthChecker() = default;

  virtual std::shared_ptr<QueryUserOrRole> GenQueryUser(const std::optional<std::string> &username,
                                                        const std::vector<std::string> &rolenames) const = 0;

  virtual std::shared_ptr<QueryUserOrRole> GenEmptyUser() const = 0;

#ifdef MG_ENTERPRISE
  [[nodiscard]] virtual std::unique_ptr<FineGrainedAuthChecker> GetFineGrainedAuthChecker(
      std::shared_ptr<QueryUserOrRole> user, const DbAccessor *db_accessor) const = 0;
#endif
};
#ifdef MG_ENTERPRISE
class FineGrainedAuthChecker {
 public:
  virtual ~FineGrainedAuthChecker() = default;

  [[nodiscard]] virtual bool Has(const VertexAccessor &vertex, memgraph::storage::View view,
                                 AuthQuery::FineGrainedPrivilege fine_grained_privilege) const = 0;

  [[nodiscard]] virtual bool Has(const EdgeAccessor &edge,
                                 AuthQuery::FineGrainedPrivilege fine_grained_privilege) const = 0;

  [[nodiscard]] virtual bool Has(const std::vector<memgraph::storage::LabelId> &labels,
                                 AuthQuery::FineGrainedPrivilege fine_grained_privilege) const = 0;

  [[nodiscard]] virtual bool Has(const memgraph::storage::EdgeTypeId &edge_type,
                                 AuthQuery::FineGrainedPrivilege fine_grained_privilege) const = 0;

  [[nodiscard]] virtual bool HasGlobalPrivilegeOnVertices(
      AuthQuery::FineGrainedPrivilege fine_grained_privilege) const = 0;

  [[nodiscard]] virtual bool HasGlobalPrivilegeOnEdges(
      AuthQuery::FineGrainedPrivilege fine_grained_privilege) const = 0;
};

class AllowEverythingFineGrainedAuthChecker final : public FineGrainedAuthChecker {
 public:
  bool Has(const VertexAccessor & /*vertex*/, const memgraph::storage::View /*view*/,
           const AuthQuery::FineGrainedPrivilege /*fine_grained_privilege*/) const override {
    return true;
  }

  bool Has(const EdgeAccessor & /*edge*/,
           const AuthQuery::FineGrainedPrivilege /*fine_grained_privilege*/) const override {
    return true;
  }

  bool Has(const std::vector<memgraph::storage::LabelId> & /*labels*/,
           const AuthQuery::FineGrainedPrivilege /*fine_grained_privilege*/) const override {
    return true;
  }

  bool Has(const memgraph::storage::EdgeTypeId & /*edge_type*/,
           const AuthQuery::FineGrainedPrivilege /*fine_grained_privilege*/) const override {
    return true;
  }

  bool HasGlobalPrivilegeOnVertices(const AuthQuery::FineGrainedPrivilege /*fine_grained_privilege*/) const override {
    return true;
  }

  bool HasGlobalPrivilegeOnEdges(const AuthQuery::FineGrainedPrivilege /*fine_grained_privilege*/) const override {
    return true;
  }
};
#endif

class AllowEverythingAuthChecker final : public AuthChecker {
 public:
  struct User : query::QueryUserOrRole {
    User() : query::QueryUserOrRole{{}, {}} {}
    User(std::string name) : query::QueryUserOrRole{std::move(name), {}} {}
    bool IsAuthorized(const std::vector<AuthQuery::Privilege> & /*privileges*/,
                      std::optional<std::string_view> /*db_name*/, UserPolicy * /*policy*/) const override {
      return true;
    }
    std::vector<std::string> GetRolenames(std::optional<std::string> /*db_name*/) const override { return {}; }
#ifdef MG_ENTERPRISE
    bool CanImpersonate(const std::string & /*target*/, query::UserPolicy * /*policy*/,
                        std::optional<std::string_view> /*db_name*/ = std::nullopt) const override {
      return true;
    }
    std::string GetDefaultDB() const override { return std::string{dbms::kDefaultDB}; }
#endif
  };

  std::shared_ptr<query::QueryUserOrRole> GenQueryUser(const std::optional<std::string> &name,
                                                       const std::vector<std::string> & /*roles*/) const override {
    if (name) return std::make_shared<User>(*name);
    return std::make_shared<User>();
  }

  std::shared_ptr<QueryUserOrRole> GenEmptyUser() const override { return std::make_shared<User>(); }

#ifdef MG_ENTERPRISE
  std::unique_ptr<FineGrainedAuthChecker> GetFineGrainedAuthChecker(std::shared_ptr<QueryUserOrRole> /*user*/,
                                                                    const DbAccessor * /*dba*/) const override {
    return std::make_unique<AllowEverythingFineGrainedAuthChecker>();
  }
#endif
};

}  // namespace memgraph::query
