#pragma once
// ============================================================
// X-Phage Semantic Analyzer v1.0.0
//
// A standalone compiler pass, run after parsing and before XIL
// lowering:
//
//   Parser → AST → Semantic Analyzer → Verified AST → XIL Builder → Backend
//
// Responsibility is deliberately narrow and separate from lowering:
// this pass only resolves names, checks types, and validates call
// arity/argument types — it does not build any IR. Keeping these
// concerns apart (rather than checking-while-lowering) is what lets
// Smart Ownership, generics, and Fusion UI-specific checks be added
// later as their own passes over the same Verified AST, without
// entangling them with code generation the way an in-lowering check
// would.
//
// On success, the analyzer returns the same Program it was given
// (the AST itself isn't transformed — "Verified AST" means "AST
// that has passed this pass", not a different data structure). On
// failure, it returns the diagnostics collected, and the caller
// (interface.cpp) should stop before transpilation/lowering, the
// same way a parser error stops before semantic analysis today.
// ============================================================
#include "xphage/ast.hpp"
#include "diagnostic.hpp"
#include "symbol_table.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace xphage::sema {

struct AnalysisResult {
    bool                          ok = true;
    std::vector<diag::Diagnostic> diagnostics; // errors AND warnings together;
                                                 // caller filters by severity
};

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(const std::string& source_text = "");

    // Runs the full analysis over a top-level program (the merged
    // AST that interface.cpp already produces — after stdlib
    // resolution, after `~link` merging, before transpile/lower).
    AnalysisResult analyze(const Program& prog);

private:
    SymbolTable    symtab_;
    std::vector<diag::Diagnostic> diags_;
    std::string    source_text_; // for source-line snippets in diagnostics;
                                  // empty when analyzing a merged multi-file
                                  // program (snippets are best-effort, not
                                  // load-bearing — the file:line:col is the
                                  // part that must always be correct)

    // ── Pass 1: declaration collection ─────────────────────────
    // Walks top-level decls (forge/nexus/pulse/global/atom/extern)
    // and registers them in the symbol table BEFORE checking any
    // function bodies, so forward references and mutual recursion
    // between top-level functions resolve correctly (matching how
    // every real compiler handles top-level declarations — order
    // in the source doesn't matter for visibility).
    void collect_declarations(const Program& prog);
    void collect_one_declaration(const ASTNodePtr& n);

    // ── Pass 2: body checking ──────────────────────────────────
    void check_program(const Program& prog);
    // `self_type` is the receiver type when the body is an impl method:
    // `self` is then in scope (typed as the receiver) whether the method
    // was written with an explicit `self` parameter or — the form the
    // language book uses throughout Chapter 8 — without one.
    void check_pulse_body(const ASTNodePtr& pulse_decl, const std::string& self_type = "");
    void check_stmt(const ASTNodePtr& stmt);
    void check_block(const ASTNodePtr& block);
    // Returns the inferred/declared type string for the expression,
    // or "auto" when it can't be determined (XPhage's lowering
    // already tolerates "auto" throughout, so an inference miss
    // degrades to "don't check this particular subexpression's
    // type" rather than a hard failure).
    std::string check_expr(const ASTNodePtr& expr);

    // ── Specific checks ─────────────────────────────────────────
    void check_identifier(const ASTNodePtr& n);
    void check_call(const ASTNodePtr& n);
    void check_assign(const ASTNodePtr& n);
    void check_binary_op(const ASTNodePtr& n);

    // ── Generics: basic bound validation ────────────────────────
    // type_name implements bound_nexus if an `impl bound_nexus for
    // type_name { ... }` was seen anywhere in the program (recorded
    // in impl_registry_ by a pre-pass — see collect_impls). Returns
    // true (permissively) when bound_nexus is empty (no bound to
    // check) or when type_name itself couldn't be determined (e.g.
    // an argument expression check_expr can't infer a type for) —
    // in both cases there's nothing concrete to validate against, and
    // silently accepting is preferable to a false-positive error on
    // code this pass simply doesn't have enough information about.
    bool satisfies_bound(const std::string& type_name, const std::string& bound_nexus) const;
    // A bound may be several nexus joined by '+' ("Ord+Clone"). Returns
    // the first one `type_name` does not satisfy, or "" when it
    // satisfies all of them.
    std::string first_unsatisfied_bound(const std::string& type_name, const std::string& bound) const;
    bool satisfies_one_bound(const std::string& type_name, const std::string& bound_nexus) const;

    // ── nexus conformance ───────────────────────────────────────
    // `impl N for T` must provide every method of N that has no default
    // body, with the same arity and (with `Self` = T) the same
    // parameter/return types, and may not add methods N does not
    // declare (those belong in an inherent `impl T { }`).
    void check_impl_conformance(const ASTNodePtr& impl);
    // A nexus is a compile-time contract, not a type: it can be a
    // generic bound (`<T: Animal>`) but not a parameter/field/variable
    // type (that would need dynamic dispatch, which is not supported).
    void check_not_nexus_type(const std::string& type_str, const Span& span, const char* what);
    // Every name in a type parameter's bound must be a declared nexus
    // (or the built-in `Numeric`).
    void check_bound_names(const ASTNodePtr& type_param);
    std::unordered_map<std::string, const ASTNode*> nexus_nodes_; // nexus name -> its NexusDecl
    std::vector<std::string> realm_stack_;                        // collect_impls: enclosing realms
    // ── associated (receiver-less) functions: `Type::f()`, `T::f()` ──
    std::unordered_map<std::string, std::unordered_set<std::string>> assoc_fns_; // bare receiver type -> fn names
    std::unordered_set<std::string> forge_names_;                                 // bare forge names
    std::unordered_set<std::string> realm_names_;
    std::unordered_map<std::string, std::string> fn_type_params_;                 // type param in scope -> bound ("A+B")
    void check_assoc_path_call(const ASTNodePtr& call);
    // Walks the whole program once, before any call-site checking,
    // recording every `impl Nexus for Type { ... }` into
    // impl_registry_.
    void collect_impls(const ASTNodePtr& n);
    std::unordered_map<std::string, std::unordered_set<std::string>> impl_registry_; // nexus -> {types implementing it}

    // ── `?` (error propagation) validation ───────────────────────
    // `expr?` returns early from the ENCLOSING pulse, so it is only
    // meaningful when that pulse itself returns a Result-like or
    // Option-like enum, and only applies to a value of such a type. An
    // enum is "Result-like" when it has `Ok(T)` and `Err(E)` variants and
    // "Option-like" when it has `Some(T)` and a fieldless `None` —
    // decided by shape, not by the enum's name, so a user-declared
    // Result/Option (the language defines them as ordinary generic
    // enums) and any enum with the same shape all qualify.
    struct TryShape {
        enum Kind { NotTry, ResultLike, OptionLike } kind = NotTry;
        std::string enum_name;
        std::string ok_type;    // payload of Ok / Some, type args substituted
        std::string fail_type;  // payload of Err (empty for Option-like)
    };
    TryShape try_shape_of(const std::string& type_str) const;
    void check_propagate(const ASTNodePtr& expr);
    void check_fstring_propagate(const ASTNodePtr& expr);
    std::unordered_map<std::string, const ASTNode*> enum_decl_nodes_; // enum name -> its EnumDecl
    std::string                     current_fn_name_;      // empty outside any pulse
    std::string                     current_fn_ret_;       // declared return type; empty = returns nothing
    std::vector<std::string>        current_fn_tparams_;   // enclosing pulse's own type parameters

    // ── Diagnostic helpers ──────────────────────────────────────
    void error(const std::string& code, const Span& span, const std::string& msg,
               uint32_t underline_len = 1,
               std::optional<std::string> help = std::nullopt);
    void warning(const std::string& code, const Span& span, const std::string& msg,
                 uint32_t underline_len = 1,
                 std::optional<std::string> help = std::nullopt);

    // Builtin/runtime function table: names the analyzer should
    // treat as always-resolved, since their real definitions are
    // injected by the transpiler/runtime rather than declared in
    // user XPhage source. Populated from the same name lists the
    // transpiler itself uses (math/string/io/collections runtime,
    // core_ops.cpp), so the two stay in sync by construction rather
    // than by two people remembering to update both places.
    void seed_builtins();
};

} // namespace xphage::sema
