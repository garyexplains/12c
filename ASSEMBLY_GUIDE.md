# From C to twelve AArch64 instructions

4c translates a small C language into assembly for an AArch64 processor. Its
generated code uses just twelve instruction names. This guide connects those
instructions to familiar C operations: creating a variable, assigning a value,
calling a function, testing a condition, and walking through memory.

You can read the assembly without running an AArch64 machine:

```sh
make
./build/4c --target linux examples/hello.c -o build/hello.s
```

Open `build/hello.s`. A `// C line ...` comment identifies the source statement.
Every instruction then has its own comment: the part before the semicolon says
what it does; the part after it explains its purpose. Labels and assembler
directives are not instructions and do not need this commentary.

On native AArch64 Linux, assemble, link and run the result with:

```sh
cc build/hello.s -o build/hello
./build/hello
```

Here `cc` assembles and links 4c's output. It does not compile the example's C.
The twelve-instruction rule applies to code emitted by 4c, including its software
floating-point routines. System startup code, libc and linker-generated code
have their own instructions and are outside that rule.

## The small amount of notation you need

| Notation | Meaning in this compiler |
| --- | --- |
| `x0` ... `x30` | 64-bit general-purpose registers: small storage locations inside the processor. |
| `w0` ... `w30` | The low 32 bits of the corresponding `x` register. Writing a `w` register clears that register's upper 32 bits. |
| `xzr`, `wzr` | Read as zero; writes are discarded. Useful for zero, negation and unconditional jumps. |
| `sp` | Stack pointer: the address of the current bottom of the stack. |
| `x29` | This compiler's frame pointer: a stable base for the current function's locals and parameters. |
| `x30` | Link register: where a call saves the address to return to. |
| `d0` ... `d7` | Floating-point argument/result registers used by the Linux double ABI. |
| `#16` | The number 16 written directly into the instruction: an *immediate*. |
| `[x9]` | Memory at the address held in `x9`. The brackets mean access memory, not the register itself. |
| `[sp, #8]` | Memory eight bytes above the address in `sp`. |
| `.Lelse7:` | A label giving an address a name. It does not execute. The numeric suffix just makes it unique. |
| `.word 65` | Place four bytes of data in the output. This is an assembler directive, not an instruction. |

An address and the contents at that address are different values. For example,
`add x9, x29, #16` computes an address; `ldr w0, [x9]` reads four bytes there.

Expression results normally use `w0` for 32-bit integers and `x0` for pointers,
64-bit integers and internal double bit patterns. `x9` often holds a saved left
operand or destination address. Registers such as `x10` and `x11` are scratch
space. Their roles can change between operations; a register is not permanently
attached to a C variable.

## The twelve instructions

| Instruction | What it does | Why 4c needs it |
| --- | --- | --- |
| `LDR` | Load a value from memory. | Read locals, pointers, literal constants, saved registers and double bits. |
| `STR` | Store a value in memory. | Assign objects, save parameters and protect intermediate values across calls. |
| `LDRB` | Load one byte and extend it with zeros. | Read a `char`, `_Bool`, or byte of an aggregate. |
| `STRB` | Store the low byte of a register. | Write characters, booleans and aggregate bytes. |
| `ADD` | Add two values; some forms can shift or extend an operand first. | Arithmetic, addresses, stack adjustments and synthesized operations. |
| `SUB` | Subtract one value from another. | Differences, negation, register copies and clearing registers. |
| `CBZ` | Jump if a register is zero. | Conditions, loop control, and unconditional jumps using `xzr`. |
| `TBZ` | Jump if a particular bit is zero. | Inspect signs and individual bits without condition flags. |
| `BL` | Call a named target and save the return address in `x30`. | User functions, libc and private software arithmetic routines. |
| `BLR` | Call an address held in a register. | Reserved for indirect calls; C function pointers are not implemented yet. |
| `RET` | Return through a register, normally `x30`. | Resume the caller. |
| `ADRP` | Compute a page-relative address for a symbol. | Locate globals, strings and external objects without fixed absolute addresses. |

The budget counts instruction names, with documented operand forms. A byte load
has its own name (`LDRB`), while a floating-register load still uses `LDR`.
Assembler aliases can disguise instructions: a disassembler may print `MOV` for
some `ADD` forms. The object audit disables aliases to inspect the real operation.

## 1. Establish a function's stack frame

For the small `hello.c` example, `main` starts with:

```asm
main:
    sub sp, sp, #32      // Move sp down 32 bytes; reserve this function's frame.
    str x29, [sp]       // Save the old frame pointer; the caller needs it back.
    str x30, [sp, #8]   // Save the return address; a later BL will overwrite x30.
    add x29, sp, #0     // Copy sp into x29; give locals a stable address base.
```

The stack grows toward lower addresses. In this frame, the saved `x29` occupies
offset 0, saved `x30` offset 8, and local `a` begins at offset 16. The frame has
padding so its size is a multiple of 16. Temporary expression and argument slots
are allocated below it by changing `sp`; `x29` stays fixed.

The platform calling convention, or *ABI*, is the agreement that allows separately
compiled functions to call one another. Maintaining stack alignment and saving
the caller's frame pointer are parts of that agreement.

## 2. Initialize `int a = 65`

Here are the three instructions emitted for the initializer in `hello.c`:

```asm
    ldr w0, .LC0        // Load the word 65 into w0; obtain the initializer value.
    add x9, x29, #16    // Compute a's address in x9; locate its stack slot.
    str w0, [x9]        // Store four bytes at that address; initialize the int a.
```

The constant is in a *literal pool* after the function's return code:

```asm
.LC0:
    .word 0x00000041    // Four bytes of data representing 65; not executed.
```

4c uses literal loads instead of relying on a `MOV` constant-building instruction.
The destination `w0` selects a 32-bit load. The address stays in a 64-bit register
because AArch64 pointers are 64 bits. `STR w0` writes four bytes, even though
the address register is 64 bits wide.

Reading the variable reverses the process:

```asm
    add x0, x29, #16    // Compute a's address; find the object to read.
    ldr w0, [x0]        // Read its four-byte value; make it the expression result.
```

The second instruction replaces the address in `x0` with the loaded value in
`w0`. This is intentional: that expression no longer needs the address.

## 3. Assign an expression: `a = a + 1`

Assignment needs both a destination address and a computed right-hand value.
The compiler saves the address because evaluating the expression also uses `x0`.
It similarly protects the left operand while evaluating the right operand.

The following shows that sequence with a readable name for the constant label:

```asm
    add x0, x29, #16    // Locate a; prepare the assignment destination.
    sub sp, sp, #16     // Reserve an aligned temporary slot; protect the address.
    str x0, [sp]        // Save a's address; expression evaluation will reuse x0.

    add x0, x29, #16    // Locate a again; the right-hand expression reads it.
    ldr w0, [x0]        // Read a; obtain the addition's left operand.
    sub sp, sp, #16     // Reserve another temporary slot; protect that operand.
    str x0, [sp]        // Save its bits; the next load will replace w0.
    ldr w0, .Lone       // Load 1; obtain the addition's right operand.
    ldr x9, [sp]        // Recover the saved left operand; keep both operands ready.
    add sp, sp, #16     // Release this temporary slot; the operand is in x9 now.
    add w0, w9, w0      // Add the two 32-bit values; calculate the new value of a.

    ldr x9, [sp]        // Recover a's destination address; prepare to store.
    add sp, sp, #16     // Release the address slot; restore the previous stack depth.
    str w0, [x9]        // Store the result in a; complete the assignment.
```

`.Lone` would contain `.word 1` in the literal pool. The real compiler uses unique
`.LC` labels. The result remains in `w0`, allowing assignment itself to be used
as an expression, such as `putchar(a = 65)`.

This is deliberately simple rather than optimized. Keeping a variable in a
register or removing redundant loads could shorten the code, but would require
more analysis of lifetimes and calls.

## 4. Call the C runtime: `putchar(a)`

For this call, the ABI expects the character argument in `w0`:

```asm
    sub sp, sp, #16     // Reserve an aligned argument slot; stage the argument.
    add x0, x29, #16    // Find a; prepare to read the argument expression.
    ldr w0, [x0]        // Read a's value; obtain the character code.
    str x0, [sp, #0]    // Save it in the argument slot; preserve its evaluated value.
    ldr w0, [sp, #0]    // Put it in w0; satisfy the callee's argument convention.
    add sp, sp, #16     // Release staging storage; the argument is in its register.
    bl putchar          // Call libc and save the next address in x30; print the character.
```

Why save and immediately reload a single argument? The same strategy also handles
multiple arguments and nested calls. 4c evaluates each argument into its own
temporary slot before loading the final argument registers. A call inside a later
argument cannot destroy an earlier argument's value. 4c chooses left-to-right
argument evaluation; portable C programs must not assume all C compilers do so.

Integer and pointer arguments use `w0`/`x0` onward according to width. Results use
`w0` for a 32-bit integer and `x0` for a pointer or 64-bit integer. Those registers
are scratch from the caller's perspective: a called function may overwrite them.
This is why locals and intermediate values are saved in memory.

`BL` is not a system call instruction. It enters the linked library function.
Libc and the operating system handle the actual output. On macOS the assembler
spells this external symbol `_putchar`; the Linux symbol is `putchar`.

## 5. Return to the caller

For `return 0`, 4c loads zero into `w0` and jumps to its shared epilogue:

```asm
    ldr w0, .Lzero      // Load zero; set main's integer return value.
    cbz xzr, .Lreturn   // Always jump because xzr is zero; share one cleanup path.
.Lreturn:
    ldr x30, [sp, #8]   // Recover the original return address; undo BL's overwrite.
    ldr x29, [sp]       // Recover the caller's frame pointer; restore its frame base.
    add sp, sp, #32     // Release this function's frame; restore the caller's sp.
    ret                 // Jump through x30; resume the caller with w0 as the result.
```

Here `.Lzero` means a pool word containing zero. All expression temporaries have
already been removed before reaching the epilogue. Multiple C `return` statements
can therefore use this same cleanup sequence.

## 6. Conditions, loops and comparisons

For `if (a)`, the value zero means false:

```asm
    add x0, x29, #16    // Locate a; obtain the condition's address.
    ldr w0, [x0]        // Read a; test its actual value rather than its address.
    cbz w0, .Lelse      // Jump when a is zero; skip the true branch.
    // Instructions for the true branch go here.
    cbz xzr, .Lend      // Always jump; avoid also executing the else branch.
.Lelse:
    // Instructions for the false branch go here.
.Lend:
```

A `while` loop adds a label before the condition and an unconditional
`cbz xzr, ...` back to that label after the body. `break` jumps to the enclosing
loop's exit. `&&` skips its right operand when the left is false; `||` skips it
when the left is true. These jumps preserve C's short-circuit behavior.

Equality needs a difference and a zero test:

```asm
    sub w10, w9, w0     // Subtract the right operand from the left; equal values give zero.
    cbz w10, .Lequal    // Jump when the difference is zero; select the equality result.
```

The remaining paths explicitly produce 0 or 1. No `CMP`, condition-code branch,
or conditional-select instruction is needed.

Signed ordering is more subtle: subtracting two signed values can wrap. 4c uses
`TBZ` to examine their sign bits first. If the signs differ, the negative operand
is smaller. If they agree, the subtraction's sign determines the ordering.
The sign bit is bit 31 for `int` and bit 63 for `long`.

Unsigned ordering maps the operands into signed order by adding the top-bit
constant before the same comparison strategy. When widths differ, sign or zero
extension must happen before comparing. For example, widening `-1` as unsigned
bits would incorrectly turn it into a large positive `long`.

## 7. Bytes, pointers, arrays and globals

Writing and reading through a character pointer uses byte instructions:

```asm
    strb w0, [x9]       // Store the low byte of w0; update the character pointed to by x9.
    ldrb w0, [x9]       // Read one byte with zero extension; recover that character's bits.
```

On Linux, plain `char` is unsigned. For macOS signed `char`, 4c tests bit 7 and
subtracts 256 when the byte represents a negative value. `_Bool` storage also
uses one byte, with scalar assignments normalized to 0 or 1 first.

Array indexing is address arithmetic. For an `int *p`, `p + i` means move `i`
elements, not `i` bytes. Once the index has been extended to 64 bits, an `ADD`
operand can scale it:

```asm
    add x10, x10, x0, lsl #2 // Add x0 shifted left two bits; scale an int index by four bytes.
```

In this mechanism `x10` starts at zero. Sizes that are not powers of two use
several shifted additions. Multidimensional arrays scale by a whole row's size.
Member access adds a fixed byte offset from the record's base address.

Global addresses cannot be expressed relative to the current stack frame. For
a Linux global `counter`, the address construction is:

```asm
    adrp x0, counter             // Find counter's page; start a position-independent address.
    add x0, x0, :lo12:counter    // Add its offset within the page; obtain counter's full address.
    ldr w0, [x0]                // Read its four-byte value; evaluate the global int.
```

The linker fills in the page and offset relocations. External objects such as
Linux `stderr` instead use the global offset table (GOT):

```asm
    adrp x0, :got:stderr         // Locate the GOT page; find the external object's address slot.
    ldr x0, [x0, :got_lo12:stderr] // Read the resolved address; let the linker locate stderr.
    ldr x0, [x0]                // Read the FILE pointer stored there; obtain the stream argument.
```

The distinction matters: the GOT supplies the address of the `stderr` object;
that object itself contains a pointer to a stream. Darwin symbol and variadic
ABI issues remain outstanding; these snippets describe the working Linux path.

Struct assignment copies bytes, including padding. A counter in `x11` selects
the next byte for `LDRB` from the source and `STRB` to the destination. `ADD`
advances the counter, `SUB` compares it with the object size, and `CBZ` exits or
repeats. No special memory-copy instruction is required.

## 8. Arithmetic without multiply, divide or bitwise instructions

The compiler constructs multiplication from the multiplier's bits. For one bit:

```asm
    tbz x0, #3, .Lskip_bit       // Skip when multiplier bit 3 is clear; it contributes nothing.
    add x11, x11, x9, lsl #3    // Add eight times the multiplicand; accumulate this bit's contribution.
.Lskip_bit:
```

The accumulator `x11` starts at zero. The compiler emits a step for every bit at
the chosen width. Unsigned arithmetic keeps the low-width result; signed C
overflow remains undefined even though the hardware operations wrap.

Integer division uses binary long division: shift a partial remainder, bring
down a dividend bit, subtract the divisor when possible, and set a quotient bit.
Sign handling surrounds the unsigned-magnitude calculation. Integer division,
remainder, and multiplication support both 32-bit and 64-bit operands. Remainder
selects the final partial remainder instead of the quotient and restores only
the dividend's sign. Unsigned comparisons check the top bits before subtracting
so divisors above the signed range are handled correctly.

Bitwise AND sets a result bit when both input bits are set; XOR sets it when
exactly one is set. TBZ selects those cases and a shifted ADD inserts the bit
into an initially zero result. Complement uses `-value - 1`, implemented by two
SUB instructions at the promoted operand's width.

For `value << count`, a loop doubles the value with ADD once per shift position.
For `value >> count`, each step rebuilds the value with bit tests and shifted
adds, moving source bit i to result bit i-1. Signed right shift also restores
the sign bit. CBZ stops after the requested number of steps.

Compound assignment saves the destination address on the stack, evaluates the
right operand, loads the old destination value, performs the operation, and
stores the converted result back through the saved address. Postfix increment
also saves the old value, returning it after writing the incremented value.
This ensures expressions such as `*p++ *= 7` advance the pointer only once.

Bitwise OR tests both operands' bits and adds a bit's value only if it is not
already present. The software right-shift primitive similarly tests each source
bit and adds its value at the next lower position. `TBZ` targets nearby labels;
larger control-flow paths use `CBZ`. Generated instruction growth still has to
respect branch and literal-load reach.

## 9. Doubles without floating-point arithmetic instructions

Internally, a double is a 64-bit pattern in `x0` or memory. Software routines
unpack its sign, exponent and significand, compute with integers, and round the
result. Those routines are C text in [src/softfloat.h](src/softfloat.h), compiled
by 4c itself. They must pass the same instruction audit as the user program.

The Linux ABI still expects a double in a floating-point register at a call
boundary. A memory round trip moves the bits without `FMOV`:

```asm
    sub sp, sp, #16     // Reserve an aligned slot; prepare a bit-preserving transfer.
    str x0, [sp]        // Store the double's raw bits; make them available to a floating-register load.
    ldr d0, [sp]        // Load those bits into d0; place the double in an ABI register.
    add sp, sp, #16     // Release the slot; the value is now in d0.
```

This transfers bits; it does not convert an integer's numerical value to a
double. That conversion requires the software rounding routine.

Linux assigns integer and double argument registers independently. For
`printf("%d %.1f", 7, 1.5)`, the format pointer goes in `x0`, 7 in `w1`, and
1.5 in `d0`. Double results also use `d0`. The compiler stages all arguments
before loading those registers. The supported operations and numerical limits
are documented in [FLOATING_POINT.md](FLOATING_POINT.md).

## Reading and checking your own examples

Start with `hello.c`, then try `arithmetic.c`, `alphabet.c`, `recursion.c`,
`arrays.c` and `globals.c`. Change one statement at a time and compare the output.
Look for three things: where the values live, which values must survive the next
operation, and where control can jump.

The explanations are added in [src/asm_comments.h](src/asm_comments.h) after code
generation, including buffered loop steps, prologues, epilogues and private
routines. This keeps explanatory text out of instruction-size accounting. Tests
require an explanation on every emitted instruction and check that removing the
comments leaves identical assembled code/data. The normal native tests still
execute the annotated assembly and audit the actual machine instructions.

This is a learning compiler, not a complete C implementation. The wider suite
still exposes the deferred correctness issues listed in [TODO.md](TODO.md).
The supported Linux Dhrystone path has its own end-to-end checks. Use those
limits when choosing experiments, and use system C as an independent reference
for behavior that the subset claims to support.
