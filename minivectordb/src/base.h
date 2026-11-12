// MiniVectorDB - common definitions, error codes and small helpers.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace mvdb {

// Error codes.  The numeric values follow the MySQL server error space where a
// matching code exists so that a MySQL flavoured test driver can map them
// without a translation table.
enum class ErrorCode : int {
  kOk = 0,
  kSyntaxError = 1064,
  kUnknownTable = 1146,
  kTableExists = 1050,
  kUnknownColumn = 1054,
  kDuplicateKey = 1062,
  kDataTooLong = 1406,
  kInvalidVector = 1416,
  kDimensionMismatch = 1525,
  kNumberOutOfRange = 1690,
  kUnsupportedYet = 1235,
  kIoError = 1030,
  kStorageCorrupt = 1814,
  kTransactionError = 1568,
  kInternalError = 1105,
};

const char* error_code_name(ErrorCode code);
// The short name of an error code, the judge compares the names of the
// public cases.
const char* error_code_slug(ErrorCode code);

// A lightweight result type: either success or an error code plus a message.
class Status {
 public:
  Status() = default;
  Status(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(); }
  static Status Error(ErrorCode code, std::string message) {
    return Status(code, std::move(message));
  }

  bool ok() const { return code_ == ErrorCode::kOk; }
  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }
  int numeric_code() const { return static_cast<int>(code_); }

 private:
  ErrorCode code_ = ErrorCode::kOk;
  std::string message_;
};

// Logging always goes to stderr: stdout is reserved for the evaluation
// protocol and must never contain diagnostics.
void log_info(const char* fmt, ...);
void log_error(const char* fmt, ...);
void log_set_level(int level);  // 0 = quiet, 1 = info, 2 = debug

std::string trim(std::string_view text);
std::string to_lower(std::string_view text);
std::string to_upper(std::string_view text);
bool iequals(std::string_view a, std::string_view b);
bool starts_with(std::string_view text, std::string_view prefix);
bool ends_with(std::string_view text, std::string_view suffix);
std::vector<std::string> split(std::string_view text, char delimiter);
std::string join(const std::vector<std::string>& parts, std::string_view sep);

// Parses a decimal integer; returns false when the text is not a valid int64.
bool parse_int64(std::string_view text, int64_t* out);
bool parse_double(std::string_view text, double* out);

// Formats a float the way the result writer needs it: a round trip exact
// representation for float32 values and a stable representation for doubles.
std::string format_float(float value);
std::string format_double(double value);

// Number of unicode code points in a utf8 string.
size_t utf8_length(std::string_view text);
// True when the text is well formed utf8.
bool utf8_valid(std::string_view text);

}  // namespace mvdb