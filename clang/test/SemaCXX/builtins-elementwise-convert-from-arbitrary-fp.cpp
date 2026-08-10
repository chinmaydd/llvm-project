// RUN: %clang_cc1 -triple x86_64-unknown-unknown -std=c++17 -fsyntax-only \
// RUN:   -verify %s

template <typename Dst, typename Src> Dst conv(Src b) {
  return __builtin_elementwise_convert_from_f8e5m2(b, Dst); // expected-error {{argument type 'unsigned short' must be an integer type 8 bits wide to match format 'Float8E5M2'}} \
                                                            // expected-error {{second argument to __builtin_elementwise_convert_from_f8e5m2 must be a floating-point type or a vector of floating-point types}}
}

float instantiate_ok(unsigned char b) { return conv<float, unsigned char>(b); }
double instantiate_ok2(signed char b) { return conv<double, signed char>(b); }

// expected-note@+1 {{in instantiation of function template specialization 'conv<float, unsigned short>' requested here}}
float instantiate_bad_src(unsigned short b) { return conv<float, unsigned short>(b); }

// expected-note@+1 {{in instantiation of function template specialization 'conv<int, unsigned char>' requested here}}
int instantiate_bad_dst(unsigned char b) { return conv<int, unsigned char>(b); }

template <typename Dst> Dst non_dependent_bad_src_width(unsigned short b) {
  return __builtin_elementwise_convert_from_f8e5m2(b, Dst); // expected-error {{argument type 'unsigned short' must be an integer type 8 bits wide to match format 'Float8E5M2'}}
}
float instantiate_non_dependent_bad_src_width(unsigned short b) {
  return non_dependent_bad_src_width<float>(b);
}

template <typename Dst> Dst non_dependent_bad_src_type(float b) {
  return __builtin_elementwise_convert_from_f8e5m2(b, Dst); // expected-error {{first argument to __builtin_elementwise_convert_from_f8e5m2 must be an integer type or a vector of integer types}}
}
float instantiate_non_dependent_bad_src_type(float b) {
  return non_dependent_bad_src_type<float>(b);
}

template <typename Src> int non_dependent_bad_dst(Src b) {
  return __builtin_elementwise_convert_from_f8e5m2(b, int); // expected-error {{second argument to __builtin_elementwise_convert_from_f8e5m2 must be a floating-point type or a vector of floating-point types}}
}
int instantiate_non_dependent_bad_dst(unsigned char b) {
  return non_dependent_bad_dst(b);
}

// A dependent source operand is validated on instantiation.
template <typename Src> float dependent_src(Src b) {
  return __builtin_elementwise_convert_from_f8e4m3fn(b, float);
}
float use_dependent_src(unsigned char b) { return dependent_src(b); }

template <typename Src>
auto deduced_result(Src src)
    -> decltype(__builtin_elementwise_convert_from_f8e5m2(src, float)) {
  return __builtin_elementwise_convert_from_f8e5m2(src, float);
}
static_assert(__is_same(decltype(deduced_result((unsigned char)0)), float), "");

void noexcept_check(unsigned char b) {
  static_assert(
      noexcept(__builtin_elementwise_convert_from_f8e5m2(b, float)), "");
}

// Constant evaluation is not supported yet.
static_assert(
    !__has_constexpr_builtin(__builtin_elementwise_convert_from_f8e5m2), "");

constexpr float constant_evaluation_is_deferred =
    __builtin_elementwise_convert_from_f8e5m2(
        (unsigned char)0, float); // expected-error@-1 {{constexpr variable 'constant_evaluation_is_deferred' must be initialized by a constant expression}}
