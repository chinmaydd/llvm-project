// RUN: %clang_cc1 -triple riscv64 -target-feature +v -fsyntax-only -verify %s

float sizeless_source(__rvv_uint8m1_t src) {
  return __builtin_elementwise_convert_from_f8e5m2(src, float); // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 has sizeless vector type; only fixed-length vectors are supported}}
}

__rvv_float32m1_t sizeless_destination(unsigned char src) {
  return __builtin_elementwise_convert_from_f8e5m2(src, __rvv_float32m1_t); // expected-error {{second argument to __builtin_elementwise_convert_from_f8e5m2 has sizeless vector type; only fixed-length vectors are supported}}
}
