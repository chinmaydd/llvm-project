// RUN: c-index-test -test-load-source all %s | FileCheck %s

typedef float dst_t;

float convert(unsigned char bits) {
  return __builtin_elementwise_convert_from_f8e5m2(bits, dst_t);
}

// CHECK: elementwise-convert-from-arbitrary-fp.c:3:15: TypedefDecl=dst_t:3:15 (Definition)
// CHECK: elementwise-convert-from-arbitrary-fp.c:6:10: UnexposedExpr=
// CHECK-NEXT: elementwise-convert-from-arbitrary-fp.c:6:58: TypeRef=dst_t:3:15
// CHECK-NEXT: elementwise-convert-from-arbitrary-fp.c:6:52: UnexposedExpr=bits:5:29
// CHECK-NEXT: elementwise-convert-from-arbitrary-fp.c:6:52: DeclRefExpr=bits:5:29
