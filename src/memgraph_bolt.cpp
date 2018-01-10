#include <csignal>
#include <experimental/filesystem>
#include <iostream>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "communication/bolt/v1/session.hpp"
#include "communication/messaging/distributed.hpp"
#include "communication/server.hpp"
#include "config.hpp"
#include "distributed/coordination_master.hpp"
#include "distributed/coordination_worker.hpp"
#include "io/network/network_endpoint.hpp"
#include "io/network/network_error.hpp"
#include "io/network/socket.hpp"
#include "utils/flag_validation.hpp"
#include "utils/scheduler.hpp"
#include "utils/signals/handler.hpp"
#include "utils/stacktrace.hpp"
#include "utils/sysinfo/memory.hpp"
#include "utils/terminate_handler.hpp"
#include "version.hpp"

namespace fs = std::experimental::filesystem;
using communication::bolt::SessionData;
using io::network::NetworkEndpoint;
using io::network::Socket;
using SessionT = communication::bolt::Session<Socket>;
using ResultStreamT = SessionT::ResultStreamT;
using ServerT = communication::Server<SessionT, SessionData>;

// General purpose flags.
DEFINE_string(interface, "0.0.0.0",
              "Communication interface on which to listen.");
DEFINE_string(port, "7687", "Communication port on which to listen.");
DEFINE_VALIDATED_int32(num_workers,
                       std::max(std::thread::hardware_concurrency(), 1U),
                       "Number of workers", FLAG_IN_RANGE(1, INT32_MAX));
DEFINE_string(log_file, "", "Path to where the log should be stored.");
DEFINE_HIDDEN_string(
    log_link_basename, "",
    "Basename used for symlink creation to the last log file.");
DEFINE_uint64(memory_warning_threshold, 1024,
              "Memory warning treshold, in MB. If Memgraph detects there is "
              "less available RAM available it will log a warning. Set to 0 to "
              "disable.");

// Distributed flags.
DEFINE_HIDDEN_bool(
    master, false,
    "If this Memgraph server is the master in a distributed deployment.");
DEFINE_HIDDEN_string(master_host, "0.0.0.0",
                     "For master node indicates the host served on. For worker "
                     "node indicates the master location.");
DEFINE_VALIDATED_HIDDEN_int32(
    master_port, 0,
    "For master node the port on which to serve. For "
    "worker node indicates the master's port.",
    FLAG_IN_RANGE(0, std::numeric_limits<uint16_t>::max()));
DEFINE_HIDDEN_bool(
    worker, false,
    "If this Memgraph server is a worker in a distributed deployment.");
DEFINE_HIDDEN_string(worker_host, "0.0.0.0",
                     "For worker node indicates the host served on. For master "
                     "node this flag is not used.");
DEFINE_VALIDATED_HIDDEN_int32(
    worker_port, 0,
    "For master node it's unused. For worker node "
    "indicates the port on which to serve. If zero (default value), a port is "
    "chosen at random. Sent to the master when registring worker node.",
    FLAG_IN_RANGE(0, std::numeric_limits<uint16_t>::max()));

// Needed to correctly handle memgraph destruction from a signal handler.
// Without having some sort of a flag, it is possible that a signal is handled
// when we are exiting main, inside destructors of GraphDb and similar. The
// signal handler may then initiate another shutdown on memgraph which is in
// half destructed state, causing invalid memory access and crash.
volatile sig_atomic_t is_shutting_down = 0;

// Registers the given shutdown function with the appropriate signal handlers.
// See implementation for details.
void InitSignalHandlers(const std::function<void()> &shutdown) {
  // Prevent handling shutdown inside a shutdown. For example, SIGINT handler
  // being interrupted by SIGTERM before is_shutting_down is set, thus causing
  // double shutdown.
  sigset_t block_shutdown_signals;
  sigemptyset(&block_shutdown_signals);
  sigaddset(&block_shutdown_signals, SIGTERM);
  sigaddset(&block_shutdown_signals, SIGINT);

  CHECK(SignalHandler::RegisterHandler(Signal::Terminate, shutdown,
                                       block_shutdown_signals))
      << "Unable to register SIGTERM handler!";
  CHECK(SignalHandler::RegisterHandler(Signal::Interupt, shutdown,
                                       block_shutdown_signals))
      << "Unable to register SIGINT handler!";

  // Setup SIGUSR1 to be used for reopening log files, when e.g. logrotate
  // rotates our logs.
  CHECK(SignalHandler::RegisterHandler(Signal::User1, []() {
    google::CloseLogDestination(google::INFO);
  })) << "Unable to register SIGUSR1 handler!";
}

void StartMemWarningLogger() {
  Scheduler mem_log_scheduler;
  if (FLAGS_memory_warning_threshold > 0) {
    mem_log_scheduler.Run(std::chrono::seconds(3), [] {
      auto free_ram_mb = utils::AvailableMem() / 1024;
      if (free_ram_mb < FLAGS_memory_warning_threshold)
        LOG(WARNING) << "Running out of available RAM, only " << free_ram_mb
                     << " MB left.";
    });
  }
}

void MasterMain() {
  google::SetUsageMessage("Memgraph distributed master");
  // RPC for worker registration, shutdown and endpoint info exchange.
  communication::messaging::System system(FLAGS_master_host, FLAGS_master_port);
  distributed::MasterCoordination master(system);

  // Bolt server stuff.
  SessionData session_data{system, master};
  NetworkEndpoint endpoint(FLAGS_interface, FLAGS_port);
  ServerT server(endpoint, session_data, FLAGS_num_workers);

  // Handler for regular termination signals
  auto shutdown = [&server, &session_data] {
    if (is_shutting_down) return;
    is_shutting_down = 1;
    // Server needs to be shutdown first and then the database. This prevents a
    // race condition when a transaction is accepted during server shutdown.
    server.Shutdown();
    session_data.db.Shutdown();
  };

  InitSignalHandlers(shutdown);

  StartMemWarningLogger();

  server.AwaitShutdown();
}

void WorkerMain() {
  google::SetUsageMessage("Memgraph distributed worker");
  // RPC for worker registration, shutdown and endpoint info exchange.
  communication::messaging::System system(FLAGS_worker_host, FLAGS_worker_port);
  io::network::NetworkEndpoint master_endpoint{
      FLAGS_master_host, static_cast<uint16_t>(FLAGS_master_port)};
  distributed::WorkerCoordination worker(system, master_endpoint);
  auto worker_id = worker.RegisterWorker();

  // The GraphDb destructor shuts some RPC down. Ensure correct ordering.
  {
    GraphDb db{system, worker_id, worker, master_endpoint};
    query::Interpreter interpreter;
    StartMemWarningLogger();
    // Wait for the shutdown command from the master.
    worker.WaitForShutdown();
  }
}

void SingleNodeMain() {
  google::SetUsageMessage("Memgraph single-node database server");
  SessionData session_data;
  NetworkEndpoint endpoint(FLAGS_interface, FLAGS_port);
  ServerT server(endpoint, session_data, FLAGS_num_workers);

  // Handler for regular termination signals
  auto shutdown = [&server, &session_data] {
    if (is_shutting_down) return;
    is_shutting_down = 1;
    // Server needs to be shutdown first and then the database. This prevents a
    // race condition when a transaction is accepted during server shutdown.
    server.Shutdown();
    session_data.db.Shutdown();
  };
  InitSignalHandlers(shutdown);

  StartMemWarningLogger();

  server.AwaitShutdown();
}

int main(int argc, char **argv) {
  gflags::SetVersionString(version_string);

  // Load config before parsing arguments, so that flags from the command line
  // overwrite the config.
  LoadConfig();
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  google::InitGoogleLogging(argv[0]);
  google::SetLogDestination(google::INFO, FLAGS_log_file.c_str());
  google::SetLogSymlink(google::INFO, FLAGS_log_link_basename.c_str());

  // Unhandled exception handler init.
  std::set_terminate(&terminate_handler);

  CHECK(!(FLAGS_master && FLAGS_worker))
      << "Can't run Memgraph as worker and master at the same time";
  if (FLAGS_master)
    MasterMain();
  else if (FLAGS_worker)
    WorkerMain();
  else
    SingleNodeMain();
  return 0;
}
