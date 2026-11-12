// Value model of MiniVectorDB: integer keys, utf8 strings and float32 vectors.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base.h"

namespace mvdb {

// A parsed vector literal.
struct VectorValue {
  std::vector<float> values;
};

// Parses "[1, 0, 0, 0]" or "1,0,0,0" into float32 values.
//  * rejects non numeric elements, NaN, Infinity and values that do not fit
//    into a finite float32 (overflow).
//  * when `expected_dim` is non zero the dimension must match exactly.
Status parse_vector_literal(std::string_view text, uint32_t expected_dim,
                             VectorValue* out);

// Renders a vector using the canonical "[1, 0, 0, 0]" form.
std::string format_vector(const std::vector<float>& values);

// Euclidean (L2) distance between two float32 vectors of the same dimension.
// The accumulation is done in double precision so that the result is stable
// and independent of the vectorisation width.
double l2_distance(const float* a, const float* b, size_t dim);

// The sum of the squares of the differences of two float32 vectors.  It is
// the square of the L2 distance, the square root is monotone and the order of
// two rows is the same for the distance and for its square.  A scan that only
// sorts by the distance can therefore use the square and skip the square root
// of every row.  The accumulation is the same as in l2_distance, the square
// root of the result is bit for bit the distance.
double l2_distance_squared(const float* a, const float* b, size_t dim);

// Same as l2_distance but with a compile time friendly signature for the
// scan kernels.
inline double l2_distance(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t dim = a.size() < b.size() ? a.size() : b.size();
  return l2_distance(a.data(), b.data(), dim);
}

// Number of unicode code points, used for VARCHAR length validation.
size_t utf8_length_of(std::string_view text);

}  // namespace mvdb