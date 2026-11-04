#include "../include/monomorphize.hpp"
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <sstream>
#include <cctype>
#include <algorithm>

namespace xphage::generics {

namespace {

// Deep-clones an AST subtree. Needed because ASTNode's default copy
// constructor only shallow-copies `children` (a vector of
// shared_ptr<ASTNode> — copying the vector copies the pointers, not
// the pointees), so two specializations of the same generic
// declaration would otherwise share the same child nodes and any
// mutation made to one (the type-substitution walk below) would
// silently corrupt the other.
ASTNodePtr deep_clone(const ASTNodePtr& n) {
    if (!n) return nullptr;
    auto copy = std::make_shared<ASTNode>(*n); // shallow copy of scalar fields + child pointers
    copy->children.clear();
    for (auto& c : n->children) copy->children.push_back(deep_clone(c));
    return copy;
}

// Replaces every occurrence of a type-parameter name with its
// concrete substitution, wherever a type name can appear: a
// TypeAnnot/FieldDecl's `extra` (parameter/field type), a PulseDecl's
// `extra2` (return type), and — for a qualified/generic type
// reference like "Vec<T>" — as a substring match within a larger type
// string, not just an exact match, since parse_type_name() builds
// composite type strings by string concatenation (see its own
// comment). A whole-word substring replace (not literal
// find-and-replace of any substring) avoids mangling an unrelated
// type that merely contains the parameter name as a substring (e.g.
// substituting "T" must not corrupt "Team" or "TreeNode").
std::string substitute_type_string(const std::string& type_str,
                                    const std::unordered_map<std::string, std::string>& subst) {
    if (subst.empty() || type_str.empty()) return type_str;
    std::string result;
    result.reserve(type_str.size());
    size_t i = 0;
    auto is_ident_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    while (i < type_str.size()) {
        if (is_ident_char(type_str[i]) &&
            (i == 0 || !is_ident_char(type_str[i - 1]))) {
            size_t start = i;
            while (i < type_str.size() && is_ident_char(type_str[i])) i++;
            std::string word = type_str.substr(start, i - start);
            auto it = subst.find(word);
            result += (it != subst.end()) ? it->second : word;
        } else {
            result += type_str[i];
            i++;
        }
    }
    return result;
}

// Walks an entire cloned subtree, substituting type-parameter names
// in every field that can hold a type string (FieldDecl/TypeAnnot's
// extra, PulseDecl's extra2, EnumVariant payload types, ImplDecl's
// extra for a generic impl's receiver type — though generic impl
// itself isn't attempted in this pass, see the header's own note on
// scope). Does NOT touch `value` (names/identifiers/operators are
// never type strings in this AST's convention) except for the one
// deliberate case of a SpawnExpr's `value` (the constructed type's
// name, e.g. "Box" or "Box<T>" for a not-yet-specialized generic
// spawn) and a CallExpr whose callee is a generic function's own
// name — both handled separately at the call/construction-site
// rewrite step below, not by this generic walk, since substituting
// those correctly requires knowing which concrete specialization was
// selected, not just which type-parameter names exist.
void substitute_types_in_subtree(const ASTNodePtr& n,
                                  const std::unordered_map<std::string, std::string>& subst) {
    if (!n) return;
    if (!n->extra.empty())  n->extra  = substitute_type_string(n->extra, subst);
    if (!n->extra2.empty()) n->extra2 = substitute_type_string(n->extra2, subst);
    // `spawn Box<T> { ... }` keeps its type in `value`, not `extra`. Left
    // unsubstituted, concretize_subtree later instantiated the bogus
    // specialization Box<T> (-> Box__T, fields of type `T`), and any method of
    // a generic type that built a value of its own type — `-> Self`, `spawn
    // Box<T> {..}`, i.e. every builder/clone/map-style method — failed to
    // compile with "'T' does not name a type".
    if (n->kind == NodeKind::SpawnExpr && !n->value.empty())
        n->value = substitute_type_string(n->value, subst);
    // `T::default()` inside a generic body: the head segment of the path is a
    // type parameter and becomes the concrete type (`int::default`,
    // `Box<int>::new` — the latter turned into its mangled name by
    // concretize_subtree afterwards).
    if (n->kind == NodeKind::PathExpr) {
        size_t cc = n->value.find("::");
        if (cc != std::string::npos) {
            auto it = subst.find(n->value.substr(0, cc));
            if (it != subst.end()) n->value = it->second + n->value.substr(cc);
        }
    }
    for (auto& c : n->children) substitute_types_in_subtree(c, subst);
}

// One generic declaration's essential info, collected up front.
struct GenericDecl {
    ASTNodePtr           node;         // the original PulseDecl/ForgeDecl/EnumDecl
    std::vector<std::string> param_names; // e.g. ["T"] or ["T", "E"]
};

// A specific instantiation already generated, keyed by (generic name,
// concrete type-argument tuple joined with "|") so the same
// specialization is never generated twice even if called from many
// sites — e.g. identity(1) and identity(2) both want T=int and should
// share one identity__int, not get two separately-generated (but
// identical) copies.
std::string mangle_name(const std::string& base, const std::vector<std::string>& type_args) {
    std::string m = base;
    for (auto& t : type_args) {
        m += "__";
        for (char c : t) m += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return m;
}

// Infers concrete type arguments for a call/construction site,
// mirroring (deliberately kept in sync with, though not literally
// shared code with — see the header's design-rationale comment on
// why monomorphization is a separate pass rather than folded into
// semantic analysis) SemanticAnalyzer::check_call's own inference:
// walks each parameter position that references a type-parameter
// name, and for a CallExpr pulls the corresponding argument's
// concrete type from a caller-supplied per-node type map (populated
// during a lightweight local re-inference here, since this pass runs
// independently of the semantic analyzer's own internal state).
// A numeric type name: int/float and the sized ones (numeric_types.hpp).
static bool is_numeric_name(const std::string& t) {
    return t == "int" || t == "float" || t == "i8" || t == "i16" || t == "i32" || t == "isize" ||
           t == "u8" || t == "u16" || t == "u32" || t == "u64" || t == "usize" || t == "f32";
}

std::string infer_arg_type(const ASTNodePtr& arg) {
    if (!arg) return "";
    switch (arg->kind) {
        case NodeKind::IntLit:
        case NodeKind::FloatLit: {
            auto sfx = arg->attrs.find("suffix");
            if (sfx != arg->attrs.end())
                return sfx->second == "i64" ? "int" : sfx->second == "f64" ? "float" : sfx->second;
            return arg->kind == NodeKind::IntLit ? "int" : "float";
        }
        case NodeKind::BoolLit:  return "bool";
        case NodeKind::StringLit:
        case NodeKind::FStringLit: return "str";
        case NodeKind::SpawnExpr: return arg->value;
        // `-1`, `-x`: a unary sign keeps its operand's numeric type, and
        // `!b` is a bool. Without this `option_unwrap_or(o, -1)` could not
        // infer T from the (very ordinary) negative literal and the generic
        // call was left unspecialized.
        case NodeKind::UnaryOp: {
            if (arg->children.empty()) return "";
            if (arg->value == "!" || arg->value == "not") return "bool";
            if (arg->value == "-" || arg->value == "+") {
                std::string t = infer_arg_type(arg->children[0]);
                return is_numeric_name(t) ? t : std::string();
            }
            return "";
        }
        // Arithmetic on operands of known type, comparisons and logic.
        case NodeKind::BinaryOp: {
            const std::string& op = arg->value;
            if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" ||
                op == "&&" || op == "||" || op == "and" || op == "or") return "bool";
            if (arg->children.size() < 2) return "";
            if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
                std::string l = infer_arg_type(arg->children[0]);
                std::string r = infer_arg_type(arg->children[1]);
                if (l.empty() || r.empty()) return "";
                if (l == r && (is_numeric_name(l) || (l == "str" && op == "+"))) return l;
                if ((l == "int" && r == "float") || (l == "float" && r == "int")) return "float";
            }
            return "";
        }
        default: return ""; // identifiers and anything else: not
                             // reliably inferable without a real
                             // symbol table here; a generic call using
                             // a variable of unannotated type simply
                             // won't be monomorphized (see the
                             // fallback behavior in monomorphize()
                             // below, which leaves such a call
                             // untouched rather than guessing).
    }
}

} // namespace

MonomorphizeResult monomorphize(const Program& prog, const Program& library_decls) {
    MonomorphizeResult result;

    // Pass 1: collect every generic declaration (has at least one
    // TypeParamDecl child), from both the program itself and any
    // resolved stdlib declarations passed alongside it. Also collect
    // every (non-generic-declaration-site) PulseDecl's own declared
    // return type by name — needed so `atom found = find_user(42)`
    // followed by `probe found { diverge Option.Some(user) -> ... }`
    // can resolve found's type back to find_user's return type, since
    // the probe's subject is just a plain identifier with no type
    // annotation of its own anywhere nearby.
    std::unordered_map<std::string, GenericDecl> generics;
    std::unordered_map<std::string, std::string> function_return_types; // function name -> declared return type string
    // function name -> declared parameter type strings, in order. Used to give
    // each call ARGUMENT the type its parameter expects as context, so a
    // generic enum construction passed straight in — `show(Result.Ok(5))` —
    // can resolve its type arguments from the callee's signature. Only
    // recorded for non-generic pulses: a generic callee's parameter types
    // mention type parameters and say nothing concrete.
    std::unordered_map<std::string, std::vector<std::string>> function_param_types;
    // Generic type's base name -> its impl<T> block(s) (usually one,
    // but nothing prevents `impl<T> Box<T> { ... }` appearing more
    // than once in different files/sections the way an ordinary
    // non-generic impl can). Each ImplDecl node here still has its
    // own TypeParamDecl children (not stripped) and still names its
    // receiver as the generic form ("Box<T>", not yet substituted) —
    // substitution happens per-specialization in
    // instantiate_generic_type, the same place the forge/enum's own
    // fields get substituted.
    std::unordered_map<std::string, std::vector<ASTNodePtr>> generic_impls;
    // Set if any type string anywhere in the program (or library_decls)
    // looks like a builtin-collection instantiation ("Vec<...>",
    // "Map<...>", "Set<...>") — checked cheaply (prefix match, no
    // parsing) during the same collect() walk that gathers user
    // generics below. Needed because the early-return just below this
    // walk ("nothing generic anywhere, skip Pass 2 entirely") predates
    // Vec/Map/Set support and otherwise short-circuits past the
    // rewrite walk (and therefore past instantiate_generic_type's
    // Vec/Map/Set interception) whenever a program uses a builtin
    // collection but declares no generic pulse/forge/enum of its own —
    // exactly the common case for a program that only ever spawns a
    // Vec<T>.
    bool has_builtin_collections = false;
    auto looks_like_builtin_collection = [](const std::string& s) {
        return s.compare(0, 4, "Vec<") == 0 ||
               s.compare(0, 4, "Map<") == 0 ||
               s.compare(0, 4, "Set<") == 0;
    };
    std::function<void(const ASTNodePtr&)> collect;
    collect = [&](const ASTNodePtr& n) {
        if (!n) return;
        if (!has_builtin_collections) {
            if (looks_like_builtin_collection(n->value) ||
                looks_like_builtin_collection(n->extra) ||
                looks_like_builtin_collection(n->extra2)) {
                has_builtin_collections = true;
            }
        }
        if (n->kind == NodeKind::PulseDecl || n->kind == NodeKind::AsyncPulseDecl ||
            n->kind == NodeKind::ForgeDecl || n->kind == NodeKind::EnumDecl) {
            std::vector<std::string> params;
            for (auto& c : n->children) {
                if (c && c->kind == NodeKind::TypeParamDecl) params.push_back(c->value);
            }
            if (!params.empty() && !n->value.empty()) {
                generics[n->value] = GenericDecl{n, std::move(params)};
            }
            if ((n->kind == NodeKind::PulseDecl || n->kind == NodeKind::AsyncPulseDecl) &&
                !n->value.empty() && !n->extra2.empty()) {
                function_return_types[n->value] = n->extra2;
            }
            if ((n->kind == NodeKind::PulseDecl || n->kind == NodeKind::AsyncPulseDecl) &&
                !n->value.empty() && params.empty()) {
                std::vector<std::string> pts;
                for (auto& pc : n->children)
                    if (pc && pc->kind == NodeKind::FieldDecl) pts.push_back(pc->extra);
                function_param_types[n->value] = std::move(pts);
            }
        }
        if (n->kind == NodeKind::ImplDecl && !n->extra.empty()) {
            bool has_type_params = false;
            for (auto& c : n->children) if (c && c->kind == NodeKind::TypeParamDecl) has_type_params = true;
            if (has_type_params) {
                size_t lt = n->extra.find('<');
                std::string base = (lt == std::string::npos) ? n->extra : n->extra.substr(0, lt);
                generic_impls[base].push_back(n);
            }
        }
        for (auto& c : n->children) collect(c);
    };
    for (auto& n : prog) collect(n);
    for (auto& n : library_decls) collect(n);

    if (generics.empty() && !has_builtin_collections) {
        // Nothing generic anywhere, and no builtin-collection use
        // either — return the program unchanged (still deep-cloned,
        // so the caller owns an independent copy
        // consistent with what this pass does when it DOES rewrite
        // something, rather than a mix of "sometimes clones,
        // sometimes doesn't" depending on program content).
        for (auto& n : prog) result.program.push_back(deep_clone(n));
        return result;
    }

    // Pass 2: walk the whole program (deep-cloned, so rewriting is
    // safe), find every call/construction site referencing a generic
    // declaration, infer its concrete type arguments, and generate
    // (once per distinct name+type-argument combination) a
    // specialization — recording it for insertion after the
    // rewrite walk finishes, and rewriting the call/construction site
    // in place to reference the specialization's mangled name.
    std::unordered_map<std::string, ASTNodePtr> specializations; // mangled name -> cloned+substituted decl
    std::unordered_set<std::string> generic_names_used;          // original generic names that got >=1 real instantiation
    // Variable name -> its declared/inferred concrete type, tracked
    // as the rewrite walk encounters each atom/shadow declaration (in
    // program order — this is a simple linear scan, not real scoping,
    // so a variable shadowed in a nested block with a different type
    // isn't modeled correctly; every current test case is flat enough
    // that this doesn't matter, and getting it wrong just means a
    // probe pattern fails to resolve, which surfaces as a normal
    // downstream error rather than silently generating wrong code).
    // Needed so `atom found = find_user(42)` followed later by `probe
    // found { diverge Option.Some(user) -> ... }` can resolve found's
    // type back through find_user's own (already-instantiated, via
    // function_return_types) return type.
    std::unordered_map<std::string, std::string> var_types;
    // Specialized function name (e.g. "unbox__Box_int_") -> its concrete,
    // already-instantiated return type (e.g. "Box__int"). Filled in when a
    // generic pulse specialization is created; consulted when typing
    // `atom x = <call>` so chained generic calls can infer through it.
    std::unordered_map<std::string, std::string> specialized_return_types;

    // Mangled specialization name -> (generic base name, ordered
    // concrete type arguments). mangle_name's flat "Box__int" form is
    // ambiguous to split back apart, so instead of parsing it, every
    // specialization records the exact arguments it was built from
    // here. Needed to unify a generic parameter type like `Box<T>`
    // against an argument variable whose tracked type is the mangled
    // `Box__int` (see the CallExpr inference below), and pull out T=int.
    struct SpecInfo {
        std::string base;
        std::vector<std::string> args;
    };
    std::unordered_map<std::string, SpecInfo> mangled_type_args;

    // Splits the top-level, comma-separated arguments of a type
    // string's angle brackets, honouring nested brackets: "Map<K, Vec<V>>"
    // -> {"K", "Vec<V>"}. Returns empty (and leaves `base` as the whole
    // string) when the string has no angle brackets.
    auto split_type_string = [](const std::string& s, std::string& base) {
        std::vector<std::string> out;
        size_t lt = s.find('<');
        if (lt == std::string::npos || s.empty() || s.back() != '>') { base = s; return out; }
        base = s.substr(0, lt);
        std::string args = s.substr(lt + 1, s.size() - lt - 2);
        std::string cur;
        int depth = 0;
        for (char c : args) {
            if (c == '<') depth++;
            if (c == '>') depth--;
            if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); }
            else if (c != ' ') cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };

    // Unifies a generic parameter's declared type pattern against a
    // concrete type, binding type parameters into `subst`:
    //   pattern "T"        vs "int"        -> T = int
    //   pattern "Box<T>"   vs "Box__int"   -> T = int  (via mangled_type_args)
    //   pattern "Vec<T>"   vs "Vec<int>"   -> T = int  (builtin bracket form)
    //   pattern "Map<K,V>" vs "Map<int, str>" -> K = int, V = str
    // Returns false on a genuine mismatch (different base, different
    // arity, or a parameter already bound to a conflicting type), so a
    // caller can leave the call unspecialized rather than guess.
    std::function<bool(const std::string&, const std::string&,
                       const std::vector<std::string>&,
                       std::unordered_map<std::string, std::string>&)> unify_type;
    unify_type = [&](const std::string& pattern, const std::string& concrete,
                     const std::vector<std::string>& param_names,
                     std::unordered_map<std::string, std::string>& subst) -> bool {
        // Bare type parameter: bind it (or check consistency).
        for (auto& pn : param_names) {
            if (pn == pattern) {
                auto it = subst.find(pn);
                if (it == subst.end()) { subst[pn] = concrete; return true; }
                return it->second == concrete;
            }
        }
        // Not a type parameter and no angle brackets: a plain type
        // name, which must match exactly.
        std::string pbase;
        auto pargs = split_type_string(pattern, pbase);
        if (pargs.empty()) return pattern == concrete;

        // Pattern is a generic instantiation like Box<T>. The concrete
        // side is either already-mangled (Box__int, recorded in
        // mangled_type_args) or still in bracket form (Vec<int>).
        std::vector<std::string> cargs;
        auto mit = mangled_type_args.find(concrete);
        if (mit != mangled_type_args.end()) {
            if (mit->second.base != pbase) return false;
            cargs = mit->second.args;
        } else {
            std::string cbase;
            cargs = split_type_string(concrete, cbase);
            if (cargs.empty() || cbase != pbase) return false;
        }
        if (cargs.size() != pargs.size()) return false;
        for (size_t i = 0; i < pargs.size(); i++) {
            if (!unify_type(pargs[i], cargs[i], param_names, subst)) return false;
        }
        return true;
    };

    // Given a type string that might be a generic instantiation
    // (e.g. "Option<User>", "Result<Config, str>"), and assuming its
    // base name IS a known generic ForgeDecl/EnumDecl, generates (if
    // not already generated) the corresponding specialization and
    // returns its mangled name — or, if type_str isn't a generic
    // instantiation of a known generic type at all, returns type_str
    // unchanged. This is the single shared entry point for every
    // place a concrete generic type name can appear outside of a
    // call/spawn expression: a function's declared return type
    // (PulseDecl.extra2), a parameter's declared type
    // (FieldDecl.extra), or an explicit local annotation
    // (AtomDecl/ShadowDecl.extra) — e.g. `atom found: Option<User> =
    // find_user(42)`. Reusable rather than duplicating the same
    // parse-args/build-subst/clone-and-substitute logic the
    // SpawnExpr case above already does inline, since that logic is
    // identical regardless of which AST field the type string came
    // from.
    std::function<std::string(const std::string&)> instantiate_generic_type;
    // Turns every now-concrete generic type string inside an
    // already-substituted specialization (Box<int>, Box<Box<int>>,
    // Vec<Box<int>> ...) into its real mangled specialization, by
    // running instantiate_generic_type over each type-bearing field
    // and each `spawn Box<...>` site. Shared by BOTH specialization
    // paths (generic types and generic functions): substitution alone
    // only rewrites text, so without this step a nested argument like
    // the inner Box<int> of Box<Box<int>> would never get its own
    // struct, and the outer field would name a type nobody emitted.
    std::function<void(const ASTNodePtr&)> concretize_subtree;
    instantiate_generic_type = [&](const std::string& type_str) -> std::string {
        size_t lt = type_str.find('<');
        if (lt == std::string::npos || type_str.back() != '>') return type_str;
        std::string base = type_str.substr(0, lt);
        std::string args_str = type_str.substr(lt + 1, type_str.size() - lt - 2);
        std::vector<std::string> type_args;
        {
            std::string cur;
            int depth = 0;
            for (char c : args_str) {
                if (c == '<') depth++;
                if (c == '>') depth--;
                if (c == ',' && depth == 0) { type_args.push_back(cur); cur.clear(); }
                else if (c != ' ') cur += c;
            }
            if (!cur.empty()) type_args.push_back(cur);
        }
        // Builtin Vec<T>/Map<K,V>/Set<T> collections: checked first so a
        // genuine builtin-collection use is intercepted before the
        // user-generics lookup below, but only when no user ForgeDecl/
        // EnumDecl by that name exists in `generics` — this never shadows
        // a real user generic that happens to be named the same. No
        // specialization AST node is generated here (unlike the ordinary
        // path below): the C++ emitters synthesize the concrete container
        // type directly from this type string whenever they encounter it
        // (see xil_cpp_type / ASTTranspiler::cpp_type).
        //
        // Deliberately NOT run through mangle_name's flat "Base__arg__arg"
        // scheme the way an ordinary user generic specialization is:
        // mangle_name's "__" joining is only unambiguous to *generate*
        // (it never needs to be split back apart, since a real
        // specialization AST node carries its own already-substituted
        // fields) — for an ordinary generic that's fine, because the
        // mangled name only ever needs to match a genuinely-declared
        // struct of that exact name. Vec/Map/Set have no such
        // declaration to match: the C++ emitters reconstruct
        // std::vector<...>/std::map<...>/std::set<...> directly from
        // this type string wherever it's later used as a register/
        // field/return type, and "Vec__Option__User" is genuinely
        // ambiguous to split back into element types (it's
        // indistinguishable from what "Vec<Option__User>" flattening
        // would also produce). So the bracket structure is kept intact
        // — "Vec<int>", "Map<int, str>", "Vec<Option__User>" (element
        // types that ARE real user generics still get mangled
        // recursively via instantiate_generic_type below, since those
        // DO have a real declaration to match; only the outer Vec/Map/
        // Set wrapper itself skips flat mangling) — so every later
        // consumer can recover the element type(s) with a simple
        // bracket-depth-aware split instead of needing a separate
        // side-table threaded through the whole pipeline.
        if ((base == "Vec" || base == "Map" || base == "Set") &&
            generics.find(base) == generics.end()) {
            std::vector<std::string> resolved_args;
            resolved_args.reserve(type_args.size());
            for (auto& ta : type_args) resolved_args.push_back(instantiate_generic_type(ta));
            std::string out = base + "<";
            for (size_t i = 0; i < resolved_args.size(); i++) {
                if (i) out += ", ";
                out += resolved_args[i];
            }
            out += ">";
            generic_names_used.insert(base);
            return out;
        }
        auto git = generics.find(base);
        if (git == generics.end() ||
            (git->second.node->kind != NodeKind::ForgeDecl &&
             git->second.node->kind != NodeKind::EnumDecl)) {
            return type_str; // not a generic type name this pass knows about
        }
        auto& gd = git->second;
        if (type_args.size() != gd.param_names.size()) return type_str; // arity mismatch — leave as-is, will fail downstream with a clearer error than guessing
        std::unordered_map<std::string, std::string> subst;
        for (size_t i = 0; i < gd.param_names.size(); i++) subst[gd.param_names[i]] = type_args[i];
        std::string mangled = mangle_name(base, type_args);
        mangled_type_args[mangled] = SpecInfo{base, type_args};
        if (!specializations.count(mangled)) {
            auto spec = deep_clone(gd.node);
            spec->value = mangled;
            std::vector<ASTNodePtr> kept;
            for (auto& c : spec->children) {
                if (!c || c->kind != NodeKind::TypeParamDecl) kept.push_back(c);
            }
            spec->children = std::move(kept);
            substitute_types_in_subtree(spec, subst);
            // Register BEFORE concretizing so a self-referential type
            // (forge Node<T> { next: Node<T> }) finds its own entry and
            // terminates instead of recursing forever.
            specializations[mangled] = spec;
            concretize_subtree(spec);

            // If this generic type has associated impl<T> block(s)
            // (impl<T> Box<T> { ... }), generate a matching concrete
            // impl for this same specialization: clone each one,
            // substitute T -> the concrete type throughout (method
            // bodies included — a method like `get(self) -> T` needs
            // its own return type substituted the same way the
            // forge/enum's own fields were above), and rename its
            // receiver (`.extra`) to the mangled specialization name
            // so emit_forge's impl_methods_ lookup (keyed by the
            // forge's own, now-mangled, name) finds it. Without this,
            // a generic type's methods would never be instantiated at
            // all — only its fields would be, since ForgeDecl and
            // ImplDecl are entirely separate AST nodes with no
            // structural link between them beyond sharing a type
            // name.
            auto git_impls = generic_impls.find(base);
            if (git_impls != generic_impls.end()) {
                for (auto& impl_node : git_impls->second) {
                    std::string impl_key = mangled + "::impl:" +
                        std::to_string(reinterpret_cast<uintptr_t>(impl_node.get()));
                    if (specializations.count(impl_key)) continue;
                    auto impl_spec = deep_clone(impl_node);
                    impl_spec->extra = mangled;
                    std::vector<ASTNodePtr> ikept;
                    for (auto& c : impl_spec->children) {
                        if (!c || c->kind != NodeKind::TypeParamDecl) ikept.push_back(c);
                    }
                    impl_spec->children = std::move(ikept);
                    substitute_types_in_subtree(impl_spec, subst);
                    specializations[impl_key] = impl_spec;
                    concretize_subtree(impl_spec);
                }
            }
        }
        generic_names_used.insert(base);
        return mangled;
    };

    concretize_subtree = [&](const ASTNodePtr& x) {
        if (!x) return;
        if (!x->extra.empty())  x->extra  = instantiate_generic_type(x->extra);
        if (!x->extra2.empty()) x->extra2 = instantiate_generic_type(x->extra2);
        if (x->kind == NodeKind::SpawnExpr && !x->value.empty()) {
            x->value = instantiate_generic_type(x->value);
        }
        if (x->kind == NodeKind::PathExpr) {
            // "Box<int>::new" -> "Box__int::new": only the type head is a type.
            size_t cc = x->value.rfind("::");
            if (cc != std::string::npos && x->value.find('<') != std::string::npos &&
                x->value.find('<') < cc)
                x->value = instantiate_generic_type(x->value.substr(0, cc)) + x->value.substr(cc);
        }
        for (auto& ch : x->children) concretize_subtree(ch);
    };

    std::function<void(ASTNodePtr&, const std::string&)> rewrite;
    rewrite = [&](ASTNodePtr& n, const std::string& enclosing_return_type) {
        if (!n) return;

        // A function body's own enclosing return-type context is set
        // once here (after this node's own extra2 has possibly been
        // instantiated to a concrete mangled type below) and threaded
        // down through every descendant — this is what lets a bare
        // `return Option.Some(user)` inside a `pulse find_user(id:
        // int) -> Option<User>` resolve which Option<X> specialization
        // to construct, since the construction site itself never
        // names a type argument at all (only "Option.Some", not
        // "Option<User>.Some").
        std::string next_context = enclosing_return_type;
        if (n->kind == NodeKind::PulseDecl || n->kind == NodeKind::AsyncPulseDecl) {
            if (!n->extra2.empty()) {
                n->extra2 = instantiate_generic_type(n->extra2);
                next_context = n->extra2;
            } else {
                next_context = "";
            }
        }
        // Variant construction, resolved TOP-DOWN so type information can
        // flow INTO the arguments. `Result.Ok(Option.None)` inside a
        // function returning Result<Option<int>, str> must make the inner
        // `Option.None` an Option<int>: the only signal is that Ok's
        // payload type, in the already-specialized Result, is Option__int.
        // Children are rewritten before their parent's own rewrite runs
        // (below), so by then it would be too late; instead work out here,
        // before descending, (a) which specialization this construction is
        // and (b) the payload type each argument is expected to have, and
        // pass (b) down as that argument's context.
        std::vector<std::string> arg_contexts;   // per call argument; empty = inherit
        // Ordinary call to a known non-generic pulse: each argument's
        // context is the type its parameter declares. (A variant
        // construction, handled just below, sets the same table from the
        // enum's payload types instead.)
        if (n->kind == NodeKind::CallExpr && !n->children.empty() && n->children[0] &&
            n->children[0]->kind == NodeKind::Identifier) {
            auto fpt = function_param_types.find(n->children[0]->value);
            if (fpt != function_param_types.end() && !generics.count(n->children[0]->value)) {
                for (auto pty : fpt->second) {
                    for (;;) {
                        if (pty.compare(0, 4, "own ") == 0)          pty = pty.substr(4);
                        else if (pty.compare(0, 4, "ref ") == 0)     pty = pty.substr(4);
                        else if (pty.compare(0, 8, "mut_ref ") == 0) pty = pty.substr(8);
                        else break;
                    }
                    arg_contexts.push_back(pty.empty() || pty == "auto" ? std::string()
                                                                         : instantiate_generic_type(pty));
                }
            }
        }
        if (n->kind == NodeKind::CallExpr && !n->children.empty() && n->children[0] &&
            n->children[0]->kind == NodeKind::MemberExpr &&
            !n->children[0]->children.empty() && n->children[0]->children[0] &&
            n->children[0]->children[0]->kind == NodeKind::Identifier &&
            !next_context.empty()) {
            const std::string enum_name = n->children[0]->children[0]->value;
            auto egit = generics.find(enum_name);
            const std::string vprefix = enum_name + "__";
            if (egit != generics.end() && egit->second.node->kind == NodeKind::EnumDecl &&
                next_context.compare(0, vprefix.size(), vprefix) == 0) {
                auto sit = specializations.find(next_context);
                if (sit != specializations.end()) {
                    for (auto& v : sit->second->children) {
                        if (!v || v->kind != NodeKind::EnumVariant) continue;
                        if (v->value != n->children[0]->value) continue;
                        for (auto& pt : v->children) {
                            arg_contexts.push_back(pt ? pt->extra : std::string());
                        }
                        break;
                    }
                }
            }
        }
        // probe/diverge bindings, registered TOP-DOWN for the same reason:
        // `diverge Result.Ok(o) -> { probe o { ... } }` needs `o` to be
        // known as an Option<int> BEFORE the arm body is rewritten, or the
        // inner probe cannot resolve which Option specialization it is
        // matching. `o` gets the payload type of variant `Ok` in the
        // subject's (already-specialized) enum. Only a plain identifier
        // subject is handled, matching the existing pattern-name rewrite
        // further down. Bindings are removed again after the arm so a name
        // reused in a sibling arm or later code cannot inherit a stale
        // type.
        std::vector<std::pair<std::string, std::pair<bool, std::string>>> saved_bindings;

        // Pulse PARAMETERS are typed variables too. var_types used to be
        // filled only from atom/shadow declarations, so `pulse show(r:
        // Result<int, str>) { probe r { ... } }` never resolved which
        // Result specialization `r` is: the probe patterns stayed as the
        // bare generic name `Result.Ok` and the C++ emitter fell back to
        // string comparison (`r == "Result.Ok"`, a compile error). The same
        // gap hid a parameter's type from generic-call inference. Register
        // them before the body is walked (parameters precede the body in
        // the child list) and put the previous entries back afterwards, so
        // a parameter name never leaks into an unrelated later function.
        // A generic TEMPLATE is never walked (see is_generic_template), so
        // only concrete signatures reach here.
        if (n->kind == NodeKind::PulseDecl || n->kind == NodeKind::AsyncPulseDecl) {
            for (auto& pc : n->children) {
                if (!pc || pc->kind != NodeKind::FieldDecl || pc->value.empty() || pc->extra.empty()) continue;
                std::string pty = pc->extra;
                for (;;) {   // ownership qualifiers change passing, not the type
                    if (pty.compare(0, 4, "own ") == 0)          pty = pty.substr(4);
                    else if (pty.compare(0, 4, "ref ") == 0)     pty = pty.substr(4);
                    else if (pty.compare(0, 8, "mut_ref ") == 0) pty = pty.substr(8);
                    else break;
                }
                pty = instantiate_generic_type(pty);
                if (pty.empty() || pty == "auto") continue;
                auto prev = var_types.find(pc->value);
                saved_bindings.push_back({pc->value,
                    {prev != var_types.end(), prev != var_types.end() ? prev->second : std::string()}});
                var_types[pc->value] = pty;
            }
        }

        if (n->kind == NodeKind::ProbeStmt && !n->children.empty() && n->children[0] &&
            n->children[0]->kind == NodeKind::Identifier) {
            auto svit = var_types.find(n->children[0]->value);
            if (svit != var_types.end()) {
                auto sspec = specializations.find(svit->second);
                if (sspec != specializations.end()) {
                    for (size_t ai = 1; ai < n->children.size(); ai++) {
                        auto& arm = n->children[ai];
                        if (!arm || arm->kind != NodeKind::ProbeArm) continue;
                        auto bit = arm->attrs.find("bindings");
                        if (bit == arm->attrs.end() || bit->second.empty()) continue;
                        size_t dot = arm->value.find('.');
                        if (dot == std::string::npos) continue;
                        const std::string variant_name = arm->value.substr(dot + 1);
                        const ASTNode* variant = nullptr;
                        for (auto& v : sspec->second->children) {
                            if (v && v->kind == NodeKind::EnumVariant && v->value == variant_name) {
                                variant = v.get();
                                break;
                            }
                        }
                        if (!variant) continue;
                        // bindings are comma-joined in payload order
                        size_t start = 0, idx = 0;
                        const std::string& joined = bit->second;
                        while (start <= joined.size()) {
                            size_t comma = joined.find(',', start);
                            std::string name = joined.substr(
                                start, comma == std::string::npos ? std::string::npos : comma - start);
                            if (!name.empty() && idx < variant->children.size() &&
                                variant->children[idx] && !variant->children[idx]->extra.empty()) {
                                auto prev = var_types.find(name);
                                saved_bindings.push_back({name,
                                    {prev != var_types.end(),
                                     prev != var_types.end() ? prev->second : std::string()}});
                                var_types[name] = variant->children[idx]->extra;
                            }
                            idx++;
                            if (comma == std::string::npos) break;
                            start = comma + 1;
                        }
                    }
                }
            }
        }
        // `atom x: Result<int, str> = Result.Ok(8)` — an explicit annotation is
        // the strongest hint there is for what the initializer must be, so it
        // is the context for the initializer's variant construction. Before,
        // only `return`, call arguments and enum payloads supplied context, so
        // the most direct way to write a typed binding left `Result.Ok(8)`
        // with its type arguments unresolved.
        std::string init_ctx;
        if ((n->kind == NodeKind::AtomDecl || n->kind == NodeKind::ShadowDecl) &&
            !n->extra.empty() && n->extra != "auto") {
            std::string ann = n->extra;
            for (;;) {
                if (ann.compare(0, 4, "own ") == 0)          ann = ann.substr(4);
                else if (ann.compare(0, 4, "ref ") == 0)     ann = ann.substr(4);
                else if (ann.compare(0, 8, "mut_ref ") == 0) ann = ann.substr(8);
                else break;
            }
            init_ctx = instantiate_generic_type(ann);
        }
        for (size_t ci = 0; ci < n->children.size(); ci++) {
            auto& c = n->children[ci];
            // children[0] is the callee; argument i is children[i + 1].
            std::string ctx = next_context;
            if (ci == 0 && !init_ctx.empty()) ctx = init_ctx;
            if (!arg_contexts.empty() && ci >= 1 && (ci - 1) < arg_contexts.size() &&
                !arg_contexts[ci - 1].empty()) {
                ctx = arg_contexts[ci - 1];
            }
            rewrite(c, ctx);
        }
        for (auto& sb : saved_bindings) {
            if (sb.second.first) var_types[sb.first] = sb.second.second;
            else                 var_types.erase(sb.first);
        }

        if (n->kind == NodeKind::FieldDecl || n->kind == NodeKind::AtomDecl ||
            n->kind == NodeKind::ShadowDecl) {
            if (!n->extra.empty()) n->extra = instantiate_generic_type(n->extra);
        }

        // Track this variable's concrete type, for a later ProbeStmt
        // (or anything else that needs "what type is this identifier"
        // — currently only ProbeStmt uses var_types) to resolve
        // against. Explicit annotation wins if present (already
        // instantiated above); otherwise, if the initializer is a
        // direct call to a function whose return type is known
        // (already instantiated, via function_return_types — note
        // this map holds the *original* declared return type string,
        // not retroactively updated when that function's own extra2
        // gets rewritten earlier in this same walk, since collection
        // happens once up front in Pass 1 — but since
        // instantiate_generic_type is idempotent on an
        // already-mangled string, and idempotent-no-op on any
        // non-generic type string, looking the original declared
        // return type up here and re-running it through
        // instantiate_generic_type produces the correct mangled name
        // either way).
        if ((n->kind == NodeKind::AtomDecl || n->kind == NodeKind::ShadowDecl) && !n->value.empty()) {
            std::string vtype;
            if (!n->extra.empty()) {
                vtype = n->extra;
            } else if (!n->children.empty() && n->children[0] &&
                       n->children[0]->kind == NodeKind::CallExpr &&
                       !n->children[0]->children.empty() && n->children[0]->children[0] &&
                       n->children[0]->children[0]->kind == NodeKind::Identifier) {
                // Look up by the call's ORIGINAL callee name if it's
                // already been rewritten to a monomorphized function
                // name (e.g. "identity__int") — function_return_types
                // is keyed by the original generic name, so try that
                // first via a reverse mangle-strip; simplest robust
                // approach is to just check both the current (possibly
                // already-rewritten) callee name and, if that misses,
                // fall through with no type (rather than trying to
                // unmangle).
                const std::string& callee = n->children[0]->children[0]->value;
                auto srt = specialized_return_types.find(callee);
                if (srt != specialized_return_types.end()) {
                    // Callee was already rewritten to a specialization
                    // (children are walked before the enclosing atom):
                    // its concrete return type is known exactly.
                    vtype = srt->second;
                } else {
                    auto frt = function_return_types.find(callee);
                    if (frt != function_return_types.end()) {
                        vtype = instantiate_generic_type(frt->second);
                    }
                }
            }
            // No annotation and not a call to a known function: take the
            // type from the initializer itself (`atom d = spawn Dog {}`,
            // `atom n = 5`). Children are walked before this point, so a
            // generic `spawn Box<int> {}` has already been rewritten to
            // its specialization name. Without this, passing `d` to a
            // generic pulse (`describe(d)` with `describe<T: Animal>`)
            // left T uninferrable and the call silently unspecialized.
            if (vtype.empty() && !n->children.empty() && n->children[0])
                vtype = infer_arg_type(n->children[0]);
            if (!vtype.empty()) var_types[n->value] = vtype;
        }

        // probe/diverge on an enum-typed subject: rewrite each arm's
        // "EnumName.Variant"/"EnumName.Variant(binding)" pattern to
        // "MangledName.Variant" the same way a construction site
        // does, using the probe's subject's tracked type (var_types,
        // populated just above) as the source of the concrete type
        // argument — a probe pattern names no type argument of its
        // own at all (see parse_probe_stmt), so this is the only
        // place that information can come from for a bare identifier
        // subject. A subject that ISN'T a plain identifier (e.g. a
        // direct `probe find_user(42) { ... }`) isn't resolved here;
        // narrower support than the fully general case, consistent
        // with the header's documented scope.
        if (n->kind == NodeKind::ProbeStmt && !n->children.empty() && n->children[0] &&
            n->children[0]->kind == NodeKind::Identifier) {
            auto vit = var_types.find(n->children[0]->value);
            if (vit != var_types.end() && !vit->second.empty()) {
                for (size_t i = 1; i < n->children.size(); i++) {
                    auto& arm = n->children[i];
                    if (!arm || arm->kind != NodeKind::ProbeArm) continue;
                    size_t dot = arm->value.find('.');
                    if (dot == std::string::npos) continue;
                    std::string pattern_enum = arm->value.substr(0, dot);
                    auto git2 = generics.find(pattern_enum);
                    if (git2 == generics.end() || git2->second.node->kind != NodeKind::EnumDecl) continue;
                    std::string prefix = pattern_enum + "__";
                    if (vit->second.compare(0, prefix.size(), prefix) != 0) continue;
                    arm->value = vit->second + arm->value.substr(dot);
                }
            }
        }

        // Generic enum-variant construction with no explicit type
        // argument at the construction site itself — `Option.Some
        // (user)` / `Option.None`, as opposed to `spawn Option<User>
        // {...}` which isn't valid syntax for an enum anyway (enums
        // construct via EnumName.Variant(...), not spawn — see the
        // enum work in an earlier session). The only type-argument
        // signal available is the enclosing function's own
        // (already-instantiated, per next_context above) return type,
        // when it names the same generic enum being constructed —
        // exactly the book's own documented `Option<T>`/`Result<T,E>`
        // usage pattern (§9.3): the concrete type is announced once,
        // in the function signature, and every `return Option.Some
        // (...)`/`return Option.None` inside that function's body
        // shares it. A construction site with no such enclosing
        // context (e.g. a generic enum constructed and immediately
        // assigned to an explicitly-annotated local — `atom x:
        // Option<User> = Option.Some(u)` — rather than returned) is
        // narrower support than the full book example needs, but was
        // not attempted here; see the header's scope note.
        auto try_rewrite_variant_construction = [&](const ASTNodePtr& member_expr,
                                                       ASTNodePtr* call_or_member) {
            if (!member_expr || member_expr->children.empty() || !member_expr->children[0] ||
                member_expr->children[0]->kind != NodeKind::Identifier) {
                return;
            }
            const std::string& enum_name = member_expr->children[0]->value;
            auto git = generics.find(enum_name);
            if (git == generics.end() || git->second.node->kind != NodeKind::EnumDecl) return;
            if (enclosing_return_type.empty()) return;
            // enclosing_return_type is already a mangled name like
            // "Option__User" once instantiated — recover the base
            // name to confirm it actually matches this construction's
            // enum before rewriting, so e.g. a Result<T,E>-returning
            // function accidentally containing a stray Option.Some
            // doesn't get silently mis-rewritten to Option's mangled
            // name just because SOME generic return type was in
            // scope.
            std::string prefix = enum_name + "__";
            if (enclosing_return_type.compare(0, prefix.size(), prefix) != 0) return;
            member_expr->children[0]->value = enclosing_return_type;
        };
        if (n->kind == NodeKind::MemberExpr) {
            try_rewrite_variant_construction(n, nullptr);
        } else if (n->kind == NodeKind::CallExpr && !n->children.empty() &&
                   n->children[0] && n->children[0]->kind == NodeKind::MemberExpr) {
            try_rewrite_variant_construction(n->children[0], &n);
        }

        // Generic function call: `identity(42)` where `identity` is
        // a known generic PulseDecl name, OR an explicit
        // `identity<int>(42)` form (not currently produced by the
        // parser — call-site explicit type arguments weren't added
        // to parse_call — so only implicit inference from arguments
        // is handled here; see the header's scope note).
        if (n->kind == NodeKind::CallExpr && !n->children.empty() &&
            n->children[0] && n->children[0]->kind == NodeKind::Identifier) {
            auto git = generics.find(n->children[0]->value);
            if (git != generics.end() &&
                (git->second.node->kind == NodeKind::PulseDecl ||
                 git->second.node->kind == NodeKind::AsyncPulseDecl)) {
                auto& gd = git->second;
                // Build the substitution map from each type-param
                // position's inferred concrete argument type.
                std::unordered_map<std::string, std::string> subst;
                std::vector<std::string> type_args;
                bool all_resolved = true;
                std::vector<const ASTNode*> gparams; // the generic decl's own FieldDecl params, in order
                for (auto& c : gd.node->children) {
                    if (c && c->kind == NodeKind::FieldDecl) gparams.push_back(c.get());
                }
                // Explicit type arguments, `f<int, str>(x)`: they ARE the
                // substitution, in declaration order — nothing is inferred.
                // Needed whenever a type parameter appears in no argument
                // (`zero<int>()`, `T::default()`), where inference has nothing
                // to look at. A wrong count is left unspecialized so the
                // downstream error names the real problem.
                bool explicit_args = false;
                {
                    auto tait = n->children[0]->attrs.find("type_args");
                    if (tait != n->children[0]->attrs.end()) {
                        std::vector<std::string> given;
                        std::string cur; int depth = 0;
                        for (char ch : tait->second) {
                            if (ch == '<') depth++;
                            else if (ch == '>') depth--;
                            if (ch == ',' && depth == 0) { given.push_back(cur); cur.clear(); }
                            else cur += ch;
                        }
                        if (!cur.empty()) given.push_back(cur);
                        if (given.size() == gd.param_names.size()) {
                            for (size_t gi = 0; gi < given.size(); gi++) {
                                std::string a = given[gi];
                                while (!a.empty() && a.front() == ' ') a.erase(a.begin());
                                subst[gd.param_names[gi]] = instantiate_generic_type(a);
                            }
                            explicit_args = true;
                        } else {
                            all_resolved = false;
                        }
                    }
                }
                for (size_t i = 0; i < gparams.size() && !explicit_args; i++) {
                    if (i + 1 >= n->children.size()) break;
                    const std::string& ptype = gparams[i]->extra;
                    // Does this parameter's declared type mention any
                    // type parameter at all (bare `T`, or nested like
                    // `Box<T>` / `Vec<T>` / `Map<K, V>`)? If not, it
                    // carries no inference information.
                    bool mentions_param = false;
                    for (auto& pn : gd.param_names) {
                        size_t pos = ptype.find(pn);
                        while (pos != std::string::npos) {
                            bool left_ok  = (pos == 0) || !std::isalnum(static_cast<unsigned char>(ptype[pos - 1]));
                            size_t end = pos + pn.size();
                            bool right_ok = (end >= ptype.size()) || !std::isalnum(static_cast<unsigned char>(ptype[end]));
                            if (left_ok && right_ok) { mentions_param = true; break; }
                            pos = ptype.find(pn, pos + 1);
                        }
                        if (mentions_param) break;
                    }
                    if (!mentions_param) continue;

                    // Concrete type of this argument: literal/simple
                    // expression inference first; for a plain
                    // identifier (previously never inferable) fall
                    // back to the variable's tracked declared type.
                    std::string concrete = infer_arg_type(n->children[i + 1]);
                    if (concrete.empty() && n->children[i + 1] &&
                        n->children[i + 1]->kind == NodeKind::Identifier) {
                        auto vit = var_types.find(n->children[i + 1]->value);
                        if (vit != var_types.end()) concrete = vit->second;
                    }
                    // An argument that is itself a call — `pass_through(mk(true))`
                    // — has the type its callee declares. A callee that was
                    // itself a generic call has already been rewritten to its
                    // specialization (children are walked first), whose
                    // concrete return type was recorded then; a plain pulse
                    // is looked up in the declared-signature table.
                    if (concrete.empty() && n->children[i + 1] &&
                        n->children[i + 1]->kind == NodeKind::CallExpr &&
                        !n->children[i + 1]->children.empty() && n->children[i + 1]->children[0] &&
                        n->children[i + 1]->children[0]->kind == NodeKind::Identifier) {
                        const std::string& acallee = n->children[i + 1]->children[0]->value;
                        auto srt = specialized_return_types.find(acallee);
                        if (srt != specialized_return_types.end()) {
                            concrete = srt->second;
                        } else if (!generics.count(acallee)) {
                            auto frt = function_return_types.find(acallee);
                            if (frt != function_return_types.end())
                                concrete = instantiate_generic_type(frt->second);
                        }
                    }
                    if (concrete.empty()) { all_resolved = false; break; }
                    if (!unify_type(ptype, concrete, gd.param_names, subst)) {
                        all_resolved = false; break;
                    }
                }
                if (all_resolved && subst.size() == gd.param_names.size()) {
                    for (auto& pn : gd.param_names) type_args.push_back(subst[pn]);
                    std::string mangled = mangle_name(gd.node->value, type_args);
                    if (!specializations.count(mangled)) {
                        auto spec = deep_clone(gd.node);
                        spec->value = mangled;
                        // Drop the TypeParamDecl children — the
                        // specialization is fully concrete now, and
                        // downstream (semantic re-registration isn't
                        // re-run, but IR lowering / codegen must not
                        // see a "generic-looking" declaration).
                        std::vector<ASTNodePtr> kept;
                        for (auto& c : spec->children) {
                            if (!c || c->kind != NodeKind::TypeParamDecl) kept.push_back(c);
                        }
                        spec->children = std::move(kept);
                        substitute_types_in_subtree(spec, subst);
                        // Substitution only rewrites text: `Box<T>` has
                        // become the literal string `Box<int>`. Turn every
                        // such now-concrete generic type string in the
                        // specialized body (parameter/return types, local
                        // annotations, and `spawn Box<...>` sites) into the
                        // real mangled specialization (`Box__int`), the
                        // same way the top-level rewrite walk does for
                        // ordinary code. Without this, the specialized
                        // function would refer to a type name that the
                        // struct emitter never produced.
                        // Register BEFORE walking the body so a recursive generic
                        // pulse (`f<T>` calling `f`) finds its own entry instead
                        // of specializing itself forever.
                        specializations[mangled] = spec;
                        concretize_subtree(spec);
                        // The specialized body is ordinary code now and needs the
                        // same full rewrite the rest of the program gets: variant
                        // constructions resolve their type arguments from the
                        // (now concrete) return type, probe patterns resolve their
                        // enum specialization, parameters register their types,
                        // and generic calls inside it specialize in turn. Before,
                        // only type STRINGS were concretized, so
                        // `return Result.Ok(v)` inside `pass_through<int>` kept
                        // the bare generic name `Result` and failed in C++.
                        // The walk registers locals in var_types as it goes;
                        // restore the map so this body's names cannot leak into
                        // the function that triggered the specialization.
                        {
                            auto saved_vars = var_types;
                            rewrite(spec, "");
                            var_types = saved_vars;
                        }
                        // Remember this specialization's concrete return
                        // type under its specialized name, so a later
                        // `atom got = unbox(outer)` can be typed even
                        // though the call site has just been rewritten
                        // from `unbox` to `unbox__Box_int_` (the
                        // function_return_types table is keyed by the
                        // ORIGINAL generic name and holds the raw `T`).
                        if (!spec->extra2.empty()) specialized_return_types[mangled] = spec->extra2;
                    }
                    generic_names_used.insert(gd.node->value);
                    n->children[0]->value = mangled;
                }
                // else: couldn't resolve every type argument (e.g. an
                // argument whose type isn't statically obvious to
                // this pass's lightweight inference) — leave the call
                // untouched. It will fail at IR lowering/codegen with
                // an "unknown function" error rather than silently
                // producing wrong code; a real diagnostic pointing
                // back at the original call site is a reasonable
                // follow-up (see the header's MonomorphizeError type,
                // currently unused by this fallback path).
            }
        }

        // Generic type construction: `spawn Box<int> { value: 5 }` —
        // explicit type argument in the SpawnExpr's own value string
        // (parse_spawn_expr now captures "Box<int>" verbatim — see
        // its own comment on this fix). This is the only place an
        // *explicit* type argument syntax already exists anywhere in
        // the grammar for a construction expression (call sites only
        // ever get inference, per the note above), so it reuses the
        // same instantiate_generic_type helper the type-annotation
        // fields above use, rather than a separately maintained copy
        // of the same "is this a generic instantiation string, and if
        // so, has it been specialized yet" logic.
        if (n->kind == NodeKind::SpawnExpr && !n->value.empty()) {
            n->value = instantiate_generic_type(n->value);
        }
    };

    // A top-level generic template (a pulse/forge/enum that declares
    // its own type parameters) must NOT be walked by rewrite(): inside
    // it, `T` is a placeholder, not a real type, so a signature like
    // `pulse get_value<T>(b: Box<T>) -> T` would make the walk
    // "instantiate" Box<T> as a bogus Box__T specialization (and emit
    // it as broken C++ containing a bare `T`). Templates are only ever
    // consumed by cloning + substituting a concrete type into them
    // (the `specializations` path), and are then dropped from the
    // output in Pass 3, so there is nothing for rewrite() to do on
    // the template itself.
    auto is_generic_template = [](const ASTNodePtr& n) {
        if (!n) return false;
        if (n->kind != NodeKind::PulseDecl && n->kind != NodeKind::AsyncPulseDecl &&
            n->kind != NodeKind::ForgeDecl && n->kind != NodeKind::EnumDecl &&
            n->kind != NodeKind::ImplDecl) return false;
        for (auto& c : n->children) {
            if (c && c->kind == NodeKind::TypeParamDecl) return true;
        }
        return false;
    };

    std::vector<ASTNodePtr> cloned;
    for (auto& n : prog) cloned.push_back(deep_clone(n));
    for (auto& n : cloned) {
        if (is_generic_template(n)) continue;
        rewrite(n, "");
    }

    // Pass 3: assemble the final program — original non-generic
    // declarations, plus every generated specialization, minus every
    // original generic declaration (whether or not it ended up with
    // any real instantiation; an uninstantiated generic declaration
    // has nothing concrete to lower to IR/codegen, and leaving it in
    // would just reproduce the exact "T isn't a real type" failure
    // this whole pass exists to avoid).
    for (auto& n : cloned) {
        if (!n) continue;
        bool skip = false;
        auto git = generics.find(n->value);
        if (git != generics.end() &&
            (n->kind == NodeKind::PulseDecl || n->kind == NodeKind::AsyncPulseDecl ||
             n->kind == NodeKind::ForgeDecl || n->kind == NodeKind::EnumDecl)) {
            // Confirm this really is the generic declaration itself
            // (has TypeParamDecl children), not an unrelated
            // non-generic declaration that happens to share a name
            // with something in a different scope.
            bool has_type_params = false;
            for (auto& c : n->children) if (c && c->kind == NodeKind::TypeParamDecl) has_type_params = true;
            if (has_type_params) skip = true;
        }
        // A generic impl<T> block itself (as opposed to one of the
        // concrete impl specializations this pass generated for it,
        // which live in `specializations` and get appended
        // separately below) still references T, not a concrete type
        // — leaving it in the output would fail downstream the same
        // way an un-instantiated generic forge/enum/pulse would.
        if (n->kind == NodeKind::ImplDecl) {
            bool has_type_params = false;
            for (auto& c : n->children) if (c && c->kind == NodeKind::TypeParamDecl) has_type_params = true;
            if (has_type_params) skip = true;
        }
        if (!skip) result.program.push_back(n);
    }
    // Emit specializations in dependency order. `specializations` is an
    // unordered_map, so raw iteration order is arbitrary, but a struct
    // that holds another specialization BY VALUE (Box__Box_int_ contains
    // a Box__int member) must be defined after it or the C++ backend
    // sees "Box__int does not name a type". A specialization depends on
    // every other specialization whose mangled name appears as a type
    // string (FieldDecl.extra, a pulse's parameter/return types, ...)
    // anywhere inside it. Depth-first with a visited set gives a
    // topological order; sorting the roots by name first keeps the
    // output deterministic run to run (unordered_map order is not).
    {
        std::vector<std::string> names;
        names.reserve(specializations.size());
        for (auto& kv : specializations) names.push_back(kv.first);
        std::sort(names.begin(), names.end());

        auto deps_of = [&](const ASTNodePtr& root) {
            std::vector<std::string> deps;
            std::function<void(const ASTNodePtr&)> walk = [&](const ASTNodePtr& x) {
                if (!x) return;
                auto note = [&](const std::string& ts) {
                    if (ts.empty()) return;
                    // A type string mentions a specialization if that
                    // mangled name occurs in it as a whole word.
                    for (auto& cand : names) {
                        if (cand == root->value) continue;
                        size_t pos = ts.find(cand);
                        while (pos != std::string::npos) {
                            bool l = (pos == 0) ||
                                !(std::isalnum(static_cast<unsigned char>(ts[pos - 1])) || ts[pos - 1] == '_');
                            size_t e = pos + cand.size();
                            bool r = (e >= ts.size()) ||
                                !(std::isalnum(static_cast<unsigned char>(ts[e])) || ts[e] == '_');
                            if (l && r) { deps.push_back(cand); break; }
                            pos = ts.find(cand, pos + 1);
                        }
                    }
                };
                note(x->extra);
                note(x->extra2);
                for (auto& ch : x->children) walk(ch);
            };
            walk(root);
            return deps;
        };

        std::unordered_set<std::string> emitted, visiting;
        std::function<void(const std::string&)> emit_spec;
        emit_spec = [&](const std::string& nm) {
            if (emitted.count(nm) || visiting.count(nm)) return;
            visiting.insert(nm);
            auto it = specializations.find(nm);
            if (it == specializations.end()) { visiting.erase(nm); return; }
            for (auto& d : deps_of(it->second)) emit_spec(d);
            visiting.erase(nm);
            emitted.insert(nm);
            result.program.push_back(it->second);
        };
        for (auto& nm : names) emit_spec(nm);
    }

    return result;
}

} // namespace xphage::generics
