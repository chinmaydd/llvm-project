// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu -fsyntax-only \
// RUN:   -verify=expected,aarch64 %s
// RUN: %clang_cc1 -triple powerpc64le-unknown-linux-gnu -fsyntax-only \
// RUN:   -verify=expected,powerpc %s

typedef unsigned char v4u8 __attribute__((ext_vector_type(4)));
typedef unsigned short v4u16 __attribute__((ext_vector_type(4)));
typedef unsigned char g4u8 __attribute__((vector_size(4)));
typedef float v4f32 __attribute__((ext_vector_type(4)));
typedef float v2f32 __attribute__((ext_vector_type(2)));
typedef float g4f32 __attribute__((vector_size(16)));
typedef float my_float;
typedef my_float my_float_alias;

enum __attribute__((packed)) byte_enum {
  BYTE_ZERO,
};

// Each source encoding is queried independently.
_Static_assert(__has_builtin(__builtin_elementwise_convert_from_f8e5m2), "");
_Static_assert(__has_builtin(__builtin_elementwise_convert_from_f8e4m3fn), "");
_Static_assert(__has_builtin(__builtin_elementwise_convert_from_f6e3m2fn), "");
_Static_assert(__has_builtin(__builtin_elementwise_convert_from_f6e2m3fn), "");
_Static_assert(__has_builtin(__builtin_elementwise_convert_from_f4e2m1fn), "");

// Encodings without target-independent lowering are not exposed.
_Static_assert(!__has_builtin(__builtin_elementwise_convert_from_f8e5m2fnuz), "");
_Static_assert(!__has_builtin(__builtin_elementwise_convert_from_f8e4m3), "");
_Static_assert(!__has_builtin(__builtin_elementwise_convert_from_f8e4m3fnuz), "");
_Static_assert(!__has_builtin(__builtin_elementwise_convert_from_f8e4m3b11fnuz), "");
_Static_assert(!__has_builtin(__builtin_elementwise_convert_from_f8e3m4), "");
_Static_assert(!__has_builtin(__builtin_elementwise_convert_from_f8e8m0fnu), "");
_Static_assert(!__has_builtin(__builtin_elementwise_convert_from_f8e5m3fnu), "");
_Static_assert(!__has_builtin(__builtin_convert_from_arbitrary_fp), "");

void test_accepted(unsigned char b, unsigned _BitInt(6) b6,
                   unsigned _BitInt(4) b4, v4u8 vb, g4u8 gb) {
  (void)__builtin_elementwise_convert_from_f8e5m2(b, float);
  (void)__builtin_elementwise_convert_from_f8e4m3fn(b, float);
  (void)__builtin_elementwise_convert_from_f6e3m2fn(b6, float);
  (void)__builtin_elementwise_convert_from_f6e2m3fn(b6, float);
  (void)__builtin_elementwise_convert_from_f4e2m1fn(b4, float);
  // Only the width of the container matters, not its signedness.
  (void)__builtin_elementwise_convert_from_f8e5m2((signed char)b, float);
  (void)__builtin_elementwise_convert_from_f8e5m2(b, double);
  (void)__builtin_elementwise_convert_from_f8e5m2(vb, v4f32);
  (void)__builtin_elementwise_convert_from_f8e5m2(gb, g4f32);
  // The destination decides the result vector kind, so the two may be mixed.
  (void)__builtin_elementwise_convert_from_f8e5m2(gb, v4f32);
}

// The destination is accepted on its semantics, so typedefs work.
void test_typedef_destinations(unsigned char b, v4u8 vb) {
  _Static_assert(__builtin_types_compatible_p(
      typeof(__builtin_elementwise_convert_from_f8e5m2(b, my_float)), float),
      "");
  _Static_assert(__builtin_types_compatible_p(
      typeof(__builtin_elementwise_convert_from_f8e5m2(b, my_float_alias)),
      float), "");
  _Static_assert(__builtin_types_compatible_p(
      typeof(__builtin_elementwise_convert_from_f8e5m2(vb, v4f32)), v4f32), "");
}

// The destination is an ordinary type argument, so a target that does not
// support it reports that once, from the type itself.
void test_target_types(unsigned char b) {
#if defined(__x86_64__) || defined(__aarch64__)
  (void)__builtin_elementwise_convert_from_f8e5m2(b, _Float16);
  (void)__builtin_elementwise_convert_from_f8e5m2(b, __bf16);
#endif
#ifdef __powerpc__
  (void)__builtin_elementwise_convert_from_f8e5m2(b, _Float16); // powerpc-error {{_Float16 is not supported on this target}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, __bf16);   // powerpc-error {{__bf16 is not supported on this target}}
#endif
}

void test_result_types(unsigned char b, v4u8 vb, g4u8 gb) {
  _Static_assert(__builtin_types_compatible_p(
      typeof(__builtin_elementwise_convert_from_f8e5m2(b, float)), float), "");
  _Static_assert(__builtin_types_compatible_p(
      typeof(__builtin_elementwise_convert_from_f8e5m2(vb, v4f32)), v4f32), "");
  _Static_assert(__builtin_types_compatible_p(
      typeof(__builtin_elementwise_convert_from_f8e5m2(gb, g4f32)), g4f32), "");
}

void test_arity(unsigned char b) {
  (void)__builtin_elementwise_convert_from_f8e5m2(b);         // expected-error {{expected ','}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, float, b); // expected-error {{expected ')'}}
}

void test_width(unsigned short s, unsigned char b, unsigned _BitInt(4) b4,
                v4u16 v) {
  (void)__builtin_elementwise_convert_from_f8e5m2(s, float);   // expected-error {{argument type 'unsigned short' must be an integer type 8 bits wide to match format 'Float8E5M2'}}
  (void)__builtin_elementwise_convert_from_f6e3m2fn(b, float); // expected-error {{argument type 'unsigned char' must be an integer type 6 bits wide to match format 'Float6E3M2FN'}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b4, float);  // expected-error {{argument type 'unsigned _BitInt(4)' must be an integer type 8 bits wide to match format 'Float8E5M2'}}
  (void)__builtin_elementwise_convert_from_f8e5m2(v, v4f32);   // expected-error {{vector element type 'unsigned short' must be 8 bits wide to match format 'Float8E5M2'}}
}

void test_operand_types(unsigned char b, float f, void *p) {
  (void)__builtin_elementwise_convert_from_f8e5m2(f, float); // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 must be an integer type or a vector of integer types}}
  (void)__builtin_elementwise_convert_from_f8e5m2(p, float); // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 must be an integer type or a vector of integer types}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, int);   // expected-error {{second argument to __builtin_elementwise_convert_from_f8e5m2 must be a floating-point type or a vector of floating-point types}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, void);  // expected-error {{second argument to __builtin_elementwise_convert_from_f8e5m2 must be a floating-point type or a vector of floating-point types}}
}

void test_disallowed_integer_types(_Bool bl, enum byte_enum e) {
  (void)__builtin_elementwise_convert_from_f8e5m2(bl, float); // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 must be a non-Boolean, non-enumeration integer type or a vector of such types}}
  (void)__builtin_elementwise_convert_from_f8e5m2(e, float);  // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 must be a non-Boolean, non-enumeration integer type or a vector of such types}}
}

void test_unsupported_destinations(unsigned char b) {
#ifdef __x86_64__
  (void)__builtin_elementwise_convert_from_f8e5m2(b, __float128);  // expected-error {{destination type '__float128' is not supported by __builtin_elementwise_convert_from_f8e5m2}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, long double); // expected-error {{destination type 'long double' is not supported by __builtin_elementwise_convert_from_f8e5m2}}
#endif
#ifdef __aarch64__
  (void)__builtin_elementwise_convert_from_f8e5m2(b, long double); // aarch64-error {{destination type 'long double' is not supported by __builtin_elementwise_convert_from_f8e5m2}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, __mfp8);      // aarch64-error {{destination type '__mfp8' is not supported by __builtin_elementwise_convert_from_f8e5m2}}
#endif
#ifdef __powerpc__
  (void)__builtin_elementwise_convert_from_f8e5m2(b, long double); // powerpc-error {{destination type 'long double' is not supported by __builtin_elementwise_convert_from_f8e5m2}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, __ibm128);    // powerpc-error {{destination type '__ibm128' is not supported by __builtin_elementwise_convert_from_f8e5m2}}
#endif
}

void test_vectors(unsigned char b, v4u8 vb) {
  (void)__builtin_elementwise_convert_from_f8e5m2(vb, float); // expected-error {{second argument to __builtin_elementwise_convert_from_f8e5m2 must be of vector type}}
  (void)__builtin_elementwise_convert_from_f8e5m2(b, v4f32);  // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 must be of vector type}}
  (void)__builtin_elementwise_convert_from_f8e5m2(vb, v2f32); // expected-error {{floating-point and integer arguments to __builtin_elementwise_convert_from_f8e5m2 must have the same number of elements}}
}

void test_volatile_source(volatile unsigned char *b) {
  __builtin_assume(
      __builtin_elementwise_convert_from_f8e5m2(*b, float)); // expected-warning {{assumption is ignored because it contains (potential) side-effects}}
}
