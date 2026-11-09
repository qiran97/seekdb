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
  std::string body = trim(text);
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
  while (pos <= size) {
    size_t comma = body.find(',', pos);
    std::string token = trim(
        std::string_view(body).substr(pos, comma == std::string::npos ? std::string::npos
                                                                     : comma - pos));
    if (token.empty()) {
      return Status::Error(ErrorCode::kInvalidVector,
                           "malformed vector literal: empty element");
    }
    // Reject textual NaN / Infinity spellings explicitly, strtod would
    // happily accept them.
    std::string lowered = to_lower(token);
    if (lowered.find("nan") != std::string::npos ||
        lowered.find("inf") != std::string::npos) {
      return Status::Error(ErrorCode::kInvalidVector,
                           "vector element is not a finite number: " + token);
    }
    errno = 0;
    char* end = nullptr;
    double value = strtod(token.c_str(), &end);
    if (end == token.c_str() || (end != nullptr && *end != '\0')) {
      return Status::Error(ErrorCode::kInvalidVector,
                           "vector element is not a number: " + token);
    }
    if (!std::isfinite(value)) {
      return Status::Error(ErrorCode::kInvalidVector,
                           "vector element is not a finite number: " + token);
    }
    if (value > static_cast<double>(FLT_MAX) || value < -static_cast<double>(FLT_MAX)) {
      return Status::Error(ErrorCode::kInvalidVector,
                           "vector element overflows float32: " + token);
    }
    out->values.push_back(static_cast<float>(value));
    if (comma == std::string::npos) break;
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
__attribute__((target("avx2,fma"))) double l2_distance_avx2(const float* a,
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
  return std::sqrt(acc);
}
#endif

}  // namespace

double l2_distance(const float* a, const float* b, size_t dim) {
#if defined(MVDB_X86)
  static const bool has_avx2 = __builtin_cpu_supports("avx2");
  if (has_avx2) return l2_distance_avx2(a, b, dim);
#endif
  double acc = 0.0;
  for (size_t i = 0; i < dim; ++i) {
    double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    acc += diff * diff;
  }
  return std::sqrt(acc);
}

}  // namespace mvdb