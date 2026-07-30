// RUN: %clang_cc1 -triple aarch64-none-linux-gnu -target-feature +neon \
// RUN:   -fsyntax-only -verify %s

typedef unsigned char uint8x8_t __attribute__((neon_vector_type(8)));

void test_neon_vector(uint8x8_t src) {
  (void)__builtin_elementwise_convert_from_f8e5m2_f32(src); // expected-error {{only GNU and extended fixed-length vectors are supported}}
}
