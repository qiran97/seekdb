#include "base.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mvdb {

namespace {
int g_log_level = 1;

void log_emit(FILE* stream, const char* level, const char* fmt, va_list args) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_buf;
  localtime_r(&ts.tv_sec, &tm_buf);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
  fprintf(stream, "[%s.%03ld][minivectordb][%s] ", stamp, ts.tv_nsec / 1000000, level);
  vfprintf(stream, fmt, args);
  fputc('\n', stream);
  fflush(stream);
}
}  // namespace

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk: return "OK";
    case ErrorCode::kSyntaxError: return "ER_PARSE_ERROR";
    case ErrorCode::kUnknownTable: return "ER_NO_SUCH_TABLE";
    case ErrorCode::kTableExists: return "ER_TABLE_EXISTS_ERROR";
    case ErrorCode::kUnknownColumn: return "ER_BAD_FIELD_ERROR";
    case ErrorCode::kDuplicateKey: return "ER_DUP_ENTRY";
    case ErrorCode::kDataTooLong: return "ER_DATA_TOO_LONG";
    case ErrorCode::kInvalidVector: return "ER_INVALID_VECTOR_VALUE";
    case ErrorCode::kDimensionMismatch: return "ER_VECTOR_DIMENSION_MISMATCH";
    case ErrorCode::kNumberOutOfRange: return "ER_DATA_OUT_OF_RANGE";
    case ErrorCode::kUnsupportedYet: return "ER_NOT_SUPPORTED_YET";
    case ErrorCode::kIoError: return "ER_IO_ERROR";
    case ErrorCode::kStorageCorrupt: return "ER_STORAGE_CORRUPT";
    case ErrorCode::kTransactionError: return "ER_TRANSACTION_ERROR";
    case ErrorCode::kInternalError: return "ER_INTERNAL_ERROR";
  }
  return "ER_UNKNOWN_ERROR";
}

void log_info(const char* fmt, ...) {
  if (g_log_level < 1) return;
  va_list args;
  va_start(args, fmt);
  log_emit(stderr, "INFO", fmt, args);
  va_end(args);
}

void log_error(const char* fmt, ...) {
  if (g_log_level < 1) return;
  va_list args;
  va_start(args, fmt);
  log_emit(stderr, "ERROR", fmt, args);
  va_end(args);
}

void log_set_level(int level) { g_log_level = level; }

std::string trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string to_upper(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(std::string_view text, char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.append(sep);
    out.append(parts[i]);
  }
  return out;
}

bool parse_int64(std::string_view text, int64_t* out) {
  std::string trimmed = trim(text);
  if (trimmed.empty()) return false;
  size_t index = 0;
  bool negative = false;
  if (trimmed[0] == '+' || trimmed[0] == '-') {
    negative = trimmed[0] == '-';
    index = 1;
  }
  if (index >= trimmed.size()) return false;
  unsigned long long value = 0;
  for (; index < trimmed.size(); ++index) {
    char c = trimmed[index];
    if (c < '0' || c > '9') return false;
    value = value * 10ULL + static_cast<unsigned long long>(c - '0');
    if (value > 9223372036854775807ULL + (negative ? 1ULL : 0ULL)) return false;
  }
  if (negative) {
    *out = static_cast<int64_t>(~value + 1ULL);
  } else {
    *out = static_cast<int64_t>(value);
  }
  return true;
}

bool parse_double(std::string_view text, double* out) {
  std::string trimmed = trim(text);
  if (trimmed.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double value = strtod(trimmed.c_str(), &end);
  if (end == trimmed.c_str()) return false;
  while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (end != nullptr && *end != '\0') return false;
  if (!std::isfinite(value)) return false;
  *out = value;
  return true;
}

std::string format_float(float value) {
  if (value == 0.0f) return "0";
  char buffer[64];
  // 9 significant digits round trip every float32 value exactly.
  snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
  return std::string(buffer);
}

std::string format_double(double value) {
  if (value == 0.0) return "0";
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%.17g", value);
  return std::string(buffer);
}

size_t utf8_length(std::string_view text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size();) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    size_t step = 1;
    if ((c & 0x80) == 0) {
      step = 1;
    } else if ((c & 0xE0) == 0xC0) {
      step = 2;
    } else if ((c & 0xF0) == 0xE0) {
      step = 3;
    } else if ((c & 0xF8) == 0xF0) {
      step = 4;
    }
    i += step;
    ++count;
  }
  return count;
}

bool utf8_valid(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    size_t extra = 0;
    uint32_t code_point = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1;
      code_point = c & 0x1FU;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      code_point = c & 0x0FU;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      code_point = c & 0x07U;
    } else {
      return false;
    }
    if (i + extra >= text.size()) return false;
    for (size_t k = 1; k <= extra; ++k) {
      unsigned char cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      code_point = (code_point << 6) | (cc & 0x3FU);
    }
    if (extra == 1 && code_point < 0x80) return false;
    if (extra == 2 && code_point < 0x800) return false;
    if (extra == 3 && code_point < 0x10000) return false;
    if (code_point > 0x10FFFF) return false;
    if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;
    i += extra + 1;
  }
  return true;
}

}  // namespace mvdb