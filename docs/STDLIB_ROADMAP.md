# X-Phage standard library roadmap

Status as measured on this tree (golden suite: transpiler 119/119, llvm 119/119, xil 115/119).

## 1. Layering (modelled on Rust's `library/`)

Rust splits its standard library into `core` (no heap, no OS), `alloc` (heap
types) and `std` (OS services), with the OS-specific code isolated in a
platform-abstraction layer under `std/src/sys`. X-Phage follows the same split:

| layer | needs | contents |
|---|---|---|
| `core` | nothing | numerics, `char`, `Option`/`Result`, `cmp` (Ordering, min/max), `ops`, `clone`/`default`/`convert`, `fmt` (Display/Debug), `iter`, `str`/`slice`, `hash`, `mem`, `time::Duration`, `panic` |
| `alloc` | allocator | `Vec`, `String`, `Box`, `Rc`/`Arc`, `VecDeque`, `BinaryHeap`, `BTreeMap`/`BTreeSet`, `format` |
| `std` | OS | `HashMap`/`HashSet` (needs OS randomness), `io`, `fs`, `path`, `env`, `process`, `thread`, `sync`, `time::Instant`, `net`, `error::Error`; `sys/` = the only place with `extern "C"` |
| packages (`xpm`) | - | `net` protocols, `crypt`, `ai`, `solver`, regex, json, http, async runtime - kept out of `std`, as Rust keeps them out |

## 2. Production-ready gate (every module)
1. Written in X-Phage (no C++ inlined in the transpiler).
2. Value-level golden tests on all three backends.
3. Failure is a `Result`, never a silent empty/false/0.
4. Docs + a runnable example.
5. No string-handle API (`vec_push(v: str, ...)`) left behind.

## 3. Phase 0 - language prerequisites (measured blockers)
| # | item | state |
|---|---|---|
| 0.1 | nexus: contracts, implicit `self`, default methods, `Self`, multi-bound, conformance | **done** (this delivery) |
| 0.2 | receiver-less nexus functions (`Default::default`, `From::from`), `Type::fn()` / `T::fn()` call syntax, explicit type arguments `f<T>()` | **done** |
| 0.3 | impls on primitives (`impl Ord for int`), methods on primitive receivers | **done** (receiver type must be statically known; `int::f()` cannot yet be written directly in source, only reached through `T::f()`) |
| 0.4 | generic nexus (`From<T>`), associated types (`Iterator::Item`) | open |
| 0.5 | numeric types: `u8 i32 u64 f32 usize ...` | **done** (see docs/NUMERIC_TYPES.md); `char` + char literals still open (0.5b) |
| 0.6 | function values / closures (`\|x\| x * 2` parses; analyzer rejects `x`) | open |
| 0.7 | iterator protocol (`for-in` on Map yields a raw pair; XIL/LLVM lack `for-in` over Vec) | open |
| 0.8 | FFI: C-ABI integer widths, no clash with libc header declarations | **done** for integer/float widths and `str` marshalling; pointers/structs by value still open |
| 0.9 | dynamic dispatch (`dyn`) | open, needs design |
| 0.10 | backend parity: XIL (4 known failures), LLVM Map/Set, forge-payload enums, Vec ownership | open |

## 4. Module order after Phase 0
`core` complete -> `alloc` typed collections (replace compiler-builtin Vec/Map/Set)
-> `std::io/fs/path/env/process/time` -> `sync`/`thread` -> `net` -> packages.
