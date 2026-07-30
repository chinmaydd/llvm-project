// RUN: %clang_cc1 -triple spir-unknown-unknown -x cl \
// RUN:   -finclude-default-header -fsyntax-only -verify %s
// expected-no-diagnostics

#if !__has_builtin(__builtin_elementwise_convert_from_f8e5m2_f32)
#error "missing elementwise arbitrary FP conversion builtin"
#endif

#if __has_builtin(__builtin_elementwise_convert_from_f8e5m3fnu_f32)
#error "deferred arbitrary FP conversion builtin is unexpectedly available"
#endif

float convert_scalar(uchar src) {
  return __builtin_elementwise_convert_from_f8e5m2_f32(src);
}

_Float16 convert_f16(uchar src) {
  return __builtin_elementwise_convert_from_f8e5m2_f16(src);
}

__bf16 convert_bf16(uchar src) {
  return __builtin_elementwise_convert_from_f8e5m2_bf16(src);
}

double convert_f64(uchar src) {
  return __builtin_elementwise_convert_from_f8e5m2_f64(src);
}

float4 convert_vector(uchar4 src) {
  return __builtin_elementwise_convert_from_f8e5m2_f32(src);
}
