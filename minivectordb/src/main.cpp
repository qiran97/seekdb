// Entry point of MiniVectorDB.
//
// The program reads SQL statements from stdin, writes the results to stdout
// and the diagnostics to stderr.  No network listener is started.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "server.h"

namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void handle_signal(int) { g_shutdown_requested = 1; }

void print_usage() {
  fprintf(stderr,
          "minivectordb - a small vector database\n"
          "\n"
          "Usage: minivectordb [options]\n"
          "\n"
          "Options:\n"
          "  --data-dir <path>       directory that holds the database files (default /data)\n"
          "  --protocol <name>       text (default), lines or jsonl\n"
          "  --ready-message <text>  message printed once the database is ready\n"
          "  --no-ready-message      do not print a ready message\n"
          "  --result-terminator <t> end marker of a result block (default \"-- MVDB-END --\")\n"
          "  --no-result-terminator  do not print a result terminator\n"
          "  --no-header             do not print the column names of a result set\n"
          "  --mode <exact|ann>      default vector search mode (default exact)\n"
          "  --buffer-pool <bytes>   size of the page cache\n"
          "  --cache-budget <bytes>  maximum size of the in-memory row cache\n"
          "  --log-level <0..2>      0 quiet, 1 info (default), 2 debug\n"
          "  --help                  print this help\n");
}

}  // namespace

int main(int argc, char** argv) {
  mvdb::ServerOptions options;
  const char* env_mode = getenv("MINIVECTORDB_SEARCH_MODE");
  if (env_mode != nullptr && *env_mode != '\0') {
    options.search_mode_ann = (strcasecmp(env_mode, "ann") == 0 ||
                               strcasecmp(env_mode, "approximate") == 0);
  }
  const char* env_data_dir = getenv("MINIVECTORDB_DATA_DIR");
  if (env_data_dir != nullptr && *env_data_dir != '\0') {
    options.data_dir = env_data_dir;
  }

  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    // Both "--name value" and "--name=value" are accepted.
    std::string inline_value;
    bool has_inline_value = false;
    size_t equals = argument.find('=');
    if (equals != std::string::npos && argument.compare(0, 2, "--") == 0) {
      inline_value = argument.substr(equals + 1);
      argument = argument.substr(0, equals);
      has_inline_value = true;
    }
    auto next_value = [&](const char* name) -> std::string {
      if (has_inline_value) return inline_value;
      if (i + 1 >= argc) {
        fprintf(stderr, "minivectordb: %s requires a value\n", name);
        exit(2);
      }
      return argv[++i];
    };
    if (argument == "--data-dir" || argument == "--datadir") {
      options.data_dir = next_value("--data-dir");
    } else if (argument == "--protocol") {
      options.protocol = mvdb::to_lower(next_value("--protocol"));
    } else if (argument == "--ready-message") {
      options.ready_message = next_value("--ready-message");
    } else if (argument == "--no-ready-message") {
      options.print_ready = false;
    } else if (argument == "--result-terminator") {
      options.result_terminator = next_value("--result-terminator");
    } else if (argument == "--no-result-terminator") {
      options.result_terminator.clear();
    } else if (argument == "--no-header") {
      options.print_header = false;
    } else if (argument == "--mode") {
      std::string mode = mvdb::to_lower(next_value("--mode"));
      options.search_mode_ann = (mode == "ann" || mode == "approximate");
    } else if (argument == "--buffer-pool") {
      int64_t value = 0;
      if (!mvdb::parse_int64(next_value("--buffer-pool"), &value) || value <= 0) {
        fprintf(stderr, "minivectordb: invalid --buffer-pool value\n");
        return 2;
      }
      options.buffer_pool_bytes = static_cast<size_t>(value);
    } else if (argument == "--cache-budget") {
      int64_t value = 0;
      if (!mvdb::parse_int64(next_value("--cache-budget"), &value) || value <= 0) {
        fprintf(stderr, "minivectordb: invalid --cache-budget value\n");
        return 2;
      }
      options.cache_budget_bytes = static_cast<size_t>(value);
    } else if (argument == "--log-level") {
      int64_t value = 0;
      if (!mvdb::parse_int64(next_value("--log-level"), &value)) {
        fprintf(stderr, "minivectordb: invalid --log-level value\n");
        return 2;
      }
      options.log_level = static_cast<int>(value);
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else {
      fprintf(stderr, "minivectordb: unknown option '%s'\n", argument.c_str());
      print_usage();
      return 2;
    }
  }

  // sigaction without SA_RESTART: a signal interrupts the blocking read on
  // stdin so that the program can shut down immediately.
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  signal(SIGPIPE, SIG_IGN);

  mvdb::Server server(options);
  return server.run();
}