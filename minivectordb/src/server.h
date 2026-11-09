// The stdin/stdout protocol server of MiniVectorDB.
#pragma once

#include <string>
#include <vector>

#include "engine.h"

namespace mvdb {

struct ServerOptions {
  std::string data_dir = "/data";
  std::string protocol = "text";        // text | lines | jsonl
  std::string ready_message = "MiniVectorDB ready";
  std::string result_terminator = "-- MVDB-END --";
  std::string error_prefix = "ERROR";
  bool print_ready = true;
  bool print_header = true;
  size_t buffer_pool_bytes = 0;   // 0 selects a value based on the cgroup limit
  size_t cache_budget_bytes = 0;  // 0 selects a value based on the cgroup limit
  bool search_mode_ann = false;
  int log_level = 1;
};

// Splits the incoming byte stream into complete statements.
class StatementReader {
 public:
  void append(const std::string& text) { buffer_ += text; }
  bool next(std::string* statement);
  bool flush(std::string* statement);

 private:
  std::string buffer_;
  size_t scan_ = 0;
  size_t statement_start_ = 0;
  bool in_single_ = false;
  bool in_double_ = false;
  bool in_backtick_ = false;
  bool in_line_comment_ = false;
  bool in_block_comment_ = false;
};

class Server {
 public:
  explicit Server(const ServerOptions& options) : options_(options) {}
  int run();

 private:
  void handle_statement(const std::string& statement);
  void write_result(const ResultSet& result);
  void write_error(const Status& status);
  void write_line(const std::string& line);

  ServerOptions options_;
  Engine engine_;
};

// Detects the cgroup v2 memory limit of the container, 0 when unknown.
size_t detect_memory_limit();

}  // namespace mvdb