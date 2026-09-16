/* Annotate only the completed output. This covers direct writes, buffered loop
   steps and private routines without changing code-size accounting or code.
   Operand descriptions deliberately avoid guessing source-variable identities. */
typedef struct { int frame_phase; const char *arithmetic_reason; } AsmCommentState;

static int asm_starts(const char *s, const char *prefix) {
    return !strncmp(s, prefix, strlen(prefix));
}

static const char *asm_branch_reason(const char *label) {
    if (asm_starts(label, ".Lshift")) return "move the operand bits one position per step until the shift count is exhausted";
    if (asm_starts(label, ".Lcond")) return "evaluate only the selected conditional arm and convert its result";
    if (asm_starts(label, ".Lbit")) return "include this result bit only when the bitwise operation requires it";
    if (asm_starts(label, ".Lreturn")) return "finish this function through its shared cleanup";
    if (asm_starts(label, ".Lmul")) return "skip an addition when this multiplier bit contributes nothing";
    if (asm_starts(label, ".Ldv") || asm_starts(label, ".Lds"))
        return "select the next step of software integer division";
    if (asm_starts(label, ".Lcmp") || asm_starts(label, ".Lpeq") || asm_starts(label, ".Lpend"))
        return "select the comparison result without condition flags";
    if (asm_starts(label, ".Lchar") || asm_starts(label, ".Lc9"))
        return "handle the sign of a narrow integer";
    if (asm_starts(label, ".Land") || asm_starts(label, ".Lor"))
        return "implement short-circuit logic, evaluating the right side only when needed";
    if (asm_starts(label, ".Lbool")) return "normalize a scalar to C's boolean values 0 and 1";
    if (asm_starts(label, ".Lcopy")) return "control the byte-by-byte aggregate copy";
    if (asm_starts(label, ".Lbor")) return "include each set bit only once in the bitwise OR result";
    if (asm_starts(label, ".Lsf_shift")) return "move only set bits into the software right-shift result";
    if (asm_starts(label, ".Lsw") || asm_starts(label, ".Lcse") || asm_starts(label, ".Ldfl"))
        return "dispatch or leave a switch case";
    if (strstr(label, "while") || strstr(label, "for") || asm_starts(label, ".Ldo"))
        return "control loop repetition or exit";
    if (asm_starts(label, ".Lelse") || asm_starts(label, ".Lendif"))
        return "choose or skip an if/else path";
    return "continue along the required control-flow path";
}

static const char *asm_call_reason(const char *symbol) {
    if (strstr(symbol, "__12c_sf_i64")) return "convert a signed integer to double bits using software rounding";
    if (strstr(symbol, "__12c_sf_u64")) return "convert an unsigned integer to double bits using software rounding";
    if (strstr(symbol, "__12c_sf_binary")) return "multiply or divide double bit patterns using only the allowed instructions";
    if (strstr(symbol, "__12c_sf_pack")) return "normalize and round a software double result";
    if (strstr(symbol, "__12c_sf_jam")) return "shift while remembering discarded nonzero bits for rounding";
    if (strstr(symbol, "__12c_sf_shr")) return "shift an unsigned bit pattern using the restricted instruction vocabulary";
    if (*symbol == '_') ++symbol; /* Darwin's external-symbol prefix. */
    if (!strcmp(symbol, "putchar")) return "write the character in w0 through the C runtime";
    if (!strcmp(symbol, "printf")) return "format and print the staged arguments through the C runtime";
    if (!strcmp(symbol, "fprintf")) return "format and write the staged arguments to the selected C stream";
    if (!strcmp(symbol, "malloc")) return "ask the C runtime for a block of memory";
    if (!strcmp(symbol, "free")) return "return an allocated block to the C runtime";
    if (!strcmp(symbol, "clock")) return "read process CPU time from the C runtime";
    if (!strcmp(symbol, "strcpy")) return "copy a zero-terminated string through the C runtime";
    if (!strcmp(symbol, "strcmp")) return "compare two zero-terminated strings through the C runtime";
    if (!strcmp(symbol, "strtol")) return "parse text as an integer through the C runtime";
    return "invoke this function with arguments in their ABI locations";
}

static void annotated_asm_line(FILE *out, const char *line, AsmCommentState *state) {
    const char *start = line;
    while (isspace((unsigned char)*start)) ++start;
    size_t length = strlen(start);
    if (asm_starts(start, "// Shared function epilogue.")) state->frame_phase = 2;
    else if (asm_starts(start, "// C line")) {
        state->frame_phase = 0;
        state->arithmetic_reason = NULL;
    }
    if (length && start[length - 1] == ':' &&
        (*start != '.' || asm_starts(start, ".L__12c_"))) state->frame_phase = 1;
    if (!*start || *start == '.' || asm_starts(start, "//") || start[length - 1] == ':') {
        if (length && start[length - 1] == ':') {
            if (asm_starts(start, ".Lcmp_end") || asm_starts(start, ".Lcopy_end") || asm_starts(start, ".Ldvo")) state->arithmetic_reason = NULL;
            else if (asm_starts(start, ".Lmul")) state->arithmetic_reason = "accumulate a product using additions";
            else if (asm_starts(start, ".Ldv") || asm_starts(start, ".Lds")) state->arithmetic_reason = "update a quotient, remainder, or sign in software division";
            else if (asm_starts(start, ".Lcopy")) state->arithmetic_reason = "advance or test the aggregate byte-copy position";
            else if (asm_starts(start, ".Lbor")) state->arithmetic_reason = "accumulate the set bits of the OR result";
            else if (asm_starts(start, ".Lcmp")) state->arithmetic_reason = "construct or test a comparison result without condition flags";
            else state->arithmetic_reason = NULL;
        }
        fprintf(out, "%s\n", line);
        return;
    }
    /* Some emission sites know more than can be inferred from operands. */
    if (strstr(start, "//")) { fprintf(out, "%s\n", line); return; }

    char *text = copy(start, length), *p = text;
    while (*p && !isspace((unsigned char)*p)) ++p;
    if (*p) *p++ = 0;
    char *args[4] = {"", "", "", ""};
    int count = 0, brackets = 0;
    while (*p && count < 4) {
        while (isspace((unsigned char)*p)) ++p;
        args[count++] = p;
        while (*p) {
            if (*p == '[') ++brackets;
            if (*p == ']') --brackets;
            if (*p == ',' && !brackets) break;
            ++p;
        }
        if (*p) *p++ = 0;
    }
    const char *a = args[0], *b = args[1], *c = args[2], *modifier = args[3];
    fprintf(out, "%s  // ", line);
    if (!strcmp(text, "ldr") || !strcmp(text, "ldrb") ||
        !strcmp(text, "str") || !strcmp(text, "strb")) {
        int load = text[0] == 'l';
        int bytes = strstr(text, "rb") ? 1 : *a == 'w' ? 4 : 8;
        size_t index;
        if (load && sscanf(b, ".LC%zu", &index) == 1 && index < nconstants) {
            fprintf(out, "Load constant word %u (0x%08x) into %s; obtain the constant bits needed by this computation.",
                    (unsigned)constants[index], (unsigned)constants[index], a);
        } else if (!strcmp(a, "x29") || !strcmp(a, "x30")) {
            fprintf(out, "%s %s %s %s; %s.", load ? "Restore" : "Save", a,
                    load ? "from" : "in", b, !strcmp(a, "x29")
                    ? "preserve the caller's frame pointer" : "preserve the return address for this function");
        } else if (load && (strstr(b, "GOT") || strstr(b, ":got"))) {
            fprintf(out, "Load the relocated address from %s into %s; find an external object through the global offset table.", b, a);
        } else if (!load && state->frame_phase == 1 && asm_starts(b, "[x29,")) {
            fprintf(out, "Store %d bytes from incoming argument %s in %s; keep this parameter available after other calls.", bytes, a, b);
        } else if (bytes == 1 && !strcmp(a, "w10") && strstr(b, ", x11]")) {
            fprintf(out, "%s one byte %s %s %s %s; copy the aggregate's byte at index x11, including padding.",
                    load ? "Load" : "Store", load ? "from" : "from", load ? b : a,
                    load ? "into" : "at", load ? a : b);
        } else {
            fprintf(out, "%s %d %s from %s %s ", load ? "Load" : "Store", bytes,
                    bytes == 1 ? "byte" : "bytes", load ? b : a,
                    load ? "into" : "at");
            fprintf(out, "%s", load ? a : b);
            /* Keep the action and purpose together, without assuming every
               stack value is an argument (some are arithmetic spills). */
            fputs("; ", out);
            if (*a == 'd') fputs("transfer double bits at the function-call boundary", out);
            else if (asm_starts(b, "[sp")) fputs(load ? "recover a staged value from the stack" : "keep a value safe in stack storage", out);
            else fputs(load ? "read the object value needed by the expression" : "update the destination object", out);
            if (load && bytes == 1) fputs(" (the byte is zero-extended)", out);
            fputc('.', out);
        }
    } else if (!strcmp(text, "add") || !strcmp(text, "sub")) {
        int add = !strcmp(text, "add");
        if (!strcmp(a, "sp") && !strcmp(b, "sp")) {
            fprintf(out, "%s %s bytes %s the stack; %s.", add ? "Release" : "Reserve", *c == '#' ? c + 1 : c,
                    add ? "from" : "on", add
                    ? (state->frame_phase == 2 ? "remove this function's frame before returning" : "discard temporary storage while keeping the stack aligned")
                    : (state->frame_phase == 1 ? "make room for this function's saved registers and locals" : "stage values while keeping the stack 16-byte aligned"));
        } else if (add && !strcmp(a, "x29") && !strcmp(b, "sp")) {
            fputs("Set x29 to the stack pointer; use a stable frame base while temporary stack storage changes.", out);
            state->frame_phase = 1;
        } else if (add && !strcmp(b, "x29")) {
            fprintf(out, "Put the address %s bytes above x29 in %s; locate this local or parameter for reading, writing, or taking its address.", *c == '#' ? c + 1 : c, a);
        } else if (add && !strcmp(a, "x0") && !strcmp(b, "x0") && !strcmp(c, "x0")) {
            fputs("Shift the double's bits left by one through addition; discard its sign so both positive and negative zero test false.", out);
        } else if (!add && !strcmp(b, c)) {
            fprintf(out, "Clear %s by subtracting %s from itself; produce zero without a MOV instruction.", a, b);
        } else if (!add && (!strcmp(c, "xzr") || !strcmp(c, "wzr"))) {
            fprintf(out, "Copy %s into %s by subtracting zero; move a value without a MOV instruction.", b, a);
            state->arithmetic_reason = NULL;
        } else if (!add && (!strcmp(b, "xzr") || !strcmp(b, "wzr"))) {
            fprintf(out, "Negate %s into %s by subtracting it from zero; implement unary minus or signed arithmetic.", c, a);
        } else if (!add && !strcmp(c, "#256") && !strcmp(a, b) && *a == 'w') {
            fprintf(out, "Subtract 256 from %s; interpret a byte with its top bit set as a negative signed character.", a);
        } else if (!add && (!strcmp(a, "x10") || !strcmp(a, "w10")) &&
                   (!strcmp(b, "x9") || !strcmp(b, "w9")) &&
                   (!strcmp(c, "x0") || !strcmp(c, "w0"))) {
            fprintf(out, "Subtract the right operand %s from the left operand %s into %s; prepare a comparison without a CMP instruction or condition flags.", c, b, a);
        } else if (strstr(c, "PAGEOFF") || strstr(c, ":lo12:")) {
            fprintf(out, "Add the symbol's page offset %s to %s into %s; finish its position-independent address.", c, b, a);
        } else if (*modifier) {
            unsigned shift;
            if (asm_starts(modifier, "sxtw") || asm_starts(modifier, "uxtw")) {
                fprintf(out, "%s-extend %s from 32 to 64 bits, then add it to %s into %s; preserve its %s value during arithmetic or addressing.",
                        *modifier == 's' ? "Sign" : "Zero", c, b, a, *modifier == 's' ? "signed" : "unsigned");
            } else if (sscanf(modifier, "lsl #%u", &shift) == 1) {
                fprintf(out, "Shift %s left by %u bits, then add it to %s into %s; scale or combine bits without a separate shift instruction.", c, shift, b, a);
            } else {
                fprintf(out, "%s %s and %s using %s into %s; compute the required extended or shifted arithmetic result.", add ? "Add" : "Subtract", b, c, modifier, a);
            }
        } else {
            fprintf(out, "%s %s %s %s into %s; %s.",
                    add ? "Add" : "Subtract", add ? b : c, add ? "to" : "from", add ? c : b, a,
                    state->arithmetic_reason ? state->arithmetic_reason : "compute the arithmetic value or address needed by this expression");
        }
    } else if (!strcmp(text, "cbz")) {
        if (!strcmp(a, "xzr") || !strcmp(a, "wzr"))
            fprintf(out, "Jump to %s because %s is always zero; %s.", b, a, asm_branch_reason(b));
        else fprintf(out, "If %s is zero, jump to %s; %s.", a, b, asm_branch_reason(b));
    } else if (!strcmp(text, "tbz")) {
        fprintf(out, "If bit %s of %s is clear, jump to %s; %s.", *b == '#' ? b + 1 : b, a, c, asm_branch_reason(c));
    } else if (!strcmp(text, "bl")) {
        fprintf(out, "Call %s and save the return address in x30; %s.", a, asm_call_reason(a));
    } else if (!strcmp(text, "blr")) {
        fprintf(out, "Call the address in %s and save the return address in x30; invoke an indirect function target.", a);
    } else if (!strcmp(text, "ret")) {
        fprintf(out, "Return to the address in %s; resume the caller with the result in its ABI register.", *a ? a : "x30");
    } else if (!strcmp(text, "adrp")) {
        fprintf(out, "Form the page address for %s in %s; begin locating a symbol without an absolute address.", b, a);
    } else {
        fatal("no educational annotation for instruction '%s'", text);
    }
    fputc('\n', out);
    free(text);
}

static void write_annotated_assembly(FILE *input, FILE *out) {
    size_t length = 0, capacity = 256;
    char *line = resize(NULL, capacity);
    AsmCommentState state = {0};
    int ch;
    while ((ch = fgetc(input)) != EOF) {
        if (ch == '\n') {
            line[length] = 0;
            annotated_asm_line(out, line, &state);
            length = 0;
        } else {
            if (length + 1 == capacity) { capacity *= 2; line = resize(line, capacity); }
            line[length++] = (char)ch;
        }
    }
    if (length) { line[length] = 0; annotated_asm_line(out, line, &state); }
    free(line);
}
