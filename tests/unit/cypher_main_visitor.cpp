#include <algorithm>
#include <climits>
#include <string>
#include <unordered_map>
#include <vector>

#include "antlr4-runtime.h"
#include "dbms/dbms.hpp"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "query/context.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/frontend/ast/cypher_main_visitor.hpp"
#include "query/frontend/opencypher/parser.hpp"
#include "query/typed_value.hpp"

namespace {

using namespace query;
using namespace query::frontend;
using query::TypedValue;
using testing::UnorderedElementsAre;
using testing::Pair;

class AstGenerator {
 public:
  AstGenerator(const std::string &query)
      : dbms_(),
        db_accessor_(dbms_.active()),
        context_(Config{}, *db_accessor_),
        query_string_(query),
        parser_(query),
        visitor_(context_),
        query_([&]() {
          visitor_.visit(parser_.tree());
          return visitor_.query();
        }()) {}

  Dbms dbms_;
  std::unique_ptr<GraphDbAccessor> db_accessor_;
  Context context_;
  std::string query_string_;
  ::frontend::opencypher::Parser parser_;
  CypherMainVisitor visitor_;
  Query *query_;
};

TEST(CypherMainVisitorTest, SyntaxException) {
  ASSERT_THROW(AstGenerator("CREATE ()-[*1...2]-()"), SyntaxException);
}

TEST(CypherMainVisitorTest, SyntaxExceptionOnTrailingText) {
  ASSERT_THROW(AstGenerator("RETURN 2 + 2 mirko"), SyntaxException);
}

TEST(CypherMainVisitorTest, PropertyLookup) {
  AstGenerator ast_generator("RETURN n.x");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *property_lookup = dynamic_cast<PropertyLookup *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(property_lookup->expression_);
  auto identifier = dynamic_cast<Identifier *>(property_lookup->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "n");
  ASSERT_EQ(property_lookup->property_,
            ast_generator.db_accessor_->property("x"));
}

TEST(CypherMainVisitorTest, ReturnNamedIdentifier) {
  AstGenerator ast_generator("RETURN var AS var5");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *named_expr = return_clause->named_expressions_[0];
  ASSERT_EQ(named_expr->name_, "var5");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier->name_, "var");
}

TEST(CypherMainVisitorTest, IntegerLiteral) {
  AstGenerator ast_generator("RETURN 42");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<int64_t>(), 42);
}

TEST(CypherMainVisitorTest, IntegerLiteralTooLarge) {
  ASSERT_THROW(AstGenerator("RETURN 10000000000000000000000000"),
               SemanticException);
}

TEST(CypherMainVisitorTest, BooleanLiteralTrue) {
  AstGenerator ast_generator("RETURN TrUe");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<bool>(), true);
}

TEST(CypherMainVisitorTest, BooleanLiteralFalse) {
  AstGenerator ast_generator("RETURN faLSE");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<bool>(), false);
}

TEST(CypherMainVisitorTest, NullLiteral) {
  AstGenerator ast_generator("RETURN nULl");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.type(), TypedValue::Type::Null);
}

TEST(CypherMainVisitorTest, ParenthesizedExpression) {
  AstGenerator ast_generator("RETURN (2)");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, OrOperator) {
  AstGenerator ast_generator("RETURN true Or false oR n");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *or_operator2 = dynamic_cast<OrOperator *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(or_operator2);
  auto *or_operator1 = dynamic_cast<OrOperator *>(or_operator2->expression1_);
  ASSERT_TRUE(or_operator1);
  auto *operand1 = dynamic_cast<Literal *>(or_operator1->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<bool>(), true);
  auto *operand2 = dynamic_cast<Literal *>(or_operator1->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<bool>(), false);
  auto *operand3 = dynamic_cast<Identifier *>(or_operator2->expression2_);
  ASSERT_TRUE(operand3);
  ASSERT_EQ(operand3->name_, "n");
}

TEST(CypherMainVisitorTest, XorOperator) {
  AstGenerator ast_generator("RETURN true xOr false");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *xor_operator = dynamic_cast<XorOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *operand1 = dynamic_cast<Literal *>(xor_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<bool>(), true);
  auto *operand2 = dynamic_cast<Literal *>(xor_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<bool>(), false);
}

TEST(CypherMainVisitorTest, AndOperator) {
  AstGenerator ast_generator("RETURN true and false");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *and_operator = dynamic_cast<AndOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *operand1 = dynamic_cast<Literal *>(and_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<bool>(), true);
  auto *operand2 = dynamic_cast<Literal *>(and_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<bool>(), false);
}

TEST(CypherMainVisitorTest, AdditionSubtractionOperators) {
  AstGenerator ast_generator("RETURN 1 - 2 + 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *addition_operator = dynamic_cast<AdditionOperator *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(addition_operator);
  auto *subtraction_operator =
      dynamic_cast<SubtractionOperator *>(addition_operator->expression1_);
  ASSERT_TRUE(subtraction_operator);
  auto *operand1 = dynamic_cast<Literal *>(subtraction_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 1);
  auto *operand2 = dynamic_cast<Literal *>(subtraction_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 2);
  auto *operand3 = dynamic_cast<Literal *>(addition_operator->expression2_);
  ASSERT_TRUE(operand3);
  ASSERT_EQ(operand3->value_.Value<int64_t>(), 3);
}

TEST(CypherMainVisitorTest, MulitplicationOperator) {
  AstGenerator ast_generator("RETURN 2 * 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *mult_operator = dynamic_cast<MultiplicationOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *operand1 = dynamic_cast<Literal *>(mult_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 = dynamic_cast<Literal *>(mult_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

TEST(CypherMainVisitorTest, DivisionOperator) {
  AstGenerator ast_generator("RETURN 2 / 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *div_operator = dynamic_cast<DivisionOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *operand1 = dynamic_cast<Literal *>(div_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 = dynamic_cast<Literal *>(div_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

TEST(CypherMainVisitorTest, ModOperator) {
  AstGenerator ast_generator("RETURN 2 % 3");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *mod_operator = dynamic_cast<ModOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *operand1 = dynamic_cast<Literal *>(mod_operator->expression1_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 = dynamic_cast<Literal *>(mod_operator->expression2_);
  ASSERT_TRUE(operand2);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

#define CHECK_COMPARISON(TYPE, VALUE1, VALUE2)                             \
  do {                                                                     \
    auto *and_operator = dynamic_cast<AndOperator *>(_operator);           \
    ASSERT_TRUE(and_operator);                                             \
    _operator = and_operator->expression1_;                                \
    auto *cmp_operator = dynamic_cast<TYPE *>(and_operator->expression2_); \
    ASSERT_TRUE(cmp_operator);                                             \
    auto *operand1 = dynamic_cast<Literal *>(cmp_operator->expression1_);  \
    ASSERT_EQ(operand1->value_.Value<int64_t>(), VALUE1);                  \
    auto *operand2 = dynamic_cast<Literal *>(cmp_operator->expression2_);  \
    ASSERT_EQ(operand2->value_.Value<int64_t>(), VALUE2);                  \
  } while (0)

TEST(CypherMainVisitorTest, ComparisonOperators) {
  AstGenerator ast_generator("RETURN 2 = 3 != 4 <> 5 < 6 > 7 <= 8 >= 9");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  Expression *_operator = return_clause->named_expressions_[0]->expression_;
  CHECK_COMPARISON(GreaterEqualOperator, 8, 9);
  CHECK_COMPARISON(LessEqualOperator, 7, 8);
  CHECK_COMPARISON(GreaterOperator, 6, 7);
  CHECK_COMPARISON(LessOperator, 5, 6);
  CHECK_COMPARISON(NotEqualOperator, 4, 5);
  CHECK_COMPARISON(NotEqualOperator, 3, 4);
  auto *cmp_operator = dynamic_cast<EqualOperator *>(_operator);
  ASSERT_TRUE(cmp_operator);
  auto *operand1 = dynamic_cast<Literal *>(cmp_operator->expression1_);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
  auto *operand2 = dynamic_cast<Literal *>(cmp_operator->expression2_);
  ASSERT_EQ(operand2->value_.Value<int64_t>(), 3);
}

#undef CHECK_COMPARISON

TEST(CypherMainVisitorTest, IsNull) {
  AstGenerator ast_generator("RETURN 2 iS NulL");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *is_type_operator = dynamic_cast<IsNullOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *operand1 = dynamic_cast<Literal *>(is_type_operator->expression_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, IsNotNull) {
  AstGenerator ast_generator("RETURN 2 iS nOT NulL");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *not_operator = dynamic_cast<NotOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *is_type_operator =
      dynamic_cast<IsNullOperator *>(not_operator->expression_);
  auto *operand1 = dynamic_cast<Literal *>(is_type_operator->expression_);
  ASSERT_TRUE(operand1);
  ASSERT_EQ(operand1->value_.Value<int64_t>(), 2);
}

TEST(CypherMainVisitorTest, NotOperator) {
  AstGenerator ast_generator("RETURN not true");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *not_operator = dynamic_cast<NotOperator *>(
      return_clause->named_expressions_[0]->expression_);
  auto *operand = dynamic_cast<Literal *>(not_operator->expression_);
  ASSERT_TRUE(operand);
  ASSERT_EQ(operand->value_.Value<bool>(), true);
}

TEST(CypherMainVisitorTest, UnaryMinusPlusOperators) {
  AstGenerator ast_generator("RETURN -+5");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *unary_minus_operator = dynamic_cast<UnaryMinusOperator *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(unary_minus_operator);
  auto *unary_plus_operator =
      dynamic_cast<UnaryPlusOperator *>(unary_minus_operator->expression_);
  ASSERT_TRUE(unary_plus_operator);
  auto *operand = dynamic_cast<Literal *>(unary_plus_operator->expression_);
  ASSERT_TRUE(operand);
  ASSERT_EQ(operand->value_.Value<int64_t>(), 5);
}

TEST(CypherMainVisitorTest, Aggregation) {
  AstGenerator ast_generator("RETURN COUNT(a), MIN(b), MAX(c), SUM(d), AVG(e)");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_EQ(return_clause->named_expressions_.size(), 5);
  Aggregation::Op ops[] = {Aggregation::Op::COUNT, Aggregation::Op::MIN,
                           Aggregation::Op::MAX, Aggregation::Op::SUM,
                           Aggregation::Op::AVG};
  std::string ids[] = {"a", "b", "c", "d", "e"};
  for (int i = 0; i < 5; ++i) {
    auto *aggregation = dynamic_cast<Aggregation *>(
        return_clause->named_expressions_[i]->expression_);
    ASSERT_TRUE(aggregation);
    ASSERT_EQ(aggregation->op_, ops[i]);
    auto *identifier = dynamic_cast<Identifier *>(aggregation->expression_);
    ASSERT_TRUE(identifier);
    ASSERT_EQ(identifier->name_, ids[i]);
  }
}

TEST(CypherMainVisitorTest, UndefinedFunction) {
  ASSERT_THROW(AstGenerator("RETURN "
                            "IHopeWeWillNeverHaveAwesomeMemgraphProcedureWithS"
                            "uchALongAndAwesomeNameSinceThisTestWouldFail(1)"),
               SemanticException);
}

TEST(CypherMainVisitorTest, Function) {
  AstGenerator ast_generator("RETURN abs(n, 2)");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_EQ(return_clause->named_expressions_.size(), 1);
  auto *function = dynamic_cast<Function *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(function);
  ASSERT_TRUE(function->function_);
  // Check if function is abs.
  ASSERT_EQ(function->function_({-2}).Value<int64_t>(), 2);
  ASSERT_EQ(function->arguments_.size(), 2);
}

TEST(CypherMainVisitorTest, StringLiteralDoubleQuotes) {
  AstGenerator ast_generator("RETURN \"mi'rko\"");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), "mi'rko");
}

TEST(CypherMainVisitorTest, StringLiteralSingleQuotes) {
  AstGenerator ast_generator("RETURN 'mi\"rko'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), "mi\"rko");
}

TEST(CypherMainVisitorTest, StringLiteralEscapedChars) {
  AstGenerator ast_generator(
      "RETURN '\\\\\\'\\\"\\b\\B\\f\\F\\n\\N\\r\\R\\t\\T'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), "\\'\"\b\b\f\f\n\n\r\r\t\t");
}

TEST(CypherMainVisitorTest, StringLiteralEscapedUtf16) {
  AstGenerator ast_generator("RETURN '\\u221daaa\\U221daaa'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(), u8"\u221daaa\u221daaa");
}

TEST(CypherMainVisitorTest, StringLiteralEscapedUtf32) {
  AstGenerator ast_generator("RETURN '\\u0001F600aaaa\\U0001F600aaaaaaaa'");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<std::string>(),
            u8"\U0001F600aaaa\U0001F600aaaaaaaa");
}

TEST(CypherMainVisitorTest, DoubleLiteral) {
  AstGenerator ast_generator("RETURN 3.5");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<double>(), 3.5);
}

TEST(CypherMainVisitorTest, DoubleLiteralExponent) {
  AstGenerator ast_generator("RETURN 5e-1");
  auto *query = ast_generator.query_;
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  auto *literal = dynamic_cast<Literal *>(
      return_clause->named_expressions_[0]->expression_);
  ASSERT_TRUE(literal);
  ASSERT_EQ(literal->value_.Value<double>(), 0.5);
}

TEST(CypherMainVisitorTest, NodePattern) {
  AstGenerator ast_generator(
      "MATCH (:label1:label2:label3 {a : 5, b : 10}) RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  ASSERT_FALSE(match->where_);
  ASSERT_EQ(match->patterns_.size(), 1U);
  ASSERT_TRUE(match->patterns_[0]);
  ASSERT_EQ(match->patterns_[0]->atoms_.size(), 1U);
  auto node = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node);
  ASSERT_TRUE(node->identifier_);
  ASSERT_EQ(node->identifier_->name_,
            CypherMainVisitor::kAnonPrefix + std::to_string(1));
  ASSERT_THAT(node->labels_, UnorderedElementsAre(
                                 ast_generator.db_accessor_->label("label1"),
                                 ast_generator.db_accessor_->label("label2"),
                                 ast_generator.db_accessor_->label("label3")));
  std::unordered_map<GraphDbTypes::Property, int64_t> properties;
  for (auto x : node->properties_) {
    auto *literal = dynamic_cast<Literal *>(x.second);
    ASSERT_TRUE(literal);
    ASSERT_TRUE(literal->value_.type() == TypedValue::Type::Int);
    properties[x.first] = literal->value_.Value<int64_t>();
  }
  ASSERT_THAT(properties,
              UnorderedElementsAre(
                  Pair(ast_generator.db_accessor_->property("a"), 5),
                  Pair(ast_generator.db_accessor_->property("b"), 10)));
}

TEST(CypherMainVisitorTest, NodePatternIdentifier) {
  AstGenerator ast_generator("MATCH (var) RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_FALSE(match->where_);
  auto node = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node->identifier_);
  ASSERT_EQ(node->identifier_->name_, "var");
  ASSERT_THAT(node->labels_, UnorderedElementsAre());
  ASSERT_THAT(node->properties_, UnorderedElementsAre());
}

TEST(CypherMainVisitorTest, RelationshipPatternNoDetails) {
  AstGenerator ast_generator("MATCH ()--() RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_FALSE(match->where_);
  ASSERT_EQ(match->patterns_.size(), 1U);
  ASSERT_TRUE(match->patterns_[0]);
  ASSERT_EQ(match->patterns_[0]->atoms_.size(), 3U);
  auto *node1 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node1);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_TRUE(edge);
  auto *node2 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[2]);
  ASSERT_TRUE(node2);
  ASSERT_EQ(edge->direction_, EdgeAtom::Direction::BOTH);
  ASSERT_TRUE(edge->identifier_);
  ASSERT_THAT(edge->identifier_->name_,
              CypherMainVisitor::kAnonPrefix + std::to_string(2));
}

// PatternPart in braces.
TEST(CypherMainVisitorTest, PatternPartBraces) {
  AstGenerator ast_generator("MATCH ((()--())) RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_FALSE(match->where_);
  ASSERT_EQ(match->patterns_.size(), 1U);
  ASSERT_TRUE(match->patterns_[0]);
  ASSERT_EQ(match->patterns_[0]->atoms_.size(), 3U);
  auto *node1 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node1);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_TRUE(edge);
  auto *node2 = dynamic_cast<NodeAtom *>(match->patterns_[0]->atoms_[2]);
  ASSERT_TRUE(node2);
  ASSERT_EQ(edge->direction_, EdgeAtom::Direction::BOTH);
  ASSERT_TRUE(edge->identifier_);
  ASSERT_THAT(edge->identifier_->name_,
              CypherMainVisitor::kAnonPrefix + std::to_string(2));
}

TEST(CypherMainVisitorTest, RelationshipPatternDetails) {
  AstGenerator ast_generator(
      "MATCH ()<-[:type1|type2 {a : 5, b : 10}]-() RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_FALSE(match->where_);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_EQ(edge->direction_, EdgeAtom::Direction::LEFT);
  ASSERT_THAT(
      edge->edge_types_,
      UnorderedElementsAre(ast_generator.db_accessor_->edge_type("type1"),
                           ast_generator.db_accessor_->edge_type("type2")));
  std::unordered_map<GraphDbTypes::Property, int64_t> properties;
  for (auto x : edge->properties_) {
    auto *literal = dynamic_cast<Literal *>(x.second);
    ASSERT_TRUE(literal);
    ASSERT_TRUE(literal->value_.type() == TypedValue::Type::Int);
    properties[x.first] = literal->value_.Value<int64_t>();
  }
  ASSERT_THAT(properties,
              UnorderedElementsAre(
                  Pair(ast_generator.db_accessor_->property("a"), 5),
                  Pair(ast_generator.db_accessor_->property("b"), 10)));
}

TEST(CypherMainVisitorTest, RelationshipPatternVariable) {
  AstGenerator ast_generator("MATCH ()-[var]->() RETURN 1");
  auto *query = ast_generator.query_;
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_FALSE(match->where_);
  auto *edge = dynamic_cast<EdgeAtom *>(match->patterns_[0]->atoms_[1]);
  ASSERT_EQ(edge->direction_, EdgeAtom::Direction::RIGHT);
  ASSERT_TRUE(edge->identifier_);
  ASSERT_THAT(edge->identifier_->name_, "var");
}

// // Relationship with unbounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternUnbounded) {
//   ParserTables parser("CREATE ()-[*]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 1,
//                        LLONG_MAX);
// }
//
// // Relationship with lower bounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternLowerBounded) {
//   ParserTables parser("CREATE ()-[*5..]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 5,
//                        LLONG_MAX);
// }
//
// // Relationship with upper bounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternUpperBounded) {
//   ParserTables parser("CREATE ()-[*..10]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 1, 10);
// }
//
// // Relationship with lower and upper bounded variable range.
// TEST(CypherMainVisitorTest, RelationshipPatternLowerUpperBounded) {
//   ParserTables parser("CREATE ()-[*5..10]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 5, 10);
// }
//
// // Relationship with fixed number of edges.
// TEST(CypherMainVisitorTest, RelationshipPatternFixedRange) {
//   ParserTables parser("CREATE ()-[*10]-()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 0U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   CompareRelationships(*parser.relationships_.begin(),
//                        Relationship::Direction::BOTH, {}, {}, true, 10, 10);
// }
//
//
// // PatternPart with variable.
// TEST(CypherMainVisitorTest, PatternPartVariable) {
//   ParserTables parser("CREATE var=()--()");
//   ASSERT_EQ(parser.identifiers_map_.size(), 1U);
//   ASSERT_EQ(parser.pattern_parts_.size(), 1U);
//   ASSERT_EQ(parser.relationships_.size(), 1U);
//   ASSERT_EQ(parser.nodes_.size(), 2U);
//   ASSERT_EQ(parser.pattern_parts_.begin()->second.nodes.size(), 2U);
//   ASSERT_EQ(parser.pattern_parts_.begin()->second.relationships.size(), 1U);
//   ASSERT_NE(parser.identifiers_map_.find("var"),
//   parser.identifiers_map_.end());
//   auto output_identifier = parser.identifiers_map_["var"];
//   ASSERT_NE(parser.pattern_parts_.find(output_identifier),
//             parser.pattern_parts_.end());
// }

TEST(CypherMainVisitorTest, ReturnUnanemdIdentifier) {
  AstGenerator ast_generator("RETURN var");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *return_clause = dynamic_cast<Return *>(query->clauses_[0]);
  ASSERT_TRUE(return_clause);
  ASSERT_EQ(return_clause->named_expressions_.size(), 1U);
  auto *named_expr = return_clause->named_expressions_[0];
  ASSERT_TRUE(named_expr);
  ASSERT_EQ(named_expr->name_, "var");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "var");
}

TEST(CypherMainVisitorTest, Create) {
  AstGenerator ast_generator("CREATE (n)");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *create = dynamic_cast<Create *>(query->clauses_[0]);
  ASSERT_TRUE(create);
  ASSERT_EQ(create->patterns_.size(), 1U);
  ASSERT_TRUE(create->patterns_[0]);
  ASSERT_EQ(create->patterns_[0]->atoms_.size(), 1U);
  auto node = dynamic_cast<NodeAtom *>(create->patterns_[0]->atoms_[0]);
  ASSERT_TRUE(node);
  ASSERT_TRUE(node->identifier_);
  ASSERT_EQ(node->identifier_->name_, "n");
}

TEST(CypherMainVisitorTest, Delete) {
  AstGenerator ast_generator("DELETE n, m");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *del = dynamic_cast<Delete *>(query->clauses_[0]);
  ASSERT_TRUE(del);
  ASSERT_FALSE(del->detach_);
  ASSERT_EQ(del->expressions_.size(), 2U);
  auto *identifier1 = dynamic_cast<Identifier *>(del->expressions_[0]);
  ASSERT_TRUE(identifier1);
  ASSERT_EQ(identifier1->name_, "n");
  auto *identifier2 = dynamic_cast<Identifier *>(del->expressions_[1]);
  ASSERT_TRUE(identifier2);
  ASSERT_EQ(identifier2->name_, "m");
}

TEST(CypherMainVisitorTest, DeleteDetach) {
  AstGenerator ast_generator("DETACH DELETE n");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 1U);
  auto *del = dynamic_cast<Delete *>(query->clauses_[0]);
  ASSERT_TRUE(del);
  ASSERT_TRUE(del->detach_);
  ASSERT_EQ(del->expressions_.size(), 1U);
  auto *identifier1 = dynamic_cast<Identifier *>(del->expressions_[0]);
  ASSERT_TRUE(identifier1);
  ASSERT_EQ(identifier1->name_, "n");
}

TEST(CypherMainVisitorTest, MatchWhere) {
  AstGenerator ast_generator("MATCH (n) WHERE m RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *match = dynamic_cast<Match *>(query->clauses_[0]);
  ASSERT_TRUE(match);
  ASSERT_TRUE(match->where_);
  auto *identifier = dynamic_cast<Identifier *>(match->where_->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "m");
}

TEST(CypherMainVisitorTest, Set) {
  AstGenerator ast_generator("SET a.x = b, c = d, e += f, g : h : i ");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 4U);

  {
    auto *set_property = dynamic_cast<SetProperty *>(query->clauses_[0]);
    ASSERT_TRUE(set_property);
    ASSERT_TRUE(set_property->property_lookup_);
    auto *identifier1 =
        dynamic_cast<Identifier *>(set_property->property_lookup_->expression_);
    ASSERT_TRUE(identifier1);
    ASSERT_EQ(identifier1->name_, "a");
    ASSERT_EQ(set_property->property_lookup_->property_,
              ast_generator.db_accessor_->property("x"));
    auto *identifier2 = dynamic_cast<Identifier *>(set_property->expression_);
    ASSERT_EQ(identifier2->name_, "b");
  }

  {
    auto *set_properties_assignment =
        dynamic_cast<SetProperties *>(query->clauses_[1]);
    ASSERT_TRUE(set_properties_assignment);
    ASSERT_FALSE(set_properties_assignment->update_);
    ASSERT_TRUE(set_properties_assignment->identifier_);
    ASSERT_EQ(set_properties_assignment->identifier_->name_, "c");
    auto *identifier =
        dynamic_cast<Identifier *>(set_properties_assignment->expression_);
    ASSERT_EQ(identifier->name_, "d");
  }

  {
    auto *set_properties_update =
        dynamic_cast<SetProperties *>(query->clauses_[2]);
    ASSERT_TRUE(set_properties_update);
    ASSERT_TRUE(set_properties_update->update_);
    ASSERT_TRUE(set_properties_update->identifier_);
    ASSERT_EQ(set_properties_update->identifier_->name_, "e");
    auto *identifier =
        dynamic_cast<Identifier *>(set_properties_update->expression_);
    ASSERT_EQ(identifier->name_, "f");
  }

  {
    auto *set_labels = dynamic_cast<SetLabels *>(query->clauses_[3]);
    ASSERT_TRUE(set_labels);
    ASSERT_TRUE(set_labels->identifier_);
    ASSERT_EQ(set_labels->identifier_->name_, "g");
    ASSERT_THAT(set_labels->labels_,
                UnorderedElementsAre(ast_generator.db_accessor_->label("h"),
                                     ast_generator.db_accessor_->label("i")));
  }
}

TEST(CypherMainVisitorTest, Remove) {
  AstGenerator ast_generator("REMOVE a.x, g : h : i");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);

  {
    auto *remove_property = dynamic_cast<RemoveProperty *>(query->clauses_[0]);
    ASSERT_TRUE(remove_property);
    ASSERT_TRUE(remove_property->property_lookup_);
    auto *identifier1 = dynamic_cast<Identifier *>(
        remove_property->property_lookup_->expression_);
    ASSERT_TRUE(identifier1);
    ASSERT_EQ(identifier1->name_, "a");
    ASSERT_EQ(remove_property->property_lookup_->property_,
              ast_generator.db_accessor_->property("x"));
  }
  {
    auto *remove_labels = dynamic_cast<RemoveLabels *>(query->clauses_[1]);
    ASSERT_TRUE(remove_labels);
    ASSERT_TRUE(remove_labels->identifier_);
    ASSERT_EQ(remove_labels->identifier_->name_, "g");
    ASSERT_THAT(remove_labels->labels_,
                UnorderedElementsAre(ast_generator.db_accessor_->label("h"),
                                     ast_generator.db_accessor_->label("i")));
  }
}

TEST(CypherMainVisitorTest, With) {
  AstGenerator ast_generator("WITH n AS m RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *with = dynamic_cast<With *>(query->clauses_[0]);
  ASSERT_TRUE(with);
  ASSERT_FALSE(with->where_);
  ASSERT_EQ(with->named_expressions_.size(), 1U);
  auto *named_expr = with->named_expressions_[0];
  ASSERT_EQ(named_expr->name_, "m");
  auto *identifier = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier->name_, "n");
}

TEST(CypherMainVisitorTest, WithWhere) {
  AstGenerator ast_generator("WITH n AS m WHERE k RETURN 1");
  auto *query = ast_generator.query_;
  ASSERT_EQ(query->clauses_.size(), 2U);
  auto *with = dynamic_cast<With *>(query->clauses_[0]);
  ASSERT_TRUE(with);
  ASSERT_TRUE(with->where_);
  auto *identifier = dynamic_cast<Identifier *>(with->where_->expression_);
  ASSERT_TRUE(identifier);
  ASSERT_EQ(identifier->name_, "k");
  ASSERT_EQ(with->named_expressions_.size(), 1U);
  auto *named_expr = with->named_expressions_[0];
  ASSERT_EQ(named_expr->name_, "m");
  auto *identifier2 = dynamic_cast<Identifier *>(named_expr->expression_);
  ASSERT_EQ(identifier2->name_, "n");
}

TEST(CypherMainVisitorTest, ClausesOrdering) {
  // Obviously some of the ridiculous combinations don't fail here, but they
  // will fail in semantic analysis or they make perfect sense AS a part of
  // bigger query.
  AstGenerator("RETURN 1");
  ASSERT_THROW(AstGenerator("RETURN 1 RETURN 1"), SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 MATCH (n) RETURN n"), SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 DELETE n"), SemanticException);
  ASSERT_THROW(AstGenerator("RETURN 1 WITH n AS m RETURN 1"),
               SemanticException);

  AstGenerator("CREATE (n)");
  ASSERT_THROW(AstGenerator("SET n:x MATCH (n) RETURN n"), SemanticException);
  AstGenerator("REMOVE n.x SET n.x = 1");
  AstGenerator("REMOVE n:L RETURN n");
  AstGenerator("SET n.x = 1 WITH n AS m RETURN m");

  ASSERT_THROW(AstGenerator("MATCH (n)"), SemanticException);
  AstGenerator("MATCH (n) MATCH (n) RETURN n");
  AstGenerator("MATCH (n) SET n = m");
  AstGenerator("MATCH (n) RETURN n");
  AstGenerator("MATCH (n) WITH n AS m RETURN m");

  ASSERT_THROW(AstGenerator("WITH 1 AS n"), SemanticException);
  AstGenerator("WITH 1 AS n WITH n AS m RETURN m");
  AstGenerator("WITH 1 AS n RETURN n");
  AstGenerator("WITH 1 AS n SET n += m");
  AstGenerator("WITH 1 AS n MATCH (n) RETURN n");
}
}
