# [RFC][Clang] Elementwise builtins for converting encoded FP8, FP6, and FP4 values

> Status: Draft for discussion.
> Two prototypes exist: an earlier form taking a format string and a destination type
> ([PR #212647](https://github.com/llvm/llvm-project/pull/212647)), and a later form encoding
> both the source format and the destination in the builtin name.
> This RFC proposes a third form that keeps the type argument and drops the string.

## Summary

This RFC proposes target-independent Clang builtins for converting integer-encoded FP8, FP6, and FP4 values to native floating-point types.
They lower to the existing `llvm.convert.from.arbitrary.fp` intrinsic, so no LLVM IR or backend change is required.

The builtin name identifies the source encoding, and the destination is an ordinary type argument:

```c
__builtin_elementwise_convert_from_<source_format>(bits, destination_type)
```

For example:

```c
float value = __builtin_elementwise_convert_from_f8e4m3fn(encoded, float);
```

The first operand may be a scalar integer or a fixed-length integer vector.
Vector conversion is elementwise, and the destination is written as the complete result type, as in `__builtin_convertvector`:

```c
typedef unsigned char uchar4 __attribute__((ext_vector_type(4)));
typedef float float4 __attribute__((ext_vector_type(4)));

unsigned char bits;
uchar4 bits4;

float scalar = __builtin_elementwise_convert_from_f8e4m3fn(bits, float);
float4 packed = __builtin_elementwise_convert_from_f8e4m3fn(bits4, float4);
```

Five source formats give five builtins, one per encoding.

## Motivation

Low-precision floating-point formats are commonly stored as integers because C and C++ do not provide portable arithmetic types for these encodings.
Libraries must nevertheless convert these encodings to `_Float16`, `__bf16`, `float`, and `double`.

HIP is the initial consumer.
HIP FP8 headers currently use AMDGPU-specific scalar and packed builtins such as `__builtin_amdgcn_cvt_f32_fp8` and `__builtin_amdgcn_cvt_pk_f32_fp8`.

The scalar builtin consumes an instruction-shaped 32-bit operand and a byte index, while two values require a different packed builtin:

```c++
typedef float float2 __attribute__((ext_vector_type(2)));

static inline float convert_one(unsigned char bits) {
  int word = bits;
  return __builtin_amdgcn_cvt_f32_fp8(word, 0);
}

static inline float2 convert_two(unsigned short bits) {
  int word = bits;
  return __builtin_amdgcn_cvt_pk_f32_fp8(word, false);
}
```

The header must therefore choose a scalar or packed instruction shape and maintain separate target-specific and software paths instead of expressing one elementwise conversion.

The proposed builtin expresses only the semantic conversion and accepts an integer vector with the same spelling.
A header can therefore use one operation for host and device compilation and leave scalar, packed, or generic lowering to the compiler.

Leaving that choice to the compiler does not cost code quality on the initial consumer.
On gfx950, gfx1170, and gfx1250, a `<2 x i8>` OCP FP8 conversion to `<2 x float>` selects a single `v_cvt_pk_f32_fp8_e32` and a `<4 x i8>` conversion selects two, which is what the packed target builtin produces today.
`llvm/test/CodeGen/AMDGPU/arbitrary-fp-to-float-fp8-hw.ll` covers this.

SYCL/SPIR-V, OpenCL, and portable low-precision C++ libraries could use the same semantic interface across targets.
Target-specific builtins remain appropriate when a source API intentionally exposes byte selection or another ISA-specific operation.

## Existing LLVM support

LLVM IR already provides:

```llvm
declare <fNxM> @llvm.convert.from.arbitrary.fp.<fNxM>.<iNxM>(
    <iNxM> %value, metadata %interpretation)
```

The integer operand contains the source floating-point encoding.
The metadata operand identifies its interpretation.
The intrinsic is overloaded on the source and destination types and supports scalar and vector forms.

LLVM also provides the inverse `llvm.convert.to.arbitrary.fp`, which additionally takes a rounding mode and a saturation flag.
This RFC does not propose builtins for it, but its shape informs the naming discussion below.

Target-independent SelectionDAG expansion currently supports these source formats:

| Name suffix | LLVM interpretation | Required integer element width |
| --- | --- | ---: |
| `f8e5m2` | `Float8E5M2` | 8 |
| `f8e4m3fn` | `Float8E4M3FN` | 8 |
| `f6e3m2fn` | `Float6E3M2FN` | 6 |
| `f6e2m3fn` | `Float6E2M3FN` | 6 |
| `f4e2m1fn` | `Float4E2M1FN` | 4 |

The initial Clang API is deliberately limited to this set.
Verifier recognition or APFloat support alone is not sufficient, because an accepted source program must not fail later during generic code generation, where the diagnostic has no source location.

AMDGPU already custom-lowers the two OCP FP8 formats to f32 on targets with OCP FP8 conversion instructions and to f16 on targets with separate FP8-to-f16 support.
That support was added by [PR #194144](https://github.com/llvm/llvm-project/pull/194144).
Without those features, the same conversions use generic expansion, as do all other supported combinations.
This backend status is informational and may change without changing the proposed source API.

## Scope

The initial proposal does not:

* add or change LLVM IR intrinsics,
* expose formats without target-independent generic lowering,
* convert native floating-point values to narrow encodings, which is a scoping choice rather than a missing LLVM capability,
* define deterministic or stochastic rounding controls,
* support scalable or target-specific vector kinds, or
* support constant evaluation.

## Proposed API

### Naming and feature granularity

The builtin spelling is:

```c
__builtin_elementwise_convert_from_<source_format>(bits, destination_type)
```

The source encoding is in the name because it is not a C type and cannot be spelled any other way without inventing one.
The destination is a type argument because it *is* a C type, and the type system can already answer every question about it.

The `elementwise` prefix is used in its established sense of an operation that accepts a scalar or a fixed-length vector and applies per element.
Unlike the existing `__builtin_elementwise_*` builtins, this family is not type preserving, because the source encoding has no C type that could also be the result type.
`__builtin_convertvector` is the existing precedent for a Clang builtin whose destination is supplied as a type argument.

This split gives one builtin per source encoding:

| Builtin |
| --- |
| `__builtin_elementwise_convert_from_f8e5m2` |
| `__builtin_elementwise_convert_from_f8e4m3fn` |
| `__builtin_elementwise_convert_from_f6e3m2fn` |
| `__builtin_elementwise_convert_from_f6e2m3fn` |
| `__builtin_elementwise_convert_from_f4e2m1fn` |

### Destination type argument

The second argument is the complete result type, following `__builtin_convertvector`.
A scalar source takes a scalar destination, and a vector source takes a vector destination whose element count matches the source.
A mismatched element count is diagnosed.

The destination element type is validated by its floating-point semantics rather than by a fixed list of spellings.
Accepted semantics are IEEE half, bfloat16, IEEE single, and IEEE double, which admits `_Float16`, `__bf16`, `float`, `double`, OpenCL `half`, and any typedef of them.
x87 extended, PPC double-double, IEEE quad, and `__mfp8` are rejected because the intrinsic lowering does not support them.

Validating by semantics rather than by spelling has a practical benefit over encoding the destination in the name: a caller passes whatever type it already uses, including a typedef, and OpenCL code passes `half` directly instead of converting from `_Float16` afterwards.

`__fp16` and OpenCL `half` are the same type in Clang and both have IEEE half semantics, so a purely semantic rule accepts `__fp16` too.
Outside OpenCL its arithmetic is promoted, which makes it a questionable result type.
Whether to accept it is a language-mode decision rather than a semantic one, and is open question 3.

Normal language and target availability rules apply to the destination type.
Because it is an ordinary type argument, those rules apply through the usual type machinery rather than a builtin-specific check, so an unsupported destination produces the same diagnostic that any other use of the type would.

### Extension to the narrowing direction

Naming the encoding and passing native types as arguments extends to `llvm.convert.to.arbitrary.fp` without a combinatorial explosion:

```c
unsigned char bits = __builtin_elementwise_convert_to_f8e4m3fn(value, rounding, saturate);
```

The narrowing direction needs no destination type argument at all, because the format determines the integer container width.
It needs rounding and saturation operands, which must be ordinary constant arguments under any naming scheme.

That yields five widening builtins and five narrowing builtins, symmetric and each named by the encoding.
A scheme that encodes the native type in the name instead cannot do this; see the alternatives below.

### Operand and result types

Each builtin takes exactly two arguments: the source value and the destination type.

The source element type must:

* be an integer type other than `_Bool` or `bool`,
* not be an enumeration type, and
* have exactly the bit width required by the source format.

Signedness has no semantic effect.
The integer is a bit container rather than a numerically converted value.

For example, `char`, `signed char`, and `unsigned char` are valid FP8 containers only on targets where those types are 8 bits wide.
`_BitInt(8)` is also a valid FP8 container.
FP6 and FP4 scalar operands require exact-width types such as `_BitInt(6)` and `_BitInt(4)`.
Wider integer containers are rejected even when their low bits contain the desired encoding.

Usual lvalue-to-rvalue conversion applies.
Integer promotions and usual arithmetic conversions do not apply to the source value.
An integer literal of type `int` therefore requires an explicit cast to an accepted container type.

A scalar source requires a scalar destination and a vector source requires a vector destination with the same element count.
The result type is exactly the type written by the caller, so the destination decides whether the result is a GNU `vector_size` or a Clang/OpenCL `ext_vector_type` vector, and the two may be mixed.

Only those two fixed-length vector kinds are supported initially, on either side.
Scalable vectors, sizeless vectors, matrices, and target-specific fixed vector kinds such as NEON, AltiVec, fixed-length SVE, and fixed-length RVV are rejected.

Clang permits `_BitInt` vector elements only when their width is a power of two.
FP4 vectors can therefore use `_BitInt(4)` elements.
The two FP6 formats are initially scalar-only because `_BitInt(6)` vector elements cannot be expressed.

For dependent C++ calls, source validation and result formation are deferred until instantiation.

### Conversion semantics

Each integer element is interpreted as the named source format and converted independently to the destination type.
The exact zero, infinity, NaN, and payload behavior is defined by `llvm.convert.from.arbitrary.fp`.

The properties that matter to a caller are:

* every source bit pattern produces a defined result, so no operand value is undefined behavior or poison,
* all supported combinations are exact widening conversions for finite values,
* the `FN` formats have no infinity encoding and the FP6 and FP4 formats have no NaN encoding, so those results never occur for those sources,
* for a source format that does encode NaN, the result is a NaN of the same quiet or signaling character, but the payload may be truncated or extended, and
* the conversion does not consult a dynamic rounding mode and has no floating-environment side effects, so it may be speculated.

The vector operation does not promise a particular instruction or packing strategy.
A target may use one packed instruction, several scalar instructions, or generic expansion.

### Feature detection

Each source encoding has a distinct query:

```c
#if __has_builtin(__builtin_elementwise_convert_from_f8e4m3fn)
// Clang recognizes this source encoding.
#endif
```

`__has_builtin` works on these spellings even though they are parsed as keywords, as it already does for `__builtin_convertvector`.
Under offloading it considers the currently active compilation target.
It reports frontend recognition of the spelling, and does not report native instruction support or guarantee a particular lowering.

This is the right granularity because the destination is no longer part of the spelling.
A per-pair query would in any case not have answered the question a header actually asks, since recognition of a spelling never implied that the destination type was usable on the target.
Destination availability is instead answered by the ordinary means for a type, and produces the ordinary diagnostic.

The builtins are not gated on AMDGPU or another target feature because all supported source formats have generic lowering.

### Constant expressions

The initial implementation does not support constant evaluation.
`__has_constexpr_builtin` returns zero, and a use in a context that requires a constant expression is diagnosed.

This is the weakest part of the proposal.
`__builtin_convertvector` is constant evaluable in both the tree evaluator and the bytecode interpreter, and the evaluation itself is small, because `APFloat` already has every source semantic involved and the conversions are exact.
Deferring it does not change the spelling or the type rules, but it does change the answer to `__has_constexpr_builtin` after the builtins have shipped.
Open question 2 asks whether it belongs in the initial patch.

## Lowering and Clang implementation

The scalar example in the summary lowers to:

```llvm
%result = call float @llvm.convert.from.arbitrary.fp.f32.i8(
    i8 %bits, metadata !"Float8E4M3FN")
```

A type argument cannot be parsed as an ordinary call argument, so each spelling is a keyword with a parser production that calls `ParseTypeName`, and the result is a dedicated expression node rather than a `CallExpr`.
This follows `__builtin_convertvector` exactly, and it is the main cost of the proposal.
Beyond Sema and CodeGen it requires:

* a `TokenKinds.def` keyword per spelling and a `ParseExpr.cpp` production,
* a `StmtNodes.td` node with dependence computation, classification, printing, and profiling,
* `TreeTransform` and `ASTImporter` support,
* a serialization record and reader and writer support,
* an Itanium mangling rule, and
* `RecursiveASTVisitor`, libclang, and static analyzer entries.

This cost is known rather than estimated.
The earlier string-and-type prototype implemented all of it in about 480 added lines across 30 non-test files.
The proposed form is that work minus the format string and its validation, plus one keyword per source encoding instead of one in total.

The five source encodings map to LLVM interpretation names in one place, and CodeGen emits the scalar or vector intrinsic with that metadata.

## Alternatives considered

### Source and destination both encoded in the name

The most recent prototype encodes both types in the spelling and takes a single argument:

```c
float value = __builtin_elementwise_convert_from_f8e4m3fn_f32(encoded);
```

This is a real alternative with one significant advantage: the builtins stay ordinary `CallExpr`s.
No keyword, parser production, expression node, serialization record, mangling rule, or AST visitor entry is required, and constant evaluation would later be an ordinary builtin case rather than two new evaluator nodes.
Moving to it from the string-and-type prototype removed roughly 460 non-test lines, most of it parser, AST, serialization, and mangling support.

It was not chosen for three reasons:

* It needs twenty spellings for the initial set, one per source and destination pair, and each new destination type multiplies the whole set.
* It does not extend to the narrowing direction. Encoding the native type in the name there would require five formats times four native types times five rounding modes times two saturation choices, so that family would have to adopt a different convention and the two directions would diverge.
* Its main claimed benefit, a per-pair `__has_builtin`, does not do what a header needs. The spellings are target independent, so `__has_builtin(__builtin_elementwise_convert_from_f8e5m2_bf16)` is true even where `__bf16` cannot be used, and the header still needs a separate type-availability check.

The tradeoff is therefore custom AST surface against name-space growth and an asymmetry with the narrowing direction.
Open question 1 asks whether that trade is judged correctly.

### String and destination type argument

The original prototype in [PR #212647](https://github.com/llvm/llvm-project/pull/212647) used a string and a destination type argument:

```c
__builtin_convert_from_arbitrary_fp(bits, "Float8E4M3FN", float)
```

This mirrored the IR intrinsic, but it prevented any `__has_builtin` query for an individual format and drew pushback on using string arguments to select semantics.
The form proposed here keeps its destination type argument and replaces the string with the builtin name.

### Element type instead of full destination type

The type argument could be the destination *element* type, with the result vector formed from the source element count:

```c
float4 packed = __builtin_elementwise_convert_from_f8e4m3fn(bits4, float);
```

This makes the scalar and vector call sites identical and removes the element-count mismatch diagnostic.
It was not chosen because it diverges from `__builtin_convertvector` for no strong reason, hides the result type from the reader at the call site, and leaves the result's vector kind to be inferred from the source rather than stated.
Writing the full type also keeps the door open to destinations that are not a plain elementwise widening of the source shape.

### Enum-selected source and destination

One builtin could take compiler-provided enum constants for both the source format and the destination type:

```c
float value = __builtin_elementwise_convert_from_arbitrary_fp(
    bits, __clang_arbitrary_fp_format_f8e4m3fn,
    __clang_arbitrary_fp_destination_f32);
```

This keeps an ordinary `CallExpr` and avoids both the name growth and the parser work.
It was not chosen because it reintroduces the original problem in a new spelling: the destination is a C type, and describing it with a compiler-provided enumerator rather than the type itself loses typedefs, loses the language's own availability rules, and makes the result type depend on an argument value.

### Wider integer containers

The source could accept any integer at least as wide as the format and ignore the high bits, which would let FP6 and FP4 values travel in `unsigned char` and would make FP6 vectors expressible today.
It was not chosen because silently ignoring set high bits hides encoding bugs at the one point where the program asserts what its bits mean.
Open question 4 revisits this, since it is the direct cause of the FP6 vector limitation.

### Other options

An f32-only builtin would reduce the API surface because all supported finite values fit in `float`, but would lose direct f16 instruction selection.
First-class narrow types would have broad language and ABI effects, while header bit manipulation is verbose, error-prone, and harder to optimize.

## Deferred formats and future directions

### Additional source formats

The LLVM verifier recognizes seven additional arbitrary floating-point interpretation names that `getArbitraryFPSemantics` does not yet admit to code generation:

| Expected name suffix | LLVM interpretation |
| --- | --- |
| `f8e5m2fnuz` | `Float8E5M2FNUZ` |
| `f8e4m3` | `Float8E4M3` |
| `f8e4m3fnuz` | `Float8E4M3FNUZ` |
| `f8e4m3b11fnuz` | `Float8E4M3B11FNUZ` |
| `f8e3m4` | `Float8E3M4` |
| `f8e8m0fnu` | `Float8E8M0FNU` |
| `f8e5m3fnu` | `Float8E5M3FNU` |

These remain unexposed until target-independent lowering exists for them.
Under the proposed naming each one costs a single new builtin rather than one per destination type.

The LangRef description of `llvm.convert.from.arbitrary.fp` currently omits `Float8E5M3FNU` from its list of interpretation strings even though the verifier accepts it, so a reader cross-referencing that list will count six rather than seven.
That is an upstream documentation gap and is worth fixing separately.

## Testing and rollout

The Clang patch should cover:

* feature queries for the five initial and seven deferred source encodings, plus a deferred constant-evaluation check,
* result types and intrinsic emission for every source encoding against each accepted destination semantics,
* accepted destination spellings including `_Float16`, `__bf16`, `float`, `double`, OpenCL `half`, and typedefs, and rejection of x87 extended, PPC double-double, IEEE quad, and `__mfp8`,
* signed and unsigned exact-width sources and diagnostics for invalid source types or widths,
* GNU-vector, extended-vector, FP4-vector, FP6-scalar, and unsupported-vector-kind tests,
* dependent templates, mangling, serialization, and AST printing for the new expression node, and
* C, C++, OpenCL, and target availability tests for the destination type.

Existing LLVM AMDGPU tests cover native OCP FP8 lowering and generic fallback.
The Clang patch should emit the same intrinsic forms rather than duplicate backend instruction-selection tests.

After the RFC reaches consensus:

1. Rework the existing prototype to the agreed form and submit builtin, Sema, CodeGen, documentation, and test changes.
2. Adopt the builtins in HIP headers, guarded by both `__has_builtin` and the usual availability check for the destination type, while retaining FNUZ and older-compiler fallbacks.
3. Add further source encodings only after their generic LLVM lowering is available.
4. Consider constant evaluation and the narrowing family separately.

## Open questions

1. Is a dedicated expression node, as `__builtin_convertvector` already uses, an acceptable price for the type argument, or is the twenty-name `CallExpr` form preferable despite the asymmetry with the narrowing direction?
2. Should constant evaluation be in the initial patch rather than deferred?
3. Should `__fp16` be an accepted destination, given that its arithmetic is promoted in some language modes?
4. Should the source accept containers wider than the format, which would allow FP6 vectors, at the cost of silently ignoring high bits?
5. Is exposing scalar-only FP6 conversions useful if question 4 is answered no?

## References

* [`llvm.convert.from.arbitrary.fp` in the LLVM Language Reference](https://llvm.org/docs/LangRef.html#llvm-convert-from-arbitrary-fp-intrinsic)
* [Prototype pull request #212647](https://github.com/llvm/llvm-project/pull/212647)
* [AMDGPU custom lowering pull request #194144](https://github.com/llvm/llvm-project/pull/194144)
* [APFloat UE5M3 pull request #210720](https://github.com/llvm/llvm-project/pull/210720)
* [RFC: Add New Set of Vector Math Builtins](https://discourse.llvm.org/t/rfc-add-new-set-of-vector-math-builtins/58996)
* [[RFC] Introducing elementwise clz/ctz builtins](https://discourse.llvm.org/t/rfc-introducing-elementwise-clz-ctz-builtins/85862)
* [[RFC] `__has_builtin` behavior on offloading targets](https://discourse.llvm.org/t/rfc-has-builtin-behavior-on-offloading-targets/84964)
* [SPIR-V representation of OCP low-precision types](https://github.com/KhronosGroup/SPIRV-LLVM-Translator/blob/main/docs/OCPTypesRepresentationInLLVM.rst)
* [HIP low-precision floating-point types](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/low_fp_types.html)
* [HIP FP8 header](https://github.com/ROCm/clr/blob/develop/hipamd/include/hip/amd_detail/amd_hip_fp8.h)
