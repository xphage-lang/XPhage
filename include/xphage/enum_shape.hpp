#pragma once
// Shared definition of which enums the `?` operator accepts.
//
// `expr?` returns early from the enclosing pulse, so it only makes sense on
// (and inside a pulse returning) an enum that has a "failure" variant to
// propagate. Two shapes qualify, decided purely by variant structure — never
// by the enum's name — so a user-declared Result/Option (the language defines
// them as ordinary generic enums) and any other enum with the same shape work:
//
//   Result-like: an `Ok(T)` variant and an `Err(E)` variant, one field each
//   Option-like: a `Some(T)` variant (one field) and a fieldless `None`
//
// The semantic analyzer, the C++ backend and the XIL lowering all use this
// one function so they cannot disagree about what `?` accepts. (The LLVM
// backend applies the same rule to its own already-sized EnumInfo.)
#include "ast.hpp"

namespace xphage {

// 1 = Result-like, 2 = Option-like, 0 = neither. `en` must be an EnumDecl.
inline int enum_try_kind(const ASTNode& en) {
    const ASTNode *ok = nullptr, *err = nullptr, *some = nullptr, *none = nullptr;
    for (auto& v : en.children) {
        if (!v || v->kind != NodeKind::EnumVariant) continue;
        if (v->value == "Ok")   ok   = v.get();
        if (v->value == "Err")  err  = v.get();
        if (v->value == "Some") some = v.get();
        if (v->value == "None") none = v.get();
    }
    if (ok && err && ok->children.size() == 1 && err->children.size() == 1) return 1;
    if (some && none && some->children.size() == 1 && none->children.empty()) return 2;
    return 0;
}

} // namespace xphage
