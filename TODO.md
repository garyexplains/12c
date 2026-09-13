# TODO: compile and run Dhrystone with 4c

## Goal and completion criteria

Compile the supplied modernised [Dhrystone source](examples/dhry.c) with 4c,
assemble and link against the target C library, and run it with correct final
values and working timing/reporting. The benchmark source should not need to be
rewritten to avoid unsupported language features.

The system compiler may assemble/link 4c output. An explicit system-preprocessor
step is a possible implementation choice, but the benchmark's C code must be
compiled by 4c. Start with native AArch64 Linux; validate macOS separately.

Preserve the project's 12-mnemonic restriction throughout. Successful parsing or
assembly emission alone does not complete this goal.

## Current status

The first milestone is implemented in [src/4c.c](src/4c.c):

- [x] File-scope and block-scope typedef aliases for supported types, including
  fixed-size arrays and void, with scope and name-conflict checks.
- [x] Void function declarations/definitions, calls, bare return, and fallthrough;
  reject void expressions where a value is required.
- [x] Global int, char, and pointer variables, with zero initialization or simple
  literal initializers, matching tentative declarations, and address/load/store
  emission for Linux and macOS.
- [x] An executable example combining these features: [globals.c](examples/globals.c).
- [x] Regression coverage: the latest implementation run passed all 36 tests,
  including native AArch64 Linux execution, calls across the system C ABI, and an
  actual machine-instruction whitelist audit. macOS has assembly-text checks;
  native assembly/link/execution validation is still outstanding.
- [x] Enum definitions, enumerator constants, enum variables/parameters, and
  conversions needed by Enumeration and Ident_1 through Ident_5. Enumerator
  initializers accept sequential values, a decimal literal, another enumerator
  constant, and an optional sign; enum tags have block scoping.
- [x] _Bool storage, integer promotion, and conversion that normalizes any nonzero
  scalar value to 1, including bool parameters and results across the native ABI.
- [x] Multiple declarators sharing declaration specifiers, including different
  pointer/array suffixes per declarator, for locals, globals, and typedefs.
- [x] Target-correct long and unsigned integer support for long run counts,
  size_t, and clock_t: 64-bit arithmetic, usual arithmetic conversions,
  signed/unsigned comparisons at both widths, integer suffixes such as 50000L,
  and .quad globals.
- [x] Cast parsing and conversions for `(int) Run_Index` and
  `(long) CLOCKS_PER_SEC` at scalar types and pointers; void-pointer casts and
  double casts are still future milestones.
- [x] Regression coverage: the latest implementation run passed all 45 tests,
  adding enum, _Bool, multi-declarator, long/unsigned, and cast checks with
  native execution and cross-ABI helpers.
- [x] Global arrays with static zero initialization, including a 50 by 50 int
  (10,000-byte) array that does not inherit the 4,080-byte local-frame limit.
- [x] Multidimensional array declarators and typedefs, row-major indexing, and
  array-parameter adjustment to row pointers with row-scaled pointer arithmetic.
- [x] Struct/union definitions, tag lookup with block scoping, incomplete tagged
  types, and pointers to self-referential records.
- [x] Target-correct aggregate size, alignment, member offsets, and padding,
  including nested anonymous unions and structs; layout matches system C.
- [x] Member access with `.` and `->`, preserving lvalues so member addresses
  can be passed to procedures.
- [x] sizeof(type), returning size_t without runtime evaluation.
- [x] Structure assignment, including arrays and unions within a record and
  exact self-assignment, copied byte by byte.
- [x] Regression coverage: the latest implementation run passed all 50 tests,
  adding struct/union layout, member operations, multidimensional arrays,
  sizeof, and zero-initialized global aggregate checks.
- [x] Integer multiplication and division, including long operations used by
  the driver, with C precedence, signed truncation toward zero, and
  two's-complement wrapping; synthesized from ADD/SUB/CBZ/TBZ only.
- [x] Logical negation, short-circuit && and ||, and bitwise OR; Proc_4's
  `Bool_Loc | Bool_Glob` lowers as a bitwise operation.
- [x] += and -=, and prefix ++, for scalar and indexed lvalues, evaluating
  the destination address once and applying the destination type's
  conversion.
- [x] for loops and do/while loops, with break targeting the nearest
  enclosing loop or switch.
- [x] switch, case labels with fallthrough, default, and break, exercised by
  Proc_6's enum dispatch.
- [x] Regression coverage: the latest implementation run passed all 55 tests,
  adding multiplication/division edges, logical short-circuit side-effect
  checks, bitwise OR, compound assignment/increment targets, and
  for/do/switch control-flow checks.

Other existing foundations include int/char/pointer parameters and results,
recursion, local one-dimensional arrays, strings, pointer indexing, assignment,
addition/subtraction, comparisons, if/else, and while. See [README.md](README.md)
for the exact subset and limits.

Dhrystone does **not** compile yet. The last direct attempt stopped at line 51,
`#include <stdbool.h>`, because the built-in include reader only accepts
`<stdio.h>`. That controlled header currently declares only putchar. Fixing this
first diagnostic will expose the language and ABI requirements below.

## Ordered implementation milestones

### 1. Finish basic declarations and integer types

Implemented; see the checked items in Current status above. Dhrystone still
needs the remaining milestones below before it can compile.

- [x] Enum definitions, enumerator constants, enum variables/parameters, and
  conversions needed by Enumeration and Ident_1 through Ident_5.
- [x] _Bool storage, integer promotion, and conversion that normalizes any nonzero
  scalar value to 1. Supply bool/true/false through stdbool.h when preprocessing
  is available.
- [x] Multiple declarators sharing declaration specifiers, including different
  pointer/array suffixes. Dhrystone uses both `char Ch_1_Glob, Ch_2_Glob;` and
  `typedef struct record { ... } Rec_Type, *Rec_Pointer;`.
- [x] Target-correct long and unsigned integer support needed by long run counts,
  size_t, and clock_t; integer suffixes such as 50000L; arithmetic conversions
  and comparisons at the correct width.
- [x] Cast parsing and conversions for `(int) Run_Index`, `(long) CLOCKS_PER_SEC`,
  and, in later milestones, void pointers and double.

Validate typedef composition, declaration scope, enum values, boolean
normalization, and integer width/conversion boundaries with small programs.

### 2. Add aggregate types and storage

Implemented; see the checked items in Current status above. Aggregate
arguments/results by value remain outside this milestone as planned.

- [x] Global arrays with static zero initialization. Arr_2_Glob is 50 by 50 ints
  (10,000 bytes); it does not inherit the 4,080-byte local-frame limit.
- [x] Multidimensional array declarators and typedefs, row-major indexing, and
  array-parameter adjustment. Arr_2_Dim parameters decay to pointers to rows,
  not pointers to int. Row pointer arithmetic scales by 200 bytes.
- [x] Struct/union definitions, tag lookup, incomplete tagged types, and pointers
  to self-referential records.
- [x] Target-correct aggregate size, alignment, member offsets, and padding,
  including the nested union and structs in Rec_Type.
- [x] Member access with `.` and `->`, preserving lvalues and allowing member
  addresses to be passed to procedures.
- [x] sizeof(type), returning the appropriate size type without runtime evaluation,
  for malloc(sizeof(Rec_Type)).
- [x] Structure assignment, including arrays and the union within a record:
  `*Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;`. Handle self-assignment correctly.

Validate layout against system-compiled C, nested member writes, row indexing,
and record copies. Aggregate arguments/results by value are not needed here.

### 3. Complete the benchmark's expressions and control flow

Implemented; see the checked items in Current status above.

- [x] Integer multiplication and division, including long operations used by the
  driver. Preserve precedence, signed behavior, and intermediate values across calls.
- [x] Logical negation, short-circuit && and ||, and bitwise OR. Proc_4 uses
  `Bool_Loc | Bool_Glob`; that is a bitwise operation, not logical OR.
- [x] += and -=, and prefix ++, for scalar and indexed lvalues. Evaluate the
  destination address once and apply the destination type's conversion.
- [x] for loops and do/while loops.
- [x] switch, case labels, and break, with the correct enclosing break target
  and fallthrough behavior. Proc_6 exercises enum dispatch.

Synthesize arithmetic using the allowed instructions; do not emit MUL or SDIV
silently. Keep TBZ targets in range and revisit function/literal-pool size
limits if synthesized operations make generated functions too large.

### 4. Supply preprocessing, headers, and the integer library ABI

This work can begin earlier to expose more of the original source to the parser.

- [ ] Choose and document either a limited built-in preprocessor or an explicit
  system-preprocessor path with controlled target headers. Do not assume host
  system headers are parseable by this compiler or correct for a different target.
- [ ] Support the source's object-like macros and zero-argument function-like
  macros, including nested expansion in Start_Timer(), Stop_Timer(), and
  Too_Small_Time. Handle comments on directives and header guards; preserve
  useful source diagnostics or consume preprocessor line markers.
- [ ] Add controlled stdbool.h, stdlib.h, string.h, and time.h; expand stdio.h.
  Provide NULL, EXIT_FAILURE, CLOCKS_PER_SEC, size_t, clock_t, and the necessary
  declarations using the target's actual ABI.
- [ ] Support void pointers and compatible object-pointer conversions for malloc,
  free, NULL, and the `%p` casts; support const-qualified library parameters.
- [ ] Support main(int argc, char *argv[]) and normal argument access.
- [ ] Declare and correctly call malloc, free, strtol, strcpy, strcmp, and clock.
- [ ] Support external object declarations and target-correct addressing for
  stderr, with an opaque FILE type. Account for platform header/symbol differences
  rather than assuming stderr is exported identically on Linux and macOS.
- [ ] Variadic prototypes and calls to printf/fprintf, including default argument
  promotions and platform-specific argument placement. Linux and macOS variadic
  lowering must be validated separately.

Library implementations can come from libc; implementing these library functions
inside 4c is unnecessary. First validate integer/string/pointer reporting and
64-bit clock/run-count handling without double formatting.

### 5. Implement floating-point timing and reporting

The benchmark core is integer work, but the supplied main uses double.

- [ ] Double types, literals such as 1000000.0, local storage, and assignment.
- [ ] Integer-to-double conversions and double multiplication/division for
  Microseconds and Dhrystones_Per_Second.
- [ ] Double arguments in variadic printf calls, including target ABI register
  and stack rules and preservation across calls.
- [ ] Decide and document how floating-point computation fits the instruction
  restriction. Ordinary FMUL/FDIV/conversion instructions are outside the current
  whitelist. A software implementation or an explicitly agreed runtime-helper
  policy is needed; do not silently relax the budget or assume helpers exist.

Validate conversions and arithmetic independently, then compare timing formulas
and formatted output with a system-compiled reference. Timing numbers themselves
will differ between compiler implementations and runs.

## Integration and correctness checks

- [ ] Keep small positive, negative, and native ABI tests for each milestone,
  alongside the existing regression suite and instruction audit.
- [ ] Establish a deterministic fixed-iteration check of the integer core. A
  temporary system-compiled timing/reporting harness can help during development,
  but it is an intermediate test, not completion of the unchanged-source goal.
- [ ] Compile the original examples/dhry.c end to end, link, and run. Check its
  argument handling, normal output, and final values on stderr.
- [ ] Check final scalar values: Int_Glob=5, Bool_Glob=1, Ch_1_Glob='A',
  Ch_2_Glob='B', Arr_1_Glob[8]=7; locals Int_1_Loc=5, Int_2_Loc=13,
  Int_3_Loc=7, and Enum_Loc=Ident_2 (1).
- [ ] Check record values and strings against the source's expected report:
  Ptr_Glob has Discr=0, Enum_Comp=2, Int_Comp=17; Next_Ptr_Glob has Discr=0,
  Enum_Comp=1, Int_Comp=18. Compare pointer relationships rather than literal
  addresses, which vary between processes.
- [ ] Account for adaptive retries when checking Arr_2_Glob[8][7]. This source
  initializes it to 10 only once, so it becomes 10 plus iterations across **all**
  timed attempts. Its printed expectation of Number_Of_Runs + 10 is only accurate
  when there were no earlier attempts. Use a fixed-iteration harness for an exact
  deterministic check, or track all attempts in the end-to-end test.
- [ ] Audit the final generated benchmark's machine instructions, relocation and
  branch ranges, and ABI behavior. Then repeat native validation on Apple Silicon.
- [ ] Document reproducible build/run commands and distinguish correctness from
  performance comparisons. The supplied source is one translation unit; preserve
  its existing caveat about inlining and historical Dhrystone comparison rules.

## Outside the required scope

A complete C implementation is not required for this file. Function pointers,
variadic function definitions, aggregate arguments/results by value, VLAs, and
more than eight fixed parameters can remain separate work unless a chosen header
or runtime strategy introduces a concrete dependency. Rewriting away records,
arrays, or procedures would weaken the benchmark and does not satisfy this goal.
