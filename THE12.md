# The Twelve Instructions of 12c

**Ground rule:** the budget counts instruction *names*, with their documented
operand forms. There is no `MOV`, no `CMP`, no `MUL`, no shifts, no
floating-point math — everything else is synthesized from these twelve.

All examples are taken from real 12c output (compiled from `examples/`).
Instructions are named in uppercase here, but the compiler emits lowercase
assembly, as in the listings.

---

## 1. `LDR` — load from memory

`LDR` is how the program reads: variable values, saved registers, pointers, and
64-bit double bit patterns. Crucially, it also loads *literal constants* —
AArch64 does have immediate-move instructions (`MOVZ`/`MOVN`), but they are not
among the twelve allowed names, so every constant in the program (even plain
`0` or `65`) lives in a literal pool of `.word` values placed after the
function, and `LDR` fetches it. The destination register's width selects the
access: `w0` loads 32 bits, `x0` loads 64, and `d0` loads the 8 raw bytes of a
double.

```asm
// int a = 65; — the constant comes from a literal pool, not a MOVZ
ldr w0, .LC0      // Load the word 65 into w0; obtain the constant bits.
```
```asm
// function epilogue — recovering the caller's return address
ldr x30, [sp, #8] // Read 8 bytes from the stack; restore the saved return address.
```

---

## 2. `STR` — store to memory

`STR` is the write path: it assigns variables, saves parameters, and protects
values across function calls. Because every register is scratch from the
callee's perspective, 12c is deliberately conservative — it spills an address
or operand to the stack whenever a later computation could clobber it. That is
why a simple `a = a + 1` involves several `STR`/`LDR` pairs: correctness
first, optimization never.

```asm
// prologue — saving the caller's frame pointer and return address
str x29, [sp]     // Store x29 at the top of the new frame; the caller needs it back.
str x30, [sp, #8] // Store x30 just above; a later BL will overwrite the link register.
```
```asm
// int a = 65; — completing the assignment
add x9, x29, #16  // Compute a's address in x9; locate its stack slot.
str w0, [x9]      // Store 4 bytes there; initialize the int a.
```

---

## 3. `LDRB` — load one byte, zero-extended

`LDRB` reads a single byte and zero-fills the rest of the destination
register. It is the only way 12c touches a `char` or `_Bool`, which occupy
exactly one byte, and it steps through aggregates byte by byte. The
zero-extension matters: without a dedicated sign-extending load, signed `char`
handling is done in software by testing bit 7 and correcting with `SUB` (on
Linux plain `char` is unsigned, so the load is already correct).

```asm
// reading a character through a pointer
ldrb w0, [x0]     // Load 1 byte, zero-extended; read the char the pointer addresses.
```
```asm
// struct assignment — copying a record byte by byte, padding included
ldrb w10, [x0, x11] // Load the byte at base + index; fetch the next byte of the struct.
```

---

## 4. `STRB` — store the low byte

`STRB` writes only the lowest byte of a register to memory, which is exactly
right for one-byte objects: `char` assignments, `_Bool` values normalized to
0/1, and each byte of a struct copy. Paired with `LDRB` and a counter, it
copies aggregates with no memory-copy instruction at all.

```asm
// char buf[...]; buf[i] = c; — writing into a char array
strb w0, [x9]     // Store the low byte of w0; update the character slot.
```
```asm
// struct copy — the destination side of the byte-by-byte copy loop
strb w10, [x9, x11] // Store one byte at destination base + index; write the struct byte.
```

---

## 5. `ADD` — addition, addresses, and everything scaled

`ADD` does plain arithmetic, but its documented operand forms make it the
compiler's Swiss army knife: it builds addresses (`add x9, x29, #16`), adjusts
the stack, and — with a shifted operand like `add x11, x11, x9, lsl #3` —
provides the multiply-by-2ⁿ step that powers both software multiplication and
array index scaling. Because a disassembler may print some ADD forms as `MOV`,
the project's object audit inspects the true operation.

```asm
// int a = 65; — address arithmetic: locals live at fixed offsets from x29
add x9, x29, #16  // Add 16 to the frame base; obtain a's address.
```
```asm
// software multiply, one bit of the multiplier — a shifted add accumulates the partial product
add x11, x11, x9, lsl #3 // Add x9 shifted left 3 bits; contribute multiplier bit 3's worth.
```

---

## 6. `SUB` — subtraction, negation, copies, and zero

`SUB` computes differences, but 12c leans on special operand tricks:
subtracting from the zero register `wzr` negates (`sub w0, wzr, w0`),
subtracting zero copies a register without a `MOV` (`sub w0, w11, wzr`), and
subtracting a register from itself produces zero (`sub w0, w0, w0`). It also
manages the stack in every prologue and epilogue. Equality comparisons are
built on it too: subtract the operands and test the difference for zero with
`CBZ`.

```asm
// every function starts by claiming its frame
sub sp, sp, #32   // Move sp down 32 bytes; reserve saved registers and locals.
```
```asm
// -x — unary minus via the zero register
sub w0, wzr, w0   // Subtract w0 from zero into w0; negate the value.
```

---

## 7. `CBZ` — compare and branch if zero

`CBZ` jumps to a label when a register holds zero, and it is the compiler's
*only* conditional branch. Every `if`, every loop back-edge, every `while`
condition bottoms out in a zero test. The elegant trick: branching on `xzr` —
a register that always reads zero — gives an unconditional branch, used for
jumping over `else` arms and into the shared epilogue.

```asm
// if (a) {...} else {...} — the conditional form
cbz w0, .Lelse0   // Jump to the else arm when a is zero; choose an if/else path.
```
```asm
// return 0; — the unconditional form: xzr is always zero
cbz xzr, .Lreturn16 // Jump to the shared epilogue; every return path funnels here.
```

---

## 8. `TBZ` — test bit and branch if zero

`TBZ` inspects one numbered bit of a register and jumps if that bit is clear —
no condition flags ever set or read. It is how 12c peeks at sign bits for
signed comparisons (bit 31 for `int`, bit 63 for `long`), and it drives the
bit-by-bit engines for multiplication, division, and the bitwise
`&`/`^`/`|`/shift operations, skipping the steps where a multiplier bit
contributes nothing.

```asm
// signed compare (a < b) — look at the sign bit before trusting a subtraction
tbz w0, #31, .Lcmp_positive1 // Jump when the sign bit is clear; the value is non-negative.
```
```asm
// software multiply — per-bit of the multiplier
tbz x0, #0, .Lmul0_0 // Jump when multiplier bit 0 is clear; this bit adds nothing.
```

---

## 9. `BL` — branch and link (call)

`BL` transfers control to a named function and saves the return address in the
link register `x30`. All calls — user functions, libc routines like
`putchar`, and 12c's own private software-arithmetic helpers — go through it.
The callee's epilogue later restores the caller's saved `x30` and `RET`s
through it, completing the round trip.

```asm
// putchar(a); — calling into libc
bl putchar        // Call putchar and save the next address in x30; print the char in w0.
```
```asm
// return sum(n-1) + n; — a function calling itself
bl sum            // Call sum recursively; the link register chains the pending returns.
```

---

## 10. `BLR` — branch to register (indirect call)

`BLR` calls the address held in a register rather than a fixed label — the
mechanism behind C function pointers and jump tables. 12c reserves it in the
instruction budget but does not emit it yet, because function pointers are not
implemented; the slot is a deliberate placeholder for that feature.

```asm
// (*fp)(3); — what a function-pointer call will emit (not yet generated)
blr x8            // Call the address in x8 and save the return address in x30.
```
```asm
// dispatch table: handlers[k]() — indirect call through a loaded entry (not yet generated)
ldr x10, [x9]     // Load the function address from the table slot.
blr x10           // Call whatever function the table named.
```

---

## 11. `RET` — return

`RET` jumps through a register — normally `x30`, the link register that `BL`
filled — returning control to the caller with the result already in the ABI
result register (`w0`/`x0`/`d0`). 12c emits exactly one `RET` site per
function: every C `return` statement branches (`CBZ xzr`) to a shared epilogue
that restores `x29`, `x30` and `sp` first, so the cleanup sequence exists
once, not per return.

```asm
// the shared epilogue of every function
ldr x30, [sp, #8] // Restore the return address; undo BL's overwrite of x30.
add sp, sp, #32   // Release the frame; hand the stack back to the caller.
ret               // Jump through x30; resume the caller with the result in w0.
```
```asm
// main's return — the loaded value waits in w0 for the caller (the C runtime)
ldr w0, .LC1      // Load constant 0; set main's integer return value.
ret               // Return; the C startup code receives the exit status.
```

---

## 12. `ADRP` — form a page-relative address

`ADRP` computes the address of the 4 KiB page containing a symbol — a
position-independent building block, never a complete address by itself.
Paired with one `ADD` of the symbol's low 12 bits (`:lo12:`), it reaches
globals and string literals; with the global offset table form, it reaches
external objects like `stderr`. The linker fills in the page and offset
relocations, which is what makes the output position-independent.

```asm
// reading a global: int current;
adrp x0, current  // Form the page address of current; begin a position-independent lookup.
add x0, x0, :lo12:current // Add the offset within the page; obtain current's full address.
```
```asm
// external object: stderr — resolved through the GOT
adrp x0, :got:stderr // Locate the GOT page; find where stderr's address is stored.
ldr x0, [x0, :got_lo12:stderr] // Read the resolved address; let the linker find stderr.
```

---

## Summary table

| # | Instruction | One-line role |
|---|---|---|
| 1 | `LDR` | Read memory — values, saved regs, literal constants |
| 2 | `STR` | Write memory — assignment, spills, ABI staging |
| 3 | `LDRB` | Read one byte, zero-extended |
| 4 | `STRB` | Write one byte |
| 5 | `ADD` | Arithmetic, addresses, scaled/shifted operands |
| 6 | `SUB` | Differences, negation, copies, zero, stack |
| 7 | `CBZ` | Branch if zero — every `if`/loop; `xzr` = unconditional |
| 8 | `TBZ` | Branch if a bit is zero — signs, bit engines |
| 9 | `BL` | Call by name, return address into `x30` |
| 10 | `BLR` | Call by register — reserved for function pointers |
| 11 | `RET` | Return through `x30` |
| 12 | `ADRP` | Page-relative symbol addresses (PIC) |
