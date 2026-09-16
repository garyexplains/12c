# Software binary64 on AArch64 Linux

Binary64 arithmetic uses the existing 12 mnemonics. Internal values are bit
patterns in general-purpose registers and eight-byte stack slots. `LDR Dn` and
`STR Dn` transfer values at Linux ABI boundaries. There are no `FMUL`, `FDIV`,
hardware conversion instructions, or system-compiled arithmetic helpers.

The supported operations are implemented and unchanged `examples/dhry.c` now
runs on native AArch64 Linux. The earlier integer-only fixture remains a separate
test; only that fixture delegates reporting to host C. macOS double support is
rejected until its ABI implementation and native validation are complete.

## Representation and emitted routines

- The `double` type is eight bytes and eight-aligned. Internally, expression results
  hold binary64 bits in `x0`; loads, stores and temporary spills preserve bits.
- Decimal literals are checked separately from integer tokens. Host `strtod`
  supplies compile-time binary64 bits in the process's initial C locale and
  default rounding mode (12c never changes either). The build requires a
  binary64 host double and checks its representation; literal overflow is an
  error. Subnormal literals are supported; suffixes and hex floats are rejected.
- [src/softfloat.h](src/softfloat.h) embeds ordinary C within 12c's supported
  integer subset. When conversions or arithmetic need it, 12c lexes and compiles
  this bundle after the user's functions. It is never compiled by the host C
  compiler. Symbols have private `.L__12c_` labels, and `__12c_` is reserved from
  user declarations. The primitive unsigned right shift by one is emitted as
  63 `TBZ` tests and shifted `ADD` operations; C shift operators are not added.
- Internal conversion entries accept an integer in `x0` and return binary64 bits
  in `x0`. The binary entry receives bits in `x0`/`x1` and operation selector
  0 (multiply) or 1 (divide) in `w2`, returning bits in `x0`. Ordinary frames
  preserve `x29`/`x30`; other caller-saved registers may be clobbered.
- Signed/unsigned 32/64-bit integer conversions normalize an unsigned magnitude,
  retaining guard/round/sticky information. The minimum signed integer does not
  overflow a signed negation. Rounding is nearest, ties to even, including
  significand carry into the exponent.
- Multiplication accumulates the complete 106-bit product using two 64-bit
  limbs, bit tests and additions. Division normalizes its ratio and performs
  56 restoring-division steps. Packing normalizes the significand and rounds
  once, with sticky shifts for gradual underflow.
- Signed zeros, subnormals, infinity, overflow, underflow and division by zero
  are handled. NaN inputs and invalid arithmetic return a canonical quiet NaN;
  NaN payloads and exception flags are not preserved. Floating-environment
  exceptions and dynamic rounding modes are outside the supported subset.
- Storage, unary signs, truth tests and `_Bool` conversion are supported.
  Double addition/subtraction, comparisons, increment, double-to-integer casts,
  and `float` are rejected rather than approximated.

## ABI boundary

On AArch64 Linux, general-purpose and floating-point argument registers are
allocated independently. For example, `printf("%d %.1f", 7, 1.5)` uses
`x0` for the format, `w1` for the integer and `d0` for the double. Expressions
are staged before marshaling registers so nested calls cannot destroy earlier
arguments. Double results use `d0`, with memory spill/reload to transfer between
`d0` and internal `x0` bits. The existing eight-total-arguments limit remains.

The rules come from the [Arm procedure-call standard](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst).
Apple's unnamed variadic arguments instead use stack slots, so Linux register
lowering must not be reused for macOS. See [Apple's arm64 ABI documentation](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms).

A native Linux assembly probe verified bit-preserving `STR d0` / `LDR x0` /
`STR x0` / `LDR d0` transport for zeros, a subnormal, the minimum normal, 1.5,
2^53, the largest finite value, infinity and a quiet NaN. A mixed variadic call
printed `7 1.5`. Disassembly used only allowed mnemonics. This validates transport
and argument placement, not software arithmetic or compiler lowering.

## Validation

The native tests compare against [tests/softfloat_reference.c](tests/softfloat_reference.c),
a system-compiled oracle which does not supply arithmetic to generated code:

- 625 pairs of boundary/special values and 2,048 deterministic random pairs for
  multiplication and division. Finite results and signed zeros must match bit
  for bit; NaN results are compared by classification.
- Signed/unsigned conversion extrema, rounding boundaries around 2^53, 32-bit
  extrema and deterministic random 64-bit values.
- Decimal literals, subnormals, global/array/local storage, signed zero truth
  tests, eight double parameters, nested calls, mixed variadic formatting and
  calls in both directions across the system ABI.
- Unchanged Dhrystone with the real clock, and with a controlled clock for exact
  timing-output comparison and retry accounting. The controlled-clock test also
  checks default, zero, negative and malformed run-count arguments.
- Source and object instruction audits covering user functions and all emitted
  software routines; the same tests run with an ASan/UBSan compiler build.

The wider suite still exposes deferred record compatibility, union layout,
boolean global initialization, narrow switch and external-definition bugs.
These do not block the verified Dhrystone path. Apple Silicon validation remains
outstanding.
