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

#include <string>
#include <unordered_map>

#include "query/parameters.hpp"
#include "utils/fnv.hpp"

namespace memgraph::query::frontend {

// Strings used to replace original tokens. Different types are replaced with
// different token.
const std::string kStrippedIntToken = "0";
const std::string kStrippedDoubleToken = "0.0";
const std::string kStrippedStringToken = "\"a\"";
const std::string kStrippedBooleanToken = "true";

struct HashedString {
  HashedString() = default;
  HashedString(std::string str) : str_(std::move(str)) {}
  HashedString(HashedString const &) = default;
  HashedString(HashedString &&) = default;
  HashedString &operator=(HashedString const &) = default;
  HashedString &operator=(HashedString &&) = default;

  friend bool operator==(const HashedString &lhs, const HashedString &rhs) {
    return std::tie(lhs.hash_, lhs.str_) == std::tie(rhs.hash_, rhs.str_);
  }
  friend bool operator<(const HashedString &lhs, const HashedString &rhs) {
    return std::tie(lhs.hash_, lhs.str_) < std::tie(rhs.hash_, rhs.str_);
  }
  size_t hash() const { return hash_; }
  auto str() const -> std::string const & { return str_; }

 private:
  std::string str_{};
  size_t hash_{utils::Fnv(str_)};
};

/**
 * StrippedQuery contains:
 *     * stripped query
 *     * literals stripped from query
 *     * hash of stripped query
 */
class StrippedQuery {
 public:
  /**
   * Strips the input query and stores stripped query, stripped arguments and
   * stripped query hash.
   *
   * @param query Input query.
   */
  explicit StrippedQuery(std::string query);

  /**
   * Copy constructor is deleted because we don't want to make unnecessary
   * copies of this object (copying of string and vector could be expensive)
   */
  StrippedQuery(const StrippedQuery &other) = delete;
  StrippedQuery &operator=(const StrippedQuery &other) = delete;

  /**
   * Move is allowed operation because it is not expensive and we can
   * move the object after it was created.
   */
  StrippedQuery(StrippedQuery &&other) = default;
  StrippedQuery &operator=(StrippedQuery &&other) = default;

  auto stripped_query() const -> HashedString const & { return stripped_query_; }
  const auto &original_query() const { return original_; }
  const auto &literals() const { return literals_; }
  const auto &named_expressions() const { return named_exprs_; }
  const auto &parameters() const { return parameters_; }

 private:
  // Return len of matched keyword if something is matched, otherwise 0.
  int MatchKeyword(int start) const;
  int MatchString(int start) const;
  int MatchSpecial(int start) const;
  int MatchDecimalInt(int start) const;
  int MatchOctalInt(int start) const;
  int MatchHexadecimalInt(int start) const;
  int MatchReal(int start) const;
  int MatchParameter(int start) const;
  int MatchEscapedName(int start) const;
  int MatchUnescapedName(int start) const;
  int MatchWhitespaceAndComments(int start) const;

  // Original query.
  std::string original_;

  // Stripped query.
  HashedString stripped_query_;

  // Token positions of stripped out literals mapped to their values.
  // TODO: Parameters class really doesn't provide anything interesting. This
  // could be changed to std::unordered_map, but first we need to rewrite (or
  // get rid of) hardcoded queries which expect Parameters.
  Parameters literals_;

  // Token positions of query parameters mapped to their names.
  std::unordered_map<int, std::string> parameters_;

  // Token positions of nonaliased named expressions in return statement mapped
  // to their original (unstripped) string.
  std::unordered_map<int, std::string> named_exprs_;
};

}  // namespace memgraph::query::frontend

namespace std {
template <>
struct hash<memgraph::query::frontend::HashedString> {
  size_t operator()(const memgraph::query::frontend::HashedString &hs) const noexcept {
    return hs.hash();  // or expose hash_ via accessor
  }
};
}  // namespace std
