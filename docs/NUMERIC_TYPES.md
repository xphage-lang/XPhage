# Sized numeric types

A language extension (the book defines only `int`, `float`, `bool`, `str`).
Implemented by the lexer (literals), `compiler/xphage_sema/src/numeric_types.cpp`
(typing, diagnostics, annotations) and all three backends.

## Types
`i8 i16 i32 i64 isize` · `u8 u16 u32 u64 usize` · `f32 f64`, with the aliases
`int = i64`, `float = f64`, `byte = u8`. `isize`/`usize` are 64-bit. These names are
ordinary identifiers (a variable called `byte` is still legal).

## Literals
`42`, `1_000`, `0xFF`, `0b1010_1010`, `3.14`, `1e-9`; optional suffix `i8 … usize f32 f64`
(`255u8`, `1.5f32`, `0xFFu32`). An unsuffixed integer literal takes the type its
context expects (declaration, parameter, return type, field, the other operand) and must
fit it (`XP2051`); with no context it is `int`. An integer literal may stand where a float
is expected (`atom x: f32 = 3`). Hex/binary literals up to 2^64-1 are accepted for `int`
(they wrap, as before).

## Rules
* **No implicit conversion** once a sized type is involved (`XP2050`): operands,
  comparisons, assignment, arguments, returns and field initializers must have the same
  type. Convert with `cast(x, T)`. `int`↔`float` mixing with no sized type involved is
  unchanged.
* Integer arithmetic **wraps** (two's complement) at the type's width. Shifts mask the
  count to the width (`XP2053` for a float operand or count). Unary `-` on an unsigned
  type is rejected (`XP2052`). Division by zero is the platform's (unchanged).
* `cast(x, T)`: int→int wraps/extends by the **source** type's signedness; int→float rounds
  to nearest; float→int truncates toward zero and **saturates** (NaN → 0); float→float rounds.
* `beam`/f-strings print every integer as a number (`u8` is not a character), `f32` like
  `float` (6 significant digits).
* FFI: `extern "C"` signatures use the exact C widths (`i32`, `usize`, …); `str` is marshalled
  to `const char*` on every backend.
* Compound operators: `+= -= *= /= %= &= |= ^= <<= >>=`.

## What the typer annotates (for backend authors)
Runs after monomorphization (generics are concrete): `attrs["ty"]` on sized expressions,
`attrs["opty"]` on operators/assignments (the type their operands share), `attrs["from"]` on a
cast, `attrs["var_ty"]` on a `for` whose variable is sized; an un-annotated declaration of
sized type gets its `extra` filled in. Type names are canonicalized in place.

## Not included
`char` and string/byte conversions (next), numeric methods (`abs`, `pow`, `checked_add`…),
`Vec<u8>` memory packing on the LLVM runtime (elements are stored in 64-bit slots),
overflow-trapping modes.
