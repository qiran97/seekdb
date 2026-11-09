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
  std::string value = trim(buffer_.substr(statement_start_));
  if (value.empty()) return false;
  *text = value;
  return true;
}

void StatementReader::consume_pending() {
  statement_start_ = buffer_.size();
  scan_ = buffer_.size();
}

bool StatementReader::flush(std::string* statement) {
  if (statement_start_ >= buffer_.size()) return false;
  std::string text = trim(buffer_.substr(statement_start_));
  statement_start_ = buffer_.size();
  if (text.empty()) return false;
  *statement = std::move(text);
  return true;
}

void Server::write_line(const std::string& line) {
  fwrite(line.data(), 1, line.size(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

void Server::write_error(const Status& status) {
  if (options_.protocol == "jsonl") {
    std::string line = "{\"error\":{\"code\":" + std::to_string(status.numeric_code()) +
                       ",\"message\":\"" + json_escape(status.message()) + "\"}}";
    write_line(line);
    return;
  }
  std::string line = options_.error_prefix + " " +
                     std::to_string(status.numeric_code()) + " " + status.message();
  write_line(line);
  if (options_.protocol == "text" && !options_.result_terminator.empty()) {
    write_line(options_.result_terminator);
  }
}

void Server::write_result(const ResultSet& result) {
  if (options_.protocol == "jsonl") {
    std::string line = "{\"columns\":[";
    for (size_t i = 0; i < result.columns.size(); ++i) {
      if (i != 0) line += ",";
      line += "\"" + json_escape(result.columns[i]) + "\"";
    }
    line += "],\"rows\":[";
    for (size_t r = 0; r < result.rows.size(); ++r) {
      if (r != 0) line += ",";
      line += "[";
      for (size_t c = 0; c < result.rows[r].size(); ++c) {
        if (c != 0) line += ",";
        line += "\"" + json_escape(cell_to_text(result.rows[r][c])) + "\"";
      }
      line += "]";
    }
    line += "],\"affected_rows\":" + std::to_string(result.affected_rows) + "}";
    write_line(line);
    return;
  }
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
  Status status = parse_statement(statement, &parsed);
  if (!status.ok()) {
    write_error(status);
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
  if (options_.print_ready && !options_.ready_message.empty()) {
    write_line(options_.ready_message);
  }

  StatementReader reader;
  std::string pending_input;
  // The driver of the task sends statements that end with a semicolon.  A
  // driver that sends one statement per line without a semicolon is also
  // supported: when the input stays idle for a moment and the pending text
  // is a complete statement, it is executed.
  const int idle_timeout_ms = 250;
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
      std::string pending;
      if (reader.pending(&pending)) {
        Statement parsed;
        Status status = parse_statement(pending, &parsed);
        if (status.ok() && parsed.type != StatementType::kEmpty) {
          reader.consume_pending();
          handle_statement(pending);
          if (engine_.shutdown_requested()) break;
        }
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
  std::string statement;
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