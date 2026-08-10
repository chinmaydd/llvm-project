// RUN: %clang_cc1 -triple spir-unknown-unknown -cl-std=CL3.0 \
// RUN:   -cl-ext=+cl_khr_fp16 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple spir-unknown-unknown -cl-std=CL3.0 \
// RUN:   -cl-ext=+cl_khr_fp16,-__opencl_c_fp64,-cl_khr_fp64 -DNO_FP64 \
// RUN:   -fsyntax-only -verify %s

#pragma OPENCL EXTENSION cl_khr_fp16 : enable

typedef unsigned char uchar4 __attribute__((ext_vector_type(4)));
typedef float float4 __attribute__((ext_vector_type(4)));
typedef half half4 __attribute__((ext_vector_type(4)));

#if !__has_builtin(__builtin_elementwise_convert_from_f8e5m2)
#error "missing elementwise arbitrary FP conversion builtin"
#endif

float convert_scalar(unsigned char src) {
  return __builtin_elementwise_convert_from_f8e5m2(src, float);
}

// A destination type argument lets OpenCL name half directly.
half convert_half(unsigned char src) {
  return __builtin_elementwise_convert_from_f8e5m2(src, half);
}

half4 convert_half_vector(uchar4 src) {
  return __builtin_elementwise_convert_from_f8e5m2(src, half4);
}

float4 convert_vector(uchar4 src) {
  return __builtin_elementwise_convert_from_f8e5m2(src, float4);
}

// The destination goes through the ordinary type rules, so an unavailable type
// is rejected the same way it would be anywhere else.
#ifdef NO_FP64
void convert_double(unsigned char src) {
  (void)__builtin_elementwise_convert_from_f8e5m2(src, double); // expected-error {{use of type 'double' requires cl_khr_fp64 and __opencl_c_fp64 support}}
}
#else
// expected-no-diagnostics
double convert_double(unsigned char src) {
  return __builtin_elementwise_convert_from_f8e5m2(src, double);
}
#endif
