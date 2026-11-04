// ============================================================
// X-Phage numeric typing — see numeric_types.hpp for the rules.
// ============================================================
#include "../include/numeric_types.hpp"
#include <cctype>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace xphage::sema {
namespace {

using xphage::diag::Diagnostic;
using xphage::diag::Severity;

// ── type names ───────────────────────────────────────────────

std::string canon_ident(const std::string& t) {
    if (t == "i64")  return "int";
    if (t == "f64")  return "float";
    if (t == "byte") return "u8";
    return t;
}

// Token-aware: "Vec<byte>" -> "Vec<u8>", "Map<str, i64>" -> "Map<str, int>".
std::string canon_in_string(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (std::isalpha(c) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            out += canon_ident(s.substr(i, j - i));
            i = j;
        } else {
            out += s[i++];
        }
    }
    return out;
}

std::string canon(std::string t) {
    for (const char* pre : {"own ", "ref ", "mut_ref "})
        if (t.rfind(pre, 0) == 0) t = t.substr(std::strlen(pre));
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    return canon_in_string(t);
}

bool is_sint(const std::string& t) { return t == "i8" || t == "i16" || t == "i32" || t == "isize"; }
bool is_uint(const std::string& t) { return t == "u8" || t == "u16" || t == "u32" || t == "u64" || t == "usize"; }
bool is_strict_int(const std::string& t) { return is_sint(t) || is_uint(t); }
bool is_int(const std::string& t)   { return is_strict_int(t) || t == "int"; }
bool is_fl(const std::string& t)    { return t == "f32" || t == "float"; }
bool is_strict(const std::string& t){ return is_strict_int(t) || t == "f32"; }
bool is_numeric(const std::string& t){ return is_int(t) || is_fl(t); }

int bits_of(const std::string& t) {
    if (t == "i8" || t == "u8") return 8;
    if (t == "i16" || t == "u16") return 16;
    if (t == "i32" || t == "u32" || t == "f32") return 32;
    return 64;
}

// Does the decimal literal `digits` fit integer type `t`? (`int` keeps the
// legacy behaviour: anything up to 2^64-1 is accepted and wraps.)
bool fits_int(const std::string& digits, const std::string& t, bool negated) {
    if (digits.empty()) return false;
    unsigned __int128 v = 0;
    const unsigned __int128 u64max = ~(unsigned long long)0;
    for (char c : digits) {
        if (!std::isdigit((unsigned char)c)) return false;
        v = v * 10 + (unsigned)(c - '0');
        if (v > u64max) return false;
    }
    if (t == "int") return true;
    int b = bits_of(t);
    if (is_uint(t)) {
        if (negated) return v == 0;
        unsigned __int128 mx = (b == 64) ? u64max : (((unsigned __int128)1 << b) - 1);
        return v <= mx;
    }
    unsigned __int128 maxpos = ((unsigned __int128)1 << (b - 1)) - 1;
    return negated ? v <= maxpos + 1 : v <= maxpos;
}

std::string bare_name(const std::string& t) {
    std::string r = t.substr(0, t.find('<'));
    size_t cc = r.rfind("::");
    return cc == std::string::npos ? r : r.substr(cc + 2);
}

std::vector<std::string> generic_args(const std::string& t) {
    std::vector<std::string> out;
    size_t lt = t.find('<');
    if (lt == std::string::npos || t.back() != '>') return out;
    std::string inner = t.substr(lt + 1, t.size() - lt - 2), cur;
    int depth = 0;
    for (char c : inner) {
        if (c == '<') depth++;
        else if (c == '>') depth--;
        if (c == ',' && depth == 0) { out.push_back(canon(cur)); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(canon(cur));
    return out;
}

// ── 1. canonicalise every type name written in the program ───

void canon_walk(const ASTNodePtr& n) {
    if (!n) return;
    switch (n->kind) {
        case NodeKind::FieldDecl:
        case NodeKind::AtomDecl:
        case NodeKind::ShadowDecl:
        case NodeKind::ConstDecl:
        case NodeKind::FluxDecl:
        case NodeKind::CastExpr:
        case NodeKind::ImplDecl:
            n->extra = canon_in_string(n->extra);
            break;
        case NodeKind::PulseDecl:
        case NodeKind::AsyncPulseDecl:
        case NodeKind::MethodDecl:
            n->extra2 = canon_in_string(n->extra2);
            break;
        case NodeKind::ExternDecl:
            n->extra2 = canon_in_string(n->extra2);
            for (auto& kv : n->attrs)
                if (kv.first.rfind("param_type_", 0) == 0) kv.second = canon_in_string(kv.second);
            break;
        case NodeKind::SpawnExpr:
            n->value = canon_in_string(n->value);
            break;
        default: break;
    }
    for (auto& c : n->children) canon_walk(c);
}

// ── 2. the typer ─────────────────────────────────────────────

struct Sig { std::vector<std::string> params; std::string ret; };

class Typer {
public:
    std::vector<Diagnostic> diags;

    void run(Program& prog) {
        std::vector<std::string> rp;
        collect(prog, rp);
        scopes_.emplace_back();                 // global scope
        for (auto& n : prog) ty(n, "");
    }

private:
    std::unordered_map<std::string, Sig> fns_;       // function (bare and qualified)
    std::unordered_map<std::string, Sig> methods_;   // "Type::method" (without self)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fields_;
    std::vector<std::unordered_map<std::string, std::string>> scopes_;
    std::string cur_ret_;
    std::string self_type_;

    // ---- diagnostics ----
    void error(const char* code, const ASTNode& at, const std::string& msg,
               const std::string& help = "") {
        Diagnostic d;
        d.code = code;
        d.severity = Severity::Error;
        d.message = msg;
        d.span = at.span;
        d.underline_len = 1;
        if (!help.empty()) d.help = help;
        diags.push_back(std::move(d));
    }

    void mismatch(const ASTNode& at, const std::string& expected, const std::string& found,
                  const char* what) {
        error("XP2050", at,
              std::string("mismatched types in ") + what + ": expected `" + expected + "`, found `" + found + "`",
              "there is no implicit conversion between sized numeric types; use `cast(x, " + expected + ")`");
    }

    // Both known and different while a sized type is involved -> error.
    void compat(const std::string& expected, const std::string& actual, const ASTNode& at,
                const char* what) {
        if (expected.empty() || actual.empty() || expected == actual) return;
        if (!(is_numeric(expected) && is_numeric(actual))) return;     // not this pass's business
        if (is_strict(expected) || is_strict(actual)) mismatch(at, expected, actual, what);
    }

    void annotate(const ASTNodePtr& e, const std::string& t) {
        if (e && is_strict(t)) e->attrs["ty"] = t;
    }

    // ---- scopes ----
    void declare(const std::string& name, const std::string& t) {
        if (!scopes_.empty() && !name.empty()) scopes_.back()[name] = t;
    }
    std::string lookup(const std::string& name) const {
        for (size_t i = scopes_.size(); i-- > 0;) {
            auto it = scopes_[i].find(name);
            if (it != scopes_[i].end()) return it->second;
        }
        return "";
    }

    // ---- signature collection ----
    void collect(const Program& nodes, std::vector<std::string>& rp) {
        for (auto& n : nodes) {
            if (!n) continue;
            switch (n->kind) {
                case NodeKind::RealmDecl:
                    rp.push_back(n->value);
                    collect(n->children, rp);
                    rp.pop_back();
                    break;
                case NodeKind::Block:
                    collect(n->children, rp);
                    break;
                case NodeKind::PulseDecl:
                case NodeKind::AsyncPulseDecl:
                case NodeKind::ExternDecl: {
                    if (n->value.empty()) break;
                    Sig sg = sig_of(*n);
                    fns_[n->value] = sg;
                    if (!rp.empty()) {
                        std::string q;
                        for (auto& r : rp) q += r + "::";
                        fns_[q + n->value] = sg;
                    }
                    break;
                }
                case NodeKind::ForgeDecl:
                    for (auto& f : n->children)
                        if (f && f->kind == NodeKind::FieldDecl && !f->extra.empty())
                            fields_[bare_name(n->value)][f->value] = canon(f->extra);
                    break;
                case NodeKind::ImplDecl:
                    for (auto& m : n->children)
                        if (m && (m->kind == NodeKind::MethodDecl || m->kind == NodeKind::PulseDecl) &&
                            !m->value.empty())
                            methods_[bare_name(n->extra) + "::" + m->value] = sig_of(*m);
                    break;
                default: break;
            }
        }
    }

    Sig sig_of(const ASTNode& n) const {
        Sig s;
        for (auto& c : n.children)
            if (c && c->kind == NodeKind::FieldDecl && c->value != "self" && c->value != "__self")
                s.params.push_back(canon(c->extra));
        s.ret = canon(n.extra2);
        if (s.ret == "void") s.ret.clear();
        return s;
    }

    // ---- literal handling ----
    static bool is_flex(const ASTNodePtr& e) {
        if (!e) return false;
        if (e->kind == NodeKind::IntLit || e->kind == NodeKind::FloatLit)
            return !e->attrs.count("suffix");
        if (e->kind == NodeKind::UnaryOp && (e->value == "-" || e->value == "+") && !e->children.empty())
            return is_flex(e->children[0]);
        if (e->kind == NodeKind::BinaryOp && e->children.size() == 2) {
            const std::string& op = e->value;
            if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
                op == "&" || op == "|" || op == "^")
                return is_flex(e->children[0]) && is_flex(e->children[1]);
        }
        return false;
    }

    std::string lit_int(const ASTNodePtr& e, const std::string& expect, bool negated) {
        std::string t;
        auto sit = e->attrs.find("suffix");
        if (sit != e->attrs.end()) t = canon(sit->second);
        else if (is_numeric(expect)) t = expect;
        else t = "int";
        if (is_fl(t)) { annotate(e, t); return t; }          // integer literal used as a float
        if (!fits_int(e->value, t, negated)) {
            error("XP2051", *e,
                  "literal `" + std::string(negated ? "-" : "") + e->value + "` is out of range for type `" + t + "`");
        }
        annotate(e, t);
        return t;
    }

    std::string lit_float(const ASTNodePtr& e, const std::string& expect) {
        std::string t;
        auto sit = e->attrs.find("suffix");
        if (sit != e->attrs.end()) {
            t = canon(sit->second);
            if (!is_fl(t)) {
                error("XP2054", *e, "a float literal cannot have the integer suffix `" + t + "`");
                return t;
            }
        } else {
            t = is_fl(expect) ? expect : "float";
        }
        annotate(e, t);
        return t;
    }

    // ---- the typer proper: returns the expression's type ("" = unknown) ----
    std::string ty(const ASTNodePtr& e, const std::string& expect) {
        if (!e) return "";
        switch (e->kind) {
            case NodeKind::IntLit:   return lit_int(e, expect, false);
            case NodeKind::FloatLit: return lit_float(e, expect);
            case NodeKind::BoolLit:  return "bool";
            case NodeKind::StringLit: return "str";
            case NodeKind::FStringLit:
                for (auto& c : e->children) ty(c, "");
                return "str";
            case NodeKind::Identifier: {
                std::string t = lookup(e->value);
                annotate(e, t);
                return t;
            }
            case NodeKind::UnaryOp:      return unary(e, expect);
            case NodeKind::BinaryOp:     return binary(e, expect);
            case NodeKind::CastExpr: {
                std::string from = e->children.empty() ? "" : ty(e->children[0], "");
                if (!from.empty()) e->attrs["from"] = from;   // backends pick sign/zero-extend, f2i from it
                std::string t = canon(e->extra);
                e->extra = t;
                annotate(e, t);
                return t;
            }
            case NodeKind::CallExpr:     return call(e);
            case NodeKind::MemberExpr: {
                std::string ot = e->children.empty() ? "" : ty(e->children[0], "");
                auto ff = fields_.find(bare_name(ot));
                if (ff == fields_.end()) return "";
                auto fit = ff->second.find(e->value);
                if (fit == ff->second.end()) return "";
                annotate(e, fit->second);
                return fit->second;
            }
            case NodeKind::IndexExpr: {
                std::string bt = e->children.size() > 0 ? ty(e->children[0], "") : "";
                if (e->children.size() > 1) ty(e->children[1], "");
                if (bt.rfind("Vec<", 0) == 0) {
                    auto ga = generic_args(bt);
                    if (ga.size() == 1) { annotate(e, ga[0]); return ga[0]; }
                }
                return "";
            }
            case NodeKind::SpawnExpr: {
                auto ff = fields_.find(bare_name(e->value));
                for (auto& c : e->children) {
                    if (!c) continue;
                    std::string want;
                    auto fa = c->attrs.find("field");
                    if (fa != c->attrs.end() && ff != fields_.end()) {
                        auto fit = ff->second.find(fa->second);
                        if (fit != ff->second.end()) want = fit->second;
                    }
                    std::string got = ty(c, want);
                    compat(want, got, *c, "field initializer");
                }
                return e->value;
            }
            case NodeKind::AssignExpr:   return assign(e);
            case NodeKind::RangeExpr:    return range(e);
            case NodeKind::ReturnStmt: {
                if (!e->children.empty()) {
                    std::string got = ty(e->children[0], cur_ret_);
                    compat(cur_ret_, got, *e->children[0], "return value");
                }
                return "";
            }
            case NodeKind::AtomDecl:
            case NodeKind::ShadowDecl:
            case NodeKind::ConstDecl:
            case NodeKind::FluxDecl:
            case NodeKind::GlobalDecl:   return decl(e);
            case NodeKind::Block: {
                scopes_.emplace_back();
                for (auto& c : e->children) ty(c, "");
                scopes_.pop_back();
                return "";
            }
            case NodeKind::ForStmt:      return for_stmt(e);
            case NodeKind::PulseDecl:
            case NodeKind::AsyncPulseDecl:
            case NodeKind::MethodDecl:   return function(e);
            case NodeKind::ImplDecl: {
                std::string saved = self_type_;
                self_type_ = bare_name(e->extra);
                for (auto& c : e->children)
                    if (c && c->kind != NodeKind::TypeParamDecl) ty(c, "");
                self_type_ = saved;
                return "";
            }
            case NodeKind::ForgeDecl: {
                for (auto& f : e->children) {
                    if (!f || f->kind != NodeKind::FieldDecl || f->children.empty()) continue;
                    std::string want = canon(f->extra);
                    std::string got = ty(f->children[0], want);
                    compat(want, got, *f->children[0], "field default");
                }
                return "";
            }
            case NodeKind::ProbeArm: {
                scopes_.emplace_back();
                auto b = e->attrs.find("bindings");
                if (b != e->attrs.end()) {
                    std::string cur;
                    for (char c : b->second + ",") {
                        if (c == ',') { declare(cur, ""); cur.clear(); } else cur += c;
                    }
                }
                for (auto& c : e->children) ty(c, "");
                scopes_.pop_back();
                return "";
            }
            case NodeKind::NexusDecl:
            case NodeKind::EnumDecl:
            case NodeKind::UseDecl:
            case NodeKind::TypeParamDecl:
            case NodeKind::FieldDecl:
            case NodeKind::PathExpr:
                return "";
            default:
                for (auto& c : e->children) ty(c, "");
                return "";
        }
    }

    std::string unary(const ASTNodePtr& e, const std::string& expect) {
        if (e->children.empty()) return "";
        const std::string& op = e->value;
        const ASTNodePtr& c = e->children[0];
        if (op == "!" || op == "not") { ty(c, ""); return "bool"; }
        if (op == "-" || op == "+" || op == "~") {
            std::string t = (c && c->kind == NodeKind::IntLit) ? lit_int(c, expect, op == "-")
                                                              : ty(c, expect);
            if (op == "-" && is_uint(t))
                error("XP2052", *e, "cannot apply unary `-` to the unsigned type `" + t + "`",
                      "negate a signed type, or use `cast(0, " + t + ")` arithmetic explicitly");
            if (op == "~" && is_fl(t))
                error("XP2053", *e, "bitwise `~` is not defined for the float type `" + t + "`");
            annotate(e, t);
            return t;
        }
        return ty(c, expect);
    }

    std::string binary(const ASTNodePtr& e, const std::string& expect) {
        if (e->children.size() < 2) return "";
        const std::string& op = e->value;
        const ASTNodePtr& l = e->children[0];
        const ASTNodePtr& r = e->children[1];
        if (op == "&&" || op == "||" || op == "and" || op == "or") {
            ty(l, ""); ty(r, "");
            return "bool";
        }
        const bool cmp   = op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
        const bool shift = op == "<<" || op == ">>";
        const bool bitw  = op == "&" || op == "|" || op == "^";
        const std::string hint = cmp ? "" : expect;

        if (shift) {
            std::string tl = ty(l, hint);
            std::string tr = ty(r, "");
            if (is_fl(tl)) error("XP2053", *e, "shift is not defined for the float type `" + tl + "`");
            if (!tr.empty() && !is_int(tr))
                error("XP2053", *r, "a shift count must be an integer, found `" + tr + "`");
            annotate(e, tl);
            if (is_strict(tl)) e->attrs["opty"] = tl;
            return tl;
        }

        std::string tl, tr;
        const bool fl = is_flex(l), fr = is_flex(r);
        if (fl && !fr)      { tr = ty(r, hint); tl = ty(l, is_numeric(tr) ? tr : hint); }
        else if (!fl && fr) { tl = ty(l, hint); tr = ty(r, is_numeric(tl) ? tl : hint); }
        else                { tl = ty(l, hint); tr = ty(r, hint); }

        compat(tl, tr, *r, cmp ? "comparison" : "operands");
        if (bitw && (is_fl(tl) || is_fl(tr)))
            error("XP2053", *e, "bitwise `" + op + "` is not defined for floats");

        std::string t;
        if (is_strict(tl)) t = tl;
        else if (is_strict(tr)) t = tr;
        else if (is_fl(tl) || is_fl(tr)) t = "float";
        else t = tl.empty() ? tr : tl;

        if (cmp) {
            if (is_strict(t)) e->attrs["opty"] = t;
            return "bool";
        }
        annotate(e, t);
        if (is_strict(t)) e->attrs["opty"] = t;
        return t;
    }

    std::string assign(const ASTNodePtr& e) {
        if (e->children.size() < 2) return "";
        const std::string& op = e->value;
        std::string tt = ty(e->children[0], "");
        const bool shift = op == "<<=" || op == ">>=";
        std::string want = shift ? std::string() : tt;
        std::string got = ty(e->children[1], want);
        if (shift) {
            if (!got.empty() && !is_int(got))
                error("XP2053", *e->children[1], "a shift count must be an integer, found `" + got + "`");
        } else {
            compat(tt, got, *e->children[1], "assignment");
        }
        annotate(e, tt);
        if (is_strict(tt)) e->attrs["opty"] = tt;
        return tt;
    }

    std::string range(const ASTNodePtr& e) {
        if (e->children.size() < 2) return "";
        const ASTNodePtr& l = e->children[0];
        const ASTNodePtr& r = e->children[1];
        std::string tl, tr;
        const bool fl = is_flex(l), fr = is_flex(r);
        if (fl && !fr)      { tr = ty(r, ""); tl = ty(l, is_numeric(tr) ? tr : ""); }
        else if (!fl && fr) { tl = ty(l, ""); tr = ty(r, is_numeric(tl) ? tl : ""); }
        else                { tl = ty(l, ""); tr = ty(r, ""); }
        compat(tl, tr, *r, "range bounds");
        std::string t = is_strict(tl) ? tl : (is_strict(tr) ? tr : (tl.empty() ? tr : tl));
        if (is_strict(t)) e->attrs["opty"] = t;
        return "range:" + t;
    }

    std::string decl(const ASTNodePtr& e) {
        std::string declared = canon(e->extra);
        e->extra = declared;
        std::string t = declared;
        if (!e->children.empty() && e->children[0]) {
            std::string got = ty(e->children[0], declared);
            compat(declared, got, *e->children[0], "initializer");
            if (declared.empty()) {
                t = got;
                if (t.rfind("range:", 0) == 0) t.clear();
                if (is_strict(t)) e->extra = t;       // backends read the type from `extra`
            }
        }
        declare(e->value, t);
        return "";
    }

    std::string for_stmt(const ASTNodePtr& e) {
        if (e->children.size() < 2) return "";
        std::string it = ty(e->children[0], "");
        std::string vt;
        if (it.rfind("range:", 0) == 0) vt = it.substr(6);
        else if (it.rfind("Vec<", 0) == 0) { auto ga = generic_args(it); if (ga.size() == 1) vt = ga[0]; }
        if (is_strict(vt)) e->attrs["var_ty"] = vt;
        scopes_.emplace_back();
        declare(e->value, vt);
        for (size_t i = 1; i < e->children.size(); i++) ty(e->children[i], "");
        scopes_.pop_back();
        return "";
    }

    std::string function(const ASTNodePtr& e) {
        std::string saved_ret = cur_ret_;
        cur_ret_ = canon(e->extra2);
        if (cur_ret_ == "void") cur_ret_.clear();
        scopes_.emplace_back();
        bool explicit_self = false;
        for (auto& c : e->children) {
            if (!c || c->kind != NodeKind::FieldDecl) continue;
            std::string t = canon(c->extra);
            if ((c->value == "self" || c->value == "__self") && t.empty()) t = self_type_;
            if (c->value == "self") explicit_self = true;
            declare(c->value, t);
        }
        if (!explicit_self && !self_type_.empty() && e->kind == NodeKind::MethodDecl)
            declare("self", self_type_);
        for (auto& c : e->children) {
            if (!c || c->kind == NodeKind::FieldDecl || c->kind == NodeKind::TypeParamDecl) continue;
            ty(c, "");
        }
        scopes_.pop_back();
        cur_ret_ = saved_ret;
        return "";
    }

    std::string call(const ASTNodePtr& e) {
        if (e->children.empty() || !e->children[0]) return "";
        const ASTNodePtr& callee = e->children[0];
        const Sig* sig = nullptr;
        std::string recv_t;
        Sig builtin;
        if (callee->kind == NodeKind::Identifier || callee->kind == NodeKind::PathExpr) {
            auto it = fns_.find(callee->value);
            if (it != fns_.end()) sig = &it->second;
        } else if (callee->kind == NodeKind::MemberExpr && !callee->children.empty()) {
            recv_t = ty(callee->children[0], "");
            auto it = methods_.find(bare_name(recv_t) + "::" + callee->value);
            if (it != methods_.end()) sig = &it->second;
            else if (builtin_method(recv_t, callee->value, builtin)) sig = &builtin;
        } else {
            ty(callee, "");
        }
        for (size_t i = 1; i < e->children.size(); i++) {
            std::string want = (sig && i - 1 < sig->params.size()) ? sig->params[i - 1] : "";
            std::string got = ty(e->children[i], want);
            if (e->children[i]) compat(want, got, *e->children[i], "argument");
        }
        std::string ret = sig ? sig->ret : "";
        annotate(e, ret);
        return ret;
    }

    // Vec / Map / Set methods the backends implement natively. An empty
    // parameter type means "no constraint here" (indices are ordinary ints).
    static bool builtin_method(const std::string& recv, const std::string& m, Sig& out) {
        auto ga = generic_args(recv);
        if (recv.rfind("Vec<", 0) == 0 && ga.size() == 1) {
            const std::string& T = ga[0];
            if (m == "push") { out = {{T}, ""}; return true; }
            if (m == "get")  { out = {{""}, T}; return true; }
            if (m == "set")  { out = {{"", T}, ""}; return true; }
            if (m == "pop")  { out = {{}, T}; return true; }
            if (m == "len")  { out = {{}, "int"}; return true; }
        } else if (recv.rfind("Map<", 0) == 0 && ga.size() == 2) {
            if (m == "set")    { out = {{ga[0], ga[1]}, ""}; return true; }
            if (m == "get")    { out = {{ga[0]}, ga[1]}; return true; }
            if (m == "has")    { out = {{ga[0]}, "bool"}; return true; }
            if (m == "remove") { out = {{ga[0]}, ""}; return true; }
            if (m == "len")    { out = {{}, "int"}; return true; }
        } else if (recv.rfind("Set<", 0) == 0 && ga.size() == 1) {
            if (m == "add")    { out = {{ga[0]}, "bool"}; return true; }
            if (m == "has")    { out = {{ga[0]}, "bool"}; return true; }
            if (m == "remove") { out = {{ga[0]}, ""}; return true; }
            if (m == "len")    { out = {{}, "int"}; return true; }
        }
        return false;
    }
};

} // namespace

void canonicalize_type_names(Program& prog) {
    for (auto& n : prog) canon_walk(n);
}

std::vector<xphage::diag::Diagnostic> annotate_numeric_types(Program& prog) {
    for (auto& n : prog) canon_walk(n);
    Typer t;
    t.run(prog);
    return std::move(t.diags);
}

} // namespace xphage::sema
