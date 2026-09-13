# A tiny C compiler for AArch64

This project will compile a subset of C into native AArch64 code using just
**12 instruction mnemonics**. Generated programs will use the system assembler,
linker, and C runtime library.

The goal is a small, understandable compiler with a deliberately restricted
instruction vocabulary. Generated programs may be inefficient. The restriction
should still allow general computation, ordinary functions, recursion, pointers,
and clean calls into the platform's C library.

Phase one is implemented in C. It compiles the example below into native assembly
and uses our own minimal `include/stdio.h` for the `putchar` declaration. The rest
of this README describes both the current implementation and the broader design.

## Build and run phase one

Requirements: a C11 host compiler and Make. Tests also require Python 3. Linking
and running the result requires an AArch64 system toolchain and C runtime.

```sh
make
./build/4c examples/hello.c -o build/hello.s
cc build/hello.s -o build/hello
./build/hello
make test
```

The example prints `A` without a newline and exits with status zero:

```c
#include <stdio.h>

int main() {
  int a = 65;
  putchar(a);
  return 0;
}
```

The compiler emits assembly; `cc` in the second step only assembles and links it.
Generated assembly includes `// C line N: ...` comments before the function
prologue and each statement's instructions. Multiline statements are flattened
into one comment; separate statements on the same source line get separate
comments. Compiler-generated fallthrough and epilogue code are labeled as well.
On a Linux host, the default target is Linux. On a Mac, it is macOS. Select a
target explicitly with `--target linux` or `--target macos`:

```sh
./build/4c --target macos examples/hello.c -o build/hello-macos.s
```

Cross-target assembly emission does not supply a cross-linker or target SDK.
Linux AArch64 execution and object-code instruction audits have been tested.
The macOS emitter has automated text checks but still needs assembly, linking,
and execution validation on an Apple Silicon Mac.

`-o` is optional; without it, assembly goes to standard output. `-I directory`
selects the directory containing our `stdio.h`. The Makefile embeds this checkout's
absolute `include` path by default, so the compiler can be invoked from another
working directory. Rebuild or supply `-I` if the checkout is moved.

### Currently supported language

- One `int main()` or `int main(void)` definition, without parameters.
- Types: 32-bit `int` and `unsigned`, 64-bit `long` and `unsigned long`, 8-bit
  `char`, 8-bit `_Bool`, and 64-bit pointers, including pointers to pointers.
  Plain `char` is unsigned on AArch64 Linux and signed on macOS, following
  each target's default ABI. Character expressions promote to `int`. `_Bool`
  storage normalizes every nonzero scalar value to 1 and zero to 0.
- Enum types: `enum` tags with block scoping and same-scope redefinition
  checks. Enumerator constants join the ordinary identifier namespace with
  sequential values or explicit initializers (a decimal literal, another
  enumerator constant, and an optional sign; no arithmetic yet). Enum
  variables, parameters, and results behave as `int`. Anonymous enums define
  constants without a tag; named tags are reusable as `enum Tag`.
- File-scope and block-scope `typedef` aliases for supported types, including
  fixed-size arrays and `void`. Aliases share the ordinary identifier namespace
  and obey block shadowing. Comma-separated declarators share one declaration's
  specifiers, each with its own `*`/array suffixes; a declarator is visible to
  later declarators and initializers on the same declaration.
- Global scalar variables (`int`, `char`, `_Bool`, `unsigned`, `long`,
  `unsigned long`, and pointers), visible after their declaration, with
  external linkage and zero initialization by default. Integer and character
  initializers accept a decimal literal with an optional unary sign and, for
  the wider types, `L`/`U` suffixes; pointer initializers accept zero. Matching
  tentative declarations may repeat, with at most one initialized definition.
  Up to 256 file-scope objects, typedef names, and enumerator constants
  combined are supported. Global arrays, `extern`, `static`, address
  initializers, and general constant expressions remain unsupported.
- User-defined functions returning `void`, `_Bool`, `char`, `int`,
  `unsigned`, `long`, `unsigned long`, or a pointer, with zero to eight
  parameters of those types,
  including recursion and mutual recursion through forward prototypes. Functions
  must be declared or defined before a call; a definition declares the function
  before its own body is parsed. Up to 256 distinct function names are supported.
- Scalar locals and one-dimensional fixed-size arrays in nested blocks, up to 256 simultaneously active
  locals including parameters and block-scope typedef names. Inner blocks may shadow outer names; sibling
  blocks reuse stack slots. Parameters can be assigned like local variables.
  Scalar initializers are optional; reading an uninitialized value remains
  undefined behavior. Local storage, including saved frame/link registers,
  is limited to 4080 bytes per function.
- String and single-byte character literals, with standard simple escapes,
  up to three octal digits, and byte-sized hex escapes. Adjacent string literals
  concatenate. String literals reside in read-only storage; use a local character
  array for mutable text.
- Address-of `&`, dereference `*`, assignment through pointers, and array indexing
  `a[i]`. Arrays decay to pointers in value contexts. Pointer addition/subtraction
  by an `int` scales by element size and handles negative indexes. Compatible
  pointers support `==`, `!=`, and truth tests; literal `0` is accepted as a null
  pointer constant (also parenthesized or with unary `+`/`-`).
- Explicit-size array declarations such as `int a[4]`, brace initializers such as
  `int a[4] = {1, 2}`, and string initializers such as `char s[] = "hello"` or
  `char s[8] = "hello"`. Omitted initializer elements are zero-filled. Array
  parameters such as `char s[]` are adjusted to pointers.
- Decimal literals with optional `L`/`l` and `U`/`u` suffixes (combined for
  `unsigned long`), local references, parentheses,
  and unary `+` and `-`. Unsuffixed values must fit `int`; `U` values must fit
  `unsigned int`; suffixed values must fit `long` or `unsigned long`.
- Casts to the scalar types and to pointers, such as `(long) x`, `(unsigned) n`,
  `(char) 300`, `(_Bool) p`, or `(int *) 0`. Cast declarators are limited to
  specifiers and `*` forms; casts to array or `void`, and named or array
  declarators inside casts, are rejected. Casts to `_Bool` normalize as above;
  pointer-to-integer casts remain unsupported.
- Binary `+` and `-`, with left associativity and unary operators taking
  precedence. Parentheses override precedence. Mixed-width operands follow the
  usual arithmetic conversions across `char`, `_Bool`, `int`, `unsigned`,
  `long`, and `unsigned long`, so 64-bit math stays at 64 bits.
- Signed and unsigned integer comparisons `==`, `!=`, `<`, `<=`, `>`, and `>=`,
  returning exactly `0` or `1` at the common operands' width and signedness.
  Arithmetic binds more tightly than ordering comparisons,
  which bind more tightly than equality comparisons; assignment binds last.
- Assignment to existing locals, including chained assignments (`a = b = 65`)
  and assignments used as expressions (`putchar(a = 65)`).
- `if`/`else`, including `else if`, and `while` loops. Conditions accept integer
  or pointer expressions: zero/null is false and other values are true. Bodies may be
  single statements or blocks; declarations require a block. An `else` belongs
  to the nearest unmatched `if`.
- Typed prototypes for external or user-defined functions. Use `(void)` for a zero-argument prototype;
  old-style unspecified-argument declarations such as `int f();` are rejected.
  Definitions may use either `f()` or `f(void)` for zero parameters. Parameter
  names are optional in prototypes and required in definitions.
- Calls with zero to eight scalar arguments, including nested calls and results used
  as initializers or return values.
- Expression statements, empty statements, and `return` with an expression.
  Void functions support bare `return;` and fallthrough; their calls can be
  expression statements, but cannot be used where a value is required.
  Falling off the end of `main` returns zero.
- Line comments, block comments, and `#include <stdio.h>`.

Our header currently declares only `int putchar(int c);`; libc supplies its
implementation. The compiler reads this header itself and does not load host
system headers. Repeated includes work because matching prototypes may repeat.
General preprocessing, macros, other includes, and trailing comments on include
directives are not supported yet.

Multiplication, division, other binary operators, compound assignments (`+=`),
increment/decrement, logical operators (`!`, `&&`, `||`), `break`, `continue`,
`for`, function pointers, variadic calls, more than eight parameters,
and floating point remain future work. Global arrays, `const`,
`void` objects/pointers, `sizeof`, multidimensional array declarations,
and parenthesized declarators are not supported yet. Neither are pointer ordering,
pointer-to-pointer subtraction, or general computed null pointer constants.
Scalar self-initializer reads, enumerator arithmetic in initializers,
and the literal expression `-2147483648` are also
outside this subset. Array lengths must be decimal literals; an omitted length
is supported only for a string initializer or array parameter.
Unsupported syntax is rejected rather than passed to the host compiler.

The emitter uses stack slots for locals, 32-bit integer operations, byte loads and
stores for characters and `_Bool`, 64-bit pointer and `long` operations, and a
separate 16-byte aligned frame with saved frame/link registers for each
function. Each
function has its own return epilogue and literal pool, with unique labels.
Arguments are evaluated left-to-right into aligned temporary stack slots, then
loaded into `w0` through `w7` (or `x0` through `x7` for 64-bit types) before the call. Callees save incoming
parameters in their frames before executing the body. Results are returned in
`w0` for 32-bit integers and `x0` for 64-bit types. Character and `_Bool`
arguments and results are narrowed and extended according to the target.
These conventions permit calls
between generated and system-compiled C.
Only `main` has C's implicit zero return guarantee; other functions should
explicitly return a value on every path where their caller uses the result.
Locals are addressed through the frame pointer. Arithmetic saves intermediate
values in 16-byte stack slots, keeping the stack aligned and preserving values
across nested expressions and calls. Assignment associates right-to-left;
arithmetic operands are evaluated left-to-right. As in C, signed integer overflow
and unsequenced conflicting accesses to a variable have undefined behavior.
Signed ordering tests operand signs before examining a subtraction result,
avoiding overflow errors when comparing opposite-sign values. Its `TBZ` branches
target nearby labels within each comparison; larger statement bodies use `CBZ`.
The same fixed-size sequence serves 64-bit operands with the `x` registers and
bit 63, and unsigned operands are pre-adjusted by adding 2^31 or 2^63, which
maps unsigned order onto signed order. 64-bit integer constants load from two
32-bit pool words combined by a shifted `ADD` (`lsl #32`), keeping the pool
word-aligned without wider pool entries.
Integer constants reside in a literal pool after the function. Strings use
`ADRP`/`ADD` with Linux or macOS page relocations. Pointer indexing extends
the integer index (`sxtw`, or `uxtw` for unsigned types) and uses shifted `ADD`
forms to scale it, retaining the
12-mnemonic vocabulary. A conservative code-size
limit keeps literal loads and return branches in range. Unwind metadata is not
implemented yet.

### Tests

`make test` checks syntax errors, header lookup, target-specific assembly, and
the allowed instruction vocabulary. On a native AArch64 host, it also links and
runs programs covering the examples, arithmetic precedence, assignment values,
integer boundaries, locals and intermediate values surviving calls, nested calls,
return values, scope shadowing, nested branches and loops, and the largest
supported stack frame. Function tests cover recursion, mutual recursion, prototype
diagnostics, parameter scope, and eight-argument calls in both directions between
generated code and system-compiled C. Memory tests cover byte conversions, escaped
strings, mutable arrays, scaled/negative indexing, pointer aliasing and indirection,
null tests, and mixed pointer/character calls across the native ABI.
Comparison tests cover all six operators over a matrix
including `INT_MIN`, `INT_MAX`, negative values, zero, and positive values.
Declaration tests cover enums (sequential, explicit, negative, and block-scoped
enumerator values; tag shadowing), `_Bool` normalization from integers,
characters, and pointers, comma-separated declarators for locals, globals, and
typedef aliases with per-declarator suffixes, `long`/`unsigned long` boundaries
and usual arithmetic conversions, unsigned ordering, and casts across the
scalar types, plus cross-ABI calls passing `long` and `_Bool` values.
On Linux with `objdump`, it audits
the actual machine instructions in generated functions, disabling disassembler aliases so
`ADD x29, sp, #0` is not displayed as `MOV`. Native execution tests are skipped on
other host architectures. Set `TEST_CC` to override the test linker driver.

The arithmetic example also prints `A`:

```sh
./build/4c examples/arithmetic.c -o build/arithmetic.s
cc build/arithmetic.s -o build/arithmetic
./build/arithmetic
```

The control-flow example prints `A` through `Z` followed by a newline:

```sh
./build/4c examples/alphabet.c -o build/alphabet.s
cc build/alphabet.s -o build/alphabet
./build/alphabet
```

The recursive example computes `sum(10) + 10` and prints `A`:

```sh
./build/4c examples/recursion.c -o build/recursion.s
cc build/recursion.s -o build/recursion
./build/recursion
```

The string-walking example prints `Hello, Jetson!`:

```sh
./build/4c examples/strings.c -o build/strings.s
cc build/strings.s -o build/strings
./build/strings
```

`examples/globals.c` combines typedef aliases, global scalar storage, and void
procedures. It checks a shared counter and prints `A`. These features are the
first milestone toward `examples/dhry.c`; Dhrystone still needs preprocessing,
enums/booleans, aggregates, additional operators/control flow, and library/ABI
support including floating-point reporting.

`examples/arrays.c` demonstrates a mutable character array, array indexing,
address-of, pointer assignment, and passing an array to a function.

## Target platforms

- NVIDIA Jetson boards, including Jetson Thor, running AArch64 Linux.
- Raspberry Pi boards running a 64-bit AArch64 Linux operating system.
- Apple Silicon Macs, including M1, M2, and later models, running macOS.

The computational backend will use baseline A64 instructions. Linux will use ELF
and its AArch64 ABI; macOS will use Mach-O and Apple's arm64 ABI. Platform-specific
assembly directives, symbol names, relocations, and argument placement will be
handled separately.

## Inspiration

[SUBLEQ](https://github.com/davidar/subleq) combines subtraction, memory access,
and conditional branching in one abstract instruction:

```text
mem[B] = mem[B] - mem[A]
if mem[B] <= 0: goto C
```

Stephen Dolan's [mov is Turing-complete](https://drwho.virtadpt.net/files/mov.pdf)
shows how loads and stores can implement computation, including equality tests
through memory aliasing. Its construction uses x86 `mov` operations and an
unconditional branch to repeat execution. On a RISC architecture, the operations
represented by those `mov` forms naturally span multiple instructions.

Our design takes inspiration from both, while using native AArch64 arithmetic,
branches, and calls. It does not require self-modifying executable code or an
interpreter for SUBLEQ.

## The 12-instruction vocabulary

| Mnemonic | Role |
| --- | --- |
| `LDR` | Load values, constants, pointers, and saved registers |
| `STR` | Store values and save registers |
| `LDRB` | Load an unsigned byte for character and buffer access |
| `STRB` | Store a byte |
| `ADD` | Addition, address calculations, and stack management |
| `SUB` | Subtraction, negation, copies, and synthesized arithmetic |
| `CBZ` | Branch on zero; unconditional branch using the zero register |
| `TBZ` | Branch when a selected bit is zero |
| `BL` | Call a named function |
| `BLR` | Call through a function pointer |
| `RET` | Return from a function |
| `ADRP` | Form page-relative addresses for globals and address tables |

We count **mnemonics with explicitly allowed operand forms**, not distinct binary
encodings. For example, `LDR` has multiple addressing forms and register widths.
The implementation should document its allowed forms and audit the actual emitted
instructions. Assembler aliases and pseudo-instructions must not silently expand
into instructions outside this vocabulary.

The restriction applies to code generated by this compiler. The platform's
startup objects, C library, dynamic loader, and linker-generated stubs or veneers
may contain other instructions. No special arithmetic or call trampoline runtime
is intended to be necessary for the initial integer/pointer subset.

### Why we expanded from four instructions

`LDR`, `STR`, `SUB`, and `CBZ` provide a simple computational core. Subtraction can
implement addition, and a branch on the architectural zero register implements
an unconditional jump.

That core has no conventional indirect call or return. Adding `ADD`, `BL`, and
`RET` makes stack frames and direct calls straightforward. `ADRP` and `BLR` bring
the practical count to nine by supporting normal position-independent addressing
and function-pointer calls. Byte loads and stores bring it to eleven and make
direct manipulation of C strings and buffers practical.

This is a practical instruction budget, not a claim of mathematical minimality.

### Why TBZ is the twelfth instruction

`TBZ` inspects a bit without requiring condition flags:

```asm
tbz x0, #63, nonnegative    // Sign test for a signed 64-bit integer
tbz x0, #0, even            // Parity test
tbz x0, #5, bit_is_clear    // Flag test
```

It fills an important gap in the smaller set:

- Signed ordering can check operand signs first. When the signs match, the sign
  of their difference determines ordering without signed subtraction overflow.
- Bitwise AND, OR, XOR, and shifts can be synthesized by inspecting bits and
  constructing results with additions.
- Multiplication and division can use binary algorithms instead of potentially
  enormous repeated-addition or repeated-subtraction loops.

The bit index is an immediate constant. Generated routines must use fixed bit
tests, unroll those tests, or dispatch among them. `TBZ` also has a shorter branch
range than `CBZ`; code layout must keep its targets nearby or introduce local
branch sequences. Large generated routines will need branch-range management.

## Basic code generation

`xzr` is AArch64's architectural zero register. These examples illustrate what
the original four-instruction core can express:

```asm
// Copy x1 into x0
sub x0, x1, xzr

// Negate x1
sub x0, xzr, x1

// Add x1 and x2 using subtraction and a scratch register
sub x9, xzr, x2
sub x0, x1, x9

// Branch if x1 == x2
sub x9, x1, x2
cbz x9, equal

// Unconditional branch
cbz xzr, target
```

With the final vocabulary, addition can simply use `ADD`. Constants may be loaded
with real `LDR` literal instructions and nearby constant pools. Global and external
addresses will use platform-appropriate `ADRP` plus `ADD` or `LDR` relocation
sequences. Constant pools and branches both require range-aware layout.

Arithmetic lowering must respect C integer widths, signedness, promotions, and
conversions. In particular, a wrapped subtraction cannot by itself implement
signed ordering. Signed byte loads also require explicit sign extension because
`LDRB` zero-extends its result.

## Calling the C runtime

Generated code will follow each platform's calling convention directly. The
compiler is responsible for argument and return-value placement, preserving
callee-saved registers, saving the link register when needed, and maintaining
16-byte stack alignment at the required boundaries.

For example, a function calling `getchar` can use this frame:

```asm
read_character:
    sub sp, sp, #16
    str x29, [sp]
    str x30, [sp, #8]
    add x29, sp, #0

    bl getchar              // Use _getchar on macOS

    ldr x30, [sp, #8]
    ldr x29, [sp]
    add sp, sp, #16
    ret                     // Result remains in w0
```

This is an instruction example; exported symbol declarations and unwind
directives are platform-specific and omitted here. Individual `LDR` and `STR`
instructions replace the usual paired loads and stores, so `LDP` and `STP` are
unnecessary. Register copies do not require `MOV`.

The initial subset should support integer and pointer interfaces such as `puts`,
`getchar`, `malloc`, `free`, and `memcpy`, as well as compiled callbacks passed to
C library functions. Live caller-saved values must be protected across calls.

Platform details include:

- Apple reserves `x18`; avoid using it in the shared register allocation scheme.
- Linker stubs may use `x16` and `x17`; do not retain live values in them across
  calls.
- Linux and macOS have different variadic argument placement rules. Calls such
  as `printf("%d", n)` require separate ABI lowering, but no new mnemonics.
- Narrow argument extension, stack argument layout, and frame conventions must
  follow the target ABI rather than assuming all arm64 platforms are identical.

Passing and receiving `float` or `double` can use floating-point register forms of
`LDR` and `STR` without increasing the mnemonic count. Floating-point arithmetic,
numeric conversions, aggregate arguments, and full C ABI coverage are separate
implementation work and are not promised by the initial subset.

The macOS target is ordinary `arm64`; an `arm64e` pointer-authentication ABI is
outside the initial scope.

## Initial C subset

The first implementation should cover:

- Integer types, including `char`, with defined target sizes and signedness.
- Local and global variables.
- Arithmetic, comparison, logical, and bitwise expressions.
- Assignment, `if`/`else`, and `while`.
- Functions, parameters, return values, and recursion.
- Pointers, arrays, and string literals.
- External function declarations and integer/pointer C library calls.
- Function pointers and callbacks.

Floating point, structs and unions, variadic function definitions, and a complete
preprocessor can wait. A system preprocessor may be used initially, but system
headers can contain language extensions beyond the supported subset; initial
examples can use controlled declarations for supported library interfaces.

The compiler must document the supported language precisely. An inefficient
implementation is acceptable; silently changing the semantics of supported C
operations is not.

## Compiler and build structure

Start with a small lexer and parser, semantic analysis, a simple intermediate
representation, and an AArch64 assembly emitter. Prioritize simple memory-backed
temporaries and correct function frames over sophisticated register allocation.

Emit assembly first rather than implementing ELF and Mach-O object writers. Use
the system assembler and invoke the system compiler driver for linking, so it
supplies the appropriate startup objects, library paths, and platform options.
The driver assembles and links the generated code; it does not compile our C
subset for us.

The computational lowering can be shared across targets. Keep assembly syntax
details, symbol naming, relocations, ABI argument classification, and object
format directives in small platform-specific components.

## Computational completeness

Mutable memory, arithmetic, and conditional control flow provide the building
blocks for universal computation. As with other real computers, the claim of
Turing completeness assumes arbitrarily extendable memory. A physical machine
with a fixed 64-bit address space has finite resources.

A formal proof should describe a memory-backed abstract machine with extendable
storage and show how its operations lower into the restricted vocabulary. Two
fixed-width registers alone must not be treated as unbounded mathematical
counters. That proof remains work to do.

## First milestones and validation

1. Emit a function that calls `getchar` or `puts` and returns through the native
   ABI on Linux and macOS.
2. Compile recursive factorial and an array-processing program.
3. Exercise allocation, byte buffers, function pointers, and callbacks.
4. Validate platform-specific integer variadic calls.
5. Audit generated function bodies to ensure they use only the 12 allowed
   mnemonics and approved operand forms.

Tests should compare supported C behavior with a system compiler, including zero,
negative values, integer boundaries, signed and unsigned comparisons, recursion,
and values surviving external calls. Avoid using undefined C behavior as a test
oracle. Instruction audits must distinguish executable instructions from embedded
literal data and exclude separately supplied system code.

## References

- [davidar/subleq](https://github.com/davidar/subleq)
- [Stephen Dolan: mov is Turing-complete](https://drwho.virtadpt.net/files/mov.pdf)
- [Arm A64 instruction overview](https://developer.arm.com/-/media/Files/pdf/graphics-and-multimedia/ARMv8_InstructionSetOverview.pdf)
- [Arm procedure-call standard for AArch64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
- [Apple: Writing ARM64 code for Apple platforms](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms)
- [Arm TBZ instruction reference](https://developer.arm.com/documentation/ddi0602/2024-12/Base-Instructions/TBZ--Test-bit-and-branch-if-zero-)
