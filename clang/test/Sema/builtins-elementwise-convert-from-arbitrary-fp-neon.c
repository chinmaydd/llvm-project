// RUN: %clang_cc1 -triple aarch64-none-linux-gnu -target-feature +neon \
// RUN:   -fsyntax-only -verify %s

typedef unsigned char uint8x8_t __attribute__((neon_vector_type(8)));
typedef float float32x2_t __attribute__((neon_vector_type(2)));
typedef float f8 __attribute__((ext_vector_type(8)));

void test_neon_source(uint8x8_t src) {
  (void)__builtin_elementwise_convert_from_f8e5m2(src, f8); // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 has unsupported vector type 'uint8x8_t' (vector of 8 'unsigned char' values); only GNU and extended fixed-length vectors are supported}}
}

void test_neon_destination(unsigned char src) {
  (void)__builtin_elementwise_convert_from_f8e5m2(src, float32x2_t); // expected-error {{second argument to __builtin_elementwise_convert_from_f8e5m2 has unsupported vector type 'float32x2_t' (vector of 2 'float' values); only GNU and extended fixed-length vectors are supported}}
}
