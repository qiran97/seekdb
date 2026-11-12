#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mvdb {

const JsonValue* JsonValue::find(const std::string& key) const {
  if (type != Type::kObject) return nullptr;
  for (const auto& member : members) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

namespace {

class Reader {
 public:
  explicit Reader(std::string_view text) : text_(text) {}

  bool read(JsonValue* out) {
    skip_space();
    if (!read_value(out, 0)) return false;
    skip_space();
    return pos_ == text_.size();
  }

 private:
  static constexpr int kMaxDepth = 32;

  void skip_space() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
        continue;
      }
      break;
    }
  }

  bool literal(const char* text, size_t length) {
    if (text_.size() - pos_ < length) return false;
    if (text_.compare(pos_, length, text) != 0) return false;
    pos_ += length;
    return true;
  }

  bool read_value(JsonValue* out, int depth) {
    if (depth > kMaxDepth) return false;
    if (pos_ >= text_.size()) return false;
    char c = text_[pos_];
    switch (c) {
      case '{': return read_object(out, depth);
      case '[': return read_array(out, depth);
      case '"': {
        out->type = JsonValue::Type::kString;
        return read_string(&out->text);
      }
      case 't':
        if (!literal("true", 4)) return false;
        out->type = JsonValue::Type::kBoolean;
        out->boolean = true;
        return true;
      case 'f':
        if (!literal("false", 5)) return false;
        out->type = JsonValue::Type::kBoolean;
        out->boolean = false;
        return true;
      case 'n':
        if (!literal("null", 4)) return false;
        out->type = JsonValue::Type::kNull;
        return true;
      default:
        return read_number(out);
    }
  }

  bool read_object(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kObject;
    ++pos_;  // '{'
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_space();
      if (pos_ >= text_.size() || text_[pos_] != '"') return false;
      std::string key;
      if (!read_string(&key)) return false;
      skip_space();
      if (pos_ >= text_.size() || text_[pos_] != ':') return false;
      ++pos_;
      skip_space();
      JsonValue value;
      if (!read_value(&value, depth + 1)) return false;
      out->members.emplace_back(std::move(key), std::move(value));
      skip_space();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool read_array(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kArray;
    ++pos_;  // '['
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_space();
      JsonValue value;
      if (!read_value(&value, depth + 1)) return false;
      out->items.push_back(std::move(value));
      skip_space();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool read_hex(unsigned* value) {
    if (text_.size() - pos_ < 4) return false;
    unsigned result = 0;
    for (int i = 0; i < 4; ++i) {
      char c = text_[pos_ + i];
      result <<= 4;
      if (c >= '0' && c <= '9') {
        result |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        result |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        result |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    pos_ += 4;
    *value = result;
    return true;
  }

  static void append_utf8(std::string* out, unsigned code) {
    if (code < 0x80) {
      out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool read_string(std::string* out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') return false;
    ++pos_;
    out->clear();
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) return false;
        char escape = text_[pos_++];
        switch (escape) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            unsigned code = 0;
            if (!read_hex(&code)) return false;
            if (code >= 0xD800 && code <= 0xDBFF) {
              // A surrogate pair, the low part follows.
              if (text_.size() - pos_ >= 2 && text_[pos_] == '\\' &&
                  text_[pos_ + 1] == 'u') {
                size_t save = pos_;
                pos_ += 2;
                unsigned low = 0;
                if (!read_hex(&low)) return false;
                if (low >= 0xDC00 && low <= 0xDFFF) {
                  code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else {
                  pos_ = save;
                }
              }
            }
            append_utf8(out, code);
            break;
          }
          default:
            return false;
        }
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20) return false;
      out->push_back(c);
      ++pos_;
    }
    return false;
  }

  bool read_number(JsonValue* out) {
    size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    bool digits = false;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
      digits = true;
    }
    bool real = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      real = true;
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
        digits = true;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      real = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
      bool exponent = false;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
        exponent = true;
      }
      if (!exponent) return false;
    }
    if (!digits) return false;
    std::string number(text_.substr(start, pos_ - start));
    if (!real) {
      errno = 0;
      char* end = nullptr;
      long long value = strtoll(number.c_str(), &end, 10);
      if (errno == 0 && end != nullptr && *end == '\0') {
        out->type = JsonValue::Type::kInteger;
        out->integer = value;
        out->real = static_cast<double>(value);
        out->text = number;
        return true;
      }
    }
    errno = 0;
    char* end = nullptr;
    double value = strtod(number.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(value)) return false;
    out->type = JsonValue::Type::kReal;
    out->real = value;
    out->text = number;
    return true;
  }

  std::string_view text_;
  size_t pos_ = 0;
};

}  // namespace

bool parse_json(std::string_view text, JsonValue* out) {
  Reader reader(text);
  return reader.read(out);
}

void json_write_string(std::string* out, const std::string& value) {
  out->push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\t': out->append("\\t"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      default:
        if (c < 0x20) {
          char buffer[8];
          snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out->append(buffer);
        } else {
          out->push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out->push_back('"');
}

void json_write_real(std::string* out, double value) {
  if (!std::isfinite(value)) {
    out->append("null");
    return;
  }
  char buffer[40];
  snprintf(buffer, sizeof(buffer), "%.17g", value);
  out->append(buffer);
}

void json_write_integer(std::string* out, int64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  out->append(buffer);
}

void json_write_value(std::string* out, const JsonValue& value) {
  switch (value.type) {
    case JsonValue::Type::kNull:
      out->append("null");
      break;
    case JsonValue::Type::kBoolean:
      out->append(value.boolean ? "true" : "false");
      break;
    case JsonValue::Type::kInteger:
      json_write_integer(out, value.integer);
      break;
    case JsonValue::Type::kReal:
      json_write_real(out, value.real);
      break;
    case JsonValue::Type::kString:
      json_write_string(out, value.text);
      break;
    case JsonValue::Type::kArray: {
      out->push_back('[');
      for (size_t i = 0; i < value.items.size(); ++i) {
        if (i != 0) out->push_back(',');
        json_write_value(out, value.items[i]);
      }
      out->push_back(']');
      break;
    }
    case JsonValue::Type::kObject: {
      out->push_back('{');
      for (size_t i = 0; i < value.members.size(); ++i) {
        if (i != 0) out->push_back(',');
        json_write_string(out, value.members[i].first);
        out->push_back(':');
        json_write_value(out, value.members[i].second);
      }
      out->push_back('}');
      break;
    }
  }
}

}  // namespace mvdb