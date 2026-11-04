#pragma once
// ============================================================
// X-Phage numeric typing
//
// Sized numeric types (language extension; the book defines only int/float):
//
//   i8 i16 i32 i64 isize   u8 u16 u32 u64 usize   f32 f64
//
// with the aliases  int = i64,  float = f64,  byte = u8.
//
// Rules
//  * An unsuffixed integer literal takes the type its context expects
//    (declared type, parameter, return type, field, other operand) and must
//    fit it; with no context it is `int`. An unsuffixed float literal is
//    `f32`/`f64` by context, default `float`. A suffix (`255u8`, `1.5f32`,
//    `0xFFu32`) fixes the type. An integer literal may stand where a float is
//    expected (`atom x: f32 = 3`).
//  * Arithmetic, comparison, bitwise operators, assignment, arguments, returns
//    and field initializers require BOTH sides to have the same type once any
//    sized type is involved: there is no implicit conversion between sized
//    types, nor between a sized type and int/float. Use `cast(x, T)`.
//    (Legacy `int` <-> `float` mixing, with no sized type involved, is
//    unchanged.)
//  * Integer arithmetic wraps (two's complement) at the type's width. Shifts
//    mask the count to the width. Unary `-` is rejected on unsigned types.
//  * `cast(x, T)`: int -> int wraps/extends by the SOURCE type's signedness;
//    int -> float rounds to nearest; float -> int truncates toward zero and
//    saturates (NaN -> 0); float -> float rounds.
//
// This pass runs after monomorphization (so every generic is concrete) and
// annotates the AST so no backend has to infer anything:
//   attrs["ty"]    on an expression whose type is a sized numeric type
//   attrs["opty"]  on a BinaryOp/AssignExpr: the type its operands share
//   attrs["var_ty"] on a ForStmt whose loop variable is sized
// An un-annotated declaration of sized type gets its `extra` filled in.
// Type names are also canonicalized in place (i64 -> int, f64 -> float,
// byte -> u8).
// ============================================================
#include "xphage/ast.hpp"
#include "diagnostic.hpp"
#include <vector>

namespace xphage::sema {

// Rewrites every written type name to its canonical spelling in place
// (i64 -> int, f64 -> float, byte -> u8). Run BEFORE semantic analysis so
// bounds, impls and generic inference see one name per type.
void canonicalize_type_names(Program& prog);

std::vector<xphage::diag::Diagnostic> annotate_numeric_types(Program& prog);

} // namespace xphage::sema
