#pragma once
// ============================================================
// xphage_codegen_llvm — LLVM Native Backend v4.0.0
// AST → LLVM IR → native object → binary (no C++ intermediate)
// LLVM 16-21 compatible (version-gated includes)
// AeonCoreX Lab
// ============================================================
#include "xphage/ast.hpp"
#include "xphage/ir.hpp"
#include <string>
#include <vector>

namespace xphage::codegen_llvm {

struct LLVMConfig {
    std::string  target_triple;     // "" = host auto-detect
    std::string  cpu        = "native";
    std::string  features   = "";
    int          opt_level  = 2;    // 0=none 1=less 2=default 3=aggressive
    bool         debug_info = false;
    // Defaults to true (position-independent code) — caught by
    // actually linking real generated output for the first time: with
    // this false, LLVM's TargetMachine leaves the relocation model
    // unset, which falls back to LLVM's own static-relocation default
    // — but the system `cc` this backend shells out to for the final
    // link step (see link_binary below) links a PIE executable by
    // default on this (and virtually every modern Linux) toolchain,
    // which requires PIC object code. The mismatch only actually
    // surfaces once generated code touches something needing a real
    // relocation against .rodata (e.g. a string literal) — a
    // plain-arithmetic program with no strings linked successfully
    // even with the old `false` default, which is why this wasn't
    // caught immediately.
    bool         pic        = true;
    bool         emit_llvm_ir = false;   // write .ll alongside .o
    bool         verbose    = false;
};

struct LLVMResult {
    bool        success     = false;
    std::string output_obj;          // path to .o
    std::string output_bin;          // path to linked binary (if link=true)
    std::string llvm_ir_path;        // .ll dump if emit_llvm_ir
    std::string error;
    double      elapsed_ms  = 0.0;
};

// Primary: compile AST → native .o  then link with xprt
LLVMResult compile_ast(const Program&         ast,
                        const std::string&     output_obj,
                        const LLVMConfig&      cfg = {});

// IR module → native .o
LLVMResult compile_ir(const xphage::ir::IRModule& mod,
                       const std::string&     output_obj,
                       const LLVMConfig&      cfg = {});

// Link object file(s) + xprt → executable
bool link_binary(const std::vector<std::string>& objs,
                 const std::string&              xprt_lib,
                 const std::string&              output_bin,
                 bool                            verbose = false);

bool        is_available();
std::string llvm_version_str();

} // namespace xphage::codegen_llvm

// Legacy bridge
class XPhageLLVMCompiler {
public:
    void compile_tokens(const std::vector<Token>& tokens,
                        std::string output_obj);
};
