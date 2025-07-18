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

#include "query_plan_checker.hpp"

#include <iostream>
#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <tuple>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "query/exceptions.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/frontend/semantic/symbol_generator.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/plan/operator.hpp"
#include "query/plan/planner.hpp"

#include "query_common.hpp"
#include "utils/bound.hpp"

namespace memgraph::query {
::std::ostream &operator<<(::std::ostream &os, const Symbol &sym) {
  return os << "Symbol{\"" << sym.name() << "\" [" << sym.position() << "] " << Symbol::TypeToString(sym.type()) << "}";
}
}  // namespace memgraph::query

using namespace memgraph::query::plan;
using memgraph::query::AstStorage;
using memgraph::query::CypherUnion;
using memgraph::query::EdgeAtom;
using memgraph::query::SingleQuery;
using memgraph::query::Symbol;
using memgraph::query::SymbolTable;
using Type = memgraph::query::EdgeAtom::Type;
using Direction = memgraph::query::EdgeAtom::Direction;
using Bound = ScanAllByEdgeTypePropertyRange::Bound;
namespace ms = memgraph::storage;

namespace {

class Planner {
 public:
  template <class TDbAccessor>
  Planner(QueryParts query_parts, PlanningContext<TDbAccessor> context,
          const std::vector<memgraph::query::IndexHint> &index_hints) {
    memgraph::query::Parameters parameters;
    PostProcessor post_processor(parameters, index_hints, context.db);
    plan_ = MakeLogicalPlanForSingleQuery<RuleBasedPlanner>(query_parts, &context);
    plan_ = post_processor.Rewrite(std::move(plan_), &context);
  }

  auto &plan() { return *plan_; }

 private:
  std::unique_ptr<LogicalOperator> plan_;
};

template <class... TChecker>
auto CheckPlan(LogicalOperator &plan, const SymbolTable &symbol_table, TChecker... checker) {
  std::list<BaseOpChecker *> checkers{&checker...};
  PlanChecker plan_checker(checkers, symbol_table);
  plan.Accept(plan_checker);
  EXPECT_TRUE(plan_checker.checkers_.empty());
}

template <class TPlanner, class... TChecker>
auto CheckPlan(memgraph::query::CypherQuery *query, AstStorage &storage, TChecker... checker) {
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TPlanner>(&dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, checker...);
}

template <class T>
class TestPlanner : public ::testing::Test {
 public:
  AstStorage storage;
};

using PlannerTypes = ::testing::Types<Planner>;

void DeleteListContent(std::list<BaseOpChecker *> *list) {
  for (BaseOpChecker *ptr : *list) {
    delete ptr;
  }
}
TYPED_TEST_SUITE(TestPlanner, PlannerTypes);

TYPED_TEST(TestPlanner, MatchNodeReturn) {
  // Test MATCH (n) RETURN n
  FakeDbAccessor dba;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN(as_n)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateNodeReturn) {
  // Test CREATE (n) RETURN n AS n
  FakeDbAccessor dba;
  auto ident_n = IDENT("n");
  auto query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), RETURN(ident_n, AS("n"))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateExpand) {
  // Test CREATE (n) -[r :rel1]-> (m)
  auto relationship = "relationship";
  auto *query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"), EDGE("r", Direction::OUT, {relationship}), NODE("m")))));
  CheckPlan<TypeParam>(query, this->storage, ExpectCreateNode(), ExpectCreateExpand(), ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, CreateMultipleNode) {
  // Test CREATE (n), (m)
  auto *query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n")), PATTERN(NODE("m")))));
  CheckPlan<TypeParam>(query, this->storage, ExpectCreateNode(), ExpectCreateNode(), ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, CreateNodeExpandNode) {
  // Test CREATE (n) -[r :rel]-> (m), (l)
  auto relationship = "rel";
  auto *query = QUERY(SINGLE_QUERY(
      CREATE(PATTERN(NODE("n"), EDGE("r", Direction::OUT, {relationship}), NODE("m")), PATTERN(NODE("l")))));
  CheckPlan<TypeParam>(query, this->storage, ExpectCreateNode(), ExpectCreateExpand(), ExpectCreateNode(),
                       ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, CreateNamedPattern) {
  // Test CREATE p = (n) -[r :rel]-> (m)
  auto relationship = "rel";
  auto *query =
      QUERY(SINGLE_QUERY(CREATE(NAMED_PATTERN("p", NODE("n"), EDGE("r", Direction::OUT, {relationship}), NODE("m")))));
  CheckPlan<TypeParam>(query, this->storage, ExpectCreateNode(), ExpectCreateExpand(), ExpectConstructNamedPath(),
                       ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, MatchCreateExpand) {
  // Test MATCH (n) CREATE (n) -[r :rel1]-> (m)
  auto relationship = "relationship";
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                                   CREATE(PATTERN(NODE("n"), EDGE("r", Direction::OUT, {relationship}), NODE("m")))));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectCreateExpand(), ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, MatchLabeledNodes) {
  // Test MATCH (n :label) RETURN n
  FakeDbAccessor dba;
  auto label = "label";
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label))), RETURN(as_n)));
  {
    // Without created label index
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
  }
  {
    // With created label index
    dba.SetIndexCount(dba.Label(label), 0);
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByLabel(), ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, MatchPathReturn) {
  // Test MATCH (n) -[r :relationship]- (m) RETURN n
  FakeDbAccessor dba;
  auto relationship = "relationship";
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r", Direction::BOTH, {relationship}), NODE("m"))), RETURN(as_n)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchNamedPatternReturn) {
  // Test MATCH p = (n) -[r :relationship]- (m) RETURN p
  FakeDbAccessor dba;
  auto relationship = "relationship";
  auto *as_p = NEXPR("p", IDENT("p"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(NAMED_PATTERN("p", NODE("n"), EDGE("r", Direction::BOTH, {relationship}), NODE("m"))), RETURN(as_p)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectConstructNamedPath(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchNamedPatternWithPredicateReturn) {
  // Test MATCH p = (n) -[r :relationship]- (m) WHERE 2 = p RETURN p
  FakeDbAccessor dba;
  auto relationship = "relationship";
  auto *as_p = NEXPR("p", IDENT("p"));
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(NAMED_PATTERN("p", NODE("n"), EDGE("r", Direction::BOTH, {relationship}), NODE("m"))),
                         WHERE(EQ(LITERAL(2), IDENT("p"))), RETURN(as_p)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectConstructNamedPath(), ExpectFilter(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, OptionalMatchNamedPatternReturn) {
  // Test OPTIONAL MATCH p = (n) -[r]- (m) RETURN p
  FakeDbAccessor dba;
  auto node_n = NODE("n");
  auto edge = EDGE("r");
  auto node_m = NODE("m");
  auto pattern = NAMED_PATTERN("p", node_n, edge, node_m);
  auto as_p = AS("p");
  auto *query = QUERY(SINGLE_QUERY(OPTIONAL_MATCH(pattern), RETURN("p", as_p)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto get_symbol = [&symbol_table](const auto *ast_node) { return symbol_table.at(*ast_node->identifier_); };
  std::vector<Symbol> optional_symbols{get_symbol(pattern), get_symbol(node_n), get_symbol(edge), get_symbol(node_m)};
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  std::list<BaseOpChecker *> optional{new ExpectScanAll(), new ExpectExpand(), new ExpectConstructNamedPath()};
  CheckPlan(planner.plan(), symbol_table, ExpectOptional(optional_symbols, optional), ExpectProduce());
  DeleteListContent(&optional);
}

TYPED_TEST(TestPlanner, MatchWhereReturn) {
  // Test MATCH (n) WHERE n.property < 42 RETURN n
  FakeDbAccessor dba;
  auto property = dba.Property("property");
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                                   WHERE(LESS(PROPERTY_LOOKUP(dba, "n", property), LITERAL(42))), RETURN(as_n)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchDelete) {
  // Test MATCH (n) DELETE n
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), DELETE(IDENT("n"))));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectDelete(), ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, MatchNodeSet) {
  // Test MATCH (n) SET n.prop = 42, n = n, n :label
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto label = "label";
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), SET(PROPERTY_LOOKUP(dba, "n", prop), LITERAL(42)),
                                   SET("n", IDENT("n")), SET("n", {label})));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectSetProperty(), ExpectSetProperties(),
                       ExpectSetLabels(), ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, MatchRemove) {
  // Test MATCH (n) REMOVE n.prop REMOVE n :label
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto label = "label";
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), REMOVE(PROPERTY_LOOKUP(dba, "n", prop)), REMOVE("n", {label})));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectRemoveProperty(), ExpectRemoveLabels(),
                       ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, MatchMultiPattern) {
  // Test MATCH (n) -[r]- (m), (j) -[e]- (i) RETURN n
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m")), PATTERN(NODE("j"), EDGE("e"), NODE("i"))), RETURN("n")));
  // We expect the expansions after the first to have a uniqueness filter in a
  // single MATCH clause.
  std::list<BaseOpChecker *> left_cartesian_ops{new ExpectScanAll(), new ExpectExpand()};
  std::list<BaseOpChecker *> right_cartesian_ops{new ExpectScanAll(), new ExpectExpand()};

  CheckPlan<TypeParam>(query, this->storage, ExpectCartesian(left_cartesian_ops, right_cartesian_ops),
                       ExpectEdgeUniquenessFilter(), ExpectProduce());

  DeleteListContent(&left_cartesian_ops);
  DeleteListContent(&right_cartesian_ops);
}

TYPED_TEST(TestPlanner, MatchMultiPatternWithHashJoin) {
  // Test MATCH (a:label)-[r1]->(b), (c:label)-[r2]->(d) WHERE c.id = a.id return a, b, c, d;
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto property = PROPERTY_PAIR(dba, "id");

  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("a", label_name), EDGE("r1"), NODE("b")),
                         PATTERN(NODE("c", label_name), EDGE("r2"), NODE("d"))),
                   WHERE(EQ(PROPERTY_LOOKUP(dba, "c", property.second), PROPERTY_LOOKUP(dba, "a", property.second))),
                   RETURN("a", "b", "c", "d")));

  std::list<BaseOpChecker *> left_indexed_join_ops{new ExpectScanAll(), new ExpectFilter(), new ExpectExpand()};
  std::list<BaseOpChecker *> right_indexed_join_ops{new ExpectScanAll(), new ExpectFilter(), new ExpectExpand()};

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectHashJoin(left_indexed_join_ops, right_indexed_join_ops),
            ExpectEdgeUniquenessFilter(), ExpectProduce());

  DeleteListContent(&left_indexed_join_ops);
  DeleteListContent(&right_indexed_join_ops);
}

TYPED_TEST(TestPlanner, MatchMultiPatternWithAsymmetricHashJoin) {
  // Test MATCH (a:label)-[r1]->(b), (c:label)-[r2]->(d) WHERE c.id = a.id2 return a, b, c, d;
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto property_1 = PROPERTY_PAIR(dba, "id");
  const auto property_2 = PROPERTY_PAIR(dba, "id2");

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("a", label_name), EDGE("r1"), NODE("b")),
            PATTERN(NODE("c", label_name), EDGE("r2"), NODE("d"))),
      WHERE(EQ(PROPERTY_LOOKUP(dba, "c", property_1.second), PROPERTY_LOOKUP(dba, "a", property_2.second))),
      RETURN("a", "b", "c", "d")));

  std::list<BaseOpChecker *> left_indexed_join_ops{new ExpectScanAll(), new ExpectFilter(), new ExpectExpand()};
  std::list<BaseOpChecker *> right_indexed_join_ops{new ExpectScanAll(), new ExpectFilter(), new ExpectExpand()};

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectHashJoin(left_indexed_join_ops, right_indexed_join_ops),
            ExpectEdgeUniquenessFilter(), ExpectProduce());

  DeleteListContent(&left_indexed_join_ops);
  DeleteListContent(&right_indexed_join_ops);
}

TYPED_TEST(TestPlanner, MatchMultiPatternWithIndexJoin) {
  // Test MATCH (a:label)-[r1]->(b), (c:label)-[r2]->(d) WHERE c.id = a.id return a, b, c, d;
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "id");
  dba.SetIndexCount(label, 1);
  dba.SetIndexCount(label, property.second, 1);

  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("a", label_name), EDGE("r1"), NODE("b")),
                         PATTERN(NODE("c", label_name), EDGE("r2"), NODE("d"))),
                   WHERE(EQ(PROPERTY_LOOKUP(dba, "c", property.second), PROPERTY_LOOKUP(dba, "a", property.second))),
                   RETURN("a", "b", "c", "d")));

  auto c_prop = PROPERTY_LOOKUP(dba, "c", property);
  std::list<BaseOpChecker *> left_indexed_join_ops{new ExpectScanAllByLabel(), new ExpectExpand()};
  std::list<BaseOpChecker *> right_indexed_join_ops{
      new ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{property.second}},
                                         {ExpressionRange::Equal(c_prop)}),
      new ExpectExpand()};

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectIndexedJoin(left_indexed_join_ops, right_indexed_join_ops),
            ExpectEdgeUniquenessFilter(), ExpectProduce());

  DeleteListContent(&left_indexed_join_ops);
  DeleteListContent(&right_indexed_join_ops);
}

TYPED_TEST(TestPlanner, MatchMultiPatternSameStart) {
  // Test MATCH (n), (n) -[e]- (m) RETURN n
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n")), PATTERN(NODE("n"), EDGE("e"), NODE("m"))), RETURN("n")));
  // We expect the second pattern to generate only an Expand, since another
  // ScanAll would be redundant.
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectExpand(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchMultiPatternSameExpandStart) {
  // Test MATCH (n) -[r]- (m), (m) -[e]- (l) RETURN n
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m")), PATTERN(NODE("m"), EDGE("e"), NODE("l"))), RETURN("n")));
  // We expect the second pattern to generate only an Expand. Another
  // ScanAll would be redundant, as it would generate the nodes obtained from
  // expansion. Additionally, a uniqueness filter is expected.
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectExpand(), ExpectExpand(),
                       ExpectEdgeUniquenessFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MultiMatch) {
  // Test MATCH (n) -[r]- (m) MATCH (j) -[e]- (i) -[f]- (h) RETURN n
  FakeDbAccessor dba;
  auto *node_n = NODE("n");
  auto *edge_r = EDGE("r");
  auto *node_m = NODE("m");
  auto *node_j = NODE("j");
  auto *edge_e = EDGE("e");
  auto *node_i = NODE("i");
  auto *edge_f = EDGE("f");
  auto *node_h = NODE("h");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(node_n, edge_r, node_m)),
                                   MATCH(PATTERN(node_j, edge_e, node_i, edge_f, node_h)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  // Multiple MATCH clauses form a Cartesian product, so the uniqueness should
  // not cross MATCH boundaries.
  std::list<BaseOpChecker *> left_cartesian_ops{new ExpectScanAll(), new ExpectExpand()};
  std::list<BaseOpChecker *> right_cartesian_ops{new ExpectScanAll(), new ExpectExpand(), new ExpectExpand(),
                                                 new ExpectEdgeUniquenessFilter()};
  CheckPlan(planner.plan(), symbol_table, ExpectCartesian(left_cartesian_ops, right_cartesian_ops), ExpectProduce());

  DeleteListContent(&left_cartesian_ops);
  DeleteListContent(&right_cartesian_ops);
}

TYPED_TEST(TestPlanner, MultiMatchSameStart) {
  // Test MATCH (n) MATCH (n) -[r]- (m) RETURN n
  FakeDbAccessor dba;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))), RETURN(as_n)));
  // Similar to MatchMultiPatternSameStart, we expect only Expand from second
  // MATCH clause.
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWithReturn) {
  // Test MATCH (old) WITH old AS new RETURN new
  FakeDbAccessor dba;
  auto *as_new = NEXPR("new", IDENT("new"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("old"))), WITH("old", AS("new")), RETURN(as_new)));
  // No accumulation since we only do reads.
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWithWhereReturn) {
  // Test MATCH (old) WITH old AS new WHERE new.prop < 42 RETURN new
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto *as_new = NEXPR("new", IDENT("new"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("old"))), WITH("old", AS("new")),
                                   WHERE(LESS(PROPERTY_LOOKUP(dba, "new", prop), LITERAL(42))), RETURN(as_new)));
  // No accumulation since we only do reads.
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateMultiExpand) {
  // Test CREATE (n) -[r :r]-> (m), (n) - [p :p]-> (l)
  auto r = "r";
  auto p = "p";
  auto *query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"), EDGE("r", Direction::OUT, {r}), NODE("m")),
                                          PATTERN(NODE("n"), EDGE("p", Direction::OUT, {p}), NODE("l")))));
  CheckPlan<TypeParam>(query, this->storage, ExpectCreateNode(), ExpectCreateExpand(), ExpectCreateExpand(),
                       ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, MatchWithSumWhereReturn) {
  // Test MATCH (n) WITH SUM(n.prop) + 42 AS sum WHERE sum < 42
  //      RETURN sum AS result
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto sum = SUM(PROPERTY_LOOKUP(dba, "n", prop), false);
  auto literal = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WITH(ADD(sum, literal), AS("sum")),
                                   WHERE(LESS(IDENT("sum"), LITERAL(42))), RETURN("sum", AS("result"))));
  auto aggr = ExpectAggregate({sum}, {literal});
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), aggr, ExpectProduce(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchReturnSum) {
  // Test MATCH (n) RETURN SUM(n.prop1) AS sum, n.prop2 AS group
  FakeDbAccessor dba;
  auto prop1 = dba.Property("prop1");
  auto prop2 = dba.Property("prop2");
  auto sum = SUM(PROPERTY_LOOKUP(dba, "n", prop1), false);
  auto n_prop2 = PROPERTY_LOOKUP(dba, "n", prop2);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN(sum, AS("sum"), n_prop2, AS("group"))));
  auto aggr = ExpectAggregate({sum}, {n_prop2});
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateWithSum) {
  // Test CREATE (n) WITH SUM(n.prop) AS sum
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto ident_n = IDENT("n");
  auto n_prop = PROPERTY_LOOKUP(dba, ident_n, prop);
  auto sum = SUM(n_prop, false);
  auto query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), WITH(sum, AS("sum"))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  // We expect both the accumulation and aggregation because the part before
  // WITH updates the database.
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWithSumWithDistinctWhereReturn) {
  // Test MATCH (n) WITH SUM(DISTINCT n.prop) + 42 AS sum WHERE sum < 42
  //      RETURN sum AS result
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto sum = SUM(PROPERTY_LOOKUP(dba, "n", prop), true);
  auto literal = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WITH(ADD(sum, literal), AS("sum")),
                                   WHERE(LESS(IDENT("sum"), LITERAL(42))), RETURN("sum", AS("result"))));
  auto aggr = ExpectAggregate({sum}, {literal});
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), aggr, ExpectProduce(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchReturnSumWithDistinct) {
  // Test MATCH (n) RETURN SUM(DISTINCT n.prop1) AS sum, n.prop2 AS group
  FakeDbAccessor dba;
  auto prop1 = dba.Property("prop1");
  auto prop2 = dba.Property("prop2");
  auto sum = SUM(PROPERTY_LOOKUP(dba, "n", prop1), true);
  auto n_prop2 = PROPERTY_LOOKUP(dba, "n", prop2);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN(sum, AS("sum"), n_prop2, AS("group"))));
  auto aggr = ExpectAggregate({sum}, {n_prop2});
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateWithSumWithDistinct) {
  // Test CREATE (n) WITH SUM(DISTINCT n.prop) AS sum
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto ident_n = IDENT("n");
  auto n_prop = PROPERTY_LOOKUP(dba, ident_n, prop);
  auto sum = SUM(n_prop, true);
  auto query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), WITH(sum, AS("sum"))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  // We expect both the accumulation and aggregation because the part before
  // WITH updates the database.
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWithCreate) {
  // Test MATCH (n) WITH n AS a CREATE (a) -[r :r]-> (b)
  auto r_type = "r";
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WITH("n", AS("a")),
                                   CREATE(PATTERN(NODE("a"), EDGE("r", Direction::OUT, {r_type}), NODE("b")))));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectProduce(), ExpectCreateExpand(),
                       ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, MatchReturnSkipLimit) {
  // Test MATCH (n) RETURN n SKIP 2 LIMIT 1
  FakeDbAccessor dba;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN(as_n, SKIP(LITERAL(2)), LIMIT(LITERAL(1)))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(), ExpectSkip(), ExpectLimit());
}

TYPED_TEST(TestPlanner, CreateWithSkipReturnLimit) {
  // Test CREATE (n) WITH n AS m SKIP 2 RETURN m LIMIT 1
  FakeDbAccessor dba;
  auto ident_n = IDENT("n");
  auto query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), WITH(ident_n, AS("m"), SKIP(LITERAL(2))),
                                  RETURN("m", LIMIT(LITERAL(1)))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  // Since we have a write query, we need to have Accumulate. This is a bit
  // different than Neo4j 3.0, which optimizes WITH followed by RETURN as a
  // single RETURN clause and then moves Skip and Limit before Accumulate.
  // This causes different behaviour. A newer version of Neo4j does the same
  // thing as us here (but who knows if they change it again).
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, ExpectProduce(), ExpectSkip(), ExpectProduce(),
            ExpectLimit());
}

TYPED_TEST(TestPlanner, CreateReturnSumSkipLimit) {
  // Test CREATE (n) RETURN SUM(n.prop) AS s SKIP 2 LIMIT 1
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto ident_n = IDENT("n");
  auto n_prop = PROPERTY_LOOKUP(dba, ident_n, prop);
  auto sum = SUM(n_prop, false);
  auto query =
      QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), RETURN(sum, AS("s"), SKIP(LITERAL(2)), LIMIT(LITERAL(1)))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr, ExpectProduce(), ExpectSkip(), ExpectLimit());
}

TYPED_TEST(TestPlanner, CreateReturnSumWithDistinctSkipLimit) {
  // Test CREATE (n) RETURN SUM(n.prop) AS s SKIP 2 LIMIT 1
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto ident_n = IDENT("n");
  auto n_prop = PROPERTY_LOOKUP(dba, ident_n, prop);
  auto sum = SUM(n_prop, true);
  auto query =
      QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), RETURN(sum, AS("s"), SKIP(LITERAL(2)), LIMIT(LITERAL(1)))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr, ExpectProduce(), ExpectSkip(), ExpectLimit());
}

TYPED_TEST(TestPlanner, MatchReturnOrderBy) {
  // Test MATCH (n) RETURN n AS m ORDER BY n.prop
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto *as_m = NEXPR("m", IDENT("n"));
  auto *node_n = NODE("n");
  auto ret = RETURN(as_m, ORDER_BY(PROPERTY_LOOKUP(dba, "n", prop)));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(node_n)), ret));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(), ExpectOrderBy());
}

TYPED_TEST(TestPlanner, CreateWithOrderByWhere) {
  // Test CREATE (n) -[r :r]-> (m)
  //      WITH n AS new ORDER BY new.prop, r.prop WHERE m.prop < 42
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto r_type = "r";
  auto ident_n = IDENT("n");
  auto ident_r = IDENT("r");
  auto ident_m = IDENT("m");
  auto new_prop = PROPERTY_LOOKUP(dba, "new", prop);
  auto r_prop = PROPERTY_LOOKUP(dba, ident_r, prop);
  auto m_prop = PROPERTY_LOOKUP(dba, ident_m, prop);
  auto query =
      QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"), EDGE("r", Direction::OUT, {r_type}), NODE("m"))),
                         WITH(ident_n, AS("new"), ORDER_BY(new_prop, r_prop)), WHERE(LESS(m_prop, LITERAL(42)))));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  // Since this is a write query, we expect to accumulate to old used symbols.
  auto acc = ExpectAccumulate({
      symbol_table.at(*ident_n),  // `n` in WITH
      symbol_table.at(*ident_r),  // `r` in ORDER BY
      symbol_table.at(*ident_m),  // `m` in WHERE
  });
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), ExpectCreateExpand(), acc, ExpectProduce(),
            ExpectOrderBy(), ExpectFilter(), ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, ReturnAddSumCountOrderBy) {
  // Test RETURN SUM(1) + COUNT(2) AS result ORDER BY result
  auto sum = SUM(LITERAL(1), false);
  auto count = COUNT(LITERAL(2), false);
  auto *query = QUERY(SINGLE_QUERY(RETURN(ADD(sum, count), AS("result"), ORDER_BY(IDENT("result")))));
  auto aggr = ExpectAggregate({sum, count}, {});
  CheckPlan<TypeParam>(query, this->storage, aggr, ExpectProduce(), ExpectOrderBy());
}

TYPED_TEST(TestPlanner, ReturnAddSumCountWithDistinctOrderBy) {
  // Test RETURN SUM(1) + COUNT(2) AS result ORDER BY result
  auto sum = SUM(LITERAL(1), true);
  auto count = COUNT(LITERAL(2), true);
  auto *query = QUERY(SINGLE_QUERY(RETURN(ADD(sum, count), AS("result"), ORDER_BY(IDENT("result")))));
  auto aggr = ExpectAggregate({sum, count}, {});
  CheckPlan<TypeParam>(query, this->storage, aggr, ExpectProduce(), ExpectOrderBy());
}

TYPED_TEST(TestPlanner, MatchMerge) {
  // Test MATCH (n) MERGE (n) -[r :r]- (m)
  //      ON MATCH SET n.prop = 42 ON CREATE SET m = n
  //      RETURN n AS n
  FakeDbAccessor dba;
  auto r_type = "r";
  auto prop = dba.Property("prop");
  auto ident_n = IDENT("n");
  auto query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                   MERGE(PATTERN(NODE("n"), EDGE("r", Direction::BOTH, {r_type}), NODE("m")),
                         ON_MATCH(SET(PROPERTY_LOOKUP(dba, "n", prop), LITERAL(42))), ON_CREATE(SET("m", IDENT("n")))),
                   RETURN(ident_n, AS("n"))));
  std::list<BaseOpChecker *> on_match{new ExpectExpand(), new ExpectSetProperty()};
  std::list<BaseOpChecker *> on_create{new ExpectCreateExpand(), new ExpectSetProperties()};
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  // We expect Accumulate after Merge, because it is considered as a write.
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectMerge(on_match, on_create), acc, ExpectProduce());
  DeleteListContent(&on_match);
  DeleteListContent(&on_create);
}

TYPED_TEST(TestPlanner, MatchOptionalMatchWhereReturn) {
  // Test MATCH (n) OPTIONAL MATCH (n) -[r]- (m) WHERE m.prop < 42 RETURN r
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), OPTIONAL_MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                                   WHERE(LESS(PROPERTY_LOOKUP(dba, "m", prop), LITERAL(42))), RETURN("r")));
  std::list<BaseOpChecker *> optional{new ExpectScanAll(), new ExpectExpand(), new ExpectFilter()};
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectOptional(optional), ExpectProduce());
  DeleteListContent(&optional);
}

TYPED_TEST(TestPlanner, MatchOptionalMatchNodePropertyWithIndex) {
  // Test MATCH (n:Label) OPTIONAL MATCH (m:Label) WHERE n.prop = m.prop RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, property.second, 0);

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n", label_name))), OPTIONAL_MATCH(PATTERN(NODE("m", label_name))),
      WHERE(EQ(PROPERTY_LOOKUP(dba, "n", property.second), PROPERTY_LOOKUP(dba, "m", property.second))), RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  auto m_prop = PROPERTY_LOOKUP(dba, "m", property);
  std::list<BaseOpChecker *> optional{new ExpectScanAllByLabelProperties(
      label, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(m_prop)})};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectOptional(optional), ExpectProduce());
  DeleteListContent(&optional);
}

TYPED_TEST(TestPlanner, MatchUnwindReturn) {
  // Test MATCH (n) UNWIND [1,2,3] AS x RETURN n, x
  FakeDbAccessor dba;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *as_x = NEXPR("x", IDENT("x"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), AS("x")),
                                   RETURN(as_n, as_x)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectUnwind(), ExpectProduce());
}

TYPED_TEST(TestPlanner, ReturnDistinctOrderBySkipLimit) {
  // Test RETURN DISTINCT 1 ORDER BY 1 SKIP 1 LIMIT 1
  auto *query = QUERY(
      SINGLE_QUERY(RETURN_DISTINCT(LITERAL(1), AS("1"), ORDER_BY(LITERAL(1)), SKIP(LITERAL(1)), LIMIT(LITERAL(1)))));
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), ExpectDistinct(), ExpectOrderBy(), ExpectSkip(),
                       ExpectLimit());
}

TYPED_TEST(TestPlanner, CreateWithDistinctSumWhereReturn) {
  // Test CREATE (n) WITH DISTINCT SUM(n.prop) AS s WHERE s < 42 RETURN s
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto node_n = NODE("n");
  auto sum = SUM(PROPERTY_LOOKUP(dba, "n", prop), false);
  auto query = QUERY(SINGLE_QUERY(CREATE(PATTERN(node_n)), WITH_DISTINCT(sum, AS("s")),
                                  WHERE(LESS(IDENT("s"), LITERAL(42))), RETURN("s")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto acc = ExpectAccumulate({symbol_table.at(*node_n->identifier_)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr, ExpectProduce(), ExpectDistinct(),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchCrossReferenceVariable) {
  // Test MATCH (n {prop: m.prop}), (m {prop: n.prop}) RETURN n
  FakeDbAccessor dba;
  auto prop = PROPERTY_PAIR(dba, "prop");
  auto node_n = NODE("n");
  auto m_prop = PROPERTY_LOOKUP(dba, "m", prop.second);
  std::get<0>(node_n->properties_)[this->storage.GetPropertyIx(prop.first)] = m_prop;
  auto node_m = NODE("m");
  auto n_prop = PROPERTY_LOOKUP(dba, "n", prop.second);
  std::get<0>(node_m->properties_)[this->storage.GetPropertyIx(prop.first)] = n_prop;
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(node_n), PATTERN(node_m)), RETURN("n")));

  // We expect both ScanAll to come before filters (2 are joined into one),
  // because they need to populate the symbol values.
  // They are combined in a Cartesian (rewritten to HashJoin) to generate values from both symbols respectively and
  // independently
  std::list<BaseOpChecker *> left_hash_join_ops{new ExpectScanAll()};
  std::list<BaseOpChecker *> right_hash_join_ops{new ExpectScanAll()};

  CheckPlan<TypeParam>(query, this->storage, ExpectHashJoin(left_hash_join_ops, right_hash_join_ops), ExpectFilter(),
                       ExpectProduce());

  DeleteListContent(&left_hash_join_ops);
  DeleteListContent(&right_hash_join_ops);
}

TYPED_TEST(TestPlanner, MatchWhereBeforeExpand) {
  // Test MATCH (n) -[r]- (m) WHERE n.prop < 42 RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                                   WHERE(LESS(PROPERTY_LOOKUP(dba, "n", prop), LITERAL(42))), RETURN(as_n)));
  // We expect Filter to come immediately after ScanAll, since it only uses `n`.
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectExpand(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchEdgeTypeIndex) {
  FakeDbAccessor dba;
  auto indexed_edge_type = dba.EdgeType("indexed_edgetype");
  dba.SetIndexCount(indexed_edge_type, 1);
  {
    // Test MATCH ()-[r:indexed_edgetype]->() RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("anon2"))),
        RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeType(), ExpectProduce());
  }
  {
    // Test MATCH (a)-[r:indexed_edgetype]->() RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("a"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("anon2"))),
        RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeType(), ExpectProduce());
  }
  {
    // Test MATCH ()-[r:indexed_edgetype]->(b) RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("b"))),
        RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeType(), ExpectProduce());
  }
  {
    // Test MATCH (a)-[r:indexed_edgetype]->(b) RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(
            PATTERN(NODE("a"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}), NODE("b"))),
        RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeType(), ExpectProduce());
  }
  {
    // Test MATCH ()-[r:not_indexed_edgetype]->() RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"not_indexed_edgetype"}),
                      NODE("anon2"))),
        RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectProduce());
  }
  {
    // Test MATCH p=()-[r:indexed_edgetype]->() RETURN p;
    auto *as_p = NEXPR("p", IDENT("p"));
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(NAMED_PATTERN("p", NODE("anon1"),
                            EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}), NODE("anon2"))),
        RETURN(as_p)));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeType(), ExpectConstructNamedPath(), ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, MatchEdgeTypePropertyIndexExistence) {
  FakeDbAccessor dba;
  auto edge_type = dba.EdgeType("indexed_edgetype");
  auto prop = dba.Property("indexed_property");
  auto prop_pair = PROPERTY_PAIR(dba, "indexed_property");

  dba.SetIndexCount(edge_type, prop, 1);
  {
    // Test MATCH ()-[r:indexed_edgetype]->() WHERE r.prop IS NOT NULL RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("anon2"))),
        WHERE(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "r", prop)))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypeProperty(edge_type, prop_pair), ExpectProduce());
  }
  {
    // Test MATCH (a)-[r:indexed_edgetype]->() WHERE r.prop IS NOT NULL RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("a"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("anon2"))),
        WHERE(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "r", prop)))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypeProperty(edge_type, prop_pair), ExpectProduce());
  }
  {
    // Test MATCH ()-[r:indexed_edgetype]->(b) WHERE r.prop IS NOT NULL RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("b"))),
        WHERE(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "r", prop)))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypeProperty(edge_type, prop_pair), ExpectProduce());
  }
  {
    // Test MATCH (a)-[r:indexed_edgetype]->(b) WHERE r.prop IS NOT NULL RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(
            PATTERN(NODE("a"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}), NODE("b"))),
        WHERE(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "r", prop)))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypeProperty(edge_type, prop_pair), ExpectProduce());
  }
  {
    // Test MATCH ()-[r:not_indexed_edgetype]->() WHERE r.prop IS NOT NULL RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"not_indexed_edgetype"}),
                      NODE("anon2"))),
        WHERE(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "r", prop)))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, MatchEdgeTypePropertyIndexPointLookup) {
  FakeDbAccessor dba;
  auto edge_type = dba.EdgeType("indexed_edgetype");
  auto prop = dba.Property("indexed_property");
  const auto property_pair = PROPERTY_PAIR(dba, "indexed_property");
  dba.SetIndexCount(edge_type, prop, 1);
  auto lit_1 = LITERAL(1);
  {
    // Test MATCH ()-[r:indexed_edgetype]->() WHERE r.prop=1 RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("anon2"))),
        WHERE(EQ(PROPERTY_LOOKUP(dba, "r", prop), LITERAL(1))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypePropertyValue(edge_type, property_pair, lit_1),
              ExpectProduce());
  }
  {
    // Test MATCH (a)-[r:indexed_edgetype]->() WHERE r.prop=1 RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("a"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("anon2"))),
        WHERE(EQ(PROPERTY_LOOKUP(dba, "r", prop), LITERAL(1))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypePropertyValue(edge_type, property_pair, lit_1),
              ExpectProduce());
  }
  {
    // Test MATCH ()-[r:indexed_edgetype]->(b) WHERE r.prop=1 RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}),
                      NODE("b"))),
        WHERE(EQ(PROPERTY_LOOKUP(dba, "r", prop), LITERAL(1))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypePropertyValue(edge_type, property_pair, lit_1),
              ExpectProduce());
  }
  {
    // Test MATCH (a)-[r:indexed_edgetype]->(b) WHERE r.prop=1 RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(
            PATTERN(NODE("a"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}), NODE("b"))),
        WHERE(EQ(PROPERTY_LOOKUP(dba, "r", prop), LITERAL(1))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypePropertyValue(edge_type, property_pair, lit_1),
              ExpectProduce());
  }
  {
    // Test MATCH ()-[r:not_indexed_edgetype]->() WHERE r.prop=1 RETURN r;
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("anon1"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"not_indexed_edgetype"}),
                      NODE("anon2"))),
        WHERE(EQ(PROPERTY_LOOKUP(dba, "r", prop), LITERAL(1))), RETURN("r")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
  }
  {
    // Test MATCH p=()-[r:indexed_edgetype]->() WHERE r.prop=1 RETURN p;
    auto *as_p = NEXPR("p", IDENT("p"));
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(NAMED_PATTERN("p", NODE("anon1"),
                            EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {"indexed_edgetype"}), NODE("anon2"))),
        WHERE(EQ(PROPERTY_LOOKUP(dba, "r", prop), LITERAL(1))), RETURN(as_p)));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByEdgeTypePropertyValue(edge_type, property_pair, lit_1),
              ExpectConstructNamedPath(), ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, EdgeRangeFilterNoIndex1) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 1 < r.prop < 10 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(LESS(LITERAL(1), PROPERTY_LOOKUP(dba, "r", property.second)),
                LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterWIndex1) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 1 < r.prop < 10 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto edge_type = dba.EdgeType(edge_type_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(edge_type, property.second, 1);

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(LESS(LITERAL(1), PROPERTY_LOOKUP(dba, "r", property.second)),
                LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByEdgeTypePropertyRange(edge_type, property.second,
                                                 Bound{LITERAL(1), memgraph::utils::BoundType::EXCLUSIVE},
                                                 Bound{PARAMETER_LOOKUP(2), memgraph::utils::BoundType::EXCLUSIVE}),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterNoIndex2) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 >= r.prop > 1 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                GREATER(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterWIndex2) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 >= r.prop > 1 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto edge_type = dba.EdgeType(edge_type_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(edge_type, property.second, 1);

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                GREATER(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByEdgeTypePropertyRange(edge_type, property.second,
                                                 Bound{PARAMETER_LOOKUP(2), memgraph::utils::BoundType::EXCLUSIVE},
                                                 Bound{LITERAL(10), memgraph::utils::BoundType::INCLUSIVE}),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterNoIndex3) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 >= r.prop > 1 AND r.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                AND(GREATER(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)),
                    LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(3))))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterWIndex3) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 >= r.prop > 1 AND r.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto edge_type = dba.EdgeType(edge_type_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(edge_type, property.second, 1);

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                AND(GREATER(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)),
                    LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(3))))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByEdgeTypePropertyRange(edge_type, property.second,
                                                 Bound{PARAMETER_LOOKUP(2), memgraph::utils::BoundType::EXCLUSIVE},
                                                 Bound{LITERAL(10), memgraph::utils::BoundType::INCLUSIVE}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterNoIndex4) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 >= r.prop < 7 AND r.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                AND(LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)),
                    LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(3))))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterWIndex4) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 >= r.prop < 7 AND r.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto edge_type = dba.EdgeType(edge_type_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(edge_type, property.second, 1);

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                AND(LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)),
                    LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(3))))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByEdgeTypePropertyRange(edge_type, property.second, std::nullopt,
                                                 Bound{LITERAL(10), memgraph::utils::BoundType::INCLUSIVE}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterNoIndex5) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 > r.prop < 7 AND r.prop >= 5 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                AND(LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)),
                    GREATER_EQ(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(3))))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, EdgeRangeFilterWIndex5) {
  // Test MATCH (n)-[r:TYPE]->(m) WHERE 10 > r.prop < 7 AND r.prop >= 5 RETURN n
  FakeDbAccessor dba;
  const auto edge_type_name = "TYPE";
  const auto edge_type = dba.EdgeType(edge_type_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(edge_type, property.second, 1);

  auto *query = QUERY(SINGLE_QUERY(
      MATCH(
          PATTERN(NODE("n"), EDGE("r", memgraph::query::EdgeAtom::Direction::OUT, {edge_type_name}, false), NODE("m"))),
      WHERE(AND(GREATER(LITERAL(10), PROPERTY_LOOKUP(dba, "r", property.second)),
                AND(LESS(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(2)),
                    GREATER_EQ(PROPERTY_LOOKUP(dba, "r", property.second), PARAMETER_LOOKUP(3))))),
      RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByEdgeTypePropertyRange(edge_type, property.second,
                                                 Bound{PARAMETER_LOOKUP(3), memgraph::utils::BoundType::INCLUSIVE},
                                                 Bound{LITERAL(10), memgraph::utils::BoundType::EXCLUSIVE}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchFilterPropIsNotNull) {
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto prop = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, 1);
  dba.SetIndexCount(label, prop.second, 1);
  {
    // Test MATCH (n :label) -[r]- (m) WHERE n.prop IS NOT NULL RETURN n
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"), EDGE("r"), NODE("m"))),
                                     WHERE(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "n", prop)))), RETURN("n")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    // We expect ExpectScanAllByLabelProperties to come instead of ScanAll > Filter.
    CheckPlan(planner.plan(), symbol_table,
              ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{prop.second}},
                                             std::vector{ExpressionRange::IsNotNull()}),
              ExpectExpand(), ExpectProduce());
  }

  {
    // Test MATCH (n :label) -[r]- (m) WHERE n.prop IS NOT NULL OR true RETURN n
    auto *query =
        QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"), EDGE("r"), NODE("m"))),
                           WHERE(OR(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "n", prop))), LITERAL(true))), RETURN("n")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    // We expect ScanAllBy > Filter because of the "or true" condition.
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectExpand(), ExpectProduce());
  }

  {
    // Test MATCH (n :label) -[r]- (m)
    //      WHERE n.prop IS NOT NULL AND n.x = 2 RETURN n
    auto prop_x = PROPERTY_PAIR(dba, "x");
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("n", "label"), EDGE("r"), NODE("m"))),
        WHERE(AND(NOT(IS_NULL(PROPERTY_LOOKUP(dba, "n", prop))), EQ(PROPERTY_LOOKUP(dba, "n", prop_x), LITERAL(2)))),
        RETURN("n")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    // We expect ScanAllByLabelProperties > Filter
    // to come instead of ScanAll > Filter.
    CheckPlan(planner.plan(), symbol_table,
              ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{prop.second}},
                                             std::vector{ExpressionRange::IsNotNull()}),
              ExpectFilter(), ExpectExpand(), ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, MatchFilterWhere) {
  // Test MATCH (n)-[r]-(m) WHERE exists((n)-[]-()) and n!=n and 7!=8 RETURN n
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
      WHERE(AND(EXISTS(PATTERN(NODE("n"), EDGE("edge2", memgraph::query::EdgeAtom::Direction::BOTH, {}, false),
                               NODE("node3", std::nullopt, false))),
                AND(NEQ(IDENT("n"), IDENT("n")), NEQ(LITERAL(7), LITERAL(8))))),
      RETURN("n")));

  std::list<BaseOpChecker *> pattern_filter{new ExpectScanAll(), new ExpectExpand(), new ExpectLimit(),
                                            new ExpectEvaluatePatternFilter()};
  CheckPlan<TypeParam>(
      query, this->storage,
      ExpectFilter(),  // 7!=8
      ExpectScanAll(),
      ExpectFilter(std::vector<std::list<BaseOpChecker *>>{pattern_filter}),  // filter pulls from expand
      ExpectExpand(), ExpectProduce());
  DeleteListContent(&pattern_filter);
}

TYPED_TEST(TestPlanner, MultiMatchWhere) {
  // Test MATCH (n) -[r]- (m) MATCH (l) WHERE n.prop < 42 RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))), MATCH(PATTERN(NODE("l"))),
                                   WHERE(LESS(PROPERTY_LOOKUP(dba, "n", prop), LITERAL(42))), RETURN("n")));

  // The 2 match expansions need to be separated with a cartesian so they can be generated independently
  std::list<BaseOpChecker *> left_cartesian_ops{new ExpectScanAll(), new ExpectFilter(), new ExpectExpand()};
  std::list<BaseOpChecker *> right_cartesian_ops{new ExpectScanAll()};

  CheckPlan<TypeParam>(query, this->storage, ExpectCartesian(left_cartesian_ops, right_cartesian_ops), ExpectProduce());

  DeleteListContent(&left_cartesian_ops);
  DeleteListContent(&right_cartesian_ops);
}

TYPED_TEST(TestPlanner, MatchOptionalMatchWhere) {
  // Test MATCH (n) -[r]- (m) OPTIONAL MATCH (l) WHERE n.prop < 42 RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))), OPTIONAL_MATCH(PATTERN(NODE("l"))),
                                   WHERE(LESS(PROPERTY_LOOKUP(dba, "n", prop), LITERAL(42))), RETURN("n")));
  // Even though WHERE is in the second MATCH clause, and it uses the value from
  // first ScanAll, it must remain part of the Optional. It should come before
  // optional ScanAll.
  std::list<BaseOpChecker *> optional{new ExpectFilter(), new ExpectScanAll()};
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectExpand(), ExpectOptional(optional),
                       ExpectProduce());
  DeleteListContent(&optional);
}

TYPED_TEST(TestPlanner, MatchReturnAsterisk) {
  // Test MATCH (n) -[e]- (m) RETURN *, m.prop
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto ret = RETURN(PROPERTY_LOOKUP(dba, "m", prop), AS("m.prop"));
  ret->body_.all_identifiers = true;
  auto query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("e"), NODE("m"))), ret));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectProduce());
  std::vector<std::string> output_names;
  for (const auto &output_symbol : planner.plan().OutputSymbols(symbol_table)) {
    output_names.emplace_back(output_symbol.name());
  }
  std::vector<std::string> expected_names{"e", "m", "n", "m.prop"};
  EXPECT_EQ(output_names, expected_names);
}

TYPED_TEST(TestPlanner, MatchReturnAsteriskSum) {
  // Test MATCH (n) RETURN *, SUM(n.prop) AS s
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto sum = SUM(PROPERTY_LOOKUP(dba, "n", prop), false);
  auto ret = RETURN(sum, AS("s"));
  ret->body_.all_identifiers = true;
  auto query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), ret));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  auto *produce = dynamic_cast<Produce *>(&planner.plan());
  ASSERT_TRUE(produce);
  const auto &named_expressions = produce->named_expressions_;
  ASSERT_EQ(named_expressions.size(), 2);
  auto *expanded_ident = dynamic_cast<memgraph::query::Identifier *>(named_expressions[0]->expression_);
  ASSERT_TRUE(expanded_ident);
  auto aggr = ExpectAggregate({sum}, {expanded_ident});
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), aggr, ExpectProduce());
  std::vector<std::string> output_names;
  for (const auto &output_symbol : planner.plan().OutputSymbols(symbol_table)) {
    output_names.emplace_back(output_symbol.name());
  }
  std::vector<std::string> expected_names{"n", "s"};
  EXPECT_EQ(output_names, expected_names);
}

TYPED_TEST(TestPlanner, UnwindMergeNodeProperty) {
  // Test UNWIND [1] AS i MERGE (n {prop: i})
  auto node_n = NODE("n");
  std::get<0>(node_n->properties_)[this->storage.GetPropertyIx("prop")] = IDENT("i");
  auto *query = QUERY(SINGLE_QUERY(UNWIND(LIST(LITERAL(1)), AS("i")), MERGE(PATTERN(node_n))));
  std::list<BaseOpChecker *> on_match{new ExpectScanAll(), new ExpectFilter()};
  std::list<BaseOpChecker *> on_create{new ExpectCreateNode()};
  CheckPlan<TypeParam>(query, this->storage, ExpectUnwind(), ExpectMerge(on_match, on_create), ExpectEmptyResult());
  DeleteListContent(&on_match);
  DeleteListContent(&on_create);
}

TYPED_TEST(TestPlanner, UnwindMergeNodePropertyWithIndex) {
  // Test UNWIND [1] AS i MERGE (n :label {prop: i}) with label-property index
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, property.second, 1);
  auto node_n = NODE("n", label_name);
  std::get<0>(node_n->properties_)[this->storage.GetPropertyIx(property.first)] = IDENT("i");
  auto *query = QUERY(SINGLE_QUERY(UNWIND(LIST(LITERAL(1)), AS("i")), MERGE(PATTERN(node_n))));
  std::list<BaseOpChecker *> on_match{new ExpectScanAllByLabelProperties(
      label, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(IDENT("i"))})};
  std::list<BaseOpChecker *> on_create{new ExpectCreateNode()};
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectUnwind(), ExpectMerge(on_match, on_create), ExpectEmptyResult());
  DeleteListContent(&on_match);
  DeleteListContent(&on_create);
}

TYPED_TEST(TestPlanner, MultipleOptionalMatchReturn) {
  // Test OPTIONAL MATCH (n) OPTIONAL MATCH (m) RETURN n
  auto *query =
      QUERY(SINGLE_QUERY(OPTIONAL_MATCH(PATTERN(NODE("n"))), OPTIONAL_MATCH(PATTERN(NODE("m"))), RETURN("n")));
  std::list<BaseOpChecker *> optional{new ExpectScanAll()};
  CheckPlan<TypeParam>(query, this->storage, ExpectOptional(optional), ExpectOptional(optional), ExpectProduce());
  DeleteListContent(&optional);
}

TYPED_TEST(TestPlanner, FunctionAggregationReturn) {
  // Test WITH 42 AS group_by RETURN sqrt(SUM(2)) AS result, group_by AS group_by
  auto sum = SUM(LITERAL(2), false);
  const std::string group_by_ident = "group_by";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(SINGLE_QUERY(with_group_by, RETURN(FN("sqrt", sum), AS("result"), group_by, AS(group_by_ident))));
  auto aggr = ExpectAggregate({sum}, {group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, FunctionWithoutArguments) {
  // Test RETURN pi() AS pi
  auto *query = QUERY(SINGLE_QUERY(RETURN(FN("pi"), AS("pi"))));
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce());
}

TYPED_TEST(TestPlanner, ListLiteralAggregationReturn) {
  // Test WITH 42 AS group_by RETURN [SUM(2)] AS result, group_by AS group_by
  auto sum = SUM(LITERAL(2), false);
  const std::string group_by_ident = "group_by";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(SINGLE_QUERY(with_group_by, RETURN(LIST(sum), AS("result"), group_by, AS(group_by_ident))));
  auto aggr = ExpectAggregate({sum}, {group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MapLiteralAggregationReturn) {
  // Test WITH 42 AS group_by RETURN {sum: SUM(2)} AS result, group_by AS group_by
  auto sum = SUM(LITERAL(2), false);
  const std::string group_by_ident = "group_by";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(SINGLE_QUERY(with_group_by, RETURN(MAP({this->storage.GetPropertyIx("sum"), sum}), AS("result"),
                                                         group_by, AS(group_by_ident))));
  auto aggr = ExpectAggregate({sum}, {group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MapProjectionLiteralAggregationReturn) {
  // Test WITH 42 AS group_by, {} as map RETURN map {sum: SUM(2)} AS result, group_by AS group_by
  AstStorage storage;
  FakeDbAccessor dba;
  auto sum = SUM(LITERAL(2), false);
  const std::string group_by_ident = "group_by";
  auto group_by = IDENT(group_by_ident);
  auto with_clause = WITH(LITERAL(42), AS(group_by_ident), MAP(), AS("map"));
  auto elements = std::unordered_map<memgraph::query::PropertyIx, memgraph::query::Expression *>{
      {storage.GetPropertyIx("sum"), sum}};
  auto *query = QUERY(SINGLE_QUERY(
      with_clause, RETURN(MAP_PROJECTION(IDENT("map"), elements), AS("result"), group_by, AS(group_by_ident))));
  auto aggr = ExpectAggregate({sum}, {group_by});
  CheckPlan<TypeParam>(query, storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, EmptyListIndexAggregation) {
  // Test WITH 42 AS group_by RETURN [][SUM(2)] AS result, group_by AS group_by
  auto sum = SUM(LITERAL(2), false);
  auto empty_list = LIST();
  const std::string group_by_ident = "group_by";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(SINGLE_QUERY(
      with_group_by, RETURN(this->storage.template Create<memgraph::query::SubscriptOperator>(empty_list, sum),
                            AS("result"), group_by, AS(group_by_ident))));
  // We expect to group by `group_by` and the empty list, because it is a
  // sub-expression of a binary operator which contains an aggregation. This is
  // similar to grouping by '1' in `RETURN 1 + SUM(2)`.
  auto aggr = ExpectAggregate({sum}, {empty_list, group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, ListSliceAggregationReturn) {
  // Test WITH 42 AS group_by RETURN [1, 2][0..SUM(2)] AS result, group_by AS group_by
  auto sum = SUM(LITERAL(2), false);
  auto list = LIST(LITERAL(1), LITERAL(2));
  const std::string group_by_ident = "group_by";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(
      SINGLE_QUERY(with_group_by, RETURN(SLICE(list, LITERAL(0), sum), AS("result"), group_by, AS(group_by_ident))));
  // Similarly to EmptyListIndexAggregation test, we expect grouping by list and
  // `group_by`, because slicing is an operator.
  auto aggr = ExpectAggregate({sum}, {list, group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, ListWithAggregationAndGroupBy) {
  // Test WITH 42 AS s RETURN [sum(2), s]
  auto sum = SUM(LITERAL(2), false);
  const std::string group_by_ident = "s";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(SINGLE_QUERY(with_group_by, RETURN(LIST(sum, group_by), AS("result"))));
  auto aggr = ExpectAggregate({sum}, {group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, AggregationWithListWithAggregationAndGroupBy) {
  // Test WITH 42 AS s RETURN sum(2), [sum(3), s]
  auto sum2 = SUM(LITERAL(2), false);
  auto sum3 = SUM(LITERAL(3), false);
  const std::string group_by_ident = "s";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(SINGLE_QUERY(with_group_by, RETURN(sum2, AS("sum2"), LIST(sum3, group_by), AS("list"))));
  auto aggr = ExpectAggregate({sum2, sum3}, {group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MapWithAggregationAndGroupBy) {
  // Test WITH 42 as s RETURN {lit: s, sum: sum(2)}
  auto sum = SUM(LITERAL(2), false);
  const std::string group_by_ident = "s";
  auto group_by = IDENT(group_by_ident);
  auto with_group_by = WITH(LITERAL(42), AS(group_by_ident));
  auto *query = QUERY(SINGLE_QUERY(with_group_by, RETURN(MAP({this->storage.GetPropertyIx("sum"), sum},
                                                             {this->storage.GetPropertyIx("lit"), group_by}),
                                                         AS("result"))));
  auto aggr = ExpectAggregate({sum}, {group_by});
  CheckPlan<TypeParam>(query, this->storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MapProjectionWithAggregationAndGroupBy) {
  // Test WITH 42 as s, {} as map RETURN map {lit: s, sum: SUM(2)} AS result
  AstStorage storage;
  FakeDbAccessor dba;
  auto sum = SUM(LITERAL(2), false);
  const std::string group_by_ident = "s";
  auto group_by = IDENT(group_by_ident);
  auto with_clause = WITH(LITERAL(42), AS(group_by_ident), MAP(), AS("map"));
  auto projection = std::unordered_map<memgraph::query::PropertyIx, memgraph::query::Expression *>{
      {storage.GetPropertyIx("lit"), group_by}, {storage.GetPropertyIx("sum"), sum}};
  auto *query = QUERY(SINGLE_QUERY(with_clause, RETURN(MAP_PROJECTION(IDENT("map"), projection), AS("result"))));
  auto aggr = ExpectAggregate({sum}, {group_by});
  CheckPlan<TypeParam>(query, storage, ExpectProduce(), aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, AtomIndexedLabelProperty) {
  // Test MATCH (n :label {property: 42, not_indexed: 0}) RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  auto not_indexed = PROPERTY_PAIR(dba, "not_indexed");
  dba.SetIndexCount(label, 1);
  dba.SetIndexCount(label, property.second, 1);
  auto node = NODE("n", "label");
  auto lit_42 = LITERAL(42);
  std::get<0>(node->properties_)[this->storage.GetPropertyIx(property.first)] = lit_42;
  std::get<0>(node->properties_)[this->storage.GetPropertyIx(not_indexed.first)] = LITERAL(0);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(node)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{property.second}},
                                           std::vector{ExpressionRange::Equal(lit_42)}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, AtomPropertyWhereLabelIndexing) {
  // Test MATCH (n {property: 42}) WHERE n.not_indexed AND n:label RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  auto not_indexed = PROPERTY_PAIR(dba, "not_indexed");
  dba.SetIndexCount(label, property.second, 0);
  auto node = NODE("n");
  auto lit_42 = LITERAL(42);
  std::get<0>(node->properties_)[this->storage.GetPropertyIx(property.first)] = lit_42;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(node)),
      WHERE(AND(PROPERTY_LOOKUP(dba, "n", not_indexed),
                this->storage.template Create<memgraph::query::LabelsTest>(
                    IDENT("n"), std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("label")}))),
      RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{property.second}},
                                           std::vector{ExpressionRange::Equal(lit_42)}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, WhereIndexedLabelProperty) {
  // Test MATCH (n :label) WHERE n.property = 42 RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  dba.SetIndexCount(label, property.second, 0);
  auto lit_42 = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(EQ(PROPERTY_LOOKUP(dba, "n", property), lit_42)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{property.second}},
                                           std::vector{ExpressionRange::Equal(lit_42)}),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, BestPropertyIndexed) {
  // Test MATCH (n :label) WHERE n.property = 1 AND n.better = 42 RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  // Add a vertex with :label+property combination, so that the best
  // :label+better remains empty and thus better choice.
  dba.SetIndexCount(label, property, 1);
  auto better = PROPERTY_PAIR(dba, "better");
  dba.SetIndexCount(label, better.second, 0);
  auto lit_42 = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n", "label"))),
      WHERE(AND(EQ(PROPERTY_LOOKUP(dba, "n", property), LITERAL(1)), EQ(PROPERTY_LOOKUP(dba, "n", better), lit_42))),
      RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{better.second}},
                                           std::vector{ExpressionRange::Equal(lit_42)}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MultiPropertyIndexScan) {
  // Test MATCH (n :label1), (m :label2) WHERE n.prop1 = 1 AND m.prop2 = 2
  //      RETURN n, m
  FakeDbAccessor dba;
  auto label1 = dba.Label("label1");
  auto label2 = dba.Label("label2");
  auto prop1 = PROPERTY_PAIR(dba, "prop1");
  auto prop2 = PROPERTY_PAIR(dba, "prop2");
  dba.SetIndexCount(label1, prop1.second, 0);
  dba.SetIndexCount(label2, prop2.second, 0);
  auto lit_1 = LITERAL(1);
  auto lit_2 = LITERAL(2);
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label1")), PATTERN(NODE("m", "label2"))),
                   WHERE(AND(EQ(PROPERTY_LOOKUP(dba, "n", prop1), lit_1), EQ(PROPERTY_LOOKUP(dba, "m", prop2), lit_2))),
                   RETURN("n", "m")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_cartesian_ops{new ExpectScanAllByLabelProperties(
      label1, std::vector{ms::PropertyPath{prop1.second}}, std::vector{ExpressionRange::Equal(lit_1)})};
  std::list<BaseOpChecker *> right_cartesian_ops{new ExpectScanAllByLabelProperties(
      label2, std::vector{ms::PropertyPath{prop2.second}}, std::vector{ExpressionRange::Equal(lit_2)})};

  CheckPlan(planner.plan(), symbol_table, ExpectCartesian(left_cartesian_ops, right_cartesian_ops), ExpectProduce());

  DeleteListContent(&left_cartesian_ops);
  DeleteListContent(&right_cartesian_ops);
}

TYPED_TEST(TestPlanner, WhereIndexedLabelPropertyRange) {
  // Test MATCH (n :label) WHERE n.property REL_OP 42 RETURN n
  // REL_OP is one of: `<`, `<=`, `>`, `>=`
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  dba.SetIndexCount(label, property, 0);
  auto lit_42 = LITERAL(42);
  auto n_prop = PROPERTY_LOOKUP(dba, "n", property);
  auto check_planned_range = [&, this](const auto &rel_expr, auto lower_bound, auto upper_bound) {
    // Shadow the first this->storage, so that the query is created in this one.
    this->storage.GetLabelIx("label");
    this->storage.GetPropertyIx("property");
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))), WHERE(rel_expr), RETURN("n")));
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table,
              ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{property}},
                                             std::vector{ExpressionRange::Range(lower_bound, upper_bound)}),
              ExpectProduce());
  };
  {
    // Test relation operators which form an upper bound for range.
    std::vector<std::pair<memgraph::query::Expression *, Bound::Type>> upper_bound_rel_op{
        std::make_pair(LESS(n_prop, lit_42), Bound::Type::EXCLUSIVE),
        std::make_pair(LESS_EQ(n_prop, lit_42), Bound::Type::INCLUSIVE),
        std::make_pair(GREATER(lit_42, n_prop), Bound::Type::EXCLUSIVE),
        std::make_pair(GREATER_EQ(lit_42, n_prop), Bound::Type::INCLUSIVE)};
    for (const auto &rel_op : upper_bound_rel_op) {
      check_planned_range(rel_op.first, std::nullopt, Bound(lit_42, rel_op.second));
    }
  }
  {
    // Test relation operators which form a lower bound for range.
    std::vector<std::pair<memgraph::query::Expression *, Bound::Type>> lower_bound_rel_op{
        std::make_pair(LESS(lit_42, n_prop), Bound::Type::EXCLUSIVE),
        std::make_pair(LESS_EQ(lit_42, n_prop), Bound::Type::INCLUSIVE),
        std::make_pair(GREATER(n_prop, lit_42), Bound::Type::EXCLUSIVE),
        std::make_pair(GREATER_EQ(n_prop, lit_42), Bound::Type::INCLUSIVE)};
    for (const auto &rel_op : lower_bound_rel_op) {
      check_planned_range(rel_op.first, Bound(lit_42, rel_op.second), std::nullopt);
    }
  }
}

TYPED_TEST(TestPlanner, WherePreferEqualityIndexOverRange) {
  // Test MATCH (n :label) WHERE n.property = 42 AND n.property > 0 RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  dba.SetIndexCount(label, property.second, 0);
  auto lit_42 = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(AND(EQ(PROPERTY_LOOKUP(dba, "n", property), lit_42),
                                             GREATER(PROPERTY_LOOKUP(dba, "n", property), LITERAL(0)))),
                                   RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{property.second}},
                                           std::vector{ExpressionRange::Equal(lit_42)}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, UnableToUsePropertyIndex) {
  // Test MATCH (n: label) WHERE n.property = n.property RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  dba.SetIndexCount(label, 0);
  dba.SetIndexCount(label, property, 0);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(EQ(PROPERTY_LOOKUP(dba, "n", property), PROPERTY_LOOKUP(dba, "n", property))),
                                   RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  // We can only get ScanAllByLabelIndex, because we are comparing properties
  // with those on the same node.
  CheckPlan(planner.plan(), symbol_table, ExpectScanAllByLabel(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, SecondPropertyIndex) {
  // Test MATCH (n :label), (m :label) WHERE m.property = n.property RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  dba.SetIndexCount(label, 0);
  dba.SetIndexCount(label, dba.Property("property"), 0);
  auto n_prop = PROPERTY_LOOKUP(dba, "n", property);
  auto m_prop = PROPERTY_LOOKUP(dba, "m", property);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label")), PATTERN(NODE("m", "label"))),
                                   WHERE(EQ(m_prop, n_prop)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  // Note: We are scanning for m, therefore property should equal n_prop.
  std::list<BaseOpChecker *> left_index_join_ops{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> right_index_join_ops{new ExpectScanAllByLabelProperties(
      label, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(n_prop)})};

  CheckPlan(planner.plan(), symbol_table, ExpectIndexedJoin(left_index_join_ops, right_index_join_ops),
            ExpectProduce());

  DeleteListContent(&left_index_join_ops);
  DeleteListContent(&right_index_join_ops);
}

TYPED_TEST(TestPlanner, UnableToUseSecondPropertyIndex) {
  // Test MATCH (n :label), (m :label) WHERE m.property = n.property RETURN n
  FakeDbAccessor dba;
  auto property = PROPERTY_PAIR(dba, "property");
  auto n_prop = PROPERTY_LOOKUP(dba, "n", property);
  auto m_prop = PROPERTY_LOOKUP(dba, "m", property);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label")), PATTERN(NODE("m", "label"))),
                                   WHERE(EQ(m_prop, n_prop)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_index_join_ops{new ExpectScanAll(), new ExpectFilter()};
  std::list<BaseOpChecker *> right_index_join_ops{new ExpectScanAll(), new ExpectFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectHashJoin(left_index_join_ops, right_index_join_ops), ExpectProduce());

  DeleteListContent(&left_index_join_ops);
  DeleteListContent(&right_index_join_ops);
}

TYPED_TEST(TestPlanner, ReturnSumGroupByAll) {
  // Test RETURN sum([1,2,3]), all(x in [1] where x = 1)
  auto sum = SUM(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), false);
  auto *all = ALL("x", LIST(LITERAL(1)), WHERE(EQ(IDENT("x"), LITERAL(1))));
  auto *query = QUERY(SINGLE_QUERY(RETURN(sum, AS("sum"), all, AS("all"))));
  auto aggr = ExpectAggregate({sum}, {all});
  CheckPlan<TypeParam>(query, this->storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariable) {
  // Test MATCH (n) -[r *..3]-> (m) RETURN r
  auto edge = EDGE_VARIABLE("r");
  edge->upper_bound_ = LITERAL(3);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectExpandVariable(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariableNoBounds) {
  // Test MATCH (n) -[r *]-> (m) RETURN r
  auto edge = EDGE_VARIABLE("r");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectExpandVariable(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariableInlinedFilter) {
  // Test MATCH (n) -[r :type * {prop: 42}]-> (m) RETURN r
  FakeDbAccessor dba;
  auto type = "type";
  auto prop = PROPERTY_PAIR(dba, "prop");
  auto edge = EDGE_VARIABLE("r", Type::DEPTH_FIRST, Direction::BOTH, {type});
  std::get<0>(edge->properties_)[this->storage.GetPropertyIx(prop.first)] = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(),
                       ExpectExpandVariable(),  // Filter is both inlined and post-expand
                       ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariableNotInlinedFilter) {
  // Test MATCH (n) -[r :type * {prop: m.prop}]-> (m) RETURN r
  FakeDbAccessor dba;
  auto type = "type";
  auto prop = PROPERTY_PAIR(dba, "prop");
  auto edge = EDGE_VARIABLE("r", Type::DEPTH_FIRST, Direction::BOTH, {type});
  std::get<0>(edge->properties_)[this->storage.GetPropertyIx(prop.first)] =
      EQ(PROPERTY_LOOKUP(dba, "m", prop), LITERAL(42));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectExpandVariable(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariableTotalWeightSymbol) {
  // Test MATCH p = (a {id: 0})-[r* wShortest (e, v | 1) total_weight]->(b)
  // RETURN *
  FakeDbAccessor dba;
  auto edge = EDGE_VARIABLE("r", Type::WEIGHTED_SHORTEST_PATH, Direction::BOTH, {}, nullptr, nullptr, nullptr, nullptr,
                            nullptr, IDENT("total_weight"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("*")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  auto *root = dynamic_cast<Produce *>(&planner.plan());

  ASSERT_TRUE(root);

  const auto &nes = root->named_expressions_;
  EXPECT_TRUE(nes.size() == 4);

  std::vector<std::string> names(nes.size());
  std::transform(nes.begin(), nes.end(), names.begin(), [](const auto *ne) { return ne->name_; });

  EXPECT_TRUE(root->named_expressions_.size() == 4);
  EXPECT_TRUE(memgraph::utils::Contains(names, "m"));
  EXPECT_TRUE(memgraph::utils::Contains(names, "n"));
  EXPECT_TRUE(memgraph::utils::Contains(names, "r"));
  EXPECT_TRUE(memgraph::utils::Contains(names, "total_weight"));
}

TYPED_TEST(TestPlanner, UnwindMatchVariable) {
  // Test UNWIND [1,2,3] AS depth MATCH (n) -[r*d]-> (m) RETURN r
  auto edge = EDGE_VARIABLE("r", Type::DEPTH_FIRST, Direction::OUT);
  edge->lower_bound_ = IDENT("d");
  edge->upper_bound_ = IDENT("d");
  auto *query = QUERY(SINGLE_QUERY(UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), AS("d")),
                                   MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, this->storage, ExpectUnwind(), ExpectScanAll(), ExpectExpandVariable(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchBfs) {
  // Test MATCH (n) -[r:type *..10 (r, n|n)]-> (m) RETURN r
  FakeDbAccessor dba;
  auto edge_type = this->storage.GetEdgeTypeIx("type");
  auto *bfs = this->storage.template Create<memgraph::query::EdgeAtom>(
      IDENT("r"), memgraph::query::EdgeAtom::Type::BREADTH_FIRST, Direction::OUT,
      std::vector<memgraph::query::QueryEdgeType>{edge_type});
  bfs->filter_lambda_.inner_edge = IDENT("r");
  bfs->filter_lambda_.inner_node = IDENT("n");
  bfs->filter_lambda_.expression = IDENT("n");
  bfs->upper_bound_ = LITERAL(10);
  auto *as_r = NEXPR("r", IDENT("r"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), bfs, NODE("m"))), RETURN(as_r)));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpandBfs(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchScanToExpand) {
  // Test MATCH (n) -[r]- (m :label {property: 1}) RETURN r
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  // Fill vertices to the max + 1.
  dba.SetIndexCount(label, property, FLAGS_query_vertex_count_to_expand_existing + 1);
  dba.SetIndexCount(label, FLAGS_query_vertex_count_to_expand_existing + 1);
  auto node_m = NODE("m", "label");
  std::get<0>(node_m->properties_)[this->storage.GetPropertyIx("property")] = LITERAL(1);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), node_m)), RETURN("r")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  // We expect 1x ScanAll and then Expand, since we are guessing that
  // is faster (due to high label index vertex count).
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWhereAndSplit) {
  // Test MATCH (n) -[r]- (m) WHERE n.prop AND r.prop RETURN m
  FakeDbAccessor dba;
  auto prop = PROPERTY_PAIR(dba, "prop");
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                         WHERE(AND(PROPERTY_LOOKUP(dba, "n", prop), PROPERTY_LOOKUP(dba, "r", prop))), RETURN("m")));
  // We expect `n.prop` filter right after scanning `n`.
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectFilter(), ExpectExpand(), ExpectFilter(),
                       ExpectProduce());
}

TYPED_TEST(TestPlanner, ReturnAsteriskOmitsLambdaSymbols) {
  // Test MATCH (n) -[r* (ie, in | true)]- (m) RETURN *
  FakeDbAccessor dba;
  auto edge = EDGE_VARIABLE("r", Type::DEPTH_FIRST, Direction::BOTH);
  edge->filter_lambda_.inner_edge = IDENT("ie");
  edge->filter_lambda_.inner_node = IDENT("in");
  edge->filter_lambda_.expression = LITERAL(true);
  auto ret = this->storage.template Create<memgraph::query::Return>();
  ret->body_.all_identifiers = true;
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), ret));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  auto *produce = dynamic_cast<Produce *>(&planner.plan());
  ASSERT_TRUE(produce);
  std::vector<std::string> outputs;
  for (const auto &output_symbol : produce->OutputSymbols(symbol_table)) {
    outputs.emplace_back(output_symbol.name());
  }
  // We expect `*` expanded to `n`, `r` and `m`.
  EXPECT_EQ(outputs.size(), 3);
  for (const auto &name : {"n", "r", "m"}) {
    EXPECT_TRUE(memgraph::utils::Contains(outputs, name));
  }
}

TYPED_TEST(TestPlanner, FilterRegexMatchIndex) {
  // Test MATCH (n :label) WHERE n.prop =~ "regex" RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto label = dba.Label("label");
  dba.SetIndexCount(label, 0);
  dba.SetIndexCount(label, prop, 0);
  auto *regex_match =
      this->storage.template Create<memgraph::query::RegexMatch>(PROPERTY_LOOKUP(dba, "n", prop), LITERAL("regex"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))), WHERE(regex_match), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{prop}},
                                           std::vector{ExpressionRange::RegexMatch()}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, FilterRegexMatchPreferEqualityIndex) {
  // Test MATCH (n :label) WHERE n.prop =~ "regex" AND n.prop = 42 RETURN n
  FakeDbAccessor dba;
  auto prop = PROPERTY_PAIR(dba, "prop");
  auto label = dba.Label("label");
  dba.SetIndexCount(label, 0);
  dba.SetIndexCount(label, prop.second, 0);
  auto *regex_match =
      this->storage.template Create<memgraph::query::RegexMatch>(PROPERTY_LOOKUP(dba, "n", prop), LITERAL("regex"));
  auto *lit_42 = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(AND(regex_match, EQ(PROPERTY_LOOKUP(dba, "n", prop), lit_42))), RETURN("n")));
  // We expect that we use index by property value equal to 42, because that's
  // much better than property range for regex matching.
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{prop.second}},
                                           std::vector{ExpressionRange::Equal(lit_42)}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, FilterRegexMatchPreferEqualityIndex2) {
  // Test MATCH (n :label)
  // WHERE n.prop =~ "regex" AND n.prop = 42 AND n.prop > 0 RETURN n
  FakeDbAccessor dba;
  auto prop = PROPERTY_PAIR(dba, "prop");
  auto label = dba.Label("label");
  dba.SetIndexCount(label, 0);
  dba.SetIndexCount(label, prop.second, 0);
  auto *regex_match =
      this->storage.template Create<memgraph::query::RegexMatch>(PROPERTY_LOOKUP(dba, "n", prop), LITERAL("regex"));
  auto *lit_42 = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(AND(AND(regex_match, EQ(PROPERTY_LOOKUP(dba, "n", prop), lit_42)),
                                             GREATER(PROPERTY_LOOKUP(dba, "n", prop), LITERAL(0)))),
                                   RETURN("n")));
  // We expect that we use index by property value equal to 42, because that's
  // much better than property range.
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{prop.second}},
                                           std::vector{ExpressionRange::Equal(lit_42)}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, FilterRegexMatchPreferRangeIndex) {
  // Test MATCH (n :label) WHERE n.prop =~ "regex" AND n.prop > 42 RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto label = dba.Label("label");
  dba.SetIndexCount(label, 0);
  dba.SetIndexCount(label, prop, 0);
  auto *regex_match =
      this->storage.template Create<memgraph::query::RegexMatch>(PROPERTY_LOOKUP(dba, "n", prop), LITERAL("regex"));
  auto *lit_42 = LITERAL(42);
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                         WHERE(AND(regex_match, GREATER(PROPERTY_LOOKUP(dba, "n", prop), lit_42))), RETURN("n")));
  // We expect that we use index by property range on a concrete value (42), as
  // it is much better than using a range from empty string for regex matching.
  Bound lower_bound(lit_42, Bound::Type::EXCLUSIVE);
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{prop}},
                                           std::vector{ExpressionRange::Range(lower_bound, std::nullopt)}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, CallProcedureStandalone) {
  // Test CALL proc(1,2,3) YIELD field AS result
  FakeDbAccessor dba;
  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  ast_call->procedure_name_ = "proc";
  ast_call->arguments_ = {LITERAL(1), LITERAL(2), LITERAL(3)};
  ast_call->result_fields_ = {"field"};
  ast_call->result_identifiers_ = {IDENT("result")};
  auto *query = QUERY(SINGLE_QUERY(ast_call));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  std::vector<Symbol> result_syms;
  result_syms.reserve(ast_call->result_identifiers_.size());
  for (const auto *ident : ast_call->result_identifiers_) {
    result_syms.push_back(symbol_table.at(*ident));
  }
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(
      planner.plan(), symbol_table,
      ExpectCallProcedure(ast_call->procedure_name_, ast_call->arguments_, ast_call->result_fields_, result_syms));
}

TYPED_TEST(TestPlanner, CallProcedureAfterScanAll) {
  // Test MATCH (n) CALL proc(n) YIELD field AS result RETURN result
  FakeDbAccessor dba;
  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  ast_call->procedure_name_ = "proc";
  ast_call->arguments_ = {IDENT("n")};
  ast_call->result_fields_ = {"field"};
  ast_call->result_identifiers_ = {IDENT("result")};
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), ast_call, RETURN("result")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  std::vector<Symbol> result_syms;
  result_syms.reserve(ast_call->result_identifiers_.size());
  for (const auto *ident : ast_call->result_identifiers_) {
    result_syms.push_back(symbol_table.at(*ident));
  }
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectCallProcedure(ast_call->procedure_name_, ast_call->arguments_, ast_call->result_fields_, result_syms),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, CallProcedureBeforeScanAll) {
  // Test CALL proc() YIELD field MATCH (n) WHERE n.prop = field RETURN n
  FakeDbAccessor dba;
  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  ast_call->procedure_name_ = "proc";
  ast_call->result_fields_ = {"field"};
  ast_call->result_identifiers_ = {IDENT("field")};
  auto property = dba.Property("prop");
  auto *query = QUERY(SINGLE_QUERY(ast_call, MATCH(PATTERN(NODE("n"))),
                                   WHERE(EQ(PROPERTY_LOOKUP(dba, "n", property), IDENT("field"))), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  std::vector<Symbol> result_syms;
  result_syms.reserve(ast_call->result_identifiers_.size());
  for (const auto *ident : ast_call->result_identifiers_) {
    result_syms.push_back(symbol_table.at(*ident));
  }
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectCallProcedure(ast_call->procedure_name_, ast_call->arguments_, ast_call->result_fields_, result_syms),
            ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, ScanAllById) {
  // Test MATCH (n) WHERE id(n) = 42 RETURN n
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EQ(FN("id", IDENT("n")), LITERAL(42))), RETURN("n")));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAllById(), ExpectProduce());
}

TYPED_TEST(TestPlanner, ScanAllByEdgeId) {
  // Test MATCH ()-[r]->() WHERE id(r) = 42 RETURN r
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("anon1"), EDGE("r"), NODE("anon2"))),
                                   WHERE(EQ(FN("id", IDENT("r")), LITERAL(42))), RETURN("r")));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAllByEdgeId(), ExpectProduce());
}

TYPED_TEST(TestPlanner, BfsToExisting) {
  // Test MATCH (n)-[r *bfs]-(m) WHERE id(m) = 42 RETURN r
  auto *bfs = this->storage.template Create<memgraph::query::EdgeAtom>(
      IDENT("r"), memgraph::query::EdgeAtom::Type::BREADTH_FIRST, Direction::BOTH);
  bfs->filter_lambda_.inner_edge = IDENT("ie");
  bfs->filter_lambda_.inner_node = IDENT("in");
  bfs->filter_lambda_.expression = LITERAL(true);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), bfs, NODE("m"))),
                                   WHERE(EQ(FN("id", IDENT("m")), LITERAL(42))), RETURN("r")));
  CheckPlan<TypeParam>(query, this->storage, ExpectScanAll(), ExpectScanAllById(), ExpectExpandBfs(), ExpectProduce());
}

TYPED_TEST(TestPlanner, LabelPropertyInListValidOptimization) {
  // Test MATCH (n:label) WHERE n.property IN ['a'] RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  auto *lit_list_a = LIST(LITERAL('a'));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(IN_LIST(PROPERTY_LOOKUP(dba, "n", property), lit_list_a)), RETURN("n")));
  {
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
  }
  {
    dba.SetIndexCount(label, 1);
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByLabel(), ExpectFilter(), ExpectProduce());
  }
  {
    dba.SetIndexCount(label, property.second, 1);
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    // Unwind produces a sybmol, then scan would be based on that identifier
    // CheckPlan ATM is only checking stucture and types, values are not checked
    // Hence a fake Identifier is enough for this test
    auto fake_identifier = IDENT("fake");
    CheckPlan(planner.plan(), symbol_table, ExpectUnwind(),
              ExpectScanAllByLabelProperties(label, std::vector{ms::PropertyPath{property.second}},
                                             std::vector{ExpressionRange::Equal(fake_identifier)}),
              ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, LabelPropertyInListWhereLabelPropertyOnLeftNotListOnRight) {
  // Test MATCH (n:label) WHERE n.property IN 'a' RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  auto *lit_a = LITERAL('a');
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(IN_LIST(PROPERTY_LOOKUP(dba, "n", property), lit_a)), RETURN("n")));
  {
    dba.SetIndexCount(label, property.second, 1);
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, LabelPropertyInListWhereLabelPropertyOnRight) {
  // Test MATCH (n:label) WHERE ['a'] IN n.property RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR(dba, "property");
  auto *lit_list_a = LIST(LITERAL('a'));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", "label"))),
                                   WHERE(IN_LIST(lit_list_a, PROPERTY_LOOKUP(dba, "n", property))), RETURN("n")));
  {
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
  }
  {
    dba.SetIndexCount(label, 1);
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByLabel(), ExpectFilter(), ExpectProduce());
  }
  {
    dba.SetIndexCount(label, property.second, 1);
    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table, ExpectScanAllByLabel(), ExpectFilter(), ExpectProduce());
  }
}

TYPED_TEST(TestPlanner, Foreach) {
  FakeDbAccessor dba;
  {
    auto *i = NEXPR("i", IDENT("i"));
    auto *query = QUERY(SINGLE_QUERY(FOREACH(i, {CREATE(PATTERN(NODE("n")))})));
    auto create = ExpectCreateNode();
    std::list<BaseOpChecker *> updates{&create};
    std::list<BaseOpChecker *> input;
    CheckPlan<TypeParam>(query, this->storage, ExpectForeach(input, updates), ExpectEmptyResult());
  }
  {
    auto *i = NEXPR("i", IDENT("i"));
    auto *query = QUERY(SINGLE_QUERY(FOREACH(i, {DELETE(IDENT("i"))})));
    auto del = ExpectDelete();
    std::list<BaseOpChecker *> updates{&del};
    std::list<BaseOpChecker *> input;
    CheckPlan<TypeParam>(query, this->storage, ExpectForeach({input}, updates), ExpectEmptyResult());
  }
  {
    auto prop = dba.Property("prop");
    auto *i = NEXPR("i", IDENT("i"));
    auto *query = QUERY(SINGLE_QUERY(FOREACH(i, {SET(PROPERTY_LOOKUP(dba, "i", prop), LITERAL(10))})));
    auto set_prop = ExpectSetProperty();
    std::list<BaseOpChecker *> updates{&set_prop};
    std::list<BaseOpChecker *> input;
    CheckPlan<TypeParam>(query, this->storage, ExpectForeach({input}, updates), ExpectEmptyResult());
  }
  {
    auto *i = NEXPR("i", IDENT("i"));
    auto *j = NEXPR("j", IDENT("j"));
    auto *query = QUERY(SINGLE_QUERY(FOREACH(i, {FOREACH(j, {CREATE(PATTERN(NODE("n"))), DELETE(IDENT("i"))})})));
    auto create = ExpectCreateNode();
    auto del = ExpectDelete();
    std::list<BaseOpChecker *> input;
    std::list<BaseOpChecker *> nested_updates{{&create, &del}};
    auto nested_foreach = ExpectForeach(input, nested_updates);
    std::list<BaseOpChecker *> updates{&nested_foreach};
    CheckPlan<TypeParam>(query, this->storage, ExpectForeach(input, updates), ExpectEmptyResult());
  }
  {
    auto *i = NEXPR("i", IDENT("i"));
    auto *j = NEXPR("j", IDENT("j"));
    auto create = ExpectCreateNode();
    std::list<BaseOpChecker *> empty;
    std::list<BaseOpChecker *> updates{&create};
    auto input_op = ExpectForeach(empty, updates);
    std::list<BaseOpChecker *> input{&input_op};
    auto *query =
        QUERY(SINGLE_QUERY(FOREACH(i, {CREATE(PATTERN(NODE("n")))}), FOREACH(j, {CREATE(PATTERN(NODE("n")))})));
    CheckPlan<TypeParam>(query, this->storage, ExpectForeach(input, updates), ExpectEmptyResult());
  }

  {
    // FOREACH with index
    // FOREACH (n in [...] | MERGE (v:Label));
    const auto label_name = "label";
    const auto label = dba.Label(label_name);
    dba.SetIndexCount(label, 0);

    auto *n = NEXPR("n", IDENT("n"));
    auto *query = QUERY(SINGLE_QUERY(FOREACH(n, {MERGE(PATTERN(NODE("v", label_name)))})));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

    std::list<BaseOpChecker *> on_match{new ExpectScanAllByLabel()};
    std::list<BaseOpChecker *> on_create{new ExpectCreateNode()};

    auto create = ExpectMerge(on_match, on_create);
    std::list<BaseOpChecker *> updates{&create};
    std::list<BaseOpChecker *> input;
    CheckPlan(planner.plan(), symbol_table, ExpectForeach(input, updates), ExpectEmptyResult());

    DeleteListContent(&on_match);
    DeleteListContent(&on_create);
  }
}

TYPED_TEST(TestPlanner, Exists) {
  // MATCH (n) WHERE exists((n)-[]-())
  FakeDbAccessor dba;
  {
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("n"))),
        WHERE(EXISTS(PATTERN(NODE("n"), EDGE("edge", memgraph::query::EdgeAtom::Direction::BOTH, {}, false),
                             NODE("node", std::nullopt, false)))),
        RETURN("n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> pattern_filter{new ExpectExpand(), new ExpectLimit(), new ExpectEvaluatePatternFilter()};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
              ExpectFilter(std::vector<std::list<BaseOpChecker *>>{pattern_filter}), ExpectProduce());

    DeleteListContent(&pattern_filter);
  }

  // MATCH (n) WHERE exists((n)-[:TYPE]-(:Two))
  {
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("n"))),
        WHERE(EXISTS(PATTERN(NODE("n"), EDGE("edge", memgraph::query::EdgeAtom::Direction::BOTH, {"TYPE"}, false),
                             NODE("node", "Two", false)))),
        RETURN("n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> pattern_filter{new ExpectExpand(), new ExpectFilter(), new ExpectLimit(),
                                              new ExpectEvaluatePatternFilter()};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
              ExpectFilter(std::vector<std::list<BaseOpChecker *>>{pattern_filter}), ExpectProduce());

    DeleteListContent(&pattern_filter);
  }

  // MATCH (n) WHERE exists((n)-[:TYPE]-(:Two)) AND exists((n)-[]-())
  {
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("n"))),
        WHERE(AND(EXISTS(PATTERN(NODE("n"), EDGE("edge", memgraph::query::EdgeAtom::Direction::BOTH, {"TYPE"}, false),
                                 NODE("node", "Two", false))),
                  EXISTS(PATTERN(NODE("n"), EDGE("edge2", memgraph::query::EdgeAtom::Direction::BOTH, {}, false),
                                 NODE("node2", std::nullopt, false))))),
        RETURN("n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> pattern_filter_with_types{new ExpectExpand(), new ExpectFilter(), new ExpectLimit(),
                                                         new ExpectEvaluatePatternFilter()};
    std::list<BaseOpChecker *> pattern_filter_without_types{new ExpectExpand(), new ExpectLimit(),
                                                            new ExpectEvaluatePatternFilter()};

    CheckPlan(
        planner.plan(), symbol_table, ExpectScanAll(),
        ExpectFilter(std::vector<std::list<BaseOpChecker *>>{pattern_filter_without_types, pattern_filter_with_types}),
        ExpectProduce());

    DeleteListContent(&pattern_filter_with_types);
    DeleteListContent(&pattern_filter_without_types);
  }

  // MATCH (n) WHERE n.prop = 1 AND exists((n)-[:TYPE]-(:Two))
  {
    auto property = dba.Property("prop");
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("n"))),
        WHERE(AND(EXISTS(PATTERN(NODE("n"), EDGE("edge", memgraph::query::EdgeAtom::Direction::BOTH, {"TYPE"}, false),
                                 NODE("node", "Two", false))),
                  PROPERTY_LOOKUP(dba, "n", property))),
        RETURN("n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> pattern_filter{new ExpectExpand(), new ExpectFilter(), new ExpectLimit(),
                                              new ExpectEvaluatePatternFilter()};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
              ExpectFilter(std::vector<std::list<BaseOpChecker *>>{pattern_filter}), ExpectProduce());

    DeleteListContent(&pattern_filter);
  }

  // MATCH (n) WHERE exists((n)-[:TYPE]-(:Two)) OR exists((n)-[]-())
  {
    auto *query = QUERY(SINGLE_QUERY(
        MATCH(PATTERN(NODE("n"))),
        WHERE(OR(EXISTS(PATTERN(NODE("n"), EDGE("edge", memgraph::query::EdgeAtom::Direction::BOTH, {"TYPE"}, false),
                                NODE("node", "Two", false))),
                 EXISTS(PATTERN(NODE("n"), EDGE("edge2", memgraph::query::EdgeAtom::Direction::BOTH, {}, false),
                                NODE("node2", std::nullopt, false))))),
        RETURN("n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> pattern_filter_with_types{new ExpectExpand(), new ExpectFilter(), new ExpectLimit(),
                                                         new ExpectEvaluatePatternFilter()};
    std::list<BaseOpChecker *> pattern_filter_without_types{new ExpectExpand(), new ExpectLimit(),
                                                            new ExpectEvaluatePatternFilter()};

    CheckPlan(
        planner.plan(), symbol_table, ExpectScanAll(),
        ExpectFilter(std::vector<std::list<BaseOpChecker *>>{pattern_filter_with_types, pattern_filter_without_types}),
        ExpectProduce());

    DeleteListContent(&pattern_filter_with_types);
    DeleteListContent(&pattern_filter_without_types);
  }
}

TYPED_TEST(TestPlanner, Subqueries) {
  // MATCH (n) CALL { MATCH (m) RETURN (m) } RETURN n, m
  FakeDbAccessor dba;
  {
    auto *subquery = SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN("n"));
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("m"))), CALL_SUBQUERY(subquery), RETURN("m", "n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> subquery_plan{new ExpectScanAll(), new ExpectProduce()};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectApply(subquery_plan), ExpectProduce());

    DeleteListContent(&subquery_plan);
  }

  // MATCH (n) CALL { MATCH (m)-[r]->(n) RETURN (m) } RETURN n, m
  {
    auto *subquery = SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r", Direction::OUT), NODE("m"))), RETURN("n"));
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("m"))), CALL_SUBQUERY(subquery), RETURN("m", "n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> subquery_plan{new ExpectScanAll(), new ExpectExpand(), new ExpectProduce()};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectApply(subquery_plan), ExpectProduce());

    DeleteListContent(&subquery_plan);
  }

  // MATCH (n) CALL { MATCH (p)-[r]->(s) WHERE s.prop = 2 RETURN (p) } RETURN n, p
  {
    auto property = dba.Property("prop");
    auto *subquery = SINGLE_QUERY(MATCH(PATTERN(NODE("p"), EDGE("r", Direction::OUT), NODE("s"))),
                                  WHERE(EQ(PROPERTY_LOOKUP(dba, "s", property), LITERAL(2))), RETURN("p"));
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), CALL_SUBQUERY(subquery), RETURN("n", "p")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> subquery_plan{new ExpectScanAll(), new ExpectExpand(), new ExpectFilter(),
                                             new ExpectProduce()};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectApply(subquery_plan), ExpectProduce());

    DeleteListContent(&subquery_plan);
  }

  // MATCH (m) CALL { MATCH (n) CALL { MATCH (o) RETURN o } RETURN n, o } RETURN m, n, o
  {
    auto *subquery_inside_subquery = SINGLE_QUERY(MATCH(PATTERN(NODE("o"))), RETURN("o"));
    auto *subquery = SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), CALL_SUBQUERY(subquery_inside_subquery), RETURN("n", "o"));
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("m"))), CALL_SUBQUERY(subquery), RETURN("m", "n", "o")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
    std::list<BaseOpChecker *> subquery_inside_subquery_plan{new ExpectScanAll(), new ExpectProduce()};
    std::list<BaseOpChecker *> subquery_plan{new ExpectScanAll(), new ExpectApply(subquery_inside_subquery_plan),
                                             new ExpectProduce()};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectApply(subquery_plan), ExpectProduce());

    DeleteListContent(&subquery_plan);
    DeleteListContent(&subquery_inside_subquery_plan);
  }

  // MATCH (m) CALL { MATCH (n) RETURN n UNION MATCH (n) RETURN n } RETURN m, n
  {
    auto *subquery = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN("n")),
                           UNION_ALL(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN("n"))));
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("m"))), CALL_SUBQUERY(subquery), RETURN("m", "n")));

    auto symbol_table = memgraph::query::MakeSymbolTable(query);
    auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

    std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAll(), new ExpectProduce()};
    std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAll(), new ExpectProduce()};
    std::list<BaseOpChecker *> subquery_plan{new ExpectUnion(left_subquery_part, right_subquery_part)};

    CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectApply(subquery_plan), ExpectProduce());

    DeleteListContent(&subquery_plan);
    DeleteListContent(&left_subquery_part);
    DeleteListContent(&right_subquery_part);
  }
}

TYPED_TEST(TestPlanner, PatternComprehensionInReturn) {
  FakeDbAccessor dba;
  const auto prop = PROPERTY_PAIR(dba, "prop");
  // MATCH (n) RETURN [(n)-[edge]->(m) | m.prop]
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      RETURN(NEXPR("alias", PATTERN_COMPREHENSION(nullptr,
                                                  PATTERN(NODE("n"), EDGE("edge", EdgeAtom::Direction::BOTH, {}, false),
                                                          NODE("m", std::nullopt, false)),
                                                  nullptr, PROPERTY_LOOKUP(dba, "m", prop))))));

  std::list<std::unique_ptr<BaseOpChecker>> input_ops;
  input_ops.push_back(std::make_unique<ExpectScanAll>());
  std::list<std::unique_ptr<BaseOpChecker>> list_collection_branch_ops;
  list_collection_branch_ops.push_back(std::make_unique<ExpectExpand>());
  list_collection_branch_ops.push_back(std::make_unique<ExpectProduce>());

  CheckPlan<TypeParam>(query, this->storage, ExpectRollUpApply(input_ops, list_collection_branch_ops), ExpectProduce());
}

TYPED_TEST(TestPlanner, PatternComprehensionInWith) {
  FakeDbAccessor dba;
  const auto prop = PROPERTY_PAIR(dba, "prop");
  // MATCH (n) WITH [(n)-[edge]->(m) | m.prop] AS alias RETURN alias
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      WITH(NEXPR("alias", PATTERN_COMPREHENSION(nullptr,
                                                PATTERN(NODE("n"), EDGE("edge", EdgeAtom::Direction::BOTH, {}, false),
                                                        NODE("m", std::nullopt, false)),
                                                nullptr, PROPERTY_LOOKUP(dba, "m", prop)))),
      RETURN("alias")));

  std::list<std::unique_ptr<BaseOpChecker>> input_ops;
  input_ops.push_back(std::make_unique<ExpectScanAll>());
  std::list<std::unique_ptr<BaseOpChecker>> list_collection_branch_ops;
  list_collection_branch_ops.push_back(std::make_unique<ExpectExpand>());
  list_collection_branch_ops.push_back(std::make_unique<ExpectProduce>());

  CheckPlan<TypeParam>(query, this->storage, ExpectRollUpApply(input_ops, list_collection_branch_ops), ExpectProduce(),
                       ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterNoIndex1) {
  // Test MATCH (n:Label) WHERE 1 < n.prop < 10 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                                   WHERE(AND(LESS(LITERAL(1), PROPERTY_LOOKUP(dba, "n", property.second)),
                                             LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)))),
                                   RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterWIndex1) {
  // Test MATCH (n:Label) WHERE 1 < n.prop < 10 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, property.second, 1);

  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                                   WHERE(AND(LESS(LITERAL(1), PROPERTY_LOOKUP(dba, "n", property.second)),
                                             LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)))),
                                   RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(
                label, std::vector{ms::PropertyPath{property.second}},
                std::vector{ExpressionRange::Range(Bound{LITERAL(1), memgraph::utils::BoundType::EXCLUSIVE},
                                                   Bound{PARAMETER_LOOKUP(2), memgraph::utils::BoundType::EXCLUSIVE})}),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterNoIndex2) {
  // Test MATCH (n:Label) WHERE 10 >= n.prop > 1 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                                   WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                             GREATER(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)))),
                                   RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterWIndex2) {
  // Test MATCH (n:Label) WHERE 10 >= n.prop > 1 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, property.second, 1);

  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                                   WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                             GREATER(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)))),
                                   RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(
                label, std::vector{ms::PropertyPath{property.second}},
                std::vector{ExpressionRange::Range(Bound{PARAMETER_LOOKUP(2), memgraph::utils::BoundType::EXCLUSIVE},
                                                   Bound{LITERAL(10), memgraph::utils::BoundType::INCLUSIVE})}),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterNoIndex3) {
  // Test MATCH (n:Label) WHERE 10 >= n.prop > 1 AND n.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                         WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                   AND(GREATER(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)),
                                       LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(3))))),
                         RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterWIndex3) {
  // Test MATCH (n:Label) WHERE 10 >= n.prop > 1 AND n.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, property.second, 1);

  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                         WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                   AND(GREATER(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)),
                                       LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(3))))),
                         RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(
                label, std::vector{ms::PropertyPath{property.second}},
                std::vector{ExpressionRange::Range(Bound{PARAMETER_LOOKUP(2), memgraph::utils::BoundType::EXCLUSIVE},
                                                   Bound{LITERAL(10), memgraph::utils::BoundType::INCLUSIVE})}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterNoIndex4) {
  // Test MATCH (n:Label) WHERE 10 >= n.prop < 7 AND n.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                         WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                   AND(LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)),
                                       LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(3))))),
                         RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterWIndex4) {
  // Test MATCH (n:Label) WHERE 10 >= n.prop < 7 AND n.prop < 5 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, property.second, 1);

  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                         WHERE(AND(GREATER_EQ(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                   AND(LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)),
                                       LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(3))))),
                         RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(
      planner.plan(), symbol_table,
      ExpectScanAllByLabelProperties(
          label, std::vector{ms::PropertyPath{property.second}},
          std::vector{ExpressionRange::Range(std::nullopt, Bound{LITERAL(10), memgraph::utils::BoundType::INCLUSIVE})}),
      ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterNoIndex5) {
  // Test MATCH (n:Label) WHERE 10 > n.prop < 7 AND n.prop >= 5 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto property = PROPERTY_PAIR(dba, "prop");

  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                         WHERE(AND(GREATER(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                   AND(LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)),
                                       GREATER_EQ(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(3))))),
                         RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, RangeFilterWIndex5) {
  // Test MATCH (n:Label) WHERE 10 > n.prop < 7 AND n.prop >= 5 RETURN n
  FakeDbAccessor dba;
  const auto label_name = "label";
  const auto label = dba.Label(label_name);
  const auto property = PROPERTY_PAIR(dba, "prop");
  dba.SetIndexCount(label, property.second, 1);

  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label_name))),
                         WHERE(AND(GREATER(LITERAL(10), PROPERTY_LOOKUP(dba, "n", property.second)),
                                   AND(LESS(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(2)),
                                       GREATER_EQ(PROPERTY_LOOKUP(dba, "n", property.second), PARAMETER_LOOKUP(3))))),
                         RETURN("n")));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelProperties(
                label, std::vector{ms::PropertyPath{property.second}},
                std::vector{ExpressionRange::Range(Bound{PARAMETER_LOOKUP(3), memgraph::utils::BoundType::INCLUSIVE},
                                                   Bound{LITERAL(10), memgraph::utils::BoundType::EXCLUSIVE})}),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateQuery) {
  // Test USING PERIODIC COMMIT 1 UNWIND range(1, 3) as x CREATE (n);
  FakeDbAccessor dba;

  auto *query = PERIODIC_QUERY(
      SINGLE_QUERY(UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), AS("x")), CREATE(PATTERN(NODE("n")))),
      COMMIT_FREQUENCY(LITERAL(1)));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectUnwind(), ExpectCreateNode(), ExpectPeriodicCommit(),
            ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateQueryReturn) {
  // Test USING PERIODIC COMMIT 1 UNWIND range(1, 3) as x CREATE (n) RETURN n;
  // this one without periodic commit returns accumulate and we don't want to accumulate
  // because that will create all the deltas in advance and we won't be able to periodically commit
  FakeDbAccessor dba;

  auto *query = PERIODIC_QUERY(
      SINGLE_QUERY(UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), AS("x")), CREATE(PATTERN(NODE("n"))), RETURN("n")),
      COMMIT_FREQUENCY(LITERAL(1)));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectUnwind(), ExpectCreateNode(), ExpectPeriodicCommit(), ExpectProduce());
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateQueryNested) {
  // Test UNWIND range(1, 3) as x CALL { CREATE (n) } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto nexpr_x = NEXPR("x", IDENT("x"));
  auto *subquery = SINGLE_QUERY(CREATE(PATTERN(NODE("n"))));
  auto *query = QUERY(SINGLE_QUERY(UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), nexpr_x),
                                   CALL_PERIODIC_SUBQUERY(subquery, COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectCreateNode(), new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectUnwind(), ExpectPeriodicSubquery(subquery_plan),
            ExpectAccumulate({symbol_table.at(*nexpr_x)}), ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateQueryNestedWith) {
  // Test UNWIND range(1, 3) as x CALL { WITH (n) CREATE (m) } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto nexpr_x = NEXPR("x", IDENT("x"));
  auto *subquery = SINGLE_QUERY(WITH("x", AS("a")), CREATE(PATTERN(NODE("m"))));
  auto *query = QUERY(SINGLE_QUERY(UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), nexpr_x),
                                   CALL_PERIODIC_SUBQUERY(subquery, COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectProduce(), new ExpectCreateNode(), new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectUnwind(), ExpectPeriodicSubquery(subquery_plan),
            ExpectAccumulate({symbol_table.at(*nexpr_x)}), ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateQueryNestedWholeQuery) {
  // Test CALL { UNWIND range(1, 3) as x CREATE (n) } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto *query = QUERY(SINGLE_QUERY(CALL_PERIODIC_SUBQUERY(
      SINGLE_QUERY(UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), AS("x")), CREATE(PATTERN(NODE("n")))),
      COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectUnwind(), new ExpectCreateNode(), new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectPeriodicSubquery(subquery_plan), ExpectAccumulate({}),
            ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicCommitLoadCsv) {
  // Test USING PERIODIC COMMIT 1 LOAD CSV FROM "x" WITH HEADER AS row CREATE (n);
  FakeDbAccessor dba;

  auto *query = PERIODIC_QUERY(SINGLE_QUERY(LOAD_CSV(LITERAL("temp"), "row"), CREATE(PATTERN(NODE("n")))),
                               COMMIT_FREQUENCY(LITERAL(1)));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectLoadCsv(), ExpectCreateNode(), ExpectPeriodicCommit(),
            ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, PeriodicCommitLoadCsvWithCallAtEnd) {
  // Test USING PERIODIC COMMIT 1 LOAD CSV FROM "x" WITH HEADER AS row CALL { CREATE (n) };
  FakeDbAccessor dba;

  auto *query = PERIODIC_QUERY(
      SINGLE_QUERY(LOAD_CSV(LITERAL("temp"), "row"), CALL_SUBQUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n")))))),
      COMMIT_FREQUENCY(LITERAL(1)));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectCreateNode(), new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectLoadCsv(), ExpectApply(subquery_plan), ExpectPeriodicCommit(),
            ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicCommitLoadCsvNested) {
  // Test LOAD CSV FROM "x" WITH HEADER AS row CALL { CREATE (n) } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto ident_row = IDENT("row");
  auto *subquery = SINGLE_QUERY(CREATE(PATTERN(NODE("n"))));
  auto *query = QUERY(SINGLE_QUERY(LOAD_CSV(LITERAL("temp"), ident_row),
                                   CALL_PERIODIC_SUBQUERY(subquery, COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectCreateNode(), new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectLoadCsv(), ExpectPeriodicSubquery(subquery_plan),
            ExpectAccumulate({symbol_table.at(*ident_row)}), ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicCommitLoadCsvNestedWholeQuery) {
  // Test CALL { LOAD CSV FROM "x" WITH HEADER AS row CREATE (n) } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto *query = QUERY(SINGLE_QUERY(CALL_PERIODIC_SUBQUERY(
      SINGLE_QUERY(LOAD_CSV(LITERAL("temp"), "row"), CREATE(PATTERN(NODE("n")))), COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectLoadCsv(), new ExpectCreateNode(), new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectPeriodicSubquery(subquery_plan), ExpectAccumulate({}),
            ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateCallProcedure) {
  // Test USING PERIODIC COMMIT 1 CALL migrate.migrate() YIELD result CREATE (n);
  FakeDbAccessor dba;

  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  auto *query = PERIODIC_QUERY(SINGLE_QUERY(ast_call, CREATE(PATTERN(NODE("n")))), COMMIT_FREQUENCY(LITERAL(1)));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectBasicCallProcedure(), ExpectCreateNode(), ExpectPeriodicCommit(),
            ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateCallProcedureNested) {
  // Test CALL migrate.migrate() YIELD result CALL { CREATE (n) } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  auto *subquery = SINGLE_QUERY(CREATE(PATTERN(NODE("n"))));
  auto *query = QUERY(SINGLE_QUERY(ast_call, CALL_PERIODIC_SUBQUERY(subquery, COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectCreateNode(), new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectBasicCallProcedure(), ExpectPeriodicSubquery(subquery_plan),
            ExpectAccumulate({}), ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicCommitCreateCallProcedureNestedWholeQuery) {
  // Test CALL { CALL migrate.migrate() YIELD result CREATE (n) } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  auto *query = QUERY(SINGLE_QUERY(
      CALL_PERIODIC_SUBQUERY(SINGLE_QUERY(ast_call, CREATE(PATTERN(NODE("n")))), COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> subquery_plan{new ExpectBasicCallProcedure(), new ExpectCreateNode(),
                                           new ExpectEmptyResult()};

  CheckPlan(planner.plan(), symbol_table, ExpectPeriodicSubquery(subquery_plan), ExpectAccumulate({}),
            ExpectEmptyResult());

  DeleteListContent(&subquery_plan);
}

TYPED_TEST(TestPlanner, PeriodicSubqueryWithDeleteCantCombine) {
  // Test MATCH (n) CALL { WITH n DETACH DELETE n } IN TRANSACTIONS OF 1 ROWS;
  FakeDbAccessor dba;

  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), CALL_PERIODIC_SUBQUERY(SINGLE_QUERY(WITH("n"), DELETE(IDENT("n"))),
                                                                           COMMIT_FREQUENCY(LITERAL(1)))));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  ASSERT_THROW(MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query), memgraph::utils::NotYetImplemented);
}

TYPED_TEST(TestPlanner, PeriodicCommitWithDelete) {
  // Test USING PERIODIC COMMIT 1 MATCH (n) DETACH DELETE n;
  FakeDbAccessor dba;

  auto *ast_call = this->storage.template Create<memgraph::query::CallProcedure>();
  auto *query =
      PERIODIC_QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), DELETE(IDENT("n"))), COMMIT_FREQUENCY(LITERAL(1)));

  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectDelete(), ExpectPeriodicCommit(), ExpectEmptyResult());
}

TYPED_TEST(TestPlanner, ORLabelExpressionWithoutBothIndices) {
  // Test MATCH (n:Label1|Label2) RETURN n
  FakeDbAccessor dba;
  auto label1 = dba.Label("Label1");
  [[maybe_unused]] auto label2 = dba.Label("Label2");

  // Create only one index
  dba.SetIndexCount(label1, 1);

  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, ORLabelExpressionWithIndex) {
  // Test MATCH (n:Label1|Label2) RETURN n
  FakeDbAccessor dba;
  auto label1 = dba.Label("Label1");
  auto label2 = dba.Label("Label2");
  auto property = PROPERTY_PAIR(dba, "prop");

  dba.SetIndexCount(label1, 1);
  dba.SetIndexCount(label2, 1);
  dba.SetIndexCount(label1, property.second, 1);  // this index shouldn't be used in this query
  dba.SetIndexCount(label2, property.second, 1);  // this index shouldn't be used in this query

  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabel()};
  CheckPlan(planner.plan(), symbol_table, ExpectUnion(left_subquery_part, right_subquery_part), ExpectDistinct(),
            ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, ORLabelExpressionWithMultipleLabels) {
  // Test MATCH (n:Label1|Label2|Label3) RETURN n
  FakeDbAccessor dba;
  auto label1 = dba.Label("Label1");
  auto label2 = dba.Label("Label2");
  auto label3 = dba.Label("Label3");

  dba.SetIndexCount(label1, 3);
  dba.SetIndexCount(label2, 2);
  dba.SetIndexCount(label3, 1);

  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2", "Label3"}))), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  // expect union of union and scan all by label
  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> first_subquery_plan{new ExpectUnion(left_subquery_part, right_subquery_part),
                                                 new ExpectDistinct()};
  std::list<BaseOpChecker *> second_subquery_plan{new ExpectScanAllByLabel()};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(first_subquery_plan, second_subquery_plan), ExpectDistinct(),
            ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
  DeleteListContent(&first_subquery_plan);
  DeleteListContent(&second_subquery_plan);
}

TYPED_TEST(TestPlanner, ORLabelExpressionWhereClause) {
  // Test MATCH (n) WHERE n:Label1 OR n:Label2 RETURN n
  FakeDbAccessor dba;
  auto label1_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label1")};
  auto label2_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label2")};
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");

  dba.SetIndexCount(label1_id, 1);
  dba.SetIndexCount(label2_id, 1);

  auto node_identifier = IDENT("n");
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      WHERE(OR(LABELS_TEST(node_identifier, label1_ix), LABELS_TEST(node_identifier, label2_ix))), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabel()};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(left_subquery_part, right_subquery_part), ExpectDistinct(),
            ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, ORLabelExpressionWhereClauseMultipleLabels) {
  // Test MATCH (n) WHERE n:Label1 OR n:Label2 OR n:Label3 RETURN n
  FakeDbAccessor dba;
  auto label1_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label1")};
  auto label2_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label2")};
  auto label3_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label3")};
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto label3_id = dba.Label("Label3");

  dba.SetIndexCount(label1_id, 3);
  dba.SetIndexCount(label2_id, 2);
  dba.SetIndexCount(label3_id, 1);

  auto node_identifier = IDENT("n");
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                   WHERE(OR(LABELS_TEST(node_identifier, label1_ix),
                            OR(LABELS_TEST(node_identifier, label2_ix), LABELS_TEST(node_identifier, label3_ix)))),
                   RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> first_subquery_plan{new ExpectUnion(left_subquery_part, right_subquery_part),
                                                 new ExpectDistinct()};
  std::list<BaseOpChecker *> second_subquery_plan{new ExpectScanAllByLabel()};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(first_subquery_plan, second_subquery_plan), ExpectDistinct(),
            ExpectProduce());

  DeleteListContent(&first_subquery_plan);
  DeleteListContent(&second_subquery_plan);
  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, ORLabelExpressionMatchWhereCombination) {
  // Test Match (n:Label1|Label2) WHERE n:Label3 OR n:Label4 RETURN n
  FakeDbAccessor dba;
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto label3_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label3")};
  auto label4_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label4")};
  auto label3_id = dba.Label("Label3");
  auto label4_id = dba.Label("Label4");

  dba.SetIndexCount(label1_id, 1);
  dba.SetIndexCount(label2_id, 1);
  dba.SetIndexCount(label3_id, 2);
  dba.SetIndexCount(label4_id, 2);

  auto node_identifier = IDENT("n");
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))),
      WHERE(OR(LABELS_TEST(node_identifier, label3_ix), LABELS_TEST(node_identifier, label4_ix))), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabel(label1_id)};
  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabel(label2_id)};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(left_subquery_part, right_subquery_part), ExpectDistinct(),
            ExpectFilter(), ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, LabelExpressionCombination) {
  // Test MATCH (n) WHERE n:Label1 OR n:Label2 AND n:Label3 RETURN n -> fallback to scan all and generic filter
  FakeDbAccessor dba;
  auto label1_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label1")};
  auto label2_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label2")};
  auto label3_ix = std::vector<memgraph::query::LabelIx>{this->storage.GetLabelIx("Label3")};
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto label3_id = dba.Label("Label3");

  dba.SetIndexCount(label1_id, 3);
  dba.SetIndexCount(label2_id, 2);
  dba.SetIndexCount(label3_id, 1);

  auto node_identifier = IDENT("n");
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                   WHERE(OR(LABELS_TEST(node_identifier, label1_ix),
                            AND(LABELS_TEST(node_identifier, label2_ix), LABELS_TEST(node_identifier, label3_ix)))),
                   RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, ORLabelExpressionUsingIndexCombination) {
  // Test MATCH (n:Label1|Label2) WHERE n.prop = 1 RETURN n
  FakeDbAccessor dba;
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto property = PROPERTY_PAIR(dba, "prop");

  dba.SetIndexCount(label1_id, 1);
  dba.SetIndexCount(label2_id, 1);
  dba.SetIndexCount(label1_id, property.second, 1);

  auto node_identifier = IDENT("n");
  auto lit_1 = LITERAL(1);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))),
                                   WHERE(EQ(PROPERTY_LOOKUP(dba, "n", property.second), lit_1)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabel()};
  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabelProperties(
      label1_id, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(lit_1)})};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(left_subquery_part, right_subquery_part), ExpectDistinct(),
            ExpectFilter(), ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, ORLabelExpressionUsingOnlyPropertyIndex) {
  // Test MATCH (n:Label1|Label2) WHERE n.prop = 1 RETURN n
  FakeDbAccessor dba;
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto property = PROPERTY_PAIR(dba, "prop");

  dba.SetIndexCount(label1_id, 1);
  dba.SetIndexCount(label2_id, property.second, 1);
  dba.SetIndexCount(label1_id, property.second, 1);

  auto node_identifier = IDENT("n");
  auto lit_1 = LITERAL(1);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))),
                                   WHERE(EQ(PROPERTY_LOOKUP(dba, "n", property.second), lit_1)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabelProperties(
      label2_id, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(lit_1)})};
  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabelProperties(
      label1_id, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(lit_1)})};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(left_subquery_part, right_subquery_part), ExpectDistinct(),
            ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, ORLabelExpressionUsingPropertyIndexNoLabelIndex) {
  // Test MATCH (n:Label1|Label2) RETURN n
  FakeDbAccessor dba;
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto property = PROPERTY_PAIR(dba, "prop");

  // Here no label index is present (but label property index is) hence we should fallback to scan all and filtering
  dba.SetIndexCount(label1_id, property.second, 1);
  dba.SetIndexCount(label2_id, property.second, 1);

  auto node_identifier = IDENT("n");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, ORLabelExpressionMultipleMatchStatementsPropertyIndex) {
  // Test MATCH (n:Label1|Label2) MATCH (n:Label3|Label4) WHERE n.prop = 1 RETURN n
  FakeDbAccessor dba;
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto label3_id = dba.Label("Label3");
  auto label4_id = dba.Label("Label4");
  auto property = PROPERTY_PAIR(dba, "prop");

  dba.SetIndexCount(label1_id, 2);
  dba.SetIndexCount(label2_id, 2);
  dba.SetIndexCount(label3_id, property.second, 1);
  dba.SetIndexCount(label4_id, property.second, 1);
  // Plan should use label property index on Label3 and Label4 because of smaller count

  auto node_identifier = IDENT("n");
  auto lit_1 = LITERAL(1);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))),
                                   MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label3", "Label4"}))),
                                   WHERE(EQ(PROPERTY_LOOKUP(dba, "n", property.second), lit_1)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabelProperties(
      label3_id, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(lit_1)})};
  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabelProperties(
      label4_id, std::vector{ms::PropertyPath{property.second}}, std::vector{ExpressionRange::Equal(lit_1)})};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(left_subquery_part, right_subquery_part), ExpectDistinct(),
            ExpectFilter(), ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, ORLabelsExpressionIndexHints) {
  // Test MATCH (n:Label1|Label2) MATCH (n:Label3|Label4) WHERE n.prop < 2 RETURN n
  FakeDbAccessor dba;
  auto label1_id = dba.Label("Label1");
  auto label2_id = dba.Label("Label2");
  auto label3_id = dba.Label("Label3");
  auto label4_id = dba.Label("Label4");
  auto property = PROPERTY_PAIR(dba, "prop");

  dba.SetIndexCount(label1_id, 5);
  dba.SetIndexCount(label2_id, property.second, 1);
  dba.SetIndexCount(label3_id, property.second, 1);
  dba.SetIndexCount(label4_id, property.second, 1);
  // Plan should use label index on Label1 and do a union with the Label2 property index because of the index hint

  auto index_hint = memgraph::query::IndexHint{.index_type_ = memgraph::query::IndexHint::IndexType::LABEL,
                                               .label_ix_ = this->storage.GetLabelIx("Label1")};
  auto node_identifier = IDENT("n");
  auto lit_2 = LITERAL(2);
  Bound upper_bound(lit_2, Bound::Type::EXCLUSIVE);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label1", "Label2"}))),
                                   MATCH(PATTERN(NODE_WITH_LABELS("n", {"Label3", "Label4"}))),
                                   WHERE(LESS(PROPERTY_LOOKUP(dba, "n", property.second), lit_2)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query, {index_hint});

  std::list<BaseOpChecker *> left_subquery_part{new ExpectScanAllByLabel(label1_id)};
  std::list<BaseOpChecker *> right_subquery_part{new ExpectScanAllByLabelProperties(
      label2_id, std::vector{ms::PropertyPath{property.second}},
      std::vector{ExpressionRange::Range(std::nullopt, Bound{lit_2, memgraph::utils::BoundType::EXCLUSIVE})})};

  CheckPlan(planner.plan(), symbol_table, ExpectUnion(left_subquery_part, right_subquery_part), ExpectDistinct(),
            ExpectFilter(), ExpectProduce());

  DeleteListContent(&left_subquery_part);
  DeleteListContent(&right_subquery_part);
}

TYPED_TEST(TestPlanner, BasicExistsSubquery) {
  FakeDbAccessor dba;

  auto *exists_subquery = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m")))));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EXISTS_SUBQUERY(exists_subquery)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> filter_tree{new ExpectExpand(), new ExpectLimit(), new ExpectEvaluatePatternFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectFilter(std::vector<std::list<BaseOpChecker *>>{filter_tree}), ExpectProduce());

  DeleteListContent(&filter_tree);
}

TYPED_TEST(TestPlanner, ExistsSubqueryMatchWhere) {
  FakeDbAccessor dba;

  auto name = dba.Property("name");
  auto *exists_subquery =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                         WHERE(EQ(PROPERTY_LOOKUP(dba, "n", name), PROPERTY_LOOKUP(dba, "m", name)))));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EXISTS_SUBQUERY(exists_subquery)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> filter_tree{new ExpectExpand(), new ExpectFilter(), new ExpectLimit(),
                                         new ExpectEvaluatePatternFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectFilter(std::vector<std::list<BaseOpChecker *>>{filter_tree}), ExpectProduce());

  DeleteListContent(&filter_tree);
}

TYPED_TEST(TestPlanner, ExistsSubqueryMatchWhereOmitReturn) {
  FakeDbAccessor dba;

  auto name = dba.Property("name");
  auto *exists_subquery =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                         WHERE(EQ(PROPERTY_LOOKUP(dba, "n", name), PROPERTY_LOOKUP(dba, "m", name))), RETURN("n")));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EXISTS_SUBQUERY(exists_subquery)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> filter_tree{new ExpectExpand(), new ExpectFilter(), new ExpectLimit(),
                                         new ExpectEvaluatePatternFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectFilter(std::vector<std::list<BaseOpChecker *>>{filter_tree}), ExpectProduce());

  DeleteListContent(&filter_tree);
}

TYPED_TEST(TestPlanner, ExistsSubqueryWithMatchWhere) {
  FakeDbAccessor dba;

  auto name = dba.Property("name");
  auto *exists_subquery =
      QUERY(SINGLE_QUERY(WITH(LITERAL("Ozzy"), AS("ozzyName")), MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                         WHERE(EQ(PROPERTY_LOOKUP(dba, "n", name), IDENT("ozzyName")))));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EXISTS_SUBQUERY(exists_subquery)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> filter_tree{new ExpectProduce(), new ExpectFilter(), new ExpectExpand(), new ExpectLimit(),
                                         new ExpectEvaluatePatternFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectFilter(std::vector<std::list<BaseOpChecker *>>{filter_tree}), ExpectProduce());

  DeleteListContent(&filter_tree);
}

TYPED_TEST(TestPlanner, ExistsSubqueryWithMatchWhereOnVertexPropety) {
  FakeDbAccessor dba;

  auto name = dba.Property("name");
  auto *exists_subquery =
      QUERY(SINGLE_QUERY(WITH(LITERAL("Ozzy"), AS("ozzyName")), MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                         WHERE(EQ(PROPERTY_LOOKUP(dba, "m", name), IDENT("ozzyName")))));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EXISTS_SUBQUERY(exists_subquery)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> filter_tree{new ExpectProduce(), new ExpectExpand(), new ExpectFilter(), new ExpectLimit(),
                                         new ExpectEvaluatePatternFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectFilter(std::vector<std::list<BaseOpChecker *>>{filter_tree}), ExpectProduce());

  DeleteListContent(&filter_tree);
}

TYPED_TEST(TestPlanner, ExistsSubqueryNested) {
  FakeDbAccessor dba;

  auto name = dba.Property("name");
  auto *nested_exists_subquery = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("m"), EDGE("r2"), NODE("o")))));
  auto *exists_subquery = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))), WHERE(EXISTS_SUBQUERY(nested_exists_subquery))));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EXISTS_SUBQUERY(exists_subquery)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> nested_filter_tree{new ExpectExpand(), new ExpectLimit(),
                                                new ExpectEvaluatePatternFilter()};
  std::list<BaseOpChecker *> filter_tree{new ExpectExpand(),
                                         new ExpectFilter(std::vector<std::list<BaseOpChecker *>>{nested_filter_tree}),
                                         new ExpectLimit(), new ExpectEvaluatePatternFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectFilter(std::vector<std::list<BaseOpChecker *>>{filter_tree}), ExpectProduce());

  DeleteListContent(&nested_filter_tree);
  DeleteListContent(&filter_tree);
}

TYPED_TEST(TestPlanner, ExistsSubqueryWithUnion) {
  FakeDbAccessor dba;

  auto name = dba.Property("name");
  auto *exists_subquery = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r1"), NODE("m1")))),
                                UNION(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r2"), NODE("m2"))))));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), WHERE(EXISTS_SUBQUERY(exists_subquery)), RETURN("n")));
  auto symbol_table = memgraph::query::MakeSymbolTable(query);
  auto planner = MakePlanner<TypeParam>(&dba, this->storage, symbol_table, query);

  std::list<BaseOpChecker *> left_exists_part{new ExpectExpand()};
  std::list<BaseOpChecker *> right_exists_part{new ExpectExpand()};
  std::list<BaseOpChecker *> exists_union_plan{new ExpectUnion(left_exists_part, right_exists_part),
                                               new ExpectDistinct(), new ExpectLimit(),
                                               new ExpectEvaluatePatternFilter()};

  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectFilter(std::vector<std::list<BaseOpChecker *>>{exists_union_plan}), ExpectProduce());

  DeleteListContent(&left_exists_part);
  DeleteListContent(&right_exists_part);
  DeleteListContent(&exists_union_plan);
}

}  // namespace
