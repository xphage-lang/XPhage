#pragma once
// ============================================================
// X-Phage trait expansion — desugaring for `nexus` / `impl`
//
// A backend-independent AST -> AST pass, run on the fully merged
// program (after `~link` stdlib resolution and tri-modular merging,
// before semantic analysis), so the semantic analyzer and every
// backend (AST transpiler, XIL, LLVM) see exactly the same, already
// expanded program and none of them needs trait-specific logic:
//
//  1. Default methods (language book §8.4).  For every
//     `impl N for T { ... }`, each method of `nexus N` that has a body
//     and is not defined by the impl is cloned into the impl.  The
//     clone is then an ordinary impl method, so it is statically
//     dispatched on T and can call the impl's required methods through
//     `self`.
//
//  2. `Self`.  Inside an impl, the type name `Self` (in parameter,
//     return, local-declaration and `spawn` positions) is replaced by
//     the impl's receiver type.  Inside the `nexus` declaration itself
//     `Self` stays symbolic: the semantic analyzer substitutes the
//     impl's receiver type when it checks an impl against its nexus.
//
// The pass is idempotent.
// ============================================================
#include "xphage/ast.hpp"

namespace xphage::sema {

void expand_traits(Program& prog);

// Replaces every whole-identifier `Self` in a type string with `recv`
// ("Vec<Self>" -> "Vec<Point>"). Shared with the semantic analyzer,
// which applies it to nexus signatures when checking an impl.
std::string replace_self_type(const std::string& type, const std::string& recv);

// Run AFTER monomorphization: hoists every associated (receiver-less) function
// out of its impl into a free function `<Type>__<fn>` and rewrites
// `Type::fn(...)` calls to it. See trait_expand.cpp.
void lower_assoc_functions(Program& prog);

} // namespace xphage::sema
