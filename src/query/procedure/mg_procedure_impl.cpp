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

#include "query/procedure/mg_procedure_impl.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "dbms/dbms_handler.hpp"
#include "flags/experimental.hpp"
#include "flags/run_time_configurable.hpp"
#include "glue/auth.hpp"
#include "license/license.hpp"
#include "mg_procedure.h"
#include "module.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/interpreter.hpp"
#include "query/interpreter_context.hpp"
#include "query/procedure/cypher_types.hpp"
#include "query/procedure/fmt.hpp"
#include "query/procedure/mg_procedure_helpers.hpp"
#include "query/stream/common.hpp"
#include "query/string_helpers.hpp"
#include "query/typed_value.hpp"
#include "storage/v2/edge_accessor.hpp"
#include "storage/v2/id_types.hpp"
#include "storage/v2/indices/text_index.hpp"
#include "storage/v2/indices/vector_edge_index.hpp"
#include "storage/v2/indices/vector_index.hpp"
#include "storage/v2/indices/vector_index_utils.hpp"
#include "storage/v2/property_value.hpp"
#include "storage/v2/storage_mode.hpp"
#include "storage/v2/vertex_accessor.hpp"
#include "storage/v2/view.hpp"
#include "utils/algorithm.hpp"
#include "utils/concepts.hpp"
#include "utils/logging.hpp"
#include "utils/math.hpp"
#include "utils/memory.hpp"
#include "utils/memory_tracker.hpp"
#include "utils/string.hpp"
#include "utils/temporal.hpp"
#include "utils/variant_helpers.hpp"

#include <mutex>

// This file contains implementation of top level C API functions, but this is
// all actually part of memgraph::query::procedure. So use that namespace for simplicity.
// NOLINTNEXTLINE(google-build-using-namespace)
using namespace memgraph::query::procedure;

namespace {

void *MgpAlignedAllocImpl(memgraph::utils::MemoryResource &memory, const size_t size_in_bytes, const size_t alignment) {
  if (size_in_bytes == 0U || !memgraph::utils::IsPow2(alignment)) return nullptr;
  // Simplify alignment by always using values greater or equal to max_align.
  const size_t alloc_align = std::max(alignment, alignof(std::max_align_t));
  // Allocate space for header containing size & alignment info.
  const size_t header_size = sizeof(size_in_bytes) + sizeof(alloc_align);
  // We need to return the `data` pointer aligned to the requested alignment.
  // Since we request the initial memory to be aligned to `alloc_align`, we can
  // just allocate an additional multiple of `alloc_align` of bytes such that
  // the header fits. `data` will then be aligned after this multiple of bytes.
  static_assert(std::is_same_v<size_t, uint64_t>);
  const auto maybe_bytes_for_header = memgraph::utils::RoundUint64ToMultiple(header_size, alloc_align);
  if (!maybe_bytes_for_header) return nullptr;
  const size_t bytes_for_header = *maybe_bytes_for_header;
  const size_t alloc_size = bytes_for_header + size_in_bytes;
  if (alloc_size < size_in_bytes) return nullptr;

  void *ptr = memory.allocate(alloc_size, alloc_align);
  char *data = reinterpret_cast<char *>(ptr) + bytes_for_header;
  std::memcpy(data - sizeof(size_in_bytes), &size_in_bytes, sizeof(size_in_bytes));
  std::memcpy(data - sizeof(size_in_bytes) - sizeof(alloc_align), &alloc_align, sizeof(alloc_align));
  return data;
}

void MgpFreeImpl(memgraph::utils::MemoryResource &memory, void *const p) noexcept {
  try {
    if (!p) return;
    char *const data = reinterpret_cast<char *>(p);
    // Read the header containing size & alignment info.
    size_t size_in_bytes{};
    std::memcpy(&size_in_bytes, data - sizeof(size_in_bytes), sizeof(size_in_bytes));
    size_t alloc_align{};
    std::memcpy(&alloc_align, data - sizeof(size_in_bytes) - sizeof(alloc_align), sizeof(alloc_align));
    // Reconstruct how many bytes we allocated on top of the original request.
    // We need not check allocation request overflow, since we did so already in
    // mgp_aligned_alloc.
    const size_t header_size = sizeof(size_in_bytes) + sizeof(alloc_align);
    const size_t bytes_for_header = *memgraph::utils::RoundUint64ToMultiple(header_size, alloc_align);
    const size_t alloc_size = bytes_for_header + size_in_bytes;
    // Get the original ptr we allocated.
    void *const original_ptr = data - bytes_for_header;
    memory.deallocate(original_ptr, alloc_size, alloc_align);
  } catch (const memgraph::utils::BasicException &be) {
    spdlog::error("BasicException during the release of memory for query modules: {}", be.what());
  } catch (const std::exception &e) {
    spdlog::error("std::exception during the release of memory for query modules: {}", e.what());
  } catch (...) {
    spdlog::error("Unexpected throw during the release of memory for query modules");
  }
}
struct DeletedObjectException : public memgraph::utils::BasicException {
  using memgraph::utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(DeletedObjectException)
};

struct KeyAlreadyExistsException : public memgraph::utils::BasicException {
  using memgraph::utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(KeyAlreadyExistsException)
};

struct InsufficientBufferException : public memgraph::utils::BasicException {
  using memgraph::utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(InsufficientBufferException)
};

struct ImmutableObjectException : public memgraph::utils::BasicException {
  using memgraph::utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(ImmutableObjectException)
};

struct ValueConversionException : public memgraph::utils::BasicException {
  using memgraph::utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(ValueConversionException)
};

struct SerializationException : public memgraph::utils::BasicException {
  using memgraph::utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(SerializationException)
};

struct AuthorizationException : public memgraph::utils::BasicException {
  using memgraph::utils::BasicException::BasicException;
  SPECIALIZE_GET_EXCEPTION_NAME(AuthorizationException)
};

template <typename TFunc, typename TReturn>
concept ReturnsType = std::same_as<std::invoke_result_t<TFunc>, TReturn>;

template <typename TFunc>
concept ReturnsVoid = ReturnsType<TFunc, void>;

template <ReturnsVoid TFunc>
void WrapExceptionsHelper(TFunc &&func) {
  std::forward<TFunc>(func)();
}

template <typename TFunc, typename TReturn = std::invoke_result_t<TFunc>>
void WrapExceptionsHelper(TFunc &&func, TReturn *result) {
  *result = {};
  *result = std::forward<TFunc>(func)();
}

template <typename TFunc, typename... Args>
[[nodiscard]] mgp_error WrapExceptions(TFunc &&func, Args &&...args) noexcept {
  static_assert(sizeof...(args) <= 1, "WrapExceptions should have only one or zero parameter!");
  try {
    memgraph::utils::MemoryTracker::OutOfMemoryExceptionEnabler oom_enabler;
    WrapExceptionsHelper(std::forward<TFunc>(func), std::forward<Args>(args)...);
  } catch (const DeletedObjectException &neoe) {
    spdlog::error("Deleted object error during mg API call: {}", neoe.what());
    return mgp_error::MGP_ERROR_DELETED_OBJECT;
  } catch (const KeyAlreadyExistsException &kaee) {
    spdlog::error("Key already exists error during mg API call: {}", kaee.what());
    return mgp_error::MGP_ERROR_KEY_ALREADY_EXISTS;
  } catch (const InsufficientBufferException &ibe) {
    spdlog::error("Insufficient buffer error during mg API call: {}", ibe.what());
    return mgp_error::MGP_ERROR_INSUFFICIENT_BUFFER;
  } catch (const ImmutableObjectException &ioe) {
    spdlog::error("Immutable object error during mg API call: {}", ioe.what());
    return mgp_error::MGP_ERROR_IMMUTABLE_OBJECT;
  } catch (const ValueConversionException &vce) {
    spdlog::error("Value converion error during mg API call: {}", vce.what());
    return mgp_error::MGP_ERROR_VALUE_CONVERSION;
  } catch (const SerializationException &se) {
    spdlog::error("Serialization error during mg API call: {}", se.what());
    return mgp_error::MGP_ERROR_SERIALIZATION_ERROR;
  } catch (const AuthorizationException &ae) {
    spdlog::error("Authorization error during mg API call: {}", ae.what());
    return mgp_error::MGP_ERROR_AUTHORIZATION_ERROR;
  } catch (const std::bad_alloc &bae) {
    spdlog::error("Memory allocation error during mg API call: {}", bae.what());
    return mgp_error::MGP_ERROR_UNABLE_TO_ALLOCATE;
  } catch (const memgraph::utils::OutOfMemoryException &oome) {
    [[maybe_unused]] auto blocker = memgraph::utils::MemoryTracker::OutOfMemoryExceptionBlocker{};
    spdlog::error("Memory limit exceeded during mg API call: {}", oome.what());
    return mgp_error::MGP_ERROR_UNABLE_TO_ALLOCATE;
  } catch (const std::out_of_range &oore) {
    spdlog::error("Out of range error during mg API call: {}", oore.what());
    return mgp_error::MGP_ERROR_OUT_OF_RANGE;
  } catch (const std::invalid_argument &iae) {
    spdlog::error("Invalid argument error during mg API call: {}", iae.what());
    return mgp_error::MGP_ERROR_INVALID_ARGUMENT;
  } catch (const std::logic_error &lee) {
    spdlog::error("Logic error during mg API call: {}", lee.what());
    return mgp_error::MGP_ERROR_LOGIC_ERROR;
  } catch (const memgraph::utils::temporal::InvalidArgumentException &e) {
    spdlog::error("Invalid argument was sent to an mg API call for temporal types: {}", e.what());
    return mgp_error::MGP_ERROR_INVALID_ARGUMENT;
  } catch (const std::exception &e) {
    spdlog::error("Unexpected error during mg API call: {}", e.what());
    return mgp_error::MGP_ERROR_UNKNOWN_ERROR;
  } catch (...) {
    spdlog::error("Unexpected error during mg API call");
    return mgp_error::MGP_ERROR_UNKNOWN_ERROR;
  }
  return mgp_error::MGP_ERROR_NO_ERROR;
}

// Graph mutations
bool MgpGraphIsMutable(const mgp_graph &graph) noexcept {
  return graph.view == memgraph::storage::View::NEW && graph.ctx != nullptr;
}

bool MgpVertexIsMutable(const mgp_vertex &vertex) { return MgpGraphIsMutable(*vertex.graph); }

bool MgpEdgeIsMutable(const mgp_edge &edge) { return MgpVertexIsMutable(edge.from); }
}  // namespace

int mgp_is_enterprise_valid() { return memgraph::license::global_license_checker.IsEnterpriseValidFast(); }

mgp_error mgp_alloc(mgp_memory *memory, size_t size_in_bytes, void **result) {
  return mgp_aligned_alloc(memory, size_in_bytes, alignof(std::max_align_t), result);
}

mgp_error mgp_aligned_alloc(mgp_memory *memory, const size_t size_in_bytes, const size_t alignment, void **result) {
  return WrapExceptions(
      [memory, size_in_bytes, alignment] { return MgpAlignedAllocImpl(*memory->impl, size_in_bytes, alignment); },
      result);
}

void mgp_free(mgp_memory *memory, void *const p) {
  static_assert(noexcept(MgpFreeImpl(*memory->impl, p)));
  MgpFreeImpl(*memory->impl, p);
}

mgp_error mgp_global_alloc(size_t size_in_bytes, void **result) {
  return mgp_global_aligned_alloc(size_in_bytes, alignof(std::max_align_t), result);
}

mgp_error mgp_global_aligned_alloc(size_t size_in_bytes, size_t alignment, void **result) {
  return WrapExceptions(
      [size_in_bytes, alignment] {
        return MgpAlignedAllocImpl(gModuleRegistry.GetSharedMemoryResource(), size_in_bytes, alignment);
      },
      result);
}

void mgp_global_free(void *const p) {
  static_assert(noexcept(MgpFreeImpl(gModuleRegistry.GetSharedMemoryResource(), p)));
  MgpFreeImpl(gModuleRegistry.GetSharedMemoryResource(), p);
}

namespace {

template <class U, class... TArgs>
U *NewRawMgpObject(memgraph::utils::Allocator<U> allocator, TArgs &&...args) {
  return allocator.template new_object<U>(std::forward<TArgs>(args)...);
}

template <class U, class... TArgs>
U *NewRawMgpObject(memgraph::utils::MemoryResource *memory, TArgs &&...args) {
  memgraph::utils::Allocator<U> allocator(memory);
  return allocator.template new_object<U>(std::forward<TArgs>(args)...);
}

template <class U, class... TArgs>
U *NewRawMgpObject(mgp_memory *memory, TArgs &&...args) {
  return NewRawMgpObject<U, TArgs...>(memory->impl, std::forward<TArgs>(args)...);
}

// Assume that deallocation and object destruction never throws. If it does,
// we are in big trouble.
template <class T>
void DeleteRawMgpObject(T *ptr) noexcept {
  try {
    if (!ptr) return;
    memgraph::utils::Allocator<T> allocator(ptr->GetMemoryResource());
    allocator.delete_object(ptr);
  } catch (...) {
    LOG_FATAL("Cannot deallocate mgp object");
  }
}

template <class U, class... TArgs>
MgpUniquePtr<U> NewMgpObject(mgp_memory *memory, TArgs &&...args) {
  return MgpUniquePtr<U>(NewRawMgpObject<U>(memory->impl, std::forward<TArgs>(args)...), &DeleteRawMgpObject<U>);
}

mgp_value_type FromTypedValueType(memgraph::query::TypedValue::Type type) {
  switch (type) {
    case memgraph::query::TypedValue::Type::Null:
      return MGP_VALUE_TYPE_NULL;
    case memgraph::query::TypedValue::Type::Bool:
      return MGP_VALUE_TYPE_BOOL;
    case memgraph::query::TypedValue::Type::Int:
      return MGP_VALUE_TYPE_INT;
    case memgraph::query::TypedValue::Type::Double:
      return MGP_VALUE_TYPE_DOUBLE;
    case memgraph::query::TypedValue::Type::String:
      return MGP_VALUE_TYPE_STRING;
    case memgraph::query::TypedValue::Type::List:
      return MGP_VALUE_TYPE_LIST;
    case memgraph::query::TypedValue::Type::Map:
      return MGP_VALUE_TYPE_MAP;
    case memgraph::query::TypedValue::Type::Vertex:
      return MGP_VALUE_TYPE_VERTEX;
    case memgraph::query::TypedValue::Type::Edge:
      return MGP_VALUE_TYPE_EDGE;
    case memgraph::query::TypedValue::Type::Path:
      return MGP_VALUE_TYPE_PATH;
    case memgraph::query::TypedValue::Type::Date:
      return MGP_VALUE_TYPE_DATE;
    case memgraph::query::TypedValue::Type::LocalTime:
      return MGP_VALUE_TYPE_LOCAL_TIME;
    case memgraph::query::TypedValue::Type::LocalDateTime:
      return MGP_VALUE_TYPE_LOCAL_DATE_TIME;
    case memgraph::query::TypedValue::Type::ZonedDateTime:
      throw std::logic_error{"mgp_value for TypedValue::Type::ZonedDateTime doesn't exist."};
    case memgraph::query::TypedValue::Type::Duration:
      return MGP_VALUE_TYPE_DURATION;
    case memgraph::query::TypedValue::Type::Enum:
      throw std::logic_error{"mgp_value for TypedValue::Type::Enum doesn't exist."};
    case memgraph::query::TypedValue::Type::Point2d:
      throw std::logic_error{"mgp_value for TypedValue::Type::Point2d doesn't exist."};
    case memgraph::query::TypedValue::Type::Point3d:
      throw std::logic_error{"mgp_value for TypedValue::Type::Point3d doesn't exist."};
    case memgraph::query::TypedValue::Type::Function:
      throw std::logic_error{"mgp_value for TypedValue::Type::Function doesn't exist."};
    case memgraph::query::TypedValue::Type::Graph:
      throw std::logic_error{"mgp_value for TypedValue::Type::Graph doesn't exist."};
  }
}
}  // namespace

bool IsDeleted(const mgp_vertex *vertex) { return vertex->getImpl().impl_.vertex_->deleted; }

bool IsDeleted(const mgp_edge *edge) { return edge->impl.IsDeleted(); }

bool ContainsDeleted(const mgp_path *path) {
  return std::ranges::any_of(path->vertices, [](const auto &vertex) { return IsDeleted(&vertex); }) ||
         std::ranges::any_of(path->edges, [](const auto &edge) { return IsDeleted(&edge); });
}

bool ContainsDeleted(const mgp_list *list) {
  return std::ranges::any_of(list->elems, [](const auto &elem) { return ContainsDeleted(&elem); });
}

bool ContainsDeleted(const mgp_map *map) {
  return std::ranges::any_of(map->items, [](const auto &item) { return ContainsDeleted(&item.second); });
}

bool ContainsDeleted(const mgp_value *val) {
  switch (val->type) {
    // Value types
    case MGP_VALUE_TYPE_NULL:
    case MGP_VALUE_TYPE_BOOL:
    case MGP_VALUE_TYPE_INT:
    case MGP_VALUE_TYPE_DOUBLE:
    case MGP_VALUE_TYPE_STRING:
    case MGP_VALUE_TYPE_DATE:
    case MGP_VALUE_TYPE_LOCAL_TIME:
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME:
    case MGP_VALUE_TYPE_DURATION:
      return false;
    // Reference types
    case MGP_VALUE_TYPE_LIST:
      return ContainsDeleted(val->list_v);
    case MGP_VALUE_TYPE_MAP:
      return ContainsDeleted(val->map_v);
    case MGP_VALUE_TYPE_VERTEX:
      return IsDeleted(val->vertex_v);
    case MGP_VALUE_TYPE_EDGE:
      return IsDeleted(val->edge_v);
    case MGP_VALUE_TYPE_PATH:
      return ContainsDeleted(val->path_v);
    default:
      throw memgraph::query::QueryRuntimeException("Value of unknown type");
  }
  return false;
}

memgraph::storage::NameIdMapper *GetNameIdMapper(const mgp_graph *graph) {
  return graph->getImpl()->GetStorageAccessor()->GetNameIdMapper();
}
memgraph::query::TypedValue ToTypedValue(const mgp_value &val, memgraph::utils::Allocator<mgp_value> alloc) {
  switch (val.type) {
    case MGP_VALUE_TYPE_NULL:
      return memgraph::query::TypedValue(alloc);
    case MGP_VALUE_TYPE_BOOL:
      return memgraph::query::TypedValue(val.bool_v, alloc);
    case MGP_VALUE_TYPE_INT:
      return memgraph::query::TypedValue(val.int_v, alloc);
    case MGP_VALUE_TYPE_DOUBLE:
      return memgraph::query::TypedValue(val.double_v, alloc);
    case MGP_VALUE_TYPE_STRING:
      return {val.string_v, alloc};
    case MGP_VALUE_TYPE_LIST: {
      const auto *list = val.list_v;
      memgraph::query::TypedValue::TVector tv_list(alloc);
      tv_list.reserve(list->elems.size());
      for (const auto &elem : list->elems) {
        tv_list.emplace_back(ToTypedValue(elem, alloc));
      }
      return memgraph::query::TypedValue(std::move(tv_list));
    }
    case MGP_VALUE_TYPE_MAP: {
      const auto *map = val.map_v;
      memgraph::query::TypedValue::TMap tv_map(alloc);
      for (const auto &item : map->items) {
        tv_map.emplace(item.first, ToTypedValue(item.second, alloc));
      }
      return memgraph::query::TypedValue(std::move(tv_map));
    }
    case MGP_VALUE_TYPE_VERTEX:
      return memgraph::query::TypedValue(val.vertex_v->getImpl(), alloc);
    case MGP_VALUE_TYPE_EDGE:
      return memgraph::query::TypedValue(val.edge_v->impl, alloc);
    case MGP_VALUE_TYPE_PATH: {
      const auto *path = val.path_v;
      MG_ASSERT(!path->vertices.empty());
      MG_ASSERT(path->vertices.size() == path->edges.size() + 1);
      memgraph::query::Path tv_path(path->vertices[0].getImpl(), alloc);
      for (size_t i = 0; i < path->edges.size(); ++i) {
        tv_path.Expand(path->edges[i].impl);
        tv_path.Expand(path->vertices[i + 1].getImpl());
      }
      return memgraph::query::TypedValue(std::move(tv_path));
    }
    case MGP_VALUE_TYPE_DATE:
      return memgraph::query::TypedValue(val.date_v->date, alloc);
    case MGP_VALUE_TYPE_LOCAL_TIME:
      return memgraph::query::TypedValue(val.local_time_v->local_time, alloc);
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME:
      return memgraph::query::TypedValue(val.local_date_time_v->local_date_time, alloc);
    case MGP_VALUE_TYPE_DURATION:
      return memgraph::query::TypedValue(val.duration_v->duration, alloc);
  }
}

mgp_value::mgp_value(allocator_type alloc) noexcept : type(MGP_VALUE_TYPE_NULL), alloc(alloc) {}

mgp_value::mgp_value(bool val, allocator_type alloc) noexcept : type(MGP_VALUE_TYPE_BOOL), alloc(alloc), bool_v(val) {}

mgp_value::mgp_value(int64_t val, allocator_type alloc) noexcept : type(MGP_VALUE_TYPE_INT), alloc(alloc), int_v(val) {}

mgp_value::mgp_value(double val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_DOUBLE), alloc(alloc), double_v(val) {}

mgp_value::mgp_value(const char *val, allocator_type alloc)
    : type(MGP_VALUE_TYPE_STRING), alloc(alloc), string_v(val, alloc) {}

mgp_value::mgp_value(mgp_list *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_LIST), alloc(alloc), list_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_map *val, allocator_type alloc) noexcept : type(MGP_VALUE_TYPE_MAP), alloc(alloc), map_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_vertex *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_VERTEX), alloc(alloc), vertex_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_edge *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_EDGE), alloc(alloc), edge_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_path *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_PATH), alloc(alloc), path_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_date *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_DATE), alloc(alloc), date_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_local_time *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_LOCAL_TIME), alloc(alloc), local_time_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_local_date_time *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_LOCAL_DATE_TIME), alloc(alloc), local_date_time_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(mgp_duration *val, allocator_type alloc) noexcept
    : type(MGP_VALUE_TYPE_DURATION), alloc(alloc), duration_v(val) {
  MG_ASSERT(val->GetMemoryResource() == alloc.resource(),
            "Unable to take ownership of a pointer with different allocator.");
}

mgp_value::mgp_value(const memgraph::query::TypedValue &tv, mgp_graph *graph, allocator_type alloc)
    : type(FromTypedValueType(tv.type())), alloc(alloc) {
  switch (type) {
    case MGP_VALUE_TYPE_NULL:
      break;
    case MGP_VALUE_TYPE_BOOL:
      bool_v = tv.ValueBool();
      break;
    case MGP_VALUE_TYPE_INT:
      int_v = tv.ValueInt();
      break;
    case MGP_VALUE_TYPE_DOUBLE:
      double_v = tv.ValueDouble();
      break;
    case MGP_VALUE_TYPE_STRING:
      new (&string_v) memgraph::utils::pmr::string(tv.ValueString(), alloc);
      break;
    case MGP_VALUE_TYPE_LIST: {
      // Fill the stack allocated container and then construct the actual member
      // value. This handles the case when filling the container throws
      // something and our destructor doesn't get called so member value isn't
      // released.
      memgraph::utils::pmr::vector<mgp_value> elems(alloc);
      elems.reserve(tv.ValueList().size());
      for (const auto &elem : tv.ValueList()) {
        elems.emplace_back(elem, graph);
      }
      memgraph::utils::Allocator<mgp_list> allocator(alloc);
      list_v = allocator.new_object<mgp_list>(std::move(elems));
      break;
    }
    case MGP_VALUE_TYPE_MAP: {
      // Fill the stack allocated container and then construct the actual member
      // value. This handles the case when filling the container throws
      // something and our destructor doesn't get called so member value isn't
      // released.
      memgraph::utils::pmr::map<memgraph::utils::pmr::string, mgp_value> items(alloc);
      for (const auto &item : tv.ValueMap()) {
        items.emplace(item.first, mgp_value(item.second, graph, alloc));
      }
      memgraph::utils::Allocator<mgp_map> allocator(alloc);
      map_v = allocator.new_object<mgp_map>(std::move(items));
      break;
    }
    case MGP_VALUE_TYPE_VERTEX: {
      memgraph::utils::Allocator<mgp_vertex> allocator(alloc);
      vertex_v = std::visit(
          memgraph::utils::Overloaded{
              [&](memgraph::query::DbAccessor *) { return allocator.new_object<mgp_vertex>(tv.ValueVertex(), graph); },
              [&](memgraph::query::SubgraphDbAccessor *impl) {
                return allocator.new_object<mgp_vertex>(
                    memgraph::query::SubgraphVertexAccessor(tv.ValueVertex(), impl->getGraph()), graph);
              }},
          graph->impl);

      break;
    }
    case MGP_VALUE_TYPE_EDGE: {
      memgraph::utils::Allocator<mgp_edge> allocator(alloc);

      edge_v = std::visit(
          memgraph::utils::Overloaded{
              [&tv, graph, &allocator](memgraph::query::DbAccessor *) {
                return allocator.new_object<mgp_edge>(tv.ValueEdge(), tv.ValueEdge().DeletedEdgeFromVertex(),
                                                      tv.ValueEdge().DeletedEdgeToVertex(), graph);
              },
              [&tv, graph, &allocator](memgraph::query::SubgraphDbAccessor *db_impl) {
                return allocator.new_object<mgp_edge>(
                    tv.ValueEdge(),
                    memgraph::query::SubgraphVertexAccessor(tv.ValueEdge().DeletedEdgeFromVertex(),
                                                            db_impl->getGraph()),
                    memgraph::query::SubgraphVertexAccessor(tv.ValueEdge().DeletedEdgeToVertex(), db_impl->getGraph()),
                    graph);
              }},
          graph->impl);
      break;
    }
    case MGP_VALUE_TYPE_PATH: {
      // Fill the stack allocated container and then construct the actual member
      // value. This handles the case when filling the container throws
      // something and our destructor doesn't get called so member value isn't
      // released.
      mgp_path tmp_path(alloc);
      tmp_path.vertices.reserve(tv.ValuePath().vertices().size());
      for (const auto &v : tv.ValuePath().vertices()) {
        std::visit(
            memgraph::utils::Overloaded{
                [&v, graph, &tmp_path](memgraph::query::DbAccessor *) { tmp_path.vertices.emplace_back(v, graph); },
                [&v, graph, &tmp_path](memgraph::query::SubgraphDbAccessor *impl) {
                  tmp_path.vertices.emplace_back(memgraph::query::SubgraphVertexAccessor(v, impl->getGraph()), graph);
                }},
            graph->impl);
      }
      tmp_path.edges.reserve(tv.ValuePath().edges().size());
      for (const auto &e : tv.ValuePath().edges()) {
        std::visit(memgraph::utils::Overloaded{
                       [&e, graph, &tmp_path](memgraph::query::DbAccessor *) { tmp_path.edges.emplace_back(e, graph); },
                       [&e, graph, &tmp_path](memgraph::query::SubgraphDbAccessor *db_impl) {
                         tmp_path.edges.emplace_back(
                             e, memgraph::query::SubgraphVertexAccessor(e.From(), db_impl->getGraph()),
                             memgraph::query::SubgraphVertexAccessor(e.To(), db_impl->getGraph()), graph);
                       }},
                   graph->impl);
      }
      memgraph::utils::Allocator<mgp_path> allocator(alloc);
      path_v = allocator.new_object<mgp_path>(std::move(tmp_path));
      break;
    }
    case MGP_VALUE_TYPE_DATE: {
      memgraph::utils::Allocator<mgp_date> allocator(alloc);
      date_v = allocator.new_object<mgp_date>(tv.ValueDate());
      break;
    }
    case MGP_VALUE_TYPE_LOCAL_TIME: {
      memgraph::utils::Allocator<mgp_local_time> allocator(alloc);
      local_time_v = allocator.new_object<mgp_local_time>(tv.ValueLocalTime());
      break;
    }
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME: {
      memgraph::utils::Allocator<mgp_local_date_time> allocator(alloc);
      local_date_time_v = allocator.new_object<mgp_local_date_time>(tv.ValueLocalDateTime());
      break;
    }
    case MGP_VALUE_TYPE_DURATION: {
      memgraph::utils::Allocator<mgp_duration> allocator(alloc);
      duration_v = allocator.new_object<mgp_duration>(tv.ValueDuration());
      break;
    }
  }
}

mgp_value::mgp_value(const memgraph::storage::PropertyValue &pv, memgraph::storage::NameIdMapper *name_id_mapper,
                     allocator_type alloc)
    : alloc(alloc) {
  switch (pv.type()) {
    case memgraph::storage::PropertyValue::Type::Null:
      type = MGP_VALUE_TYPE_NULL;
      break;
    case memgraph::storage::PropertyValue::Type::Bool:
      type = MGP_VALUE_TYPE_BOOL;
      bool_v = pv.ValueBool();
      break;
    case memgraph::storage::PropertyValue::Type::Int:
      type = MGP_VALUE_TYPE_INT;
      int_v = pv.ValueInt();
      break;
    case memgraph::storage::PropertyValue::Type::Double:
      type = MGP_VALUE_TYPE_DOUBLE;
      double_v = pv.ValueDouble();
      break;
    case memgraph::storage::PropertyValue::Type::String:
      type = MGP_VALUE_TYPE_STRING;
      new (&string_v) memgraph::utils::pmr::string(pv.ValueString(), alloc);
      break;
    case memgraph::storage::PropertyValue::Type::List: {
      // Fill the stack allocated container and then construct the actual member
      // value. This handles the case when filling the container throws
      // something and our destructor doesn't get called so member value isn't
      // released.
      type = MGP_VALUE_TYPE_LIST;
      memgraph::utils::pmr::vector<mgp_value> elems(alloc);
      elems.reserve(pv.ValueList().size());
      for (const auto &elem : pv.ValueList()) {
        elems.emplace_back(elem, name_id_mapper);
      }
      memgraph::utils::Allocator<mgp_list> allocator(alloc);
      list_v = allocator.new_object<mgp_list>(std::move(elems));
      break;
    }
    case memgraph::storage::PropertyValue::Type::Map: {
      // Fill the stack allocated container and then construct the actual member
      // value. This handles the case when filling the container throws
      // something and our destructor doesn't get called so member value isn't
      // released.
      type = MGP_VALUE_TYPE_MAP;
      memgraph::utils::pmr::map<memgraph::utils::pmr::string, mgp_value> items(alloc);
      for (const auto &item : pv.ValueMap()) {
        auto key_as_name = name_id_mapper->IdToName(item.first.AsUint());
        auto value = mgp_value(item.second, name_id_mapper, alloc);
        items.emplace(std::move(key_as_name), std::move(value));
      }
      memgraph::utils::Allocator<mgp_map> allocator(alloc);
      map_v = allocator.new_object<mgp_map>(std::move(items));
      break;
    }
    case memgraph::storage::PropertyValue::Type::TemporalData: {
      const auto &temporal_data = pv.ValueTemporalData();
      switch (temporal_data.type) {
        case memgraph::storage::TemporalType::Date: {
          type = MGP_VALUE_TYPE_DATE;
          date_v = NewRawMgpObject<mgp_date>(alloc.resource(), temporal_data.microseconds);
          break;
        }
        case memgraph::storage::TemporalType::LocalTime: {
          type = MGP_VALUE_TYPE_LOCAL_TIME;
          local_time_v = NewRawMgpObject<mgp_local_time>(alloc.resource(), temporal_data.microseconds);
          break;
        }
        case memgraph::storage::TemporalType::LocalDateTime: {
          type = MGP_VALUE_TYPE_LOCAL_DATE_TIME;
          local_date_time_v = NewRawMgpObject<mgp_local_date_time>(alloc.resource(), temporal_data.microseconds);
          break;
        }
        case memgraph::storage::TemporalType::Duration: {
          type = MGP_VALUE_TYPE_DURATION;
          duration_v = NewRawMgpObject<mgp_duration>(alloc.resource(), temporal_data.microseconds);
          break;
        }
      }
      break;
    }
    case memgraph::storage::PropertyValue::Type::ZonedTemporalData: {
      throw std::logic_error{"mgp_value for PropertyValue::Type::ZonedTemporalData doesn't exist."};
      break;
    }
    case memgraph::storage::PropertyValue::Type::Enum: {
      throw std::logic_error{
          "mgp_value for PropertyValue::Type::Enum doesn't exist. Contact Memgraph team under team@memgraph.com or "
          "open a new issue / comment under existing one under github.com/memgraph/memgraph."};
      break;
    }
    case memgraph::storage::PropertyValue::Type::Point2d: {
      throw std::logic_error{
          "mgp_value for PropertyValue::Type::Point2d doesn't exist. Contact Memgraph team under team@memgraph.com or "
          "open a new issue / comment under existing one under github.com/memgraph/memgraph."};
      break;
    }
    case memgraph::storage::PropertyValue::Type::Point3d: {
      throw std::logic_error{
          "mgp_value for PropertyValue::Type::Point3d doesn't exist. Contact Memgraph team under team@memgraph.com or "
          "open a new issue / comment under existing one under github.com/memgraph/memgraph."};
      break;
    }
  }
}

mgp_value::mgp_value(const mgp_value &other, allocator_type alloc) : type(other.type), alloc(alloc) {
  switch (other.type) {
    case MGP_VALUE_TYPE_NULL:
      break;
    case MGP_VALUE_TYPE_BOOL:
      bool_v = other.bool_v;
      break;
    case MGP_VALUE_TYPE_INT:
      int_v = other.int_v;
      break;
    case MGP_VALUE_TYPE_DOUBLE:
      double_v = other.double_v;
      break;
    case MGP_VALUE_TYPE_STRING:
      new (&string_v) memgraph::utils::pmr::string(other.string_v, alloc);
      break;
    case MGP_VALUE_TYPE_LIST: {
      list_v = NewRawMgpObject<mgp_list>(alloc, *other.list_v);
      break;
    }
    case MGP_VALUE_TYPE_MAP: {
      map_v = NewRawMgpObject<mgp_map>(alloc, *other.map_v);
      break;
    }
    case MGP_VALUE_TYPE_VERTEX: {
      vertex_v = NewRawMgpObject<mgp_vertex>(alloc, *other.vertex_v);
      break;
    }
    case MGP_VALUE_TYPE_EDGE: {
      edge_v = NewRawMgpObject<mgp_edge>(alloc, *other.edge_v);
      break;
    }
    case MGP_VALUE_TYPE_PATH: {
      path_v = NewRawMgpObject<mgp_path>(alloc, *other.path_v);
      break;
    }
    case MGP_VALUE_TYPE_DATE: {
      date_v = NewRawMgpObject<mgp_date>(alloc, *other.date_v);
      break;
    }
    case MGP_VALUE_TYPE_LOCAL_TIME: {
      local_time_v = NewRawMgpObject<mgp_local_time>(alloc, *other.local_time_v);
      break;
    }
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME: {
      local_date_time_v = NewRawMgpObject<mgp_local_date_time>(alloc, *other.local_date_time_v);
      break;
    }
    case MGP_VALUE_TYPE_DURATION: {
      duration_v = NewRawMgpObject<mgp_duration>(alloc, *other.duration_v);
      break;
    }
  }
}

namespace {

void DeleteValueMember(mgp_value *value) noexcept {
  MG_ASSERT(value);
  memgraph::utils::Allocator<mgp_value> allocator(value->GetMemoryResource());
  switch (Call<mgp_value_type>(mgp_value_get_type, value)) {
    case MGP_VALUE_TYPE_NULL:
    case MGP_VALUE_TYPE_BOOL:
    case MGP_VALUE_TYPE_INT:
    case MGP_VALUE_TYPE_DOUBLE:
      return;
    case MGP_VALUE_TYPE_STRING:
      using TString = memgraph::utils::pmr::string;
      value->string_v.~TString();
      return;
    case MGP_VALUE_TYPE_LIST:
      allocator.delete_object(value->list_v);
      return;
    case MGP_VALUE_TYPE_MAP:
      allocator.delete_object(value->map_v);
      return;
    case MGP_VALUE_TYPE_VERTEX:
      allocator.delete_object(value->vertex_v);
      return;
    case MGP_VALUE_TYPE_EDGE:
      allocator.delete_object(value->edge_v);
      return;
    case MGP_VALUE_TYPE_PATH:
      allocator.delete_object(value->path_v);
      return;
    case MGP_VALUE_TYPE_DATE:
      allocator.delete_object(value->date_v);
      return;
    case MGP_VALUE_TYPE_LOCAL_TIME:
      allocator.delete_object(value->local_time_v);
      return;
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME:
      allocator.delete_object(value->local_date_time_v);
      return;
    case MGP_VALUE_TYPE_DURATION:
      allocator.delete_object(value->duration_v);
      return;
  }
}

}  // namespace

mgp_value::mgp_value(mgp_value &&other, allocator_type alloc) : type(other.type), alloc(alloc) {
  switch (other.type) {
    case MGP_VALUE_TYPE_NULL:
      break;
    case MGP_VALUE_TYPE_BOOL:
      bool_v = other.bool_v;
      break;
    case MGP_VALUE_TYPE_INT:
      int_v = other.int_v;
      break;
    case MGP_VALUE_TYPE_DOUBLE:
      double_v = other.double_v;
      break;
    case MGP_VALUE_TYPE_STRING:
      new (&string_v) memgraph::utils::pmr::string(std::move(other.string_v), alloc);
      break;
    case MGP_VALUE_TYPE_LIST:
      static_assert(std::is_pointer_v<decltype(list_v)>, "Expected to move list_v by copying pointers.");
      if (other.alloc == alloc) {
        list_v = other.list_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        memgraph::utils::Allocator<mgp_list> allocator(alloc);
        list_v = allocator.new_object<mgp_list>(std::move(*other.list_v));
      }
      break;
    case MGP_VALUE_TYPE_MAP:
      static_assert(std::is_pointer_v<decltype(map_v)>, "Expected to move map_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        map_v = other.map_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        memgraph::utils::Allocator<mgp_map> allocator(alloc);
        map_v = allocator.new_object<mgp_map>(std::move(*other.map_v));
      }
      break;
    case MGP_VALUE_TYPE_VERTEX:
      static_assert(std::is_pointer_v<decltype(vertex_v)>, "Expected to move vertex_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        vertex_v = other.vertex_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        memgraph::utils::Allocator<mgp_vertex> allocator(alloc);
        vertex_v = allocator.new_object<mgp_vertex>(std::move(*other.vertex_v));
      }
      break;
    case MGP_VALUE_TYPE_EDGE:
      static_assert(std::is_pointer_v<decltype(edge_v)>, "Expected to move edge_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        edge_v = other.edge_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        memgraph::utils::Allocator<mgp_edge> allocator(alloc);
        edge_v = allocator.new_object<mgp_edge>(std::move(*other.edge_v));
      }
      break;
    case MGP_VALUE_TYPE_PATH:
      static_assert(std::is_pointer_v<decltype(path_v)>, "Expected to move path_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        path_v = other.path_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        memgraph::utils::Allocator<mgp_path> allocator(alloc);
        path_v = allocator.new_object<mgp_path>(std::move(*other.path_v));
      }
      break;
    case MGP_VALUE_TYPE_DATE:
      static_assert(std::is_pointer_v<decltype(date_v)>, "Expected to move date_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        date_v = other.date_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        date_v = NewRawMgpObject<mgp_date>(alloc.resource(), *other.date_v);
      }
      break;
    case MGP_VALUE_TYPE_LOCAL_TIME:
      static_assert(std::is_pointer_v<decltype(local_time_v)>, "Expected to move local_time_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        local_time_v = other.local_time_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        local_time_v = NewRawMgpObject<mgp_local_time>(alloc.resource(), *other.local_time_v);
      }
      break;
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME:
      static_assert(std::is_pointer_v<decltype(local_date_time_v)>,
                    "Expected to move local_date_time_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        local_date_time_v = other.local_date_time_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        local_date_time_v = NewRawMgpObject<mgp_local_date_time>(alloc.resource(), *other.local_date_time_v);
      }
      break;
    case MGP_VALUE_TYPE_DURATION:
      static_assert(std::is_pointer_v<decltype(duration_v)>, "Expected to move duration_v by copying pointers.");
      if (*other.GetMemoryResource() == *alloc.resource()) {
        duration_v = other.duration_v;
        other.type = MGP_VALUE_TYPE_NULL;
      } else {
        duration_v = NewRawMgpObject<mgp_duration>(alloc.resource(), *other.duration_v);
      }
      break;
  }
  DeleteValueMember(&other);
  other.type = MGP_VALUE_TYPE_NULL;
}

mgp_value::~mgp_value() noexcept { DeleteValueMember(this); }

mgp_edge *mgp_edge::Copy(const mgp_edge &edge, mgp_memory &memory) {
  return std::visit(
      memgraph::utils::Overloaded{
          [&](memgraph::query::DbAccessor *) {
            return NewRawMgpObject<mgp_edge>(&memory, edge.impl, edge.impl.DeletedEdgeFromVertex(),
                                             edge.impl.DeletedEdgeToVertex(), edge.from.graph);
          },
          [&](memgraph::query::SubgraphDbAccessor *db_impl) {
            return NewRawMgpObject<mgp_edge>(
                &memory, edge.impl,
                memgraph::query::SubgraphVertexAccessor(edge.impl.DeletedEdgeFromVertex(), db_impl->getGraph()),
                memgraph::query::SubgraphVertexAccessor(edge.impl.DeletedEdgeToVertex(), db_impl->getGraph()),
                edge.to.graph);
          }},
      edge.to.graph->impl);
}

mgp_error mgp_value_copy(mgp_value *val, mgp_memory *memory, mgp_value **result) {
  return WrapExceptions([val, memory] { return NewRawMgpObject<mgp_value>(memory, *val); }, result);
}

void mgp_value_destroy(mgp_value *val) { DeleteRawMgpObject(val); }

mgp_error mgp_value_make_null(mgp_memory *memory, mgp_value **result) {
  return WrapExceptions([memory] { return NewRawMgpObject<mgp_value>(memory); }, result);
}

mgp_error mgp_value_make_bool(int val, mgp_memory *memory, mgp_value **result) {
  return WrapExceptions([val, memory] { return NewRawMgpObject<mgp_value>(memory, val != 0); }, result);
}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DEFINE_MGP_VALUE_MAKE_WITH_MEMORY(type, param)                                                \
  mgp_error mgp_value_make_##type(param val, mgp_memory *memory, mgp_value **result) {                \
    return WrapExceptions([val, memory] { return NewRawMgpObject<mgp_value>(memory, val); }, result); \
  }

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
DEFINE_MGP_VALUE_MAKE_WITH_MEMORY(int, int64_t);
DEFINE_MGP_VALUE_MAKE_WITH_MEMORY(double, double);
DEFINE_MGP_VALUE_MAKE_WITH_MEMORY(string, const char *);

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DEFINE_MGP_VALUE_MAKE(type)                                                                             \
  mgp_error mgp_value_make_##type(mgp_##type *val, mgp_value **result) {                                        \
    return WrapExceptions([val] { return NewRawMgpObject<mgp_value>(val->GetMemoryResource(), val); }, result); \
  }

DEFINE_MGP_VALUE_MAKE(list)
DEFINE_MGP_VALUE_MAKE(map)
DEFINE_MGP_VALUE_MAKE(vertex)
DEFINE_MGP_VALUE_MAKE(edge)
DEFINE_MGP_VALUE_MAKE(path)
DEFINE_MGP_VALUE_MAKE(date)
DEFINE_MGP_VALUE_MAKE(local_time)
DEFINE_MGP_VALUE_MAKE(local_date_time)
DEFINE_MGP_VALUE_MAKE(duration)

namespace {
mgp_value_type MgpValueGetType(const mgp_value &val) noexcept { return val.type; }
}  // namespace

mgp_error mgp_value_get_type(mgp_value *val, mgp_value_type *result) {
  static_assert(noexcept(MgpValueGetType(*val)));
  *result = MgpValueGetType(*val);
  return mgp_error::MGP_ERROR_NO_ERROR;
}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DEFINE_MGP_VALUE_IS(type_lowercase, type_uppercase)              \
  mgp_error mgp_value_is_##type_lowercase(mgp_value *val, int *result) { \
    static_assert(noexcept(MgpValueGetType(*val)));                      \
    *result = MgpValueGetType(*val) == MGP_VALUE_TYPE_##type_uppercase;  \
    return mgp_error::MGP_ERROR_NO_ERROR;                                \
  }

DEFINE_MGP_VALUE_IS(null, NULL)
DEFINE_MGP_VALUE_IS(bool, BOOL)
DEFINE_MGP_VALUE_IS(int, INT)
DEFINE_MGP_VALUE_IS(double, DOUBLE)
DEFINE_MGP_VALUE_IS(string, STRING)
DEFINE_MGP_VALUE_IS(list, LIST)
DEFINE_MGP_VALUE_IS(map, MAP)
DEFINE_MGP_VALUE_IS(vertex, VERTEX)
DEFINE_MGP_VALUE_IS(edge, EDGE)
DEFINE_MGP_VALUE_IS(path, PATH)
DEFINE_MGP_VALUE_IS(date, DATE)
DEFINE_MGP_VALUE_IS(local_time, LOCAL_TIME)
DEFINE_MGP_VALUE_IS(local_date_time, LOCAL_DATE_TIME)
DEFINE_MGP_VALUE_IS(duration, DURATION)

mgp_error mgp_value_get_bool(mgp_value *val, int *result) {
  *result = val->bool_v ? 1 : 0;
  return mgp_error::MGP_ERROR_NO_ERROR;
}
mgp_error mgp_value_get_int(mgp_value *val, int64_t *result) {
  *result = val->int_v;
  return mgp_error::MGP_ERROR_NO_ERROR;
}
mgp_error mgp_value_get_double(mgp_value *val, double *result) {
  *result = val->double_v;
  return mgp_error::MGP_ERROR_NO_ERROR;
}
mgp_error mgp_value_get_string(mgp_value *val, const char **result) {
  static_assert(noexcept(val->string_v.c_str()));
  *result = val->string_v.c_str();
  return mgp_error::MGP_ERROR_NO_ERROR;
}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DEFINE_MGP_VALUE_GET(type)                                      \
  mgp_error mgp_value_get_##type(mgp_value *val, mgp_##type **result) { \
    *result = val->type##_v;                                            \
    return mgp_error::MGP_ERROR_NO_ERROR;                               \
  }

DEFINE_MGP_VALUE_GET(list)
DEFINE_MGP_VALUE_GET(map)
DEFINE_MGP_VALUE_GET(vertex)
DEFINE_MGP_VALUE_GET(edge)
DEFINE_MGP_VALUE_GET(path)
DEFINE_MGP_VALUE_GET(date)
DEFINE_MGP_VALUE_GET(local_time)
DEFINE_MGP_VALUE_GET(local_date_time)
DEFINE_MGP_VALUE_GET(duration)

mgp_error mgp_list_make_empty(size_t capacity, mgp_memory *memory, mgp_list **result) {
  return WrapExceptions(
      [capacity, memory] {
        auto list = NewMgpObject<mgp_list>(memory);
        list->elems.reserve(capacity);
        return list.release();
      },
      result);
}

mgp_error mgp_list_copy(mgp_list *list, mgp_memory *memory, mgp_list **result) {
  return WrapExceptions([list, memory] { return NewRawMgpObject<mgp_list>(memory, *list); }, result);
}

void mgp_list_destroy(mgp_list *list) { DeleteRawMgpObject(list); }

mgp_error mgp_list_contains_deleted(mgp_list *list, int *result) {
  return WrapExceptions([list, result] { *result = ContainsDeleted(list); });
}

mgp_error mgp_list_append(mgp_list *list, mgp_value *val) {
  return WrapExceptions([list, val] {
    if (Call<size_t>(mgp_list_size, list) >= Call<size_t>(mgp_list_capacity, list)) {
      throw InsufficientBufferException{
          "Cannot append a new value to the mgp_list without extending it, because its size reached its capacity!"};
    }
    list->elems.push_back(*val);
  });
}

mgp_error mgp_list_append_extend(mgp_list *list, mgp_value *val) {
  return WrapExceptions([list, val] { list->elems.push_back(*val); });
}

mgp_error mgp_list_reserve(mgp_list *list, size_t n) {
  return WrapExceptions([list, n] { list->elems.reserve(n); });
}

mgp_error mgp_list_size(mgp_list *list, size_t *result) {
  static_assert(noexcept(list->elems.size()));
  *result = list->elems.size();
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_list_capacity(mgp_list *list, size_t *result) {
  static_assert(noexcept(list->elems.capacity()));
  *result = list->elems.capacity();
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_list_at(mgp_list *list, size_t i, mgp_value **result) {
  return WrapExceptions(
      [list, i] {
        if (i >= Call<size_t>(mgp_list_size, list)) {
          throw std::out_of_range("Element cannot be retrieved, because index exceeds list's size!");
        }
        return &list->elems[i];
      },
      result);
}

mgp_error mgp_map_make_empty(mgp_memory *memory, mgp_map **result) {
  return WrapExceptions([&memory] { return NewRawMgpObject<mgp_map>(memory); }, result);
}

mgp_error mgp_map_copy(mgp_map *map, mgp_memory *memory, mgp_map **result) {
  return WrapExceptions([map, memory] { return NewRawMgpObject<mgp_map>(memory, *map); }, result);
}

void mgp_map_destroy(mgp_map *map) { DeleteRawMgpObject(map); }

mgp_error mgp_map_contains_deleted(mgp_map *map, int *result) {
  return WrapExceptions([map, result] { *result = ContainsDeleted(map); });
}

mgp_error mgp_map_insert(mgp_map *map, const char *key, mgp_value *value) {
  return WrapExceptions([&] {
    auto emplace_result = map->items.emplace(key, *value);
    if (!emplace_result.second) {
      throw KeyAlreadyExistsException{"Map already contains mapping for {}", key};
    }
  });
}

mgp_error mgp_map_update(mgp_map *map, const char *key, mgp_value *value) {
  return WrapExceptions([&] {
    auto emplace_result = map->items.emplace(key, *value);
    if (!emplace_result.second) {
      map->items.erase(emplace_result.first);
      map->items.emplace(key, *value);
    }
  });
}

mgp_error mgp_map_erase(mgp_map *map, const char *key) {
  return WrapExceptions([&] {
    auto iterator = map->items.find(key);
    if (iterator != map->items.end()) {
      map->items.erase(iterator);
    }
  });
}

mgp_error mgp_map_size(mgp_map *map, size_t *result) {
  static_assert(noexcept(map->items.size()));
  *result = map->items.size();
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_map_at(mgp_map *map, const char *key, mgp_value **result) {
  return WrapExceptions(
      [&map, &key]() -> mgp_value * {
        auto found_it = map->items.find(key);
        if (found_it == map->items.end()) {
          return nullptr;
        };
        return &found_it->second;
      },
      result);
}

mgp_error mgp_key_exists(mgp_map *map, const char *key, int *result) {
  return WrapExceptions(
      [&map, &key]() -> int {
        auto found_it = map->items.find(key);
        if (found_it == map->items.end()) {
          return 0;
        };
        return 1;
      },
      result);
}

mgp_error mgp_map_item_key(mgp_map_item *item, const char **result) {
  return WrapExceptions([&item] { return item->key; }, result);
}

mgp_error mgp_map_item_value(mgp_map_item *item, mgp_value **result) {
  return WrapExceptions([item] { return item->value; }, result);
}

mgp_error mgp_map_iter_items(mgp_map *map, mgp_memory *memory, mgp_map_items_iterator **result) {
  return WrapExceptions([map, memory] { return NewRawMgpObject<mgp_map_items_iterator>(memory, map); }, result);
}

void mgp_map_items_iterator_destroy(mgp_map_items_iterator *it) { DeleteRawMgpObject(it); }

mgp_error mgp_map_items_iterator_get(mgp_map_items_iterator *it, mgp_map_item **result) {
  return WrapExceptions(
      [it]() -> mgp_map_item * {
        if (it->current_it == it->map->items.end()) {
          return nullptr;
        };
        return &it->current;
      },
      result);
}

mgp_error mgp_map_items_iterator_next(mgp_map_items_iterator *it, mgp_map_item **result) {
  return WrapExceptions(
      [it]() -> mgp_map_item * {
        if (it->current_it == it->map->items.end()) {
          return nullptr;
        }
        if (++it->current_it == it->map->items.end()) {
          return nullptr;
        }
        it->current.key = it->current_it->first.c_str();
        it->current.value = &it->current_it->second;
        return &it->current;
      },
      result);
}

mgp_error mgp_path_make_with_start(mgp_vertex *vertex, mgp_memory *memory, mgp_path **result) {
  return WrapExceptions(
      [vertex, memory]() -> mgp_path * {
        auto path = NewMgpObject<mgp_path>(memory);
        if (path == nullptr) {
          return nullptr;
        }
        path->vertices.push_back(*vertex);
        return path.release();
      },
      result);
}

mgp_error mgp_path_copy(mgp_path *path, mgp_memory *memory, mgp_path **result) {
  return WrapExceptions(
      [path, memory] {
        MG_ASSERT(Call<size_t>(mgp_path_size, path) == path->vertices.size() - 1, "Invalid mgp_path");
        return NewRawMgpObject<mgp_path>(memory, *path);
      },
      result);
}

void mgp_path_destroy(mgp_path *path) { DeleteRawMgpObject(path); }

mgp_error mgp_path_contains_deleted(mgp_path *path, int *result) {
  return WrapExceptions([path, result] { *result = ContainsDeleted(path); });
}

mgp_error mgp_path_expand(mgp_path *path, mgp_edge *edge) {
  return WrapExceptions([path, edge] {
    MG_ASSERT(Call<size_t>(mgp_path_size, path) == path->vertices.size() - 1, "Invalid mgp_path");
    // Check that the both the last vertex on path and dst_vertex are endpoints of
    // the given edge.
    auto *src_vertex = &path->vertices.back();
    mgp_vertex *dst_vertex{nullptr};

    if (edge->to == *src_vertex) {
      dst_vertex = &edge->from;
    } else if (edge->from == *src_vertex) {
      dst_vertex = &edge->to;
    } else {
      // edge is not a continuation on src_vertex
      throw std::logic_error{"The current last vertex in the path is not part of the given edge."};
    }
    // Try appending edge and dst_vertex to path, preserving the original mgp_path
    // instance if anything fails.
    memgraph::utils::OnScopeExit scope_guard(
        [path] { MG_ASSERT(Call<size_t>(mgp_path_size, path) == path->vertices.size() - 1); });

    path->edges.push_back(*edge);
    path->vertices.push_back(*dst_vertex);
  });
}

mgp_error mgp_path_pop(struct mgp_path *path) {
  return WrapExceptions([path] {
    if (path->edges.empty()) {
      throw std::out_of_range("Path contains no relationships.");
    }

    path->vertices.pop_back();
    path->edges.pop_back();
  });
}

namespace {
size_t MgpPathSize(const mgp_path &path) noexcept { return path.edges.size(); }
}  // namespace

mgp_error mgp_path_size(mgp_path *path, size_t *result) {
  *result = MgpPathSize(*path);
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_path_vertex_at(mgp_path *path, size_t i, mgp_vertex **result) {
  return WrapExceptions(
      [path, i] {
        const auto path_size = Call<size_t>(mgp_path_size, path);
        MG_ASSERT(path_size == path->vertices.size() - 1);
        if (i > path_size) {
          throw std::out_of_range("Vertex cannot be retrieved, because index exceeds path's size!");
        }
        return &path->vertices[i];
      },
      result);
}

mgp_error mgp_path_edge_at(mgp_path *path, size_t i, mgp_edge **result) {
  return WrapExceptions(
      [path, i] {
        const auto path_size = Call<size_t>(mgp_path_size, path);
        MG_ASSERT(path_size == path->vertices.size() - 1);
        if (i > path_size) {
          throw std::out_of_range("Edge cannot be retrieved, because index exceeds path's size!");
        }
        return &path->edges[i];
      },
      result);
}

mgp_error mgp_path_equal(mgp_path *p1, mgp_path *p2, int *result) {
  return WrapExceptions(
      [p1, p2] {
        const auto p1_size = MgpPathSize(*p1);
        const auto p2_size = MgpPathSize(*p2);
        MG_ASSERT(p1_size == p1->vertices.size() - 1);
        MG_ASSERT(p2_size == p2->vertices.size() - 1);
        if (p1_size != p2_size) {
          return 0;
        }
        const auto *start1 = Call<mgp_vertex *>(mgp_path_vertex_at, p1, 0);
        const auto *start2 = Call<mgp_vertex *>(mgp_path_vertex_at, p2, 0);
        static_assert(noexcept(start1 == start2));
        if (*start1 != *start2) {
          return 0;
        }
        for (size_t i = 0; i < p1_size; ++i) {
          const auto *e1 = Call<mgp_edge *>(mgp_path_edge_at, p1, i);
          const auto *e2 = Call<mgp_edge *>(mgp_path_edge_at, p2, i);
          if (*e1 != *e2) {
            return 0;
          }
        }
        return 1;
      },
      result);
}

mgp_error mgp_date_from_string(const char *string, mgp_memory *memory, mgp_date **date) {
  return WrapExceptions([string, memory] { return NewRawMgpObject<mgp_date>(memory, string); }, date);
}

mgp_error mgp_date_from_parameters(mgp_date_parameters *parameters, mgp_memory *memory, mgp_date **date) {
  return WrapExceptions([parameters, memory] { return NewRawMgpObject<mgp_date>(memory, parameters); }, date);
}

mgp_error mgp_date_copy(mgp_date *date, mgp_memory *memory, mgp_date **result) {
  return WrapExceptions([date, memory] { return NewRawMgpObject<mgp_date>(memory, *date); }, result);
}

void mgp_date_destroy(mgp_date *date) { DeleteRawMgpObject(date); }

mgp_error mgp_date_equal(mgp_date *first, mgp_date *second, int *result) {
  return WrapExceptions([first, second] { return first->date == second->date; }, result);
}

mgp_error mgp_date_get_year(mgp_date *date, int *year) {
  return WrapExceptions([date] { return date->date.year; }, year);
}

mgp_error mgp_date_get_month(mgp_date *date, int *month) {
  return WrapExceptions([date] { return date->date.month; }, month);
}

mgp_error mgp_date_get_day(mgp_date *date, int *day) {
  return WrapExceptions([date] { return date->date.day; }, day);
}

mgp_error mgp_date_timestamp(mgp_date *date, int64_t *timestamp) {
  return WrapExceptions([date] { return date->date.MicrosecondsSinceEpoch(); }, timestamp);
}

mgp_error mgp_date_now(mgp_memory *memory, mgp_date **date) {
  return WrapExceptions([memory] { return NewRawMgpObject<mgp_date>(memory, memgraph::utils::CurrentDate()); }, date);
}

mgp_error mgp_date_add_duration(mgp_date *date, mgp_duration *dur, mgp_memory *memory, mgp_date **result) {
  return WrapExceptions([date, dur, memory] { return NewRawMgpObject<mgp_date>(memory, date->date + dur->duration); },
                        result);
}

mgp_error mgp_date_sub_duration(mgp_date *date, mgp_duration *dur, mgp_memory *memory, mgp_date **result) {
  return WrapExceptions([date, dur, memory] { return NewRawMgpObject<mgp_date>(memory, date->date - dur->duration); },
                        result);
}

mgp_error mgp_date_diff(mgp_date *first, mgp_date *second, mgp_memory *memory, mgp_duration **result) {
  return WrapExceptions(
      [first, second, memory] { return NewRawMgpObject<mgp_duration>(memory, first->date - second->date); }, result);
}

mgp_error mgp_local_time_from_string(const char *string, mgp_memory *memory, mgp_local_time **local_time) {
  return WrapExceptions([string, memory] { return NewRawMgpObject<mgp_local_time>(memory, string); }, local_time);
}

mgp_error mgp_local_time_from_parameters(mgp_local_time_parameters *parameters, mgp_memory *memory,
                                         mgp_local_time **local_time) {
  return WrapExceptions([parameters, memory] { return NewRawMgpObject<mgp_local_time>(memory, parameters); },
                        local_time);
}

mgp_error mgp_local_time_copy(mgp_local_time *local_time, mgp_memory *memory, mgp_local_time **result) {
  return WrapExceptions([local_time, memory] { return NewRawMgpObject<mgp_local_time>(memory, *local_time); }, result);
}

void mgp_local_time_destroy(mgp_local_time *local_time) { DeleteRawMgpObject(local_time); }

mgp_error mgp_local_time_equal(mgp_local_time *first, mgp_local_time *second, int *result) {
  return WrapExceptions([first, second] { return first->local_time == second->local_time; }, result);
}

mgp_error mgp_local_time_get_hour(mgp_local_time *local_time, int *hour) {
  return WrapExceptions([local_time] { return local_time->local_time.hour; }, hour);
}

mgp_error mgp_local_time_get_minute(mgp_local_time *local_time, int *minute) {
  return WrapExceptions([local_time] { return local_time->local_time.minute; }, minute);
}

mgp_error mgp_local_time_get_second(mgp_local_time *local_time, int *second) {
  return WrapExceptions([local_time] { return local_time->local_time.second; }, second);
}

mgp_error mgp_local_time_get_millisecond(mgp_local_time *local_time, int *millisecond) {
  return WrapExceptions([local_time] { return local_time->local_time.millisecond; }, millisecond);
}

mgp_error mgp_local_time_get_microsecond(mgp_local_time *local_time, int *microsecond) {
  return WrapExceptions([local_time] { return local_time->local_time.microsecond; }, microsecond);
}

mgp_error mgp_local_time_timestamp(mgp_local_time *local_time, int64_t *timestamp) {
  return WrapExceptions([local_time] { return local_time->local_time.MicrosecondsSinceEpoch(); }, timestamp);
}

mgp_error mgp_local_time_now(mgp_memory *memory, mgp_local_time **local_time) {
  return WrapExceptions(
      [memory] { return NewRawMgpObject<mgp_local_time>(memory, memgraph::utils::CurrentLocalTime()); }, local_time);
}

mgp_error mgp_local_time_add_duration(mgp_local_time *local_time, mgp_duration *dur, mgp_memory *memory,
                                      mgp_local_time **result) {
  return WrapExceptions(
      [local_time, dur, memory] {
        return NewRawMgpObject<mgp_local_time>(memory, local_time->local_time + dur->duration);
      },
      result);
}

mgp_error mgp_local_time_sub_duration(mgp_local_time *local_time, mgp_duration *dur, mgp_memory *memory,
                                      mgp_local_time **result) {
  return WrapExceptions(
      [local_time, dur, memory] {
        return NewRawMgpObject<mgp_local_time>(memory, local_time->local_time - dur->duration);
      },
      result);
}

mgp_error mgp_local_time_diff(mgp_local_time *first, mgp_local_time *second, mgp_memory *memory,
                              mgp_duration **result) {
  return WrapExceptions(
      [first, second, memory] { return NewRawMgpObject<mgp_duration>(memory, first->local_time - second->local_time); },
      result);
}

mgp_error mgp_local_date_time_from_string(const char *string, mgp_memory *memory,
                                          mgp_local_date_time **local_date_time) {
  return WrapExceptions([string, memory] { return NewRawMgpObject<mgp_local_date_time>(memory, string); },
                        local_date_time);
}

mgp_error mgp_local_date_time_from_parameters(mgp_local_date_time_parameters *parameters, mgp_memory *memory,
                                              mgp_local_date_time **local_date_time) {
  return WrapExceptions([parameters, memory] { return NewRawMgpObject<mgp_local_date_time>(memory, parameters); },
                        local_date_time);
}

mgp_error mgp_local_date_time_copy(mgp_local_date_time *local_date_time, mgp_memory *memory,
                                   mgp_local_date_time **result) {
  return WrapExceptions(
      [local_date_time, memory] { return NewRawMgpObject<mgp_local_date_time>(memory, *local_date_time); }, result);
}

void mgp_local_date_time_destroy(mgp_local_date_time *local_date_time) { DeleteRawMgpObject(local_date_time); }

mgp_error mgp_local_date_time_equal(mgp_local_date_time *first, mgp_local_date_time *second, int *result) {
  return WrapExceptions([first, second] { return first->local_date_time == second->local_date_time; }, result);
}

mgp_error mgp_local_date_time_get_year(mgp_local_date_time *local_date_time, int *year) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.date().year; }, year);
}

mgp_error mgp_local_date_time_get_month(mgp_local_date_time *local_date_time, int *month) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.date().month; }, month);
}

mgp_error mgp_local_date_time_get_day(mgp_local_date_time *local_date_time, int *day) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.date().day; }, day);
}

mgp_error mgp_local_date_time_get_hour(mgp_local_date_time *local_date_time, int *hour) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.local_time().hour; }, hour);
}

mgp_error mgp_local_date_time_get_minute(mgp_local_date_time *local_date_time, int *minute) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.local_time().minute; }, minute);
}

mgp_error mgp_local_date_time_get_second(mgp_local_date_time *local_date_time, int *second) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.local_time().second; }, second);
}

mgp_error mgp_local_date_time_get_millisecond(mgp_local_date_time *local_date_time, int *millisecond) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.local_time().millisecond; },
                        millisecond);
}

mgp_error mgp_local_date_time_get_microsecond(mgp_local_date_time *local_date_time, int *microsecond) {
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.local_time().microsecond; },
                        microsecond);
}

mgp_error mgp_local_date_time_timestamp(mgp_local_date_time *local_date_time, int64_t *timestamp) {
  // Timestamps need to be in system time (UTC)
  return WrapExceptions([local_date_time] { return local_date_time->local_date_time.SysMicrosecondsSinceEpoch(); },
                        timestamp);
}

mgp_error mgp_local_date_time_now(mgp_memory *memory, mgp_local_date_time **local_date_time) {
  return WrapExceptions(
      [memory] { return NewRawMgpObject<mgp_local_date_time>(memory, memgraph::utils::CurrentLocalDateTime()); },
      local_date_time);
}

mgp_error mgp_local_date_time_add_duration(mgp_local_date_time *local_date_time, mgp_duration *dur, mgp_memory *memory,
                                           mgp_local_date_time **result) {
  return WrapExceptions(
      [local_date_time, dur, memory] {
        return NewRawMgpObject<mgp_local_date_time>(memory, local_date_time->local_date_time + dur->duration);
      },
      result);
}

mgp_error mgp_local_date_time_sub_duration(mgp_local_date_time *local_date_time, mgp_duration *dur, mgp_memory *memory,
                                           mgp_local_date_time **result) {
  return WrapExceptions(
      [local_date_time, dur, memory] {
        return NewRawMgpObject<mgp_local_date_time>(memory, local_date_time->local_date_time - dur->duration);
      },
      result);
}

mgp_error mgp_local_date_time_diff(mgp_local_date_time *first, mgp_local_date_time *second, mgp_memory *memory,
                                   mgp_duration **result) {
  return WrapExceptions(
      [first, second, memory] {
        return NewRawMgpObject<mgp_duration>(memory, first->local_date_time - second->local_date_time);
      },
      result);
}

mgp_error mgp_duration_from_string(const char *string, mgp_memory *memory, mgp_duration **duration) {
  return WrapExceptions([memory, string] { return NewRawMgpObject<mgp_duration>(memory, string); }, duration);
}

mgp_error mgp_duration_from_parameters(mgp_duration_parameters *parameters, mgp_memory *memory,
                                       mgp_duration **duration) {
  return WrapExceptions([memory, parameters] { return NewRawMgpObject<mgp_duration>(memory, parameters); }, duration);
}

mgp_error mgp_duration_from_microseconds(int64_t microseconds, mgp_memory *memory, mgp_duration **duration) {
  return WrapExceptions([microseconds, memory] { return NewRawMgpObject<mgp_duration>(memory, microseconds); },
                        duration);
}

mgp_error mgp_duration_copy(mgp_duration *duration, mgp_memory *memory, mgp_duration **result) {
  return WrapExceptions([duration, memory] { return NewRawMgpObject<mgp_duration>(memory, *duration); }, result);
}

void mgp_duration_destroy(mgp_duration *duration) { DeleteRawMgpObject(duration); }

mgp_error mgp_duration_get_microseconds(mgp_duration *duration, int64_t *microseconds) {
  return WrapExceptions([duration] { return duration->duration.microseconds; }, microseconds);
}

mgp_error mgp_duration_equal(mgp_duration *first, mgp_duration *second, int *result) {
  return WrapExceptions([first, second] { return first->duration == second->duration; }, result);
}

mgp_error mgp_duration_neg(mgp_duration *dur, mgp_memory *memory, mgp_duration **result) {
  return WrapExceptions([memory, dur] { return NewRawMgpObject<mgp_duration>(memory, -dur->duration); }, result);
}

mgp_error mgp_duration_add(mgp_duration *first, mgp_duration *second, mgp_memory *memory, mgp_duration **result) {
  return WrapExceptions(
      [memory, first, second] { return NewRawMgpObject<mgp_duration>(memory, first->duration + second->duration); },
      result);
}

mgp_error mgp_duration_sub(mgp_duration *first, mgp_duration *second, mgp_memory *memory, mgp_duration **result) {
  return WrapExceptions(
      [memory, first, second] { return NewRawMgpObject<mgp_duration>(memory, first->duration - second->duration); },
      result);
}

/// Plugin Result

mgp_error mgp_result_set_error_msg(mgp_result *res, const char *msg) {
  return WrapExceptions([=] {
    memgraph::utils::MemoryTracker::OutOfMemoryExceptionBlocker blocker{};
    res->error_msg.emplace(msg, res->rows.get_allocator());
  });
}

mgp_error mgp_result_new_record(mgp_result *res, mgp_result_record **result) {
  return WrapExceptions(
      [res] {
        auto allocator = res->rows.get_allocator();
        res->rows.push_back(
            mgp_result_record{.signature = &res->signature,
                              .values =
                                  memgraph::utils::pmr::vector<memgraph::query::TypedValue>{
                                      res->signature.size(), memgraph::query::TypedValue(allocator), allocator},
                              .ignore_deleted_values = !res->is_transactional});
        return &res->rows.back();
      },
      result);
}

mgp_error mgp_result_reserve(mgp_result *res, size_t n) {
  return WrapExceptions([res, n] { res->rows.reserve(n); });
}

mgp_error mgp_result_record_insert(mgp_result_record *record, const char *field_name, mgp_value *val) {
  return WrapExceptions([=] {
    auto allocator = record->values.get_allocator();
    // Validate field_name & val satisfy the procedure's result signature.
    MG_ASSERT(record->signature, "Expected to have a valid signature");
    auto find_it = record->signature->find(field_name);
    if (find_it == record->signature->end()) {
      throw std::out_of_range{fmt::format("The result doesn't have any field named '{}'.", field_name)};
    }
    auto const field_id = find_it->second.field_id;
    if (record->ignore_deleted_values && ContainsDeleted(val)) [[unlikely]] {
      record->has_deleted_values = true;
      return;
    }
    auto const *field_type = find_it->second.type;
    if (!field_type->SatisfiesType(*val)) [[unlikely]] {
      throw std::logic_error{
          fmt::format("The type of value doesn't satisfy the type '{}'!", field_type->GetPresentableName())};
    }

    record->values[field_id] = ToTypedValue(*val, allocator);
  });
}

mgp_error mgp_func_result_set_error_msg(mgp_func_result *res, const char *msg, mgp_memory *memory) {
  return WrapExceptions([=] {
    // We are copying error message string here, that includes the out of memory message
    memgraph::utils::MemoryTracker::OutOfMemoryExceptionBlocker const oom_blocker;
    res->error_msg.emplace(msg, memory->impl);
  });
}

mgp_error mgp_func_result_set_value(mgp_func_result *res, mgp_value *value, mgp_memory *memory) {
  return WrapExceptions([=] { res->value = ToTypedValue(*value, memory->impl); });
}

/// Graph Constructs

void mgp_properties_iterator_destroy(mgp_properties_iterator *it) { DeleteRawMgpObject(it); }

mgp_error mgp_properties_iterator_get(mgp_properties_iterator *it, mgp_property **result) {
  return WrapExceptions(
      [it]() -> mgp_property * {
        if (it->current) {
          return &it->property;
        };
        return nullptr;
      },
      result);
}

mgp_error mgp_properties_iterator_next(mgp_properties_iterator *it, mgp_property **result) {
  // Incrementing the iterator either for on-disk or in-memory
  // storage, so perhaps the underlying thing can throw.
  // Both copying TypedValue and/or string from PropertyName may fail to
  // allocate. Also, dereferencing `it->current_it` could also throw, so
  // either way return nullptr and leave `it` in undefined state.
  // Hopefully iterator comparison doesn't throw, but wrap the whole thing in
  // try ... catch just to be sure.
  return WrapExceptions(
      [it]() -> mgp_property * {
        if (it->current_it == it->pvs.end()) {
          MG_ASSERT(!it->current,
                    "Iteration is already done, so it->current should "
                    "have been set to std::nullopt");
          return nullptr;
        }
        if (++it->current_it == it->pvs.end()) {
          it->current = std::nullopt;
          return nullptr;
        }
        memgraph::utils::OnScopeExit clean_up([it] { it->current = std::nullopt; });
        auto propToName = std::visit(
            [it](auto *impl) {
              return memgraph::utils::pmr::string(impl->PropertyToName(it->current_it->first), it->GetMemoryResource());
            },
            it->graph->impl);
        it->current.emplace(propToName,
                            mgp_value(it->current_it->second, GetNameIdMapper(it->graph), it->GetMemoryResource()));
        it->property.name = it->current->first.c_str();
        it->property.value = &it->current->second;
        clean_up.Disable();
        return &it->property;
      },
      result);
}

mgp_error mgp_vertex_get_id(mgp_vertex *v, mgp_vertex_id *result) {
  return WrapExceptions(
      [v] { return mgp_vertex_id{.as_int = std::visit([](auto &impl) { return impl.Gid().AsInt(); }, v->impl)}; },
      result);
}

mgp_error mgp_vertex_get_in_degree(struct mgp_vertex *v, size_t *result) {
  return WrapExceptions(
      [v]() -> size_t {
        auto maybe_in_degree = std::visit([v](const auto &impl) { return impl.InDegree(v->graph->view); }, v->impl);
        if (maybe_in_degree.HasError()) {
          switch (maybe_in_degree.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get the degree of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL("Query modules shouldn't have access to nonexistent objects when getting vertex degree!");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting vertex degree.");
          }
        }
        return *maybe_in_degree;
      },
      result);
}

mgp_error mgp_vertex_get_out_degree(struct mgp_vertex *v, size_t *result) {
  return WrapExceptions(
      [v]() -> size_t {
        auto maybe_out_degree = std::visit([v](const auto &impl) { return impl.OutDegree(v->graph->view); }, v->impl);
        if (maybe_out_degree.HasError()) {
          switch (maybe_out_degree.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get the degree of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL("Query modules shouldn't have access to nonexistent objects when getting vertex degree!");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting vertex degree.");
          }
        }
        return *maybe_out_degree;
      },
      result);
}

mgp_error mgp_vertex_underlying_graph_is_mutable(mgp_vertex *v, int *result) {
  return mgp_graph_is_mutable(v->graph, result);
}

namespace {
memgraph::storage::PropertyValue ToPropertyValue(const mgp_value &value,
                                                 memgraph::storage::NameIdMapper *name_id_mapper);

memgraph::storage::PropertyValue ToPropertyValue(const mgp_list &list,
                                                 memgraph::storage::NameIdMapper *name_id_mapper) {
  memgraph::storage::PropertyValue result{std::vector<memgraph::storage::PropertyValue>{}};
  auto &result_list = result.ValueList();
  for (const auto &value : list.elems) {
    result_list.push_back(ToPropertyValue(value, name_id_mapper));
  }
  return result;
}

memgraph::storage::PropertyValue ToPropertyValue(const mgp_map &map, memgraph::storage::NameIdMapper *name_id_mapper) {
  auto result_map = memgraph::storage::PropertyValue::map_t{};
  do_reserve(result_map, map.items.size());
  for (const auto &[key, value] : map.items) {
    auto property_id = memgraph::storage::PropertyId::FromUint(name_id_mapper->NameToId(key));
    result_map.insert_or_assign(property_id, ToPropertyValue(value, name_id_mapper));
  }
  return memgraph::storage::PropertyValue{std::move(result_map)};
}

memgraph::storage::PropertyValue ToPropertyValue(const mgp_value &value,
                                                 memgraph::storage::NameIdMapper *name_id_mapper) {
  switch (value.type) {
    case MGP_VALUE_TYPE_NULL:
      return memgraph::storage::PropertyValue{};
    case MGP_VALUE_TYPE_BOOL:
      return memgraph::storage::PropertyValue{value.bool_v};
    case MGP_VALUE_TYPE_INT:
      return memgraph::storage::PropertyValue{value.int_v};
    case MGP_VALUE_TYPE_DOUBLE:
      return memgraph::storage::PropertyValue{value.double_v};
    case MGP_VALUE_TYPE_STRING:
      return memgraph::storage::PropertyValue{std::string{value.string_v}};
    case MGP_VALUE_TYPE_LIST:
      return ToPropertyValue(*value.list_v, name_id_mapper);
    case MGP_VALUE_TYPE_MAP:
      return ToPropertyValue(*value.map_v, name_id_mapper);
    case MGP_VALUE_TYPE_DATE:
      return memgraph::storage::PropertyValue{memgraph::storage::TemporalData{
          memgraph::storage::TemporalType::Date, value.date_v->date.MicrosecondsSinceEpoch()}};
    case MGP_VALUE_TYPE_LOCAL_TIME:
      return memgraph::storage::PropertyValue{memgraph::storage::TemporalData{
          memgraph::storage::TemporalType::LocalTime, value.local_time_v->local_time.MicrosecondsSinceEpoch()}};
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME:
      // Use generic system time (UTC)
      return memgraph::storage::PropertyValue{
          memgraph::storage::TemporalData{memgraph::storage::TemporalType::LocalDateTime,
                                          value.local_date_time_v->local_date_time.SysMicrosecondsSinceEpoch()}};
    case MGP_VALUE_TYPE_DURATION:
      return memgraph::storage::PropertyValue{memgraph::storage::TemporalData{memgraph::storage::TemporalType::Duration,
                                                                              value.duration_v->duration.microseconds}};
    case MGP_VALUE_TYPE_VERTEX:
      throw ValueConversionException{"A vertex is not a valid property value!"};
    case MGP_VALUE_TYPE_EDGE:
      throw ValueConversionException{"An edge is not a valid property value!"};
    case MGP_VALUE_TYPE_PATH:
      throw ValueConversionException{"A path is not a valid property value!"};
  }
}

memgraph::storage::ExternalPropertyValue ToExternalPropertyValue(const mgp_value &value);

memgraph::storage::ExternalPropertyValue ToExternalPropertyValue(const mgp_list &list) {
  memgraph::storage::ExternalPropertyValue result{std::vector<memgraph::storage::ExternalPropertyValue>{}};
  auto &result_list = result.ValueList();
  for (const auto &value : list.elems) {
    result_list.push_back(ToExternalPropertyValue(value));
  }
  return result;
}

memgraph::storage::ExternalPropertyValue ToExternalPropertyValue(const mgp_map &map) {
  auto result_map = memgraph::storage::ExternalPropertyValue::map_t{};
  do_reserve(result_map, map.items.size());
  for (const auto &[key, value] : map.items) {
    result_map.insert_or_assign(std::string{key}, ToExternalPropertyValue(value));
  }
  return memgraph::storage::ExternalPropertyValue{std::move(result_map)};
}

memgraph::storage::ExternalPropertyValue ToExternalPropertyValue(const mgp_value &value) {
  switch (value.type) {
    case MGP_VALUE_TYPE_NULL:
      return memgraph::storage::ExternalPropertyValue{};
    case MGP_VALUE_TYPE_BOOL:
      return memgraph::storage::ExternalPropertyValue{value.bool_v};
    case MGP_VALUE_TYPE_INT:
      return memgraph::storage::ExternalPropertyValue{value.int_v};
    case MGP_VALUE_TYPE_DOUBLE:
      return memgraph::storage::ExternalPropertyValue{value.double_v};
    case MGP_VALUE_TYPE_STRING:
      return memgraph::storage::ExternalPropertyValue{std::string{value.string_v}};
    case MGP_VALUE_TYPE_LIST:
      return ToExternalPropertyValue(*value.list_v);
    case MGP_VALUE_TYPE_MAP:
      return ToExternalPropertyValue(*value.map_v);
    case MGP_VALUE_TYPE_DATE:
      return memgraph::storage::ExternalPropertyValue{memgraph::storage::TemporalData{
          memgraph::storage::TemporalType::Date, value.date_v->date.MicrosecondsSinceEpoch()}};
    case MGP_VALUE_TYPE_LOCAL_TIME:
      return memgraph::storage::ExternalPropertyValue{memgraph::storage::TemporalData{
          memgraph::storage::TemporalType::LocalTime, value.local_time_v->local_time.MicrosecondsSinceEpoch()}};
    case MGP_VALUE_TYPE_LOCAL_DATE_TIME:
      // Use generic system time (UTC)
      return memgraph::storage::ExternalPropertyValue{
          memgraph::storage::TemporalData{memgraph::storage::TemporalType::LocalDateTime,
                                          value.local_date_time_v->local_date_time.SysMicrosecondsSinceEpoch()}};
    case MGP_VALUE_TYPE_DURATION:
      return memgraph::storage::ExternalPropertyValue{memgraph::storage::TemporalData{
          memgraph::storage::TemporalType::Duration, value.duration_v->duration.microseconds}};
    case MGP_VALUE_TYPE_VERTEX:
      throw ValueConversionException{"A vertex is not a valid property value!"};
    case MGP_VALUE_TYPE_EDGE:
      throw ValueConversionException{"An edge is not a valid property value!"};
    case MGP_VALUE_TYPE_PATH:
      throw ValueConversionException{"A path is not a valid property value!"};
  }
}
}  // namespace

mgp_error mgp_vertex_set_property(struct mgp_vertex *v, const char *property_name, mgp_value *property_value) {
  return WrapExceptions([=] {
    auto *ctx = v->graph->ctx;

#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !ctx->auth_checker->Has(v->getImpl(), v->graph->view,
                                memgraph::query::AuthQuery::FineGrainedPrivilege::UPDATE)) {
      throw AuthorizationException{"Insufficient permissions for setting a property on vertex!"};
    }
#endif
    if (!MgpVertexIsMutable(*v)) {
      throw ImmutableObjectException{"Cannot set a property on an immutable vertex!"};
    }

    const auto prop_key =
        std::visit([property_name](auto *impl) { return impl->NameToProperty(property_name); }, v->graph->impl);

    const auto result = std::visit(
        [prop_key, property_value, name_id_mapper = GetNameIdMapper(v->graph)](auto &impl) {
          return impl.SetProperty(prop_key, ToPropertyValue(*property_value, name_id_mapper));
        },
        v->impl);

    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::DELETED_OBJECT:
          throw DeletedObjectException{"Cannot set the properties of a deleted vertex!"};
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when setting a property of a vertex!");
        case memgraph::storage::Error::PROPERTIES_DISABLED:
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when setting a property of a vertex.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize setting a property of a vertex."};
      }
    }

    ctx->execution_stats[memgraph::query::ExecutionStats::Key::UPDATED_PROPERTIES] += 1;

    auto *trigger_ctx_collector = ctx->trigger_context_collector;
    if (!trigger_ctx_collector ||
        !trigger_ctx_collector->ShouldRegisterObjectPropertyChange<memgraph::query::VertexAccessor>()) {
      return;
    }
    const auto old_value = memgraph::query::TypedValue(*result, GetNameIdMapper(v->graph));
    if (property_value->type == mgp_value_type::MGP_VALUE_TYPE_NULL) {
      trigger_ctx_collector->RegisterRemovedObjectProperty(v->getImpl(), prop_key, old_value);
      return;
    }
    const auto new_value = ToTypedValue(*property_value, property_value->alloc);
    trigger_ctx_collector->RegisterSetObjectProperty(v->getImpl(), prop_key, old_value, new_value);
  });
}

mgp_error mgp_vertex_set_properties(struct mgp_vertex *v, struct mgp_map *properties) {
  return WrapExceptions([=] {
    auto *ctx = v->graph->ctx;

#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !ctx->auth_checker->Has(v->getImpl(), v->graph->view,
                                memgraph::query::AuthQuery::FineGrainedPrivilege::UPDATE)) {
      throw AuthorizationException{"Insufficient permissions for setting properties on the vertex!"};
    }
#endif
    if (!MgpVertexIsMutable(*v)) {
      throw ImmutableObjectException{"Cannot set properties of an immutable vertex!"};
    }

    std::map<memgraph::storage::PropertyId, memgraph::storage::PropertyValue> props;
    for (const auto &item : properties->items) {
      props.insert(std::visit(
          [&item, name_id_mapper = GetNameIdMapper(v->graph)](auto *impl) {
            return std::make_pair(impl->NameToProperty(item.first), ToPropertyValue(item.second, name_id_mapper));
          },
          v->graph->impl));
    }

    const auto result = v->getImpl().UpdateProperties(props);

    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::DELETED_OBJECT:
          throw DeletedObjectException{"Cannot set the properties of a deleted vertex!"};
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when setting a property of a vertex!");
        case memgraph::storage::Error::PROPERTIES_DISABLED:
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when setting a property of a vertex.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize setting a property of a vertex."};
      }
    }

    ctx->execution_stats[memgraph::query::ExecutionStats::Key::UPDATED_PROPERTIES] +=
        static_cast<int64_t>(properties->items.size());

    auto *trigger_ctx_collector = ctx->trigger_context_collector;
    if (!trigger_ctx_collector ||
        !trigger_ctx_collector->ShouldRegisterObjectPropertyChange<memgraph::query::VertexAccessor>()) {
      return;
    }
    for (const auto &res : *result) {
      const auto property_key = std::get<0>(res);
      const auto old_value = memgraph::query::TypedValue(std::get<1>(res), GetNameIdMapper(v->graph));
      const auto new_value = memgraph::query::TypedValue(std::get<2>(res), GetNameIdMapper(v->graph));

      if (new_value.IsNull()) {
        trigger_ctx_collector->RegisterRemovedObjectProperty(v->getImpl(), property_key, old_value);
        continue;
      }

      trigger_ctx_collector->RegisterSetObjectProperty(v->getImpl(), property_key, old_value, new_value);
    }
  });
}

mgp_error mgp_vertex_add_label(struct mgp_vertex *v, mgp_label label) {
  return WrapExceptions([=] {
    auto *ctx = v->graph->ctx;
    const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label.name); }, v->graph->impl);

#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !(ctx->auth_checker->Has(v->getImpl(), v->graph->view,
                                 memgraph::query::AuthQuery::FineGrainedPrivilege::UPDATE) &&
          ctx->auth_checker->Has({label_id}, memgraph::query::AuthQuery::FineGrainedPrivilege::CREATE_DELETE))) {
      throw AuthorizationException{"Insufficient permissions for adding a label to vertex!"};
    }
#endif

    if (!MgpVertexIsMutable(*v)) {
      throw ImmutableObjectException{"Cannot add a label to an immutable vertex!"};
    }

    const auto result = std::visit([label_id](auto &impl) { return impl.AddLabel(label_id); }, v->impl);

    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::DELETED_OBJECT:
          throw DeletedObjectException{"Cannot add a label to a deleted vertex!"};
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when adding a label to a vertex!");
        case memgraph::storage::Error::PROPERTIES_DISABLED:
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when adding a label to a vertex.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize adding a label to a vertex."};
      }
    }

    if (ctx) {
      ctx->execution_stats[memgraph::query::ExecutionStats::Key::CREATED_LABELS] += 1;

      if (ctx->trigger_context_collector) {
        ctx->trigger_context_collector->RegisterSetVertexLabel(v->getImpl(), label_id);
      }
    }
  });
}

mgp_error mgp_vertex_remove_label(struct mgp_vertex *v, mgp_label label) {
  return WrapExceptions([=] {
    auto *ctx = v->graph->ctx;
    const auto label_id = std::visit([&label](auto *impl) { return impl->NameToLabel(label.name); }, v->graph->impl);

#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !(ctx->auth_checker->Has(v->getImpl(), v->graph->view,
                                 memgraph::query::AuthQuery::FineGrainedPrivilege::UPDATE) &&
          ctx->auth_checker->Has({label_id}, memgraph::query::AuthQuery::FineGrainedPrivilege::CREATE_DELETE))) {
      throw AuthorizationException{"Insufficient permissions for removing a label from vertex!"};
    }
#endif
    if (!MgpVertexIsMutable(*v)) {
      throw ImmutableObjectException{"Cannot remove a label from an immutable vertex!"};
    }
    const auto result = std::visit([label_id](auto &impl) { return impl.RemoveLabel(label_id); }, v->impl);

    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::DELETED_OBJECT:
          throw DeletedObjectException{"Cannot remove a label from a deleted vertex!"};
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when removing a label from a vertex!");
        case memgraph::storage::Error::PROPERTIES_DISABLED:
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when removing a label from a vertex.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize removing a label from a vertex."};
      }
    }

    if (ctx) {
      ctx->execution_stats[memgraph::query::ExecutionStats::Key::DELETED_LABELS] += 1;

      if (ctx->trigger_context_collector) {
        ctx->trigger_context_collector->RegisterRemovedVertexLabel(v->getImpl(), label_id);
      }
    }
  });
}

mgp_error mgp_vertex_copy(mgp_vertex *v, mgp_memory *memory, mgp_vertex **result) {
  return WrapExceptions([v, memory] { return NewRawMgpObject<mgp_vertex>(memory, *v); }, result);
}

void mgp_vertex_destroy(mgp_vertex *v) { DeleteRawMgpObject(v); }

mgp_error mgp_vertex_is_deleted(mgp_vertex *v, int *result) {
  return WrapExceptions([v] { return IsDeleted(v); }, result);
}

mgp_error mgp_vertex_equal(mgp_vertex *v1, mgp_vertex *v2, int *result) {
  // NOLINTNEXTLINE(clang-diagnostic-unevaluated-expression)
  static_assert(noexcept(*v1 == *v2));
  *result = *v1 == *v2 ? 1 : 0;
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_vertex_labels_count(mgp_vertex *v, size_t *result) {
  return WrapExceptions(
      [v]() -> size_t {
        auto maybe_labels = std::visit([v](const auto &impl) { return impl.Labels(v->graph->view); }, v->impl);
        if (maybe_labels.HasError()) {
          switch (maybe_labels.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get the labels of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL("Query modules shouldn't have access to nonexistent objects when getting vertex labels!");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting vertex labels.");
          }
        }
        return maybe_labels->size();
      },
      result);
}

mgp_error mgp_vertex_label_at(mgp_vertex *v, size_t i, mgp_label *result) {
  return WrapExceptions(
      [v, i]() -> const char * {
        // TODO: Maybe it's worth caching this in mgp_vertex.
        auto maybe_labels = std::visit([v](const auto &impl) { return impl.Labels(v->graph->view); }, v->impl);
        if (maybe_labels.HasError()) {
          switch (maybe_labels.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get a label of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL("Query modules shouldn't have access to nonexistent objects when getting a label of a vertex!");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting a label of a vertex.");
          }
        }
        if (i >= maybe_labels->size()) {
          throw std::out_of_range("Label cannot be retrieved, because index exceeds the number of labels!");
        }
        const auto &label = (*maybe_labels)[i];
        static_assert(std::is_lvalue_reference_v<
                          decltype(std::get<memgraph::query::DbAccessor *>(v->graph->impl)->LabelToName(label))>,
                      "Expected LabelToName to return a pointer or reference, so we "
                      "don't have to take a copy and manage memory.");

        const auto &name = std::visit(
            [label](const auto *impl) -> const std::string & { return impl->LabelToName(label); }, v->graph->impl);
        return name.c_str();
      },
      &result->name);
}

mgp_error mgp_vertex_has_label_named(mgp_vertex *v, const char *name, int *result) {
  return WrapExceptions(
      [v, name] {
        memgraph::storage::LabelId label;
        label = std::visit([name](auto *impl) { return impl->NameToLabel(name); }, v->graph->impl);

        auto maybe_has_label =
            std::visit([v, label](auto &impl) { return impl.HasLabel(v->graph->view, label); }, v->impl);
        if (maybe_has_label.HasError()) {
          switch (maybe_has_label.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot check the existence of a label on a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL(
                  "Query modules shouldn't have access to nonexistent objects when checking the existence of a label "
                  "on "
                  "a vertex!");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when checking the existence of a label on a vertex.");
          }
        }
        return *maybe_has_label ? 1 : 0;
      },
      result);
}

mgp_error mgp_vertex_has_label(mgp_vertex *v, mgp_label label, int *result) {
  return mgp_vertex_has_label_named(v, label.name, result);
}

mgp_error mgp_vertex_get_property(mgp_vertex *v, const char *name, mgp_memory *memory, mgp_value **result) {
  return WrapExceptions(
      [v, name, memory]() -> mgp_value * {
        const auto &key = std::visit([name](auto *impl) { return impl->NameToProperty(name); }, v->graph->impl);

        auto maybe_prop = std::visit([v, key](auto &impl) { return impl.GetProperty(v->graph->view, key); }, v->impl);
        if (maybe_prop.HasError()) {
          switch (maybe_prop.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get a property of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL(
                  "Query modules shouldn't have access to nonexistent objects when getting a property of a vertex.");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting a property of a vertex.");
          }
        }
        return NewRawMgpObject<mgp_value>(memory, std::move(*maybe_prop), GetNameIdMapper(v->graph));
      },
      result);
}

mgp_error mgp_vertex_iter_properties(mgp_vertex *v, mgp_memory *memory, mgp_properties_iterator **result) {
  // NOTE: This copies the whole properties into the iterator.
  // TODO: Think of a good way to avoid the copy which doesn't just rely on some
  // assumption that storage may return a pointer to the property store. This
  // will probably require a different API in storage.
  return WrapExceptions(
      [v, memory] {
        auto maybe_props = std::visit([v](auto &impl) { return impl.Properties(v->graph->view); }, v->impl);
        if (maybe_props.HasError()) {
          switch (maybe_props.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get the properties of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL(
                  "Query modules shouldn't have access to nonexistent objects when getting the properties of a "
                  "vertex.");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting the properties of a vertex.");
          }
        }
        return NewRawMgpObject<mgp_properties_iterator>(memory, v->graph, std::move(*maybe_props));
      },
      result);
}

void mgp_edges_iterator_destroy(mgp_edges_iterator *it) { DeleteRawMgpObject(it); }

#ifdef MG_ENTERPRISE
namespace {
void NextPermittedEdge(mgp_edges_iterator &it, const bool for_in) {
  if (const auto *ctx = it.source_vertex.graph->ctx; !ctx || !ctx->auth_checker) return;

  auto &impl_it = for_in ? it.in_it : it.out_it;
  const auto end = for_in ? it.in->end() : it.out->end();

  if (impl_it) {
    const auto *auth_checker = it.source_vertex.graph->ctx->auth_checker.get();
    const auto view = it.source_vertex.graph->view;
    while (*impl_it != end) {
      auto edgeAcc = **impl_it;
      if (auth_checker->Has(edgeAcc, memgraph::query::AuthQuery::FineGrainedPrivilege::READ)) {
        const auto &check_vertex = it.source_vertex.getImpl() == edgeAcc.From() ? edgeAcc.To() : edgeAcc.From();
        if (auth_checker->Has(check_vertex, view, memgraph::query::AuthQuery::FineGrainedPrivilege::READ)) {
          break;
        }
      }

      ++*impl_it;
    }
  }
};
}  // namespace
#endif

mgp_error mgp_vertex_iter_in_edges(mgp_vertex *v, mgp_memory *memory, mgp_edges_iterator **result) {
  return WrapExceptions(
      [v, memory] {
        auto it = NewMgpObject<mgp_edges_iterator>(memory, *v);
        MG_ASSERT(it != nullptr);

        auto maybe_edges = std::visit([v](auto &impl) { return impl.InEdges(v->graph->view); }, v->impl);
        if (maybe_edges.HasError()) {
          switch (maybe_edges.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get the inbound edges of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL(
                  "Query modules shouldn't have access to nonexistent objects when getting the inbound edges of a "
                  "vertex.");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting the inbound edges of a vertex.");
          }
        }
        it->in.emplace(std::move(maybe_edges->edges));
        it->in_it.emplace(it->in->begin());
#ifdef MG_ENTERPRISE
        if (memgraph::license::global_license_checker.IsEnterpriseValidFast()) {
          NextPermittedEdge(*it, true);
        }
#endif

        if (*it->in_it != it->in->end()) {
          std::visit(
              memgraph::utils::Overloaded{
                  [&](memgraph::query::DbAccessor *) {
                    auto edgeAcc = **it->in_it;
                    it->current_e.emplace(edgeAcc, edgeAcc.From(), edgeAcc.To(), v->graph, it->GetMemoryResource());
                  },
                  [&](memgraph::query::SubgraphDbAccessor *impl) {
                    auto edgeAcc = **it->in_it;
                    it->current_e.emplace(edgeAcc,
                                          memgraph::query::SubgraphVertexAccessor(edgeAcc.From(), impl->getGraph()),
                                          memgraph::query::SubgraphVertexAccessor(edgeAcc.To(), impl->getGraph()),
                                          v->graph, it->GetMemoryResource());
                  }},
              v->graph->impl);
        }

        return it.release();
      },
      result);
}

mgp_error mgp_vertex_iter_out_edges(mgp_vertex *v, mgp_memory *memory, mgp_edges_iterator **result) {
  return WrapExceptions(
      [v, memory] {
        auto it = NewMgpObject<mgp_edges_iterator>(memory, *v);
        MG_ASSERT(it != nullptr);
        auto maybe_edges = std::visit([v](auto &impl) { return impl.OutEdges(v->graph->view); }, v->impl);

        if (maybe_edges.HasError()) {
          switch (maybe_edges.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get the outbound edges of a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL(
                  "Query modules shouldn't have access to nonexistent objects when getting the outbound edges of a "
                  "vertex.");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting the outbound edges of a vertex.");
          }
        }

        it->out.emplace(std::move(maybe_edges->edges));
        it->out_it.emplace(it->out->begin());

#ifdef MG_ENTERPRISE
        if (memgraph::license::global_license_checker.IsEnterpriseValidFast()) {
          NextPermittedEdge(*it, false);
        }
#endif

        if (*it->out_it != it->out->end()) {
          std::visit(
              memgraph::utils::Overloaded{
                  [&](memgraph::query::DbAccessor *) {
                    memgraph::query::EdgeAccessor edgeAcc = **it->out_it;
                    it->current_e.emplace(edgeAcc, edgeAcc.From(), edgeAcc.To(), v->graph, it->GetMemoryResource());
                  },
                  [&](memgraph::query::SubgraphDbAccessor *impl) {
                    auto edgeAcc = **it->out_it;
                    it->current_e.emplace(edgeAcc,
                                          memgraph::query::SubgraphVertexAccessor(edgeAcc.From(), impl->getGraph()),
                                          memgraph::query::SubgraphVertexAccessor(edgeAcc.To(), impl->getGraph()),
                                          v->graph, it->GetMemoryResource());
                  }},
              v->graph->impl);
        }

        return it.release();
      },
      result);
}

mgp_error mgp_edges_iterator_underlying_graph_is_mutable(mgp_edges_iterator *it, int *result) {
  return mgp_vertex_underlying_graph_is_mutable(&it->source_vertex, result);
}

mgp_error mgp_edges_iterator_get(mgp_edges_iterator *it, mgp_edge **result) {
  return WrapExceptions(
      [it]() -> mgp_edge * {
        if (it->current_e.has_value()) {
          return &*it->current_e;
        }
        return nullptr;
      },
      result);
}

mgp_error mgp_edges_iterator_next(mgp_edges_iterator *it, mgp_edge **result) {
  return WrapExceptions(
      [it] {
        MG_ASSERT(it->in || it->out);
        auto next = [it](const bool for_in) -> mgp_edge * {
          auto &impl_it = for_in ? it->in_it : it->out_it;
          const auto end = for_in ? it->in->end() : it->out->end();
          if (*impl_it == end) {
            MG_ASSERT(!it->current_e,
                      "Iteration is already done, so it->current_e "
                      "should have been set to std::nullopt");
            return nullptr;
          }

          ++*impl_it;

#ifdef MG_ENTERPRISE
          if (memgraph::license::global_license_checker.IsEnterpriseValidFast()) {
            NextPermittedEdge(*it, for_in);
          }
#endif

          if (*impl_it == end) {
            it->current_e = std::nullopt;
            return nullptr;
          }
          std::visit(memgraph::utils::Overloaded{
                         [&](memgraph::query::DbAccessor *) {
                           auto edgeAcc = **impl_it;
                           it->current_e.emplace(edgeAcc, edgeAcc.From(), edgeAcc.To(), it->source_vertex.graph,
                                                 it->GetMemoryResource());
                         },
                         [&](memgraph::query::SubgraphDbAccessor *impl) {
                           auto edgeAcc = **impl_it;
                           it->current_e.emplace(
                               edgeAcc, memgraph::query::SubgraphVertexAccessor(edgeAcc.From(), impl->getGraph()),
                               memgraph::query::SubgraphVertexAccessor(edgeAcc.To(), impl->getGraph()),
                               it->source_vertex.graph, it->GetMemoryResource());
                         }},
                     it->source_vertex.graph->impl);

          return &*it->current_e;
        };
        if (it->in_it) {
          return next(true);
        }
        return next(false);
      },
      result);
}

mgp_error mgp_edge_get_id(mgp_edge *e, mgp_edge_id *result) {
  return WrapExceptions([e] { return mgp_edge_id{.as_int = e->impl.Gid().AsInt()}; }, result);
}

mgp_error mgp_edge_underlying_graph_is_mutable(mgp_edge *e, int *result) {
  return mgp_vertex_underlying_graph_is_mutable(&e->from, result);
}

mgp_error mgp_edge_copy(mgp_edge *e, mgp_memory *memory, mgp_edge **result) {
  return WrapExceptions([e, memory] { return mgp_edge::Copy(*e, *memory); }, result);
}

void mgp_edge_destroy(mgp_edge *e) { DeleteRawMgpObject(e); }

mgp_error mgp_edge_is_deleted(mgp_edge *e, int *result) {
  return WrapExceptions([e] { return IsDeleted(e); }, result);
}

mgp_error mgp_edge_equal(mgp_edge *e1, mgp_edge *e2, int *result) {
  // NOLINTNEXTLINE(clang-diagnostic-unevaluated-expression)
  static_assert(noexcept(*e1 == *e2));
  *result = *e1 == *e2 ? 1 : 0;
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_edge_get_type(mgp_edge *e, mgp_edge_type *result) {
  return WrapExceptions(
      [e] {
        const auto &name = std::visit(
            [e](const auto *impl) -> const std::string & { return impl->EdgeTypeToName(e->impl.EdgeType()); },
            e->from.graph->impl);
        return name.c_str();
      },
      &result->name);
}

mgp_error mgp_edge_get_from(mgp_edge *e, mgp_vertex **result) {
  *result = &e->from;
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_edge_get_to(mgp_edge *e, mgp_vertex **result) {
  *result = &e->to;
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_edge_get_property(mgp_edge *e, const char *name, mgp_memory *memory, mgp_value **result) {
  return WrapExceptions(
      [e, name, memory] {
        const auto &key = std::visit([name](auto *impl) { return impl->NameToProperty(name); }, e->from.graph->impl);
        auto view = e->from.graph->view;
        auto maybe_prop = e->impl.GetProperty(view, key);
        if (maybe_prop.HasError()) {
          switch (maybe_prop.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get a property of a deleted edge!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL(
                  "Query modules shouldn't have access to nonexistent objects when getting a property of an edge.");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting a property of an edge.");
          }
        }
        return NewRawMgpObject<mgp_value>(memory, std::move(*maybe_prop), GetNameIdMapper(e->from.graph));
      },
      result);
}

mgp_error mgp_edge_set_property(struct mgp_edge *e, const char *property_name, mgp_value *property_value) {
  return WrapExceptions([=] {
    auto *ctx = e->from.graph->ctx;

#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !ctx->auth_checker->Has(e->impl, memgraph::query::AuthQuery::FineGrainedPrivilege::UPDATE)) {
      throw AuthorizationException{"Insufficient permissions for setting a property on edge!"};
    }
#endif

    if (!MgpEdgeIsMutable(*e)) {
      throw ImmutableObjectException{"Cannot set a property on an immutable edge!"};
    }
    const auto prop_key =
        std::visit([property_name](auto *impl) { return impl->NameToProperty(property_name); }, e->from.graph->impl);
    const auto result = e->impl.SetProperty(prop_key, ToPropertyValue(*property_value, GetNameIdMapper(e->from.graph)));

    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::DELETED_OBJECT:
          throw DeletedObjectException{"Cannot set the properties of a deleted edge!"};
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when setting a property of an edge!");
        case memgraph::storage::Error::PROPERTIES_DISABLED:
          throw std::logic_error{"Cannot set the properties of edges, because properties on edges are disabled!"};
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when setting a property of an edge.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize setting a property of an edge."};
      }
    }

    ctx->execution_stats[memgraph::query::ExecutionStats::Key::UPDATED_PROPERTIES] += 1;
    auto *trigger_ctx_collector = e->from.graph->ctx->trigger_context_collector;
    if (!trigger_ctx_collector ||
        !trigger_ctx_collector->ShouldRegisterObjectPropertyChange<memgraph::query::EdgeAccessor>()) {
      return;
    }
    const auto old_value = memgraph::query::TypedValue(*result, GetNameIdMapper(e->from.graph));
    if (property_value->type == mgp_value_type::MGP_VALUE_TYPE_NULL) {
      e->from.graph->ctx->trigger_context_collector->RegisterRemovedObjectProperty(e->impl, prop_key, old_value);
      return;
    }
    const auto new_value = ToTypedValue(*property_value, property_value->alloc);
    e->from.graph->ctx->trigger_context_collector->RegisterSetObjectProperty(e->impl, prop_key, old_value, new_value);
  });
}

mgp_error mgp_edge_set_properties(struct mgp_edge *e, struct mgp_map *properties) {
  return WrapExceptions([=] {
    auto *ctx = e->from.graph->ctx;

#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !ctx->auth_checker->Has(e->impl, memgraph::query::AuthQuery::FineGrainedPrivilege::UPDATE)) {
      throw AuthorizationException{"Insufficient permissions for setting properties on the edge!"};
    }
#endif

    if (!MgpEdgeIsMutable(*e)) {
      throw ImmutableObjectException{"Cannot set properties of an immutable edge!"};
    }
    std::map<memgraph::storage::PropertyId, memgraph::storage::PropertyValue> props;
    for (const auto &item : properties->items) {
      props.insert(std::visit(
          [&item, &e](auto *impl) {
            return std::make_pair(impl->NameToProperty(item.first),
                                  ToPropertyValue(item.second, GetNameIdMapper(e->from.graph)));
          },
          e->from.graph->impl));
    }

    const auto result = e->impl.UpdateProperties(props);
    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::DELETED_OBJECT:
          throw DeletedObjectException{"Cannot set the properties of a deleted edge!"};
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when setting a property of an edge!");
        case memgraph::storage::Error::PROPERTIES_DISABLED:
          throw std::logic_error{"Cannot set the properties of edges, because properties on edges are disabled!"};
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when setting a property of an edge.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize setting a property of an edge."};
      }
    }

    ctx->execution_stats[memgraph::query::ExecutionStats::Key::UPDATED_PROPERTIES] +=
        static_cast<int64_t>(properties->items.size());

    auto *trigger_ctx_collector = ctx->trigger_context_collector;
    if (!trigger_ctx_collector ||
        !trigger_ctx_collector->ShouldRegisterObjectPropertyChange<memgraph::query::EdgeAccessor>()) {
      return;
    }
    for (const auto &res : *result) {
      const auto property_key = std::get<0>(res);
      const auto old_value = memgraph::query::TypedValue(std::get<1>(res), GetNameIdMapper(e->from.graph));
      const auto new_value = memgraph::query::TypedValue(std::get<2>(res), GetNameIdMapper(e->from.graph));

      if (new_value.IsNull()) {
        trigger_ctx_collector->RegisterRemovedObjectProperty(e->impl, property_key, old_value);
        continue;
      }

      trigger_ctx_collector->RegisterSetObjectProperty(e->impl, property_key, old_value, new_value);
    }
  });
}

mgp_error mgp_edge_iter_properties(mgp_edge *e, mgp_memory *memory, mgp_properties_iterator **result) {
  // NOTE: This copies the whole properties into iterator.
  // TODO: Think of a good way to avoid the copy which doesn't just rely on some
  // assumption that storage may return a pointer to the property store. This
  // will probably require a different API in storage.
  return WrapExceptions(
      [e, memory] {
        auto view = e->from.graph->view;
        auto maybe_props = e->impl.Properties(view);
        if (maybe_props.HasError()) {
          switch (maybe_props.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot get the properties of a deleted edge!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL(
                  "Query modules shouldn't have access to nonexistent objects when getting the properties of an "
                  "edge.");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              LOG_FATAL("Unexpected error when getting the properties of an edge.");
          }
        }
        return NewRawMgpObject<mgp_properties_iterator>(memory, e->from.graph, std::move(*maybe_props));
      },
      result);
}

mgp_error mgp_graph_get_vertex_by_id(mgp_graph *graph, mgp_vertex_id id, mgp_memory *memory, mgp_vertex **result) {
  return WrapExceptions(
      [graph, id, memory]() -> mgp_vertex * {
        auto maybe_vertex = std::visit(
            [graph, id](auto *impl) {
              return impl->FindVertex(memgraph::storage::Gid::FromInt(id.as_int), graph->view);
            },
            graph->impl);
        if (maybe_vertex) {
          return std::visit(memgraph::utils::Overloaded{
                                [memory, graph, maybe_vertex](memgraph::query::DbAccessor *) {
                                  return NewRawMgpObject<mgp_vertex>(memory, *maybe_vertex, graph);
                                },
                                [memory, graph, maybe_vertex](memgraph::query::SubgraphDbAccessor *impl) {
                                  return NewRawMgpObject<mgp_vertex>(
                                      memory, memgraph::query::SubgraphVertexAccessor(*maybe_vertex, impl->getGraph()),
                                      graph);
                                }},
                            graph->impl);
        }
        return nullptr;
      },
      result);
}

mgp_error mgp_create_label_index(mgp_graph *graph, const char *label, int *result) {
  return WrapExceptions(
      [graph, label]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        const auto index_res =
            std::visit(memgraph::utils::Overloaded{
                           [label_id](memgraph::query::DbAccessor *impl) { return impl->CreateIndex(label_id); },
                           [label_id](memgraph::query::SubgraphDbAccessor *impl) {
                             return impl->GetAccessor()->CreateIndex(label_id);
                           }},
                       graph->impl);
        return index_res.HasError() ? 0 : 1;
      },
      result);
}

mgp_error mgp_drop_label_index(mgp_graph *graph, const char *label, int *result) {
  return WrapExceptions(
      [graph, label]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        const auto index_res =
            std::visit(memgraph::utils::Overloaded{
                           [label_id](memgraph::query::DbAccessor *impl) { return impl->DropIndex(label_id); },
                           [label_id](memgraph::query::SubgraphDbAccessor *impl) {
                             return impl->GetAccessor()->DropIndex(label_id);
                           }},
                       graph->impl);
        return index_res.HasError() ? 0 : 1;
      },
      result);
}

mgp_error mgp_list_all_label_indices(mgp_graph *graph, mgp_memory *memory, mgp_list **result) {
  return WrapExceptions([graph, memory, result]() {
    const auto index_res = std::visit(
        memgraph::utils::Overloaded{
            [](memgraph::query::DbAccessor *impl) { return impl->ListAllIndices().label; },
            [](memgraph::query::SubgraphDbAccessor *impl) { return impl->GetAccessor()->ListAllIndices().label; }},
        graph->impl);
    if (const auto err = mgp_list_make_empty(index_res.size(), memory, result); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Listing all label indices failed due to failure of creating list");
    }
    for (const auto &label : index_res) {
      const auto label_id_str = std::visit([label](const auto *impl) { return impl->LabelToName(label); }, graph->impl);

      mgp_value *label_value = nullptr;
      if (const auto err_str = mgp_value_make_string(label_id_str.c_str(), memory, &label_value);
          err_str != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error("Listing all label indices failed due to failure of creating label value");
      }
      if (const auto err_list = mgp_list_append_extend(*result, label_value);
          err_list != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error("Listing all label indices failed due to failure of appending label value");
      }
      mgp_value_destroy(label_value);
    }
  });
}

mgp_error mgp_create_label_property_index(mgp_graph *graph, const char *label, const char *property, int *result) {
  return WrapExceptions(
      [graph, label, property]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        const auto property_path = std::visit(
            [property](auto *impl) {
              const auto property_path_as_str =
                  std::string_view{property} | ranges::views::split('.') | ranges::to<std::vector<std::string>>();
              return property_path_as_str | ranges::views::transform([impl](const auto &property_id) {
                       return impl->NameToProperty(property_id);
                     }) |
                     ranges::to<std::vector<memgraph::storage::PropertyId>>();
            },
            graph->impl);
        const auto index_res = std::visit(
            memgraph::utils::Overloaded{[label_id, property_path](memgraph::query::DbAccessor *impl) {
                                          return impl->CreateIndex(label_id, {property_path});
                                        },
                                        [label_id, property_path](memgraph::query::SubgraphDbAccessor *impl) {
                                          return impl->GetAccessor()->CreateIndex(label_id, {property_path});
                                        }},
            graph->impl);
        return index_res.HasError() ? 0 : 1;
      },
      result);
}

mgp_error mgp_drop_label_property_index(mgp_graph *graph, const char *label, const char *property, int *result) {
  return WrapExceptions(
      [graph, label, property]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        const auto property_path = std::visit(
            [property](auto *impl) {
              const auto property_path_as_str =
                  std::string_view{property} | ranges::views::split('.') | ranges::to<std::vector<std::string>>();
              return property_path_as_str | ranges::views::transform([impl](const auto &property_id) {
                       return impl->NameToProperty(property_id);
                     }) |
                     ranges::to<std::vector<memgraph::storage::PropertyId>>();
            },
            graph->impl);

        const auto index_res = std::visit(
            memgraph::utils::Overloaded{[label_id, property_path](memgraph::query::DbAccessor *impl) {
                                          return impl->DropIndex(label_id, {property_path});
                                        },
                                        [label_id, property_path](memgraph::query::SubgraphDbAccessor *impl) {
                                          return impl->GetAccessor()->DropIndex(label_id, {property_path});
                                        }},
            graph->impl);
        return index_res.HasError() ? 0 : 1;
      },
      result);
}

mgp_error create_and_append_label_property_to_mgp_list(
    mgp_graph *graph, mgp_memory *memory, mgp_list **result,
    std::pair<memgraph::storage::LabelId, memgraph::storage::PropertyPath> label_property_pair) {
  return WrapExceptions([graph, memory, result, &label_property_pair]() {
    const auto label_id_str = std::visit(
        [label_id = label_property_pair.first](const auto *impl) { return impl->LabelToName(label_id); }, graph->impl);
    const auto property_path_str = std::visit(
        [property_path = label_property_pair.second](const auto *impl) {
          return property_path | ranges::views::transform([impl](const auto &property_id) {
                   return impl->PropertyToName(property_id);
                 }) |
                 ranges::views::join('.') | ranges::to<std::string>();
        },
        graph->impl);

    // This is hack to avoid dealing with pairs
    mgp_value *label_property = nullptr;
    auto final_str = label_id_str + ":";
    final_str += property_path_str;

    if (const auto err_str = mgp_value_make_string(final_str.c_str(), memory, &label_property);
        err_str != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Creating a list of label+property pairs failed due to failure of creating label+property value");
    }
    if (const auto err_list = mgp_list_append_extend(*result, label_property);
        err_list != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Creating a list of label-property pairs due to failure of appending label+property value");
    }

    mgp_value_destroy(label_property);
  });
}

mgp_error mgp_list_all_label_property_indices(mgp_graph *graph, mgp_memory *memory, mgp_list **result) {
  return WrapExceptions([graph, memory, result]() {
    const auto index_res =
        std::visit(memgraph::utils::Overloaded{
                       [](memgraph::query::DbAccessor *impl) { return impl->ListAllIndices().label_properties; },
                       [](memgraph::query::SubgraphDbAccessor *impl) {
                         return impl->GetAccessor()->ListAllIndices().label_properties;
                       }},
                   graph->impl);

    if (const auto err = mgp_list_make_empty(index_res.size(), memory, result); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Listing all label+property indices failed due to failure of creating list");
    }

    for (const auto &[label, property_paths] : index_res) {
      if (property_paths.size() != 1) {  // TODO: Support composite indices
        continue;
      }

      if (const auto err =
              create_and_append_label_property_to_mgp_list(graph, memory, result, std::pair{label, property_paths[0]});
          err != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error(
            "Listing all label+property indices failed due to failure of appending label+property value");
      }
    }
  });
}

mgp_error mgp_create_existence_constraint(mgp_graph *graph, const char *label, const char *property, int *result) {
  return WrapExceptions(
      [graph, label, property]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        const auto property_id =
            std::visit([property](auto *impl) { return impl->NameToProperty(property); }, graph->impl);
        const auto exist_res = std::visit(
            memgraph::utils::Overloaded{[label_id, property_id](memgraph::query::DbAccessor *impl) {
                                          return impl->CreateExistenceConstraint(label_id, property_id);
                                        },
                                        [label_id, property_id](memgraph::query::SubgraphDbAccessor *impl) {
                                          return impl->GetAccessor()->CreateExistenceConstraint(label_id, property_id);
                                        }},
            graph->impl);
        return exist_res.HasError() ? 0 : 1;
      },
      result);
}

mgp_error mgp_drop_existence_constraint(mgp_graph *graph, const char *label, const char *property, int *result) {
  return WrapExceptions(
      [graph, label, property]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        const auto property_id =
            std::visit([property](auto *impl) { return impl->NameToProperty(property); }, graph->impl);
        const auto exist_res = std::visit(
            memgraph::utils::Overloaded{[label_id, property_id](memgraph::query::DbAccessor *impl) {
                                          return impl->DropExistenceConstraint(label_id, property_id);
                                        },
                                        [label_id, property_id](memgraph::query::SubgraphDbAccessor *impl) {
                                          return impl->GetAccessor()->DropExistenceConstraint(label_id, property_id);
                                        }},
            graph->impl);
        return exist_res.HasError() ? 0 : 1;
      },
      result);
}

mgp_error mgp_list_all_existence_constraints(mgp_graph *graph, mgp_memory *memory, mgp_list **result) {
  return WrapExceptions([graph, memory, result]() {
    const auto constraint_res =
        std::visit(memgraph::utils::Overloaded{
                       [](memgraph::query::DbAccessor *impl) { return impl->ListAllConstraints().existence; },
                       [](memgraph::query::SubgraphDbAccessor *impl) {
                         return impl->GetAccessor()->ListAllConstraints().existence;
                       }},
                   graph->impl);

    if (const auto err = mgp_list_make_empty(constraint_res.size(), memory, result);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Listing all existence constraints failed due to failure of creating a list");
    }

    for (const auto &label_property_pair : constraint_res) {
      auto property_path = memgraph::storage::PropertyPath{label_property_pair.second};
      if (property_path.size() != 1) {  // TODO: Support composite indices
        continue;
      }
      if (const auto err = create_and_append_label_property_to_mgp_list(
              graph, memory, result, std::pair{label_property_pair.first, property_path[0]});
          err != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error(
            "Listing all existence constraints failed due to failure of appending label+property value");
      }
    }
  });
}

mgp_error mgp_create_unique_constraint(mgp_graph *graph, const char *label, mgp_list *properties, int *result) {
  return WrapExceptions(
      [graph, label, properties]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        std::set<memgraph::storage::PropertyId> property_ids;
        for (const auto &elem : properties->elems) {
          if (elem.type != mgp_value_type::MGP_VALUE_TYPE_STRING) {
            throw std::logic_error("Expected a string in the list of properties");
          }
          property_ids.insert(std::visit(
              [prop_str = elem.string_v](auto *impl) { return impl->NameToProperty(prop_str); }, graph->impl));
        }

        const auto unique_res = std::visit(
            memgraph::utils::Overloaded{[label_id, property_ids](memgraph::query::DbAccessor *impl) {
                                          return impl->CreateUniqueConstraint(label_id, property_ids);
                                        },
                                        [label_id, property_ids](memgraph::query::SubgraphDbAccessor *impl) {
                                          return impl->GetAccessor()->CreateUniqueConstraint(label_id, property_ids);
                                        }},
            graph->impl);
        return unique_res.HasError() ? 0 : 1;
      },
      result);
}

mgp_error mgp_drop_unique_constraint(mgp_graph *graph, const char *label, mgp_list *properties, int *result) {
  return WrapExceptions(
      [graph, label, properties]() {
        const auto label_id = std::visit([label](auto *impl) { return impl->NameToLabel(label); }, graph->impl);
        std::set<memgraph::storage::PropertyId> property_ids;
        for (const auto &elem : properties->elems) {
          if (elem.type != mgp_value_type::MGP_VALUE_TYPE_STRING) {
            throw std::logic_error("Expected a string in the list of properties");
          }
          property_ids.insert(std::visit(
              [prop_str = elem.string_v](auto *impl) { return impl->NameToProperty(prop_str); }, graph->impl));
        }

        const auto unique_res = std::visit(
            memgraph::utils::Overloaded{[label_id, property_ids](memgraph::query::DbAccessor *impl) {
                                          return impl->DropUniqueConstraint(label_id, property_ids);
                                        },
                                        [label_id, property_ids](memgraph::query::SubgraphDbAccessor *impl) {
                                          return impl->GetAccessor()->DropUniqueConstraint(label_id, property_ids);
                                        }},
            graph->impl);
        return unique_res == memgraph::storage::UniqueConstraints::DeletionStatus::SUCCESS ? 1 : 0;
      },
      result);
}

mgp_error mgp_list_all_unique_constraints(mgp_graph *graph, mgp_memory *memory, mgp_list **result) {
  return WrapExceptions([graph, memory, result]() {
    const auto constraints_res = std::visit(
        memgraph::utils::Overloaded{
            [](memgraph::query::DbAccessor *impl) { return impl->ListAllConstraints().unique; },
            [](memgraph::query::SubgraphDbAccessor *impl) { return impl->GetAccessor()->ListAllConstraints().unique; }},
        graph->impl);

    if (const auto err = mgp_list_make_empty(constraints_res.size(), memory, result);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Listing all unique constraints failed due to failure of creating a list");
    }

    for (const auto &label_properties_pair : constraints_res) {
      const std::string label_id_str =
          std::visit([label_id = label_properties_pair.first](const auto *impl) { return impl->LabelToName(label_id); },
                     graph->impl);
      const std::vector<std::string> properties_str = std::visit(
          [property_ids = label_properties_pair.second](const auto *impl) {
            std::vector<std::string> property_ids_str;
            property_ids_str.reserve(property_ids.size());
            std::transform(property_ids.begin(), property_ids.end(), std::back_inserter(property_ids_str),
                           [impl](const auto &property_id) { return impl->PropertyToName(property_id); });
            return property_ids_str;
          },
          graph->impl);

      mgp_list *label_properties_mgp_list = nullptr;
      if (const auto properties_mgp_list_err =
              mgp_list_make_empty(properties_str.size() + 1, memory, &label_properties_mgp_list);
          properties_mgp_list_err != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error("Listing all unique constraints failed due to failure of creating an inner list");
      }

      mgp_value *mgp_value_label = nullptr;
      if (const auto err_label = mgp_value_make_string(label_id_str.c_str(), memory, &mgp_value_label);
          err_label != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error("Listing all unique constraints failed due to failure of creating a label value");
      }
      if (const auto err_label_into_list = mgp_list_append_extend(label_properties_mgp_list, mgp_value_label);
          err_label_into_list != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error("Listing all unique constraints failed due to failure of appending a label value");
      }

      mgp_value_destroy(mgp_value_label);

      for (const std::string &property_str : properties_str) {
        mgp_value *property_mgp_value = nullptr;
        if (const auto err_str = mgp_value_make_string(property_str.c_str(), memory, &property_mgp_value);
            err_str != mgp_error::MGP_ERROR_NO_ERROR) {
          throw std::logic_error("Listing all unique constraints failed due to failure of creating a property value");
        }
        if (const auto err_list = mgp_list_append_extend(label_properties_mgp_list, property_mgp_value);
            err_list != mgp_error::MGP_ERROR_NO_ERROR) {
          throw std::logic_error("Listing all unique constraints failed due to failure of appending a property value");
        }
        mgp_value_destroy(property_mgp_value);
      }
      mgp_value value(label_properties_mgp_list, label_properties_mgp_list->GetMemoryResource());

      if (const auto err_list = mgp_list_append_extend(*result, &value); err_list != mgp_error::MGP_ERROR_NO_ERROR) {
        throw std::logic_error("Listing all unique constraints failed due to failure of creating label+property value");
      }
    }
  });
}

mgp_error mgp_graph_is_transactional(mgp_graph *graph, int *result) {
  *result = IsTransactional(graph->storage_mode) ? 1 : 0;
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_graph_is_mutable(mgp_graph *graph, int *result) {
  *result = MgpGraphIsMutable(*graph) ? 1 : 0;
  return mgp_error::MGP_ERROR_NO_ERROR;
};

mgp_error mgp_graph_create_vertex(struct mgp_graph *graph, mgp_memory *memory, mgp_vertex **result) {
  return WrapExceptions(
      [=]() -> mgp_vertex * {

#ifdef MG_ENTERPRISE
        if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && graph->ctx &&
            graph->ctx->auth_checker &&
            !graph->ctx->auth_checker->HasGlobalPrivilegeOnVertices(
                memgraph::query::AuthQuery::FineGrainedPrivilege::CREATE_DELETE)) {
          throw AuthorizationException{"Insufficient permissions for creating vertices!"};
        }
#endif

        if (!MgpGraphIsMutable(*graph)) {
          throw ImmutableObjectException{"Cannot create a vertex in an immutable graph!"};
        }
        auto *vertex = std::visit(
            [=](auto *impl) { return NewRawMgpObject<mgp_vertex>(memory, impl->InsertVertex(), graph); }, graph->impl);
        auto &ctx = graph->ctx;
        ctx->execution_stats[memgraph::query::ExecutionStats::Key::CREATED_NODES] += 1;

        if (ctx->trigger_context_collector) {
          ctx->trigger_context_collector->RegisterCreatedObject(vertex->getImpl());
        }
        return vertex;
      },
      result);
}

mgp_error mgp_graph_delete_vertex(struct mgp_graph *graph, mgp_vertex *vertex) {
  return WrapExceptions([=] {
    auto *ctx = graph->ctx;

#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !ctx->auth_checker->Has(vertex->getImpl(), graph->view,
                                memgraph::query::AuthQuery::FineGrainedPrivilege::CREATE_DELETE)) {
      throw AuthorizationException{"Insufficient permissions for deleting a vertex!"};
    }
#endif

    if (!MgpGraphIsMutable(*graph)) {
      throw ImmutableObjectException{"Cannot remove a vertex from an immutable graph!"};
    }

    const auto result =
        std::visit(memgraph::utils::Overloaded{
                       [&](memgraph::query::DbAccessor *impl) {
                         return impl->RemoveVertex(&std::get<memgraph::query::VertexAccessor>(vertex->impl));
                       },
                       [&](memgraph::query::SubgraphDbAccessor *impl) {
                         return impl->RemoveVertex(&(std::get<memgraph::query::SubgraphVertexAccessor>(vertex->impl)));
                       }},
                   graph->impl);

    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when removing a vertex!");
        case memgraph::storage::Error::DELETED_OBJECT:
        case memgraph::storage::Error::PROPERTIES_DISABLED:
          LOG_FATAL("Unexpected error when removing a vertex.");
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          throw std::logic_error{"Cannot remove a vertex that has edges!"};
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize removing a vertex."};
      }
    }

    if (!*result) {
      return;
    }

    ctx->execution_stats[memgraph::query::ExecutionStats::Key::DELETED_NODES] += 1;

    if (ctx->trigger_context_collector) {
      ctx->trigger_context_collector->RegisterDeletedObject(**result);
    }
  });
}

mgp_error mgp_graph_detach_delete_vertex(struct mgp_graph *graph, mgp_vertex *vertex) {
  return WrapExceptions([=] {
    auto *ctx = graph->ctx;
#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !ctx->auth_checker->Has(vertex->getImpl(), graph->view,
                                memgraph::query::AuthQuery::FineGrainedPrivilege::CREATE_DELETE)) {
      throw AuthorizationException{"Insufficient permissions for deleting a vertex!"};
    }
#endif

    if (!MgpGraphIsMutable(*graph)) {
      throw ImmutableObjectException{"Cannot remove a vertex from an immutable graph!"};
    }
    const auto result = std::visit(
        memgraph::utils::Overloaded{
            [vertex](memgraph::query::DbAccessor *impl) {
              return impl->DetachRemoveVertex(&std::get<memgraph::query::VertexAccessor>(vertex->impl));
            },
            [vertex](memgraph::query::SubgraphDbAccessor *impl) {
              return impl->DetachRemoveVertex(&std::get<memgraph::query::SubgraphVertexAccessor>(vertex->impl));
            }},
        graph->impl);

    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when removing a vertex!");
        case memgraph::storage::Error::DELETED_OBJECT:
        case memgraph::storage::Error::PROPERTIES_DISABLED:
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when removing a vertex.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize removing a vertex."};
      }
    }

    if (!*result) {
      return;
    }

    ctx->execution_stats[memgraph::query::ExecutionStats::Key::DELETED_NODES] += 1;
    ctx->execution_stats[memgraph::query::ExecutionStats::Key::DELETED_EDGES] +=
        static_cast<int64_t>((*result)->second.size());

    auto *trigger_ctx_collector = ctx->trigger_context_collector;
    if (!trigger_ctx_collector) {
      return;
    }

    trigger_ctx_collector->RegisterDeletedObject((*result)->first);
    if (!trigger_ctx_collector->ShouldRegisterDeletedObject<memgraph::query::EdgeAccessor>()) {
      return;
    }
    for (const auto &edge : (*result)->second) {
      trigger_ctx_collector->RegisterDeletedObject(edge);
    }
  });
}

mgp_error mgp_graph_create_edge(mgp_graph *graph, mgp_vertex *from, mgp_vertex *to, mgp_edge_type type,
                                mgp_memory *memory, mgp_edge **result) {
  return WrapExceptions(
      [=]() -> mgp_edge * {
        auto *ctx = graph->ctx;
#ifdef MG_ENTERPRISE
        const auto edge_id =
            std::visit([type](auto *impl) { return impl->NameToEdgeType(type.name); }, from->graph->impl);
        if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
            !ctx->auth_checker->Has(edge_id, memgraph::query::AuthQuery::FineGrainedPrivilege::CREATE_DELETE)) {
          throw AuthorizationException{"Insufficient permissions for creating edges!"};
        }
#endif
        if (!MgpGraphIsMutable(*graph)) {
          throw ImmutableObjectException{"Cannot create an edge in an immutable graph!"};
        }
        auto edge =
            std::visit(memgraph::utils::Overloaded{
                           [from, to, type](memgraph::query::DbAccessor *impl) {
                             return impl->InsertEdge(&std::get<memgraph::query::VertexAccessor>(from->impl),
                                                     &std::get<memgraph::query::VertexAccessor>(to->impl),
                                                     impl->NameToEdgeType(type.name));
                           },
                           [from, to, type](memgraph::query::SubgraphDbAccessor *impl) {
                             return impl->InsertEdge(&std::get<memgraph::query::SubgraphVertexAccessor>(from->impl),
                                                     &std::get<memgraph::query::SubgraphVertexAccessor>(to->impl),
                                                     impl->NameToEdgeType(type.name));
                           }},
                       graph->impl);

        if (edge.HasError()) {
          switch (edge.GetError()) {
            case memgraph::storage::Error::DELETED_OBJECT:
              throw DeletedObjectException{"Cannot add an edge to a deleted vertex!"};
            case memgraph::storage::Error::NONEXISTENT_OBJECT:
              LOG_FATAL("Query modules shouldn't have access to nonexistent objects when creating an edge!");
            case memgraph::storage::Error::PROPERTIES_DISABLED:
            case memgraph::storage::Error::VERTEX_HAS_EDGES:
              LOG_FATAL("Unexpected error when creating an edge.");
            case memgraph::storage::Error::SERIALIZATION_ERROR:
              throw SerializationException{"Cannot serialize creating an edge."};
          }
        }

        ctx->execution_stats[memgraph::query::ExecutionStats::Key::CREATED_EDGES] += 1;

        if (ctx->trigger_context_collector) {
          ctx->trigger_context_collector->RegisterCreatedObject(*edge);
        }
        return std::visit(
            memgraph::utils::Overloaded{
                [memory, edge, from](memgraph::query::DbAccessor *) {
                  return NewRawMgpObject<mgp_edge>(memory->impl, edge.GetValue(), from->graph);
                },
                [memory, edge, from](memgraph::query::SubgraphDbAccessor *db_impl) {
                  const auto &v_from =
                      memgraph::query::SubgraphVertexAccessor(edge.GetValue().From(), db_impl->getGraph());
                  const auto &v_to = memgraph::query::SubgraphVertexAccessor(edge.GetValue().To(), db_impl->getGraph());
                  return NewRawMgpObject<mgp_edge>(memory->impl, edge.GetValue(), v_from, v_to, from->graph);
                }},
            graph->impl);
      },
      result);
}

mgp_error mgp_graph_delete_edge(struct mgp_graph *graph, mgp_edge *edge) {
  return WrapExceptions([=] {
    auto *ctx = graph->ctx;
#ifdef MG_ENTERPRISE
    if (memgraph::license::global_license_checker.IsEnterpriseValidFast() && ctx && ctx->auth_checker &&
        !ctx->auth_checker->Has(edge->impl, memgraph::query::AuthQuery::FineGrainedPrivilege::CREATE_DELETE)) {
      throw AuthorizationException{"Insufficient permissions for deleting an edge!"};
    }
#endif
    if (!MgpGraphIsMutable(*graph)) {
      throw ImmutableObjectException{"Cannot remove an edge from an immutable graph!"};
    }

    const auto result = std::visit([edge](auto *impl) { return impl->RemoveEdge(&edge->impl); }, graph->impl);
    if (result.HasError()) {
      switch (result.GetError()) {
        case memgraph::storage::Error::NONEXISTENT_OBJECT:
          LOG_FATAL("Query modules shouldn't have access to nonexistent objects when removing an edge!");
        case memgraph::storage::Error::DELETED_OBJECT:
        case memgraph::storage::Error::PROPERTIES_DISABLED:
        case memgraph::storage::Error::VERTEX_HAS_EDGES:
          LOG_FATAL("Unexpected error when removing an edge.");
        case memgraph::storage::Error::SERIALIZATION_ERROR:
          throw SerializationException{"Cannot serialize removing an edge."};
      }
    }

    if (!*result) {
      return;
    }

    ctx->execution_stats[memgraph::query::ExecutionStats::Key::DELETED_EDGES] += 1;
    if (ctx->trigger_context_collector) {
      ctx->trigger_context_collector->RegisterDeletedObject(**result);
    }
  });
}

mgp_error mgp_graph_has_text_index(mgp_graph *graph, const char *index_name, int *result) {
  return WrapExceptions([graph, index_name, result]() {
    std::visit(memgraph::utils::Overloaded{
                   [&](memgraph::query::DbAccessor *impl) { *result = impl->TextIndexExists(index_name); },
                   [&](memgraph::query::SubgraphDbAccessor *impl) {
                     *result = impl->GetAccessor()->TextIndexExists(index_name);
                   }},
               graph->impl);
  });
}

mgp_vertex *GetVertexByGid(mgp_graph *graph, memgraph::storage::Gid id, mgp_memory *memory) {
  auto get_vertex_by_gid = memgraph::utils::Overloaded{
      [graph, id, memory](memgraph::query::DbAccessor *impl) -> mgp_vertex * {
        auto maybe_vertex = impl->FindVertex(id, graph->view);
        if (!maybe_vertex) return nullptr;
        return NewRawMgpObject<mgp_vertex>(memory, *maybe_vertex, graph);
      },
      [graph, id, memory](memgraph::query::SubgraphDbAccessor *impl) -> mgp_vertex * {
        auto maybe_vertex = impl->FindVertex(id, graph->view);
        if (!maybe_vertex) return nullptr;
        return NewRawMgpObject<mgp_vertex>(
            memory, memgraph::query::SubgraphVertexAccessor(*maybe_vertex, impl->getGraph()), graph);
      }};
  return std::visit(get_vertex_by_gid, graph->impl);
}

template <typename FoundElementRange, typename MakeElementValueFunc>
void WrapVectorSearchResults(mgp_graph *graph, mgp_memory *memory, mgp_map **result, size_t found_elements_size,
                             MakeElementValueFunc make_element_value, const FoundElementRange &found_elements,
                             const std::optional<std::string> &error_msg) {
  if (const auto err = mgp_map_make_empty(memory, result); err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during creation of a mgp_map");
  }

  mgp_value *error_value = nullptr;
  if (error_msg.has_value()) {
    if (const auto err = mgp_value_make_string(error_msg.value().data(), memory, &error_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }
    if (const auto err = mgp_map_insert(*result, "error_msg", error_value); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search error failed during insertion into mgp_map");
    }
    return;
  }

  mgp_list *search_results = nullptr;
  if (const auto err = mgp_list_make_empty(found_elements_size, memory, &search_results);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during creation of a mgp_list");
  }

  for (const auto &[element_acc, distance, similarity] : found_elements) {
    mgp_value *element_value = nullptr;
    mgp_value *distance_value = nullptr;
    mgp_value *similarity_value = nullptr;

    if (const auto err = make_element_value(element_acc, memory, graph, &element_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of an element mgp_value");
    }

    if (const auto err = mgp_value_make_double(distance, memory, &distance_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a double mgp_value");
    }

    if (const auto err = mgp_value_make_double(similarity, memory, &similarity_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a double mgp_value");
    }

    mgp_list *element_distance_similarity = nullptr;
    if (const auto err = mgp_list_make_empty(3, memory, &element_distance_similarity);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a mgp_list");
    }

    if (const auto err = mgp_list_append_extend(element_distance_similarity, element_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Retrieving vector search results failed during insertion of the mgp_value into the result list");
    }

    if (const auto err = mgp_list_append_extend(element_distance_similarity, distance_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Retrieving vector search results failed during insertion of the mgp_value into the result list");
    }

    if (const auto err = mgp_list_append_extend(element_distance_similarity, similarity_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Retrieving vector search results failed during insertion of the mgp_value into the result list");
    }

    mgp_value *element_distance_similarity_value = nullptr;
    if (const auto err = mgp_value_make_list(element_distance_similarity, &element_distance_similarity_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a list mgp_value");
    }

    if (const auto err = mgp_list_append(search_results, element_distance_similarity_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Retrieving vector search results failed during insertion of the mgp_value into the result list");
    }

    mgp_value_destroy(element_distance_similarity_value);
    mgp_value_destroy(element_value);
    mgp_value_destroy(distance_value);
    mgp_value_destroy(similarity_value);
  }

  mgp_value *search_results_value = nullptr;
  if (const auto err = mgp_value_make_list(search_results, &search_results_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during creation of a list mgp_value");
  }

  if (const auto err = mgp_map_insert(*result, "search_results", search_results_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during insertion into mgp_map");
  }

  mgp_value_destroy(error_value);
  mgp_value_destroy(search_results_value);
}

void WrapVectorSearchResults(
    mgp_graph *graph, mgp_memory *memory, mgp_map **result,
    const std::vector<std::tuple<memgraph::storage::VertexAccessor, double, double>> &found_vertices,
    const std::optional<std::string> &error_msg = std::nullopt) {
  auto make_vertex_value = [](const memgraph::storage::VertexAccessor &storage_vertex_acc, mgp_memory *memory,
                              mgp_graph *graph, mgp_value **out) {
    auto query_vertex_acc = memgraph::query::VertexAccessor(storage_vertex_acc);
    return mgp_value_make_vertex(NewRawMgpObject<mgp_vertex>(memory, query_vertex_acc, graph), out);
  };
  WrapVectorSearchResults(graph, memory, result, found_vertices.size(), make_vertex_value, found_vertices, error_msg);
}

void WrapVectorSearchOnEdgesResults(
    mgp_graph *graph, mgp_memory *memory, mgp_map **result,
    const std::vector<std::tuple<memgraph::storage::EdgeAccessor, double, double>> &found_edges,
    const std::optional<std::string> &error_msg = std::nullopt) {
  auto make_edge_value = [](const memgraph::storage::EdgeAccessor &storage_edge_acc, mgp_memory *memory,
                            mgp_graph *graph, mgp_value **out) {
    auto query_edge_acc = memgraph::query::EdgeAccessor(storage_edge_acc);
    return mgp_value_make_edge(NewRawMgpObject<mgp_edge>(memory, query_edge_acc, graph), out);
  };
  WrapVectorSearchResults(graph, memory, result, found_edges.size(), make_edge_value, found_edges, error_msg);
}

template <typename InfoType, typename GetNameFunc>
void AppendIndexInfoList(const std::vector<InfoType> &info, mgp_memory *memory, mgp_list *search_results,
                         GetNameFunc get_name, memgraph::query::DbAccessor *impl,
                         memgraph::storage::VectorIndexType index_type) {
  for (const auto &[index_name, label_or_edge_type, property, metric, dimension, capacity, size, scalar_kind] : info) {
    mgp_value *index_name_value = nullptr;
    mgp_value *label_value = nullptr;
    mgp_value *property_value = nullptr;
    mgp_value *metric_value = nullptr;
    mgp_value *dimension_value = nullptr;
    mgp_value *capacity_value = nullptr;
    mgp_value *size_value = nullptr;
    mgp_value *scalar_kind_value = nullptr;
    mgp_value *index_type_value = nullptr;

    if (const auto err = mgp_value_make_string(index_name.c_str(), memory, &index_name_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }

    if (const auto err = mgp_value_make_string(get_name(label_or_edge_type).c_str(), memory, &label_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }

    if (const auto err = mgp_value_make_string(impl->PropertyToName(property).c_str(), memory, &property_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }

    if (const auto err = mgp_value_make_string(metric.c_str(), memory, &metric_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }

    if (const auto err = mgp_value_make_int(static_cast<int64_t>(dimension), memory, &dimension_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a int mgp_value");
    }

    if (const auto err = mgp_value_make_int(static_cast<int64_t>(capacity), memory, &capacity_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a int mgp_value");
    }

    if (const auto err = mgp_value_make_int(static_cast<int64_t>(size), memory, &size_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a int mgp_value");
    }

    if (const auto err = mgp_value_make_string(scalar_kind.c_str(), memory, &scalar_kind_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }

    if (const auto err =
            mgp_value_make_string(memgraph::storage::VectorIndexTypeToString(index_type), memory, &index_type_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }

    mgp_list *index_info = nullptr;
    if (const auto err = mgp_list_make_empty(9, memory, &index_info); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a mgp_list");
    }

    if (mgp_list_append_extend(index_info, index_name_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, label_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, property_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, metric_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, dimension_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, capacity_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, size_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, scalar_kind_value) != mgp_error::MGP_ERROR_NO_ERROR ||
        mgp_list_append_extend(index_info, index_type_value) != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Retrieving vector search results failed during insertion of the mgp_value into the result list");
    }

    mgp_value *index_info_value = nullptr;
    if (const auto err = mgp_value_make_list(index_info, &index_info_value); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a list mgp_value");
    }

    if (const auto err = mgp_list_append(search_results, index_info_value); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Retrieving vector search results failed during insertion of the mgp_value into the result list");
    }

    mgp_value_destroy(index_info_value);
    mgp_value_destroy(index_name_value);
    mgp_value_destroy(label_value);
    mgp_value_destroy(property_value);
    mgp_value_destroy(metric_value);
    mgp_value_destroy(dimension_value);
    mgp_value_destroy(capacity_value);
    mgp_value_destroy(size_value);
    mgp_value_destroy(scalar_kind_value);
    mgp_value_destroy(index_type_value);
  }
}

void WrapVectorIndexInfoResult(mgp_memory *memory, mgp_map **result,
                               const std::vector<memgraph::storage::VectorIndexInfo> &info,
                               const std::vector<memgraph::storage::VectorEdgeIndexInfo> &edge_info,
                               const std::optional<std::string> &error_msg = std::nullopt,
                               memgraph::query::DbAccessor *impl = nullptr) {
  if (const auto err = mgp_map_make_empty(memory, result); err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during creation of a mgp_map");
  }

  mgp_value *error_value = nullptr;
  if (error_msg.has_value()) {
    if (const auto err = mgp_value_make_string(error_msg.value().data(), memory, &error_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search results failed during creation of a string mgp_value");
    }
    if (const auto err = mgp_map_insert(*result, "error_msg", error_value); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving vector search error failed during insertion into mgp_map");
    }
    return;
  }

  mgp_list *search_results = nullptr;
  size_t total_size = info.size() + edge_info.size();
  if (const auto err = mgp_list_make_empty(total_size, memory, &search_results); err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during creation of a mgp_list");
  }

  AppendIndexInfoList(
      info, memory, search_results, [impl](auto label_id) { return impl->LabelToName(label_id); }, impl,
      memgraph::storage::VectorIndexType::ON_NODES);

  AppendIndexInfoList(
      edge_info, memory, search_results, [impl](auto edge_type_id) { return impl->EdgeTypeToName(edge_type_id); }, impl,
      memgraph::storage::VectorIndexType::ON_EDGES);

  mgp_value *search_results_value = nullptr;
  if (const auto err = mgp_value_make_list(search_results, &search_results_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during creation of a list mgp_value");
  }

  if (const auto err = mgp_map_insert(*result, "search_results", search_results_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving vector search results failed during insertion into mgp_map");
  }

  mgp_value_destroy(error_value);
  mgp_value_destroy(search_results_value);
}

void WrapTextSearch(mgp_graph *graph, mgp_memory *memory, mgp_map **result,
                    const std::vector<memgraph::storage::Gid> &vertex_ids = {},
                    const std::optional<std::string> &error_msg = std::nullopt) {
  if (const auto err = mgp_map_make_empty(memory, result); err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving text search results failed during creation of a mgp_map");
  }

  mgp_value *error_value = nullptr;
  if (error_msg.has_value()) {
    if (const auto err = mgp_value_make_string(error_msg.value().data(), memory, &error_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving text search results failed during creation of a string mgp_value");
    }
    if (const auto err = mgp_map_insert(*result, "error_msg", error_value); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving text search error failed during insertion into mgp_map");
    }
    mgp_value_destroy(error_value);
    return;
  }

  // first find vertices by their GIDs because maybe not all vertices exist in the graph anymore
  std::vector<mgp_vertex *> vertices;
  vertices.reserve(vertex_ids.size());
  for (const auto &vertex_id : vertex_ids) {
    auto vertex_ptr = GetVertexByGid(graph, vertex_id, memory);
    if (vertex_ptr) {
      vertices.push_back(vertex_ptr);
    }
  }

  mgp_list *search_results{};
  if (const auto err = mgp_list_make_empty(vertices.size(), memory, &search_results);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving text search results failed during creation of a mgp_list");
  }

  for (auto *vertex_ptr : vertices) {
    mgp_value *vertex = nullptr;
    if (const auto err = mgp_value_make_vertex(vertex_ptr, &vertex); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving text search results failed during creation of a vertex mgp_value");
    }
    if (const auto err = mgp_list_append(search_results, vertex); err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error(
          "Retrieving text search results failed during insertion of the mgp_value into the result list");
    }
    mgp_value_destroy(vertex);
  }

  mgp_value *search_results_value = nullptr;
  if (const auto err = mgp_value_make_list(search_results, &search_results_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving text search results failed during creation of a list mgp_value");
  }

  if (const auto err = mgp_map_insert(*result, "search_results", search_results_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving text index search results failed during insertion into mgp_map");
  }
  mgp_value_destroy(search_results_value);
}

void WrapTextIndexAggregation(mgp_memory *memory, mgp_map **result, const std::string &aggregation_result,
                              const std::optional<std::string> &error_msg = std::nullopt) {
  if (const auto err = mgp_map_make_empty(memory, result); err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving text search results failed during creation of a mgp_map");
  }

  mgp_value *aggregation_result_or_error_value = nullptr;
  if (const auto err = mgp_value_make_string(error_msg.value_or(aggregation_result).data(), memory,
                                             &aggregation_result_or_error_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving text search results failed during creation of a string mgp_value");
  }

  if (error_msg.has_value()) {
    if (const auto err = mgp_map_insert(*result, "error_msg", aggregation_result_or_error_value);
        err != mgp_error::MGP_ERROR_NO_ERROR) {
      throw std::logic_error("Retrieving text index aggregation error failed during insertion into mgp_map");
    }
    return;
  }

  if (const auto err = mgp_map_insert(*result, "aggregation_results", aggregation_result_or_error_value);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    throw std::logic_error("Retrieving text index aggregation results failed during insertion into mgp_map");
  }
  mgp_value_destroy(aggregation_result_or_error_value);
}

mgp_error mgp_graph_search_text_index(mgp_graph *graph, const char *index_name, const char *search_query,
                                      text_search_mode search_mode, mgp_memory *memory, mgp_map **result) {
  return WrapExceptions([graph, memory, index_name, search_query, search_mode, result]() {
    std::vector<memgraph::storage::Gid> found_vertices_ids;
    std::optional<std::string> error_msg = std::nullopt;
    try {
      found_vertices_ids = graph->getImpl()->TextIndexSearch(index_name, search_query, search_mode);
    } catch (memgraph::query::QueryException &e) {
      error_msg = e.what();
    }
    WrapTextSearch(graph, memory, result, found_vertices_ids, error_msg);
  });
}

mgp_error mgp_graph_aggregate_over_text_index(mgp_graph *graph, const char *index_name, const char *search_query,
                                              const char *aggregation_query, mgp_memory *memory, mgp_map **result) {
  return WrapExceptions([graph, memory, index_name, search_query, aggregation_query, result]() {
    std::string search_results;
    std::optional<std::string> error_msg = std::nullopt;
    try {
      search_results = graph->getImpl()->TextIndexAggregate(index_name, search_query, aggregation_query);
    } catch (memgraph::query::QueryException &e) {
      error_msg = e.what();
    }
    WrapTextIndexAggregation(memory, result, search_results, error_msg);
  });
}

mgp_error mgp_graph_search_vector_index(mgp_graph *graph, const char *index_name, mgp_list *search_query,
                                        int result_size, mgp_memory *memory, mgp_map **result) {
  return WrapExceptions([graph, memory, index_name, search_query, result, result_size]() {
    std::vector<std::tuple<memgraph::storage::VertexAccessor, double, double>> found_vertices;
    std::optional<std::string> error_msg = std::nullopt;
    try {
      std::vector<float> search_query_vector;
      search_query_vector.reserve(search_query->elems.size());
      for (auto &elem : search_query->elems) {
        auto type = MgpValueGetType(elem);
        if (type == mgp_value_type::MGP_VALUE_TYPE_DOUBLE) {
          double value = 0.0;
          if (auto err = mgp_value_get_double(&elem, &value); err != mgp_error::MGP_ERROR_NO_ERROR) {
            throw std::logic_error("Failed extracting the Double value from the vector search input argument!");
          }
          search_query_vector.push_back(static_cast<float>(value));
          continue;
        }
        if (type == mgp_value_type::MGP_VALUE_TYPE_INT) {
          int64_t value = 0;
          if (auto err = mgp_value_get_int(&elem, &value); err != mgp_error::MGP_ERROR_NO_ERROR) {
            throw std::logic_error("Failed extracting the Int value from the vector search input argument!");
          }
          search_query_vector.push_back(static_cast<float>(value));
          continue;
        }
        throw std::logic_error(
            "Unrecognized argument type when performing vector search, expected values are Double or Int!");
      }
      found_vertices = graph->getImpl()->VectorIndexSearchOnNodes(index_name, result_size, search_query_vector);
    } catch (memgraph::query::QueryException &e) {
      error_msg = e.what();
    }
    WrapVectorSearchResults(graph, memory, result, found_vertices, error_msg);
  });
}

mgp_error mgp_graph_search_vector_index_on_edges(mgp_graph *graph, const char *index_name, mgp_list *search_query,
                                                 int result_size, mgp_memory *memory, mgp_map **result) {
  return WrapExceptions([graph, memory, index_name, search_query, result, result_size]() {
    std::vector<std::tuple<memgraph::storage::EdgeAccessor, double, double>> found_edges;
    std::optional<std::string> error_msg = std::nullopt;
    try {
      std::vector<float> search_query_vector;
      search_query_vector.reserve(search_query->elems.size());
      for (auto &elem : search_query->elems) {
        auto type = MgpValueGetType(elem);
        if (type == mgp_value_type::MGP_VALUE_TYPE_DOUBLE) {
          double value = 0.0;
          if (auto err = mgp_value_get_double(&elem, &value); err != mgp_error::MGP_ERROR_NO_ERROR) {
            throw std::logic_error("Failed extracting the Double value from the vector search input argument!");
          }
          search_query_vector.push_back(static_cast<float>(value));
          continue;
        }
        if (type == mgp_value_type::MGP_VALUE_TYPE_INT) {
          int64_t value = 0;
          if (auto err = mgp_value_get_int(&elem, &value); err != mgp_error::MGP_ERROR_NO_ERROR) {
            throw std::logic_error("Failed extracting the Int value from the vector search input argument!");
          }
          search_query_vector.push_back(static_cast<float>(value));
          continue;
        }
        throw std::logic_error(
            "Unrecognized argument type when performing vector search, expected values are Double or Int!");
      }
      found_edges = graph->getImpl()->VectorIndexSearchOnEdges(index_name, result_size, search_query_vector);
    } catch (memgraph::query::QueryException &e) {
      error_msg = e.what();
    }
    WrapVectorSearchOnEdgesResults(graph, memory, result, found_edges, error_msg);
  });
}

mgp_error mgp_graph_show_index_info(mgp_graph *graph, mgp_memory *memory, mgp_map **result) {
  return WrapExceptions([graph, memory, result]() {
    std::vector<memgraph::storage::VectorIndexInfo> index_info;
    std::vector<memgraph::storage::VectorEdgeIndexInfo> edge_index_info;
    std::optional<std::string> error_msg = std::nullopt;
    try {
      index_info = graph->getImpl()->ListAllVectorIndices();
      edge_index_info = graph->getImpl()->ListAllVectorEdgeIndices();
    } catch (memgraph::query::QueryException &e) {
      error_msg = e.what();
    }
    WrapVectorIndexInfoResult(memory, result, index_info, edge_index_info, error_msg, graph->getImpl());
  });
}

#ifdef MG_ENTERPRISE
namespace {
void NextPermitted(mgp_vertices_iterator &it) {
  const auto *ctx = it.graph->ctx;

  if (!ctx || !ctx->auth_checker) {
    return;
  }

  while (it.current_it != it.vertices.end()) {
    if (ctx->auth_checker->Has(*it.current_it, it.graph->view,
                               memgraph::query::AuthQuery::FineGrainedPrivilege::READ)) {
      break;
    }

    ++it.current_it;
  }
};
}  // namespace
#endif

/// @throw anything VerticesIterable may throw
mgp_vertices_iterator::mgp_vertices_iterator(mgp_graph *graph, allocator_type alloc)
    : alloc(alloc),
      graph(graph),
      vertices(std::visit([graph](auto *impl) { return impl->Vertices(graph->view); }, graph->impl)),
      current_it(vertices.begin()) {
#ifdef MG_ENTERPRISE
  if (memgraph::license::global_license_checker.IsEnterpriseValidFast()) {
    NextPermitted(*this);
  }
#endif

  if (current_it != vertices.end()) {
    std::visit(
        memgraph::utils::Overloaded{
            [this, graph, alloc](memgraph::query::DbAccessor *) { current_v.emplace(*current_it, graph, alloc); },
            [this, graph, alloc](memgraph::query::SubgraphDbAccessor *impl) {
              current_v.emplace(memgraph::query::SubgraphVertexAccessor(*current_it, impl->getGraph()), graph, alloc);
            }},
        graph->impl);
  }
}

void mgp_vertices_iterator_destroy(mgp_vertices_iterator *it) { DeleteRawMgpObject(it); }

mgp_error mgp_graph_iter_vertices(mgp_graph *graph, mgp_memory *memory, mgp_vertices_iterator **result) {
  return WrapExceptions([graph, memory] { return NewRawMgpObject<mgp_vertices_iterator>(memory, graph); }, result);
}

mgp_error mgp_graph_approximate_vertex_count(mgp_graph *graph, size_t *result) {
  return WrapExceptions([graph, result] { *result = graph->getImpl()->VerticesCount(); });
}

mgp_error mgp_graph_approximate_edge_count(mgp_graph *graph, size_t *result) {
  return WrapExceptions([graph, result] { *result = graph->getImpl()->EdgesCount(); });
}

mgp_error mgp_vertices_iterator_underlying_graph_is_mutable(mgp_vertices_iterator *it, int *result) {
  return mgp_graph_is_mutable(it->graph, result);
}

mgp_error mgp_vertices_iterator_get(mgp_vertices_iterator *it, mgp_vertex **result) {
  return WrapExceptions(
      [it]() -> mgp_vertex * {
        if (it->current_v.has_value()) {
          return &*it->current_v;
        }
        return nullptr;
      },
      result);
}

mgp_error mgp_vertices_iterator_next(mgp_vertices_iterator *it, mgp_vertex **result) {
  return WrapExceptions(
      [it]() -> mgp_vertex * {
        if (it->current_it == it->vertices.end()) {
          MG_ASSERT(!it->current_v,
                    "Iteration is already done, so it->current_v "
                    "should have been set to std::nullopt");
          return nullptr;
        }

        ++it->current_it;
#ifdef MG_ENTERPRISE
        if (memgraph::license::global_license_checker.IsEnterpriseValidFast()) {
          NextPermitted(*it);
        }
#endif
        if (it->current_it == it->vertices.end()) {
          it->current_v = std::nullopt;
          return nullptr;
        }

        memgraph::utils::OnScopeExit clean_up([it] { it->current_v = std::nullopt; });
        std::visit(memgraph::utils::Overloaded{[it](memgraph::query::DbAccessor *) {
                                                 it->current_v.emplace(*it->current_it, it->graph,
                                                                       it->GetMemoryResource());
                                               },
                                               [it](memgraph::query::SubgraphDbAccessor *impl) {
                                                 it->current_v.emplace(memgraph::query::SubgraphVertexAccessor(
                                                                           *it->current_it, impl->getGraph()),
                                                                       it->graph, it->GetMemoryResource());
                                               }},
                   it->graph->impl);

        clean_up.Disable();
        return &*it->current_v;
      },
      result);
}

/// Type System
///
/// All types are allocated globally, so that we simplify the API and minimize
/// allocations done for types.

namespace {
void NoOpCypherTypeDeleter(CypherType * /*type*/) {}
}  // namespace

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DEFINE_MGP_TYPE_GETTER(cypher_type_name, mgp_type_name)                            \
  mgp_error mgp_type_##mgp_type_name(mgp_type **result) {                                  \
    return WrapExceptions(                                                                 \
        [] {                                                                               \
          static cypher_type_name##Type impl;                                              \
          static mgp_type mgp_type_name_type{CypherTypePtr(&impl, NoOpCypherTypeDeleter)}; \
          return &mgp_type_name_type;                                                      \
        },                                                                                 \
        result);                                                                           \
  }

DEFINE_MGP_TYPE_GETTER(Any, any);
DEFINE_MGP_TYPE_GETTER(Bool, bool);
DEFINE_MGP_TYPE_GETTER(String, string);
DEFINE_MGP_TYPE_GETTER(Int, int);
DEFINE_MGP_TYPE_GETTER(Float, float);
DEFINE_MGP_TYPE_GETTER(Number, number);
DEFINE_MGP_TYPE_GETTER(Map, map);
DEFINE_MGP_TYPE_GETTER(Node, node);
DEFINE_MGP_TYPE_GETTER(Relationship, relationship);
DEFINE_MGP_TYPE_GETTER(Path, path);
DEFINE_MGP_TYPE_GETTER(Date, date);
DEFINE_MGP_TYPE_GETTER(LocalTime, local_time);
DEFINE_MGP_TYPE_GETTER(LocalDateTime, local_date_time);
DEFINE_MGP_TYPE_GETTER(Duration, duration);

mgp_error mgp_type_list(mgp_type *type, mgp_type **result) {
  return WrapExceptions(
      [type] {
        // Maps `type` to corresponding instance of ListType.
        static memgraph::utils::pmr::map<mgp_type *, mgp_type> gListTypes(memgraph::utils::NewDeleteResource());
        static memgraph::utils::SpinLock lock;
        auto guard = std::lock_guard{lock};
        auto found_it = gListTypes.find(type);
        if (found_it != gListTypes.end()) {
          return &found_it->second;
        }
        auto alloc = gListTypes.get_allocator();
        CypherTypePtr impl(
            alloc.new_object<ListType>(
                // Just obtain the pointer to original impl, don't own it.
                CypherTypePtr(type->impl.get(), NoOpCypherTypeDeleter), alloc.resource()),
            [alloc](CypherType *base_ptr) mutable { alloc.delete_object(static_cast<ListType *>(base_ptr)); });
        return &gListTypes.emplace(type, mgp_type{std::move(impl)}).first->second;
      },
      result);
}

mgp_error mgp_type_nullable(mgp_type *type, mgp_type **result) {
  return WrapExceptions(
      [type] {
        // Maps `type` to corresponding instance of NullableType.
        static memgraph::utils::pmr::map<mgp_type *, mgp_type> gNullableTypes(memgraph::utils::NewDeleteResource());
        static memgraph::utils::SpinLock lock;
        auto guard = std::lock_guard{lock};
        auto found_it = gNullableTypes.find(type);
        if (found_it != gNullableTypes.end()) return &found_it->second;

        auto alloc = gNullableTypes.get_allocator();
        auto impl = NullableType::Create(CypherTypePtr(type->impl.get(), NoOpCypherTypeDeleter), alloc.resource());
        return &gNullableTypes.emplace(type, mgp_type{std::move(impl)}).first->second;
      },
      result);
}

namespace {
/// @throw std::bad_alloc, std::length_error
mgp_proc *mgp_module_add_procedure(mgp_module *module, const char *name, mgp_proc_cb cb,
                                   const ProcedureInfo &procedure_info) {
  if (!IsValidIdentifierName(name)) {
    throw std::invalid_argument{fmt::format("Invalid procedure name: {}", name)};
  }
  if (module->procedures.find(name) != module->procedures.end()) {
    throw std::logic_error{fmt::format("Procedure already exists with name '{}'", name)};
  };

  auto *memory = module->procedures.get_allocator().resource();
  return &module->procedures.emplace(name, mgp_proc(name, cb, memory, procedure_info)).first->second;
}

/// @throw std::bad_alloc, std::length_error
mgp_proc *mgp_module_add_batch_procedure(mgp_module *module, const char *name, mgp_proc_cb cb_batch,
                                         mgp_proc_initializer initializer, mgp_proc_cleanup cleanup,
                                         const ProcedureInfo &procedure_info) {
  if (!IsValidIdentifierName(name)) {
    throw std::invalid_argument{fmt::format("Invalid procedure name: {}", name)};
  }
  if (module->procedures.find(name) != module->procedures.end()) {
    throw std::logic_error{fmt::format("Procedure already exists with name '{}'", name)};
  };
  auto *memory = module->procedures.get_allocator().resource();
  return &module->procedures.emplace(name, mgp_proc(name, cb_batch, initializer, cleanup, memory, procedure_info))
              .first->second;
}

}  // namespace

mgp_error mgp_module_add_read_procedure(mgp_module *module, const char *name, mgp_proc_cb cb, mgp_proc **result) {
  return WrapExceptions([=] { return mgp_module_add_procedure(module, name, cb, {.is_write = false}); }, result);
}

mgp_error mgp_module_add_write_procedure(mgp_module *module, const char *name, mgp_proc_cb cb, mgp_proc **result) {
  return WrapExceptions([=] { return mgp_module_add_procedure(module, name, cb, {.is_write = true}); }, result);
}

mgp_error mgp_module_add_batch_read_procedure(mgp_module *module, const char *name, mgp_proc_cb cb_batch,
                                              mgp_proc_initializer initializer, mgp_proc_cleanup cleanup,
                                              mgp_proc **result) {
  return WrapExceptions(
      [=] {
        return mgp_module_add_batch_procedure(module, name, cb_batch, initializer, cleanup,
                                              {.is_write = false, .is_batched = true});
      },
      result);
}

mgp_error mgp_module_add_batch_write_procedure(mgp_module *module, const char *name, mgp_proc_cb cb_batch,
                                               mgp_proc_initializer initializer, mgp_proc_cleanup cleanup,
                                               mgp_proc **result) {
  return WrapExceptions(
      [=] {
        return mgp_module_add_batch_procedure(module, name, cb_batch, initializer, cleanup,
                                              {.is_write = true, .is_batched = true});
      },
      result);
}

namespace {
template <typename T>
concept IsCallable = memgraph::utils::SameAsAnyOf<T, mgp_proc, mgp_func>;

template <IsCallable TCall>
mgp_error MgpAddArg(TCall &callable, const std::string &name, mgp_type &type) {
  return WrapExceptions([&]() mutable {
    static constexpr std::string_view type_name = std::invoke([]() constexpr {
      if constexpr (std::is_same_v<TCall, mgp_proc>) {
        return "procedure";
      } else if constexpr (std::is_same_v<TCall, mgp_func>) {
        return "function";
      }
    });

    if (!IsValidIdentifierName(name.c_str())) {
      throw std::invalid_argument{fmt::format("Invalid argument name for {} '{}': {}", type_name, callable.name, name)};
    }
    if (!callable.opt_args.empty()) {
      throw std::logic_error{fmt::format("Cannot add required argument '{}' to {} '{}' after adding any optional one",
                                         name, type_name, callable.name)};
    }
    callable.args.emplace_back(name, type.impl.get());
  });
}

template <IsCallable TCall>
mgp_error MgpAddOptArg(TCall &callable, const std::string name, mgp_type &type, mgp_value &default_value) {
  return WrapExceptions([&]() mutable {
    static constexpr std::string_view type_name = std::invoke([]() constexpr {
      if constexpr (std::is_same_v<TCall, mgp_proc>) {
        return "procedure";
      } else if constexpr (std::is_same_v<TCall, mgp_func>) {
        return "function";
      }
    });

    if (!IsValidIdentifierName(name.c_str())) {
      throw std::invalid_argument{fmt::format("Invalid argument name for {} '{}': {}", type_name, callable.name, name)};
    }
    switch (MgpValueGetType(default_value)) {
      case MGP_VALUE_TYPE_VERTEX:
      case MGP_VALUE_TYPE_EDGE:
      case MGP_VALUE_TYPE_PATH:
        // default_value must not be a graph element.
        throw ValueConversionException{"Default value of argument '{}' of {} '{}' name must not be a graph element!",
                                       name, type_name, callable.name};
      case MGP_VALUE_TYPE_NULL:
      case MGP_VALUE_TYPE_BOOL:
      case MGP_VALUE_TYPE_INT:
      case MGP_VALUE_TYPE_DOUBLE:
      case MGP_VALUE_TYPE_STRING:
      case MGP_VALUE_TYPE_LIST:
      case MGP_VALUE_TYPE_MAP:
      case MGP_VALUE_TYPE_DATE:
      case MGP_VALUE_TYPE_LOCAL_TIME:
      case MGP_VALUE_TYPE_LOCAL_DATE_TIME:
      case MGP_VALUE_TYPE_DURATION:
        break;
    }
    // Default value must be of required `type`.
    if (!type.impl->SatisfiesType(default_value)) {
      throw std::logic_error{fmt::format("The default value of argument '{}' for {} '{}' doesn't satisfy type '{}'",
                                         name, type_name, callable.name, type.impl->GetPresentableName())};
    }
    auto *memory = callable.opt_args.get_allocator().resource();
    callable.opt_args.emplace_back(memgraph::utils::pmr::string(name, memory), type.impl.get(),
                                   ToTypedValue(default_value, memory));
  });
}
}  // namespace

mgp_error mgp_proc_add_arg(mgp_proc *proc, const char *name, mgp_type *type) {
  return MgpAddArg(*proc, std::string(name), *type);
}

mgp_error mgp_proc_add_opt_arg(mgp_proc *proc, const char *name, mgp_type *type, mgp_value *default_value) {
  return MgpAddOptArg(*proc, std::string(name), *type, *default_value);
}

mgp_error mgp_func_add_arg(mgp_func *func, const char *name, mgp_type *type) {
  return MgpAddArg(*func, std::string(name), *type);
}

mgp_error mgp_func_add_opt_arg(mgp_func *func, const char *name, mgp_type *type, mgp_value *default_value) {
  return MgpAddOptArg(*func, std::string(name), *type, *default_value);
}

namespace {

template <typename T>
concept ModuleProperties = memgraph::utils::SameAsAnyOf<T, mgp_proc, mgp_trans>;

template <ModuleProperties T>
mgp_error AddResultToProp(T *prop, const char *name, mgp_type *type, bool is_deprecated) noexcept {
  return WrapExceptions([=] {
    if (!IsValidIdentifierName(name)) {
      throw std::invalid_argument{fmt::format("Invalid result name for procedure '{}': {}", prop->name, name)};
    }
    if (prop->results.find(name) != prop->results.end()) {
      throw std::logic_error{fmt::format("Result already exists with name '{}' for procedure '{}'", name, prop->name)};
    };
    auto *memory = prop->results.get_allocator().resource();
    prop->results.emplace(memgraph::utils::pmr::string(name, memory), std::make_pair(type->impl.get(), is_deprecated));
  });
}

}  // namespace

mgp_error mgp_proc_add_result(mgp_proc *proc, const char *name, mgp_type *type) {
  return AddResultToProp(proc, name, type, false);
}

mgp_error MgpTransAddFixedResult(mgp_trans *trans) noexcept {
  if (const auto err = AddResultToProp(trans, "query", Call<mgp_type *>(mgp_type_string), false);
      err != mgp_error::MGP_ERROR_NO_ERROR) {
    return err;
  }
  return AddResultToProp(trans, "parameters", Call<mgp_type *>(mgp_type_nullable, Call<mgp_type *>(mgp_type_map)),
                         false);
}

mgp_error mgp_proc_add_deprecated_result(mgp_proc *proc, const char *name, mgp_type *type) {
  return AddResultToProp(proc, name, type, true);
}

int mgp_must_abort(mgp_graph *graph) {
  MG_ASSERT(graph->ctx);
  static_assert(noexcept(graph->ctx->stopping_context.MustAbort()));
  auto const reason = graph->ctx->stopping_context.MustAbort();
  // NOTE: deliberately decoupled to avoid accidental ABI breaks
  switch (reason) {
    case memgraph::query::AbortReason::TERMINATED:
      return 1;
    case memgraph::query::AbortReason::SHUTDOWN:
      return 2;
    case memgraph::query::AbortReason::TIMEOUT:
      return 3;
    case memgraph::query::AbortReason::NO_ABORT:
      return 0;
  }
}

namespace memgraph::query::procedure {

namespace {

// Print the value in user presentable fashion.
// @throw std::bad_alloc
// @throw std::length_error
std::ostream &PrintValue(const TypedValue &value, std::ostream *stream) {
  switch (value.type()) {
    case TypedValue::Type::Null:
      return (*stream) << "Null";
    case TypedValue::Type::Bool:
      return (*stream) << (value.ValueBool() ? "true" : "false");
    case TypedValue::Type::Int:
      return (*stream) << value.ValueInt();
    case TypedValue::Type::Double:
      return (*stream) << value.ValueDouble();
    case TypedValue::Type::String:
      // String value should be escaped, this allocates a new string.
      return (*stream) << memgraph::utils::Escape(value.ValueString());
    case TypedValue::Type::List:
      (*stream) << "[";
      memgraph::utils::PrintIterable(*stream, value.ValueList(), ", ",
                                     [](auto &stream, const auto &elem) { PrintValue(elem, &stream); });
      return (*stream) << "]";
    case TypedValue::Type::Map:
      (*stream) << "{";
      memgraph::utils::PrintIterable(*stream, value.ValueMap(), ", ", [](auto &stream, const auto &item) {
        // Map keys are not escaped strings.
        stream << item.first << ": ";
        PrintValue(item.second, &stream);
      });
      return (*stream) << "}";
    case TypedValue::Type::Date:
      return (*stream) << value.ValueDate();
    case TypedValue::Type::LocalTime:
      return (*stream) << value.ValueLocalTime();
    case TypedValue::Type::LocalDateTime:
      return (*stream) << value.ValueLocalDateTime();
    case TypedValue::Type::ZonedDateTime:
      return (*stream) << value.ValueZonedDateTime();
    case TypedValue::Type::Duration:
      return (*stream) << value.ValueDuration();
    case TypedValue::Type::Enum:
      // TODO: need to convert to EnumType::EnumValue form
      LOG_FATAL("enum not printable - not yet implemented");
    case TypedValue::Type::Point2d:
      return (*stream) << CypherConstructionFor(value.ValuePoint2d());
    case TypedValue::Type::Point3d:
      return (*stream) << CypherConstructionFor(value.ValuePoint3d());
    case TypedValue::Type::Vertex:
    case TypedValue::Type::Edge:
    case TypedValue::Type::Path:
    case TypedValue::Type::Graph:
    case TypedValue::Type::Function:
      LOG_FATAL("value must not be a graph|function element");
  }
}

}  // namespace

void PrintProcSignature(const mgp_proc &proc, std::ostream *stream) {
  (*stream) << proc.name << "(";
  memgraph::utils::PrintIterable(*stream, proc.args, ", ", [](auto &stream, const auto &arg) {
    stream << arg.first << " :: " << arg.second->GetPresentableName();
  });
  if (!proc.args.empty() && !proc.opt_args.empty()) (*stream) << ", ";
  memgraph::utils::PrintIterable(*stream, proc.opt_args, ", ", [](auto &stream, const auto &arg) {
    stream << std::get<0>(arg) << " = ";
    PrintValue(std::get<2>(arg), &stream) << " :: " << std::get<1>(arg)->GetPresentableName();
  });
  (*stream) << ") :: (";
  memgraph::utils::PrintIterable(*stream, proc.results, ", ", [](auto &stream, const auto &name_result) {
    const auto &[type, is_deprecated] = name_result.second;
    if (is_deprecated) stream << "DEPRECATED ";
    stream << name_result.first << " :: " << type->GetPresentableName();
  });
  (*stream) << ")";
}

void PrintFuncSignature(const mgp_func &func, std::ostream &stream) {
  stream << func.name << "(";
  utils::PrintIterable(stream, func.args, ", ", [](auto &stream, const auto &arg) {
    stream << arg.first << " :: " << arg.second->GetPresentableName();
  });
  if (!func.args.empty() && !func.opt_args.empty()) {
    stream << ", ";
  }
  utils::PrintIterable(stream, func.opt_args, ", ", [](auto &stream, const auto &arg) {
    const auto &[name, type, default_val] = arg;
    stream << name << " = ";
    PrintValue(default_val, &stream) << " :: " << type->GetPresentableName();
  });
  stream << ")";
}

bool IsValidIdentifierName(const char *name) {
  if (!name) return false;
  static std::regex regex("[_[:alpha:]][_[:alnum:]]*");
  return std::regex_match(name, regex);
}

}  // namespace memgraph::query::procedure

namespace {
using StreamSourceType = memgraph::query::stream::StreamSourceType;

class InvalidMessageFunction : public std::invalid_argument {
 public:
  InvalidMessageFunction(const StreamSourceType type, const std::string_view function_name)
      : std::invalid_argument{fmt::format("'{}' is not defined for a message from a stream of type '{}'", function_name,
                                          StreamSourceTypeToString(type))} {}
};

StreamSourceType MessageToStreamSourceType(const mgp_message::KafkaMessage & /*msg*/) {
  return StreamSourceType::KAFKA;
}

StreamSourceType MessageToStreamSourceType(const mgp_message::PulsarMessage & /*msg*/) {
  return StreamSourceType::PULSAR;
}

mgp_source_type StreamSourceTypeToMgpSourceType(const StreamSourceType type) {
  switch (type) {
    case StreamSourceType::KAFKA:
      return mgp_source_type::KAFKA;
    case StreamSourceType::PULSAR:
      return mgp_source_type::PULSAR;
  }
}

}  // namespace

mgp_error mgp_message_source_type(mgp_message *message, mgp_source_type *result) {
  return WrapExceptions(
      [message] {
        return std::visit(memgraph::utils::Overloaded{[](const auto &message) {
                            return StreamSourceTypeToMgpSourceType(MessageToStreamSourceType(message));
                          }},
                          message->msg);
      },
      result);
}

mgp_error mgp_message_payload(mgp_message *message, const char **result) {
  return WrapExceptions(
      [message] {
        return std::visit(
            memgraph::utils::Overloaded{[](const mgp_message::KafkaMessage &msg) { return msg->Payload().data(); },
                                        [](const mgp_message::PulsarMessage &msg) { return msg.Payload().data(); },
                                        [](const auto &msg) -> const char * {
                                          throw InvalidMessageFunction(MessageToStreamSourceType(msg), "payload");
                                        }},
            message->msg);
      },
      result);
}

mgp_error mgp_message_payload_size(mgp_message *message, size_t *result) {
  return WrapExceptions(
      [message] {
        return std::visit(
            memgraph::utils::Overloaded{[](const mgp_message::KafkaMessage &msg) { return msg->Payload().size(); },
                                        [](const mgp_message::PulsarMessage &msg) { return msg.Payload().size(); },
                                        [](const auto &msg) -> size_t {
                                          throw InvalidMessageFunction(MessageToStreamSourceType(msg), "payload_size");
                                        }},
            message->msg);
      },
      result);
}

mgp_error mgp_message_topic_name(mgp_message *message, const char **result) {
  return WrapExceptions(
      [message] {
        return std::visit(
            memgraph::utils::Overloaded{[](const mgp_message::KafkaMessage &msg) { return msg->TopicName().data(); },
                                        [](const mgp_message::PulsarMessage &msg) { return msg.TopicName().data(); },
                                        [](const auto &msg) -> const char * {
                                          throw InvalidMessageFunction(MessageToStreamSourceType(msg), "topic_name");
                                        }},
            message->msg);
      },
      result);
}

mgp_error mgp_message_key(mgp_message *message, const char **result) {
  return WrapExceptions(
      [message] {
        return std::visit(
            memgraph::utils::Overloaded{[](const mgp_message::KafkaMessage &msg) { return msg->Key().data(); },
                                        [](const auto &msg) -> const char * {
                                          throw InvalidMessageFunction(MessageToStreamSourceType(msg), "key");
                                        }},
            message->msg);
      },
      result);
}

mgp_error mgp_message_key_size(mgp_message *message, size_t *result) {
  return WrapExceptions(
      [message] {
        return std::visit(
            memgraph::utils::Overloaded{[](const mgp_message::KafkaMessage &msg) { return msg->Key().size(); },
                                        [](const auto &msg) -> size_t {
                                          throw InvalidMessageFunction(MessageToStreamSourceType(msg), "key_size");
                                        }},
            message->msg);
      },
      result);
}

mgp_error mgp_message_timestamp(mgp_message *message, int64_t *result) {
  return WrapExceptions(
      [message] {
        return std::visit(
            memgraph::utils::Overloaded{[](const mgp_message::KafkaMessage &msg) { return msg->Timestamp(); },
                                        [](const auto &msg) -> int64_t {
                                          throw InvalidMessageFunction(MessageToStreamSourceType(msg), "timestamp");
                                        }},
            message->msg);
      },
      result);
}

mgp_error mgp_message_offset(struct mgp_message *message, int64_t *result) {
  return WrapExceptions(
      [message] {
        return std::visit(
            memgraph::utils::Overloaded{[](const mgp_message::KafkaMessage &msg) { return msg->Offset(); },
                                        [](const auto &msg) -> int64_t {
                                          throw InvalidMessageFunction(MessageToStreamSourceType(msg), "offset");
                                        }},
            message->msg);
      },
      result);
}

mgp_error mgp_messages_size(mgp_messages *messages, size_t *result) {
  static_assert(noexcept(messages->messages.size()));
  *result = messages->messages.size();
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_messages_at(mgp_messages *messages, size_t index, mgp_message **result) {
  return WrapExceptions(
      [messages, index] {
        if (index >= Call<size_t>(mgp_messages_size, messages)) {
          throw std::out_of_range("Message cannot be retrieved, because index exceeds messages' size!");
        }
        return &messages->messages[index];
      },
      result);
}

mgp_error mgp_module_add_transformation(mgp_module *module, const char *name, mgp_trans_cb cb) {
  return WrapExceptions([=] {
    if (!IsValidIdentifierName(name)) {
      throw std::invalid_argument{fmt::format("Invalid transformation name: {}", name)};
    }
    if (module->transformations.find(name) != module->transformations.end()) {
      throw std::logic_error{fmt::format("Transformation already exists with name '{}'", name)};
    };
    auto *memory = module->transformations.get_allocator().resource();
    module->transformations.emplace(name, mgp_trans(name, cb, memory));
  });
}

mgp_error mgp_module_add_function(mgp_module *module, const char *name, mgp_func_cb cb, mgp_func **result) {
  return WrapExceptions(
      [=] {
        if (!IsValidIdentifierName(name)) {
          throw std::invalid_argument{fmt::format("Invalid function name: {}", name)};
        }
        if (module->functions.find(name) != module->functions.end()) {
          throw std::logic_error{fmt::format("Function with similar name already exists '{}'", name)};
        };
        auto *memory = module->functions.get_allocator().resource();

        return &module->functions.emplace(name, mgp_func(name, cb, memory)).first->second;
      },
      result);
}

mgp_error mgp_log(const mgp_log_level log_level, const char *output) {
  return WrapExceptions([=] {
    switch (log_level) {
      case mgp_log_level::MGP_LOG_LEVEL_TRACE:
        spdlog::trace(output);
        return;
      case mgp_log_level::MGP_LOG_LEVEL_DEBUG:
        spdlog::debug(output);
        return;
      case mgp_log_level::MGP_LOG_LEVEL_INFO:
        spdlog::info(output);
        return;
      case mgp_log_level::MGP_LOG_LEVEL_WARN:
        spdlog::warn(output);
        return;
      case mgp_log_level::MGP_LOG_LEVEL_ERROR:
        spdlog::error(output);
        return;
      case mgp_log_level::MGP_LOG_LEVEL_CRITICAL:
        spdlog::critical(output);
        return;
    }
    throw std::invalid_argument{fmt::format("Invalid log level: {}", log_level)};
  });
}

mgp_error mgp_track_current_thread_allocations(mgp_graph *graph) {
  return WrapExceptions([&]() {
    std::visit([](auto *db_accessor) -> void { db_accessor->TrackCurrentThreadAllocations(); }, graph->impl);
  });
}

mgp_error mgp_untrack_current_thread_allocations(mgp_graph *graph) {
  return WrapExceptions([&]() {
    std::visit([](auto *db_accessor) -> void { db_accessor->UntrackCurrentThreadAllocations(); }, graph->impl);
  });
}

mgp_execution_headers::mgp_execution_headers(memgraph::utils::pmr::vector<memgraph::utils::pmr::string> &&storage)
    : headers(std::move(storage)){};

mgp_error mgp_execution_headers_size(mgp_execution_headers *headers, size_t *result) {
  static_assert(noexcept(headers->headers.size()));
  *result = headers->headers.size();
  return mgp_error::MGP_ERROR_NO_ERROR;
}

mgp_error mgp_execution_headers_at(mgp_execution_headers *headers, size_t index, const char **result) {
  return WrapExceptions(
      [headers, index] {
        if (index >= Call<size_t>(mgp_execution_headers_size, headers)) {
          throw std::out_of_range("Header cannot be retrieved, because index exceeds headers' size!");
        }
        return headers->headers[index].data();
      },
      result);
}

mgp_execution_rows::mgp_execution_rows(
    memgraph::utils::pmr::vector<memgraph::utils::pmr::vector<memgraph::query::TypedValue>> &&tv_rows)
    : rows(std::move(tv_rows)) {}

struct MgProcedureResultStream final {
  explicit MgProcedureResultStream(mgp_memory *memory) : rows(memory->impl), memory(memory) {}
  using Row = std::vector<memgraph::query::TypedValue>;
  using Rows = std::vector<Row>;
  using PmrRow = memgraph::utils::pmr::vector<memgraph::query::TypedValue>;
  using PmrRows = memgraph::utils::pmr::vector<PmrRow>;

  PmrRows rows;
  mgp_memory *memory;

  void Result(const Row &row) {
    PmrRow pmr_row(memory->impl);
    pmr_row.reserve(row.size());

    for (const auto &val : row) {
      pmr_row.emplace_back(val);
    }

    rows.emplace_back(std::move(pmr_row));
  }
};

memgraph::storage::ExternalPropertyValue::map_t CreateQueryParams(mgp_map *params) {
  auto query_params = memgraph::storage::ExternalPropertyValue::map_t{};
  do_reserve(query_params, params->items.size());
  for (auto &[k, v] : params->items) {
    query_params.emplace(k, ToExternalPropertyValue(v));
  }

  return query_params;
}

struct mgp_execution_result::pImplMgpExecutionResult {
  std::unique_ptr<memgraph::query::Interpreter> interpreter;
  std::unique_ptr<mgp_execution_headers> headers;
};

mgp_execution_result::mgp_execution_result(mgp_graph *graph, allocator_type alloc)
    : pImpl(std::make_unique<pImplMgpExecutionResult>()), alloc(alloc) {
  auto &instance = memgraph::query::InterpreterContextHolder::GetInstance();
  pImpl->interpreter = std::make_unique<memgraph::query::Interpreter>(&instance, instance.dbms_handler->Get(
#ifdef MG_ENTERPRISE
                                                                                     graph->getImpl()->DatabaseName()
#endif
                                                                                         ));
}

void mgp_execution_result_destroy(mgp_execution_result *exec_result) { DeleteRawMgpObject(exec_result); }

mgp_execution_result::~mgp_execution_result() {
  auto &instance = memgraph::query::InterpreterContextHolder::GetInstance();
  instance.interpreters.WithLock([this](auto &interpreters) { interpreters.erase(pImpl->interpreter.get()); });
  // interpreter will delete itself because it's a smart pointer
}

mgp_error mgp_execute_query(mgp_graph *graph, mgp_memory *memory, const char *query, mgp_map *params,
                            mgp_execution_result **result) {
  return WrapExceptions(
      [query, params, graph, memory]() {
        auto query_string = std::string(query);
        auto &instance = memgraph::query::InterpreterContextHolder::GetInstance();

        auto *result = NewRawMgpObject<mgp_execution_result>(memory->impl, graph);
        result->pImpl->interpreter->SetUser(graph->ctx->user_or_role);

        instance.interpreters.WithLock(
            [result](auto &interpreters) { interpreters.insert(result->pImpl->interpreter.get()); });
        auto query_params_func =
            [&](memgraph::storage::Storage const *) -> memgraph::storage::ExternalPropertyValue::map_t {
          return CreateQueryParams(params);
        };
        auto prepare_query_result = result->pImpl->interpreter->Prepare(query_string, query_params_func, {});

        memgraph::utils::pmr::vector<memgraph::utils::pmr::string> headers(memory->impl);
        for (const auto &header : prepare_query_result.headers) {
          headers.emplace_back(header);
        }
        result->pImpl->headers = std::make_unique<mgp_execution_headers>(std::move(headers));

        return result;
      },
      result);
}

mgp_error mgp_fetch_execution_headers(mgp_execution_result *exec_result, mgp_execution_headers **result) {
  return WrapExceptions([exec_result]() { return exec_result->pImpl->headers.get(); }, result);
}

mgp_error mgp_pull_one(mgp_execution_result *exec_result, mgp_graph *graph, mgp_memory *memory, mgp_map **result) {
  return WrapExceptions(
      [exec_result, graph, memory]() -> mgp_map * {
        MgProcedureResultStream stream(memory);

        try {
          exec_result->pImpl->interpreter->Pull(&stream, 1, {});
        } catch (const std::exception &e) {
          return nullptr;
        }

        if (stream.rows.empty()) {
          return nullptr;
        }

        const size_t headers_size = exec_result->pImpl->headers->headers.size();
        memgraph::utils::pmr::map<memgraph::utils::pmr::string, mgp_value> items(memory->impl);
        for (size_t idx = 0; idx < headers_size; idx++) {
          items.emplace(exec_result->pImpl->headers->headers[idx], mgp_value{stream.rows[0][idx], graph, memory->impl});
        }

        return NewRawMgpObject<mgp_map>(memory->impl, std::move(items));
      },
      result);
}
