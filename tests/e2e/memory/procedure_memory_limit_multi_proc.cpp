// Copyright 2023 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include <gflags/gflags.h>
#include <algorithm>
#include <exception>
#include <ios>
#include <iostream>
#include <mgclient.hpp>

#include "utils/logging.hpp"
#include "utils/timer.hpp"

DEFINE_uint64(bolt_port, 7687, "Bolt port");
DEFINE_uint64(timeout, 120, "Timeout seconds");
DEFINE_bool(multi_db, false, "Run test in multi db environment");

int main(int argc, char **argv) {
  google::SetUsageMessage("Memgraph E2E Query Memory Limit In Multi-Thread For Global Allocators");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  memgraph::logging::RedirectToStderr();

  mg::Client::Init();

  auto client =
      mg::Client::Connect({.host = "127.0.0.1", .port = static_cast<uint16_t>(FLAGS_bolt_port), .use_ssl = false});
  if (!client) {
    LOG_FATAL("Failed to connect!");
  }

  if (FLAGS_multi_db) {
    client->Execute("CREATE DATABASE clean;");
    client->DiscardAll();
    client->Execute("USE DATABASE clean;");
    client->DiscardAll();
    client->Execute("MATCH (n) DETACH DELETE n;");
    client->DiscardAll();
  }

  bool test_passed{false};
  try {
    client->Execute(
        "CALL libproc_memory_limit.alloc_256_mib() PROCEDURE MEMORY LIMIT 400MB YIELD allocated WITH allocated AS "
        "allocated_1"
        "CALL libproc_memory_limit.alloc_32_mib() PROCEDURE MEMORY LIMIT 10MB YIELD allocated AS allocated_2 RETURN "
        "allocated_1, allocated_2");
    auto result_rows = client->FetchAll();
    if (result_rows) {
      auto row = *result_rows->begin();
      test_passed = row[0].ValueBool() == true && row[0].ValueBool() == false;
    }

  } catch (const std::exception &e) {
    test_passed = true;
  }

  MG_ASSERT(test_passed, "Error should have happend");

  return 0;
}
