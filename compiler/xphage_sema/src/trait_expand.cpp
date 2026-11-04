// ============================================================
// X-Phage trait expansion — see trait_expand.hpp
// ============================================================
#include "../include/trait_expand.hpp"
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace xphage::sema {

std::string replace_self_type(const std::string& s, const std::string& recv);

namespace {

ASTNodePtr clone(const ASTNodePtr& n) {
    if (!n) return nullptr;
    auto copy = std::make_shared<ASTNode>(*n);
    copy->children.clear();
    for (auto& c : n->children) copy->children.push_back(clone(c));
    return copy;
}

// Replaces every whole-identifier occurrence of `Self` in a type
// string ("Self", "Vec<Self>", "Option<Self>") with `recv`.
std::string replace_self(const std::string& s, const std::string& recv) {
    return replace_self_type(s, recv);
}

// Only the node kinds whose `extra`/`extra2`/`value` hold a TYPE are
// rewritten — never string literals or identifiers.
void substitute_self(const ASTNodePtr& n, const std::string& recv) {
    if (!n) return;
    switch (n->kind) {
        case NodeKind::FieldDecl:
        case NodeKind::AtomDecl:
        case NodeKind::ShadowDecl:
        case NodeKind::ConstDecl:
        case NodeKind::FluxDecl:
            n->extra = replace_self(n->extra, recv);
            break;
        case NodeKind::PulseDecl:
        case NodeKind::AsyncPulseDecl:
        case NodeKind::MethodDecl:
            n->extra  = replace_self(n->extra,  recv);
            n->extra2 = replace_self(n->extra2, recv);
            break;
        case NodeKind::SpawnExpr:
            n->value = replace_self(n->value, recv);
            break;
        case NodeKind::PathExpr: {
            size_t cc = n->value.find("::");
            if (cc != std::string::npos && n->value.compare(0, cc, "Self") == 0)
                n->value = recv + n->value.substr(cc);
            break;
        }
        default:
            break;
    }
    for (auto& c : n->children) substitute_self(c, recv);
}

bool has_body(const ASTNodePtr& m) {
    for (auto& c : m->children)
        if (c && c->kind == NodeKind::Block) return true;
    return false;
}

void collect_nexus(const Program& nodes, const std::string& prefix,
                   std::unordered_map<std::string, ASTNodePtr>& out) {
    for (auto& n : nodes) {
        if (!n) continue;
        if (n->kind == NodeKind::NexusDecl && !n->value.empty()) {
            out[n->value] = n;
            if (!prefix.empty()) out[prefix + "::" + n->value] = n;
        } else if (n->kind == NodeKind::RealmDecl) {
            collect_nexus(n->children,
                          prefix.empty() ? n->value : prefix + "::" + n->value, out);
        }
    }
}

void expand_in(Program& nodes,
               const std::unordered_map<std::string, ASTNodePtr>& nexus,
               std::vector<std::string>& realm_path) {
    for (auto& n : nodes) {
        if (!n) continue;
        if (n->kind == NodeKind::RealmDecl) {
            realm_path.push_back(n->value);
            expand_in(n->children, nexus, realm_path);
            realm_path.pop_back();
            continue;
        }
        if (n->kind != NodeKind::ImplDecl || n->extra.empty()) continue;

        // Inside a realm the receiver is written bare (`impl Lion`), but a type
        // NAME used in a signature must be the realm-qualified one (`Zoo::Lion`)
        // — that is how realm members are referred to everywhere else, and what
        // the backends resolve. `Self` therefore expands to the qualified name.
        std::string recv = n->extra;
        if (!realm_path.empty() && recv.find("::") == std::string::npos) {
            std::string q;
            for (auto& r : realm_path) q += r + "::";
            recv = q + recv;
        }

        // (1) default methods of the implemented nexus
        if (!n->value.empty()) {
            auto it = nexus.find(n->value);
            if (it != nexus.end()) {
                std::unordered_set<std::string> defined;
                for (auto& m : n->children)
                    if (m && (m->kind == NodeKind::MethodDecl ||
                              m->kind == NodeKind::PulseDecl))
                        defined.insert(m->value);
                for (auto& m : it->second->children) {
                    if (!m || (m->kind != NodeKind::MethodDecl && m->kind != NodeKind::PulseDecl)) continue;
                    if (!has_body(m) || defined.count(m->value)) continue;
                    n->children.push_back(clone(m));
                }
            }
        }

        // (2) `Self` -> receiver type, in every method (not in the
        // leading TypeParamDecl children of a generic impl)
        for (auto& m : n->children)
            if (m && m->kind != NodeKind::TypeParamDecl) substitute_self(m, recv);
    }
}

} // namespace

std::string replace_self_type(const std::string& s, const std::string& recv) {
    if (s.find("Self") == std::string::npos) return s;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (std::isalpha(c) || c == '_') {
            size_t j = i;
            while (j < s.size() &&
                   (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            std::string tok = s.substr(i, j - i);
            out += (tok == "Self") ? recv : tok;
            i = j;
        } else {
            out += s[i++];
        }
    }
    return out;
}

void expand_traits(Program& prog) {
    std::unordered_map<std::string, ASTNodePtr> nexus;
    collect_nexus(prog, "", nexus);
    std::vector<std::string> realm_path;
    expand_in(prog, nexus, realm_path);
}



// ============================================================
// Associated (receiver-less) functions
//
//   impl Point { pulse origin() -> Self { ... } }      Point::origin()
//   impl Default for int { pulse default() -> Self }   T::default()  (T = int)
//
// A member written with the `pulse` keyword and without a `self` parameter
// has no receiver, so it is an ordinary function that merely lives in the
// namespace of its type. After monomorphization (when `T::f` has become
// `Concrete::f`) every such function is hoisted out of its impl into a free
// function named `<Type>__<fn>` — next to the impl, in the same realm — and
// every `Type::fn(...)` call is rewritten to call it. No backend needs to know
// associated functions exist: they only ever see plain pulses.
// ============================================================
namespace {

bool is_prim(const std::string& t) {
    return t == "int" || t == "float" || t == "bool" || t == "str" ||
           t == "i8" || t == "i16" || t == "i32" || t == "isize" ||
           t == "u8" || t == "u16" || t == "u32" || t == "u64" || t == "usize" || t == "f32";
}

// Every backend treats a parameter literally named `self` as an implicit
// receiver and drops it from a free function's parameter list, so a hoisted
// primitive method calls its receiver `__self` instead. Renames the parameter,
// every use of it, and its uses inside f-string interpolations.
std::string rename_self_in_fstring(const std::string& s) {
    std::string out;
    int depth = 0;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '{') { depth++; out += c; i++; continue; }
        if (c == '}') { if (depth > 0) depth--; out += c; i++; continue; }
        if (depth > 0 && (std::isalpha((unsigned char)c) || c == '_')) {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            std::string tok = s.substr(i, j - i);
            bool member = i > 0 && s[i - 1] == '.';
            out += (tok == "self" && !member) ? "__self" : tok;
            i = j;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

void rename_self(const ASTNodePtr& n) {
    if (!n) return;
    if ((n->kind == NodeKind::Identifier || n->kind == NodeKind::FieldDecl) && n->value == "self")
        n->value = "__self";
    if (n->kind == NodeKind::FStringLit) n->value = rename_self_in_fstring(n->value);
    for (auto& c : n->children) rename_self(c);
}

bool is_assoc_fn(const ASTNode& m) {
    if (m.kind != NodeKind::PulseDecl && m.kind != NodeKind::AsyncPulseDecl) return false;
    for (auto& c : m.children)
        if (c && c->kind == NodeKind::FieldDecl && c->value == "self") return false;
    return !m.value.empty();
}

std::string join_path(const std::vector<std::string>& v, size_t n) {
    std::string r;
    for (size_t i = 0; i < n && i < v.size(); i++) r += (i ? "::" : "") + v[i];
    return r;
}

std::vector<std::string> split_path(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (true) {
        size_t j = s.find("::", i);
        if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
        out.push_back(s.substr(i, j - i));
        i = j + 2;
    }
    return out;
}

void hoist_assoc(Program& nodes, std::vector<std::string>& realm_path,
                 std::unordered_set<std::string>& hoisted,
                 std::unordered_set<std::string>& hoisted_prim_methods) {
    // Rebuilt in order: each hoisted function lands immediately AFTER its impl,
    // i.e. where the source declared it, so it is visible (C++ declaration
    // order inside a namespace) to everything that follows.
    Program out;
    for (auto& n : nodes) {
        if (!n) { out.push_back(n); continue; }
        if (n->kind == NodeKind::RealmDecl) {
            realm_path.push_back(n->value);
            hoist_assoc(n->children, realm_path, hoisted, hoisted_prim_methods);
            realm_path.pop_back();
            out.push_back(n);
            continue;
        }
        out.push_back(n);
        // An un-specialized generic impl is a template, not code.
        if (n->kind != NodeKind::ImplDecl || n->extra.empty() ||
            n->extra.find('<') != std::string::npos) continue;
        Program added;
        std::string bare = split_path(n->extra).back();
        const bool prim_recv = is_prim(bare);
        std::vector<ASTNodePtr> kept;
        for (auto& m : n->children) {
            // A method of a primitive (`impl Ord for int { cmp(other: Self) ... }`)
            // has no struct to live in, so it becomes a free function taking the
            // receiver as its first parameter; calls on a receiver of that
            // primitive type are rewritten later (see rewrite_prim_calls).
            if (prim_recv && m && (m->kind == NodeKind::MethodDecl || m->kind == NodeKind::PulseDecl) &&
                !m->value.empty() && !is_assoc_fn(*m)) {
                auto fn = clone(m);
                fn->kind = NodeKind::PulseDecl;
                fn->value = bare + "__" + m->value;
                bool has_self_param = false;
                for (auto& c : fn->children)
                    if (c && c->kind == NodeKind::FieldDecl && c->value == "self") {
                        has_self_param = true;
                        if (c->extra.empty()) c->extra = bare;
                    }
                if (!has_self_param) {
                    auto self_p = std::make_shared<ASTNode>(NodeKind::FieldDecl, "self", m->span.line);
                    self_p->extra = bare;
                    fn->children.insert(fn->children.begin(), self_p);
                }
                rename_self(fn);
                hoisted_prim_methods.insert(bare + "::" + m->value);
                hoisted.insert(join_path(realm_path, realm_path.size()) +
                               (realm_path.empty() ? "" : "::") + fn->value);
                added.push_back(fn);
            } else if (m && is_assoc_fn(*m)) {
                auto fn = clone(m);
                fn->value = bare + "__" + m->value;
                hoisted.insert(join_path(realm_path, realm_path.size()) +
                               (realm_path.empty() ? "" : "::") + fn->value);
                added.push_back(fn);
            } else {
                kept.push_back(m);
            }
        }
        n->children = std::move(kept);
        for (auto& a : added) out.push_back(a);
    }
    nodes = std::move(out);
}

void rewrite_assoc_calls(const ASTNodePtr& n, std::vector<std::string>& ctx,
                         const std::unordered_set<std::string>& hoisted) {
    if (!n) return;
    if (n->kind == NodeKind::RealmDecl) {
        ctx.push_back(n->value);
        for (auto& c : n->children) rewrite_assoc_calls(c, ctx, hoisted);
        ctx.pop_back();
        return;
    }
    if (n->kind == NodeKind::CallExpr && !n->children.empty() && n->children[0] &&
        n->children[0]->kind == NodeKind::PathExpr) {
        auto& callee = n->children[0];
        auto segs = split_path(callee->value);
        if (segs.size() >= 2) {
            std::string name = segs[segs.size() - 2] + "__" + segs.back();
            std::vector<std::string> prefix(segs.begin(), segs.end() - 2);
            // innermost enclosing realm first, then outward, then the top level
            for (size_t k = ctx.size() + 1; k-- > 0; ) {
                std::vector<std::string> path(ctx.begin(), ctx.begin() + k);
                path.insert(path.end(), prefix.begin(), prefix.end());
                std::string full = join_path(path, path.size()) + (path.empty() ? "" : "::") + name;
                if (!hoisted.count(full)) continue;
                ASTNodePtr repl;
                // A function that lives in a realm is always called by its FULL
                // path (`Zoo::Lion__cub`), even from inside that same realm: the
                // backends resolve qualified paths uniformly, but not every one
                // resolves an unqualified sibling name from inside a realm.
                if (path.empty())
                    repl = std::make_shared<ASTNode>(NodeKind::Identifier, name, callee->span.line);
                else
                    repl = std::make_shared<ASTNode>(NodeKind::PathExpr, full, callee->span.line);
                repl->span = callee->span;
                callee = repl;
                break;
            }
        }
    }
    for (auto& c : n->children) rewrite_assoc_calls(c, ctx, hoisted);
}

// ------------------------------------------------------------
// Method calls on primitive receivers: `a.cmp(b)` with `a: int`
//
// The methods of `impl Ord for int` were hoisted to `int__cmp(self, other)`
// above. A call `a.cmp(b)` must become `int__cmp(a, b)`, which needs the TYPE
// of `a`. This pass runs after monomorphization, so a generic `a: T` is already
// `a: int`; the types it can see are parameters, annotated and initialized
// locals, forge fields, literals, operators and the declared return types of
// functions and methods. A receiver whose type it cannot determine is left
// alone (and then fails in the backend as an unknown method, as before).
// ------------------------------------------------------------
struct TypeEnv {
    std::unordered_map<std::string, std::string> fn_ret;      // function (bare and qualified) -> return type
    std::unordered_map<std::string, std::string> method_ret;  // "Type::method" -> return type
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> forge_fields;
    std::unordered_set<std::string> prim_methods;             // "int::cmp"
    std::vector<std::unordered_map<std::string, std::string>> scopes;

    std::string lookup(const std::string& v) const {
        for (size_t i = scopes.size(); i-- > 0;) {
            auto it = scopes[i].find(v);
            if (it != scopes[i].end()) return it->second;
        }
        return "";
    }
};

std::string norm_type(std::string t) {
    for (const char* pre : {"own ", "ref ", "mut_ref "})
        if (t.rfind(pre, 0) == 0) t = t.substr(std::string(pre).size());
    return t;
}

// `Zoo::Lion` -> `Lion`, `Box<int>` -> `Box`
std::string bare_name(const std::string& t) {
    std::string r = t.substr(0, t.find('<'));
    size_t cc = r.rfind("::");
    return cc == std::string::npos ? r : r.substr(cc + 2);
}

std::string infer_type(const ASTNode& e, const TypeEnv& env) {
    switch (e.kind) {
        case NodeKind::IntLit:
        case NodeKind::FloatLit: {
            auto sfx = e.attrs.find("suffix");
            if (sfx != e.attrs.end())
                return sfx->second == "i64" ? "int" : sfx->second == "f64" ? "float" : sfx->second;
            return e.kind == NodeKind::IntLit ? "int" : "float";
        }
        case NodeKind::BoolLit:    return "bool";
        case NodeKind::StringLit:
        case NodeKind::FStringLit: return "str";
        case NodeKind::Identifier: return env.lookup(e.value);
        case NodeKind::CastExpr:   return norm_type(e.extra);
        case NodeKind::SpawnExpr:  return e.value;
        case NodeKind::MemberExpr: {
            if (e.children.empty() || !e.children[0]) return "";
            auto ff = env.forge_fields.find(bare_name(infer_type(*e.children[0], env)));
            if (ff == env.forge_fields.end()) return "";
            auto fit = ff->second.find(e.value);
            return fit == ff->second.end() ? "" : norm_type(fit->second);
        }
        case NodeKind::CallExpr: {
            if (e.children.empty() || !e.children[0]) return "";
            const ASTNode& c = *e.children[0];
            if (c.kind == NodeKind::Identifier || c.kind == NodeKind::PathExpr) {
                auto it = env.fn_ret.find(c.value);
                return it == env.fn_ret.end() ? "" : norm_type(it->second);
            }
            if (c.kind == NodeKind::MemberExpr && !c.children.empty() && c.children[0]) {
                auto it = env.method_ret.find(bare_name(infer_type(*c.children[0], env)) + "::" + c.value);
                return it == env.method_ret.end() ? "" : norm_type(it->second);
            }
            return "";
        }
        case NodeKind::BinaryOp: {
            const std::string& op = e.value;
            if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
                op == "&&" || op == "||" || op == "and" || op == "or") return "bool";
            if (e.children.size() >= 2 && e.children[0]) {
                std::string l = infer_type(*e.children[0], env);
                if (!l.empty()) return l;
                if (e.children[1]) return infer_type(*e.children[1], env);
            }
            return "";
        }
        case NodeKind::UnaryOp:
            if (e.value == "!" || e.value == "not") return "bool";
            if (!e.children.empty() && e.children[0]) return infer_type(*e.children[0], env);
            return "";
        default: return "";
    }
}

void collect_type_env(const Program& nodes, std::vector<std::string>& realm_path, TypeEnv& env) {
    for (auto& n : nodes) {
        if (!n) continue;
        switch (n->kind) {
            case NodeKind::RealmDecl:
                realm_path.push_back(n->value);
                collect_type_env(n->children, realm_path, env);
                realm_path.pop_back();
                break;
            case NodeKind::PulseDecl:
            case NodeKind::AsyncPulseDecl:
                if (!n->value.empty() && !n->extra2.empty()) {
                    env.fn_ret[n->value] = n->extra2;
                    if (!realm_path.empty())
                        env.fn_ret[join_path(realm_path, realm_path.size()) + "::" + n->value] = n->extra2;
                }
                break;
            case NodeKind::ForgeDecl:
                for (auto& f : n->children)
                    if (f && f->kind == NodeKind::FieldDecl && !f->extra.empty())
                        env.forge_fields[bare_name(n->value)][f->value] = f->extra;
                break;
            case NodeKind::ImplDecl:
                for (auto& m : n->children)
                    if (m && (m->kind == NodeKind::MethodDecl || m->kind == NodeKind::PulseDecl) &&
                        !m->value.empty() && !m->extra2.empty())
                        env.method_ret[bare_name(n->extra) + "::" + m->value] = m->extra2;
                break;
            default: break;
        }
    }
}

void rewrite_prim_calls(const ASTNodePtr& n, TypeEnv& env, const std::string& self_type) {
    if (!n) return;
    switch (n->kind) {
        case NodeKind::RealmDecl:
            for (auto& c : n->children) rewrite_prim_calls(c, env, "");
            return;
        case NodeKind::ImplDecl: {
            std::string recv = norm_type(n->extra);
            for (auto& c : n->children) rewrite_prim_calls(c, env, bare_name(recv));
            return;
        }
        case NodeKind::PulseDecl:
        case NodeKind::AsyncPulseDecl:
        case NodeKind::MethodDecl: {
            env.scopes.emplace_back();
            if (!self_type.empty()) env.scopes.back()["self"] = self_type;
            for (auto& c : n->children)
                if (c && c->kind == NodeKind::FieldDecl && !c->value.empty() && !c->extra.empty())
                    env.scopes.back()[c->value] = norm_type(c->extra);
            for (auto& c : n->children) rewrite_prim_calls(c, env, "");
            env.scopes.pop_back();
            return;
        }
        case NodeKind::Block:
            env.scopes.emplace_back();
            for (auto& c : n->children) rewrite_prim_calls(c, env, "");
            env.scopes.pop_back();
            return;
        case NodeKind::AtomDecl:
        case NodeKind::ShadowDecl:
        case NodeKind::ConstDecl:
        case NodeKind::FluxDecl: {
            for (auto& c : n->children) rewrite_prim_calls(c, env, "");
            std::string t = !n->extra.empty() ? norm_type(n->extra)
                          : (!n->children.empty() && n->children[0] ? infer_type(*n->children[0], env) : "");
            if (!t.empty() && !n->value.empty() && !env.scopes.empty()) env.scopes.back()[n->value] = t;
            return;
        }
        default: break;
    }
    for (auto& c : n->children) rewrite_prim_calls(c, env, "");   // inner calls first
    if (n->kind == NodeKind::CallExpr && !n->children.empty() && n->children[0] &&
        n->children[0]->kind == NodeKind::MemberExpr && !n->children[0]->children.empty() &&
        n->children[0]->children[0]) {
        auto callee = n->children[0];
        auto obj = callee->children[0];
        std::string ot = bare_name(norm_type(infer_type(*obj, env)));
        if (is_prim(ot) && env.prim_methods.count(ot + "::" + callee->value)) {
            auto id = std::make_shared<ASTNode>(NodeKind::Identifier, ot + "__" + callee->value,
                                                callee->span.line);
            id->span = callee->span;
            std::vector<ASTNodePtr> nc;
            nc.push_back(id);
            nc.push_back(obj);
            for (size_t i = 1; i < n->children.size(); i++) nc.push_back(n->children[i]);
            n->children = std::move(nc);
        }
    }
}

} // namespace

void lower_assoc_functions(Program& prog) {
    std::unordered_set<std::string> hoisted, prim_methods;
    std::vector<std::string> realm_path;
    hoist_assoc(prog, realm_path, hoisted, prim_methods);
    if (hoisted.empty()) return;
    std::vector<std::string> ctx;
    for (auto& n : prog) rewrite_assoc_calls(n, ctx, hoisted);

    if (!prim_methods.empty()) {
        TypeEnv env;
        env.prim_methods = std::move(prim_methods);
        std::vector<std::string> rp;
        collect_type_env(prog, rp, env);
        env.scopes.emplace_back();                       // top-level statements
        for (auto& n : prog) rewrite_prim_calls(n, env, "");
    }
}

} // namespace xphage::sema
