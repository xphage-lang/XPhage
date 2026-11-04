<div align="center">

# ⚡ X-Phage Language v4.0.0

**C++ speed · Rust safety · Python expressiveness · Own GPU UI**

*AeonCoreX Lab — One language from OS kernel to mobile app*

![Phase 1-3 Complete](https://img.shields.io/badge/Phases_1--3-Complete-brightgreen)
![C++17 Backend](https://img.shields.io/badge/Backend-C%2B%2B17-blue)
![Tests](https://img.shields.io/badge/Tests-28%2F28_passing-brightgreen)
![License](https://img.shields.io/badge/License-MIT-purple)

</div>

---

## Quick Start

```bash
# Build
git clone https://github.com/AeonCoreX-Lab/X-Phage
cd X-Phage && bash scripts/build.sh

# Run
./build/xphage examples/hello.xp0

# REPL
./build/xphage repl

# Emit C++
./build/xphage --emit=cpp examples/phase2_types.xp0
```

## Hello X-Phage

```xphage
// hello.xp0
atom name: str = "World"
beam f"Hello, {name}!"

pulse factorial(n: int) -> int {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}

beam f"10! = {factorial(10)}"
```

## Phase Status

| Phase | Status | Features |
|-------|--------|----------|
| **1 — Foundation**    | ✅ Complete | Lexer · Pratt parser · AST · C++17 transpiler · if/elif/else · while · for/in · pulse · types · f-string · pipeline \|> · vortex · quantum · chronos · bypass |
| **2 — Type System**   | ✅ Complete | forge (struct) · nexus (interface) · flux (reactive) · impl · probe/diverge · emit/absorb (EventBus) · lambda \|x\| · cast · spawn |
| **3 — Systems Power** | ✅ Complete | proc (shell capture) · env · own/ref/mut_ref · async/await · yield · glob · Phase 3 error ? · use |
| **4 — LLVM Backend**  | 🔲 Planned  | Native binary, no C++ intermediate |
| **5 — Fusion UI**     | 🔲 Planned  | Vulkan/Metal/WebGPU GPU backends |
| **6 — Generics**      | 🔲 Planned  | `pulse max<T>`, trait bounds |
| **7 — NPU/GPU**       | 🔲 Planned  | `accelerate` keyword, SIMD dispatch |
| **8 — Bare Metal**    | 🔲 Planned  | no-std, OS kernel mode |
| **9 — Self-hosting**  | 🔲 Long-term | Compiler written in X-Phage |

## Architecture

```
Source (.xp0 / .xh / .xui)
  → Lexer       (115+ tokens, f-strings, ~link, |> ..)
  → Pratt Parser (all Phase 1-3 constructs → AST)
  → IR Lowering  (AST → XPIR — SSA-like IR)
  → Transpiler   (AST → C++17 — direct, readable output)
  → c++/g++      (C++17 → native binary)
```

## Compiler CLI

```
xphage <file.xp0>         Compile and run
xphage build <file.xp0>   Compile to binary
xphage repl               Interactive REPL
xphage --emit=cpp         Show generated C++
xphage --emit=ir          Show XPIR text
xphage --emit=ast         Dump AST
xphage --libs             List stdlib modules
xphage -O                 Optimized build (-O3)
xphage --version          Show version
```

## Standard Library

```xphage
~link "io"          // file I/O, proc, env, path
~link "math"        // sin/cos/sqrt, Vec2/Vec3, PI
~link "string"      // str_trim/upper/lower/split/join
~link "collections" // range, vec_map/filter, Option, Result
~link "net"         // http_get/post, WebSocket  (-lcurl)
~link "os"          // threads, mutex, signals   (-lpthread)
~link "crypt"       // sha256, aes, rsa, uuid    (-lssl)
~link "ai"          // Tensor, nn_linear, llm_load/generate
```

## License

MIT — AeonCoreX Lab
