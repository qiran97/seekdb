// The stdin/stdout protocol server of MiniVectorDB.
#pragma once

#include <string>
#include <vector>

#include "engine.h"
#include "json.h"

namespace mvdb {

struct ServerOptions {
  std::string data_dir = "/data";
  // jsonl is the protocol of the judge: one JSON object per line.  text and
  // lines are the plain protocols of the local test scripts.
  std::string protocol = "jsonl";       // jsonl | text | lines
  std::string ready_message = "MiniVectorDB ready";
  std::string result_terminator = "-- MVDB-END --";
  std::string error_prefix = "ERROR";
  bool print_ready = true;
  bool print_header = true;
  size_t buffer_pool_bytes = 0;   // 0 selects a value based on the cgroup limit
  size_t cache_budget_bytes = 0;  // 0 selects a value based on the cgroup limit
  bool search_mode_ann = false;
  int log_level = 1;
  // How long the input has to be idle before a statement that does not end
  // with a semicolon is executed.  A driver that sends the statements with a
  // semicolon never waits for it.
  int idle_timeout_ms = 50;
};

// Splits the incoming byte stream into complete statements.
class StatementReader {
 public:
  void append(const std::string& text) { buffer_ += text; }
  bool next(std::string* statement);
  bool flush(std::string* statement);
  // The text that was received but does not contain a statement terminator.
  // The text is not trimmed, the offsets of the caller refer to the buffer.
  bool pending(std::string* text) const;
  // Removes the first `length` characters of the pending text.
  void consume_pending(size_t length);

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
  // The JSON lines protocol: one request object per line, one answer
  // object per line.
  int run_jsonl();
  void handle_json_request(const std::string& line);
  void write_json_result(const JsonValue& id, const Statement& statement,
                         const ResultSet& result);
  void write_json_error(const JsonValue& id, const Status& status);
  void write_result(const ResultSet& result);
  void write_error(const Status& status);
  void write_line(const std::string& line);

  ServerOptions options_;
  Engine engine_;
};

// Detects the cgroup v2 memory limit of the container, 0 when unknown.
size_t detect_memory_limit();

}  // namespace mvdb