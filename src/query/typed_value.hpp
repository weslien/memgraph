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
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "query/path.hpp"
#include "utils/exceptions.hpp"
#include "utils/memory.hpp"
#include "utils/pmr/flat_map.hpp"
#include "utils/pmr/string.hpp"
#include "utils/pmr/vector.hpp"
#include "utils/temporal.hpp"

namespace memgraph::query {

class Graph;  // fwd declare

namespace {
template <typename T>
concept TypedValueValidPrimativeType =
    std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> ||
    std::is_same_v<T, storage::Enum> || std::is_same_v<T, utils::Date> || std::is_same_v<T, utils::LocalTime> ||
    std::is_same_v<T, utils::LocalDateTime> || std::is_same_v<T, utils::ZonedDateTime> ||
    std::is_same_v<T, utils::Duration> || std::is_same_v<T, utils::Duration> || std::is_same_v<T, std::string>;
}

// TODO: Neo4j does overflow checking. Should we also implement it?
/**
 * Stores a query runtime value and its type.
 *
 * Values can be of a number of predefined types that are enumerated in
 * TypedValue::Type. Each such type corresponds to exactly one C++ type.
 *
 * Non-primitive value types perform additional memory allocations. To tune the
 * allocation scheme, each TypedValue stores a MemoryResource for said
 * allocations. When copying and moving TypedValue instances, take care that the
 * appropriate MemoryResource is used.
 */
class TypedValue {
 public:
  /** Custom TypedValue equality function that returns a bool
   * (as opposed to returning TypedValue as the default equality does).
   * This implementation treats two nulls as being equal and null
   * not being equal to everything else.
   */
  struct BoolEqual {
    bool operator()(const TypedValue &left, const TypedValue &right) const;
  };

  /** Hash operator for TypedValue.
   *
   * Not injecting into std
   * due to linking problems. If the implementation is in this header,
   * then it implicitly instantiates TypedValue::Value<T> before
   * explicit instantiation in .cpp file. If the implementation is in
   * the .cpp file, it won't link.
   * TODO: No longer the case as Value<T> was removed.
   */
  struct Hash {
    size_t operator()(const TypedValue &value) const;
  };

  /** A value type. Each type corresponds to exactly one C++ type */
  enum class Type : unsigned {
    Null,
    Bool,
    Int,
    Double,
    String,
    List,
    Map,
    Vertex,
    Edge,
    Path,
    Date,
    LocalTime,
    LocalDateTime,
    ZonedDateTime,
    Duration,
    Graph,
    Function,
    Enum,
    Point2d,
    Point3d
  };

  // TypedValue at this exact moment of compilation is an incomplete type, and
  // the standard says that instantiating a container with an incomplete type
  // invokes undefined behaviour. The libstdc++-8.3.0 we are using supports
  // std::map with incomplete type, but this is still murky territory. Note that
  // since C++17, std::vector is explicitly said to support incomplete types.

  /** Allocator type so that STL containers are aware that we need one */
  using allocator_type = utils::Allocator<TypedValue>;
  using alloc_trait = std::allocator_traits<allocator_type>;

  using TString = utils::pmr::string;
  using TVector = utils::pmr::vector<TypedValue>;
  using TMap = utils::pmr::flat_map<TString, TypedValue>;

  /** Construct a Null value with default utils::NewDeleteResource(). */
  TypedValue() : type_(Type::Null) {}

  /** Construct a Null value with given utils::MemoryResource. */
  explicit TypedValue(utils::MemoryResource *memory) : memory_(memory), type_(Type::Null) {}

  /**
   * Construct a copy of other.
   * utils::MemoryResource is obtained by calling
   * std::allocator_traits<>::select_on_container_copy_construction(other.memory_).
   * Since we use utils::Allocator, which does not propagate, this means that
   * memory_ will be the default utils::NewDeleteResource().
   */
  TypedValue(const TypedValue &other);

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const TypedValue &other, utils::MemoryResource *memory);

  /**
   * Construct with the value of other.
   * utils::MemoryResource is obtained from other. After the move, other will be
   * set to Null.
   */
  TypedValue(TypedValue &&other) noexcept;

  /**
   * Construct with the value of other, but use the given utils::MemoryResource.
   * After the move, other will be set to Null.
   * If `*memory != *other.GetMemoryResource()`, then a copy is made instead of
   * a move.
   */
  TypedValue(TypedValue &&other, utils::MemoryResource *memory);

  explicit TypedValue(bool value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Bool) {
    bool_v = value;
  }

  explicit TypedValue(int value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Int) {
    int_v = value;
  }

  explicit TypedValue(int64_t value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Int) {
    int_v = value;
  }

  explicit TypedValue(double value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Double) {
    double_v = value;
  }

  explicit TypedValue(storage::Enum value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Enum) {
    enum_v = value;
  }

  explicit TypedValue(const utils::Date &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Date) {
    date_v = value;
  }

  explicit TypedValue(const utils::LocalTime &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::LocalTime) {
    local_time_v = value;
  }

  explicit TypedValue(const utils::LocalDateTime &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::LocalDateTime) {
    local_date_time_v = value;
  }

  explicit TypedValue(const utils::ZonedDateTime &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::ZonedDateTime) {
    zoned_date_time_v = value;
  }

  explicit TypedValue(const utils::Duration &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Duration) {
    duration_v = value;
  }

  explicit TypedValue(const storage::Point2d &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Point2d) {
    point_2d_v = value;
  }

  explicit TypedValue(const storage::Point3d &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Point3d) {
    point_3d_v = value;
  }

  // conversion function to storage::PropertyValue
  explicit operator storage::PropertyValue() const;

  // copy constructors for non-primitive types
  explicit TypedValue(const std::string &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(value, memory_);
  }

  explicit TypedValue(const char *value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(value, memory_);
  }

  explicit TypedValue(const std::string_view value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::String) {
    new (&string_v) TString(value, memory_);
  }

  /**
   * Construct a copy of other.
   * utils::MemoryResource is obtained by calling
   * std::allocator_traits<>::
   *     select_on_container_copy_construction(other.get_allocator()).
   * Since we use utils::Allocator, which does not propagate, this means that
   * memory_ will be the default utils::NewDeleteResource().
   */
  explicit TypedValue(const TString &other)
      : TypedValue(other, std::allocator_traits<utils::Allocator<TypedValue>>::select_on_container_copy_construction(
                              other.get_allocator())
                              .GetMemoryResource()) {}

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const TString &other, utils::MemoryResource *memory) : memory_(memory), type_(Type::String) {
    new (&string_v) TString(other, memory_);
  }

  /** Construct a copy using the given utils::MemoryResource */
  explicit TypedValue(const std::vector<TypedValue> &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(value.begin(), value.end(), memory_);
  }

  template <class T>
  requires TypedValueValidPrimativeType<T>
  explicit TypedValue(const std::vector<T> &value, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(value.begin(), value.end(), memory_);
  }

  /**
   * Construct a copy of other.
   * utils::MemoryResource is obtained by calling
   * std::allocator_traits<>::
   *     select_on_container_copy_construction(other.get_allocator()).
   * Since we use utils::Allocator, which does not propagate, this means that
   * memory_ will be the default utils::NewDeleteResource().
   */
  explicit TypedValue(const TVector &other)
      : TypedValue(other, std::allocator_traits<utils::Allocator<TypedValue>>::select_on_container_copy_construction(
                              other.get_allocator())
                              .GetMemoryResource()) {}

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const TVector &value, utils::MemoryResource *memory) : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(value, memory_);
  }

  /** Construct a copy using the given utils::MemoryResource */
  explicit TypedValue(const std::map<std::string, TypedValue> &value,
                      utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Map) {
    std::construct_at(&map_v, memory_);
    for (const auto &kv : value) map_v.emplace(TString(kv.first, memory_), kv.second);
  }

  /**
   * Construct a copy of other.
   * utils::MemoryResource is obtained by calling
   * std::allocator_traits<>::
   *     select_on_container_copy_construction(other.get_allocator()).
   * Since we use utils::Allocator, which does not propagate, this means that
   * memory_ will be the default utils::NewDeleteResource().
   */
  explicit TypedValue(const TMap &other)
      : TypedValue(other, std::allocator_traits<utils::Allocator<TypedValue>>::select_on_container_copy_construction(
                              other.get_allocator())
                              .GetMemoryResource()) {}

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const TMap &value, utils::MemoryResource *memory) : memory_(memory), type_(Type::Map) {
    std::construct_at(&map_v, value, memory_);
  }

  explicit TypedValue(const VertexAccessor &vertex, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Vertex) {
    new (&vertex_v) VertexAccessor(vertex);
  }

  explicit TypedValue(const EdgeAccessor &edge, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Edge) {
    new (&edge_v) EdgeAccessor(edge);
  }

  explicit TypedValue(const Path &path, utils::MemoryResource *memory = utils::NewDeleteResource())
      : memory_(memory), type_(Type::Path) {
    auto *path_ptr = utils::Allocator<Path>(memory_).new_object<Path>(path);
    std::construct_at(&path_v, path_ptr);
  }

  /** Construct a copy using default utils::NewDeleteResource() */
  explicit TypedValue(const storage::PropertyValue &value);

  /** Construct a copy using the given utils::MemoryResource */
  TypedValue(const storage::PropertyValue &value, utils::MemoryResource *memory);

  // move constructors for non-primitive types

  /**
   * Construct with the value of other.
   * utils::MemoryResource is obtained from other. After the move, other will be
   * left in unspecified state.
   */
  explicit TypedValue(TString &&other) noexcept
      : TypedValue(std::move(other), other.get_allocator().GetMemoryResource()) {}

  /**
   * Construct with the value of other and use the given MemoryResource
   * After the move, other will be left in unspecified state.
   */
  TypedValue(TString &&other, utils::MemoryResource *memory) : memory_(memory), type_(Type::String) {
    new (&string_v) TString(std::move(other), memory_);
  }

  /**
   * Perform an element-wise move using default utils::NewDeleteResource().
   * Other will be not be empty, though elements may be Null.
   */
  explicit TypedValue(std::vector<TypedValue> &&other) : TypedValue(std::move(other), utils::NewDeleteResource()) {}

  /**
   * Perform an element-wise move of the other and use the given MemoryResource.
   * Other will be not be left empty, though elements may be Null.
   */
  TypedValue(std::vector<TypedValue> &&other, utils::MemoryResource *memory) : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(memory_);
    list_v.reserve(other.size());
    // std::vector<TypedValue> has std::allocator and there's no move
    // constructor for std::vector using different allocator types. Since
    // std::allocator is not propagated to elements, it is possible that some
    // TypedValue elements have a MemoryResource that is the same as the one we
    // are given. In such a case we would like to move those TypedValue
    // instances, so we use move_iterator.
    list_v.assign(std::make_move_iterator(other.begin()), std::make_move_iterator(other.end()));
  }

  /**
   * Construct with the value of other.
   * utils::MemoryResource is obtained from other. After the move, other will be
   * left empty.
   */
  explicit TypedValue(TVector &&other) noexcept
      : TypedValue(std::move(other), other.get_allocator().GetMemoryResource()) {}

  /**
   * Construct with the value of other and use the given MemoryResource.
   * If `other.get_allocator() != *memory`, this call will perform an
   * element-wise move and other is not guaranteed to be empty.
   */
  TypedValue(TVector &&other, utils::MemoryResource *memory) : memory_(memory), type_(Type::List) {
    new (&list_v) TVector(std::move(other), memory_);
  }

  /**
   * Perform an element-wise move using default utils::NewDeleteResource().
   * Other will not be left empty, i.e. keys will exist but their values may
   * be Null.
   */
  explicit TypedValue(std::map<std::string, TypedValue> &&other)
      : TypedValue(std::move(other), utils::NewDeleteResource()) {}

  /**
   * Perform an element-wise move using the given MemoryResource.
   * Other will not be left empty, i.e. keys will exist but their values may
   * be Null.
   */
  TypedValue(std::map<std::string, TypedValue> &&other, utils::MemoryResource *memory);
  /**
   * Construct with the value of other.
   * utils::MemoryResource is obtained from other. After the move, other will be
   * left empty.
   */
  explicit TypedValue(TMap &&other);
  /**
   * Construct with the value of other and use the given MemoryResource.
   * If `other.get_allocator() != *memory`, this call will perform an
   * element-wise move and other is not guaranteed to be empty, i.e. keys may
   * exist but their values may be Null.
   */
  TypedValue(TMap &&other, utils::MemoryResource *memory);

  explicit TypedValue(VertexAccessor &&vertex, utils::MemoryResource *memory = utils::NewDeleteResource()) noexcept
      : memory_(memory), type_(Type::Vertex) {
    new (&vertex_v) VertexAccessor(std::move(vertex));
  }

  explicit TypedValue(EdgeAccessor &&edge, utils::MemoryResource *memory = utils::NewDeleteResource()) noexcept
      : memory_(memory), type_(Type::Edge) {
    new (&edge_v) EdgeAccessor(std::move(edge));
  }

  /**
   * Construct with the value of path.
   * utils::MemoryResource is obtained from path. After the move, path will be
   * left empty.
   */
  explicit TypedValue(Path &&path);

  /**
   * Construct with the value of path and use the given MemoryResource.
   * If `*path.GetMemoryResource() != *memory`, this call will perform an
   * element-wise move and path is not guaranteed to be empty.
   */
  explicit TypedValue(Path &&path, utils::MemoryResource *memory);

  /**
   * Construct with the value of graph.
   * utils::MemoryResource is obtained from graph. After the move, graph will be
   * left empty.
   */
  explicit TypedValue(Graph &&graph);

  /**
   * Construct with the value of graph and use the given MemoryResource.
   * If `*graph.GetMemoryResource() != *memory`, this call will perform an
   * element-wise move and graph is not guaranteed to be empty.
   */
  TypedValue(Graph &&graph, utils::MemoryResource *memory);

  explicit TypedValue(std::function<void(TypedValue *)> &&other)
      : function_v(std::move(other)), type_(Type::Function) {}

  /**
   * Construct with the value of other.
   * Default utils::NewDeleteResource() is used for allocations. After the move,
   * other will be set to Null.
   */
  explicit TypedValue(storage::PropertyValue &&other);

  /**
   * Construct with the value of other, but use the given utils::MemoryResource.
   * After the move, other will be set to Null.
   */
  TypedValue(storage::PropertyValue &&other, utils::MemoryResource *memory);

  // copy assignment operators
  TypedValue &operator=(const char *);
  TypedValue &operator=(int);
  TypedValue &operator=(bool);
  TypedValue &operator=(int64_t);
  TypedValue &operator=(double);
  TypedValue &operator=(std::string_view);
  TypedValue &operator=(const TVector &);
  TypedValue &operator=(const std::vector<TypedValue> &);
  TypedValue &operator=(const TMap &);
  TypedValue &operator=(const std::map<std::string, TypedValue> &);
  TypedValue &operator=(const VertexAccessor &);
  TypedValue &operator=(const EdgeAccessor &);
  TypedValue &operator=(const Path &);
  TypedValue &operator=(const utils::Date &);
  TypedValue &operator=(const utils::LocalTime &);
  TypedValue &operator=(const utils::LocalDateTime &);
  TypedValue &operator=(const utils::ZonedDateTime &);
  TypedValue &operator=(const utils::Duration &);
  TypedValue &operator=(const storage::Enum &);
  TypedValue &operator=(const std::function<void(TypedValue *)> &);

  /** Copy assign other, utils::MemoryResource of `this` is used */
  TypedValue &operator=(const TypedValue &other);

  /** Move assign other, utils::MemoryResource of `this` is used. */
  TypedValue &operator=(TypedValue &&other) noexcept(false);

  // move assignment operators
  TypedValue &operator=(TString &&);
  TypedValue &operator=(TVector &&);
  TypedValue &operator=(std::vector<TypedValue> &&);
  TypedValue &operator=(TMap &&);
  TypedValue &operator=(std::map<std::string, TypedValue> &&);
  TypedValue &operator=(Path &&);

  ~TypedValue();

  Type type() const { return type_; }

#define DECLARE_VALUE_AND_TYPE_GETTERS_PRIMITIVE(type_param, type_enum, field) \
  /** Gets the value of type field. Throws if value is not field*/             \
  type_param &Value##type_enum();                                              \
  /** Gets the value of type field. Throws if value is not field*/             \
  type_param Value##type_enum() const;                                         \
  /** Checks if it's the value is of the given type */                         \
  bool Is##type_enum() const;                                                  \
  /** Get the value of the type field. Unchecked */                            \
  type_param UnsafeValue##type_enum() const { return field; }

#define DECLARE_VALUE_AND_TYPE_GETTERS(type_param, type_enum, field) \
  /** Gets the value of type field. Throws if value is not field*/   \
  type_param &Value##type_enum();                                    \
  /** Gets the value of type field. Throws if value is not field*/   \
  const type_param &Value##type_enum() const;                        \
  /** Checks if it's the value is of the given type */               \
  bool Is##type_enum() const;                                        \
  /** Get the value of the type field. Unchecked */                  \
  type_param const &UnsafeValue##type_enum() const { return field; }

  DECLARE_VALUE_AND_TYPE_GETTERS_PRIMITIVE(bool, Bool, bool_v)
  DECLARE_VALUE_AND_TYPE_GETTERS_PRIMITIVE(int64_t, Int, int_v)
  DECLARE_VALUE_AND_TYPE_GETTERS_PRIMITIVE(double, Double, double_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(TString, String, string_v)

  DECLARE_VALUE_AND_TYPE_GETTERS(TVector, List, list_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(TMap, Map, map_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(VertexAccessor, Vertex, vertex_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(EdgeAccessor, Edge, edge_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(Path, Path, *path_v)

  DECLARE_VALUE_AND_TYPE_GETTERS(utils::Date, Date, date_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(utils::LocalTime, LocalTime, local_time_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(utils::LocalDateTime, LocalDateTime, local_date_time_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(utils::ZonedDateTime, ZonedDateTime, zoned_date_time_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(utils::Duration, Duration, duration_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(storage::Enum, Enum, enum_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(storage::Point2d, Point2d, point_2d_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(storage::Point3d, Point3d, point_3d_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(Graph, Graph, *graph_v)
  DECLARE_VALUE_AND_TYPE_GETTERS(std::function<void(TypedValue *)>, Function, function_v)

#undef DECLARE_VALUE_AND_TYPE_GETTERS
#undef DECLARE_VALUE_AND_TYPE_GETTERS_PRIMITIVE

  bool ContainsDeleted() const;

  /**  Checks if value is a TypedValue::Null. */
  bool IsNull() const { return type_ == Type::Null; }

  /** Convenience function for checking if this TypedValue is either
   * an integer or double */
  bool IsNumeric() const;

  /** Convenience function for checking if this TypedValue can be converted into
   * storage::PropertyValue */
  bool IsPropertyValue() const;

  utils::MemoryResource *GetMemoryResource() const { return memory_; }

 private:
  // Memory resource for allocations of non primitive values
  utils::MemoryResource *memory_{utils::NewDeleteResource()};

  // storage for the value of the property
  union {
    bool bool_v;
    int64_t int_v;
    double double_v;
    // Since this is used in query runtime, size of union is not critical so
    // string and vector are used instead of pointers. It requires copy of data,
    // but most of algorithms (concatenations, serialisation...) has linear time
    // complexity so it shouldn't be a problem. This is maybe even faster
    // because of data locality.
    TString string_v;
    TVector list_v;
    TMap map_v;
    VertexAccessor vertex_v;
    EdgeAccessor edge_v;
    std::unique_ptr<Path> path_v;
    utils::Date date_v;
    utils::LocalTime local_time_v;
    utils::LocalDateTime local_date_time_v;
    utils::ZonedDateTime zoned_date_time_v;
    utils::Duration duration_v;
    storage::Enum enum_v;
    storage::Point2d point_2d_v;
    storage::Point3d point_3d_v;
    // As the unique_ptr is not allocator aware, it requires special attention when copying or moving graphs
    std::unique_ptr<Graph> graph_v;
    std::function<void(TypedValue *)> function_v;
  };

  /**
   * The Type of property.
   */
  Type type_;
};

/**
 * An exception raised by the TypedValue system. Typically when
 * trying to perform operations (such as addition) on TypedValues
 * of incompatible Types.
 */
class TypedValueException : public utils::BasicException {
 public:
  using utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(TypedValueException)
};

// binary bool operators

/**
 * Perform logical 'and' on TypedValues.
 *
 * If any of the values is false, return false. Otherwise checks if any value is
 * Null and return Null. All other cases return true. The resulting value uses
 * the same MemoryResource as the left hand side arguments.
 *
 * @throw TypedValueException if arguments are not boolean or Null.
 */
TypedValue operator&&(const TypedValue &a, const TypedValue &b);

/**
 * Perform logical 'or' on TypedValues.
 *
 * If any of the values is true, return true. Otherwise checks if any value is
 * Null and return Null. All other cases return false. The resulting value uses
 * the same MemoryResource as the left hand side arguments.
 *
 * @throw TypedValueException if arguments are not boolean or Null.
 */
TypedValue operator||(const TypedValue &a, const TypedValue &b);

/**
 * Logically negate a TypedValue.
 *
 * Negating Null value returns Null. Values other than null raise an exception.
 * The resulting value uses the same MemoryResource as the argument.
 *
 * @throw TypedValueException if TypedValue is not a boolean or Null.
 */
TypedValue operator!(const TypedValue &a);

// binary bool xor, not power operator
// Be careful: since ^ is binary operator and || and && are logical operators
// they have different priority in c++.
TypedValue operator^(const TypedValue &a, const TypedValue &b);

// comparison operators

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * Since each TypedValue may have a different MemoryResource for allocations,
 * the results is allocated using MemoryResource obtained from the left hand
 * side.
 */
TypedValue operator==(const TypedValue &a, const TypedValue &b);

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * Since each TypedValue may have a different MemoryResource for allocations,
 * the results is allocated using MemoryResource obtained from the left hand
 * side.
 */
inline TypedValue operator!=(const TypedValue &a, const TypedValue &b) { return !(a == b); }

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
TypedValue operator<(const TypedValue &a, const TypedValue &b);

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
inline TypedValue operator<=(const TypedValue &a, const TypedValue &b) { return a < b || a == b; }

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
inline TypedValue operator>(const TypedValue &a, const TypedValue &b) { return !(a <= b); }

/**
 * Compare TypedValues and return true, false or Null.
 *
 * Null is returned if either of the two values is Null.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values cannot be compared, i.e. they are
 *        not either Null, numeric or a character string type.
 */
inline TypedValue operator>=(const TypedValue &a, const TypedValue &b) { return !(a < b); }

// arithmetic operators

/**
 * Arithmetically negate a value.
 *
 * If the value is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the argument.
 *
 * @throw TypedValueException if the value is not numeric or Null.
 */
TypedValue operator-(const TypedValue &a);

/**
 * Apply the unary plus operator to a value.
 *
 * If the value is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the argument.
 *
 * @throw TypedValueException if the value is not numeric or Null.
 */
TypedValue operator+(const TypedValue &a);

/**
 * Perform addition or concatenation on two values.
 *
 * Numeric values are summed, while lists and character strings are
 * concatenated. If either value is Null, then Null is returned. The resulting
 * value uses the same MemoryResource as the left hand side argument.
 *
 * @throw TypedValueException if values cannot be summed or concatenated.
 */
TypedValue operator+(const TypedValue &a, const TypedValue &b);

/**
 * Subtract two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null.
 */
TypedValue operator-(const TypedValue &a, const TypedValue &b);

/**
 * Divide two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null, or if
 *        dividing two integer values by zero.
 */
TypedValue operator/(const TypedValue &a, const TypedValue &b);

/**
 * Multiply two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null.
 */
TypedValue operator*(const TypedValue &a, const TypedValue &b);

/**
 * Perform modulo operation on two values.
 *
 * If any of the values is Null, then Null is returned.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null.
 */
TypedValue operator%(const TypedValue &a, const TypedValue &b);

/**
 * Perform an exponentation operation on two values.
 *
 * If any of the values is Null, then Null is returned. The return value
 * is always a floating-point value, even when called with integers.
 * The resulting value uses the same MemoryResource as the left hand side
 * argument.
 *
 * @throw TypedValueException if the values are not numeric or Null.
 */
TypedValue pow(const TypedValue &a, const TypedValue &b);

/** Output the TypedValue::Type value as a string */
std::ostream &operator<<(std::ostream &os, const TypedValue::Type &type);

/** Helper method for extract CRS from possible point types */
inline auto GetCRS(TypedValue const &tv) -> std::optional<storage::CoordinateReferenceSystem> {
  switch (tv.type()) {
    using enum TypedValue::Type;
    case Point2d: {
      return tv.ValuePoint2d().crs();
    }
    case Point3d: {
      return tv.ValuePoint3d().crs();
    }
    default: {
      return std::nullopt;
    }
  }
}

}  // namespace memgraph::query
