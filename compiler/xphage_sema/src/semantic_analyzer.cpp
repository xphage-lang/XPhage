#include <cctype>
#include "../include/trait_expand.hpp"
#include "../include/semantic_analyzer.hpp"
#include <sstream>
#include <unordered_set>

namespace xphage::sema {

using diag::Diagnostic;
using diag::Severity;

SemanticAnalyzer::SemanticAnalyzer(const std::string& source_text)
    : source_text_(source_text) {
    seed_builtins();
}

// ── Builtin seeding ──────────────────────────────────────────────
// Mirrors the function name lists the transpiler's runtime blocks
// provide (see transpiler.cpp's math_runtime_names() /
// string_runtime_names() / io_runtime_names() /
// collections_runtime_names()) plus core_ops.cpp's always-available
// helpers. Kept as a static literal list here rather than sharing a
// single source of truth across two compiler modules, since
// xphage_sema doesn't currently depend on xphage_codegen_transpiler
// (and shouldn't start to, to keep the pass ordering clean — sema
// runs before any backend is even chosen). If the transpiler's
// runtime function set changes, this list needs a matching update;
// it intentionally only includes the names, not their full
// signatures, since arity checking against a stdlib function uses
// the same general call-checking logic as a user function once the
// symbol is registered.
void SemanticAnalyzer::seed_builtins() {
    static const char* math_names[] = {
        "sqrt", "cbrt", "pow", "exp", "exp2", "log", "log2", "log10",
        "abs", "sign", "floor", "ceil", "round", "trunc", "fract",
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
        "sinh", "cosh", "tanh", "to_rad", "to_deg",
        "min", "max", "clamp", "minf", "maxf", "clampf", "gcd", "lcm",
        "lerp", "lerp_clamp", "smoothstep", "smootherstep", "map_range",
        "bezier3", "bezier4",
        "rand", "rand_int", "rand_float", "rand_bool", "rand_gaussian", "rand_seed",
        "bit_count", "bit_leading_zeros", "bit_trailing_zeros",
        "is_power_of_two", "next_power_of_two",
        // not yet implemented but signature-declared by math.xh —
        // still registered so calling them is a link error, not an
        // unknown-identifier sema error (the distinction matters:
        // "this exists but isn't implemented yet" vs "this was
        // never a real function" are different problems)
        "vec2_add", "vec2_sub", "vec2_mul", "vec2_dot", "vec2_length",
        "vec3_add", "vec3_sub", "vec3_cross", "mean", "median", "stddev", "variance",
    };
    static const char* string_names[] = {
        "str_contains", "str_find", "str_find_last", "str_count",
        "str_starts_with", "str_ends_with", "str_is_empty", "str_is_blank",
        "str_upper", "str_lower", "str_title", "str_trim", "str_trim_left",
        "str_trim_right", "str_trim_chars", "str_reverse", "str_repeat",
        "str_slice", "str_left", "str_right", "str_char_at", "str_len",
        "str_split", "str_split_lines", "str_split_words", "str_join",
        "str_replace", "str_replace_first", "str_remove",
        "str_pad_left", "str_pad_right", "str_center",
        "str_to_int", "str_to_float", "str_to_bool", "int_to_str",
        "float_to_str", "bool_to_str", "str_to_hex", "hex_to_str",
        "str_matches", "str_regex_find", "str_regex_find_all",
        "str_regex_replace", "str_regex_split",
        "str_format_int", "str_format_float", "str_format_hex", "str_format_bin",
        "str_char_count", "str_is_ascii", "str_is_utf8",
    };
    static const char* io_names[] = {
        "io_read", "io_write", "io_append", "io_exists", "io_delete",
        "io_mkdir", "io_mkdir_all", "io_copy", "io_move", "io_size",
        "io_list_dir", "io_is_dir", "io_is_file",
        "print", "println", "eprint", "eprintln", "input",
        "path_join", "path_join3", "path_basename", "path_dirname",
        "path_extension", "path_stem", "path_absolute", "path_normalize",
        "env_get", "env_set", "env_has",
        "io_read_lines", "io_read_bytes", "io_write_bytes",
        "io_temp_file", "io_temp_dir",
    };
    static const char* collections_names[] = {
        "vec_new", "vec_push", "vec_get", "vec_set", "vec_size", "vec_len",
        "vec_is_empty", "vec_first", "vec_last", "vec_remove", "vec_remove_item",
        "vec_insert", "vec_contains", "vec_index_of", "vec_reverse", "vec_sort",
        "vec_unique", "vec_slice", "vec_concat", "vec_join", "vec_zip",
        "vec_enumerate", "vec_sum", "vec_min", "vec_max", "vec_avg", "vec_from_str",
        "map_new", "map_set", "map_get", "map_get_or", "map_has", "map_remove",
        "map_keys", "map_values", "map_size", "map_is_empty", "map_merge", "map_from_pairs",
        "set_new", "set_add", "set_remove", "set_contains", "set_size",
        "set_union", "set_intersect", "set_diff", "set_to_vec", "set_from_vec",
        "queue_new", "queue_push", "queue_pop", "queue_peek", "queue_size", "queue_is_empty",
        "stack_new", "stack_push", "stack_pop", "stack_peek", "stack_size", "stack_is_empty",
        "range", "range_step", "range_float",
        "zip", "enumerate", "take", "drop", "chunk",
        "vec_map", "vec_filter", "vec_reduce", "vec_each", "vec_sort_by",
        "option_map", "result_map", "flatten", "partition", "take_while", "drop_while",
    };
    static const char* core_names[] = {
        // Always available — core_ops.cpp / runtime.hpp, no ~link required
        "panic",
        "str_trim", "str_upper", "str_lower", "str_len", "str_replace",
        "str_contains", "str_starts_with", "str_ends_with",
        "str_to_int", "str_to_float", "int_to_str", "float_to_str",
    };

    auto seed_all = [&](const char* const* names, size_t count) {
        for (size_t i = 0; i < count; i++) {
            Symbol sym;
            sym.name = names[i];
            sym.kind = SymbolKind::Function;
            sym.type = "auto";
            sym.is_used = true; // builtins are never flagged unused
            symtab_.declare(sym);
        }
    };
    seed_all(math_names, sizeof(math_names)/sizeof(math_names[0]));
    seed_all(string_names, sizeof(string_names)/sizeof(string_names[0]));
    seed_all(io_names, sizeof(io_names)/sizeof(io_names[0]));
    seed_all(collections_names, sizeof(collections_names)/sizeof(collections_names[0]));
    seed_all(core_names, sizeof(core_names)/sizeof(core_names[0]));
}

// ── Diagnostic helpers ────────────────────────────────────────────
void SemanticAnalyzer::error(const std::string& code, const Span& span,
                              const std::string& msg, uint32_t underline_len,
                              std::optional<std::string> help) {
    Diagnostic d;
    d.code = code;
    d.severity = Severity::Error;
    d.message = msg;
    d.span = span;
    d.underline_len = underline_len;
    d.help = help;
    diags_.push_back(std::move(d));
}

void SemanticAnalyzer::warning(const std::string& code, const Span& span,
                                const std::string& msg, uint32_t underline_len,
                                std::optional<std::string> help) {
    Diagnostic d;
    d.code = code;
    d.severity = Severity::Warning;
    d.message = msg;
    d.span = span;
    d.underline_len = underline_len;
    d.help = help;
    diags_.push_back(std::move(d));
}

// ── Top-level entry point ─────────────────────────────────────────
AnalysisResult SemanticAnalyzer::analyze(const Program& prog) {
    diags_.clear();
    enum_decl_nodes_.clear();
    current_fn_name_.clear();
    current_fn_ret_.clear();
    current_fn_tparams_.clear();

    for (auto& n : prog) collect_impls(n);
    collect_declarations(prog);
    check_program(prog);

    AnalysisResult result;
    result.diagnostics = diags_;
    for (auto& d : diags_) {
        if (d.severity == Severity::Error) { result.ok = false; break; }
    }
    return result;
}

// Pre-pass (before declaration collection, so bound-checking has
// this data available from the very first call site it needs to
// validate): record every `impl Nexus for Type { ... }` into
// impl_registry_. Walks into realms too, since an impl can be
// declared inside one. Deliberately does NOT try to disambiguate
// `impl Type { ... }` (no nexus — an inherent impl, which the parser
// represents identically except with an empty `.extra`, see
// parse_impl_decl's own comment on this ambiguity) — an empty
// extra means nothing to register, not an error.
void SemanticAnalyzer::collect_impls(const ASTNodePtr& n) {
    if (!n) return;
    if (n->kind == NodeKind::ImplDecl && !n->value.empty() && !n->extra.empty()) {
        impl_registry_[n->value].insert(n->extra);
    }
    if (n->kind == NodeKind::ForgeDecl && !n->value.empty()) forge_names_.insert(n->value);
    if (n->kind == NodeKind::RealmDecl && !n->value.empty()) realm_names_.insert(n->value);
    if (n->kind == NodeKind::ImplDecl && !n->extra.empty()) {
        // bare receiver name: `Zoo::Lion` -> `Lion`, `Box<T>` -> `Box`
        std::string recv = n->extra.substr(0, n->extra.find('<'));
        size_t cc = recv.rfind("::");
        if (cc != std::string::npos) recv = recv.substr(cc + 2);
        for (auto& m : n->children) {
            if (!m || m->kind != NodeKind::PulseDecl || m->value.empty()) continue;
            bool has_self = false;
            for (auto& c : m->children)
                if (c && c->kind == NodeKind::FieldDecl && c->value == "self") has_self = true;
            if (!has_self) assoc_fns_[recv].insert(m->value);
        }
    }
    if (n->kind == NodeKind::NexusDecl && !n->value.empty()) {
        nexus_nodes_[n->value] = n.get();
        if (!realm_stack_.empty()) {
            std::string q;
            for (auto& r : realm_stack_) q += r + "::";
            nexus_nodes_[q + n->value] = n.get();
        }
    }
    const bool is_realm = n->kind == NodeKind::RealmDecl;
    if (is_realm) realm_stack_.push_back(n->value);
    for (auto& c : n->children) collect_impls(c);
    if (is_realm) realm_stack_.pop_back();
}

std::string SemanticAnalyzer::first_unsatisfied_bound(const std::string& type_name,
                                                       const std::string& bound) const {
    size_t start = 0;
    while (start <= bound.size()) {
        size_t plus = bound.find('+', start);
        std::string one = bound.substr(start, plus == std::string::npos ? std::string::npos
                                                                         : plus - start);
        if (!one.empty() && !satisfies_one_bound(type_name, one)) return one;
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return "";
}

bool SemanticAnalyzer::satisfies_bound(const std::string& type_name,
                                        const std::string& bound) const {
    return first_unsatisfied_bound(type_name, bound).empty();
}

bool SemanticAnalyzer::satisfies_one_bound(const std::string& type_name,
                                            const std::string& bound_nexus) const {
    if (bound_nexus.empty()) return true;   // no bound to check
    if (type_name.empty() || type_name == "auto") return true; // couldn't infer — don't false-positive
    // XPhage's built-in scalar types are treated as satisfying any
    // bound named exactly "Numeric" — there's no `impl Numeric for
    // int` anywhere to find via impl_registry_ (int/float aren't
    // forge types), but a generic numeric function is the single
    // most common reason to write a bound at all, and a strict
    // registry-only check would make `pulse add<T: Numeric>(a: T, b:
    // T) -> T` unusable with XPhage's own primitive numeric types —
    // clearly not the intended meaning of the bound.
    if (bound_nexus == "Numeric" &&
        (type_name == "int" || type_name == "float" || type_name == "i8" || type_name == "i16" ||
         type_name == "i32" || type_name == "isize" || type_name == "u8" || type_name == "u16" ||
         type_name == "u32" || type_name == "u64" || type_name == "usize" || type_name == "f32")) {
        return true;
    }
    auto it = impl_registry_.find(bound_nexus);
    if (it == impl_registry_.end()) return false; // bound names a nexus nothing implements
    return it->second.count(type_name) > 0;
}

// ── Pass 1: declaration collection ────────────────────────────────
void SemanticAnalyzer::collect_declarations(const Program& prog) {
    for (auto& n : prog) collect_one_declaration(n);
}

void SemanticAnalyzer::collect_one_declaration(const ASTNodePtr& n) {
    if (!n) return;

    switch (n->kind) {
        case NodeKind::PulseDecl:
        case NodeKind::AsyncPulseDecl: {
            if (n->value.empty()) break; // anonymous pulse — nothing to register
            if (auto* existing = symtab_.lookup_local(n->value)) {
                // A builtin placeholder (seeded by seed_builtins(),
                // identifiable by its line-0 declared_at — no real
                // source location ever has line 0) is not a genuine
                // prior declaration; a real `pulse sqrt(...)` in
                // math.xh providing the actual implementation should
                // silently take over the symbol table entry, not be
                // flagged as redefining something.
                bool existing_is_builtin = (existing->declared_at.line == 0 &&
                                             existing->declared_at.file.empty());
                if (existing->kind == SymbolKind::Function && !existing_is_builtin) {
                    Diagnostic d;
                    d.code = "XP2010";
                    d.severity = Severity::Error;
                    d.message = "the function `" + n->value + "` is defined multiple times";
                    d.span = n->span;
                    d.underline_len = (uint32_t)n->value.size();
                    d.has_secondary = true;
                    d.secondary_span = existing->declared_at;
                    d.secondary_label = "previous definition of `" + n->value + "` here";
                    diags_.push_back(std::move(d));
                    break;
                }
            }
            Symbol sym;
            sym.name = n->value;
            sym.kind = SymbolKind::Function;
            sym.type = n->extra2.empty() ? "void" : n->extra2;
            sym.declared_at = n->span;
            FunctionSig sig;
            sig.return_type = sym.type;
            sig.is_async = (n->kind == NodeKind::AsyncPulseDecl);
            for (auto& c : n->children) {
                if (c && c->kind == NodeKind::TypeParamDecl) {
                    sig.type_params.push_back({c->value, c->extra});
                } else if (c && c->kind == NodeKind::FieldDecl) {
                    sig.param_types.push_back(c->extra.empty() ? "auto" : c->extra);
                }
            }
            sym.fn_sig = std::move(sig);
            symtab_.declare(sym);
            break;
        }

        case NodeKind::ForgeDecl:
        case NodeKind::NexusDecl: {
            if (n->value.empty()) break;
            if (auto* existing = symtab_.lookup_local(n->value)) {
                Diagnostic d;
                d.code = "XP2011";
                d.severity = Severity::Error;
                d.message = "the type `" + n->value + "` is defined multiple times";
                d.span = n->span;
                d.underline_len = (uint32_t)n->value.size();
                d.has_secondary = true;
                d.secondary_span = existing->declared_at;
                d.secondary_label = "previous definition of `" + n->value + "` here";
                diags_.push_back(std::move(d));
                break;
            }
            Symbol sym;
            sym.name = n->value;
            sym.kind = SymbolKind::Type;
            sym.declared_at = n->span;
            for (auto& c : n->children) {
                if (c && c->kind == NodeKind::TypeParamDecl) {
                    sym.type_params.push_back({c->value, c->extra});
                }
            }
            symtab_.declare(sym);
            break;
        }

        case NodeKind::EnumDecl: {
            if (n->value.empty()) break;
            if (auto* existing = symtab_.lookup_local(n->value)) {
                Diagnostic d;
                d.code = "XP2011";
                d.severity = Severity::Error;
                d.message = "the type `" + n->value + "` is defined multiple times";
                d.span = n->span;
                d.underline_len = (uint32_t)n->value.size();
                d.has_secondary = true;
                d.secondary_span = existing->declared_at;
                d.secondary_label = "previous definition of `" + n->value + "` here";
                diags_.push_back(std::move(d));
                break;
            }
            Symbol sym;
            sym.name = n->value;
            sym.kind = SymbolKind::Enum;
            sym.declared_at = n->span;
            for (auto& c : n->children) {
                if (c && c->kind == NodeKind::TypeParamDecl) {
                    sym.type_params.push_back({c->value, c->extra});
                }
            }
            symtab_.declare(sym);
            enum_decl_nodes_[n->value] = n.get();
            // Each variant's arity/payload-type validation happens at
            // the actual construction site (check_call /
            // check_member, see below) rather than here — collection
            // only needs to make the enum's own name resolvable so
            // `Status.Ok` / `Status.Error(...)` don't fail with
            // "cannot find value `Status`" before ever reaching a
            // variant-specific check.
            break;
        }

        case NodeKind::GlobalDecl:
        case NodeKind::ConstDecl:
        case NodeKind::FluxDecl: {
            if (n->value.empty()) break;
            Symbol sym;
            sym.name = n->value;
            sym.kind = SymbolKind::Variable;
            sym.type = n->extra.empty() ? "auto" : n->extra;
            sym.declared_at = n->span;
            sym.is_mutable = (n->kind != NodeKind::ConstDecl); // flux is reactive
                                                                  // but still assignable,
                                                                  // same as a global
            symtab_.declare(sym);
            break;
        }

        case NodeKind::AtomDecl:
        case NodeKind::ShadowDecl: {
            // Top-level atom/shadow (e.g. the stdlib's `atom PI: float
            // = ...`, or a top-level program's `shadow t1 = ...`).
            // Function-body-local atom/shadow are handled later, in
            // check_block, with proper scoping — this collection pass
            // only walks top-level nodes.
            if (n->value.empty()) break;
            Symbol sym;
            sym.name = n->value;
            sym.kind = SymbolKind::Variable;
            // An explicit annotation (`atom u: User = ...`) always
            // wins; absent one, fall back to inferring from the
            // initializer expression itself (`atom u = spawn User
            // {...}` — SpawnExpr's own value already carries the
            // concrete type name) rather than defaulting straight to
            // "auto". Without this, every un-annotated spawn'd atom
            // read back as type "auto" everywhere downstream that
            // needs a real type — including generic bound-checking at
            // a call site, which silently skips "auto" arguments
            // rather than false-positive on something it has no type
            // information for.
            if (!n->extra.empty()) {
                sym.type = n->extra;
            } else if (!n->children.empty() && n->children[0] &&
                       n->children[0]->kind == NodeKind::SpawnExpr) {
                sym.type = n->children[0]->value;
            } else {
                sym.type = "auto";
            }
            sym.declared_at = n->span;
            sym.is_mutable = (n->kind == NodeKind::ShadowDecl);
            symtab_.declare(sym);
            break;
        }

        case NodeKind::TupleDestructure: {
            // Top-level `atom (lo, hi) = min_max(numbers)` — same
            // registration as check_stmt's TupleDestructure case
            // (see that comment), just at Pass 1 / top-level scope
            // rather than inside a function body. The initializer
            // expression itself isn't checked here (Pass 1 only
            // registers symbols; check_program's statement-level pass
            // checks expressions), only the bound names are declared.
            if (n->children.empty()) break;
            size_t init_idx = n->children.size() - 1;
            for (size_t i = 0; i < init_idx; i++) {
                if (!n->children[i] || n->children[i]->value.empty()) continue;
                Symbol sym;
                sym.name = n->children[i]->value;
                sym.kind = SymbolKind::Variable;
                sym.type = "auto";
                sym.declared_at = n->span;
                sym.is_mutable = (n->value == "shadow");
                symtab_.declare(sym);
            }
            break;
        }

        case NodeKind::ExternDecl: {
            if (n->value.empty()) break;
            Symbol sym;
            sym.name = n->value;
            sym.kind = SymbolKind::Function;
            sym.type = n->extra2.empty() ? "void" : n->extra2;
            sym.declared_at = n->span;
            sym.is_used = true; // FFI declarations are rarely "unused" in
                                 // a meaningful sense — they exist to be
                                 // available, not necessarily called from
                                 // every translation unit
            FunctionSig sig;
            sig.return_type = sym.type;
            for (auto& c : n->children) {
                if (c && c->kind == NodeKind::FieldDecl) {
                    sig.param_types.push_back(c->extra.empty() ? "auto" : c->extra);
                }
            }
            sym.fn_sig = std::move(sig);
            symtab_.declare(sym);
            break;
        }

        case NodeKind::Block: {
            // extern "C" { pulse a... pulse b... } block form —
            // unwrap and register each ExternDecl child the same way.
            for (auto& c : n->children) {
                if (c && c->kind == NodeKind::ExternDecl) collect_one_declaration(c);
            }
            break;
        }

        case NodeKind::RealmDecl: {
            // Symbols declared inside a realm are scoped under that
            // realm's namespace at the C++ level (Realm::member), but
            // for this pass's purposes — catching undefined names at
            // the call site — registering them as flat top-level
            // names is a reasonable approximation: XPhage's `use`
            // declarations bring a realm member into unqualified
            // scope, and fully qualified name resolution (Geometry::
            // length_sq) is left to the C++ compiler's own namespace
            // lookup as a deliberate scope limitation of this pass.
            for (auto& c : n->children) collect_one_declaration(c);
            break;
        }

        default:
            break;
    }
}

// ── Pass 2: body checking ─────────────────────────────────────────
void SemanticAnalyzer::check_program(const Program& prog) {
    for (auto& n : prog) {
        if (!n) continue;
        if (n->kind == NodeKind::PulseDecl || n->kind == NodeKind::AsyncPulseDecl) {
            check_pulse_body(n);
        } else if (n->kind == NodeKind::RealmDecl) {
            check_program(n->children);
        } else if (n->kind == NodeKind::ImplDecl) {
            // Previously fell to the final `else` branch below and
            // was passed to check_stmt as if it were a bare top-level
            // statement — meaningless for a declaration node, and
            // meant every impl method body was completely unchecked
            // (no parameter registration, self included — see
            // check_pulse_body's handling below for what a proper
            // method body check now does — no identifier/call
            // validation, nothing). Each method is checked the same
            // way a standalone pulse's body is, since methods and
            // pulses share the same body-checking needs (parameter
            // scoping, self included since parse_field_decl now
            // parses it as a real FieldDecl parameter).
            check_impl_conformance(n);
            auto saved_tparams = fn_type_params_;
            for (auto& c : n->children)
                if (c && c->kind == NodeKind::TypeParamDecl) fn_type_params_[c->value] = c->extra;
            struct RestoreTP { std::unordered_map<std::string, std::string>& m, saved;
                               ~RestoreTP() { m = saved; } } restore_tp{fn_type_params_, saved_tparams};
            for (auto& method : n->children) {
                if (method && method->kind != NodeKind::TypeParamDecl)
                    check_pulse_body(method, n->extra);
            }
        } else if (n->kind == NodeKind::EnumDecl) {
            // Nothing to check inside an EnumDecl's own children
            // (EnumVariant/TypeParamDecl aren't executable) — but
            // this must still be excluded from the bare-statement
            // fallback below, the same reasoning as ImplDecl above.
            continue;
        } else if (n->kind == NodeKind::Block) {
            // extern block — no body to check (declarations only)
            continue;
        } else if (n->kind == NodeKind::ForgeDecl && !n->children.empty()) {
            for (auto& f : n->children)
                if (f && f->kind == NodeKind::FieldDecl)
                    check_not_nexus_type(f->extra, f->span, "field");
            continue;
        } else if (n->kind == NodeKind::ForgeDecl || n->kind == NodeKind::NexusDecl ||
                   n->kind == NodeKind::GlobalDecl || n->kind == NodeKind::ConstDecl ||
                   n->kind == NodeKind::FluxDecl ||
                   n->kind == NodeKind::ExternDecl || n->kind == NodeKind::UseDecl ||
                   n->kind == NodeKind::LinkStmt) {
            continue; // declarations already handled in pass 1; nothing
                      // further to check at top level for these kinds
        } else {
            // A bare top-level statement (atom/shadow with an
            // initializer expression, beam, an assignment, etc.) —
            // these run inside the implicit main(), so check them in
            // the top-level scope directly, same as a statement
            // inside any function body.
            check_stmt(n);
        }
    }
}

// ── `?` validation ───────────────────────────────────────────────
namespace {

std::string strip_spaces(const std::string& s) {
    std::string o;
    for (char c : s) if (c != ' ') o += c;
    return o;
}

// "own T" / "ref T" / "mut_ref T" -> "T": qualifiers change how a value
// is passed, not what it is.
std::string strip_qualifiers(std::string t) {
    for (;;) {
        if (t.compare(0, 4, "own ") == 0)          t = t.substr(4);
        else if (t.compare(0, 4, "ref ") == 0)     t = t.substr(4);
        else if (t.compare(0, 8, "mut_ref ") == 0) t = t.substr(8);
        else break;
    }
    return t;
}

// Splits "Result<Vec<int>, str>" into base "Result" and top-level
// arguments {"Vec<int>", "str"}, honouring nested angle brackets. A type
// with no <...> yields an empty argument list and base == the whole type.
std::vector<std::string> split_type_args(const std::string& s, std::string& base) {
    std::vector<std::string> out;
    size_t lt = s.find('<');
    if (lt == std::string::npos || s.empty() || s.back() != '>') { base = s; return out; }
    base = s.substr(0, lt);
    std::string inner = s.substr(lt + 1, s.size() - lt - 2), cur;
    int depth = 0;
    for (char c : inner) {
        if (c == '<') depth++;
        if (c == '>') depth--;
        if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); }
        else if (c != ' ') cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool mentions_word(const std::string& hay, const std::string& word) {
    if (word.empty()) return false;
    size_t pos = hay.find(word);
    while (pos != std::string::npos) {
        bool l = pos == 0 || !(std::isalnum((unsigned char)hay[pos - 1]) || hay[pos - 1] == '_');
        size_t e = pos + word.size();
        bool r = e >= hay.size() || !(std::isalnum((unsigned char)hay[e]) || hay[e] == '_');
        if (l && r) return true;
        pos = hay.find(word, pos + 1);
    }
    return false;
}

bool mentions_any(const std::string& hay, const std::vector<std::string>& words) {
    for (auto& w : words) if (mentions_word(hay, w)) return true;
    return false;
}

// Replaces each whole-word occurrence of a type parameter with its argument.
std::string substitute_type_words(std::string t, const std::vector<std::string>& params,
                                  const std::vector<std::string>& args) {
    for (size_t k = 0; k < params.size() && k < args.size(); k++) {
        std::string out;
        size_t i = 0;
        const std::string& p = params[k];
        while (i < t.size()) {
            if (t.compare(i, p.size(), p) == 0 && mentions_word(t.substr(i, p.size()), p) &&
                (i == 0 || !(std::isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_')) &&
                (i + p.size() >= t.size() || !(std::isalnum((unsigned char)t[i + p.size()]) || t[i + p.size()] == '_'))) {
                out += args[k];
                i += p.size();
            } else {
                out += t[i++];
            }
        }
        t = out;
    }
    return t;
}

bool is_builtin_scalar(const std::string& t) {
    return t == "int" || t == "float" || t == "str" || t == "bool";
}

} // namespace

SemanticAnalyzer::TryShape SemanticAnalyzer::try_shape_of(const std::string& type_str) const {
    TryShape sh;
    std::string base;
    auto args = split_type_args(strip_spaces(strip_qualifiers(type_str)), base);
    auto it = enum_decl_nodes_.find(base);
    if (it == enum_decl_nodes_.end()) return sh;
    const ASTNode* en = it->second;

    std::vector<std::string> params;
    for (auto& c : en->children)
        if (c && c->kind == NodeKind::TypeParamDecl) params.push_back(c->value);

    const ASTNode *ok = nullptr, *err = nullptr, *some = nullptr, *none = nullptr;
    for (auto& v : en->children) {
        if (!v || v->kind != NodeKind::EnumVariant) continue;
        if (v->value == "Ok")   ok   = v.get();
        if (v->value == "Err")  err  = v.get();
        if (v->value == "Some") some = v.get();
        if (v->value == "None") none = v.get();
    }
    auto payload = [&](const ASTNode* v) {
        std::string ft = strip_spaces(v->children[0] ? v->children[0]->extra : std::string());
        return substitute_type_words(ft, params, args);
    };
    if (ok && err && ok->children.size() == 1 && err->children.size() == 1) {
        sh.kind = TryShape::ResultLike;
        sh.enum_name = base;
        sh.ok_type = payload(ok);
        sh.fail_type = payload(err);
    } else if (some && none && some->children.size() == 1 && none->children.empty()) {
        sh.kind = TryShape::OptionLike;
        sh.enum_name = base;
        sh.ok_type = payload(some);
    }
    return sh;
}

void SemanticAnalyzer::check_propagate(const ASTNodePtr& expr) {
    const uint32_t ulen = 1; // the `?` token itself

    // 1. Must be inside a pulse: `?` returns from the enclosing function.
    if (current_fn_name_.empty()) {
        error("XP2040", expr->span,
              "the `?` operator can only be used inside a pulse",
              ulen,
              std::string("`?` returns early from the enclosing function, so it needs one; "
                          "top-level statements have nothing to return to"));
        return;
    }

    // 2. That pulse must itself return a Result-like / Option-like enum.
    TryShape fn_shape = try_shape_of(current_fn_ret_);
    if (fn_shape.kind == TryShape::NotTry) {
        std::string what = current_fn_ret_.empty()
            ? "returns nothing"
            : "returns `" + current_fn_ret_ + "`";
        error("XP2041", expr->span,
              "the `?` operator can only be used in a pulse that returns Result or Option, "
              "but `" + current_fn_name_ + "` " + what,
              ulen,
              std::string("change the return type to `Result<T, E>` (or `Option<T>`), or handle the "
                          "value with `probe` instead of `?`"));
        return;
    }

    // 3. Work out the operand's static type where this pass can. It only
    //    knows a call's type when the callee is a declared pulse, and an
    //    identifier's when its declaration was annotated; everywhere else
    //    it stays silent (the backends re-check against real types).
    if (expr->children.empty() || !expr->children[0]) return;
    const ASTNodePtr& operand = expr->children[0];
    std::string operand_ty;
    std::vector<std::string> callee_tparams;
    if (operand->kind == NodeKind::CallExpr && !operand->children.empty() && operand->children[0] &&
        operand->children[0]->kind == NodeKind::Identifier) {
        if (auto* sym = symtab_.lookup(operand->children[0]->value)) {
            if (sym->fn_sig) {
                operand_ty = sym->fn_sig->return_type;
                for (auto& tp : sym->fn_sig->type_params) callee_tparams.push_back(tp.name);
            }
        }
    } else if (operand->kind == NodeKind::Identifier) {
        if (auto* sym = symtab_.lookup(operand->value)) operand_ty = sym->type;
    }
    if (operand_ty.empty() || operand_ty == "auto") return;

    TryShape op_shape = try_shape_of(operand_ty);
    if (op_shape.kind == TryShape::NotTry) {
        // Only complain about types we are SURE are not Result/Option: the
        // builtin scalars, and a declared enum that lacks the shape. An
        // unknown/abstract type is left to the backends.
        std::string ob, sink;
        split_type_args(strip_spaces(strip_qualifiers(operand_ty)), ob);
        if (is_builtin_scalar(ob) || (enum_decl_nodes_.count(ob) && !mentions_any(operand_ty, callee_tparams))) {
            error("XP2042", expr->span,
                  "the `?` operator can only be applied to a Result or Option, but this value has type `" +
                  operand_ty + "`",
                  ulen,
                  std::string("`?` unwraps the success value and returns the failure early; "
                              "a plain `" + operand_ty + "` has no failure case"));
        }
        return;
    }

    // 4. Result and Option do not convert into each other.
    if (op_shape.kind != fn_shape.kind) {
        std::string op_word = op_shape.kind == TryShape::ResultLike ? "Result" : "Option";
        std::string fn_word = fn_shape.kind == TryShape::ResultLike ? "Result" : "Option";
        error("XP2043", expr->span,
              "cannot use `?` on `" + operand_ty + "` in a pulse that returns `" + current_fn_ret_ + "`",
              ulen,
              "`?` does not convert between " + op_word + " and " + fn_word +
              "; `probe` on the value and build the " + fn_word + " you want");
        return;
    }

    // 5. Result -> Result: the error type is carried through unchanged
    //    (there is no implicit conversion), so it must match exactly.
    if (op_shape.kind == TryShape::ResultLike) {
        std::vector<std::string> abstract_names = current_fn_tparams_;
        abstract_names.insert(abstract_names.end(), callee_tparams.begin(), callee_tparams.end());
        const std::string a = strip_spaces(op_shape.fail_type), b = strip_spaces(fn_shape.fail_type);
        if (!a.empty() && !b.empty() && a != b &&
            !mentions_any(a, abstract_names) && !mentions_any(b, abstract_names)) {
            error("XP2044", expr->span,
                  "`?` would return error type `" + a + "`, but `" + current_fn_name_ +
                  "` returns error type `" + b + "`",
                  ulen,
                  std::string("there is no automatic error conversion; change one of the error types "
                              "so they match, or `probe` and convert the error yourself"));
        }
    }
}

// `?` inside an f-string interpolation. The parser keeps an f-string as raw
// text (no child nodes), so the analyzer never sees a PropagateExpr there;
// scan the `{...}` segments for a `?` outside string literals instead. This is
// rejected uniformly because backends can only realise it differently: an
// f-string is built inside a helper lambda in the C++ output, where the early
// return `?` performs would leave the lambda rather than the pulse. The
// language's meaning must not depend on the backend, so the fix is to bind
// the value first.
void SemanticAnalyzer::check_fstring_propagate(const ASTNodePtr& expr) {
    const std::string& v = expr->value;
    size_t i = 0;
    while (i < v.size()) {
        if (v[i] != '{') { i++; continue; }
        if (i + 1 < v.size() && v[i + 1] == '{') { i += 2; continue; }   // literal "{{"
        int depth = 1;
        bool in_str = false, has_q = false;
        size_t j = i + 1;
        for (; j < v.size(); j++) {
            char c = v[j];
            if (in_str) {
                if (c == '\\') j++;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{') depth++;
            else if (c == '}') { if (--depth == 0) break; }
            else if (c == '?') has_q = true;
        }
        if (has_q) {
            error("XP2045", expr->span,
                  "the `?` operator cannot be used inside an f-string interpolation",
                  1,
                  std::string("bind the value first, then interpolate it: `atom v = expr?` and `{v}`"));
            return;
        }
        i = j + 1;
    }
}

void SemanticAnalyzer::check_pulse_body(const ASTNodePtr& pulse_decl, const std::string& self_type) {
    // Remember which pulse we are inside, for `?` (see check_propagate).
    // Saved/restored because a pulse can be declared inside another
    // pulse's body: `?` in the inner body returns from the INNER pulse.
    const std::string              saved_fn_name    = current_fn_name_;
    const std::string              saved_fn_ret     = current_fn_ret_;
    const std::vector<std::string> saved_fn_tparams = current_fn_tparams_;
    current_fn_name_ = pulse_decl->value.empty() ? std::string("<anonymous>") : pulse_decl->value;
    current_fn_ret_  = pulse_decl->extra2;
    current_fn_tparams_.clear();
    for (auto& c : pulse_decl->children)
        if (c && c->kind == NodeKind::TypeParamDecl) current_fn_tparams_.push_back(c->value);

    symtab_.push_scope();

    struct RestoreFnTP { std::unordered_map<std::string, std::string>& m, saved;
                         ~RestoreFnTP() { m = saved; } } restore_fn_tp{fn_type_params_, fn_type_params_};
    for (auto& c : pulse_decl->children)
        if (c && c->kind == NodeKind::TypeParamDecl) fn_type_params_[c->value] = c->extra;
    check_not_nexus_type(pulse_decl->extra2, pulse_decl->span, "return type");
    for (auto& c : pulse_decl->children)
        if (c && c->kind == NodeKind::TypeParamDecl) check_bound_names(c);

    // Impl methods: `self` is always in scope. Methods written the way
    // the language book writes them (`name() -> str { ... self.x ... }`,
    // no `pulse`, no explicit `self`) carry no `self` parameter node, so
    // declare it here; an explicit `self` parameter is declared below
    // like any other, but typed as the receiver instead of "auto".
    bool has_explicit_self = false;
    for (auto& c : pulse_decl->children)
        if (c && c->kind == NodeKind::FieldDecl && c->value == "self") has_explicit_self = true;
    if (!self_type.empty() && !has_explicit_self && pulse_decl->kind == NodeKind::MethodDecl) {
        Symbol self_sym;
        self_sym.name = "self";
        self_sym.kind = SymbolKind::Variable;
        self_sym.type = self_type;
        self_sym.declared_at = pulse_decl->span;
        self_sym.is_used = true;
        symtab_.declare(self_sym);
    }

    for (auto& c : pulse_decl->children) {
        if (c && c->kind == NodeKind::FieldDecl && !c->value.empty()) {
            if (c->value != "self") check_not_nexus_type(c->extra, c->span, "parameter");
            Symbol sym;
            sym.name = c->value;
            sym.kind = SymbolKind::Variable;
            sym.type = c->extra.empty()
                           ? ((c->value == "self" && !self_type.empty()) ? self_type : std::string("auto"))
                           : c->extra;
            sym.declared_at = c->span;
            sym.is_used = true; // parameters aren't flagged unused — a
                                 // function signature often needs a
                                 // parameter present even if a
                                 // particular implementation doesn't
                                 // use it (interface consistency)
            symtab_.declare(sym);
        }
    }

    for (auto& c : pulse_decl->children) {
        if (c && c->kind == NodeKind::Block) check_block(c);
    }

    symtab_.pop_scope();
    current_fn_name_    = saved_fn_name;
    current_fn_ret_     = saved_fn_ret;
    current_fn_tparams_ = saved_fn_tparams;
}

// ── nexus: usable as a bound, not as a type ───────────────────────
void SemanticAnalyzer::check_not_nexus_type(const std::string& type_str, const Span& span,
                                             const char* what) {
    if (type_str.empty()) return;
    // strip generic arguments / pointer-ish decoration down to the head name
    std::string head = type_str;
    size_t lt = head.find('<');
    if (lt != std::string::npos) head = head.substr(0, lt);
    while (!head.empty() && (head.front() == ' ' || head.front() == '&' || head.front() == '*'))
        head.erase(head.begin());
    if (nexus_nodes_.count(head)) {
        error("XP2014", span,
              std::string(what) + " type `" + head + "` is a nexus, not a type",
              (uint32_t)head.size(),
              "a nexus is a compile-time contract: write a generic bound instead, "
              "e.g. `pulse f<T: " + head + ">(x: T)`. Dynamic dispatch through a "
              "nexus-typed value is not supported");
    }
}

void SemanticAnalyzer::check_bound_names(const ASTNodePtr& tp) {
    if (!tp || tp->extra.empty()) return;
    size_t start = 0;
    const std::string& b = tp->extra;
    while (start <= b.size()) {
        size_t plus = b.find('+', start);
        std::string one = b.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (!one.empty() && one != "Numeric" && !nexus_nodes_.count(one)) {
            error("XP2016", tp->span,
                  "unknown nexus `" + one + "` in the bound of type parameter `" + tp->value + "`",
                  (uint32_t)one.size(),
                  "declare it with `nexus " + one + " { ... }` or fix the name");
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
}

// ── nexus: impl conformance ───────────────────────────────────────
void SemanticAnalyzer::check_impl_conformance(const ASTNodePtr& impl) {
    if (!impl || impl->value.empty()) return;          // inherent impl: nothing to conform to
    auto nit = nexus_nodes_.find(impl->value);
    if (nit == nexus_nodes_.end()) {
        error("XP2016", impl->span, "unknown nexus `" + impl->value + "`",
              (uint32_t)impl->value.size(),
              "declare it with `nexus " + impl->value + " { ... }`");
        return;
    }
    const ASTNode* nx = nit->second;
    const std::string& recv = impl->extra;

    auto is_method = [](const ASTNodePtr& m) {
        return m && (m->kind == NodeKind::MethodDecl || m->kind == NodeKind::PulseDecl) &&
               !m->value.empty();
    };
    // A member written with `pulse` and without a `self` parameter has no
    // receiver: an associated function (`Type::f()` / `T::f()`), not a method.
    auto is_assoc = [](const ASTNode& m) {
        if (m.kind != NodeKind::PulseDecl) return false;
        for (auto& c : m.children)
            if (c && c->kind == NodeKind::FieldDecl && c->value == "self") return false;
        return true;
    };
    auto has_block = [](const ASTNode& m) {
        for (auto& c : m.children) if (c && c->kind == NodeKind::Block) return true;
        return false;
    };
    auto params_of = [](const ASTNode& m) {
        std::vector<const ASTNode*> ps;
        for (auto& c : m.children)
            if (c && c->kind == NodeKind::FieldDecl && c->value != "self") ps.push_back(c.get());
        return ps;
    };
    // Realm prefixes are not part of a type's identity for conformance:
    // `Self` expands to the realm-qualified receiver (`Zoo::Lion`) while a
    // nexus written elsewhere names the bare type.
    auto strip_realms = [](const std::string& t) {
        std::string out;
        size_t i = 0;
        while (i < t.size()) {
            size_t j = i;
            while (j < t.size() && (std::isalnum((unsigned char)t[j]) || t[j] == '_')) j++;
            if (j > i && j + 1 < t.size() && t[j] == ':' && t[j + 1] == ':') { i = j + 2; continue; }
            if (j == i) { out += t[i++]; continue; }
            out += t.substr(i, j - i);
            i = j;
        }
        return out;
    };
    auto norm_ret = [&](const std::string& r) {
        std::string t = strip_realms(replace_self_type(r, recv));
        return t.empty() ? std::string("void") : t;
    };

    std::unordered_map<std::string, const ASTNode*> provided;
    for (auto& m : impl->children) if (is_method(m)) provided[m->value] = m.get();

    std::unordered_set<std::string> declared;
    for (auto& req : nx->children) {
        if (!req || (req->kind != NodeKind::MethodDecl && req->kind != NodeKind::PulseDecl) ||
            req->value.empty()) continue;
        declared.insert(req->value);
        auto pit = provided.find(req->value);
        if (pit == provided.end()) {
            if (!has_block(*req)) {
                error("XP2015", impl->span,
                      "`impl " + impl->value + " for " + recv + "` is missing method `" +
                          req->value + "`",
                      (uint32_t)impl->value.size(),
                      "`" + impl->value + "` requires `" + req->value + "`; add it to this impl");
            }
            continue;
        }
        const ASTNode* got = pit->second;
        if (is_assoc(*req) != is_assoc(*got)) {
            error("XP2017", got->span,
                  "`" + req->value + "` is " +
                      std::string(is_assoc(*req) ? "an associated function (no `self`)" : "a method (takes `self`)") +
                      " in nexus `" + impl->value + "` but " +
                      std::string(is_assoc(*got) ? "an associated function" : "a method") + " in this impl",
                  (uint32_t)req->value.size(),
                  is_assoc(*req) ? "write it as `pulse " + req->value + "(...)` with no `self` parameter"
                                 : "drop the `pulse` keyword or add a `self` parameter");
            continue;
        }
        auto want_ps = params_of(*req);
        auto got_ps  = params_of(*got);
        if (want_ps.size() != got_ps.size()) {
            error("XP2017", got->span,
                  "method `" + req->value + "` takes " + std::to_string(got_ps.size()) +
                      " parameter(s) but nexus `" + impl->value + "` declares " +
                      std::to_string(want_ps.size()),
                  (uint32_t)req->value.size());
            continue;
        }
        for (size_t i = 0; i < want_ps.size(); i++) {
            std::string w = strip_realms(replace_self_type(want_ps[i]->extra, recv));
            const std::string g = strip_realms(got_ps[i]->extra);
            if (!w.empty() && !g.empty() && w != g) {
                error("XP2017", got_ps[i]->span,
                      "parameter `" + got_ps[i]->value + "` of `" + req->value + "` has type `" + g +
                          "` but nexus `" + impl->value + "` declares `" + w + "`",
                      (uint32_t)g.size());
            }
        }
        if (norm_ret(req->extra2) != norm_ret(got->extra2)) {
            error("XP2017", got->span,
                  "method `" + req->value + "` returns `" + norm_ret(got->extra2) +
                      "` but nexus `" + impl->value + "` declares `" + norm_ret(req->extra2) + "`",
                  (uint32_t)req->value.size());
        }
    }
    for (auto& m : impl->children) {
        if (is_method(m) && !declared.count(m->value)) {
            error("XP2018", m->span,
                  "method `" + m->value + "` is not a member of nexus `" + impl->value + "`",
                  (uint32_t)m->value.size(),
                  "move it to an inherent `impl " + recv + " { ... }` block");
        }
    }
}

void SemanticAnalyzer::check_block(const ASTNodePtr& block) {
    symtab_.push_scope();
    for (auto& stmt : block->children) check_stmt(stmt);
    symtab_.pop_scope();
}

void SemanticAnalyzer::check_stmt(const ASTNodePtr& stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case NodeKind::AtomDecl:
        case NodeKind::ShadowDecl: {
            check_not_nexus_type(stmt->extra, stmt->span, "variable");
            if (!stmt->children.empty() && stmt->children[0]) {
                check_expr(stmt->children[0]);
            }
            if (!stmt->value.empty()) {
                if (symtab_.is_at_root_scope()) {
                    // Top-level atom/shadow — Pass 1 (collect_declar-
                    // ations) already registered this exact symbol
                    // before body-checking ever started. Re-declaring
                    // it here would find that Pass-1 registration via
                    // lookup_local and report the statement as
                    // shadowing itself. There is genuinely nothing
                    // left to do here for a top-level declaration
                    // beyond the initializer check already done above
                    // — mark it used-if-referenced is handled the
                    // normal way when something later reads it.
                } else {
                    if (auto* existing = symtab_.lookup_local(stmt->value)) {
                        warning("XP2020", stmt->span,
                            "variable `" + stmt->value + "` shadows a previous declaration in the same scope",
                            (uint32_t)stmt->value.size());
                    }
                    Symbol sym;
                    sym.name = stmt->value;
                    sym.kind = SymbolKind::Variable;
                    // Same inference-from-initializer fallback as the
                    // top-level AtomDecl/ShadowDecl case above — see
                    // its comment for why.
                    if (!stmt->extra.empty()) {
                        sym.type = stmt->extra;
                    } else if (!stmt->children.empty() && stmt->children[0] &&
                               stmt->children[0]->kind == NodeKind::SpawnExpr) {
                        sym.type = stmt->children[0]->value;
                    } else {
                        sym.type = "auto";
                    }
                    sym.declared_at = stmt->span;
                    sym.is_mutable = (stmt->kind == NodeKind::ShadowDecl);
                    symtab_.declare(sym);
                }
            }
            break;
        }

        case NodeKind::TupleDestructure: {
            // atom (lo, hi) = min_max(numbers) — children[0..n-2] are
            // the bound Identifier names, children[n-1] is the
            // initializer. Registers each bound name as a real
            // symbol (type "auto" — this pass doesn't attempt to
            // infer each tuple element's individual concrete type,
            // matching this pass's general "auto when not confidently
            // knowable" convention elsewhere) so a later reference to
            // e.g. `lo` resolves instead of failing with "cannot find
            // value `lo`" the way it would with no declaration at
            // all — TupleDestructure isn't handled by the generic
            // default: case (which only recurses into children, never
            // declares anything), so without this case here, every
            // name a tuple destructure binds would be entirely
            // unresolvable afterward.
            if (stmt->children.empty()) break;
            size_t init_idx = stmt->children.size() - 1;
            check_expr(stmt->children[init_idx]);
            for (size_t i = 0; i < init_idx; i++) {
                if (!stmt->children[i] || stmt->children[i]->value.empty()) continue;
                Symbol sym;
                sym.name = stmt->children[i]->value;
                sym.kind = SymbolKind::Variable;
                sym.type = "auto";
                sym.declared_at = stmt->span;
                sym.is_mutable = (stmt->value == "shadow");
                symtab_.declare(sym);
            }
            break;
        }

        case NodeKind::Block:
            check_block(stmt);
            break;

        case NodeKind::IfStmt:
        case NodeKind::ElifStmt: {
            if (!stmt->children.empty()) check_expr(stmt->children[0]);
            for (size_t i = 1; i < stmt->children.size(); i++) {
                if (stmt->children[i] && stmt->children[i]->kind == NodeKind::Block)
                    check_block(stmt->children[i]);
                else
                    check_stmt(stmt->children[i]);
            }
            break;
        }

        case NodeKind::WhileStmt: {
            if (!stmt->children.empty()) check_expr(stmt->children[0]);
            for (size_t i = 1; i < stmt->children.size(); i++) check_stmt(stmt->children[i]);
            break;
        }

        case NodeKind::ForStmt: {
            // for i in range { ... } — the loop variable is scoped to
            // the loop body only.
            symtab_.push_scope();
            if (!stmt->value.empty()) {
                Symbol sym;
                sym.name = stmt->value;
                sym.kind = SymbolKind::Variable;
                sym.type = "auto";
                sym.declared_at = stmt->span;
                sym.is_used = true;
                symtab_.declare(sym);
            }
            for (auto& c : stmt->children) {
                if (c && c->kind == NodeKind::Block) check_block(c);
                else if (c) check_expr(c);
            }
            symtab_.pop_scope();
            break;
        }

        case NodeKind::ReturnStmt:
        case NodeKind::YieldStmt:
        case NodeKind::BeamStmt:
        case NodeKind::BypassStmt: {
            for (auto& c : stmt->children) check_expr(c);
            break;
        }

        case NodeKind::AbsorbStmt:
        case NodeKind::EmitStmt: {
            // absorb "event" { ... } / emit "event" { ... } — the
            // payload is a Block (a statement list), which needs
            // check_block (its own scope, statement-by-statement
            // checking), not check_expr (which has no notion of a
            // Block as a statement container and would just recurse
            // through its children as if they were subexpressions).
            for (auto& c : stmt->children) {
                if (c && c->kind == NodeKind::Block) check_block(c);
                else if (c) check_expr(c);
            }
            break;
        }

        case NodeKind::ExprStmt: {
            for (auto& c : stmt->children) check_expr(c);
            // A bare ExprStmt's "value" itself might directly be the
            // expression (depending on how the parser shaped it) —
            // check defensively.
            break;
        }

        case NodeKind::UnsafeBlock: {
            if (!stmt->children.empty() && stmt->children[0]) check_block(stmt->children[0]);
            break;
        }

        case NodeKind::PulseDecl:
        case NodeKind::AsyncPulseDecl: {
            // A nested/local function definition — register it in
            // the current scope (so sibling statements can call it)
            // then check its body in its own nested scope.
            collect_one_declaration(stmt);
            check_pulse_body(stmt);
            break;
        }

        case NodeKind::ProbeStmt: {
            // probe subject { diverge Enum.Variant(a, b) -> body ... }
            //
            // children[0] is the subject expression; every other child
            // is a ProbeArm. A destructuring pattern
            // (`Enum.Variant(a, b)`) introduces the names a, b for the
            // duration of THAT ARM'S body only, so each arm gets its own
            // scope, exactly like a for-loop variable (see ForStmt). The
            // parser stores the names comma-joined in attrs["bindings"].
            //
            // Previously ProbeStmt fell through to the generic default
            // below, which walks expression children but never opens a
            // scope or declares anything: a payload binding was invisible
            // to the analyzer, so `diverge Option.Some(v) -> use(v)` was
            // rejected with "cannot find value `v`" (an f-string body
            // happened to mask this because it is not identifier-checked).
            // That made nested matching — Result<Option<T>, E>, the exact
            // shape a Result-based stdlib returns — impossible to write.
            if (!stmt->children.empty() && stmt->children[0]) {
                check_expr(stmt->children[0]);
            }
            for (size_t i = 1; i < stmt->children.size(); i++) {
                auto& arm = stmt->children[i];
                if (!arm) continue;
                symtab_.push_scope();
                auto bit = arm->attrs.find("bindings");
                if (bit != arm->attrs.end() && !bit->second.empty()) {
                    size_t start = 0;
                    const std::string& joined = bit->second;
                    while (start <= joined.size()) {
                        size_t comma = joined.find(',', start);
                        std::string name = joined.substr(
                            start, comma == std::string::npos ? std::string::npos : comma - start);
                        if (!name.empty()) {
                            Symbol sym;
                            sym.name = name;
                            sym.kind = SymbolKind::Variable;
                            sym.type = "auto";
                            sym.declared_at = arm->span;
                            sym.is_used = true; // a pattern binding may legitimately go unused
                            symtab_.declare(sym);
                        }
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
                for (auto& c : arm->children) {
                    if (c && c->kind == NodeKind::Block) check_block(c);
                    else if (c) check_expr(c);
                }
                symtab_.pop_scope();
            }
            break;
        }

        default: {
            // Statement kinds not specifically handled above (quantum,
            // chronos, ether, vortex, matrix,
            // synapse, absorb, link, use, realm-nested decls, etc.)
            // still have their expression-typed children walked, so
            // a call/identifier deep inside one of these is still
            // checked — just without a kind-specific scoping rule.
            // This is a deliberate "check what we can identify,
            // don't block on what we don't have a specific rule for
            // yet" trade-off, consistent with the rest of the
            // compiler's incremental approach to language coverage.
            for (auto& c : stmt->children) {
                if (c) check_expr(c);
            }
            break;
        }
    }
}

std::string SemanticAnalyzer::check_expr(const ASTNodePtr& expr) {
    if (!expr) return "auto";

    switch (expr->kind) {
        case NodeKind::Identifier:
            check_identifier(expr);
            if (auto* sym = symtab_.lookup(expr->value)) return sym->type;
            return "auto";

        case NodeKind::CallExpr:
            check_call(expr);
            return "auto"; // return-type inference for the call result
                            // is intentionally out of scope for this
                            // pass — see check_call for what IS checked

        case NodeKind::IntLit:
        case NodeKind::FloatLit: {
            // an explicit suffix (`3u8`, `1.5f32`) fixes the type; i64/f64 are int/float
            auto sfx = expr->attrs.find("suffix");
            if (sfx != expr->attrs.end())
                return sfx->second == "i64" ? "int" : sfx->second == "f64" ? "float" : sfx->second;
            return expr->kind == NodeKind::IntLit ? "int" : "float";
        }
        case NodeKind::BoolLit:   return "bool";
        case NodeKind::StringLit: return "str";
        case NodeKind::FStringLit:
            check_fstring_propagate(expr);
            return "str";
        case NodeKind::NullLit:   return "auto";

        case NodeKind::BinaryOp:
            check_binary_op(expr);
            return "auto";

        case NodeKind::AssignExpr:
            check_assign(expr);
            return "auto";

        case NodeKind::SpawnExpr: {
            // SpawnExpr.value is the concrete constructed type's name
            // (e.g. "User", or "Geometry::Vector2" for a
            // realm-qualified spawn — see parse_spawn_expr) — a real,
            // useful type for anything downstream that wants it
            // (generic bound-checking at a call site is the concrete
            // motivating case; previously this fell into the generic
            // "auto" fallback group below, which is fine for most
            // purposes but meant a spawn used directly as a call
            // argument, e.g. `add(spawn User{...}, ...)`, was
            // silently exempted from bound-checking the same way an
            // un-annotated `atom` used to be — see AtomDecl/
            // ShadowDecl's own SpawnExpr-inference fix for the
            // matching case this addresses for the indirect-via-atom
            // form).
            for (auto& c : expr->children) check_expr(c);
            return expr->value.empty() ? "auto" : expr->value;
        }

        case NodeKind::UnaryOp:
        case NodeKind::IndexExpr:
        case NodeKind::MemberExpr:
        case NodeKind::CastExpr:
        case NodeKind::RangeExpr:
        case NodeKind::PipelineExpr:
        case NodeKind::AwaitExpr: {
            for (auto& c : expr->children) check_expr(c);
            return "auto";
        }

        // Its own case: PropagateExpr used to share the block above with
        // several unrelated expression kinds, and `?` validation must run
        // for `?` only.
        case NodeKind::PropagateExpr: {
            for (auto& c : expr->children) check_expr(c);
            check_propagate(expr);
            return "auto";
        }

        default: {
            for (auto& c : expr->children) check_expr(c);
            return "auto";
        }
    }
}

void SemanticAnalyzer::check_identifier(const ASTNodePtr& n) {
    auto* sym = symtab_.lookup(n->value);
    if (sym) {
        sym->is_used = true;
        return;
    }

    auto suggestion = symtab_.suggest_similar(n->value, SymbolKind::Variable);
    if (!suggestion.has_value()) {
        suggestion = symtab_.suggest_similar(n->value, SymbolKind::Function);
    }

    std::optional<std::string> help;
    if (suggestion.has_value()) {
        help = "did you mean `" + *suggestion + "`?";
    }

    error("XP2001", n->span,
          "cannot find value `" + n->value + "` in this scope",
          (uint32_t)n->value.size(), help);
}

// `Type::f(...)` / `T::f(...)`: the head segment names either a type parameter
// (then one of its bounds' nexus must declare `f` as an associated function) or
// a forge/primitive (then it must have an impl providing `f`). A head that is a
// realm or anything else is an ordinary path and is left to the other checks.
void SemanticAnalyzer::check_assoc_path_call(const ASTNodePtr& call) {
    if (!call || call->children.empty() || !call->children[0] ||
        call->children[0]->kind != NodeKind::PathExpr) return;
    const ASTNode& callee = *call->children[0];
    std::vector<std::string> segs;
    for (size_t i = 0;;) {
        size_t j = callee.value.find("::", i);
        if (j == std::string::npos) { segs.push_back(callee.value.substr(i)); break; }
        segs.push_back(callee.value.substr(i, j - i));
        i = j + 2;
    }
    if (segs.size() < 2) return;
    const std::string& fn   = segs.back();
    const std::string& head = segs[segs.size() - 2];
    if (realm_names_.count(head)) return;                       // Realm::fn

    auto tp = fn_type_params_.find(head);
    if (tp != fn_type_params_.end()) {
        bool found = false;
        size_t start = 0;
        const std::string& b = tp->second;
        while (!b.empty() && start <= b.size()) {
            size_t plus = b.find('+', start);
            std::string one = b.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            auto nit = nexus_nodes_.find(one);
            if (nit != nexus_nodes_.end())
                for (auto& m : nit->second->children)
                    if (m && m->kind == NodeKind::PulseDecl && m->value == fn) found = true;
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        if (!found) {
            error("XP2019", callee.span,
                  "type parameter `" + head + "` has no associated function `" + fn + "` in its bounds",
                  (uint32_t)callee.value.size(),
                  tp->second.empty()
                      ? "`" + head + "` has no bound; add one whose nexus declares `pulse " + fn + "(...)`, "
                        "e.g. `<" + head + ": SomeNexus>`"
                      : "none of `" + tp->second + "` declares `pulse " + fn + "(...)`");
        }
        return;
    }
    bool primitive = head == "int" || head == "float" || head == "bool" || head == "str";
    if (forge_names_.count(head) || primitive) {
        auto it = assoc_fns_.find(head);
        if (it == assoc_fns_.end() || !it->second.count(fn)) {
            error("XP2019", callee.span,
                  "no associated function `" + fn + "` found for type `" + head + "`",
                  (uint32_t)callee.value.size(),
                  "declare it in an `impl " + head + " { pulse " + fn + "(...) -> ... }` block "
                  "(a member without `self`)");
        }
    }
}

void SemanticAnalyzer::check_call(const ASTNodePtr& n) {
    check_assoc_path_call(n);
    if (n->children.empty()) return;
    auto& callee = n->children[0];

    // Only a plain identifier callee gets full resolution + arity
    // checking here. A computed callee (e.g. calling through a
    // member access or an array index) is walked for its
    // subexpressions but not arity-checked, since the symbol table
    // doesn't yet track member-function signatures separately.
    if (!callee || callee->kind != NodeKind::Identifier) {
        for (auto& c : n->children) check_expr(c);
        return;
    }

    auto* sym = symtab_.lookup(callee->value);
    if (!sym) {
        auto suggestion = symtab_.suggest_similar(callee->value, SymbolKind::Function);
        std::optional<std::string> help;
        if (suggestion.has_value()) {
            help = "did you mean `" + *suggestion + "`?";
        } else if (callee->value.size() > 2) {
            // No close function-name match — for a call specifically
            // (as opposed to a bare identifier reference), a missing
            // ~link is a very common real cause, so it's worth a
            // generic nudge even without a specific module to name.
            help = "if this is a standard library function, check that the "
                   "module providing it is linked with ~link \"<module>\"";
        }
        error("XP2002", callee->span,
              "cannot find function `" + callee->value + "` in this scope",
              (uint32_t)callee->value.size(), help);
        // Still check the arguments themselves for their own errors,
        // even though the callee itself didn't resolve.
        for (size_t i = 1; i < n->children.size(); i++) check_expr(n->children[i]);
        return;
    }

    sym->is_used = true;

    if (sym->kind != SymbolKind::Function) {
        error("XP2003", callee->span,
              "`" + callee->value + "` is not a function and cannot be called",
              (uint32_t)callee->value.size(),
              "`" + callee->value + "` is declared as a variable here");
        for (size_t i = 1; i < n->children.size(); i++) check_expr(n->children[i]);
        return;
    }

    // Arity check — only when the signature doesn't contain any
    // "auto" parameters (a function genuinely generic over argument
    // count/shape, like the collections module's higher-order
    // functions, intentionally isn't arity-checked here).
    if (sym->fn_sig.has_value()) {
        size_t expected = sym->fn_sig->param_types.size();
        size_t actual = n->children.size() - 1; // children[0] is the callee
        bool has_auto_param = false;
        for (auto& t : sym->fn_sig->param_types) if (t == "auto") has_auto_param = true;

        if (!has_auto_param && expected != actual) {
            std::ostringstream msg;
            msg << "this function takes " << expected
                << (expected == 1 ? " argument" : " arguments")
                << " but " << actual << (actual == 1 ? " was" : " were") << " supplied";
            error("XP2004", n->span, msg.str(), (uint32_t)callee->value.size());
        }

        // Generic call: infer each type parameter's concrete type
        // argument from the actual argument expressions passed at
        // the positions that reference it, then (a) check every
        // position referencing the same type parameter agreed on the
        // same concrete type (e.g. `add<T>(a: T, b: T)` called as
        // `add(1, "x")` — both positions reference T, but infer
        // different concrete types) and (b) validate the inferred
        // type against the parameter's bound, if it has one.
        if (!sym->fn_sig->type_params.empty() && expected == actual) {
            std::unordered_map<std::string, std::string> inferred; // type param name -> concrete type
            std::unordered_map<std::string, Span> first_use_span;  // for a clear conflict message
            for (size_t i = 0; i < expected; i++) {
                const std::string& ptype = sym->fn_sig->param_types[i];
                bool is_type_param = false;
                std::string bound;
                for (auto& tp : sym->fn_sig->type_params) {
                    if (tp.name == ptype) { is_type_param = true; bound = tp.bound; break; }
                }
                if (!is_type_param) continue;
                std::string arg_type = check_expr(n->children[i + 1]);
                if (arg_type.empty() || arg_type == "auto") continue; // nothing concrete to check
                auto it = inferred.find(ptype);
                if (it == inferred.end()) {
                    inferred[ptype] = arg_type;
                    first_use_span[ptype] = n->children[i + 1]->span;
                    std::string unmet = bound.empty() ? std::string()
                                                      : first_unsatisfied_bound(arg_type, bound);
                    if (!unmet.empty()) {
                        error("XP2012", n->children[i + 1]->span,
                              "type `" + arg_type + "` does not satisfy bound `" + unmet +
                              "` required by type parameter `" + ptype + "`",
                              1,
                              "`" + unmet + "` requires an `impl " + unmet + " for " +
                              arg_type + "` (or, for Numeric, int/float)");
                    }
                } else if (it->second != arg_type) {
                    error("XP2013", n->children[i + 1]->span,
                          "type parameter `" + ptype + "` was inferred as `" + it->second +
                          "` here but as `" + arg_type + "` at a previous argument",
                          1,
                          "every argument using the same type parameter must agree on its concrete type");
                }
            }
            // Arguments not referencing a type parameter still need
            // their own subexpressions checked (identifiers marked
            // used, nested calls validated, etc.) — the loop above
            // only called check_expr for type-parameter positions.
            for (size_t i = 0; i < expected; i++) {
                const std::string& ptype = sym->fn_sig->param_types[i];
                bool is_type_param = false;
                for (auto& tp : sym->fn_sig->type_params) if (tp.name == ptype) { is_type_param = true; break; }
                if (!is_type_param) check_expr(n->children[i + 1]);
            }
            return;
        }
    }

    for (size_t i = 1; i < n->children.size(); i++) check_expr(n->children[i]);
}

void SemanticAnalyzer::check_assign(const ASTNodePtr& n) {
    // AssignExpr shape: children[0] = target, children[1] = value
    if (n->children.size() < 2) {
        for (auto& c : n->children) check_expr(c);
        return;
    }
    auto& target = n->children[0];
    check_expr(n->children[1]); // check RHS first — its own identifiers
                                 // must resolve regardless of LHS shape

    if (target && target->kind == NodeKind::Identifier) {
        auto* sym = symtab_.lookup(target->value);
        if (!sym) {
            // Reuse the same undefined-identifier diagnostic as a
            // plain read — assigning to an undeclared name is the
            // same underlying problem.
            check_identifier(target);
            return;
        }
        sym->is_used = true;
        if (!sym->is_mutable) {
            error("XP2030", target->span,
                  "cannot assign to `" + target->value + "`, as it is declared with `atom`",
                  (uint32_t)target->value.size(),
                  "consider using `shadow` instead of `atom` if this value needs to change");
        }
    } else if (target) {
        check_expr(target); // member/index assignment target — walk
                             // its subexpressions for their own checks
    }
}

void SemanticAnalyzer::check_binary_op(const ASTNodePtr& n) {
    for (auto& c : n->children) check_expr(c);
}

} // namespace xphage::sema
