# [RFC][Clang] Elementwise builtins for converting encoded FP8 values

## Summary

Target-independent Clang spellings for converting integer-encoded FP8 values to native floating-point types. They lower to the existing `llvm.convert.from.arbitrary.fp`, so no IR change is proposed.

Each spelling names both the source encoding and the destination type, and takes one argument:

```c
__builtin_elementwise_convert_from_<source_format>_<destination_type>(bits)
```

The operand may be a scalar integer or a fixed-length integer vector. Conversion is elementwise and preserves the element count, so one spelling serves both:

```c
float  scalar = __builtin_elementwise_convert_from_f8e5m2_f32(bits);
float4 packed = __builtin_elementwise_convert_from_f8e5m2_f32(bits4);
```

Three source encodings (`f8e5m2`, `f8e4m3fn`, `f8e5m3fnu`) and three destinations (`f16` → `_Float16`, `bf16` → `__bf16`, `f32` → `float`) give nine builtins. Because the destination is in the name, these stay ordinary `CallExpr`s: no keyword, parser production, or expression node.

## Motivation

Low-precision formats are commonly stored as integers, because C and C++ have no portable arithmetic types for these encodings. Libraries must still convert them to `_Float16`, `__bf16`, and `float`.

HIP is the initial consumer. Its FP8 headers use AMDGPU-specific builtins and must pick a scalar or packed instruction shape at the source level:

```c++
__builtin_amdgcn_cvt_f32_fp8((int)bits, 0);        // instruction-shaped word + byte index
__builtin_amdgcn_cvt_pk_f32_fp8((int)bits, false); // a different builtin for two values
```

So the header maintains separate target-specific and software paths instead of expressing one elementwise conversion. The proposed builtin expresses only the semantics and accepts a vector with the same spelling, leaving scalar-or-packed selection to the compiler:

```c++
float  one = __builtin_elementwise_convert_from_f8e4m3fn_f32(bits);
float2 two = __builtin_elementwise_convert_from_f8e4m3fn_f32(bits2);
```

This does not claim SPIR-V support or universal cross-target codegen. Target-specific builtins remain appropriate where an API intentionally exposes byte selection or another ISA-specific operation.

## Existing LLVM support

```llvm
%result = call float @llvm.convert.from.arbitrary.fp.f32.i8(
    i8 %bits, metadata !"Float8E4M3FN")
```

The integer holds the encoding; the metadata names its interpretation. The intrinsic is overloaded on both types and supports scalar and vector forms.


| Source suffix | Interpretation  | Width | Proposal |
| ------------- | --------------- | ----- | -------- |
| `f8e5m2`      | `Float8E5M2`    | 8     | Exposed  |
| `f8e4m3fn`    | `Float8E4M3FN`  | 8     | Exposed  |
| `f8e5m3fnu`   | `Float8E5M3FNU` | 8     | Exposed  |
| `f6e3m2fn`    | `Float6E3M2FN`  | 6     | Deferred |
| `f6e2m3fn`    | `Float6E2M3FN`  | 6     | Deferred |
| `f4e2m1fn`    | `Float4E2M1FN`  | 4     | Deferred |


The sub-byte encodings are deferred because Clang cannot spell their vector element types coherently: there is no 6-bit element type at all, and for `<8 x _BitInt(4)>` Clang reports `sizeof` 8 while the emitted `<8 x i4>` occupies 4 bytes.

LLVM also has the inverse `llvm.convert.to.arbitrary.fp`, which additionally takes a rounding mode and a saturation flag. No builtin is proposed for it here. A narrowing family would cost one spelling per encoding under any naming scheme, since its source type is deduced from the operand and those two operands have to be ordinary constant arguments regardless, so the decision below concerns the widening direction only.

## Scope

Widening conversions only, from the three exposed encodings to `_Float16`, `__bf16`, and `float`, for scalar and fixed-length vector operands, in C, C++, OpenCL, CUDA, and HIP.

Not proposed: any IR change, the sub-byte encodings, an `f64` destination, narrowing conversions, rounding controls, constant evaluation, or scalable and target-specific vector kinds.

## Naming

The source precedes the destination, mirroring the intrinsic. `elementwise` is used in its established sense of accepting a scalar or fixed-length vector and applying per element; unlike the rest of that family this one is not type preserving, because the source encoding has no C type that could also be the result type. Suffix meanings are uniform in every language mode. `f16` is `_Float16`, not `__fp16` or the OpenCL `half`.

Encoding both types in the name keeps these ordinary calls. A destination given as a *type* argument cannot be an ordinary call argument, so it needs a keyword, a parser production, and a dedicated expression node, pulling in dependence computation, printing, profiling, `TreeTransform`, `ASTImporter`, serialization, and AST-consumer entries. Both forms were prototyped: excluding tests and docs, the type-argument form came to 28 files and 427 insertions against 7 files and 246 insertions here. Neither form needs a new mangling, since `ConvertVectorExpr` and its siblings sit in the `FIXME: invent manglings for all these` list in `ItaniumMangle.cpp` and have shipped for years without one.

The cost of the proposed form is a spelling per destination type: 3 x 3 = 9 today, and 10 x 4 = 40 if every recognized interpretation were eventually exposed with an `f64` destination. Those spellings are also permanent, because they cannot be withdrawn once `__has_builtin` answers for them.

## Operand and result types

One argument. The source element type must be an integer, not `_Bool`/`bool` or an enumeration, and exactly the width of the source suffix; wider containers are rejected even when their low bits hold the encoding. Signedness has no effect, since the integer is a bit container rather than a numerically converted value, so `char`, `signed char`, `unsigned char`, and `_BitInt(8)` all work where they are 8 bits.

`__mfp8` is also accepted as a scalar source on targets that have it: it is an opaque 8-bit container with no interpretation of its own, which is exactly what this builtin supplies. Its Neon vector types are rejected with the other target-specific kinds.

Integer promotions and the usual arithmetic conversions do not apply. This keeps the operand exactly matched to the intrinsic and avoids an implicit truncation rule, at an ergonomic cost, since every bit-manipulation expression in C has type `int`:

```c
unsigned char b;
__builtin_elementwise_convert_from_f8e5m2_f32(b >> 1);                  // error: 'int' is not 8 bits
__builtin_elementwise_convert_from_f8e5m2_f32((unsigned char)(b >> 1)); // ok
```

Relaxing this later to accept wider containers would be source-compatible.

A vector result has the same element count, the destination element type, and the same vector kind, either GNU `vector_size` or Clang/OpenCL `ext_vector_type`. Scalable, sizeless, matrix, and target-specific fixed kinds are rejected, because preserving both element count and a target-specific kind across widening can produce an invalid combination (such as a widened NEON vector).

Sema applies normal target and language availability rules to the result type, including target-aware diagnostics for offload code. Dependent C++ calls defer validation to instantiation.

## Conversion semantics

Each element is interpreted as the named format and converted independently. For a caller:

- every source bit pattern produces a defined result, so no operand value is UB or poison;
- conversions are exact for finite values, **with one exception**: `Float8E5M3FNU` has `maxExponent` 16 against `_Float16`'s 15, so its seven largest finite encodings (values 65536 through 114688) exceed `_Float16`'s range and become infinity. `bf16` and `f32` are exact for every encoding;
- `f8e4m3fn` and `f8e5m3fnu` have no infinity encoding, and `f8e5m3fnu` has no sign bit;
- NaN results follow LLVM's general NaN rules, with no promise about sign, quiet/signaling state, or payload;
- no dynamic rounding mode is consulted and there are no floating-environment side effects, so the call may be speculated.

The vector form promises no particular instruction or packing strategy.

## Builtin support queries

`__has_builtin` reports only that Clang knows the spelling, which is what makes the *encoding* queryable, since encodings have no other query mechanism. It says nothing about backend lowering, and nothing about **destination type availability**:

```
avr, msp430, sparc:
  __has_builtin(__builtin_elementwise_convert_from_f8e5m2_bf16)  ->  1
  __bf16 x;   ->  error: __bf16 is not supported on this target
```

For `_Float16` a header can pair the query with `__FLT16_MANT_DIG__`. For `__bf16` Clang defines no availability macro at all, so there is currently no preprocessor guard for a `__bf16` entry point. That gap predates this proposal, but the naming scheme leans on `__has_builtin`, so it should be closed.

## Constant expressions

Not supported initially: `__has_constexpr_builtin` returns zero and use in a constant-expression context is diagnosed, including in a static-storage initializer.

## Alternative: destination as a type argument

```c
float  value  = __builtin_elementwise_convert_from_f8e4m3fn(encoded, float);
float4 packed = __builtin_elementwise_convert_from_f8e4m3fn(encoded4, float4);
```

The encoding stays in the name, so `__has_builtin` remains meaningful for the part that has no other query mechanism, but the destination becomes an ordinary type argument. That is better in several ways: three names instead of nine, and one per new encoding rather than three; the destination is validated by its floating-point semantics rather than a fixed list, so callers can pass a typedef and OpenCL can pass `half` directly; and `f64` becomes an ordinary type-availability question rather than a naming decision.

It was not chosen because of the frontend surface measured under Naming. Reviewers were split: @arsenm preferred the suffix form, @MrSidims does not find suffix overloads ergonomic. This is open question 1.

## Future directions

**Stochastic rounding.** Rounding never arises when widening. It arises when narrowing, and stochastic rounding is not expressible today: `llvm.convert.to.arbitrary.fp` takes its rounding mode as metadata, and metadata cannot carry a seed, which is a runtime value. Hardware exists but only via target intrinsics: AMDGPU's `llvm.amdgcn.cvt.sr.fp8.f32` and siblings take an `i32` seed as an ordinary SSA operand. Supporting it generically needs an IR change, either a seed operand meaningful only in a stochastic mode or a separate intrinsic.

**Scaled conversions.** The OCP microscaling formats pair a block of narrow values with a shared power-of-two scale encoded as `Float8E8M0FNU`, and hardware fuses the scale into the conversion. AMDGPU has 81 such intrinsics, for example:

```llvm
declare <2 x float> @llvm.amdgcn.cvt.scalef32.pk.f32.fp8(
    i32 %src, float %scale, i1 %src_lo_hi_sel)
```

`llvm.convert.from.arbitrary.fp` has no scale operand, so a scaled conversion has to be spelled as a conversion followed by a multiply, leaving any re-fusion to the backend. Adding one raises the same question as the seed above, since a scale is also a runtime value that metadata cannot carry, and it raises a naming question for this proposal specifically: whether a scale becomes an extra argument to these builtins or a parallel family of spellings. The latter would multiply the name count again, which is a reason to settle the naming question now rather than after a scaled family exists.

## Open questions

1. Is encoding the destination in the name the right trade? It keeps these as ordinary calls, at the cost of a spelling per destination type, no typedef or OpenCL `half` destinations, and a `__has_builtin` query that is silent about destination availability. The type-argument alternative is prototyped and measured above; note that neither form requires a new mangling.
2. Should constant evaluation be in the initial patch?
3. Should a future narrowing family be designed around stochastic rounding from the start? It needs a runtime seed operand that metadata cannot carry, so this is also an IR design question.

## References

- [`llvm.convert.from.arbitrary.fp` in the LangRef](https://llvm.org/docs/LangRef.html#llvm-convert-from-arbitrary-fp-intrinsic)
- [Prototype PR #212647](https://github.com/llvm/llvm-project/pull/212647)
- [[RFC] Introducing elementwise clz/ctz builtins](https://discourse.llvm.org/t/rfc-introducing-elementwise-clz-ctz-builtins/85862)
- [[RFC] `__has_builtin` behavior on offloading targets](https://discourse.llvm.org/t/rfc-has-builtin-behavior-on-offloading-targets/84964)
- [SPIR-V representation of OCP low-precision types](https://github.com/KhronosGroup/SPIRV-LLVM-Translator/blob/main/docs/OCPTypesRepresentationInLLVM.rst)
- [HIP low-precision floating-point types](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/low_fp_types.html) · [HIP FP8 header](https://github.com/ROCm/clr/blob/develop/hipamd/include/hip/amd_detail/amd_hip_fp8.h)

