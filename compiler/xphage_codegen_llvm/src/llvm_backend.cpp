// ============================================================
// xphage_codegen_llvm — LLVM AST Visitor Backend v4.0.0
// Phase 1-3: complete codegen. No C++ intermediate.
// LLVM 16-21 compatible via version-gated macros.
// AeonCoreX Lab
// ============================================================
#include "../include/codegen_llvm.hpp"
#include "xphage/ast.hpp"
#include "../../xphage_lexer/include/lexer.hpp"
#include "../../xphage_parse/include/parser.hpp"

// ── non-LLVM stub (always compiled, safe fallback) ───────────
#ifndef ENABLE_LLVM

namespace xphage::codegen_llvm {
bool        is_available()     { return false; }
std::string llvm_version_str() { return "disabled"; }

LLVMResult compile_ast(const Program&, const std::string& o, const LLVMConfig&) {
    LLVMResult r; r.error = "LLVM backend not compiled in (rebuild with -DENABLE_LLVM=ON)"; return r;
}
LLVMResult compile_ir(const xphage::ir::IRModule&, const std::string& o, const LLVMConfig&) {
    LLVMResult r; r.error = "LLVM backend not compiled in"; return r;
}
bool link_binary(const std::vector<std::string>&, const std::string&,
                 const std::string&, bool) { return false; }
} // namespace

void XPhageLLVMCompiler::compile_tokens(const std::vector<Token>&, std::string) {
    fprintf(stderr, "[LLVM] disabled — rebuild with -DENABLE_LLVM=ON\n");
}

#else  // ENABLE_LLVM ─────────────────────────────────────────

// ── Version-gated LLVM includes ──────────────────────────────
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

// Host triple / target detection (moved in LLVM 17)
#if LLVM_VERSION_MAJOR >= 17
  #include <llvm/TargetParser/Host.h>
  #include <llvm/TargetParser/Triple.h>
#else
  #include <llvm/Support/Host.h>
  #include <llvm/ADT/Triple.h>
#endif

// CodeGenFileType moved in LLVM 18
#if LLVM_VERSION_MAJOR >= 18
  #include <llvm/Support/CodeGen.h>
  #define XP_OBJ_FILETYPE llvm::CodeGenFileType::ObjectFile
  #define XP_ASM_FILETYPE llvm::CodeGenFileType::AssemblyFile
#else
  #define XP_OBJ_FILETYPE llvm::CGFT_ObjectFile
  #define XP_ASM_FILETYPE llvm::CGFT_AssemblyFile
#endif

// Pass manager
#if LLVM_VERSION_MAJOR >= 16
  #include <llvm/Passes/PassBuilder.h>
  #include <llvm/Analysis/CGSCCPassManager.h>
  #include <llvm/Analysis/LoopAnalysisManager.h>
  #define XP_USE_NEW_PM 1
#endif
#include <llvm/IR/LegacyPassManager.h>

// Reloc model
#if LLVM_VERSION_MAJOR >= 16
  #include <optional>
  #define XP_RM std::optional<llvm::Reloc::Model>
#else
  #define XP_RM llvm::Optional<llvm::Reloc::Model>
#endif

#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <fstream>
#include <chrono>
#include <stdexcept>
#include <unordered_set>
#include <cctype>
#include <algorithm>
#include <functional>
#include <cstdlib>

using namespace llvm;

// ─────────────────────────────────────────────────────────────
// ASTCodegen — the visitor that turns AST → LLVM IR
// ─────────────────────────────────────────────────────────────
class ASTCodegen {
public:
    LLVMContext&      ctx;
    IRBuilder<>       B;
    Module&           M;

    // ── Symbol table: name → alloca ptr ───────────────────────
    struct Scope {
        std::unordered_map<std::string, Value*>    vars;      // name → alloca
        std::unordered_map<std::string, Type*>     var_types; // name → LLVM type
        std::unordered_map<std::string, Function*> fns;       // name → Function
        // name → SOURCE type string ("Vec<int>", "Vec<str>", ...). LLVM's
        // opaque pointers erase what a `ptr` points to, so a Vec<int> and
        // a Vec<str> are indistinguishable by LLVM type alone; the
        // element type is needed to read an element back correctly.
        // Lives in Scope so it shadows/pops exactly like vars/var_types.
        std::unordered_map<std::string, std::string> var_src;
    };
    std::vector<Scope>  scopes;
    // `use Realm::member` import aliasing — bare name -> fully
    // qualified name (e.g. "circle_area" -> "Geometry::circle_area").
    // Mirrors ir_lower.cpp's identical IRBuilder::use_aliases; see its
    // own comment for the full rationale. Populated by a pre-pass in
    // gen_module, before any real codegen, so a `use` appearing
    // anywhere resolves regardless of declaration order relative to
    // its call sites — consulted wherever an unqualified name is
    // resolved to a real Function (currently just CallExpr's callee
    // resolution; a bare-identifier VALUE reference to a qualified
    // const/atom/flux via `use` is a separate, not-yet-implemented
    // gap — see gen_module's UseDecl case).
    std::unordered_map<std::string, std::string> use_aliases;

    // Loop stack for break/continue
    struct LoopInfo { BasicBlock* cond; BasicBlock* end; };
    std::vector<LoopInfo> loop_stack;

    // xprt external function cache
    std::unordered_map<std::string, FunctionCallee> xprt_cache;

    // Forge struct types
    std::unordered_map<std::string, StructType*>        forge_types;

    // Enum (sum type) registry. An enum value is the LLVM struct
    //   %enum.<Name> = type { i32 tag, [W x i64] payload }
    // where W is the payload width, in 8-byte words, of the WIDEST variant.
    // Each variant lays its fields out at running word offsets. A scalar
    // (int, bool, double, or a str/Vec pointer) takes one word; a nested
    // enum is stored INLINE and takes its full struct width, so
    // Result<Option<int>, str> is a plain value with no hidden allocation.
    // Generic enums need nothing special: monomorphization has already
    // turned Option<int> into the concrete enum `Option__int` before this
    // backend runs, so it is registered and sized exactly like a
    // hand-written one. A directly recursive enum cannot be inline and is
    // rejected with a clear error (it needs an indirect payload; future work).
    struct EnumVariantInfo {
        std::string              name;
        unsigned                 tag;
        std::vector<std::string> field_types;  // source type strings, in order
        // Word (8-byte) offset and width of each field inside the payload
        // area. Scalars are 1 word; a nested enum is INLINE, so it takes
        // its whole struct width (1 tag word + its own payload words) —
        // no heap allocation, value semantics preserved.
        std::vector<unsigned>    field_off;
        std::vector<unsigned>    field_words;
    };
    struct EnumInfo {
        StructType*                  type = nullptr;
        unsigned                     slots = 0;   // payload width in words = widest variant
        std::vector<EnumVariantInfo> variants;
    };
    // Enum declarations by name, kept so an enum can be sized on demand
    // when another enum's layout depends on it (inner before outer).
    std::unordered_map<std::string, const ASTNode*> enum_decls;
    std::unordered_set<std::string>                 enum_sizing;  // cycle detection
    std::unordered_map<std::string, EnumInfo> enum_types;
    std::unordered_map<std::string, std::vector<std::string>> forge_fields;
    // forge -> field -> the AST of its `= default` expression, for fields a
    // `spawn` omits.
    std::unordered_map<std::string, std::unordered_map<std::string, const ASTNode*>> forge_defaults;
    // Method receivers: variable name ("self") -> the forge it points to.
    // `self` is a pointer parameter; every other forge variable is an
    // alloca holding the struct itself. Scoped to the function being
    // generated (see the guard in gen_pulse).
    std::unordered_map<std::string, std::string> self_forge_;

    // An addressable place: where a value lives, its LLVM type, and — when
    // that value is itself a forge — the forge's name.
    struct Place { Value* addr = nullptr; Type* ty = nullptr; std::string forge; };

    static std::string forge_name_of(Type* t) {
        if (auto* st = dyn_cast<StructType>(t)) {
            if (st->hasName()) {
                llvm::StringRef sn = st->getName();
                if (sn.starts_with("forge.")) return sn.substr(6).str();
            }
        }
        return "";
    }

    // Resolves an lvalue expression — `x`, `self`, `a.b`, `self.items.c` —
    // to the address of the storage it names. This one routine is what
    // field reads, field assignment and method-call receivers all share,
    // so a receiver such as `b.items` or `self.inner` addresses the REAL
    // object (never a copy). Returns an empty Place when the expression is
    // not an addressable forge place.
    Place gen_place(const ASTNode& n) {
        Place p;
        if (n.kind == NodeKind::Identifier) {
            Value* slot = lookup(n.value);
            if (!slot) return p;
            auto sit = self_forge_.find(n.value);
            if (sit != self_forge_.end()) {
                p.addr  = B.CreateLoad(opaque_ptr(), slot, n.value + ".ptr");
                p.forge = sit->second;
                auto ft = forge_types.find(p.forge);
                p.ty = ft != forge_types.end() ? (Type*)ft->second : nullptr;
                return p;
            }
            p.addr  = slot;
            p.ty    = lookup_type(n.value);
            p.forge = forge_name_of(p.ty);
            return p;
        }
        if (n.kind == NodeKind::MemberExpr && !n.children.empty() && n.children[0]) {
            Place base = gen_place(*n.children[0]);
            if (!base.addr || base.forge.empty()) return p;
            auto fit = forge_fields.find(base.forge);
            auto tit = forge_types.find(base.forge);
            if (fit == forge_fields.end() || tit == forge_types.end()) return p;
            for (size_t i = 0; i < fit->second.size(); i++) {
                if (fit->second[i] != n.value) continue;
                p.addr  = B.CreateStructGEP(tit->second, base.addr, (unsigned)i);
                p.ty    = tit->second->getElementType(i);
                p.forge = forge_name_of(p.ty);
                return p;
            }
        }
        return p;
    }

    // Name of the forge an expression evaluates to, resolved WITHOUT emitting
    // any IR — for type inference at a declaration (`atom c = b.rebox(9)`),
    // where the result's type must be known before the initializer is
    // generated. Handles variables, `self`, and field chains.
    std::string static_forge_of(const ASTNode& e) {
        if (e.kind == NodeKind::Identifier) {
            auto sit = self_forge_.find(e.value);
            if (sit != self_forge_.end()) return sit->second;
            if (!lookup(e.value)) return "";
            return forge_name_of(lookup_type(e.value));
        }
        if (e.kind == NodeKind::MemberExpr && !e.children.empty() && e.children[0]) {
            std::string base = static_forge_of(*e.children[0]);
            if (base.empty()) return "";
            auto fit = forge_fields.find(base);
            auto tit = forge_types.find(base);
            if (fit == forge_fields.end() || tit == forge_types.end()) return "";
            for (size_t i = 0; i < fit->second.size(); i++)
                if (fit->second[i] == e.value)
                    return forge_name_of(tit->second->getElementType(i));
        }
        return "";
    }

    // The LLVM type an expression evaluates to, derived WITHOUT emitting IR
    // (nullptr when it cannot be determined). Used to size the slot of an
    // un-annotated `atom x = <expr>` before the initializer is generated:
    // the old rule was a growing list of recognised initializer shapes with
    // an `i64` fallback, so any other shape — `"pe" + "ar"`, `a * 1.5`,
    // `p.name`, `obj.len()` — silently got an integer slot and the first
    // use of it was ill-typed IR (or, worse, a wrong value).
    Type* static_type_of(const ASTNode& e) {
        switch (e.kind) {
            case NodeKind::IntLit:     return B.getInt64Ty();
            case NodeKind::FloatLit:   return B.getDoubleTy();
            case NodeKind::BoolLit:    return B.getInt1Ty();
            case NodeKind::StringLit:
            case NodeKind::FStringLit: return xp_type("str");
            case NodeKind::Identifier:
                if (self_forge_.count(e.value)) return opaque_ptr();
                if (!lookup(e.value)) return nullptr;
                return lookup_type(e.value);
            case NodeKind::UnaryOp:
                if (e.value == "!" || e.value == "not") return B.getInt1Ty();
                if (!e.children.empty() && e.children[0]) return static_type_of(*e.children[0]);
                return nullptr;
            case NodeKind::BinaryOp: {
                const std::string& op = e.value;
                if (op == "&&" || op == "||" || op == "and" || op == "or" || op == "==" || op == "!=" ||
                    op == "<" || op == "<=" || op == ">" || op == ">=") return B.getInt1Ty();
                if (e.children.size() < 2 || !e.children[0] || !e.children[1]) return nullptr;
                Type* l = static_type_of(*e.children[0]);
                Type* r = static_type_of(*e.children[1]);
                if ((l && l->isDoubleTy()) || (r && r->isDoubleTy())) return B.getDoubleTy();
                if (op == "+" && ((l && l->isPointerTy()) || (r && r->isPointerTy())))
                    return xp_type("str");
                Type* t = l ? l : r;
                if (t && t->isIntegerTy(1)) return B.getInt64Ty();   // bool in arithmetic
                return t;
            }
            case NodeKind::CallExpr: {
                if (e.children.empty() || !e.children[0]) return nullptr;
                const ASTNode& c = *e.children[0];
                if (c.kind == NodeKind::Identifier || c.kind == NodeKind::PathExpr) {
                    Function* f = lookup_fn(c.value);
                    if (!f) f = M.getFunction(c.value);
                    if (f && !f->getReturnType()->isVoidTy()) return f->getReturnType();
                    return nullptr;
                }
                if (Function* mf = method_callee_of(e))
                    if (!mf->getReturnType()->isVoidTy()) return mf->getReturnType();
                return nullptr;
            }
            case NodeKind::MemberExpr: {
                if (e.children.empty() || !e.children[0]) return nullptr;
                std::string base = static_forge_of(*e.children[0]);
                if (base.empty()) return nullptr;
                auto fit = forge_fields.find(base);
                auto tit = forge_types.find(base);
                if (fit == forge_fields.end() || tit == forge_types.end()) return nullptr;
                for (size_t i = 0; i < fit->second.size(); i++)
                    if (fit->second[i] == e.value) return tit->second->getElementType(i);
                return nullptr;
            }
            default: return nullptr;
        }
    }

    // The Function an `obj.method(...)` call resolves to, or null.
    Function* method_callee_of(const ASTNode& call) {
        if (call.kind != NodeKind::CallExpr || call.children.empty() || !call.children[0] ||
            call.children[0]->kind != NodeKind::MemberExpr ||
            call.children[0]->children.empty() || !call.children[0]->children[0]) return nullptr;
        const ASTNode& c = *call.children[0];
        std::string recv = static_forge_of(*c.children[0]);
        if (recv.empty()) return nullptr;
        std::string nm = recv + "__" + c.value;
        if (Function* f = lookup_fn(nm)) return f;
        return M.getFunction(nm);
    }

    // Value coercions shared by call arguments, field stores and defaults.
    Value* coerce_value(Value* v, Type* want) {
        Type* have = v->getType();
        if (have == want) return v;
        if (want->isStructTy() && have->isPointerTy()) return B.CreateLoad(want, v);
        if (want->isDoubleTy() && have->isIntegerTy()) return B.CreateSIToFP(v, want);
        if (want->isIntegerTy(64) && have->isIntegerTy(1)) return B.CreateZExt(v, want);
        if (want->isIntegerTy(64) && have->isDoubleTy()) return B.CreateFPToSI(v, want);
        return v;
    }

    explicit ASTCodegen(LLVMContext& c, Module& m)
        : ctx(c), B(c), M(m) {
        scopes.push_back({});
    }

    // ── Type helpers ──────────────────────────────────────────
    Type* xp_type(const std::string& t) {
        // Sized numeric types: exact LLVM widths. LLVM integers carry no
        // signedness — it comes from the AST's `ty`/`opty` annotations (see
        // numeric_types.hpp) and selects sdiv/udiv, icmp slt/ult, ashr/lshr...
        if (t == "i8"  || t == "u8")                       return B.getInt8Ty();
        if (t == "i16" || t == "u16")                      return B.getInt16Ty();
        if (t == "i32" || t == "u32")                      return B.getInt32Ty();
        if (t == "isize" || t == "u64" || t == "usize")    return B.getInt64Ty();
        if (t == "f32")                                    return B.getFloatTy();
        if (t == "int"  || t == "long long" || t.empty()) return B.getInt64Ty();
        if (t == "float"|| t == "double")                  return B.getDoubleTy();
        if (t == "bool")                                   return B.getInt1Ty();
        if (t == "str")                                    return xpstr_ptr_ty();
        if (t == "void")                                   return B.getVoidTy();
        // Raw pointer (FFI): *mut T / *const T → opaque pointer.
        // LLVM's opaque-pointer model means the pointee type doesn't
        // need to be resolved here — any pointer is just `ptr`.
        if (t.size() > 1 && t[0] == '*')                    return opaque_ptr();
        if (forge_types.count(t))                          return forge_types.at(t);
        if (enum_types.count(t))                           return enum_types.at(t).type;
        if (!vec_elem_type(t).empty())                     return opaque_ptr();
        // Ownership qualifiers fold into the type string ("mut_ref Vec<int>").
        if (t.compare(0, 8, "mut_ref ") == 0)              return xp_type(t.substr(8));
        if (t.compare(0, 4, "ref ") == 0)                  return xp_type(t.substr(4));
        if (t.compare(0, 4, "own ") == 0)                  return xp_type(t.substr(4));
        // A mangled generic specialization ("Option__int") that reached
        // here unregistered would otherwise silently become an i64 and
        // miscompile. Every legitimate one is registered in Pass 1, so
        // this can only mean the monomorphizer emitted a use without its
        // declaration — surface that instead of computing garbage.
        if (t.find("__") != std::string::npos && !t.empty() && std::isupper((unsigned char)t[0]))
            throw std::runtime_error("unresolved specialized type '" + t +
                                     "' (declaration missing from the program)");
        return B.getInt64Ty(); // default
    }

    // XpStr* is an opaque ptr in LLVM 17+, i8* before
    Type* xpstr_ptr_ty() {
#if LLVM_VERSION_MAJOR >= 17
        return PointerType::getUnqual(ctx);
#else
        return PointerType::getUnqual(Type::getInt8Ty(ctx));
#endif
    }

    Type* opaque_ptr() {
#if LLVM_VERSION_MAJOR >= 17
        return PointerType::getUnqual(ctx);
#else
        return PointerType::getUnqual(Type::getInt8Ty(ctx));
#endif
    }

    // ── Scope helpers ─────────────────────────────────────────
    void push_scope() { scopes.push_back({}); }
    void pop_scope()  { scopes.pop_back(); }

    Value* lookup(const std::string& name) {
        for (int i = (int)scopes.size()-1; i >= 0; i--) {
            auto it = scopes[i].vars.find(name);
            if (it != scopes[i].vars.end()) return it->second;
        }
        return nullptr;
    }

    Type* lookup_type(const std::string& name) {
        for (int i = (int)scopes.size()-1; i >= 0; i--) {
            auto it = scopes[i].var_types.find(name);
            if (it != scopes[i].var_types.end()) return it->second;
        }
        return B.getInt64Ty();
    }

    Function* lookup_fn(const std::string& name) {
        for (int i = (int)scopes.size()-1; i >= 0; i--) {
            auto it = scopes[i].fns.find(name);
            if (it != scopes[i].fns.end()) return it->second;
        }
        if (auto* f = M.getFunction(name)) return f;
        return nullptr;
    }

    void def_var(const std::string& n, Value* v, Type* t) {
        scopes.back().vars[n] = v;
        scopes.back().var_types[n] = t;
    }
    void def_var_src(const std::string& n, const std::string& src) {
        scopes.back().var_src[n] = src;
    }
    std::string lookup_src(const std::string& n) {
        for (int i = (int)scopes.size()-1; i >= 0; i--) {
            // A name defined in an inner scope WITHOUT a recorded source
            // type must not inherit an outer variable's, so stop at the
            // first scope that defines the variable at all.
            if (scopes[i].vars.count(n)) {
                auto it = scopes[i].var_src.find(n);
                return it != scopes[i].var_src.end() ? it->second : std::string();
            }
        }
        return std::string();
    }
    // "Vec<int>" -> "int"; empty when `t` is not a Vec<...> type string.
    static std::string vec_elem_type(std::string t) {
        for (;;) {   // strip ownership qualifiers the parser folds into types
            if (t.compare(0, 4, "own ") == 0)          t = t.substr(4);
            else if (t.compare(0, 4, "ref ") == 0)     t = t.substr(4);
            else if (t.compare(0, 8, "mut_ref ") == 0) t = t.substr(8);
            else break;
        }
        if (t.size() > 5 && t.compare(0, 4, "Vec<") == 0 && t.back() == '>')
            return t.substr(4, t.size() - 5);
        return std::string();
    }

    void def_fn(const std::string& n, Function* f) {
        scopes.back().fns[n] = f;
    }

    // ── xprt function declarations (lazy) ─────────────────────
    FunctionCallee xprt(const std::string& name,
                        Type* ret,
                        std::vector<Type*> params,
                        bool variadic = false) {
        auto it = xprt_cache.find(name);
        if (it != xprt_cache.end()) return it->second;
        auto ft = FunctionType::get(ret, params, variadic);
        auto fc = M.getOrInsertFunction(name, ft);
        xprt_cache[name] = fc;
        return fc;
    }

    // Common xprt shorthands
    FunctionCallee xprt_str_new() {
        return xprt("xprt_str_new", xpstr_ptr_ty(), {opaque_ptr()});
    }
    FunctionCallee xprt_beam_str() {
        return xprt("xprt_beam_str", B.getVoidTy(), {xpstr_ptr_ty()});
    }
    FunctionCallee xprt_beam_int() {
        return xprt("xprt_beam_int", B.getVoidTy(), {B.getInt64Ty()});
    }
    FunctionCallee xprt_beam_float() {
        return xprt("xprt_beam_float", B.getVoidTy(), {B.getDoubleTy()});
    }
    FunctionCallee xprt_beam_bool() {
        // Runtime ABI is `void xprt_beam_bool(int v)`: a 32-bit int (declared
        // as i1 and called with an i64 — the verifier rejected every
        // `beam <bool expression>`; same fix as xprt_str_from_bool).
        return xprt("xprt_beam_bool", B.getVoidTy(), {B.getInt32Ty()});
    }
    FunctionCallee xprt_str_concat() {
        return xprt("xprt_str_concat", xpstr_ptr_ty(),
                    {xpstr_ptr_ty(), xpstr_ptr_ty()});
    }
    FunctionCallee xprt_str_from_int() {
        return xprt("xprt_str_from_int", xpstr_ptr_ty(), {B.getInt64Ty()});
    }
    FunctionCallee xprt_str_from_float() {
        return xprt("xprt_str_from_float", xpstr_ptr_ty(), {B.getDoubleTy()});
    }
    FunctionCallee xprt_str_from_bool() {
        // The C runtime signature is `XpStr* xprt_str_from_bool(int v)`, a
        // 32-bit int. This was declared as i1 and then called with an i64
        // (three different widths for one argument), which the LLVM
        // verifier rejects on every bool interpolation. It only produced
        // correct output by accident of x86-64 register passing. Declare
        // and call it with the runtime's actual ABI: i32.
        return xprt("xprt_str_from_bool", xpstr_ptr_ty(), {B.getInt32Ty()});
    }
    FunctionCallee xprt_str_eq_cstr() {
        return xprt("xprt_str_eq_cstr", B.getInt1Ty(),
                    {xpstr_ptr_ty(), opaque_ptr()});
    }

    // ── String literal → global constant + xprt_str_new ──────
    Value* make_str_literal(const std::string& s) {
        // Create a global constant char array
        Constant* cstr = ConstantDataArray::getString(ctx, s, true);
        auto* gv = new GlobalVariable(M, cstr->getType(), true,
                    GlobalValue::PrivateLinkage, cstr, ".xpstr");
        gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        // Decay to i8* / ptr
        Value* ptr = B.CreateConstGEP2_32(cstr->getType(), gv, 0, 0);
        return B.CreateCall(xprt_str_new(), {ptr});
    }

    // ── f-string builder ─────────────────────────────────────
    // Parses raw f-string content, builds XpStr by concatenation
    Value* build_fstring(const std::string& raw, Function* fn,
                         const std::vector<ASTNodePtr>* parts = nullptr) {
        size_t part_k = 0;
        // Start with empty XpStr*
        Value* result = make_str_literal("");
        std::string cur_lit;
        bool in_expr = false;
        std::string expr_buf;

        auto flush_lit = [&]() {
            if (cur_lit.empty()) return;
            Value* part = make_str_literal(cur_lit);
            result = B.CreateCall(xprt_str_concat(), {result, part});
            cur_lit.clear();
        };

        int brace_depth = 0;   // same brace-depth segmentation as the parser and the other backends
        for (size_t i = 0; i < raw.size(); i++) {
            if (!in_expr) {
                if (raw[i] == '{') { flush_lit(); in_expr = true; brace_depth = 1; }
                else cur_lit += raw[i];
            } else {
                if (raw[i] == '{') { brace_depth++; expr_buf += raw[i]; }
                else if (raw[i] == '}' && --brace_depth > 0) { expr_buf += raw[i]; }
                else if (raw[i] == '}') {
                    // Evaluate expr_buf as an arbitrary expression —
                    // NOT just a bare known-identifier or an inline
                    // integer constant, which is all the previous
                    // version of this code handled (anything else,
                    // including a function call like `fib(10)`, a
                    // field access like `r.val`, or a qualified call
                    // like `Geometry::length_sq(v)`, silently fell
                    // into the catch(...) below and produced a literal
                    // "?...?" placeholder string instead — caught by
                    // running the golden suite against real generated
                    // output: every golden test uses an f-string with
                    // at least one non-trivial interpolation, and every
                    // one of them showed this exact placeholder text
                    // in its output). Fixed the same way ir_lower.cpp's
                    // lower_fstring_expr already does for the XIL
                    // backend: re-lex and re-parse the interpolation's
                    // raw source text with the real Lexer + Parser,
                    // then lower the resulting expression node through
                    // this class's own normal expression codegen
                    // (gen_expr) — rather than hand-rolling a second,
                    // much more limited mini-parser inline here.
                    std::string trimmed = expr_buf;
                    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(0,1);
                    while (!trimmed.empty() && trimmed.back()  == ' ') trimmed.pop_back();
                    Value* ev = nullptr;

                    // the pre-parsed interpolation (parser's attach_fstring_parts), if any
                    ASTNodePtr pre = (parts && part_k < parts->size()) ? (*parts)[part_k] : nullptr;
                    part_k++;
                    if (!trimmed.empty()) {
                        xphage::lexer::Lexer lex(trimmed, "<fstring>");
                        auto toks = pre ? std::vector<Token>() : lex.tokenize();
                        if (pre || !lex.has_errors()) {
                            xphage::parse::Parser parser(toks, "<fstring>");
                            ASTNodePtr node = pre ? pre : parser.parse_single_expression();
                            if ((pre || !parser.has_errors()) && node) {
                                Value* raw_val = gen_expr(*node);
                                bool is_uns = false;
                                if (raw_val) raw_val = widen_for_show(raw_val, attr_ty(*node), is_uns);
                                if (raw_val) {
                                    Type* t = raw_val->getType();
                                    if (t == B.getInt64Ty() && is_uns)
                                        ev = B.CreateCall(xprt("xprt_str_from_uint", xpstr_ptr_ty(), {B.getInt64Ty()}), {raw_val});
                                    else if (t == B.getInt64Ty())
                                        ev = B.CreateCall(xprt_str_from_int(), {raw_val});
                                    else if (t == B.getDoubleTy())
                                        ev = B.CreateCall(xprt_str_from_float(), {raw_val});
                                    else if (t == B.getInt1Ty())
                                        ev = B.CreateCall(xprt_str_from_bool(),
                                                           {B.CreateZExt(raw_val, B.getInt32Ty())});
                                    else
                                        ev = raw_val; // already XpStr*
                                }
                            }
                        }
                    }
                    // Best-effort: drop an unparseable/unresolvable
                    // segment as an empty contribution rather than
                    // emit invalid IR or crash — mirrors
                    // lower_fstring_expr's own "return empty string"
                    // fallback on the XIL side. This should only ever
                    // be reached for a genuinely malformed
                    // interpolation now, not for any ordinary
                    // expression shape.
                    if (!ev) ev = make_str_literal("");
                    result = B.CreateCall(xprt_str_concat(), {result, ev});
                    expr_buf.clear();
                    in_expr = false;
                } else {
                    expr_buf += raw[i];
                }
            }
        }
        flush_lit();
        return result;
    }

    // ── Load a value (auto-deref alloca) ──────────────────────
    Value* load_var(const std::string& name) {
        Value* ptr = lookup(name);
        if (!ptr) return B.getInt64(0);
        return B.CreateLoad(lookup_type(name), ptr, name);
    }

    // ── Expression codegen ────────────────────────────────────
    Value* gen_expr(const ASTNode& n) {
        switch (n.kind) {
            case NodeKind::IntLit: {
                auto t = n.attrs.find("ty");
                if (t != n.attrs.end()) {            // typed (sized) literal: exactly its type
                    Type* T = xp_type(t->second);
                    unsigned long long v = std::stoull(n.value);
                    if (T->isFloatingPointTy()) return ConstantFP::get(T, (double)v);
                    return ConstantInt::get(T, v, false);
                }
                return B.getInt64((uint64_t)std::stoull(n.value));   // legacy `int`: up to 2^64-1, wraps
            }
            case NodeKind::FloatLit: {
                auto t = n.attrs.find("ty");
                return ConstantFP::get(t != n.attrs.end() ? xp_type(t->second) : B.getDoubleTy(),
                                       std::stod(n.value));
            }
            case NodeKind::BoolLit:
                return B.getInt1(n.value == "true" ? 1 : 0);
            case NodeKind::NullLit:
                return Constant::getNullValue(opaque_ptr());
            case NodeKind::StringLit:
                return make_str_literal(n.value);
            case NodeKind::FStringLit:
                return build_fstring(n.value, B.GetInsertBlock()->getParent(), &n.children);

            case NodeKind::Identifier: {
                Value* ptr = lookup(n.value);
                if (!ptr) {
                    // Could be a zero-arg function call shorthand
                    Function* f = lookup_fn(n.value);
                    if (f && f->arg_size() == 0)
                        return B.CreateCall(f, {});
                    return B.getInt64(0);
                }
                return B.CreateLoad(lookup_type(n.value), ptr, n.value);
            }

            case NodeKind::BinaryOp: {
                if (n.children.size() < 2) return B.getInt64(0);
                if (n.value == "&&" || n.value == "and") return gen_short_circuit(n, true);
                if (n.value == "||" || n.value == "or")  return gen_short_circuit(n, false);
                Value* L = gen_expr(*n.children[0]);
                Value* R = gen_expr(*n.children[1]);
                const std::string& op = n.value;
                {   // sized operands: the typer recorded the type both sides share
                    auto ot = n.attrs.find("opty");
                    if (ot != n.attrs.end())
                        if (Value* tv = typed_binop(op, ot->second, L, R)) return tv;
                }

                // Determine dominant type
                bool is_float = L->getType()->isDoubleTy() ||
                                R->getType()->isDoubleTy();
                bool is_bool  = op=="&&"||op=="||"||
                                op=="=="||op=="!="||op=="<"||op=="<="||
                                op==">"||op==">="||op=="and"||op=="or";

                // Coerce int → float if needed
                if (is_float) {
                    if (L->getType()->isIntegerTy())
                        L = B.CreateSIToFP(L, B.getDoubleTy());
                    if (R->getType()->isIntegerTy())
                        R = B.CreateSIToFP(R, B.getDoubleTy());
                }
                // Coerce i1 → i64 for arithmetic
                if (!is_bool && !is_float) {
                    if (L->getType()->isIntegerTy(1))
                        L = B.CreateZExt(L, B.getInt64Ty());
                    if (R->getType()->isIntegerTy(1))
                        R = B.CreateZExt(R, B.getInt64Ty());
                }

                // `+` on two strings is concatenation (runtime XpStr objects, so
                // the integer add below produced `add ptr`, which LLVM's own
                // verifier rejects: every program that joined two strings with
                // `+` failed to compile on this backend).
                if (op == "+" && L->getType()->isPointerTy() && R->getType()->isPointerTy())
                    return B.CreateCall(xprt_str_concat(), {L, R});
                if (op == "+") return is_float ? B.CreateFAdd(L,R) : B.CreateAdd(L,R);
                if (op == "-") return is_float ? B.CreateFSub(L,R) : B.CreateSub(L,R);
                if (op == "*") return is_float ? B.CreateFMul(L,R) : B.CreateMul(L,R);
                if (op == "/") return is_float ? B.CreateFDiv(L,R) : B.CreateSDiv(L,R);
                if (op == "%") return B.CreateSRem(L,R);

                // Two string values compare by CONTENT. They are pointers to
                // runtime XpStr objects, so the plain integer compare below
                // would compare ADDRESSES: `s == "1"` was false whenever the
                // literal in the callee and the argument were different
                // objects (always, across functions) — a silent wrong answer.
                // A compare against a literal null stays a pointer compare.
                if ((op == "==" || op == "!=" || op == "eq") &&
                    L->getType()->isPointerTy() && R->getType()->isPointerTy() &&
                    !isa<ConstantPointerNull>(L) && !isa<ConstantPointerNull>(R)) {
                    Value* rc = B.CreateCall(xprt("xprt_str_eq", B.getInt32Ty(),
                                                  {xpstr_ptr_ty(), xpstr_ptr_ty()}), {L, R});
                    Value* eq = B.CreateICmpNE(rc, B.getInt32(0));
                    return op == "!=" ? B.CreateNot(eq) : eq;
                }
                // Ordering of two strings: the sign of xprt_str_cmp (byte-wise
                // lexicographic, like std::string::compare) against 0. Before,
                // `<` on two strings either compared ADDRESSES (silently wrong:
                // `max("pear", "apple")` returned "apple") or asserted inside
                // LLVM when the operand types differed.
                if ((op == "<" || op == "<=" || op == ">" || op == ">=") &&
                    L->getType()->isPointerTy() && R->getType()->isPointerTy()) {
                    Value* rc = B.CreateCall(xprt("xprt_str_cmp", B.getInt32Ty(),
                                                  {xpstr_ptr_ty(), xpstr_ptr_ty()}), {L, R});
                    Value* zero = B.getInt32(0);
                    if (op == "<")  return B.CreateICmpSLT(rc, zero);
                    if (op == "<=") return B.CreateICmpSLE(rc, zero);
                    if (op == ">")  return B.CreateICmpSGT(rc, zero);
                    return B.CreateICmpSGE(rc, zero);
                }
                if (op == "=="||op=="eq") return is_float ? B.CreateFCmpOEQ(L,R):B.CreateICmpEQ(L,R);
                if (op == "!=")           return is_float ? B.CreateFCmpONE(L,R):B.CreateICmpNE(L,R);
                if (op == "<")            return is_float ? B.CreateFCmpOLT(L,R):B.CreateICmpSLT(L,R);
                if (op == "<=")           return is_float ? B.CreateFCmpOLE(L,R):B.CreateICmpSLE(L,R);
                if (op == ">")            return is_float ? B.CreateFCmpOGT(L,R):B.CreateICmpSGT(L,R);
                if (op == ">=")           return is_float ? B.CreateFCmpOGE(L,R):B.CreateICmpSGE(L,R);

                if (op == "&&"||op=="and") {
                    if (!L->getType()->isIntegerTy(1)) L = B.CreateICmpNE(L, B.getInt64(0));
                    if (!R->getType()->isIntegerTy(1)) R = B.CreateICmpNE(R, B.getInt64(0));
                    return B.CreateAnd(L, R);
                }
                if (op == "||"||op=="or") {
                    if (!L->getType()->isIntegerTy(1)) L = B.CreateICmpNE(L, B.getInt64(0));
                    if (!R->getType()->isIntegerTy(1)) R = B.CreateICmpNE(R, B.getInt64(0));
                    return B.CreateOr(L, R);
                }
                // Bitwise operators (integer only)
                if (op == "&")  return B.CreateAnd(L, R);
                if (op == "|")  return B.CreateOr(L, R);
                if (op == "^")  return B.CreateXor(L, R);
                if (op == "<<") return B.CreateShl(L, R);
                if (op == ">>") return B.CreateAShr(L, R); // arithmetic right shift
                return B.getInt64(0);
            }

            case NodeKind::UnaryOp: {
                if (n.children.empty()) return B.getInt64(0);
                Value* V = gen_expr(*n.children[0]);
                if (n.value == "!") {
                    if (!V->getType()->isIntegerTy(1))
                        V = B.CreateICmpNE(V, B.getInt64(0));
                    return B.CreateNot(V);
                }
                if (n.value == "-") {
                    if (V->getType()->isFloatingPointTy()) return B.CreateFNeg(V);
                    return B.CreateNeg(V);
                }
                if (n.value == "~") {      // bitwise NOT
                    return B.CreateNot(V);
                }
                return V;
            }

            case NodeKind::AssignExpr: {
                if (n.children.size() < 2) return B.getInt64(0);
                std::string op   = n.value; // = += -= *= /=
                std::string name;
                if (n.children[0]->kind == NodeKind::Identifier)
                    name = n.children[0]->value;

                Value* rhs = gen_expr(*n.children[1]);
                Value* ptr = name.empty() ? nullptr : lookup(name);
                Type*  t   = nullptr;
                if (ptr) t = lookup_type(name);
                if (n.children[0]->kind == NodeKind::MemberExpr) {
                    // `p.x = v`, `self.n += 1`, `h.c.n = v`: store through the
                    // field's real address. This case used to fall into
                    // `if (!ptr) return rhs;` — the assignment compiled to
                    // NOTHING and the program ran with the field unchanged.
                    Place fp = gen_place(*n.children[0]);
                    if (!fp.addr || !fp.ty)
                        throw std::runtime_error("cannot assign to `." + n.children[0]->value +
                            "`: the target is not an addressable forge field on the LLVM backend");
                    ptr  = fp.addr;
                    t    = fp.ty;
                    name = n.children[0]->value;
                }
                if (!ptr) return rhs;

                Value* val = rhs;
                if (op != "=" && n.attrs.count("opty")) {
                    // sized left-hand side: the whole operation happens in its type
                    Value* cur = B.CreateLoad(t, ptr, name);
                    if (Value* tv = typed_binop(op.substr(0, op.size() - 1), n.attrs.at("opty"), cur, rhs))
                        val = tv;
                } else if (op != "=") {
                    Value* cur = B.CreateLoad(t, ptr, name);
                    if      (op == "+=") val = t->isDoubleTy() ? B.CreateFAdd(cur,rhs):B.CreateAdd(cur,rhs);
                    else if (op == "-=") val = t->isDoubleTy() ? B.CreateFSub(cur,rhs):B.CreateSub(cur,rhs);
                    else if (op == "*=") val = t->isDoubleTy() ? B.CreateFMul(cur,rhs):B.CreateMul(cur,rhs);
                    else if (op == "/=") val = t->isDoubleTy() ? B.CreateFDiv(cur,rhs):B.CreateSDiv(cur,rhs);
                    else {
                        // %= &= |= ^= <<= >>= : used to fall through and leave the
                        // variable UNCHANGED. They are exactly typed_binop's operators.
                        std::string nm = t->isDoubleTy() ? "float" : t->isIntegerTy(64) ? "int" : name_of_llvm(t);
                        Value* tv = nm.empty() ? nullptr : typed_binop(op.substr(0, op.size() - 1), nm, cur, rhs);
                        if (!tv)
                            throw std::runtime_error("compound assignment `" + op + "` is not supported by the LLVM backend for this operand type");
                        val = tv;
                    }
                }
                // Coerce if needed
                if (t != val->getType()) {
                    if (t->isDoubleTy() && val->getType()->isIntegerTy())
                        val = B.CreateSIToFP(val, B.getDoubleTy());
                    else if (t->isIntegerTy(64) && val->getType()->isIntegerTy(1))
                        val = B.CreateZExt(val, B.getInt64Ty());
                    else if (t->isIntegerTy(64) && val->getType()->isDoubleTy())
                        val = B.CreateFPToSI(val, B.getInt64Ty());
                }
                B.CreateStore(val, ptr);
                return val;
            }

            case NodeKind::CallExpr: {
                if (n.children.empty()) return B.getInt64(0);
                // `Option__int.Some(42)` — enum variant construction. The
                // callee is a MemberExpr whose object names a registered
                // enum; anything else falls through to normal call handling.
                if (n.children[0] && n.children[0]->kind == NodeKind::MemberExpr &&
                    !n.children[0]->children.empty() && n.children[0]->children[0] &&
                    n.children[0]->children[0]->kind == NodeKind::Identifier) {
                    std::vector<ASTNodePtr> cargs(n.children.begin() + 1, n.children.end());
                    if (Value* ev = gen_enum_construct(n.children[0]->children[0]->value,
                                                       n.children[0]->value, cargs)) {
                        return ev;
                    }
                }
                // Method call on a Vec<T> receiver: `xs.push(x)`, `xs.get(i)`,
                // `xs.len()`. Previously ANY method call fell through to the
                // "unknown callee" fallback below and silently evaluated to
                // 0, so every Vec operation was a no-op that still reported
                // success (len=0, get=0) — a silent miscompile.
                if (n.children[0] && n.children[0]->kind == NodeKind::MemberExpr &&
                    !n.children[0]->children.empty() && n.children[0]->children[0] &&
                    n.children[0]->children[0]->kind == NodeKind::Identifier) {
                    const std::string& recv = n.children[0]->children[0]->value;
                    std::string elem = vec_elem_type(lookup_src(recv));
                    if (!elem.empty()) {
                        const std::string& method = n.children[0]->value;
                        Value* vptr = B.CreateLoad(opaque_ptr(), lookup(recv), recv);
                        if (method == "push") {
                            if (n.children.size() != 2)
                                throw std::runtime_error("Vec.push takes exactly 1 argument");
                            Value* item = gen_expr(*n.children[1]);
                            B.CreateCall(xprt("xprt_vec_push", B.getVoidTy(), {opaque_ptr(), opaque_ptr()}),
                                         {vptr, B.CreateIntToPtr(to_slot(item), opaque_ptr())});
                            return B.getInt64(0);
                        }
                        if (method == "len" || method == "size") {
                            return B.CreateCall(xprt("xprt_vec_len", B.getInt64Ty(), {opaque_ptr()}), {vptr});
                        }
                        if (method == "get" || method == "at") {
                            if (n.children.size() != 2)
                                throw std::runtime_error("Vec.get takes exactly 1 argument");
                            Value* idx = gen_expr(*n.children[1]);
                            Value* raw = B.CreateCall(
                                xprt("xprt_vec_get", opaque_ptr(), {opaque_ptr(), B.getInt64Ty()}), {vptr, idx});
                            return from_slot(B.CreatePtrToInt(raw, B.getInt64Ty()), elem);
                        }
                        throw std::runtime_error("Vec has no method '" + method +
                                                 "' in the LLVM backend yet");
                    }
                }
                std::string fname;
                if (n.children[0]->kind == NodeKind::Identifier) {
                    fname = n.children[0]->value;
                } else if (n.children[0]->kind == NodeKind::PathExpr) {
                    // A qualified realm call (`Geometry::length_sq(v)`)
                    // — the parser already assembles the full
                    // "Realm::name" string directly onto the PathExpr
                    // node's own value (see parse_primary's
                    // COLON_COLON loop, and ir_lower.cpp's identical
                    // handling of this same node kind, which this
                    // mirrors), and gen_pulse/gen_module already
                    // declare a realm-nested pulse's LLVM Function
                    // under that exact qualified name (see RealmDecl's
                    // own fix below). Previously unhandled here
                    // entirely — fname stayed empty and this always
                    // fell through to the `!fn` "return B.getInt64(0)"
                    // fallback below, explaining why every qualified
                    // call always evaluated to 0 with no error at all.
                    fname = n.children[0]->value;
                }
                // A bare name imported via `use Realm::name` resolves
                // to its qualified name — e.g. `circle_area(2.0)`
                // after `use Geometry::circle_area` needs to look up
                // the Function actually declared as
                // "Geometry::circle_area" (per RealmDecl's own
                // qualification), not a nonexistent bare
                // "circle_area". Checked only when the direct/
                // unqualified lookup below would otherwise fail
                // (lookup_fn/M.getFunction(fname) first, alias as a
                // fallback) so an ordinary same-name local function
                // (if one somehow existed) would still take priority
                // over an alias — matching ir_lower.cpp's identical
                // alias-as-fallback ordering for Identifier lowering.
                // `panic(msg)`: the always-available abort primitive (see the C++
                // backends' `panic`). Lowered to the runtime's xprt_panic, which
                // prints to stderr and exits with status 101.
                if (fname == "panic" && !lookup_fn("panic") && n.children.size() == 2) {
                    Value* m = gen_expr(*n.children[1]);
                    B.CreateCall(xprt("xprt_panic", B.getVoidTy(), {xpstr_ptr_ty()}), {m});
                    return B.getInt64(0);
                }
                // `obj.method(args)` on a forge place: call `Forge__method`
                // with the object's ADDRESS as the receiver (a method mutates
                // the real object, never a copy). Receivers that are not
                // plain places fall through to the explicit "not supported"
                // diagnostic below; Vec methods and enum construction were
                // already handled above.
                if (n.children[0]->kind == NodeKind::MemberExpr && !n.children[0]->children.empty() &&
                    n.children[0]->children[0]) {
                    const ASTNode& callee = *n.children[0];
                    Place rp = gen_place(*callee.children[0]);
                    if (rp.addr && !rp.forge.empty()) {
                        std::string mname = rp.forge + "__" + callee.value;
                        Function* mf = lookup_fn(mname);
                        if (!mf) mf = M.getFunction(mname);
                        if (mf) {
                            std::vector<Value*> margs = { rp.addr };
                            for (size_t i = 1; i < n.children.size(); i++) {
                                if (!n.children[i]) continue;
                                Value* a = gen_expr(*n.children[i]);
                                if (margs.size() < mf->arg_size())
                                    a = coerce_value(a, mf->getFunctionType()->getParamType((unsigned)margs.size()));
                                margs.push_back(a);
                            }
                            return B.CreateCall(mf, margs);
                        }
                    }
                }
                Function* fn = lookup_fn(fname);
                if (!fn) fn = M.getFunction(fname);
                if (!fn) {
                    auto uit = use_aliases.find(fname);
                    if (uit != use_aliases.end()) {
                        // lookup_fn first, not M.getFunction directly
                        // — see gen_pulse's own def_fn(name, fn) call:
                        // every pulse's Function is actually created
                        // TWICE (once as an empty forward-declaration
                        // in Pass 2 below, keeping the clean symbol
                        // name; once for real, with its body, in
                        // gen_pulse itself — LLVM auto-suffixes the
                        // second Function::Create's name on collision,
                        // e.g. "Geometry::circle_area.2"), and
                        // def_fn's scope-tracked registry is what gets
                        // updated to point at the SECOND, real one.
                        // M.getFunction(qualified_name) would instead
                        // find the module's first-registered (empty,
                        // Pass-2) declaration directly, bypassing that
                        // update entirely — caught by testing: the
                        // direct-PathExpr-call path above already got
                        // this right by using lookup_fn first, but
                        // this use_aliases fallback initially didn't,
                        // and produced a real, distinct link failure
                        // ("undefined reference to Geometry::circle_area")
                        // rather than a wrong answer, since the
                        // empty Pass-2 declaration this found really
                        // has no body to link against at all.
                        fn = lookup_fn(uit->second);
                        if (!fn) fn = M.getFunction(uit->second);
                    }
                }
                if (!fn) {
                    // A call whose callee is `obj.method` reaches here when no
                    // dispatch above claimed it (Vec methods and enum
                    // construction are handled earlier). impl methods ARE
                    // generated as functions, but a CALL to one is not lowered
                    // yet (self-pointer passing is unimplemented), so this
                    // used to fall through to a constant 0: the program
                    // compiled, linked, ran, and printed a wrong answer. Fail
                    // loudly instead.
                    if (n.children[0]->kind == NodeKind::MemberExpr) {
                        std::string mname = n.children[0]->value;
                        throw std::runtime_error("method call `." + mname +
                            "(...)` is not supported by the LLVM backend yet (only Vec methods and "
                            "enum construction are); use --backend=transpiler or --backend=xil");
                    }
                    // A plain name that resolved to nothing: not a declared pulse,
                    // not a builtin the backend lowers, not an import alias.
                    // This used to evaluate to a constant 0 — the program
                    // compiled and ran and used 0 as the call's result (e.g. an
                    // un-specialized generic call, or a stdlib function this
                    // backend has no implementation of). Say so instead.
                    throw std::runtime_error("call to `" + fname + "` could not be resolved by the LLVM backend "
                        "(not a declared pulse, and not a runtime function this backend provides)");
                }

                std::vector<Value*> args;
                for (size_t i = 1; i < n.children.size(); i++) {
                    if (!n.children[i]) continue;
                    Value* arg = gen_expr(*n.children[i]);
                    // Coerce to expected param type
                    if (i - 1 < fn->arg_size()) {
                        Type* expected = fn->getFunctionType()->getParamType(i-1);
                        if (expected != arg->getType()) {
                            // A struct-typed parameter (an enum or forge, passed
                            // BY VALUE) receiving a pointer: enum construction
                            // and `spawn` both yield the ADDRESS of a fresh
                            // alloca, so load the value through it. Without
                            // this the call failed verification ("Call
                            // parameter type does not match function
                            // signature"), which made it impossible to pass an
                            // Option/Result/enum to any function.
                            if (expected->isStructTy() && arg->getType()->isPointerTy())
                                arg = B.CreateLoad(expected, arg);
                            else if (expected->isDoubleTy() && arg->getType()->isIntegerTy())
                                arg = B.CreateSIToFP(arg, expected);
                            else if (expected->isIntegerTy(64) && arg->getType()->isIntegerTy(1))
                                arg = B.CreateZExt(arg, expected);
                            else if (expected->isIntegerTy(64) && arg->getType()->isDoubleTy())
                                arg = B.CreateFPToSI(arg, expected);
                        }
                    }
                    // extern "C": an XPhage `str` is an XpStr*, C wants `const char*`
                    auto sh = extern_shapes.find(fn->getName().str());
                    if (sh != extern_shapes.end() && i - 1 < sh->second.str_param.size() &&
                        sh->second.str_param[i - 1] && arg->getType()->isPointerTy())
                        arg = B.CreateCall(xprt("xprt_str_cstr", B.getPtrTy(), {xpstr_ptr_ty()}), {arg});
                    args.push_back(arg);
                }
                Value* call = B.CreateCall(fn, args);
                // ... and a returned `const char*` becomes an XPhage str
                auto shr = extern_shapes.find(fn->getName().str());
                if (shr != extern_shapes.end() && shr->second.str_ret)
                    return B.CreateCall(xprt("xprt_str_new", xpstr_ptr_ty(), {B.getPtrTy()}), {call});
                return call;
            }

            case NodeKind::CastExpr: {
                if (n.children.empty()) return B.getInt64(0);
                Value* v  = gen_expr(*n.children[0]);
                Type*  to = xp_type(n.extra);
                if (v->getType() == to) return v;
                if (to->isIntegerTy() || to->isFloatingPointTy()) {
                    auto f = n.attrs.find("from");
                    std::string from = f != n.attrs.end() ? f->second : name_of_llvm(v->getType());
                    return convert_numeric(v, from, n.extra);
                }
                return B.CreateBitCast(v, to);
            }

            case NodeKind::PipelineExpr: {
                // expr |> fn  →  fn(expr)
                if (n.children.size() < 2) return B.getInt64(0);
                Value*  arg  = gen_expr(*n.children[0]);
                std::string fn_name;
                if (n.children[1]->kind == NodeKind::Identifier)
                    fn_name = n.children[1]->value;
                Function* fn = lookup_fn(fn_name);
                if (!fn) return arg;
                return B.CreateCall(fn, {arg});
            }

            case NodeKind::ProcExpr: {
                if (n.children.empty()) return make_str_literal("");
                Value* cmd = gen_expr(*n.children[0]);
                // If cmd is XpStr*, get cstr
                FunctionCallee cstr_fn = xprt("xprt_str_cstr", opaque_ptr(),
                                              {xpstr_ptr_ty()});
                Value* cstr = B.CreateCall(cstr_fn, {cmd});
                FunctionCallee proc_fn = xprt("xprt_proc", xpstr_ptr_ty(),
                                              {opaque_ptr()});
                return B.CreateCall(proc_fn, {cstr});
            }

            case NodeKind::EnvExpr: {
                Value* key = make_cstr(n.value);
                FunctionCallee env_fn = xprt("xprt_env_get", xpstr_ptr_ty(),
                                             {opaque_ptr()});
                return B.CreateCall(env_fn, {key});
            }

            case NodeKind::PropagateExpr:
                return gen_propagate(n);

            case NodeKind::AwaitExpr: {
                if (n.children.empty()) return B.getInt64(0);
                // std::future<T>::get() equivalent — call as regular function for now
                // Full async requires coroutine support (Phase 6+)
                return gen_expr(*n.children[0]);
            }

            case NodeKind::MemberExpr: {
                if (n.children.empty()) return B.getInt64(0);
                // `Option__int.None` / `Color.Green` — a fieldless variant
                // used as a value. Only when the object is an Identifier
                // naming a registered enum AND no variable of that name
                // shadows it.
                if (n.children[0] && n.children[0]->kind == NodeKind::Identifier &&
                    enum_types.count(n.children[0]->value) && !lookup(n.children[0]->value)) {
                    if (Value* ev = gen_enum_construct(n.children[0]->value, n.value, {})) return ev;
                }
                // A field of a forge place — `r.val`, `self.n`, `h.c.n`,
                // `b.items.len`: resolved through gen_place so nested access
                // and `self` work. (Previously only `ident.field` was
                // understood; anything deeper silently evaluated to 0.)
                {
                    Place fp = gen_place(n);
                    if (fp.addr && fp.ty) return B.CreateLoad(fp.ty, fp.addr);
                }
                if (n.children[0]->kind != NodeKind::Identifier)
                    throw std::runtime_error("member access `." + n.value +
                        "` on a value that is not an addressable forge place is not supported by the "
                        "LLVM backend; use --backend=transpiler or --backend=xil");
                // Handle flux.get()
                std::string obj_name;
                if (n.children[0]->kind == NodeKind::Identifier)
                    obj_name = n.children[0]->value;
                Value* ptr = lookup(obj_name);
                if (!ptr) return B.getInt64(0);
                Type*  t   = lookup_type(obj_name);
                Value* obj = B.CreateLoad(t, ptr, obj_name);
                // For forge structs. forge_fields/forge_types are
                // keyed by the forge's own declared name (e.g.
                // "Result"), NOT by the variable name (e.g. "r") — the
                // old `forge_fields.count(obj_name)` here checked the
                // variable name directly against that map, which can
                // never match (a real, previously-undetected bug:
                // every field access silently fell through to
                // `return obj` — the whole loaded struct value's raw
                // bit pattern — instead of the actual field; caught by
                // running the golden suite against real generated
                // output, where `f"val={r.val}"` printed a garbage-
                // looking large integer instead of the real field
                // value). Resolve the variable's actual forge type
                // name first — StructType::getName() recovers it from
                // the "forge." prefix gen_forge names every forge
                // struct type with — then look up forge_fields/
                // forge_types by THAT name.
                std::string forge_name;
                if (auto* st = dyn_cast<StructType>(t)) {
                    llvm::StringRef sn = st->getName();
                    if (sn.starts_with("forge.")) forge_name = sn.substr(6).str();
                }
                if (!forge_name.empty() && forge_fields.count(forge_name)) {
                    auto& fields = forge_fields.at(forge_name);
                    for (size_t i = 0; i < fields.size(); i++) {
                        if (fields[i] == n.value) {
                            Value* gep = B.CreateStructGEP(
                                forge_types.count(forge_name) ? (Type*)forge_types.at(forge_name) : t,
                                ptr, (unsigned)i);
                            // Field type — need to query the struct element type
                            if (forge_types.count(forge_name)) {
                                Type* ft = forge_types.at(forge_name)->getElementType(i);
                                return B.CreateLoad(ft, gep);
                            }
                            return B.CreateLoad(B.getInt64Ty(), gep);
                        }
                    }
                }
                return obj;
            }

            case NodeKind::SpawnExpr: {
                // spawn Vec<T> { } — a growable array; the runtime owns it.
                if (!vec_elem_type(n.value).empty()) {
                    return B.CreateCall(xprt("xprt_vec_new", opaque_ptr(), {}), {}, "vec");
                }
                // spawn TypeName { field: val, ... }
                if (!forge_types.count(n.value)) return B.getInt64(0);
                StructType* st = forge_types.at(n.value);
                Value* alloca  = B.CreateAlloca(st, nullptr, n.value + "_spawn");
                auto& fields   = forge_fields.at(n.value);
                for (auto& child : n.children) {
                    if (!child) continue;
                    std::string fname = child->attrs.count("field")
                                        ? child->attrs.at("field") : "";
                    for (size_t i = 0; i < fields.size(); i++) {
                        if (fields[i] == fname) {
                            Value* gep = B.CreateStructGEP(st, alloca, (unsigned)i);
                            Value* val = gen_expr(*child);
                            // Coerce
                            Type* ft = st->getElementType(i);
                            if (ft != val->getType()) {
                                if (ft->isDoubleTy() && val->getType()->isIntegerTy())
                                    val = B.CreateSIToFP(val, ft);
                                else if (ft->isIntegerTy(64) && val->getType()->isDoubleTy())
                                    val = B.CreateFPToSI(val, ft);
                                else if (ft->isIntegerTy(64) && val->getType()->isIntegerTy(1))
                                    val = B.CreateZExt(val, ft);
                            }
                            B.CreateStore(val, gep);
                        }
                    }
                }
                // Fields the spawn did not mention take their declared default.
                {
                    std::unordered_set<std::string> given;
                    for (auto& child : n.children)
                        if (child && child->attrs.count("field")) given.insert(child->attrs.at("field"));
                    auto dit = forge_defaults.find(n.value);
                    if (dit != forge_defaults.end()) {
                        for (size_t i = 0; i < fields.size(); i++) {
                            if (given.count(fields[i])) continue;
                            auto fd = dit->second.find(fields[i]);
                            if (fd == dit->second.end()) continue;
                            Value* val = coerce_value(gen_expr(*fd->second), st->getElementType(i));
                            B.CreateStore(val, B.CreateStructGEP(st, alloca, (unsigned)i));
                        }
                    }
                }
                return alloca;
            }

            default:
                return B.getInt64(0);
        }
    }

    // ── Enum helpers ──────────────────────────────────────────
    // Widen/narrow a value to the uniform 8-byte payload slot type (i64).
    // Every payload field is stored as one i64 so reading variant field j
    // is always "load slot j", independent of the field's own type.
    // ── sized numeric operations ────────────────────────────────────────────
    static bool xs_uint(const std::string& t) {
        return t == "u8" || t == "u16" || t == "u32" || t == "u64" || t == "usize";
    }
    static std::string attr_ty(const ASTNode& n) {
        auto it = n.attrs.find("ty");
        return it == n.attrs.end() ? std::string() : it->second;
    }
    static std::string name_of_llvm(Type* t) {
        if (t->isIntegerTy(1))  return "bool";
        if (t->isIntegerTy(64)) return "int";
        if (t->isDoubleTy())    return "float";
        if (t->isFloatTy())     return "f32";
        if (t->isIntegerTy(32)) return "i32";
        if (t->isIntegerTy(16)) return "i16";
        if (t->isIntegerTy(8))  return "i8";
        return "";
    }
    // Resize/convert `v` to `want`, taking integer signedness from `sgn`.
    Value* fit_numeric(Value* v, Type* want, bool sgn) {
        Type* have = v->getType();
        if (have == want) return v;
        if (want->isIntegerTy() && have->isIntegerTy()) return B.CreateIntCast(v, want, sgn);
        if (want->isFloatingPointTy() && have->isFloatingPointTy())
            return have->getPrimitiveSizeInBits() < want->getPrimitiveSizeInBits()
                       ? B.CreateFPExt(v, want) : B.CreateFPTrunc(v, want);
        if (want->isFloatingPointTy() && have->isIntegerTy())
            return sgn ? B.CreateSIToFP(v, want) : B.CreateUIToFP(v, want);
        return v;
    }
    // One binary operator in sized type `ty`: integer arithmetic wraps, shifts
    // mask their count to the width, division/remainder/compare/shift-right
    // choose signed or unsigned by `ty`. nullptr when `op` is not one of them.
    Value* typed_binop(const std::string& op, const std::string& ty, Value* L, Value* R) {
        Type* T = xp_type(ty);
        const bool fp = T->isFloatingPointTy(), uns = xs_uint(ty);
        if (op == "<<" || op == ">>") {
            L = fit_numeric(L, T, !uns);
            unsigned bits = T->getIntegerBitWidth();
            Value* cnt = B.CreateAnd(B.CreateIntCast(R, T, false), ConstantInt::get(T, bits - 1));
            if (op == "<<") return B.CreateShl(L, cnt);
            return uns ? B.CreateLShr(L, cnt) : B.CreateAShr(L, cnt);
        }
        L = fit_numeric(L, T, !uns);
        R = fit_numeric(R, T, !uns);
        if (op == "+") return fp ? B.CreateFAdd(L, R) : B.CreateAdd(L, R);
        if (op == "-") return fp ? B.CreateFSub(L, R) : B.CreateSub(L, R);
        if (op == "*") return fp ? B.CreateFMul(L, R) : B.CreateMul(L, R);
        if (op == "/") return fp ? B.CreateFDiv(L, R) : (uns ? B.CreateUDiv(L, R) : B.CreateSDiv(L, R));
        if (op == "%") return fp ? B.CreateFRem(L, R) : (uns ? B.CreateURem(L, R) : B.CreateSRem(L, R));
        if (op == "&") return B.CreateAnd(L, R);
        if (op == "|") return B.CreateOr(L, R);
        if (op == "^") return B.CreateXor(L, R);
        if (fp) {
            if (op == "==") return B.CreateFCmpOEQ(L, R);
            if (op == "!=") return B.CreateFCmpUNE(L, R);
            if (op == "<")  return B.CreateFCmpOLT(L, R);
            if (op == "<=") return B.CreateFCmpOLE(L, R);
            if (op == ">")  return B.CreateFCmpOGT(L, R);
            if (op == ">=") return B.CreateFCmpOGE(L, R);
            return nullptr;
        }
        if (op == "==") return B.CreateICmpEQ(L, R);
        if (op == "!=") return B.CreateICmpNE(L, R);
        if (op == "<")  return uns ? B.CreateICmpULT(L, R) : B.CreateICmpSLT(L, R);
        if (op == "<=") return uns ? B.CreateICmpULE(L, R) : B.CreateICmpSLE(L, R);
        if (op == ">")  return uns ? B.CreateICmpUGT(L, R) : B.CreateICmpSGT(L, R);
        if (op == ">=") return uns ? B.CreateICmpUGE(L, R) : B.CreateICmpSGE(L, R);
        return nullptr;
    }
    // cast(x, to): int -> int wraps/extends by the SOURCE signedness; int -> float
    // rounds; float -> int truncates toward zero and saturates (NaN -> 0), via
    // LLVM's saturating conversion intrinsics; float -> float rounds.
    Value* convert_numeric(Value* v, const std::string& from_name, const std::string& to_name) {
        Type* T = xp_type(to_name);
        Type* S = v->getType();
        if (S == T) return v;
        const bool from_signed = !xs_uint(from_name);
        if (T->isFloatingPointTy()) {
            if (S->isFloatingPointTy()) return fit_numeric(v, T, true);
            if (S->isIntegerTy(1)) return B.CreateUIToFP(v, T);
            return from_signed ? B.CreateSIToFP(v, T) : B.CreateUIToFP(v, T);
        }
        if (T->isIntegerTy(1)) {
            if (S->isFloatingPointTy()) return B.CreateFCmpUNE(v, ConstantFP::get(S, 0.0));
            return B.CreateICmpNE(v, ConstantInt::get(S, 0));
        }
        if (T->isIntegerTy()) {
            if (S->isFloatingPointTy()) {
                auto id = xs_uint(to_name) ? Intrinsic::fptoui_sat : Intrinsic::fptosi_sat;
                return B.CreateIntrinsic(id, {T, S}, {v});
            }
            if (S->isIntegerTy(1)) return B.CreateZExt(v, T);
            if (S->isIntegerTy()) return B.CreateIntCast(v, T, from_signed);
        }
        return B.CreateBitCast(v, T);
    }
    // A value about to be shown: widened to the 64-bit forms the runtime prints.
    Value* widen_for_show(Value* v, const std::string& ty, bool& is_unsigned) {
        is_unsigned = xs_uint(ty);
        Type* t = v->getType();
        if (t->isFloatTy()) return B.CreateFPExt(v, B.getDoubleTy());
        if (t->isIntegerTy() && !t->isIntegerTy(1) && !t->isIntegerTy(64))
            return is_unsigned ? B.CreateZExt(v, B.getInt64Ty()) : B.CreateSExt(v, B.getInt64Ty());
        return v;
    }

    Value* to_slot(Value* v) {
        Type* t = v->getType();
        if (t->isFloatTy())     return B.CreateZExt(B.CreateBitCast(v, B.getInt32Ty()), B.getInt64Ty());
        if (t->isIntegerTy(64)) return v;
        if (t->isIntegerTy(1))  return B.CreateZExt(v, B.getInt64Ty());
        if (t->isDoubleTy())    return B.CreateBitCast(v, B.getInt64Ty());
        if (t->isPointerTy())   return B.CreatePtrToInt(v, B.getInt64Ty());
        if (t->isIntegerTy())   return B.CreateSExtOrTrunc(v, B.getInt64Ty());
        throw std::runtime_error("enum payload of unsupported LLVM type");
    }
    // Inverse of to_slot: reinterpret a loaded i64 slot as the field's
    // declared source type.
    Value* from_slot(Value* slot, const std::string& src_type) {
        Type* want = xp_type(src_type);
        if (want->isIntegerTy(64)) return slot;
        if (want->isIntegerTy(1))  return B.CreateICmpNE(slot, B.getInt64(0));
        if (want->isFloatTy())     return B.CreateBitCast(B.CreateTrunc(slot, B.getInt32Ty()), want);
        if (want->isIntegerTy())   return B.CreateTrunc(slot, want);
        if (want->isDoubleTy())    return B.CreateBitCast(slot, B.getDoubleTy());
        if (want->isPointerTy())   return B.CreateIntToPtr(slot, want);
        // A struct-typed payload (e.g. Option<SomeForge>) would need the
        // whole struct behind a pointer; not supported yet. Say so rather
        // than silently reading garbage.
        throw std::runtime_error("enum payload of struct type '" + src_type +
                                 "' is not supported by the LLVM backend yet");
    }
    // Width, in 8-byte words, that a value of source type `t` occupies when
    // stored INLINE in an enum payload. Scalars, pointers (str, Vec) and
    // widened bool/double are one word. A nested enum takes its full struct
    // (tag word + payload words). Struct-typed (forge) payloads are not
    // supported yet and are rejected loudly rather than mis-sized.
    unsigned field_words_of(const std::string& t) {
        auto eit = enum_types.find(t);
        if (eit != enum_types.end()) return 1 + std::max(1u, eit->second.slots);
        if (forge_types.count(t))
            throw std::runtime_error("forge-typed enum payload '" + t +
                                     "' is not supported by the LLVM backend yet");
        return 1;
    }
    // Store `val` (as produced by gen_expr) into payload word offset `off`
    // of the payload array `payload`, of source type `src`.
    void store_field(Value* payload, Type* arr_ty, unsigned off,
                     const std::string& src, Value* val) {
        auto eit = enum_types.find(src);
        if (eit != enum_types.end()) {
            // Nested enum: gen_expr yields a pointer to its alloca (or the
            // struct value). Copy its words into consecutive payload words.
            const EnumInfo& ie = eit->second;
            unsigned w = 1 + std::max(1u, ie.slots);
            Value* srcp = val;
            if (!val->getType()->isPointerTy()) {
                srcp = B.CreateAlloca(ie.type, nullptr, "nest_tmp");
                B.CreateStore(val, srcp);
            }
            for (unsigned k = 0; k < w; k++) {
                // word k of the inner struct: word 0 is the i32 tag (widened),
                // words 1.. are its payload array elements.
                Value* word;
                if (k == 0) {
                    Value* t = B.CreateLoad(B.getInt32Ty(), B.CreateStructGEP(ie.type, srcp, 0));
                    word = B.CreateZExt(t, B.getInt64Ty());
                } else {
                    Value* ip = B.CreateStructGEP(ie.type, srcp, 1);
                    Value* eg = B.CreateInBoundsGEP(ie.type->getElementType(1), ip,
                                                    {B.getInt32(0), B.getInt32(k - 1)});
                    word = B.CreateLoad(B.getInt64Ty(), eg);
                }
                Value* dst = B.CreateInBoundsGEP(arr_ty, payload, {B.getInt32(0), B.getInt32(off + k)});
                B.CreateStore(word, dst);
            }
            return;
        }
        Value* dst = B.CreateInBoundsGEP(arr_ty, payload, {B.getInt32(0), B.getInt32(off)});
        B.CreateStore(to_slot(val), dst);
    }
    // Inverse: read the field of source type `src` at payload word `off`.
    // A nested enum is rebuilt into a fresh alloca and returned as the
    // struct VALUE (so callers can store it like any other struct).
    Value* load_field(Value* payload, Type* arr_ty, unsigned off, const std::string& src) {
        auto eit = enum_types.find(src);
        if (eit != enum_types.end()) {
            const EnumInfo& ie = eit->second;
            unsigned w = 1 + std::max(1u, ie.slots);
            Value* tmp = B.CreateAlloca(ie.type, nullptr, "nest_val");
            for (unsigned k = 0; k < w; k++) {
                Value* sp = B.CreateInBoundsGEP(arr_ty, payload, {B.getInt32(0), B.getInt32(off + k)});
                Value* word = B.CreateLoad(B.getInt64Ty(), sp);
                if (k == 0) {
                    B.CreateStore(B.CreateTrunc(word, B.getInt32Ty()), B.CreateStructGEP(ie.type, tmp, 0));
                } else {
                    Value* ip = B.CreateStructGEP(ie.type, tmp, 1);
                    Value* eg = B.CreateInBoundsGEP(ie.type->getElementType(1), ip,
                                                    {B.getInt32(0), B.getInt32(k - 1)});
                    B.CreateStore(word, eg);
                }
            }
            return B.CreateLoad(ie.type, tmp);
        }
        Value* sp = B.CreateInBoundsGEP(arr_ty, payload, {B.getInt32(0), B.getInt32(off)});
        return from_slot(B.CreateLoad(B.getInt64Ty(), sp), src);
    }
    const EnumVariantInfo* find_variant(const EnumInfo& e, const std::string& vname) {
        for (auto& v : e.variants) if (v.name == vname) return &v;
        return nullptr;
    }

    // Build `Enum.Variant(args...)` (or the bare `Enum.Variant`) in a fresh
    // alloca and return a POINTER to it — the same convention SpawnExpr
    // uses, so ReturnStmt / AtomDecl / call-argument paths that already
    // "load through a pointer when a struct is expected" work unchanged.
    // Returns nullptr when `enum_name` is not a registered enum, so the
    // caller can fall through to ordinary call/member handling.
    Value* gen_enum_construct(const std::string& enum_name, const std::string& vname,
                              const std::vector<ASTNodePtr>& args) {
        auto eit = enum_types.find(enum_name);
        if (eit == enum_types.end()) return nullptr;
        const EnumInfo& e = eit->second;
        const EnumVariantInfo* v = find_variant(e, vname);
        if (!v) throw std::runtime_error("enum '" + enum_name + "' has no variant '" + vname + "'");
        if (args.size() != v->field_types.size())
            throw std::runtime_error("variant '" + enum_name + "." + vname + "' takes " +
                std::to_string(v->field_types.size()) + " argument(s), got " +
                std::to_string(args.size()));

        // Evaluate arguments BEFORE creating the alloca's stores so a
        // nested construction (Result.Ok(Option.Some(3))) has finished
        // building its own alloca first.
        std::vector<Value*> vals;
        for (auto& a : args) vals.push_back(gen_expr(*a));

        Value* slot_mem = B.CreateAlloca(e.type, nullptr, enum_name + "_" + vname);
        // Zero the whole value first so unused payload words are
        // deterministic (matters for any future by-value equality/hash).
        B.CreateStore(Constant::getNullValue(e.type), slot_mem);
        B.CreateStore(B.getInt32(v->tag), B.CreateStructGEP(e.type, slot_mem, 0));
        Value* payload = B.CreateStructGEP(e.type, slot_mem, 1);
        Type* arr_ty = e.type->getElementType(1);
        for (size_t i = 0; i < vals.size(); i++) {
            store_field(payload, arr_ty, v->field_off[i], v->field_types[i], vals[i]);
        }
        return slot_mem;
    }

    // `a && b` / `a || b` with real short-circuit evaluation: the right
    // operand is lowered into its OWN block that only runs when the left one
    // does not already decide the result. Previously both operands were
    // evaluated up front and then and/or-ed, so `flag && risky()` ran
    // risky() (and its side effects, or a `?` early return) even when flag
    // was false.
    Value* gen_short_circuit(const ASTNode& n, bool is_and) {
        auto to_i1 = [&](Value* v) -> Value* {
            if (v->getType()->isIntegerTy(1)) return v;
            if (v->getType()->isDoubleTy())   return B.CreateFCmpONE(v, ConstantFP::get(v->getType(), 0.0));
            return B.CreateICmpNE(v, Constant::getNullValue(v->getType()));
        };
        Value* L = to_i1(gen_expr(*n.children[0]));
        Function* fn = B.GetInsertBlock()->getParent();
        BasicBlock* lhs_bb = B.GetInsertBlock();
        BasicBlock* rhs_bb = BasicBlock::Create(ctx, is_and ? "and_rhs" : "or_rhs", fn);
        BasicBlock* end_bb = BasicBlock::Create(ctx, is_and ? "and_end" : "or_end", fn);
        if (is_and) B.CreateCondBr(L, rhs_bb, end_bb);
        else        B.CreateCondBr(L, end_bb, rhs_bb);
        B.SetInsertPoint(rhs_bb);
        Value* R = to_i1(gen_expr(*n.children[1]));
        // The right operand can itself create blocks (a nested `&&`, or a
        // `?`), so the phi's incoming edge is the block we END in.
        BasicBlock* rhs_end = B.GetInsertBlock();
        B.CreateBr(end_bb);
        B.SetInsertPoint(end_bb);
        PHINode* phi = B.CreatePHI(B.getInt1Ty(), 2, is_and ? "and_res" : "or_res");
        phi->addIncoming(B.getInt1(is_and ? 0 : 1), lhs_bb);
        phi->addIncoming(R, rhs_end);
        return phi;
    }

    // ── `?` (error propagation) ───────────────────────────────
    // Shape of an enum for `?`: 1 = Result-like (Ok(T) + Err(E)),
    // 2 = Option-like (Some(T) + fieldless None), 0 = neither. Decided by
    // variant shape, not the enum's name — the same rule the analyzer and
    // the C++ backend use, so all three agree on what `?` accepts.
    int try_kind(const EnumInfo& e) {
        const EnumVariantInfo *ok = nullptr, *err = nullptr, *some = nullptr, *none = nullptr;
        for (auto& v : e.variants) {
            if (v.name == "Ok")   ok   = &v;
            if (v.name == "Err")  err  = &v;
            if (v.name == "Some") some = &v;
            if (v.name == "None") none = &v;
        }
        if (ok && err && ok->field_types.size() == 1 && err->field_types.size() == 1) return 1;
        if (some && none && some->field_types.size() == 1 && none->field_types.empty()) return 2;
        return 0;
    }
    // "enum.Result__int__str" (an LLVM struct type) -> "Result__int__str"; "" if not an enum.
    std::string enum_name_of_type(Type* t) {
        if (auto* st = dyn_cast_or_null<StructType>(t)) {
            if (st->hasName()) {
                llvm::StringRef sn = st->getName();
                if (sn.starts_with("enum.")) return sn.substr(5).str();
            }
        }
        return std::string();
    }
    // Source type string of what `expr?` evaluates to (the Ok/Some payload
    // type), or "" when it cannot be determined statically. Needed so
    // `atom x = f()?` gets a slot of the right type BEFORE the initializer
    // is lowered: an unannotated atom otherwise defaults to i64, which
    // would silently reinterpret a str pointer or an enum as an integer.
    std::string propagate_ok_type(const ASTNode& prop) {
        if (prop.children.empty() || !prop.children[0]) return std::string();
        const ASTNode& op = *prop.children[0];
        std::string ename;
        if (op.kind == NodeKind::CallExpr && !op.children.empty() && op.children[0] &&
            op.children[0]->kind == NodeKind::Identifier) {
            if (Function* callee = lookup_fn(op.children[0]->value))
                ename = enum_name_of_type(callee->getReturnType());
        } else if (op.kind == NodeKind::Identifier) {
            ename = enum_name_of_type(lookup_type(op.value));
        } else if (op.kind == NodeKind::PropagateExpr) {
            std::string inner = propagate_ok_type(op);
            if (enum_types.count(inner)) ename = inner;
        }
        auto eit = enum_types.find(ename);
        if (eit == enum_types.end()) return std::string();
        for (auto& v : eit->second.variants)
            if ((v.name == "Ok" || v.name == "Some") && v.field_types.size() == 1)
                return v.field_types[0];
        return std::string();
    }

    // `expr?`: evaluate expr (a Result-like or Option-like enum); if it is
    // the failure variant, RETURN the failure from the enclosing pulse right
    // away (rebuilt in that pulse's own return type, error payload carried
    // through unchanged); otherwise the value is the success payload.
    // Previously this fell through to gen_expr's default and yielded a
    // constant 0: `?` compiled, linked, ran, never propagated anything, and
    // handed back garbage for every success too.
    Value* gen_propagate(const ASTNode& n) {
        if (n.children.empty() || !n.children[0])
            throw std::runtime_error("`?` has no operand");
        Value* opv = gen_expr(*n.children[0]);
        if (!opv->getType()->isStructTy())
            throw std::runtime_error("`?` operand must be a call or a variable holding a Result/Option "
                                     "(got a value that is not an enum)");
        std::string oname = enum_name_of_type(opv->getType());
        auto oit = enum_types.find(oname);
        if (oit == enum_types.end())
            throw std::runtime_error("`?` operand is not a Result or Option (an enum with Ok/Err or Some/None)");
        const EnumInfo& oe = oit->second;

        Function* fn = B.GetInsertBlock()->getParent();
        std::string rname = enum_name_of_type(fn->getReturnType());
        auto rit = enum_types.find(rname);
        if (rit == enum_types.end())
            throw std::runtime_error("`?` used in `" + fn->getName().str() +
                                     "`, which does not return a Result or Option");
        const EnumInfo& re = rit->second;

        int ok_kind = try_kind(oe), rk = try_kind(re);
        if (ok_kind == 0)
            throw std::runtime_error("`?` operand type '" + oname + "' is not a Result or Option");
        if (rk == 0)
            throw std::runtime_error("`?` used in `" + fn->getName().str() + "`, whose return type '" +
                                     rname + "' is not a Result or Option");
        if (ok_kind != rk)
            throw std::runtime_error("`?` cannot convert between Result and Option: operand is '" + oname +
                                     "' but `" + fn->getName().str() + "` returns '" + rname + "'");

        const char* fail_name    = rk == 1 ? "Err" : "None";
        const char* success_name = rk == 1 ? "Ok"  : "Some";
        const EnumVariantInfo* o_fail = find_variant(oe, fail_name);
        const EnumVariantInfo* r_fail = find_variant(re, fail_name);
        const EnumVariantInfo* o_succ = find_variant(oe, success_name);
        if (rk == 1 && o_fail->field_types[0] != r_fail->field_types[0])
            throw std::runtime_error("`?` cannot convert the error type: operand's error type is '" +
                                     o_fail->field_types[0] + "' but `" + fn->getName().str() +
                                     "` returns error type '" + r_fail->field_types[0] + "'");

        // The operand's value in memory, so its tag and payload can be read.
        Value* slot = B.CreateAlloca(oe.type, nullptr, "q_operand");
        B.CreateStore(opv, slot);
        Value* tag = B.CreateLoad(B.getInt32Ty(), B.CreateStructGEP(oe.type, slot, 0), "q_tag");
        Value* is_fail = B.CreateICmpEQ(tag, B.getInt32(o_fail->tag), "q_is_fail");
        BasicBlock* fail_bb = BasicBlock::Create(ctx, "q_fail", fn);
        BasicBlock* ok_bb   = BasicBlock::Create(ctx, "q_ok",   fn);
        B.CreateCondBr(is_fail, fail_bb, ok_bb);

        // Failure: build the failure value in the FUNCTION's return type and return it.
        B.SetInsertPoint(fail_bb);
        Value* rslot = B.CreateAlloca(re.type, nullptr, "q_ret");
        B.CreateStore(Constant::getNullValue(re.type), rslot);
        B.CreateStore(B.getInt32(r_fail->tag), B.CreateStructGEP(re.type, rslot, 0));
        if (rk == 1) {
            Value* op_payload  = B.CreateStructGEP(oe.type, slot, 1);
            Value* ret_payload = B.CreateStructGEP(re.type, rslot, 1);
            Value* err_val = load_field(op_payload, oe.type->getElementType(1),
                                        o_fail->field_off[0], o_fail->field_types[0]);
            store_field(ret_payload, re.type->getElementType(1),
                        r_fail->field_off[0], r_fail->field_types[0], err_val);
        }
        B.CreateRet(B.CreateLoad(re.type, rslot, "q_ret_val"));

        // Success: the payload is the value of the whole `?` expression.
        B.SetInsertPoint(ok_bb);
        Value* payload = B.CreateStructGEP(oe.type, slot, 1);
        return load_field(payload, oe.type->getElementType(1), o_succ->field_off[0], o_succ->field_types[0]);
    }

    // Make a C-string pointer for a literal
    Value* make_cstr(const std::string& s) {
        Constant* c = ConstantDataArray::getString(ctx, s, true);
        auto* gv = new GlobalVariable(M, c->getType(), true,
                    GlobalValue::PrivateLinkage, c, ".cstr");
        gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        return B.CreateConstGEP2_32(c->getType(), gv, 0, 0);
    }

    // ── Statement codegen ─────────────────────────────────────
    void gen_stmt(const ASTNode& n, Function* fn) {
        if (!B.GetInsertBlock()) return;
        if (B.GetInsertBlock()->getTerminator()) {
            // After a return, skip (dead code)
            if (n.kind == NodeKind::ReturnStmt) return;
        }

        switch (n.kind) {
            // ── Variable declarations ──────────────────────────
            case NodeKind::AtomDecl:
            case NodeKind::ShadowDecl:
            case NodeKind::ConstDecl: {
                // xp_type("") defaults to i64 (see its own comment —
                // empty-string is checked before the forge_types
                // fallback, so it can never resolve to a struct type
                // on its own). For `atom r = spawn Result {...}` with
                // no explicit annotation (n.extra empty — the common
                // case; only `atom r: Result = ...` populates it),
                // that meant `r` was always allocated as a plain i64
                // regardless of what it was actually initialized
                // with — a real bug caught by running the golden
                // suite against real generated output: `store ptr
                // %Result_spawn, ptr %r` into an i64-typed alloca,
                // then reading that raw pointer bit-pattern back as
                // if it were a field value, explained the
                // garbage-looking large integers `f"val={r.val}"`
                // printed. Mirrors the identical fallback the AST/XIL
                // transpiler backends already use for this same
                // no-annotation case: when there's no explicit type
                // string, fall back to a SpawnExpr initializer's own
                // type before defaulting.
                std::string type_str = n.extra;
                if (type_str.empty() && !n.children.empty() && n.children[0]) {
                    // Direct enum construction: `atom c = Color.Green` (bare
                    // MemberExpr) or `atom o = Option__int.Some(1)` (a call
                    // whose callee is that MemberExpr).
                    {
                        const ASTNode* me = nullptr;
                        if (n.children[0]->kind == NodeKind::MemberExpr) me = n.children[0].get();
                        else if (n.children[0]->kind == NodeKind::CallExpr && !n.children[0]->children.empty() &&
                                 n.children[0]->children[0] &&
                                 n.children[0]->children[0]->kind == NodeKind::MemberExpr)
                            me = n.children[0]->children[0].get();
                        if (me && !me->children.empty() && me->children[0] &&
                            me->children[0]->kind == NodeKind::Identifier &&
                            enum_types.count(me->children[0]->value) && !lookup(me->children[0]->value))
                            type_str = me->children[0]->value;
                    }
                    if (!type_str.empty()) {
                        // resolved above; skip the forge/call inference below
                    } else if (n.children[0]->kind == NodeKind::SpawnExpr) {
                        type_str = n.children[0]->value;
                    } else if (n.children[0]->kind == NodeKind::CallExpr &&
                               !n.children[0]->children.empty() &&
                               n.children[0]->children[0] &&
                               n.children[0]->children[0]->kind == NodeKind::Identifier) {
                        // `shadow t1 = make_todo(...)` — no explicit
                        // annotation, and the initializer isn't a
                        // direct SpawnExpr but a call to a function
                        // that itself returns a struct. Without this,
                        // `t1` still fell back to a plain i64 alloca
                        // even after the SpawnExpr case above was
                        // fixed — a real bug caught by running the
                        // golden suite: the return-type verifier error
                        // was gone (function correctly returns
                        // %forge.TodoItem by value now), but the
                        // caller still declared `t1` as i64 and
                        // `store %forge.TodoItem %1, ptr %t1` into
                        // that i64 slot corrupted the stack, causing a
                        // segfault a few instructions later rather
                        // than a clean verifier error. Resolve the
                        // callee's real LLVM return type directly from
                        // the Module (functions are pre-declared
                        // before any body is generated, so this lookup
                        // always finds a real function here), then
                        // recover its forge name the same way the
                        // MemberExpr fix above does (StructType::
                        // getName()'s "forge." prefix), since
                        // forge_types/xp_type key by that name string,
                        // not by an LLVM Type* directly.
                        if (auto* callee = M.getFunction(n.children[0]->children[0]->value)) {
                            if (auto* st = dyn_cast<StructType>(callee->getReturnType())) {
                                llvm::StringRef sn = st->getName();
                                if (sn.starts_with("forge.")) type_str = sn.substr(6).str();
                                else if (sn.starts_with("enum.")) type_str = sn.substr(5).str();
                            }
                        }
                    } else if (Function* mf = method_callee_of(*n.children[0])) {
                        // `atom c = obj.method(...)`: the variable has the forge/
                        // enum type that METHOD returns, exactly as for a plain
                        // function call above. Without this `c` fell back to a
                        // bare i64 slot and a later `c.get()` could not be
                        // resolved as a method at all.
                        if (auto* st = dyn_cast<StructType>(mf->getReturnType())) {
                            llvm::StringRef sn = st->getName();
                            if (sn.starts_with("forge.")) type_str = sn.substr(6).str();
                            else if (sn.starts_with("enum.")) type_str = sn.substr(5).str();
                        }
                    }
                }
                // `atom x = f()?` — the slot type is the Ok/Some payload type.
                if (type_str.empty() && !n.children.empty() && n.children[0] &&
                    n.children[0]->kind == NodeKind::PropagateExpr) {
                    type_str = propagate_ok_type(*n.children[0]);
                }
                Type* ty = xp_type(type_str);
                // No annotation and no struct/enum recovered above: if the
                // initializer is a call, the callee's ACTUAL LLVM return
                // type is the variable's type. Without this, an
                // unannotated `atom s = f(...)` where f returns str /
                // float / bool fell back to the default i64 slot, the
                // returned value was stored into it, and `beam` then
                // printed a string POINTER as an integer (e.g.
                // s=94012137702048 instead of s=hello). The bug is not
                // generics-specific: a plain non-generic
                // `pulse f(x: str) -> str` hit it too. Struct/enum
                // returns are already handled through type_str above, so
                // only override when that produced nothing.
                if (type_str.empty() && !n.children.empty() && n.children[0] &&
                    n.children[0]->kind == NodeKind::CallExpr &&
                    !n.children[0]->children.empty() && n.children[0]->children[0] &&
                    n.children[0]->children[0]->kind == NodeKind::Identifier) {
                    if (Function* callee = lookup_fn(n.children[0]->children[0]->value)) {
                        Type* rt = callee->getReturnType();
                        if (rt && !rt->isVoidTy()) ty = rt;
                    }
                }
                // Same for a scalar/str/float returned by a METHOD call:
                // `atom s = obj.name()` must be a str slot, not the i64 default.
                if (type_str.empty() && !n.children.empty() && n.children[0]) {
                    if (Function* mf = method_callee_of(*n.children[0])) {
                        Type* rt = mf->getReturnType();
                        if (rt && !rt->isVoidTy()) ty = rt;
                    }
                }
                // An unannotated atom initialised by an expression that is
                // boolean by construction (`true`, `a > b`, `x && y`, `!x`)
                // is a bool. It fell back to the i64 default, so `beam`
                // printed 1/0 where the other backends print true/false.
                if (type_str.empty() && !n.children.empty() && n.children[0]) {
                    const ASTNode& ini = *n.children[0];
                    bool boolean = ini.kind == NodeKind::BoolLit;
                    if (ini.kind == NodeKind::BinaryOp) {
                        const std::string& bo = ini.value;
                        boolean = bo=="&&"||bo=="||"||bo=="and"||bo=="or"||bo=="=="||bo=="!="||
                                  bo=="<"||bo=="<="||bo==">"||bo==">=";
                    }
                    if (ini.kind == NodeKind::UnaryOp && (ini.value == "!" || ini.value == "not")) boolean = true;
                    if (boolean) ty = B.getInt1Ty();
                    // Likewise a string or float literal: `atom needle = "a"` was
                    // given the i64 default, so passing it to a pulse taking a
                    // str failed IR verification ("Call parameter type does not
                    // match function signature"), and a float literal was
                    // truncated through an integer slot.
                    if (ini.kind == NodeKind::StringLit || ini.kind == NodeKind::FStringLit)
                        ty = xp_type("str");
                    else if (ini.kind == NodeKind::FloatLit)
                        ty = B.getDoubleTy();
                }
                // Last resort before the i64 default: the initializer's own type.
                if (n.extra.empty() && type_str.empty() && ty->isIntegerTy(64) &&
                    !n.children.empty() && n.children[0]) {
                    if (Type* st = static_type_of(*n.children[0])) {
                        if (st != ty && !st->isVoidTy()) ty = st;
                    }
                }
                Value* alloca = B.CreateAlloca(ty, nullptr, n.value);
                def_var(n.value, alloca, ty);
                {
                    // Remember the SOURCE type where it carries information
                    // the LLVM type erases (Vec<T>'s element type).
                    std::string src = n.extra;
                    if (src.empty() && !n.children.empty() && n.children[0] &&
                        n.children[0]->kind == NodeKind::SpawnExpr)
                        src = n.children[0]->value;
                    if (src.empty() && !n.children.empty() && n.children[0] &&
                        n.children[0]->kind == NodeKind::PropagateExpr)
                        src = propagate_ok_type(*n.children[0]);
                    if (!vec_elem_type(src).empty()) def_var_src(n.value, src);
                }
                if (!n.children.empty() && n.children[0]) {
                    Value* init = gen_expr(*n.children[0]);
                    // Coerce. A SpawnExpr's own gen_expr case (see
                    // above) returns a ptr — the address of a fresh,
                    // fully-populated struct alloca — not the struct
                    // value itself, so when `ty` is now (per this
                    // fix's own type-inference above) a real struct
                    // type, `init` and `ty` never matched here even
                    // after the AtomDecl/xp_type fix alone: storing a
                    // raw ptr into a struct-typed slot is invalid IR.
                    // Load through it instead — the struct-type case
                    // is checked first, alongside (not instead of) the
                    // existing numeric coercions, since a SpawnExpr
                    // initializer is the only pointer-typed case that
                    // can reach here at all (every scalar-typed
                    // initializer already produces a plain value). A
                    // CallExpr initializer (the new case just above)
                    // needs no equivalent load: a function returning a
                    // struct now correctly returns it BY VALUE (per
                    // the ReturnStmt fix), so init's type already
                    // matches ty directly in that case.
                    if (ty->isStructTy() && init->getType()->isPointerTy()) {
                        init = B.CreateLoad(ty, init);
                    } else if (init->getType() != ty) {
                        if (ty->isDoubleTy() && init->getType()->isIntegerTy())
                            init = B.CreateSIToFP(init, ty);
                        else if (ty->isIntegerTy(64) && init->getType()->isDoubleTy())
                            init = B.CreateFPToSI(init, ty);
                        else if (ty->isIntegerTy(64) && init->getType()->isIntegerTy(1))
                            init = B.CreateZExt(init, ty);
                        else if (ty->isIntegerTy(1) && init->getType()->isIntegerTy(64))
                            init = B.CreateICmpNE(init, B.getInt64(0));
                    }
                    B.CreateStore(init, alloca);
                } else {
                    // Zero-initialise
                    B.CreateStore(Constant::getNullValue(ty), alloca);
                }
                break;
            }

            case NodeKind::FluxDecl: {
                // Store as normal alloca; full reactor hooking via xprt_flux_*
                Type* ty = xp_type(n.extra);
                Value* alloca = B.CreateAlloca(ty, nullptr, n.value + "_flux");
                def_var(n.value, alloca, ty);
                if (!n.children.empty() && n.children[0]) {
                    Value* init = gen_expr(*n.children[0]);
                    B.CreateStore(init, alloca);
                }
                break;
            }

            // ── beam ──────────────────────────────────────────
            case NodeKind::BeamStmt: {
                if (n.children.empty()) {
                    B.CreateCall(xprt("xprt_beam_newline",B.getVoidTy(),{}));
                    break;
                }
                Value* v = gen_expr(*n.children[0]);
                bool is_uns = false;
                v = widen_for_show(v, attr_ty(*n.children[0]), is_uns);
                Type*  t = v->getType();
                if (t == B.getInt64Ty() && is_uns)
                    B.CreateCall(xprt("xprt_beam_uint", B.getVoidTy(), {B.getInt64Ty()}), {v});
                else if (t == B.getInt64Ty())    B.CreateCall(xprt_beam_int(),   {v});
                else if (t == B.getDoubleTy()) B.CreateCall(xprt_beam_float(),{v});
                else if (t == B.getInt1Ty())   B.CreateCall(xprt_beam_bool(), {B.CreateZExt(v,B.getInt32Ty())});
                else B.CreateCall(xprt_beam_str(), {v}); // XpStr*
                break;
            }

            // ── bypass ────────────────────────────────────────
            case NodeKind::BypassStmt: {
                if (n.children.empty()) break;
                Value* v = gen_expr(*n.children[0]);
                FunctionCallee bp = xprt("xprt_bypass", B.getVoidTy(), {opaque_ptr()});
                // If v is XpStr*, get cstr first
                if (v->getType() == xpstr_ptr_ty()) {
                    FunctionCallee cstr_fn = xprt("xprt_str_cstr", opaque_ptr(),{xpstr_ptr_ty()});
                    v = B.CreateCall(cstr_fn, {v});
                }
                B.CreateCall(bp, {v});
                break;
            }

            // ── chronos ───────────────────────────────────────
            case NodeKind::ChronosStmt: {
                FunctionCallee cf = xprt("xprt_chronos", B.getVoidTy(), {B.getInt64Ty()});
                Value* ms = n.children.empty() ? B.getInt64(0) : gen_expr(*n.children[0]);
                if (!ms->getType()->isIntegerTy(64))
                    ms = B.CreateFPToSI(ms, B.getInt64Ty());
                B.CreateCall(cf, {ms});
                break;
            }

            // ── scan ─────────────────────────────────────────
            case NodeKind::ScanStmt: {
                Value* ptr = lookup(n.value);
                if (!ptr) break;
                Type*  ty  = lookup_type(n.value);
                Value* v;
                if (ty == B.getInt64Ty())
                    v = B.CreateCall(xprt("xprt_scan_int",B.getInt64Ty(),{}));
                else if (ty == B.getDoubleTy())
                    v = B.CreateCall(xprt("xprt_scan_float",B.getDoubleTy(),{}));
                else
                    v = B.CreateCall(xprt("xprt_scan_str",xpstr_ptr_ty(),{}));
                B.CreateStore(v, ptr);
                break;
            }

            // ── return ────────────────────────────────────────
            case NodeKind::ReturnStmt: {
                if (n.children.empty()) {
                    B.CreateRetVoid();
                } else {
                    Value* v = gen_expr(*n.children[0]);
                    Type*  rt = fn->getReturnType();
                    // Same ptr-vs-struct-value mismatch as AtomDecl's
                    // own fix above, for the identical reason: a
                    // SpawnExpr's gen_expr returns a ptr (the address
                    // of a freshly-populated struct alloca), so
                    // `return spawn TodoItem {...}` from a function
                    // declared `-> TodoItem` produced a genuine LLVM
                    // verifier error ("Function return type does not
                    // match operand type of return inst! ret ptr
                    // %TodoItem_spawn") on every real struct-returning
                    // function — caught by running the golden suite
                    // against real generated output.
                    if (rt->isStructTy() && v->getType()->isPointerTy()) {
                        v = B.CreateLoad(rt, v);
                    } else if (v->getType() != rt) {
                        if (rt->isDoubleTy() && v->getType()->isIntegerTy())
                            v = B.CreateSIToFP(v, rt);
                        else if (rt->isIntegerTy(64) && v->getType()->isDoubleTy())
                            v = B.CreateFPToSI(v, rt);
                        else if (rt->isIntegerTy(64) && v->getType()->isIntegerTy(1))
                            v = B.CreateZExt(v, rt);
                        else if (rt->isIntegerTy(1) && v->getType()->isIntegerTy(64))
                            v = B.CreateICmpNE(v, B.getInt64(0));
                    }
                    B.CreateRet(v);
                }
                break;
            }

            case NodeKind::BreakStmt:
                if (!loop_stack.empty()) B.CreateBr(loop_stack.back().end);
                break;

            case NodeKind::ContinueStmt:
                if (!loop_stack.empty()) B.CreateBr(loop_stack.back().cond);
                break;

            // ── if / elif / else ──────────────────────────────
            case NodeKind::IfStmt: {
                if (n.children.size() < 2) break;
                gen_if(n, fn);
                break;
            }

            // ── while ─────────────────────────────────────────
            case NodeKind::WhileStmt: {
                if (n.children.size() < 2) break;
                gen_while(n, fn);
                break;
            }

            // ── for i in start..end ───────────────────────────
            case NodeKind::ForStmt: {
                if (n.children.size() < 2) break;
                gen_for(n, fn);
                break;
            }

            // ── probe / diverge ───────────────────────────────
            case NodeKind::ProbeStmt: {
                if (n.children.empty()) break;
                gen_probe(n, fn);
                break;
            }

            // ── emit ──────────────────────────────────────────
            case NodeKind::EmitStmt: {
                FunctionCallee emit_fn = xprt("xprt_emit", B.getVoidTy(),
                    {opaque_ptr(), opaque_ptr()});
                Value* ev  = make_cstr(n.value);
                Value* dat = make_cstr("");
                B.CreateCall(emit_fn, {ev, dat});
                break;
            }

            // ── absorb ────────────────────────────────────────
            case NodeKind::AbsorbStmt: {
                // Create a handler function for this absorb block
                std::string hname = "xp_absorb_" + sanitize_id(n.value) + "_" +
                                    std::to_string(reinterpret_cast<uintptr_t>(&n));
                FunctionType* hft = FunctionType::get(
                    B.getVoidTy(), {opaque_ptr(), opaque_ptr()}, false);
                Function* hfn = Function::Create(hft,
                    Function::InternalLinkage, hname, M);
                BasicBlock* hentry = BasicBlock::Create(ctx, "entry", hfn);
                // IRBuilder<> is non-copyable in real LLVM (its copy
                // constructor is explicitly deleted — caught the
                // moment this file was first compiled against real
                // LLVM headers, since the stub build this codebase
                // has always used until now never exercised this
                // path at all). saveIP()/restoreIP() are exactly what
                // this pattern is for: capture the outer builder's
                // insertion point, switch into the new handler
                // function, then restore it — no need to (and no way
                // to) copy the builder itself.
                IRBuilderBase::InsertPoint saved = B.saveIP();
                B.SetInsertPoint(hentry);
                push_scope();
                if (!n.children.empty() && n.children[0])
                    gen_block(*n.children[0], hfn);
                B.CreateRetVoid();
                pop_scope();
                B.restoreIP(saved); // restore builder

                // Register handler at startup
                FunctionCallee absorb_fn = xprt("xprt_absorb", B.getVoidTy(),
                    {opaque_ptr(), opaque_ptr()});
                Value* ev = make_cstr(n.value);
                B.CreateCall(absorb_fn, {ev, hfn});
                break;
            }

            // ── vortex (try/catch) ────────────────────────────
            case NodeKind::VortexStmt: {
                // No exception support in LLVM IR without DWARF/SEH
                // Emit body inline; catch block emitted in else path
                if (!n.children.empty() && n.children[0])
                    gen_block(*n.children[0], fn);
                break;
            }

            // ── quantum (thread) ─────────────────────────────
            case NodeKind::QuantumStmt: {
                // Simplified: emit block inline (thread spawn needs xprt_thread_spawn)
                if (!n.children.empty() && n.children[0])
                    gen_block(*n.children[0], fn);
                break;
            }

            // ── Expression statement ──────────────────────────
            case NodeKind::ExprStmt: {
                if (!n.children.empty() && n.children[0])
                    gen_expr(*n.children[0]);
                break;
            }

            case NodeKind::Block:
                gen_block(n, fn);
                break;

            default:
                // For anything else that's expression-like, evaluate it
                if (!n.children.empty())
                    for (auto& c : n.children)
                        if (c) gen_stmt(*c, fn);
                break;
        }
    }

    // ── Block codegen ─────────────────────────────────────────
    void gen_block(const ASTNode& blk, Function* fn) {
        push_scope();
        for (auto& child : blk.children) {
            if (!child) continue;
            if (B.GetInsertBlock() && B.GetInsertBlock()->getTerminator()) break;
            gen_stmt(*child, fn);
        }
        pop_scope();
    }

    // ── if / elif / else ─────────────────────────────────────
    void gen_if(const ASTNode& n, Function* fn) {
        BasicBlock* merge = BasicBlock::Create(ctx, "if_end", fn);

        auto emit_branch = [&](Value* cond, const ASTNode& body,
                                BasicBlock* next) {
            if (!cond->getType()->isIntegerTy(1))
                cond = B.CreateICmpNE(cond, B.getInt64(0));
            BasicBlock* then = BasicBlock::Create(ctx, "then", fn);
            B.CreateCondBr(cond, then, next);
            B.SetInsertPoint(then);
            gen_block(body, fn);
            if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(merge);
        };

        BasicBlock* next = merge;
        // Pre-create "next" blocks for elif/else chains
        std::vector<BasicBlock*> elif_blocks;
        BasicBlock*              else_block = nullptr;
        for (size_t i = 2; i < n.children.size(); i++) {
            if (!n.children[i]) continue;
            if (n.children[i]->kind == NodeKind::ElifStmt) {
                elif_blocks.push_back(BasicBlock::Create(ctx,"elif",fn));
            } else if (n.children[i]->kind == NodeKind::ElseStmt) {
                else_block = BasicBlock::Create(ctx,"else",fn);
            }
        }

        // Determine final "else" landing
        BasicBlock* first_next = merge;
        if (else_block)              first_next = else_block;
        else if (!elif_blocks.empty()) first_next = elif_blocks[0];

        // Emit if
        Value* cond = gen_expr(*n.children[0]);
        if (!cond->getType()->isIntegerTy(1))
            cond = B.CreateICmpNE(cond, B.getInt64(0));
        BasicBlock* then = BasicBlock::Create(ctx,"then",fn);
        B.CreateCondBr(cond, then, first_next);
        B.SetInsertPoint(then);
        gen_block(*n.children[1], fn);
        if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(merge);

        // Emit elif chains
        size_t ei = 0;
        BasicBlock* else_or_merge = else_block ? else_block : merge;
        for (size_t i = 2; i < n.children.size(); i++) {
            if (!n.children[i]) continue;
            if (n.children[i]->kind == NodeKind::ElifStmt) {
                BasicBlock* cur_elif = elif_blocks[ei];
                BasicBlock* nxt = (ei+1 < elif_blocks.size())
                                   ? elif_blocks[ei+1] : else_or_merge;
                B.SetInsertPoint(cur_elif);
                Value* ec = gen_expr(*n.children[i]->children[0]);
                if (!ec->getType()->isIntegerTy(1))
                    ec = B.CreateICmpNE(ec, B.getInt64(0));
                BasicBlock* ethen = BasicBlock::Create(ctx,"elif_then",fn);
                B.CreateCondBr(ec, ethen, nxt);
                B.SetInsertPoint(ethen);
                gen_block(*n.children[i]->children[1], fn);
                if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(merge);
                ei++;
            } else if (n.children[i]->kind == NodeKind::ElseStmt && else_block) {
                B.SetInsertPoint(else_block);
                gen_block(*n.children[i]->children[0], fn);
                if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(merge);
            }
        }
        B.SetInsertPoint(merge);
    }

    // ── while ────────────────────────────────────────────────
    void gen_while(const ASTNode& n, Function* fn) {
        BasicBlock* cond_bb = BasicBlock::Create(ctx,"while_cond",fn);
        BasicBlock* body_bb = BasicBlock::Create(ctx,"while_body",fn);
        BasicBlock* end_bb  = BasicBlock::Create(ctx,"while_end",fn);

        loop_stack.push_back({cond_bb, end_bb});
        B.CreateBr(cond_bb);
        B.SetInsertPoint(cond_bb);
        Value* c = gen_expr(*n.children[0]);
        if (!c->getType()->isIntegerTy(1))
            c = B.CreateICmpNE(c, B.getInt64(0));
        B.CreateCondBr(c, body_bb, end_bb);
        B.SetInsertPoint(body_bb);
        gen_block(*n.children[1], fn);
        if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(cond_bb);
        loop_stack.pop_back();
        B.SetInsertPoint(end_bb);
    }

    // ── for i in start..end ──────────────────────────────────
    void gen_for(const ASTNode& n, Function* fn) {
        auto& iter_node = n.children[0];
        bool is_range = iter_node && iter_node->kind == NodeKind::RangeExpr;

        BasicBlock* cond_bb = BasicBlock::Create(ctx,"for_cond",fn);
        BasicBlock* body_bb = BasicBlock::Create(ctx,"for_body",fn);
        BasicBlock* step_bb = BasicBlock::Create(ctx,"for_step",fn);
        BasicBlock* end_bb  = BasicBlock::Create(ctx,"for_end",fn);

        // Loop variable: `int` unless the numeric typer found sized range bounds
        // (`for i in 0..n` with n: u8 counts in a u8, compares unsigned, ...).
        auto vt = n.attrs.find("var_ty");
        const std::string vty = vt != n.attrs.end() ? vt->second : std::string();
        Type* LT = vty.empty() ? B.getInt64Ty() : xp_type(vty);
        const bool uns = xs_uint(vty);
        Value* loop_var = B.CreateAlloca(LT, nullptr, n.value);
        def_var(n.value, loop_var, LT);

        if (is_range) {
            Value* start = gen_expr(*iter_node->children[0]);
            Value* limit = gen_expr(*iter_node->children[1]);
            // Allocate limit
            Value* lim_var = B.CreateAlloca(LT, nullptr, "for_lim");
            if (start->getType()->isFloatingPointTy()) start = B.CreateFPToSI(start, LT);
            if (limit->getType()->isFloatingPointTy()) limit = B.CreateFPToSI(limit, LT);
            start = fit_numeric(start, LT, !uns);
            limit = fit_numeric(limit, LT, !uns);
            B.CreateStore(start, loop_var);
            B.CreateStore(limit, lim_var);

            loop_stack.push_back({step_bb, end_bb});
            B.CreateBr(cond_bb);
            B.SetInsertPoint(cond_bb);
            Value* cur = B.CreateLoad(LT, loop_var, "cur");
            Value* lim = B.CreateLoad(LT, lim_var, "lim");
            Value* c   = uns ? B.CreateICmpULT(cur, lim) : B.CreateICmpSLT(cur, lim);
            B.CreateCondBr(c, body_bb, end_bb);
            B.SetInsertPoint(body_bb);
            push_scope();
            gen_block(*n.children[1], fn);
            pop_scope();
            if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(step_bb);
            B.SetInsertPoint(step_bb);
            Value* next = B.CreateAdd(B.CreateLoad(LT, loop_var), ConstantInt::get(LT, 1));
            B.CreateStore(next, loop_var);
            B.CreateBr(cond_bb);
            loop_stack.pop_back();
        } else {
            // Used to emit an EMPTY loop here: the body was silently never run.
            throw std::runtime_error("`for " + n.value + " in <collection>` is not supported by the "
                "LLVM backend yet (only `for x in a..b`); use --backend=transpiler");
        }
        B.SetInsertPoint(end_bb);
    }

    // ── probe / diverge ──────────────────────────────────────
    void gen_probe(const ASTNode& n, Function* fn) {
        Value* subj = gen_expr(*n.children[0]);
        BasicBlock* end_bb = BasicBlock::Create(ctx,"probe_end",fn);

        // Enum subject: dispatch on the tag and destructure payload slots.
        // Detected by the subject's LLVM type being a registered
        // `enum.<Name>` struct (a plain identifier loads as the struct
        // value; a direct construction/call yields a pointer to it, which
        // is normalised to a stack slot below so slots can be GEP'd).
        {
            Type* st_ty = nullptr;
            Value* slot_ptr = nullptr;
            if (n.children[0]->kind == NodeKind::Identifier) {
                if (Value* vp = lookup(n.children[0]->value)) {
                    Type* vt = lookup_type(n.children[0]->value);
                    if (vt && vt->isStructTy()) { st_ty = vt; slot_ptr = vp; }
                }
            } else if (subj->getType()->isStructTy()) {
                st_ty = subj->getType();
                slot_ptr = B.CreateAlloca(st_ty, nullptr, "probe_subj");
                B.CreateStore(subj, slot_ptr);
            }
            std::string ename;
            if (auto* sty = st_ty ? dyn_cast<StructType>(st_ty) : nullptr) {
                llvm::StringRef sn = sty->getName();
                if (sn.starts_with("enum.")) ename = sn.substr(5).str();
            }
            if (!ename.empty() && enum_types.count(ename)) {
                const EnumInfo& e = enum_types.at(ename);
                Value* tag = B.CreateLoad(B.getInt32Ty(), B.CreateStructGEP(e.type, slot_ptr, 0), "tag");
                Type* arr_ty = e.type->getElementType(1);
                bool closed = false;

                for (size_t i = 1; i < n.children.size() && !closed; i++) {
                    auto& arm = n.children[i];
                    if (!arm || arm->kind != NodeKind::ProbeArm) continue;
                    BasicBlock* arm_bb  = BasicBlock::Create(ctx, "earm", fn);
                    BasicBlock* next_bb = BasicBlock::Create(ctx, "enext", fn);

                    const EnumVariantInfo* var = nullptr;
                    if (arm->value == "_") {
                        B.CreateBr(arm_bb);
                        closed = true;
                    } else {
                        // pattern is "<Enum>.<Variant>"; take the part after the dot
                        size_t dot = arm->value.find('.');
                        std::string vname = dot == std::string::npos ? arm->value : arm->value.substr(dot + 1);
                        var = find_variant(e, vname);
                        if (!var) throw std::runtime_error("pattern '" + arm->value +
                                        "' names no variant of enum '" + ename + "'");
                        Value* cond = B.CreateICmpEQ(tag, B.getInt32(var->tag));
                        B.CreateCondBr(cond, arm_bb, next_bb);
                    }
                    B.SetInsertPoint(arm_bb);
                    push_scope();
                    // Bind payload slots to the arm's named bindings.
                    auto bit = arm->attrs.find("bindings");
                    if (var && bit != arm->attrs.end() && !bit->second.empty()) {
                        size_t start = 0, idx = 0;
                        const std::string& joined = bit->second;
                        while (start <= joined.size()) {
                            size_t comma = joined.find(',', start);
                            std::string bname = joined.substr(
                                start, comma == std::string::npos ? std::string::npos : comma - start);
                            if (!bname.empty() && idx < var->field_types.size()) {
                                Value* payload = B.CreateStructGEP(e.type, slot_ptr, 1);
                                Value* val = load_field(payload, arr_ty, var->field_off[idx],
                                                        var->field_types[idx]);
                                Type*  vty = xp_type(var->field_types[idx]);
                                Value* home = B.CreateAlloca(vty, nullptr, bname);
                                B.CreateStore(val, home);
                                def_var(bname, home, vty);
                                if (!vec_elem_type(var->field_types[idx]).empty())
                                    def_var_src(bname, var->field_types[idx]);
                            }
                            idx++;
                            if (comma == std::string::npos) break;
                            start = comma + 1;
                        }
                    }
                    if (!arm->children.empty() && arm->children[0]) {
                        if (arm->children[0]->kind == NodeKind::Block) gen_block(*arm->children[0], fn);
                        else gen_stmt(*arm->children[0], fn);
                    }
                    pop_scope();
                    if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(end_bb);
                    B.SetInsertPoint(next_bb);
                }
                if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(end_bb);
                B.SetInsertPoint(end_bb);
                return;
            }
        }

        for (size_t i = 1; i < n.children.size(); i++) {
            auto& arm = n.children[i];
            if (!arm || arm->kind != NodeKind::ProbeArm) continue;

            BasicBlock* arm_bb  = BasicBlock::Create(ctx,"arm",fn);
            BasicBlock* next_bb = BasicBlock::Create(ctx,"next",fn);

            if (arm->value == "_") {
                // Wildcard — always taken
                B.CreateBr(arm_bb);
                B.SetInsertPoint(arm_bb);
                if (!arm->children.empty() && arm->children[0])
                    gen_block(*arm->children[0], fn);
                if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(end_bb);
                B.SetInsertPoint(next_bb);
                B.CreateBr(end_bb);
                break;
            } else {
                // Compare subject against pattern
                Value* cond;
                Type*  st = subj->getType();
                if (st == B.getInt64Ty()) {
                    Value* pat = B.getInt64(std::stoll(arm->value));
                    cond = B.CreateICmpEQ(subj, pat);
                } else if (st == xpstr_ptr_ty()) {
                    Value* pat = make_cstr(arm->value);
                    cond = B.CreateCall(xprt_str_eq_cstr(), {subj, pat});
                } else {
                    cond = B.CreateICmpEQ(subj,
                        ConstantInt::get(st, std::stoll(arm->value)));
                }
                B.CreateCondBr(cond, arm_bb, next_bb);
                B.SetInsertPoint(arm_bb);
                if (!arm->children.empty() && arm->children[0])
                    gen_block(*arm->children[0], fn);
                if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(end_bb);
                B.SetInsertPoint(next_bb);
            }
        }
        if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(end_bb);
        B.SetInsertPoint(end_bb);
    }

    // ── Enum type declaration ─────────────────────────────────
    void gen_enum(const ASTNode& n) {
        if (enum_types.count(n.value)) return;          // already sized (as a dependency)
        if (enum_sizing.count(n.value))
            throw std::runtime_error("enum '" + n.value + "' contains itself by value "
                "(directly or through another enum); a recursive type needs an indirect "
                "payload, which the LLVM backend does not support yet");
        enum_sizing.insert(n.value);

        EnumInfo info;
        unsigned tag = 0;
        for (auto& v : n.children) {
            if (!v || v->kind != NodeKind::EnumVariant) continue;
            EnumVariantInfo vi;
            vi.name = v->value;
            vi.tag  = tag++;
            unsigned off = 0;
            for (auto& pt : v->children) {
                std::string ft = pt ? pt->extra : std::string();
                // A field whose type is another enum needs THAT enum sized
                // first, so its inline width is known.
                auto dit = enum_decls.find(ft);
                if (dit != enum_decls.end()) gen_enum(*dit->second);
                unsigned w = field_words_of(ft);
                vi.field_types.push_back(ft);
                vi.field_off.push_back(off);
                vi.field_words.push_back(w);
                off += w;
            }
            info.slots = std::max<unsigned>(info.slots, off);
            info.variants.push_back(std::move(vi));
        }
        std::vector<Type*> members = { B.getInt32Ty() };
        // Always at least one word so the struct has a stable, non-empty
        // shape even for fieldless enums.
        members.push_back(ArrayType::get(B.getInt64Ty(), std::max(1u, info.slots)));
        info.type = StructType::create(ctx, members, "enum." + n.value);
        enum_types[n.value] = std::move(info);
        enum_sizing.erase(n.value);
    }

    // ── Forge type declaration ────────────────────────────────
    void gen_forge(const ASTNode& n) {
        std::vector<Type*> field_types;
        std::vector<std::string> field_names;
        for (auto& f : n.children) {
            if (!f || f->kind != NodeKind::FieldDecl) continue;
            field_types.push_back(xp_type(f->extra));
            field_names.push_back(f->value);
            if (!f->children.empty() && f->children[0])
                forge_defaults[n.value][f->value] = f->children[0].get();
        }
        StructType* st = StructType::create(ctx, field_types, "forge." + n.value);
        forge_types[n.value]  = st;
        forge_fields[n.value] = field_names;
    }

    // ── extern "ABI" pulse name(params) -> ret  → FFI declaration ──
    // Declares an external symbol for the linker to resolve — no
    // body is emitted. This is how XPhage calls into a C library,
    // a Rust `cdylib`/`staticlib` (functions exported as
    // `#[no_mangle] extern "C" fn ...`), or any other ABI-compatible
    // object. The actual `.a`/`.so`/`.lib` must be supplied to the
    // linker separately (via xphage.pkg native-deps or a manual
    // -l flag) — this only creates the LLVM-side declaration.
    // extern function -> (which parameters are `str`, is the return `str`)
    struct ExternShape { std::vector<bool> str_param; bool str_ret = false; };
    std::unordered_map<std::string, ExternShape> extern_shapes;

    Function* gen_extern(const ASTNode& n) {
        if (Function* existing = M.getFunction(n.value)) return existing;
        {
            ExternShape sh;
            sh.str_ret = n.extra2 == "str";
            for (auto& c : n.children)
                if (c && c->kind == NodeKind::FieldDecl) sh.str_param.push_back(c->extra == "str");
            extern_shapes[n.value] = std::move(sh);
        }

        Type* ret = xp_type(n.extra2);
        std::vector<Type*> param_types;
        for (auto& c : n.children) {
            if (c && c->kind == NodeKind::FieldDecl) {
                param_types.push_back(xp_type(c->extra));
            }
        }
        FunctionType* ft = FunctionType::get(ret, param_types, false);
        Function* fn = Function::Create(ft, Function::ExternalLinkage, n.value, M);
        def_fn(n.value, fn);
        return fn;
    }

    // ── Pulse (function) codegen ──────────────────────────────
    Function* gen_pulse(const ASTNode& n, const std::string& receiver = "") {
        std::string  name   = n.value.empty() ? "main" : n.value;
        std::string  ret_tn = n.extra2;
        bool is_main = (name == "main");

        // Collect params (FieldDecl children before body)
        std::vector<std::pair<std::string,std::string>> params;
        for (auto& c : n.children) {
            if (!c) continue;
            if (c->kind == NodeKind::FieldDecl) {
                params.push_back({c->value, c->extra});
            }
        }

        // Methods: the receiver is parameter 0, always a pointer to the
        // object — whether the method was written with an explicit `self`
        // parameter or, as in the language book, without one. Marked with a
        // sentinel type string and given the opaque pointer type below.
        static const std::string SELF_PTR = "\x01self_ptr";
        // Scope the receiver table to this function: nested generation (an
        // impl method emitted while another body is open) must neither see
        // nor clobber the caller's `self`.
        struct SelfGuard {
            std::unordered_map<std::string, std::string>& m;
            std::unordered_map<std::string, std::string> saved;
            explicit SelfGuard(std::unordered_map<std::string, std::string>& mm)
                : m(mm), saved(std::move(mm)) { m.clear(); }
            ~SelfGuard() { m = std::move(saved); }
        } self_guard(self_forge_);
        if (!receiver.empty()) {
            for (size_t k = 0; k < params.size(); ) {
                if (params[k].first == "self") params.erase(params.begin() + k); else k++;
            }
            params.insert(params.begin(), {"self", SELF_PTR});
            self_forge_["self"] = receiver;
        }

        // Build LLVM function type
        Type* ret = is_main ? B.getInt32Ty() : xp_type(ret_tn);
        std::vector<Type*> param_types;
        for (auto& [pn,pt] : params)
            param_types.push_back(pt == SELF_PTR ? opaque_ptr() : xp_type(pt));

        FunctionType* ft = FunctionType::get(ret, param_types, false);
        // Reuse the forward declaration Pass 2 already created for this
        // name instead of creating a SECOND Function. LLVM resolves a name
        // collision by renaming the newcomer (`main` -> `main.1`), which
        // left the clean symbol attached to an empty, bodiless
        // declaration while the real body lived under the suffixed name.
        // Calls between pulses hid this (they go through the tracked
        // Function*), but the linker resolves by NAME, so the C entry
        // point `main` bound to the empty declaration: every program
        // using an explicit `pulse main()` failed with "undefined
        // reference to `main'" on this backend. Only a matching, still
        // bodiless declaration is reused — a same-named function that
        // already has a body is a genuine duplicate and must stay
        // distinct rather than be silently merged.
        Function* fn = nullptr;
        if (Function* existing = M.getFunction(name)) {
            if (existing->empty() && existing->getFunctionType() == ft) fn = existing;
        }
        if (!fn) fn = Function::Create(ft, Function::ExternalLinkage, name, M);

        // Name params
        size_t i = 0;
        for (auto& arg : fn->args()) {
            if (i < params.size()) {
                arg.setName(params[i].first);
                i++;
            }
        }
        def_fn(name, fn);

        // Find body block
        const ASTNode* body = nullptr;
        for (auto& c : n.children) {
            if (c && c->kind == NodeKind::Block) { body = c.get(); break; }
        }
        if (!body) {
            // Declaration-only (forward decl) — no body needed
            fn->setLinkage(Function::ExternalLinkage);
            return fn;
        }

        BasicBlock* entry = BasicBlock::Create(ctx, "entry", fn);
        B.SetInsertPoint(entry);
        push_scope();

        // Allocate and store each arg
        i = 0;
        for (auto& arg : fn->args()) {
            Type* aty = params[i].second == SELF_PTR ? opaque_ptr() : xp_type(params[i].second);
            Value* alloca = B.CreateAlloca(aty, nullptr, params[i].first);
            // A plain (unqualified) Vec<T> parameter has COPY semantics per
            // the language book: the callee gets its own array and cannot
            // affect the caller's. A Vec is an XpVec* handle here, so
            // storing the incoming pointer would let the callee mutate the
            // caller's data (observed: caller's len grew from 1 to 3). Clone
            // on entry. `ref` / `mut_ref` (and `own`) keep sharing the
            // handle, which is exactly their documented meaning.
            Value* incoming = &arg;
            {
                const std::string& pty = params[i].second;
                bool qualified = pty.compare(0, 4, "ref ") == 0 ||
                                 pty.compare(0, 8, "mut_ref ") == 0 ||
                                 pty.compare(0, 4, "own ") == 0;
                if (!qualified && !vec_elem_type(pty).empty()) {
                    incoming = B.CreateCall(
                        xprt("xprt_vec_clone", opaque_ptr(), {opaque_ptr()}), {&arg}, "vec_copy");
                }
            }
            B.CreateStore(incoming, alloca);
            def_var(params[i].first, alloca, aty);
            if (!vec_elem_type(params[i].second).empty()) def_var_src(params[i].first, params[i].second);
            i++;
        }

        gen_block(*body, fn);
        pop_scope();

        // Add implicit return if block didn't terminate
        if (!B.GetInsertBlock()->getTerminator()) {
            if (ret == B.getVoidTy())   B.CreateRetVoid();
            else if (ret == B.getInt32Ty()) B.CreateRet(B.getInt32(0));
            else if (ret == B.getInt64Ty()) B.CreateRet(B.getInt64(0));
            else if (ret == B.getDoubleTy())B.CreateRet(ConstantFP::get(ret,0.0));
            else if (ret == B.getInt1Ty())  B.CreateRet(B.getInt1(0));
            else B.CreateRetVoid();
        }

        // Verify
        std::string err;
        raw_string_ostream es(err);
        if (verifyFunction(*fn, &es)) {
            errs() << "[xphage llvm] verify error in " << name << ": " << err << "\n";
        }
        return fn;
    }

    static std::string sanitize_id(const std::string& s) {
        std::string r; for(char c:s) r += (isalnum(c)?c:'_'); return r;
    }

    // Flattens every `realm Name { members... }` block's members into
    // the top-level list, each with its declared name qualified as
    // "Name::member" — mirrors ir_lower.cpp's identical RealmDecl
    // handling (see its own extensive comment for the full rationale;
    // this is the same fix, ported to this backend's different
    // architecture). ir_lower.cpp folds this qualification into its
    // single-pass lower_top dispatch directly; this backend instead
    // makes three separate passes directly over the top-level Program
    // vector (forge types, then pulse forward-decls, then bodies), and
    // a realm's nested members are children of the RealmDecl node, not
    // top-level entries — so none of those three passes would ever
    // see them without this flattening happening first, ahead of all
    // three. A nested `realm` recurses naturally, since flattening a
    // nested RealmDecl's own children (after qualifying the nested
    // realm's members with the combined "Outer::Inner::member" prefix)
    // happens inline in the same recursive call, so the eventual
    // top-level list is already fully flat and needs no repeated
    // passes.
    static void flatten_realms(const Program& in, Program& out,
                                const std::string& prefix = "") {
        for (auto& top : in) {
            if (!top) continue;
            if (top->kind == NodeKind::RealmDecl) {
                std::string inner_prefix = prefix.empty() ? top->value
                                          : prefix + "::" + top->value;
                flatten_realms(top->children, out, inner_prefix);
                continue;
            }
            if (top->kind == NodeKind::Block && !top->children.empty()) {
                bool all_extern = true;
                for (auto& c : top->children) if (!c || c->kind != NodeKind::ExternDecl) all_extern = false;
                if (all_extern) { for (auto& c : top->children) out.push_back(c); continue; }
            }
            if (prefix.empty()) {
                out.push_back(top);
                continue;
            }
            if (top->kind == NodeKind::ConstDecl || top->kind == NodeKind::AtomDecl ||
                top->kind == NodeKind::FluxDecl) {
                // Left unqualified deliberately, matching
                // ir_lower.cpp's own identical choice — see its
                // comment on this exact point for the full reasoning
                // (in-realm references resolve unqualified; this
                // backend has no separate qualified-lookup path for
                // these three kinds the way it now does for
                // PulseDecl/PathExpr calls, so keeping the unqualified
                // name is what keeps existing in-realm references
                // working without new resolution machinery here too).
                out.push_back(top);
                continue;
            }
            if (top->kind == NodeKind::ForgeDecl || top->kind == NodeKind::NexusDecl ||
                top->kind == NodeKind::PulseDecl || top->kind == NodeKind::AsyncPulseDecl) {
                auto qualified = std::make_shared<ASTNode>(*top);
                qualified->value = prefix + "::" + top->value;
                out.push_back(qualified);
                continue;
            }
            if (top->kind == NodeKind::ImplDecl) {
                auto qualified = std::make_shared<ASTNode>(*top);
                qualified->extra = prefix + "::" + top->extra;
                out.push_back(qualified);
                continue;
            }
            // LinkStmt/UseDecl/other: no declaration to qualify.
            out.push_back(top);
        }
    }

    // ── Top-level module codegen ──────────────────────────────
    void gen_module(const Program& prog_in) {
        Program prog;
        flatten_realms(prog_in, prog);

        // Pre-pass: collect every `use Realm::member` alias, mirroring
        // ir_lower.cpp's identical collect_uses pre-pass (see its own
        // comment for the wildcard-`use Realm::*` caveat, which
        // applies here too — left unhandled deliberately rather than
        // guessed at).
        for (auto& top : prog) {
            if (!top || top->kind != NodeKind::UseDecl) continue;
            const std::string& path = top->value;
            bool is_wildcard = path.size() >= 3 && path.compare(path.size()-3, 3, "::*") == 0;
            if (is_wildcard) continue;
            size_t last = path.rfind("::");
            if (last == std::string::npos) continue;
            std::string bare = path.substr(last + 2);
            if (!bare.empty()) use_aliases[bare] = path;
        }

        // Pass 1a: enum types. Registered FIRST so a forge field or a
        // pulse signature can name one, and so an enum whose payload is
        // another enum resolves (gen_enum sizes a dependency on demand, so
        // declaration order between enums does not matter).
        for (auto& top : prog) {
            if (top && top->kind == NodeKind::EnumDecl) enum_decls[top->value] = top.get();
        }
        for (auto& top : prog) {
            if (!top) continue;
            if (top->kind == NodeKind::EnumDecl) gen_enum(*top);
        }
        // Pass 1b: forge types (needed before function bodies reference them)
        for (auto& top : prog) {
            if (!top) continue;
            if (top->kind == NodeKind::ForgeDecl) gen_forge(*top);
        }

        // Pass 2: forward-declare all pulses so mutual recursion works
        for (auto& top : prog) {
            if (!top) continue;
            if (top->kind == NodeKind::PulseDecl ||
                top->kind == NodeKind::AsyncPulseDecl) {
                std::string nm = top->value.empty() ? "main" : top->value;
                // Create forward decl
                std::vector<Type*> param_types;
                for (auto& c : top->children) {
                    if (c && c->kind == NodeKind::FieldDecl)
                        param_types.push_back(xp_type(c->extra));
                }
                bool is_main = (nm == "main");
                Type* ret = is_main ? B.getInt32Ty() : xp_type(top->extra2);
                FunctionType* ft = FunctionType::get(ret, param_types, false);
                Function* fn = Function::Create(ft, Function::ExternalLinkage, nm, M);
                def_fn(nm, fn);
            }
            // impl methods: `Forge__method(ptr self, params...)`. Declared up
            // front like pulses, so a call may precede the impl block in the
            // file and methods may call one another.
            if (top->kind == NodeKind::ImplDecl && !top->extra.empty()) {
                for (auto& m : top->children) {
                    if (!m || m->value.empty() ||
                        (m->kind != NodeKind::MethodDecl && m->kind != NodeKind::PulseDecl &&
                         m->kind != NodeKind::AsyncPulseDecl)) continue;
                    std::string nm = top->extra + "__" + m->value;
                    std::vector<Type*> pts = { opaque_ptr() };
                    for (auto& c : m->children)
                        if (c && c->kind == NodeKind::FieldDecl && c->value != "self")
                            pts.push_back(xp_type(c->extra));
                    FunctionType* ft = FunctionType::get(xp_type(m->extra2), pts, false);
                    Function* fn = Function::Create(ft, Function::ExternalLinkage, nm, M);
                    def_fn(nm, fn);
                }
            }
        }

        // Pass 3: emit all pulse bodies + impl methods + top-level stmts
        Function* implicit_main = nullptr;
        BasicBlock* implicit_entry = nullptr;
        bool has_explicit_main = false;

        // Check for explicit main pulse
        for (auto& top : prog) {
            if (!top) continue;
            if ((top->kind == NodeKind::PulseDecl || top->kind == NodeKind::AsyncPulseDecl)
                && (top->value == "main" || top->value.empty())) {
                has_explicit_main = true;
                break;
            }
        }

        // If no explicit main, create implicit one for top-level stmts
        if (!has_explicit_main) {
            FunctionType* mft = FunctionType::get(B.getInt32Ty(), false);
            implicit_main = Function::Create(mft, Function::ExternalLinkage, "main", M);
            implicit_entry = BasicBlock::Create(ctx, "entry", implicit_main);
            B.SetInsertPoint(implicit_entry);
            def_fn("main", implicit_main);
            push_scope();
        }

        for (auto& top : prog) {
            if (!top) continue;
            switch (top->kind) {
                case NodeKind::ForgeDecl:
                case NodeKind::EnumDecl:
                case NodeKind::NexusDecl:
                case NodeKind::LinkStmt:
                    break; // already handled / metadata only

                case NodeKind::RealmDecl:
                    // Every RealmDecl (including nested ones) is fully
                    // consumed by flatten_realms above, before any of
                    // the three passes in this function ever run — a
                    // bare RealmDecl node can no longer reach this
                    // switch at all, since its members were already
                    // hoisted into `prog` under qualified names. Kept
                    // as a no-op case (rather than removed) only as a
                    // defensive fallback.
                    break;

                case NodeKind::UseDecl:
                    // `use Realm::member` import aliasing — NOT
                    // implemented yet (see "still open" in
                    // XIL_RESTORE_STATUS.md's Part -11 for the exact
                    // gap this leaves: an unqualified reference to a
                    // realm member after `use` still requires the
                    // qualified name to resolve; only the qualified-
                    // reference path itself — PathExpr/CallExpr,
                    // fixed above — actually works).
                    break;

                case NodeKind::ExternDecl:
                    gen_extern(*top);
                    break;

                case NodeKind::UnsafeBlock: {
                    // Transparent: lower the inner declarations/statements
                    // through this same top-level dispatch.
                    if (!top->children.empty() && top->children[0]) {
                        for (auto& inner : top->children[0]->children) {
                            if (!inner) continue;
                            if (inner->kind == NodeKind::ExternDecl) {
                                gen_extern(*inner);
                            } else if (implicit_main) {
                                gen_stmt(*inner, implicit_main);
                            }
                        }
                    }
                    break;
                }

                case NodeKind::GlobalDecl: {
                    if (!implicit_main) break;
                    // Global as local in implicit main
                    Type* ty = B.getInt64Ty();
                    Value* a  = B.CreateAlloca(ty, nullptr, top->value);
                    def_var(top->value, a, ty);
                    if (!top->children.empty() && top->children[0]) {
                        Value* v = gen_expr(*top->children[0]);
                        B.CreateStore(v, a);
                    }
                    break;
                }

                case NodeKind::ConstDecl: {
                    // A top-level `const` needs the exact same fix as
                    // `flux` just below, for a related but distinct
                    // reason: ordinary top-level consts (e.g. `const
                    // LIMIT: int = 100` in the golden order_independence
                    // test, which passes) happened to already work when
                    // routed through gen_stmt's local-scoped ConstDecl
                    // case, ONLY because every reference to them was
                    // also inside the same implicit-main body. A
                    // realm-scoped const (e.g. Geometry::PI, left
                    // unqualified by flatten_realms per its own
                    // documented reasoning — matching how it's
                    // referenced unqualified from other pulses in the
                    // same realm) breaks that assumption: it's declared
                    // here (as a top-level statement, hitting this
                    // switch) but read from Geometry::circle_area, a
                    // genuinely separate LLVM Function — so a plain
                    // CreateAlloca inside implicit_main was invisible
                    // there, and (worse than flux's old silent-zero
                    // symptom) each *reference* to PI inside
                    // circle_area apparently re-declared its own local
                    // via the SAME "not found → fall through to a
                    // fresh local" pattern, producing two conflicting
                    // `%PI` SSA definitions in one function — a real,
                    // caught-by-running LLVM verifier error
                    // ("Instruction does not dominate all uses!"),
                    // followed by a segfault once the module ran
                    // anyway despite the verifier's complaint. Same
                    // fix as FluxDecl below: a real GlobalVariable
                    // registered into scopes[0] rather than an alloca
                    // in whichever function happens to be emitting it.
                    // Applied unconditionally (not just for
                    // realm-originated consts) since a global is
                    // strictly more correct for a top-level const in
                    // every case, including the ones that already
                    // happened to work under the old local-alloca
                    // behavior purely by coincidence.
                    Type* ty = xp_type(top->extra);
                    auto* g = new GlobalVariable(M, ty, /*isConstant=*/false,
                        GlobalValue::InternalLinkage, Constant::getNullValue(ty),
                        "const." + top->value);
                    scopes[0].vars[top->value] = g;
                    scopes[0].var_types[top->value] = ty;
                    if (!top->children.empty() && top->children[0] && implicit_main) {
                        Value* v = gen_expr(*top->children[0]);
                        B.CreateStore(v, g);
                    }
                    break;
                }

                case NodeKind::FluxDecl: {
                    if (!implicit_main) break;
                    // `flux` is XPhage's mutable global state, meant
                    // to be visible from anywhere — including an
                    // `absorb "event" { ... }` handler, which is its
                    // own separate LLVM Function with a fresh,
                    // independent scope stack. A plain CreateAlloca
                    // here put the storage on main's stack frame,
                    // which is invisible to any other function by
                    // construction — every `absorb` handler's read of
                    // a flux variable silently fell through
                    // Identifier's own gen_expr "not found" fallback
                    // (a plain `i64 0`, not a compile error — see that
                    // case's own comment), so `absorb "purchase" {
                    // event_count = event_count + 1 }` never actually
                    // incremented anything visible outside the handler
                    // either. Caught by running the golden events test
                    // against real generated output.
                    //
                    // Fixed with a real, module-level GlobalVariable
                    // instead — registered into scopes[0] (the base
                    // scope pushed once in the constructor and never
                    // popped, unlike every per-function scope pushed
                    // by push_scope/pop_scope) rather than
                    // scopes.back(), so lookup()'s scope-stack walk
                    // (which always checks all the way down to index 0
                    // as a last resort) finds it from ANY function, not
                    // just whichever one happened to be on top when it
                    // was declared — the same reason ordinary def_var
                    // callers use scopes.back(): every OTHER kind of
                    // variable genuinely should be scoped to wherever
                    // it's declared, only `flux`'s "reachable from
                    // anywhere" semantics need this base-scope
                    // treatment. A GlobalVariable needs a Constant
                    // initializer (not an arbitrary computed Value*),
                    // so it's zero-initialized here and the real
                    // initializer expression (if any) is stored into
                    // it as a normal instruction in main's body, same
                    // as the alloca version already did.
                    Type* ty = xp_type(top->extra);
                    auto* g = new GlobalVariable(M, ty, /*isConstant=*/false,
                        GlobalValue::InternalLinkage, Constant::getNullValue(ty),
                        "flux." + top->value);
                    scopes[0].vars[top->value] = g;
                    scopes[0].var_types[top->value] = ty;
                    if (!top->children.empty() && top->children[0]) {
                        Value* v = gen_expr(*top->children[0]);
                        B.CreateStore(v, g);
                    }
                    break;
                }

                case NodeKind::PulseDecl:
                case NodeKind::AsyncPulseDecl: {
                    // Restore implicit_main insertion point after each pulse
                    BasicBlock* saved_bb = nullptr;
                    if (implicit_main && B.GetInsertBlock()) {
                        saved_bb = B.GetInsertBlock();
                    }
                    gen_pulse(*top);
                    if (saved_bb) B.SetInsertPoint(saved_bb);
                    break;
                }

                case NodeKind::ImplDecl: {
                    for (auto& method : top->children) {
                        if (!method || method->value.empty() ||
                            (method->kind != NodeKind::MethodDecl && method->kind != NodeKind::PulseDecl &&
                             method->kind != NodeKind::AsyncPulseDecl)) continue;
                        // Emit as free function: ForgeType__method_name(self, ...)
                        ASTNode synthetic = *method;
                        synthetic.value = top->extra + "__" + method->value;
                        BasicBlock* saved = implicit_main ? B.GetInsertBlock() : nullptr;
                        gen_pulse(synthetic, top->extra);
                        if (saved) B.SetInsertPoint(saved);
                    }
                    break;
                }

                case NodeKind::AbsorbStmt: {
                    if (implicit_main) gen_stmt(*top, implicit_main);
                    break;
                }

                default: {
                    // All other top-level stmts → implicit main
                    if (implicit_main) gen_stmt(*top, implicit_main);
                    break;
                }
            }
        }

        // Close implicit main
        if (implicit_main) {
            pop_scope();
            if (!B.GetInsertBlock()->getTerminator())
                B.CreateRet(B.getInt32(0));
        }
    }
};

// ─────────────────────────────────────────────────────────────
// Optimisation helper
// ─────────────────────────────────────────────────────────────
static void run_optimisations(Module& M, TargetMachine* TM, int opt_level) {
    if (opt_level == 0) return;

#ifdef XP_USE_NEW_PM
    LoopAnalysisManager      LAM;
    FunctionAnalysisManager  FAM;
    CGSCCAnalysisManager     CGAM;
    ModuleAnalysisManager    MAM;

    PassBuilder PB(TM);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    OptimizationLevel ol;
    switch (opt_level) {
        case 1:  ol = OptimizationLevel::O1; break;
        case 3:  ol = OptimizationLevel::O3; break;
        default: ol = OptimizationLevel::O2; break;
    }
    auto MPM = PB.buildPerModuleDefaultPipeline(ol);
    MPM.run(M, MAM);
#else
    legacy::PassManager PM;
    TM->addPassesToEmitFile(PM, *raw_null_ostream::Create(), nullptr,
                             XP_ASM_FILETYPE);
#endif
}

// ─────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────
namespace xphage::codegen_llvm {

bool is_available() { return true; }

std::string llvm_version_str() {
    return std::string(LLVM_VERSION_STRING) +
           " (LLVM " + std::to_string(LLVM_VERSION_MAJOR) + ")";
}

LLVMResult compile_ast(const Program& ast,
                        const std::string& output_obj,
                        const LLVMConfig& cfg) {
    LLVMResult res;
    auto t0 = std::chrono::steady_clock::now();

    // Initialise all LLVM targets
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    InitializeAllAsmPrinters();

    LLVMContext ctx;
    auto mod = std::make_unique<Module>("xphage", ctx);

    // Codegen
    try {
        ASTCodegen cg(ctx, *mod);
        cg.gen_module(ast);
    } catch (const std::exception& e) {
        res.error = std::string("Codegen exception: ") + e.what();
        return res;
    }

    // Optional: dump LLVM IR
    if (cfg.emit_llvm_ir) {
        std::string ir_path = output_obj + ".ll";
        std::error_code ec;
        raw_fd_ostream ll_out(ir_path, ec);
        if (!ec) {
            mod->print(ll_out, nullptr);
            res.llvm_ir_path = ir_path;
        }
    }

    // Select target
    std::string triple = cfg.target_triple.empty()
        ? sys::getDefaultTargetTriple() : cfg.target_triple;
    mod->setTargetTriple(triple);

    std::string err_str;
    const Target* tgt = TargetRegistry::lookupTarget(triple, err_str);
    if (!tgt) { res.error = "Target lookup failed: " + err_str; return res; }

    TargetOptions opts;
    XP_RM rm;
    if (cfg.pic) rm = Reloc::PIC_;

    // "native" is a clang/llvm-mc CLI convenience alias — it is NOT a
    // real MCPU string the C++ TargetMachine API understands (caught
    // by actually running this against real LLVM for the first time:
    // createTargetMachine("native", ...) doesn't error out cleanly,
    // it silently produces a subtarget that doesn't match the host's
    // real capabilities — "'native' is not a recognized processor for
    // this target" from LLVM's own diagnostics, followed by a fatal
    // "64-bit code requested on a subtarget that doesn't support it").
    // The correct way to ask for "whatever CPU is actually running
    // this" in the C++ API is sys::getHostCPUName().
    std::string cpu = cfg.cpu.empty() || cfg.cpu == "native"
                     ? std::string(sys::getHostCPUName()) : cfg.cpu;
    std::string feat = cfg.features;

    auto TM = std::unique_ptr<TargetMachine>(
        tgt->createTargetMachine(triple, cpu, feat, opts, rm));
    mod->setDataLayout(TM->createDataLayout());

    // Reloc::PIC_ on the TargetMachine alone was not enough — caught
    // by actually linking real generated output: a program with any
    // string literal (needing a real relocation against .rodata)
    // failed with "relocation R_X86_64_32 ... can not be used when
    // making a PIE object", even with cfg.pic true. The module itself
    // also needs its own PIC/PIE level module flags for the code
    // generator to consistently treat every relocation as PIC-safe —
    // Reloc::Model on the TargetMachine governs the default codegen
    // relocation model, but these module flags are what several
    // codegen paths (including how a global constant's address gets
    // materialized) actually key off of. BigPIC/Large matches what a
    // normal position-independent Linux executable needs — the same
    // thing `-fPIE -fPIC` would produce from clang/gcc — matching
    // what the system `cc` this backend shells out to for the final
    // link step (see link_binary below) links by default.
    if (cfg.pic) {
        mod->setPICLevel(PICLevel::BigPIC);
        mod->setPIELevel(PIELevel::Large);
    }

    // Optimise
    run_optimisations(*mod, TM.get(), cfg.opt_level);

    // Verify module
    std::string verr;
    raw_string_ostream vs(verr);
    if (verifyModule(*mod, &vs)) {
        if (cfg.verbose) errs() << "[xphage llvm] module verify warning: " << verr << "\n";
    }

    // Emit object
    std::error_code ec;
    raw_fd_ostream dest(output_obj, ec, sys::fs::OF_None);
    if (ec) { res.error = "Cannot open output: " + ec.message(); return res; }

    legacy::PassManager PM;
    if (TM->addPassesToEmitFile(PM, dest, nullptr, XP_OBJ_FILETYPE)) {
        res.error = "Cannot emit object file for this target";
        return res;
    }
    PM.run(*mod);
    dest.flush();

    auto t1 = std::chrono::steady_clock::now();
    res.elapsed_ms = std::chrono::duration<double,std::milli>(t1-t0).count();
    res.output_obj  = output_obj;
    res.success     = true;

    if (cfg.verbose)
        outs() << "[llvm] " << triple << " → " << output_obj
               << "  (" << res.elapsed_ms << " ms)\n";
    return res;
}

LLVMResult compile_ir(const xphage::ir::IRModule& mod,
                       const std::string& output_obj,
                       const LLVMConfig& cfg) {
    LLVMResult r;
    r.error = "compile_ir: use compile_ast for Phase 4. LLVM IR module path in Phase 5.";
    return r;
}

bool link_binary(const std::vector<std::string>& objs,
                 const std::string& xprt_lib,
                 const std::string& output_bin,
                 bool verbose) {
    // Build link command: cc objs... xprt.a -lm -lpthread -o bin
    std::string cmd = "cc";
    for (auto& o : objs) cmd += " \"" + o + "\"";
    if (!xprt_lib.empty()) cmd += " \"" + xprt_lib + "\"";
    cmd += " -lm -lpthread";
#if !defined(_WIN32)
    cmd += " -lstdc++fs";
#endif
    cmd += " -o \"" + output_bin + "\"";
    if (verbose) errs() << "[xphage link] " << cmd << "\n";
    return std::system(cmd.c_str()) == 0;
}

} // namespace xphage::codegen_llvm

// Legacy bridge
void XPhageLLVMCompiler::compile_tokens(const std::vector<Token>& toks,
                                         std::string output_obj) {
    fprintf(stderr, "[llvm] token-based compile not supported in v4 — use compile_ast\n");
}

#endif // ENABLE_LLVM
