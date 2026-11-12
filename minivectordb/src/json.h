// A small JSON reader and writer.  The judge sends one JSON object per line
// and expects one JSON object per line as the answer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mvdb {

class JsonValue {
 public:
  enum class Type { kNull, kBoolean, kInteger, kReal, kString, kArray, kObject };

  Type type = Type::kNull;
  bool boolean = false;
  int64_t integer = 0;
  double real = 0.0;
  std::string text;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  bool is_object() const { return type == Type::kObject; }
  bool is_string() const { return type == Type::kString; }
  // The member of an object, null when it is not there.
  const JsonValue* find(const std::string& key) const;
};

// Reads one JSON value.  The text may hold leading and trailing spaces.
// Returns false when the text is not a single valid JSON value.  NaN and
// infinity are not accepted.
bool parse_json(std::string_view text, JsonValue* out);

// Writes a JSON string with the quotes and the escapes.
void json_write_string(std::string* out, const std::string& value);
// Writes a finite JSON number.
void json_write_real(std::string* out, double value);
// Writes a JSON number that keeps the precision of a 64 bit integer.
void json_write_integer(std::string* out, int64_t value);
// Writes a value that was read before, the type is kept.
void json_write_value(std::string* out, const JsonValue& value);

}  // namespace mvdb