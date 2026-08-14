# [RFC][Clang] Elementwise builtins for converting encoded FP8 values

Implemented and passing `check-clang`; two alternatives were also prototyped and are described below.

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

Two source encodings (`f8e5m2`, `f8e4m3fn`) and three destinations (`f16` → `_Float16`, `bf16` → `__bf16`, `f32` → `float`) give six builtins. Because the destination is in the name, these stay ordinary `CallExpr`s: no keyword, parser production, expression node, serialization record, or mangling rule.

## Prior review

[PR #212647](https://github.com/llvm/llvm-project/pull/212647) proposed an earlier form taking a format string and a destination type. This RFC answers that feedback:

* @shiltian and @MrSidims objected to selecting semantics with a string operand — the string is gone.
* @arsenm asked for an enum or a type suffix, preferring the suffix so the exact spelling is `__has_builtin`-queryable, and noted it must include the return type. This is that form. He also asked for elementwise vector support, adopted here.
* @MrSidims does not find suffix overloads ergonomic either, though he reported the equivalent SPIR-V builtins were straightforward to implement and use. Open question 1 puts this to the list.
* @AaronBallman asked for an RFC covering constant expressions, language availability, and the motivating use case — all below.
* @MrSidims asked about stochastic rounding, which the current intrinsics cannot express — see Future directions.

## Motivation

Low-precision formats are commonly stored as integers, because C and C++ have no portable arithmetic types for these encodings. Libraries must still convert them to `_Float16`, `__bf16`, and `float`.

HIP is the initial consumer. Its FP8 headers use AMDGPU-specific builtins and must pick a scalar or packed instruction shape at the source level:

```c++
__builtin_amdgcn_cvt_f32_fp8((int)bits, 0);        // instruction-shaped word + byte index
__builtin_amdgcn_cvt_pk_f32_fp8((int)bits, false); // a different builtin for two values
```

So the header maintains separate target-specific and software paths instead of expressing one elementwise conversion. The proposed builtin expresses only the semantics and accepts a vector with the same spelling, leaving scalar-or-packed selection to the compiler. Packed selection is retained today: on gfx950, gfx1170, and gfx1250 the `<2 x i8>` and `<4 x i8>` FP8-to-f32 forms select one and two `v_cvt_pk_f32_fp8_e32` instructions respectively.

This does not claim SPIR-V support or universal cross-target codegen. Target-specific builtins remain appropriate where an API intentionally exposes byte selection or another ISA-specific operation.

## Existing LLVM support

```llvm
declare <fNxM> @llvm.convert.from.arbitrary.fp.<fNxM>.<iNxM>(
    <iNxM> %value, metadata %interpretation)
```

The integer holds the encoding; the metadata names its interpretation. Generic SelectionDAG expansion supports five source formats:

| Source suffix | Interpretation | Width | Proposal |
| --- | --- | ---: | --- |
| `f8e5m2` | `Float8E5M2` | 8 | Exposed |
| `f8e4m3fn` | `Float8E4M3FN` | 8 | Exposed |
| `f6e3m2fn` | `Float6E3M2FN` | 6 | Deferred |
| `f6e2m3fn` | `Float6E2M3FN` | 6 | Deferred |
| `f4e2m1fn` | `Float4E2M1FN` | 4 | Deferred |

Lowering exists **only in SelectionDAG**. GlobalISel has none: `-global-isel` fails with `LLVM ERROR: unable to translate instruction` from `IRTranslator` rather than falling back. AMDGPU `clang -O0` still uses SelectionDAG, so this is off the default path, but it is a hard error, not a missing optimization.

LLVM also has the inverse `llvm.convert.to.arbitrary.fp`, which additionally takes a rounding mode and a saturation flag. No builtin is proposed for it here, but its shape bears on the naming discussion.

## Prerequisites

Two LLVM-side changes, proposed as separate patches:

1. **AMDGPU f16 gating.** `SITargetLowering::LowerCONVERT_FROM_ARBITRARY_FP` routes an `f16` destination to `lowerFromFP8` unconditionally; it must be gated on `hasFP8F16ConversionInsts()` so other targets fall back to generic expansion. Required before `_f16` is usable across AMDGPU targets.
2. **LangRef NaN clarification.** LangRef says the NaN representation is preserved (quiet stays quiet, signaling stays signaling). The generic expansion does not implement that, and this proposal's NaN wording assumes the weaker contract. Relaxing a documented guarantee deserves its own review.

## Scope

The initial proposal does not: change LLVM IR; expose the sub-byte encodings, other source formats, or an `f64` destination; add non-SelectionDAG codegen; convert native values to narrow encodings; define rounding controls; support scalable or target-specific vector kinds; or support constant evaluation.

## Naming

The source precedes the destination, mirroring the intrinsic. `elementwise` is used in its established sense of accepting a scalar or fixed-length vector and applying per element; unlike the rest of that family this one is not type preserving, because the source encoding has no C type that could also be the result type. Suffix meanings are uniform in every language mode — `f16` is `_Float16`, not `__fp16` or the OpenCL `half`.

Encoding both types in the name keeps these ordinary calls. A destination given as a *type* argument cannot be an ordinary call argument, so it forces a keyword, a parser production, and a dedicated expression node, pulling in dependence computation, printing, profiling, `TreeTransform`, `ASTImporter`, serialization, an Itanium mangling, and AST-consumer entries. Both forms were prototyped, so this is measured rather than asserted:

| | Non-test files | Non-test lines |
| --- | ---: | ---: |
| Destination as a type argument | 40, of which 19 are AST plumbing | ~890 |
| Destination in the name (proposed) | 6 | ~240 |

The mangling is the part that does not wash out: a new node needs an Itanium vendor-extension mangling that becomes ABI once shipped, embedding the LLVM-internal interpretation string in mangled names.

**The cost, plainly.** The multiplier is the number of destination types, applied to the widening half of the family. Today that is 2 × 3 = 6. If every recognized interpretation were eventually exposed with an `f64` destination it would be 10 × 4 = 40. A narrowing family costs one name per encoding under any scheme, since its source type is deduced and its rounding and saturation operands must be constant arguments regardless. Open question 1 asks whether the trade is right.

## Operand and result types

One argument. The source element type must be an integer, not `_Bool`/`bool` or an enumeration, and exactly the width of the source suffix; wider containers are rejected even when their low bits hold the encoding. Signedness has no effect — the integer is a bit container, not a numerically converted value — so `char`, `signed char`, `unsigned char`, and `_BitInt(8)` all work where they are 8 bits.

`__mfp8` is also accepted as a scalar source on targets that have it: it is an opaque 8-bit container with no interpretation of its own, which is exactly what this builtin supplies. Its Neon vector types are rejected with the other target-specific kinds.

Integer promotions and the usual arithmetic conversions do **not** apply. This keeps the operand exactly matched to the intrinsic and avoids an implicit truncation rule, but it costs ergonomics, since every bit-manipulation expression in C has type `int`:

```c
unsigned char b;
__builtin_elementwise_convert_from_f8e5m2_f32(b >> 1);                  // error: 'int' is not 8 bits
__builtin_elementwise_convert_from_f8e5m2_f32((unsigned char)(b >> 1)); // ok
```

A vector result has the same element count, the destination element type, and the same vector kind — GNU `vector_size` or Clang/OpenCL `ext_vector_type`. Scalable, sizeless, matrix, and target-specific fixed kinds are rejected, because preserving both element count and a target-specific kind across widening can produce an invalid combination such as a widened NEON vector. Returning a generic vector instead would also be defensible; starting strict is source-compatible with relaxing later.

Sema applies normal target and language availability rules to the result type, including target-aware diagnostics for offload code. Dependent C++ calls defer validation to instantiation.

## Conversion semantics

Each element is interpreted as the named format and converted independently. For a caller:

* every source bit pattern produces a defined result — no operand value is UB or poison;
* all supported combinations are exact widening conversions for finite values;
* `f8e4m3fn` has no infinity encoding;
* NaN results follow LLVM's general NaN rules, with no promise about sign, quiet/signaling state, or payload (see prerequisite 2);
* no dynamic rounding mode is consulted and there are no floating-environment side effects, so the call may be speculated.

The vector form promises no particular instruction or packing strategy.

## Builtin support queries

`__has_builtin` reports only that Clang knows the spelling, which is what makes the *encoding* queryable — encodings have no other query mechanism. It says nothing about backend lowering, and nothing about **destination type availability**, which is a sharp edge worth stating up front:

```
avr, msp430, sparc:
  __has_builtin(__builtin_elementwise_convert_from_f8e5m2_bf16)  ->  1
  __bf16 x;   ->  error: __bf16 is not supported on this target
```

For `_Float16` a header can pair the query with `__FLT16_MANT_DIG__`. For `__bf16` **Clang defines no availability macro at all**, so there is currently no preprocessor guard for a `__bf16` entry point. That is a pre-existing gap, but this naming scheme leans on `__has_builtin`, so it should be closed — a `__BF16__`-style predefined macro is proposed as a follow-up.

## Constant expressions

Not supported initially: `__has_constexpr_builtin` returns zero and use in a constant-expression context is diagnosed, including in a static-storage initializer.

`__builtin_convertvector` and most `__builtin_elementwise_*` builtins are constant evaluable, and the evaluation here is small — `APFloat` has every semantic involved and the conversions are exact. In this form it is an ordinary builtin case in the classic evaluator and the bytecode interpreter rather than a new visitor in each. Deferring does not affect the spelling or type rules, but it does change the answer to `__has_constexpr_builtin` after shipping, hence open question 2.

## Implementation

```llvm
%result = call float @llvm.convert.from.arbitrary.fp.f32.i8(
    i8 %bits, metadata !"Float8E4M3FN")
```

A TableGen multiclass defines the six spellings with `NoThrow`, `Const`, and `CustomTypeChecking`. Exposed encodings live in `clang/include/clang/Basic/ArbitraryFPFormats.def`, which Sema and CodeGen expand to recover the interpretation from the builtin ID; because the expansion names builtin IDs directly, a stale entry fails to compile. Tests cover spelling queries, the absence of deferred encodings, result types and emission for all six, every diagnostic, the promotion cast requirement, GNU and extended vectors, `__mfp8`, dependent C++ templates, and C/C++/OpenCL — including `half` interoperation — plus per-target availability and an OpenMP device case.

## Alternatives considered

**Destination as a type argument** — `__builtin_elementwise_convert_from_f8e4m3fn(encoded, float)`. Also prototyped, and genuinely better in several ways: two names instead of six and one per new encoding rather than three; the destination is validated by its floating-point semantics rather than a fixed list, so callers can pass a typedef and OpenCL can pass `half`; `f64` becomes an ordinary type-availability question; and the encoding stays in the name, so `__has_builtin` remains meaningful for the part that has no other query. Rejected on the frontend and ABI cost measured above. Reviewers were split — @arsenm preferred the suffix form, @MrSidims does not find suffix overloads ergonomic. Open question 1.

**An f32-only family** — every exposed conversion is exact into every destination (`Float8E5M2` is 1-5-2, `Float8E4M3FN` is 1-4-3, both strictly inside `_Float16`, `__bf16`, and `float`), so `float` is a lossless intermediate and `(_Float16)__builtin_..._f32(x)` cannot double-round. Two names, no destination multiplier, at the cost of relying on a `fptrunc`-of-intrinsic peephole for native f16 selection.

**String plus destination type** — the original prototype. Prevented any per-encoding `__has_builtin` query, drew pushback on string-selected semantics, and paid the same custom-AST cost.

**Enum-selected source and destination** — keeps an ordinary `CallExpr` and avoids name growth, but makes the result type depend on an argument value, and an enum needs a separate way to query accepted enumerators.

**Wider integer containers** — would remove the cast shown above and give sub-byte encodings a byte-lane representation, but the exact-width contract matches the intrinsic type and avoids an implicit truncation rule. The natural fallback if the promotion ergonomics prove unacceptable.

**First-class narrow types** — broad language and ABI effects. Note Clang already has one, `__mfp8`, which this accepts as a container rather than duplicating.

## Future directions

**Sub-byte encodings.** FP6 cannot be spelled at all — Clang has no 6-bit vector element type. FP4 can be spelled scalar as `_BitInt(4)`, and `_BitInt(4)` vectors are permitted since 4 is a power of two, but their layout is incoherent today: for `<8 x _BitInt(4)>` Clang reports `sizeof` 8 while the emitted `<8 x i4>` occupies 4 bytes, and the x86-64 ABI then coerces the argument to `double`. Exposing an FP4 vector API on that would bake in a layout matching neither Clang's own `sizeof` nor the packed nibbles hardware uses. Deferred until `_BitInt` vector layout is settled, or a packed integer container is chosen for sub-byte encodings.

**f64 destination.** Supported by IR; deferred because no consumer has been identified and Clang's `double` is not uniformly IEEE binary64 across targets.

**Additional source formats.** The verifier recognizes seven more names that `getArbitraryFPSemantics` does not admit to codegen (`Float8E5M2FNUZ`, `Float8E4M3`, `Float8E4M3FNUZ`, `Float8E4M3B11FNUZ`, `Float8E3M4`, `Float8E8M0FNU`, `Float8E5M3FNU`). Separately, LangRef omits `Float8E5M3FNU` from its list even though the verifier accepts it — worth fixing on its own.

**Rounding and stochastic rounding.** Rounding never arises when widening, since every supported conversion is exact. It arises when narrowing, and stochastic rounding is not expressible today: `llvm.convert.to.arbitrary.fp` takes its rounding mode as metadata, and metadata cannot carry a seed, which is a runtime value. Hardware exists but only via target intrinsics — AMDGPU's `llvm.amdgcn.cvt.sr.fp8.f32` and siblings take an `i32` seed as an ordinary SSA operand. Supporting it generically needs an IR change: a seed operand meaningful only in a stochastic mode, or a separate intrinsic. That belongs with the narrowing family, but the answer should inform its operand list before it is designed.

**The narrowing direction.** It would name the destination encoding and deduce its source type, so it costs one spelling per encoding under any naming scheme. The asymmetry this proposal accepts is therefore not about name growth but about which direction has to name a type at all: only widening does, because only widening has a result type that cannot be deduced.

## Rollout

1. Post the two prerequisite patches, then the Clang implementation.
2. Adopt in HIP headers, guarded by `__has_builtin` plus the destination type's availability check, retaining FNUZ and older-compiler fallbacks.
3. Reconsider deferred encodings given a consumer, a coherent source representation, and codegen support.
4. Consider constant evaluation, GlobalISel support, and the narrowing family separately.

## Open questions

1. Is encoding the destination in the name the right trade? It keeps these as ordinary calls with no new mangling, at the cost of a spelling per destination type, no typedef or OpenCL `half` destinations, and a `__has_builtin` query that is silent about destination availability. The type-argument alternative is prototyped and measured above.
2. Should constant evaluation be in the initial patch?
3. Should a future narrowing family be designed around stochastic rounding from the start? It needs a runtime seed operand that metadata cannot carry, so this is also an IR design question.

## References

* [`llvm.convert.from.arbitrary.fp` in the LangRef](https://llvm.org/docs/LangRef.html#llvm-convert-from-arbitrary-fp-intrinsic)
* [Prototype PR #212647](https://github.com/llvm/llvm-project/pull/212647) · [AMDGPU lowering PR #194144](https://github.com/llvm/llvm-project/pull/194144) · [APFloat UE5M3 PR #210720](https://github.com/llvm/llvm-project/pull/210720)
* [[RFC] Introducing elementwise clz/ctz builtins](https://discourse.llvm.org/t/rfc-introducing-elementwise-clz-ctz-builtins/85862)
* [[RFC] `__has_builtin` behavior on offloading targets](https://discourse.llvm.org/t/rfc-has-builtin-behavior-on-offloading-targets/84964)
* [SPIR-V representation of OCP low-precision types](https://github.com/KhronosGroup/SPIRV-LLVM-Translator/blob/main/docs/OCPTypesRepresentationInLLVM.rst)
* [HIP low-precision floating-point types](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/low_fp_types.html) · [HIP FP8 header](https://github.com/ROCm/clr/blob/develop/hipamd/include/hip/amd_detail/amd_hip_fp8.h)
