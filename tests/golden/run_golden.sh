#!/usr/bin/env bash
# ============================================================
# X-Phage Golden Test Suite v4.1.0
# Verifies: single-file .xp / Tri-Modular / Mixed combos
# all produce IDENTICAL output. Declaration order independence.
# AeonCoreX Lab
# ============================================================
XPHAGE="${XPHAGE:-/tmp/xphage2}"
DIR="$(cd "$(dirname "$0")" && pwd)"
PASS=0; FAIL=0
G="\033[1;32m"; R="\033[1;31m"; Y="\033[1;33m"; C="\033[1;36m"; X="\033[0m"

# Backend under test. Previously `run_golden.sh --backend=llvm` was
# accepted and silently IGNORED (run() forwarded fixed arguments), so the
# suite only ever exercised the default backend even when LLVM was
# requested. Select explicitly with BACKEND=llvm (or --backend=llvm); the
# flag is appended to every compiler invocation. Default is unchanged.
BACKEND="${BACKEND:-}"
for _a in "$@"; do
    case "$_a" in --backend=*) BACKEND="${_a#--backend=}";; esac
done
BACKEND_FLAG=""
[ -n "$BACKEND" ] && BACKEND_FLAG="--backend=$BACKEND"
echo "Backend under test: ${BACKEND:-default}"

# `~link "core"` etc. resolve against <repo>/library; make that independent of
# the directory the suite is started from.
export XPHAGE_HOME="${XPHAGE_HOME:-$(cd "$DIR/../.." && pwd)}"

# stdout only / stderr only (the compiler's own progress lines are filtered as
# in run()). Needed where the two streams are asserted separately, e.g. panic.
run_stdout() { "$XPHAGE" "$@" $BACKEND_FLAG 2>/dev/null | grep -v "^\[ok\]" | grep -v "^\[xp " | grep -v "^\[multi\]"; }
run_stderr() { "$XPHAGE" "$@" $BACKEND_FLAG 2>&1 >/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep '^panic:'; }
run() { "$XPHAGE" "$@" $BACKEND_FLAG 2>&1 | grep -v "^\[ok\]" | grep -v "^\[xp " | grep -v "^\[multi\]"; }

check_eq() {
    local name="$1" got="$2" exp="$3"
    if [ "$got" = "$exp" ]; then
        echo -e "  ${G}✓${X} $name"; PASS=$((PASS+1))
    else
        echo -e "  ${R}✗${X} $name"
        echo -e "    ${Y}expected:${X}"; echo "$exp" | sed 's/^/      /'
        echo -e "    ${Y}got:${X}";      echo "$got" | sed 's/^/      /'
        FAIL=$((FAIL+1))
    fi
}
# A check that only makes sense on one backend (the default counts as
# "transpiler"). Elsewhere it is reported as skipped, never as a pass, so a
# known unsupported feature is visible in the output instead of hidden.
check_eq_only() {
    local only="$1"; shift
    if [ -z "$BACKEND" ] && [ "$only" = "transpiler" ] || [ "$BACKEND" = "$only" ]; then
        check_eq "$@"
    else
        echo -e "  ${Y}-${X} $1  (skipped: needs backend '$only')"
    fi
}
# Error code (XPnnnn) of the first diagnostic the compiler prints for a program.
err_code() { run "$@" | sed 's/\x1b\[[0-9;]*m//g' | grep -o 'XP[0-9]\{4\}' | head -1; }
section() { echo -e "\n${C}── $1 ──${X}"; }

section "Calculator (3 file formats)"
CALC_EXP="calc=42
sq(7)=49
fib(10)=34
max=99"
A=$(run "$DIR/calculator/calc.xp")
B=$(run "$DIR/calculator/calc.xh" "$DIR/calculator/calc.xp0")
C=$(run "$DIR/calculator/calc_split.xp")
check_eq "single .xp"     "$A" "$CALC_EXP"
check_eq ".xh + .xp0"     "$B" "$CALC_EXP"
check_eq "A == B"          "$A" "$B"
check_eq "B == C"          "$B" "$C"

section "Todo Logic (2 formats)"
TODO_EXP="added: Buy milk
added: Write code
done: Buy milk
open=1"
A=$(run "$DIR/todo/todo.xp")
B=$(run "$DIR/todo/todo.xh" "$DIR/todo/todo.xp0")
check_eq "single .xp" "$A" "$TODO_EXP"
check_eq ".xh + .xp0" "$B" "$TODO_EXP"
check_eq "A == B"      "$A" "$B"

section "Event System (2 formats)"
EV_EXP="login: nahid
purchase: book
purchase: pen
total=2"
A=$(run "$DIR/events/events.xp")
B=$(run "$DIR/events/events.xp0")
check_eq "single .xp" "$A" "$EV_EXP"
check_eq ".xp0 only"  "$B" "$EV_EXP"
check_eq "A == B"      "$A" "$B"

section "Config Loader (2 formats)"
CFG_EXP="host=api.xphage.dev
port=443
valid=true"
A=$(run "$DIR/config/config.xp")
B=$(run "$DIR/config/config.xh" "$DIR/config/config.xp0")
check_eq "single .xp" "$A" "$CFG_EXP"
check_eq ".xh + .xp0" "$B" "$CFG_EXP"
check_eq "A == B"      "$A" "$B"

section "Declaration Order Independence (3 orderings)"
ORD_EXP="val=50
label=squared
counter=5
limit=100"
O1=$(run "$DIR/order_independence/order1.xp")
O2=$(run "$DIR/order_independence/order2.xp")
O3=$(run "$DIR/order_independence/order3.xp")
check_eq "order1 (forge→pulse→flux→exec)" "$O1" "$ORD_EXP"
check_eq "order2 (pulse→forge→exec→flux)" "$O2" "$ORD_EXP"
check_eq "order3 (flux→pulse→forge→exec)" "$O3" "$ORD_EXP"
check_eq "1==2" "$O1" "$O2"
check_eq "2==3" "$O2" "$O3"

section "Mixed Combinations (2 input combos)"
MX_EXP="localhost:8080
requests=2"
A=$(run "$DIR/mixed_combo/combo_a.xp")
B=$(run "$DIR/mixed_combo/combo_b.xh" "$DIR/mixed_combo/combo_b.xp0")
check_eq "combo_a: all in .xp" "$A" "$MX_EXP"
check_eq "combo_b: .xh + .xp0" "$B" "$MX_EXP"
check_eq "A == B" "$A" "$B"

section "Realm/Namespace (2 formats — types, nested realm, use)"
REALM_EXP="len_sq=25
area=12.5664
champion"
A=$(run "$DIR/realm_test/realm.xp")
B=$(run "$DIR/realm_test/realm.xh" "$DIR/realm_test/realm.xp0")
check_eq "single .xp"  "$A" "$REALM_EXP"
check_eq ".xh + .xp0"  "$B" "$REALM_EXP"
check_eq "A == B"       "$A" "$B"

echo ""
section "Generics (monomorphization)"
# Identifier args + nested generic parameter types must infer T; generic
# templates must never leak an un-substituted Box__T into the output.
check_eq "identifier arg infers T (Box<T>)"   "$(run "$DIR/generics/box_identity_infer.xp")"   "v=42"
check_eq "multiple type params (Pair<A,B>)"   "$(run "$DIR/generics/multi_param.xp")"          "a=7 b=hello"
check_eq "Vec<T> parameter, two instantiations" "$(run "$DIR/generics/vec_param.xp")"          "f=10
g=ann"
check_eq "shared specialization (Box<int> x2)" "$(run "$DIR/generics/shared_specialization.xp")" "x=1 y=2"
check_eq "nested generics Box<Box<int>>"      "$(run "$DIR/generics/nested_box.xp")"           "deep=5"
# Parameter-passing semantics per the book (ch.7.5 / 12.1): plain = copy,
# ref = const&, mut_ref = &. Collection method dispatch must work on params.
check_eq "Vec param by value = copy"          "$(run "$DIR/generics/vec_by_value.xp")"         "n=0"
check_eq "Vec param mut_ref mutates caller"   "$(run "$DIR/generics/vec_by_ref.xp")"           "n=2"

section "Enums: Option<T> / Result<T,E> as ordinary generic enums"
# Option and Result are plain user-declared generic enums (no compiler
# special-casing); these guard the generic-enum machinery they rely on.
check_eq "Option<int> Some/None"            "$(run "$DIR/enums/option_basic.xp")"          "some=42
none"
check_eq "Result<int,str> Ok/Err"           "$(run "$DIR/enums/result_basic.xp")"          "ok=5
err=div by zero"
check_eq "Option<int>+Option<str>+Result"   "$(run "$DIR/enums/multi_instantiation.xp")"   "int=7
str=ann
err=bad"
check_eq "Result<Option<int>,str> (nested)" "$(run "$DIR/enums/result_of_option.xp")"      "found=50"
check_eq "Option<Vec<int>>"                 "$(run "$DIR/enums/option_of_vec.xp")"         "len=2"
# Pre-existing sema bug (not generics): a probe arm's payload binding was
# invisible to the analyzer whenever used as a call argument / inner subject.
check_eq "probe binding usable as argument" "$(run "$DIR/enums/probe_binding_as_arg.xp")"  "n=3
circle=3"
check_eq "probe binding as inner subject"   "$(run "$DIR/enums/probe_binding_as_subject.xp")" "circle=4"

section "Enums (continued): nesting, payload types, arity, arguments"
check_eq "Result<Option<Option<int>>,str> (3 deep)" "$(run "$DIR/enums/three_deep_nesting.xp")" "k=-1 err=neg
k=0 outer-none
k=1 inner-none
k=7 deep=700"
check_eq "int/float/bool/str payloads in one enum"  "$(run "$DIR/enums/mixed_payload_types.xp")" "int 42
float 2.5
bool true
str hi
nothing"
check_eq "multi-field variants Point(x,y)"          "$(run "$DIR/enums/multi_field_variants.xp")" "point 3,4
named age=30
quit"
check_eq "enum passed as a function argument"       "$(run "$DIR/enums/enum_as_argument.xp")" "green"

section "Collections and value semantics (backend parity)"
check_eq "Vec push/len/get"                         "$(run "$DIR/generics/vec_ops_toplevel.xp")" "len=3
first=10
last=30"
check_eq "Vec by-value param is a real copy"        "$(run "$DIR/generics/vec_copy_semantics.xp")" "inside=3
outside=1"
check_eq "generic pulse returning str (unannotated)" "$(run "$DIR/generics/generic_fn_returns_str.xp")" "s=hello
n=41"
check_eq "bool interpolation"                       "$(run "$DIR/generics/bool_interpolation.xp")" "b=true"

section "? error propagation (Result / Option are ordinary generic enums)"
check_eq "Result: ok and both early-return points"      "$(run "$DIR/propagate/basic_result.xp")"        "ok=3
err=bad input
err=bad input"
check_eq "Option: Some/None through a chain"            "$(run "$DIR/propagate/option_chain.xp")"        "d1=some 20
d9=none
s12=some 30
s19=none
s91=none"
check_eq "operands evaluate left to right, stop at first failure" "$(run "$DIR/propagate/evaluation_order.xp")" "eval A
eval B
eval C
=> ok 111
eval A
eval B
=> err B failed
eval A
=> err A failed"
check_eq "short-circuit: flag && f()? skips f() when flag false" "$(run "$DIR/propagate/short_circuit.xp")" "=> ok false
parse 5
=> ok true
parse x
=> err bad x"
check_eq "? inside a loop leaves the loop and the pulse"  "$(run "$DIR/propagate/loop_early_exit.xp")"    "i=0 total=0
i=1 total=1
i=2 total=3
=> ok 3
=> err 1 is odd"
check_eq "three-level propagation with str payloads"     "$(run "$DIR/propagate/three_level_chain.xp")"  "ok <[alpha+beta]>
err no such file: zzz
err no such file: nope"
check_eq "? on a variable, not just a call"              "$(run "$DIR/propagate/variable_operand.xp")"   "ok 8
err cannot parse q"
check_eq "enum as the error type"                        "$(run "$DIR/propagate/enum_error_type.xp")"    "k=1 ok 200
k=2 err denied
k=3 err not-found"
check_eq "Vec payload: shadow xs = load(n)?; xs.push"     "$(run "$DIR/propagate/vec_payload.xp")"        "len 4
err negative"
check_eq "? inside a generic pulse"                      "$(run "$DIR/propagate/generic_pulse.xp")"      "ok 42
err nope"
check_eq "? inside an impl method" "$(run "$DIR/propagate/impl_method.xp")" "left 29
err insufficient funds"

section "? diagnostics (rejected by the analyzer, identical on every backend)"
check_eq "XP2040 ? outside any pulse"                    "$(err_code "$DIR/propagate/neg_toplevel.xp")"            "XP2040"
check_eq "XP2041 pulse returns int"                      "$(err_code "$DIR/propagate/neg_returns_int.xp")"         "XP2041"
check_eq "XP2041 pulse returns nothing"                  "$(err_code "$DIR/propagate/neg_returns_void.xp")"        "XP2041"
check_eq "XP2042 operand is not a Result/Option"         "$(err_code "$DIR/propagate/neg_operand_int.xp")"         "XP2042"
check_eq "XP2043 Option used in a Result pulse"          "$(err_code "$DIR/propagate/neg_option_in_result.xp")"    "XP2043"
check_eq "XP2044 error types differ"                     "$(err_code "$DIR/propagate/neg_error_type_mismatch.xp")" "XP2044"
check_eq "XP2045 ? inside an f-string interpolation"     "$(err_code "$DIR/propagate/neg_in_fstring.xp")"          "XP2045"

section "Backend parity (bugs found while building ?)"
check_eq "string == compares content, not addresses"     "$(run "$DIR/parity/string_equality.xp")"       "eq1=1 eq2=0"
check_eq "&& / || do not evaluate the right side needlessly" "$(run "$DIR/parity/short_circuit_ops.xp")" "a=false b=true"
check_eq "unannotated bool atoms print true/false"       "$(run "$DIR/parity/bool_atoms.xp")"            "lit=true cmp=true lg=false neg=false call=true ann=true"
check_eq "probe on a generic-enum parameter; ctor as argument" "$(run "$DIR/parity/param_probe_and_arg_context.xp")" "ok=5"
check_eq "impl method call" "$(run "$DIR/parity/method_call.xp")" "left 30"

section "stdlib: core (Option / Result written in XPhage itself) and the panic primitive"
check_eq "core: option_*/result_* helpers"            "$(run "$DIR/stdlib/core_basic.xp")"      "some1=true none9=true
or1=10 or9=-1
unwrap2=20
ok=true err=true
uo=7 uo2=99"
check_eq "core + ? + conversions (ok_or, to_option)"  "$(run "$DIR/stdlib/core_interplay.xp")"  "d1=14 d2=-1
c=97 d=0
err_some=true ok_some=true"
check_eq "option_unwrap(None): stdout stops at the panic" "$(run_stdout "$DIR/stdlib/core_unwrap_none.xp")" "start"
check_eq "option_unwrap(None): panic message on stderr"   "$(run_stderr "$DIR/stdlib/core_unwrap_none.xp")" "panic: option_unwrap called on None"
check_eq "result_expect(Err, msg): panic uses your message" "$(run_stderr "$DIR/stdlib/core_expect_err.xp")" "panic: could not load config"
check_eq "panic(msg): nothing after it runs"          "$(run_stdout "$DIR/stdlib/panic_primitive.xp")" "before"
check_eq "core: option_some/option_expect/result_unwrap + typed-atom context" "$(run "$DIR/stdlib/core_remaining.xp")" "a=41 u=8 b=41"
check_eq "XPhage-source Vec algorithms (index loop, str ==)" "$(run "$DIR/stdlib/vec_algorithm_in_xphage.xp")" "total=12
count=2"

section "nexus: contracts, implicit self, default methods, Self, multiple bounds"
# Language book Chapter 8. A nexus is a compile-time contract: it is checked by
# the analyzer and every call is statically dispatched on the concrete type.
check_eq "book 8.2: impl-for, implicit self, bounded dispatch" "$(run "$DIR/nexus/book_animal.xp")" "Woof
Rex says Woof on 4 legs
Tom says Meow on 4 legs"
check_eq "book 8.4: default methods (inherited, overridden, via a bound)" "$(run "$DIR/nexus/default_methods.xp")" "> direct
> [INFO] default
> [WARN] w
q:[INFO] i
overridden:w2
> [INFO] via generic"
check_eq "Self in nexus signatures and impl bodies (spawn Self)" "$(run "$DIR/nexus/self_type.xp")" "true
false
2"
check_eq "multiple bounds: T: Named + Aged" "$(run "$DIR/nexus/multi_bound.xp")" "Ada (36)"
check_eq "nexus declared in a realm, default method through it" "$(run "$DIR/nexus/realm_nexus.xp")" "roar!"
check_eq "generic impl<T>: bare-form methods, -> Self, spawn Self" "$(run "$DIR/nexus/generic_impl_self.xp")" "5
9"
check_eq "tri-modular: nexus + impl in .xh, caller in .xp0" "$(run "$DIR/nexus/shapes.xh" "$DIR/nexus/shapes.xp0")" "rect: 12"

section "associated functions: Type::f(), T::f(), explicit type arguments"
# A `pulse` member without `self` has no receiver: an associated function. After
# monomorphization they are hoisted to free functions, so no backend special-cases them.
check_eq "inherent associated fns, Self::new() inside an impl" "$(run "$DIR/nexus/assoc_inherent.xp")" "7
0
2"
check_eq "nexus associated fns: T::default(), static default via Self::, make<T>()" "$(run "$DIR/nexus/assoc_default_trait.xp")" "100
3
true
false"
check_eq "associated fn of a realm member, called from inside and outside" "$(run "$DIR/nexus/assoc_realm.xp")" "1
11"
check_eq "explicit type arguments f<A, B>(...); a < b still compares" "$(run "$DIR/nexus/explicit_type_args.xp")" "7
s
0
true"

section "nexus implemented for primitives (impl Ord for int, impl Default for str, ...)"
# A method of a primitive receiver becomes a free function `int__cmp(self, other)`;
# a call on a receiver whose type is known (a bounded `a: T` after monomorphization,
# a typed local, a field) is rewritten to it.
check_eq "Ord + Default for int and str through generic bounds, default method lt()" "$(run "$DIR/nexus/prim_impls.xp")" "9
pear
true
false
0
[]"
check_eq "string concat, ordering and equality; float arithmetic slot" "$(run "$DIR/nexus/prim_strings.xp")" "pear-x
false
true
true
false
true
true
3"

section "sized numeric types (i8..u64, isize/usize, f32; aliases int=i64 float=f64 byte=u8)"
# Language extension, rules in compiler/xphage_sema/include/numeric_types.hpp. Every
# expected value below was worked out by hand; all three backends must agree.
check_eq "wrap-around arithmetic, shifts, saturating/extending casts, f32, printing of every width" "$(run "$DIR/numeric/wrap_arith.xp")" "44
206
32
-128
-2147483648
4294967295
18446744073709551615
0
65534
28
14
15
-128
44
4294967295
3
-3
2147483647
0
-56
18446744073709551615
0.1
0.3
200 127 0.1 44
true
true"
check_eq "sized struct fields/params/returns, typed range loops, u64 hash, compound and bitwise ops" "$(run "$DIR/numeric/structs_loops_bits.xp")" "4 20 9
1395
4932904490461320209
-25536
-6384
1
191
78
78"
check_eq "Vec<u8> and Vec<f32> element types" "$(run "$DIR/numeric/collections_f32.xp")" "432
255
3.75"
check_eq "impl Ord for u8/i32 through a generic bound; i64/f64/byte aliases" "$(run "$DIR/numeric/generic_impls.xp")" "200
-5
7
10
3"
check_eq "extern "C" with exact-width i32 / usize signatures" "$(run "$DIR/numeric/ffi_widths.xp")" "65
true
5"
check_eq "generic T: Numeric arithmetic wraps per concrete type after monomorphization" "$(run "$DIR/numeric/generic_numeric.xp")" "44
3.75
7
3.5
-2
3705032704"
check_eq "literal forms: separators, hex, binary, suffixes, exponent" "$(run "$DIR/numeric/literals.xp")" "1255
170
-128
1000
5
4294967295"
check_eq "%= &= |= ^= <<= >>= on int and on sized types" "$(run "$DIR/numeric/compound_ops.xp")" "16
4"

section "numeric type diagnostics"
check_eq "XP2051 literal does not fit its type" "$(err_code "$DIR/numeric/neg_literal_range.xp")" "XP2051"
check_eq "XP2050 operands of different sized types" "$(err_code "$DIR/numeric/neg_mixed_types.xp")" "XP2050"
check_eq "XP2052 unary minus on an unsigned type" "$(err_code "$DIR/numeric/neg_unsigned_negate.xp")" "XP2052"
check_eq "XP2050 argument type (int into u8)" "$(err_code "$DIR/numeric/neg_arg_type.xp")" "XP2050"
check_eq "XP2050 float literal where an integer type is declared" "$(err_code "$DIR/numeric/neg_float_to_int.xp")" "XP2050"
check_eq "XP2050 no implicit widening u8 -> u16" "$(err_code "$DIR/numeric/neg_widen_implicit.xp")" "XP2050"
check_eq "XP2050 struct field initializer type" "$(err_code "$DIR/numeric/neg_field_type.xp")" "XP2050"
check_eq "XP2054 integer suffix on a float literal" "$(err_code "$DIR/numeric/neg_float_suffix.xp")" "XP2054"
check_eq "XP2053 shift of a float" "$(err_code "$DIR/numeric/neg_shift_float.xp")" "XP2053"

section "nexus diagnostics (rejected by the analyzer, identical on every backend)"
check_eq "XP2015 impl is missing a required method"        "$(err_code "$DIR/nexus/neg_missing_method.xp")"          "XP2015"
check_eq "XP2016 impl of an unknown nexus"                 "$(err_code "$DIR/nexus/neg_unknown_nexus_impl.xp")"      "XP2016"
check_eq "XP2016 bound names an unknown nexus"             "$(err_code "$DIR/nexus/neg_unknown_bound.xp")"           "XP2016"
check_eq "XP2017 method arity differs from the nexus"      "$(err_code "$DIR/nexus/neg_arity.xp")"                   "XP2017"
check_eq "XP2017 method return type differs"               "$(err_code "$DIR/nexus/neg_return_type.xp")"             "XP2017"
check_eq "XP2018 method is not a member of the nexus"      "$(err_code "$DIR/nexus/neg_extra_method.xp")"            "XP2018"
check_eq "XP2014 a nexus used as a parameter type"         "$(err_code "$DIR/nexus/neg_nexus_as_type.xp")"           "XP2014"
check_eq "XP2012 second bound of T: A + B not satisfied"   "$(err_code "$DIR/nexus/neg_bound_unsatisfied_multi.xp")" "XP2012"
check_eq "XP2012 a primitive without the impl does not satisfy the bound"  "$(err_code "$DIR/nexus/neg_prim_missing.xp")" "XP2012"
check_eq "XP2019 associated function does not exist on the type"  "$(err_code "$DIR/nexus/neg_assoc_unknown.xp")"   "XP2019"
check_eq "XP2019 T::f() but no bound declares f"                  "$(err_code "$DIR/nexus/neg_assoc_unbounded.xp")" "XP2019"
check_eq "XP2017 nexus says associated fn, impl wrote a method"   "$(err_code "$DIR/nexus/neg_assoc_vs_method.xp")" "XP2017"

echo "══════════════════════════════════════════════"
printf "  ${G}PASS${X}: %-3d   ${R}FAIL${X}: %-3d   Total: %d\n" "$PASS" "$FAIL" "$((PASS+FAIL))"
echo "══════════════════════════════════════════════"
if [ "$FAIL" -eq 0 ]; then
    echo -e "  ${G}ALL GOLDEN TESTS PASSED ✓${X}"
    echo -e "  Compiler handles all combinations identically."
    exit 0
else
    echo -e "  ${R}FAILURES: $FAIL ✗${X}"
    exit 1
fi
