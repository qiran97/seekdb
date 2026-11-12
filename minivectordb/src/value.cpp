#include "value.h"

#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#define MVDB_X86 1
#include <immintrin.h>
#endif

namespace mvdb {

size_t utf8_length_of(std::string_view text) { return utf8_length(text); }

Status parse_vector_literal(std::string_view text, uint32_t expected_dim,
                             VectorValue* out) {
  out->values.clear();
  if (expected_dim != 0) out->values.reserve(expected_dim);
  // The literal is read without copying it, a vector of a hundred elements
  // is parsed for every row that is written.
  size_t begin = 0;
  size_t stop = text.size();
  while (begin < stop && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (stop > begin && std::isspace(static_cast<unsigned char>(text[stop - 1]))) --stop;
  std::string_view body = text.substr(begin, stop - begin);
  if (!body.empty() && body.front() == '[') {
    if (body.back() != ']') {
      return Status::Error(ErrorCode::kInvalidVector,
                           "malformed vector literal: missing closing ']'");
    }
    body = body.substr(1, body.size() - 2);
  }
  if (body.empty()) {
    return Status::Error(ErrorCode::kInvalidVector, "empty vector literal");
  }

  size_t pos = 0;
  const size_t size = body.size();
  const char* const data = body.data();
  while (pos <= size) {
    size_t comma = body.find(',', pos);
    const size_t element_end = comma == std::string_view::npos ? size : comma;
    // The element is trimmed in place.
    size_t first = pos;
    size_t last = element_end;
    while (first < last && std::isspace(static_cast<unsigned char>(data[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(data[last - 1]))) --last;
    if (first == last) {
      return Status::Error(ErrorCode::kInvalidVector,
                           "malformed vector literal: empty element");
    }
    // The element is only copied for the error message.
    const auto element_error = [&](const char* reason) {
      return Status::Error(ErrorCode::kInvalidVector,
                           std::string(reason) + ": " +
                               std::string(body.substr(first, last - first)));
    };
    // Reject textual NaN / Infinity spellings explicitly, strtod would
    // happily accept them.  The element can only hold such a spelling if it
    // has a letter n or i, the copy of the element is only needed for the
    // error message.
    bool has_letter = false;
    for (size_t i = first; i < last; ++i) {
      char c = data[i];
      if (c == 'n' || c == 'N' || c == 'i' || c == 'I') {
        has_letter = true;
        break;
      }
    }
    if (has_letter) {
      std::string lowered = to_lower(body.substr(first, last - first));
      if (lowered.find("nan") != std::string::npos ||
          lowered.find("inf") != std::string::npos) {
        return element_error("vector element is not a finite number");
      }
    }
    errno = 0;
    char* parsed_end = nullptr;
    const char* const start = data + first;
    double value = strtod(start, &parsed_end);
    if (parsed_end == start || parsed_end > data + last) {
      return element_error("vector element is not a number");
    }
    // Only spaces may follow the number inside the element.
    const char* scan = parsed_end;
    while (scan < data + last && std::isspace(static_cast<unsigned char>(*scan))) ++scan;
    if (scan != data + last) {
      return element_error("vector element is not a number");
    }
    if (!std::isfinite(value)) {
      return element_error("vector element is not a finite number");
    }
    if (value > static_cast<double>(FLT_MAX) || value < -static_cast<double>(FLT_MAX)) {
      return element_error("vector element overflows float32");
    }
    out->values.push_back(static_cast<float>(value));
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
    if (pos > size) break;
  }

  if (out->values.empty()) {
    return Status::Error(ErrorCode::kInvalidVector, "empty vector literal");
  }
  if (expected_dim != 0 && out->values.size() != expected_dim) {
    return Status::Error(ErrorCode::kDimensionMismatch,
                         "vector dimension mismatch: expected " +
                             std::to_string(expected_dim) + ", got " +
                             std::to_string(out->values.size()));
  }
  return Status::Ok();
}

std::string format_vector(const std::vector<float>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ", ";
    out += format_float(values[i]);
  }
  out += "]";
  return out;
}

namespace {

#if defined(MVDB_X86)
__attribute__((target("avx2,fma"))) double l2_distance_squared_avx2(const float* a,
                                                                    const float* b,
                                                                    size_t dim) {
  __m256 sum = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 diff = _mm256_sub_ps(va, vb);
    sum = _mm256_add_ps(sum, _mm256_mul_ps(diff, diff));
  }
  // Horizontal sum of the eight float32 lanes.
  __m128 low = _mm256_castps256_ps128(sum);
  __m128 high = _mm256_extractf128_ps(sum, 1);
  __m128 total = _mm_add_ps(low, high);
  __m128 high2 = _mm_movehl_ps(total, total);
  total = _mm_add_ps(total, high2);
  __m128 high3 = _mm_shuffle_ps(total, total, 0x55);
  total = _mm_add_ss(total, high3);
  float partial = _mm_cvtss_f32(total);

  double acc = 0.0;
  for (; i < dim; ++i) {
    double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    acc += diff * diff;
  }
  acc += static_cast<double>(partial);
  return acc;
}
#endif

}  // namespace

double l2_distance_squared(const float* a, const float* b, size_t dim) {
#if defined(MVDB_X86)
  static const bool has_avx2 = __builtin_cpu_supports("avx2");
  if (has_avx2) return l2_distance_squared_avx2(a, b, dim);
#endif
  double acc = 0.0;
  for (size_t i = 0; i < dim; ++i) {
    double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    acc += diff * diff;
  }
  return acc;
}

double l2_distance(const float* a, const float* b, size_t dim) {
  return std::sqrt(l2_distance_squared(a, b, dim));
}

}  // namespace mvdb