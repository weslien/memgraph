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

#include "utils/memory_tracker.hpp"

#include <atomic>
#include <exception>

#include "utils/atomic_max.hpp"
#include "utils/logging.hpp"
#include "utils/readable_size.hpp"

namespace memgraph::utils {

namespace {

// Prevent memory tracker for throwing during the stack unwinding
bool MemoryTrackerCanThrow() {
  return !std::uncaught_exceptions() && MemoryTracker::OutOfMemoryExceptionEnabler::CanThrow() &&
         !MemoryTracker::OutOfMemoryExceptionBlocker::IsBlocked();
}

}  // namespace

thread_local uint64_t MemoryTracker::OutOfMemoryExceptionEnabler::counter_ = 0;
thread_local uint64_t MemoryTracker::OutOfMemoryExceptionBlocker::counter_ = 0;

MemoryTracker total_memory_tracker;

// TODO (antonio2368): Define how should the peak memory be logged.
// Logging every time the peak changes is too much so some kind of distribution
// should be used.
void MemoryTracker::LogPeakMemoryUsage() const { spdlog::info("Peak memory usage: {}", GetReadableSize(peak_)); }

// TODO (antonio2368): Define how should the memory be logged.
// Logging on each allocation is too much so some kind of distribution
// should be used.
void MemoryTracker::LogMemoryUsage(const int64_t current) {
  spdlog::info("Current memory usage: {}", GetReadableSize(current));
}

void MemoryTracker::UpdatePeak(const int64_t will_be) {
  auto peak_old = peak_.load(std::memory_order_relaxed);
  if (will_be > peak_old) {
    peak_.store(will_be, std::memory_order_relaxed);
  }
}

void MemoryTracker::SetHardLimit(const int64_t limit) {
  const int64_t next_limit = std::invoke([this, limit] {
    if (maximum_hard_limit_ == 0) {
      return limit;
    }
    return limit == 0 ? maximum_hard_limit_ : std::min(maximum_hard_limit_, limit);
  });

  if (next_limit <= 0) {
    spdlog::warn("Invalid memory limit.");
    return;
  }

  const auto previous_limit = hard_limit_.exchange(next_limit, std::memory_order_relaxed);
  if (previous_limit != next_limit) {
    // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
    spdlog::info("Memory limit set to {}", utils::GetReadableSize(next_limit));
  }
}

void MemoryTracker::TryRaiseHardLimit(const int64_t limit) {
  atomic_fetch_max_explicit(&hard_limit_, limit, std::memory_order_acq_rel);
}

void MemoryTracker::ResetTrackings() {
  hard_limit_.store(0, std::memory_order_relaxed);
  peak_.store(0, std::memory_order_relaxed);
  amount_.store(0, std::memory_order_relaxed);
  maximum_hard_limit_ = 0;
}

void MemoryTracker::SetMaximumHardLimit(const int64_t limit) {
  if (maximum_hard_limit_ < 0) {
    spdlog::warn("Invalid maximum hard limit.");
    return;
  }
  maximum_hard_limit_ = limit;
}

bool MemoryTracker::Alloc(int64_t const size) {
  MG_ASSERT(size >= 0, "Negative size passed to the MemoryTracker.");

  const int64_t will_be = size + amount_.fetch_add(size, std::memory_order_relaxed);

  const auto current_hard_limit = hard_limit_.load(std::memory_order_relaxed);

  if (current_hard_limit && will_be > current_hard_limit && MemoryTrackerCanThrow()) [[unlikely]] {
    MemoryTracker::OutOfMemoryExceptionBlocker exception_blocker;

    amount_.fetch_sub(size, std::memory_order_relaxed);

    // register our error data, we will pick this up on the other side of jemalloc
    MemoryErrorStatus().set({size, will_be, current_hard_limit});

    return false;
  }
  UpdatePeak(will_be);
  return true;
}

void MemoryTracker::DoCheck() {
  const auto current_hard_limit = hard_limit_.load(std::memory_order_relaxed);
  const auto current_amount = amount_.load(std::memory_order_relaxed);
  if (current_hard_limit && current_amount > current_hard_limit && MemoryTrackerCanThrow()) [[unlikely]] {
    MemoryTracker::OutOfMemoryExceptionBlocker exception_blocker;
    throw OutOfMemoryException(
        fmt::format("Memory limit exceeded! Current "
                    "use is {}, while the maximum allowed size for allocation is set to {}.",
                    GetReadableSize(static_cast<double>(current_amount)),
                    GetReadableSize(static_cast<double>(current_hard_limit))));
  }
}

void MemoryTracker::Free(const int64_t size) { amount_.fetch_sub(size, std::memory_order_relaxed); }

// DEVNOTE: important that this is allocated at thread construction time
//          otherwise subtle bug where jemalloc will try to lock an non-recursive mutex
//          that it already owns
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local MemoryTrackerStatus status;
auto MemoryErrorStatus() -> MemoryTrackerStatus & { return status; }

auto MemoryTrackerStatus::msg() -> std::optional<std::string> {
  if (!data_) return std::nullopt;

  auto [size, will_be, hard_limit] = *data_;
  data_.reset();
  return fmt::format(
      "Memory limit exceeded! Attempting to allocate a chunk of {} which would put the current "
      "use to {}, while the maximum allowed size for allocation is set to {}.",
      // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
      GetReadableSize(size), GetReadableSize(will_be), GetReadableSize(hard_limit));
}

}  // namespace memgraph::utils
