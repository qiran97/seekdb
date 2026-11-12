#include "server.h"

#include "value.h"


#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

namespace mvdb {

namespace {

std::string escape_field(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    switch (c) {
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\\': out += "\\\\"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 2);
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string cell_to_text(const ResultCell& cell) {
  switch (cell.type) {
    case ResultCell::Type::kInteger:
      return std::to_string(cell.number);
    case ResultCell::Type::kReal:
      return format_double(cell.real);
    case ResultCell::Type::kVector:
      return format_vector(cell.vector);
    case ResultCell::Type::kText:
    default:
      return cell.text;
  }
}

}  // namespace

size_t detect_memory_limit() {
  const char* paths[] = {"/sys/fs/cgroup/memory.max",
                         "/sys/fs/cgroup/memory/memory.limit_in_bytes"};
  for (const char* path : paths) {
    FILE* file = fopen(path, "r");
    if (file == nullptr) continue;
    char buffer[64] = {0};
    if (fgets(buffer, sizeof(buffer), file) != nullptr) {
      std::string text = trim(buffer);
      if (text != "max") {
        int64_t value = 0;
        if (parse_int64(text, &value) && value > 0) {
          fclose(file);
          return static_cast<size_t>(value);
        }
      }
    }
    fclose(file);
  }
  return 0;
}

bool StatementReader::next(std::string* statement) {
  while (scan_ < buffer_.size()) {
    char c = buffer_[scan_];
    if (in_line_comment_) {
      if (c == '\n') in_line_comment_ = false;
      ++scan_;
      continue;
    }
    if (in_block_comment_) {
      if (c == '*' && scan_ + 1 < buffer_.size() && buffer_[scan_ + 1] == '/') {
        in_block_comment_ = false;
        scan_ += 2;
        continue;
      }
      ++scan_;
      continue;
    }
    if (in_single_ || in_double_ || in_backtick_) {
      char quote = in_single_ ? '\'' : (in_double_ ? '"' : '`');
      if (c == '\\' && in_backtick_ == false) {
        scan_ += 2;
        continue;
      }
      if (c == quote) {
        if (scan_ + 1 < buffer_.size() && buffer_[scan_ + 1] == quote) {
          scan_ += 2;
          continue;
        }
        in_single_ = false;
        in_double_ = false;
        in_backtick_ = false;
      }
      ++scan_;
      continue;
    }
    if (c == '-' && scan_ + 1 < buffer_.size() && buffer_[scan_ + 1] == '-') {
      in_line_comment_ = true;
      scan_ += 2;
      continue;
    }
    if (c == '#') {
      in_line_comment_ = true;
      ++scan_;
      continue;
    }
    if (c == '/' && scan_ + 1 < buffer_.size() && buffer_[scan_ + 1] == '*') {
      in_block_comment_ = true;
      scan_ += 2;
      continue;
    }
    if (c == '\'') {
      in_single_ = true;
      ++scan_;
      continue;
    }
    if (c == '"') {
      in_double_ = true;
      ++scan_;
      continue;
    }
    if (c == '`') {
      in_backtick_ = true;
      ++scan_;
      continue;
    }
    if (c == ';') {
      std::string text = trim(buffer_.substr(statement_start_, scan_ - statement_start_));
      statement_start_ = scan_ + 1;
      ++scan_;
      if (text.empty()) continue;
      *statement = std::move(text);
      buffer_.erase(0, statement_start_);
      scan_ -= statement_start_;
      statement_start_ = 0;
      return true;
    }
    ++scan_;
  }
  return false;
}

bool StatementReader::pending(std::string* text) const {
  std::string value = buffer_.substr(statement_start_);
  if (trim(value).empty()) return false;
  *text = value;
  return true;
}

void StatementReader::consume_pending(size_t length) {
  statement_start_ += length;
  if (statement_start_ > buffer_.size()) statement_start_ = buffer_.size();
  // The scanner starts at the text that is left.  The flags of the scanner
  // describe the state after the text that was scanned, the statement that is
  // removed is complete and does not leave a literal open.
  scan_ = statement_start_;
  // The text that was consumed is removed from the buffer, the buffer does
  // not grow over the life of the server.
  buffer_.erase(0, statement_start_);
  scan_ = 0;
  statement_start_ = 0;
}

bool StatementReader::flush(std::string* statement) {
  if (statement_start_ >= buffer_.size()) return false;
  std::string text = trim(buffer_.substr(statement_start_));
  statement_start_ = buffer_.size();
  if (text.empty()) return false;
  *statement = std::move(text);
  return true;
}

// Takes the next statement out of the text that was received without a
// semicolon.  A driver that sends one statement per line is supported: the
// first line that is a complete statement is taken, the rest of the text
// stays pending.  A statement that spans several lines is taken when the
// whole text is a complete statement.  The length of the statement is zero
// when the text does not hold a complete statement yet.
size_t take_pending_statement(StatementReader* reader, std::string* statement) {
  std::string pending;
  if (!reader->pending(&pending)) return 0;
  size_t length = 0;
  Statement parsed;
  Status status = parse_first_statement(pending, &parsed, &length);
  if (!status.ok() || parsed.type == StatementType::kEmpty) return 0;
  if (length == 0 || length > pending.size()) length = pending.size();
  *statement = trim(pending.substr(0, length));
  reader->consume_pending(length);
  return length;
}

// Writes a cell of a result set as a JSON value.
void json_write_cell(std::string* out, const ResultCell& cell) {
  switch (cell.type) {
    case ResultCell::Type::kInteger:
      json_write_integer(out, cell.number);
      break;
    case ResultCell::Type::kReal:
      json_write_real(out, cell.real);
      break;
    case ResultCell::Type::kText:
      json_write_string(out, cell.text);
      break;
    case ResultCell::Type::kVector: {
      out->push_back('[');
      for (size_t i = 0; i < cell.vector.size(); ++i) {
        if (i != 0) out->push_back(',');
        json_write_real(out, static_cast<double>(cell.vector[i]));
      }
      out->push_back(']');
      break;
    }
  }
}

void Server::write_line(const std::string& line) {
  fwrite(line.data(), 1, line.size(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

void Server::write_error(const Status& status) {
  std::string line = options_.error_prefix + " " +
                     std::to_string(status.numeric_code()) + " " + status.message();
  write_line(line);
  if (options_.protocol == "text" && !options_.result_terminator.empty()) {
    write_line(options_.result_terminator);
  }
}

void Server::write_json_result(const JsonValue& id, const Statement& statement,
                               const ResultSet& result) {
  std::string line = "{\"id\":";
  json_write_value(&line, id);
  line += ",\"ok\":true";
  if (result.has_result_set) {
    line += ",\"columns\":[";
    for (size_t i = 0; i < result.columns.size(); ++i) {
      if (i != 0) line += ",";
      json_write_string(&line, result.columns[i]);
    }
    line += "],\"rows\":[";
    for (size_t r = 0; r < result.rows.size(); ++r) {
      if (r != 0) line += ",";
      line += "[";
      for (size_t c = 0; c < result.rows[r].size(); ++c) {
        if (c != 0) line += ",";
        json_write_cell(&line, result.rows[r][c]);
      }
      line += "]";
    }
    line += "]";
  } else if (statement.type == StatementType::kInsert ||
             statement.type == StatementType::kUpdate ||
             statement.type == StatementType::kDelete) {
    line += ",\"affected_rows\":" + std::to_string(result.affected_rows);
  }
  line += "}";
  write_line(line);
}

void Server::write_json_error(const JsonValue& id, const Status& status) {
  std::string line = "{\"id\":";
  json_write_value(&line, id);
  line += ",\"ok\":false,\"error\":{\"code\":";
  json_write_string(&line, error_code_slug(status.code()));
  line += ",\"message\":";
  json_write_string(&line, status.message());
  line += "}}";
  write_line(line);
}

void Server::write_result(const ResultSet& result) {
  if (options_.protocol == "text" && result.has_result_set) {
    if (options_.print_header && !result.columns.empty()) {
      std::string header;
      for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i != 0) header += "\t";
        header += escape_field(result.columns[i]);
      }
      write_line(header);
    }
    for (const std::vector<ResultCell>& row : result.rows) {
      std::string line;
      for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) line += "\t";
        line += escape_field(cell_to_text(row[i]));
      }
      write_line(line);
    }
  } else if (options_.protocol == "lines" && result.has_result_set) {
    for (const std::vector<ResultCell>& row : result.rows) {
      std::string line;
      for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) line += "\t";
        line += escape_field(cell_to_text(row[i]));
      }
      write_line(line);
    }
  }
  if (options_.protocol == "text" && !options_.result_terminator.empty()) {
    write_line(options_.result_terminator);
  }
}

void Server::handle_statement(const std::string& statement) {
  if (getenv("MINIVECTORDB_DEBUG_STATEMENT")) {
    fprintf(stderr, "[stmt] %s\n", statement.c_str());
  }
  Statement parsed;
  size_t used = 0;
  Status status = parse_first_statement(statement, &parsed, &used);
  if (!status.ok()) {
    write_error(status);
    return;
  }
  // Only the first statement of the text would be executed, the rest of it
  // must not be dropped without a word.
  if (!only_statement_separators(statement.substr(used))) {
    write_error(Status::Error(ErrorCode::kUnsupportedYet,
                              "only one statement per request is supported"));
    return;
  }
  if (parsed.type == StatementType::kEmpty) return;
  ResultSet result;
  status = engine_.execute(parsed, &result);
  if (!status.ok()) {
    write_error(status);
    return;
  }
  write_result(result);
}

void Server::handle_json_request(const std::string& line) {
  JsonValue request;
  if (!parse_json(line, &request) || !request.is_object()) {
    log_error("the request is not a JSON object: %s", line.c_str());
    write_line("{\"ok\":false,\"error\":{\"code\":\"bad_request\","
               "\"message\":\"the request is not a JSON object\"}}");
    return;
  }
  const JsonValue* id_value = request.find("id");
  const JsonValue* op_value = request.find("op");
  if (id_value == nullptr) {
    write_line("{\"ok\":false,\"error\":{\"code\":\"bad_request\","
               "\"message\":\"the request has no id\"}}");
    return;
  }
  static const JsonValue kNoId;
  const JsonValue& id = id_value == nullptr ? kNoId : *id_value;
  if (op_value == nullptr || !op_value->is_string()) {
    write_json_error(id, Status::Error(ErrorCode::kSyntaxError,
                                       "the request has no op"));
    return;
  }
  const std::string& op = op_value->text;
  if (op == "shutdown") {
    // The transaction that is still open is rolled back, the data is
    // written, the answer is the last line of the output.
    Status status = engine_.rollback_open_transaction();
    if (!status.ok()) {
      log_error("rollback on shutdown failed: %s", status.message().c_str());
    }
    status = engine_.close();
    if (!status.ok()) {
      log_error("shutdown failed: %s", status.message().c_str());
    }
    std::string line_out = "{\"id\":";
    json_write_value(&line_out, id);
    line_out += ",\"ok\":true}";
    write_line(line_out);
    engine_.request_shutdown();
    return;
  }
  if (op != "execute") {
    write_json_error(id, Status::Error(ErrorCode::kUnsupportedYet,
                                       "unknown operation '" + op + "'"));
    return;
  }
  const JsonValue* sql_value = request.find("sql");
  if (sql_value == nullptr || !sql_value->is_string()) {
    write_json_error(id, Status::Error(ErrorCode::kSyntaxError,
                                       "the request has no sql"));
    return;
  }
  // The mode of the vector search belongs to the request.
  const JsonValue* mode_value = request.find("search_mode");
  if (mode_value != nullptr && mode_value->is_string()) {
    engine_.set_search_mode_ann(mode_value->text == "ann");
  }
  Statement parsed;
  size_t used = 0;
  Status status = parse_first_statement(sql_value->text, &parsed, &used);
  if (!status.ok()) {
    write_json_error(id, status);
    return;
  }
  // Only the first statement of the text would be executed, the rest of it
  // must not be dropped without a word.
  if (!only_statement_separators(sql_value->text.substr(used))) {
    write_json_error(id, Status::Error(ErrorCode::kUnsupportedYet,
                                       "only one statement per request is supported"));
    return;
  }
  if (parsed.type == StatementType::kEmpty) {
    std::string line_out = "{\"id\":";
    json_write_value(&line_out, id);
    line_out += ",\"ok\":true}";
    write_line(line_out);
    return;
  }
  ResultSet result;
  status = engine_.execute(parsed, &result);
  if (!status.ok()) {
    write_json_error(id, status);
    return;
  }
  write_json_result(id, parsed, result);
}

int Server::run_jsonl() {
  std::string pending;
  char chunk[8192];
  while (true) {
    size_t newline = pending.find('\n');
    while (newline == std::string::npos) {
      ssize_t received = read(0, chunk, sizeof(chunk));
      if (received < 0) {
        if (errno == EINTR) continue;
        return 1;
      }
      if (received == 0) break;
      pending.append(chunk, static_cast<size_t>(received));
      newline = pending.find('\n');
    }
    if (newline == std::string::npos) {
      // The input ended.  A last line without a newline is a request too.
      if (!pending.empty()) {
        std::string line = pending;
        pending.clear();
        if (!trim(line).empty()) handle_json_request(line);
      }
      break;
    }
    std::string line = pending.substr(0, newline);
    pending.erase(0, newline + 1);
    if (!trim(line).empty()) {
      // A statement that fails with an exception must not take the program
      // down, the driver waits for an answer.
      try {
        handle_json_request(line);
      } catch (const std::exception& error) {
        log_error("the request failed: %s", error.what());
        write_line("{\"ok\":false,\"error\":{\"code\":\"internal_error\","
                   "\"message\":\"the statement could not be executed\"}}");
      }
    }
    if (engine_.shutdown_requested()) break;
  }
  if (!engine_.shutdown_requested()) {
    Status status = engine_.rollback_open_transaction();
    if (!status.ok()) {
      log_error("rollback on shutdown failed: %s", status.message().c_str());
    }
    status = engine_.close();
    if (!status.ok()) {
      log_error("shutdown failed: %s", status.message().c_str());
    }
  }
  return 0;
}

int Server::run() {
  log_set_level(options_.log_level);
  const size_t memory_limit = detect_memory_limit();
  size_t buffer_pool = options_.buffer_pool_bytes;
  size_t cache_budget = options_.cache_budget_bytes;
  if (buffer_pool == 0) {
    buffer_pool = 16u << 20;
    if (memory_limit > 0 && memory_limit / 8 < buffer_pool) {
      buffer_pool = memory_limit / 8;
    }
    if (buffer_pool < (1u << 20)) buffer_pool = 1u << 20;
  }
  if (cache_budget == 0) {
    cache_budget = 64u << 20;
    if (memory_limit > 0 && memory_limit / 4 < cache_budget) {
      cache_budget = memory_limit / 4;
    }
    if (cache_budget < (4u << 20)) cache_budget = 4u << 20;
  }

  Status status = engine_.open(options_.data_dir, buffer_pool, cache_budget);
  if (!status.ok()) {
    fprintf(stderr, "minivectordb: cannot open %s: %s\n", options_.data_dir.c_str(),
            status.message().c_str());
    return 1;
  }
  log_info("minivectordb ready, data dir %s, buffer pool %zu bytes, cache budget %zu bytes",
           options_.data_dir.c_str(), buffer_pool, cache_budget);
  if (options_.protocol == "jsonl") {
    // The handshake of the judge: one JSON object, nothing else.
    write_line("{\"type\":\"ready\"}");
    return run_jsonl();
  }
  if (options_.print_ready && !options_.ready_message.empty()) {
    write_line(options_.ready_message);
  }

  StatementReader reader;
  std::string pending_input;
  // The driver of the task sends statements that end with a semicolon.  A
  // driver that sends one statement per line without a semicolon is also
  // supported: when the input stays idle for a moment and the pending text
  // is a complete statement, it is executed.
  const int idle_timeout_ms = options_.idle_timeout_ms;
  while (true) {
    struct pollfd descriptor;
    descriptor.fd = 0;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    int ready = poll(&descriptor, 1, idle_timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) {
      std::string statement;
      if (take_pending_statement(&reader, &statement) != 0) {
        handle_statement(statement);
        if (engine_.shutdown_requested()) break;
      }
      continue;
    }
    char chunk[8192];
    ssize_t received = read(0, chunk, sizeof(chunk));
    if (received < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (received == 0) break;  // end of input
    pending_input.append(chunk, static_cast<size_t>(received));
    reader.append(pending_input);
    pending_input.clear();
    std::string statement;
    while (reader.next(&statement)) {
      handle_statement(statement);
      if (engine_.shutdown_requested()) break;
    }
  }
  // The input ended.  The statements that were received without a semicolon
  // are executed as well.
  std::string statement;
  while (take_pending_statement(&reader, &statement) != 0) {
    handle_statement(statement);
    if (engine_.shutdown_requested()) break;
  }
  while (reader.flush(&statement)) {
    handle_statement(statement);
    if (engine_.shutdown_requested()) break;
  }

  status = engine_.rollback_open_transaction();
  if (!status.ok()) {
    log_error("rollback on shutdown failed: %s", status.message().c_str());
  }
  status = engine_.close();
  if (!status.ok()) {
    log_error("shutdown failed: %s", status.message().c_str());
  }
  return 0;
}

}  // namespace mvdb