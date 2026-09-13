#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DEFAULT_INCLUDE_DIR
#define DEFAULT_INCLUDE_DIR "include"
#endif

/* A deliberately small C frontend. The host compiler builds this file;
   only the assembly emitted below is subject to the instruction whitelist. */
typedef struct {
    char *text;
    const char *file;
    int line;
    const char *source;
} Token;

typedef enum { TY_INT, TY_CHAR, TY_PTR, TY_ARRAY, TY_VOID } TypeKind;
typedef struct Type {
    TypeKind kind;
    struct Type *base;
    int size;
} Type;
static Type void_type = {TY_VOID, NULL, 0};
static Type int_type = {TY_INT, NULL, 4}, char_type = {TY_CHAR, NULL, 1};
typedef struct { Type *type; int lvalue, local, zero; } Expr;
typedef struct { unsigned char *data; size_t length; } String;
static String *strings;
static size_t nstrings;
static int frame_bytes, max_frame_bytes;

typedef struct {
    char *name;
    int offset;
    Type *type;
    int initialized;
} Local;

static Token *tokens;
static size_t ntokens, capacity, pos;
static Local locals[256];
static size_t nlocals, scope_base, next_label;
/* An offset of -1 marks a typedef in the ordinary identifier namespace. */
static Local globals[256];
static uint32_t global_values[256];
static size_t nglobals;
static int find_local(const char *s);
static int find_global(const char *s) {
    for (size_t i = 0; i < nglobals; ++i)
        if (!strcmp(globals[i].name, s)) return (int)i;
    return -1;
}
static Type *alias_type(const char *s) {
    int i = find_local(s);
    if (i >= 0) return locals[i].offset == -1 ? locals[i].type : NULL;
    i = find_global(s);
    return i >= 0 && globals[i].offset == -1 ? globals[i].type : NULL;
}
static Token *current(void);
static int type_start(void) {
    const char *s = current()->text;
    return !strcmp(s, "int") || !strcmp(s, "char") ||
           !strcmp(s, "void") || alias_type(s) != NULL;
}
typedef struct {
    char *name;
    int nparams;
    int defined;
    Type *result;
    Type *params[8];
} Function;

static Function functions[256];
static size_t nfunctions;
static uint32_t *constants;
static size_t nconstants;
static const char *include_dir = DEFAULT_INCLUDE_DIR;
static int macos;
static FILE *body, *program;
static size_t body_chars, current_function;

static void fatal(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "4c: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *resize(void *p, size_t size) {
    void *result = realloc(p, size);
    if (!result) fatal("out of memory");
    return result;
}

static char *copy(const char *s, size_t len) {
    char *result = resize(NULL, len + 1);
    memcpy(result, s, len);
    result[len] = 0;
    return result;
}

static void token(const char *s, size_t len, const char *file, int line) {
    if (ntokens == capacity) {
        capacity = capacity ? capacity * 2 : 128;
        tokens = resize(tokens, capacity * sizeof(*tokens));
    }
    tokens[ntokens++] = (Token){copy(s, len), file, line, s};
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open %s: %s", path, strerror(errno));
    size_t used = 0, cap = 4096;
    char *data = resize(NULL, cap);
    int ch, line = 1;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == 0) fatal("%s:%d: embedded NUL byte", path, line);
        if (used + 1 == cap) data = resize(data, cap *= 2);
        data[used++] = (char)ch;
        if (ch == '\n') ++line;
    }
    if (ferror(f)) fatal("cannot read %s", path);
    fclose(f);
    data[used] = 0;
    return data;
}

/* Only #include <stdio.h> is supported in phase one. No host headers or host
   preprocessor are involved. Repeated prototypes are harmless. */
static void lex(const char *path, int depth) {
    if (depth > 8) fatal("%s: include nesting too deep", path);
    char *data = read_file(path), *p = data;
    int line = 1, line_start = 1;
    while (*p) {
        if (*p == '\n') { ++line; line_start = 1; ++p; continue; }
        if (isspace((unsigned char)*p)) { ++p; continue; }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') ++p;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            int start = line;
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') { ++line; line_start = 1; }
                ++p;
            }
            if (!*p) fatal("%s:%d: unterminated comment", path, start);
            p += 2;
            continue;
        }
        if (*p == '#' && line_start) {
            ++p;
            while (*p == ' ' || *p == '\t') ++p;
            if (strncmp(p, "include", 7))
                fatal("%s:%d: only #include <stdio.h> is supported", path, line);
            p += 7;
            while (*p == ' ' || *p == '\t') ++p;
            if (strncmp(p, "<stdio.h>", 9))
                fatal("%s:%d: only #include <stdio.h> is supported", path, line);
            p += 9;
            while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
            if (*p && *p != '\n')
                fatal("%s:%d: unexpected text after include", path, line);
            size_t size = strlen(include_dir) + sizeof("/stdio.h");
            char *header = resize(NULL, size);
            snprintf(header, size, "%s/stdio.h", include_dir);
            lex(header, depth + 1);
            continue;
        }
        line_start = 0;
        char *start = p;
        if (*p == '"' || *p == '\'') {
            int quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\n' || *p == '\r')
                    fatal("%s:%d: newline in literal", path, line);
                if (*p == '\\') {
                    ++p;
                    if (!*p || *p == '\n' || *p == '\r')
                        fatal("%s:%d: unterminated literal", path, line);
                }
                ++p;
            }
            if (!*p) fatal("%s:%d: unterminated literal", path, line);
            ++p;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            do { ++p; } while (isalnum((unsigned char)*p) || *p == '_');
        } else if (isdigit((unsigned char)*p)) {
            do { ++p; } while (isalnum((unsigned char)*p) || *p == '_');
        } else if ((strchr("=!<>", p[0]) && p[1] == '=')) {
            p += 2;
        } else if (strchr("(){}[];=,+-<>*&", *p)) {
            if ((p[0] == '+' && p[1] == '+') || (p[0] == '-' && p[1] == '-'))
                fatal("%s:%d: increment and decrement are not supported", path, line);
            ++p;
        } else {
            fatal("%s:%d: unsupported character '%c'", path, line, *p);
        }
        token(start, (size_t)(p - start), path, line);
    }
    if (!depth) token("<eof>", 5, path, line);
    /* Tokens retain source spans for assembly comments until compilation ends. */
}

static Token *current(void) { return &tokens[pos]; }

static void error(const char *message) {
    fatal("%s:%d: %s (got '%s')", current()->file, current()->line,
          message, current()->text);
}

static int take(const char *text) {
    if (strcmp(current()->text, text)) return 0;
    ++pos;
    return 1;
}

static void expect(const char *text) {
    if (!take(text)) {
        char message[128];
        snprintf(message, sizeof(message), "expected '%s'", text);
        error(message);
    }
}

static int identifier(const char *s) {
    static const char *keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default",
        "do", "double", "else", "enum", "extern", "float", "for", "goto",
        "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch",
        "typedef", "union", "unsigned", "void", "volatile", "while",
        "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
        "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"
    };
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    for (size_t i = 0; i < sizeof(keywords) / sizeof(*keywords); ++i)
        if (!strcmp(s, keywords[i])) return 0;
    return 1;
}

static char *name(void) {
    if (!identifier(current()->text)) error("expected an identifier");
    return tokens[pos++].text;
}

static Type *derived(TypeKind kind, Type *base, int size) {
    Type *t = resize(NULL, sizeof(*t));
    *t = (Type){kind, base, size};
    return t;
}
static Type *pointer(Type *base) { return derived(TY_PTR, base, 8); }
static int same_type(Type *a, Type *b) {
    return a->kind == b->kind && a->size == b->size &&
           (!a->base || same_type(a->base, b->base));
}
static int integer(Type *t) { return t->kind == TY_INT || t->kind == TY_CHAR; }
static Type *parse_type(void) {
    Type *t;
    if (take("int")) t = &int_type;
    else if (take("char")) t = &char_type;
    else if (take("void")) t = &void_type;
    else if ((t = alias_type(current()->text))) ++pos;
    else { error("expected 'int', 'char', 'void', or a typedef name"); return NULL; }
    while (take("*")) {
        if (t->kind == TY_VOID) error("void pointers are not supported yet");
        t = pointer(t);
    }
    return t;
}
static int decimal(void) {
    const char *s = current()->text;
    if (!isdigit((unsigned char)*s) || (s[0] == '0' && s[1]))
        error("expected a decimal integer from 0 to 2147483647");
    errno = 0;
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (*end || errno || n > INT32_MAX)
        error("expected a decimal integer from 0 to 2147483647");
    ++pos;
    return (int)n;
}
static Type *array_suffix(Type *t, int parameter) {
    if (!take("[")) return t;
    if (!t->size) error("array element type must be complete");
    int n = -1;
    if (strcmp(current()->text, "]")) n = decimal();
    expect("]");
    if (parameter) return pointer(t);
    if (n == -1) return derived(TY_ARRAY, t, 0);
    if (n <= 0 || n > 4064 / t->size) error("array size must fit the 4080-byte frame limit");
    return derived(TY_ARRAY, t, n * t->size);
}
static int allocate(Type *t) {
    int offset = (frame_bytes + 7) & ~7;
    frame_bytes = offset + t->size;
    if (frame_bytes > 4080) error("locals exceed the 4080-byte frame limit");
    if (frame_bytes > max_frame_bytes) max_frame_bytes = frame_bytes;
    return offset;
}

static int find_local(const char *s) {
    for (size_t i = nlocals; i > 0; --i)
        if (!strcmp(locals[i - 1].name, s)) return (int)i - 1;
    return -1;
}

static int find_function(const char *s) {
    for (size_t i = 0; i < nfunctions; ++i)
        if (!strcmp(functions[i].name, s)) return (int)i;
    return -1;
}

static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int written = vfprintf(body, fmt, ap);
    va_end(ap);
    if (written < 0) fatal("cannot write assembly buffer");
    body_chars += (size_t)written;
}

/* Flatten a source span to one // comment, including multiline statements.
   Whitespace is normalized; all original non-whitespace characters are kept. */
static void source_comment(FILE *out, size_t first, size_t last) {
    const char *p = tokens[first].source;
    const char *end = tokens[last].source + strlen(tokens[last].text);
    fprintf(out, "    // C line %d: ", tokens[first].line);
    int space = 0;
    for (; p < end; ++p) {
        if (isspace((unsigned char)*p)) { space = 1; continue; }
        if (space) fputc(' ', out);
        space = 0;
        fputc((unsigned char)*p, out);
    }
    fputc('\n', out);
}

static void literal(uint32_t value) {
    constants = resize(constants, (nconstants + 1) * sizeof(*constants));
    constants[nconstants] = value;
    emit("    ldr w0, .LC%zu\n", nconstants++);
}

static Expr expression(void);
static Expr value(Expr e) {
    if (e.type->kind == TY_VOID) error("void expression has no value");
    if (e.type->kind == TY_ARRAY) {
        e.type = pointer(e.type->base);
    } else if (e.lvalue) {
        if (e.local >= 0 && !locals[e.local].initialized)
            error("unknown local value in self-initializer");
        if (e.type->kind == TY_PTR) emit("    ldr x0, [x0]\n");
        else if (e.type->kind == TY_INT) emit("    ldr w0, [x0]\n");
        else {
            emit("    ldrb w0, [x0]\n");
            if (macos) {
                size_t id = next_label++;
                emit("    tbz w0, #7, .Lchar%zu\n    sub w0, w0, #256\n.Lchar%zu:\n", id, id);
            }
        }
    }
    e.lvalue = 0;
    e.local = -1;
    return e;
}
static void narrow_char(void) {
    emit("    sub sp, sp, #16\n    strb w0, [sp]\n    ldrb w0, [sp]\n    add sp, sp, #16\n");
    if (macos) {
        size_t id = next_label++;
        emit("    tbz w0, #7, .Lchar%zu\n    sub w0, w0, #256\n.Lchar%zu:\n", id, id);
    }
}
static Expr convert(Expr e, Type *to) {
    e = value(e);
    if (to->kind == TY_PTR) {
        if (!(same_type(e.type, to) || (integer(e.type) && e.zero)))
            error("incompatible pointer conversion");
    } else {
        if (!integer(e.type)) error("cannot convert pointer to integer");
        if (to->kind == TY_CHAR) narrow_char();
    }
    return (Expr){to, 0, -1, e.zero};
}
static void store_value(Type *t, const char *address) {
    emit("    %s %s0, [%s]\n", t->kind == TY_CHAR ? "strb" : "str",
         t->kind == TY_PTR ? "x" : "w", address);
}
static String decode_literal(void) {
    const unsigned char *p = (unsigned char *)current()->text + 1;
    const unsigned char *end = (unsigned char *)current()->text + strlen(current()->text) - 1;
    unsigned char *data = resize(NULL, strlen(current()->text) + 1);
    size_t n = 0;
    while (p < end) {
        unsigned int ch = *p++;
        if (ch == '\\') {
            ch = *p++;
            if (ch >= '0' && ch <= '7') {
                ch -= '0';
                for (int i = 1; i < 3 && p < end && *p >= '0' && *p <= '7'; ++i)
                    ch = ch * 8 + (*p++ - '0');
            } else if (ch == 'x') {
                if (p == end || !isxdigit(*p)) error("hex escape requires digits");
                ch = 0;
                while (p < end && isxdigit(*p)) {
                    unsigned int digit = isdigit(*p) ? *p - '0' : tolower(*p) - 'a' + 10;
                    ch = ch * 16 + digit;
                    if (ch > 255) error("escape does not fit a byte");
                    ++p;
                }
            } else {
                switch (ch) {
                case 'n': ch = '\n'; break; case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break; case 'a': ch = '\a'; break;
                case 'b': ch = '\b'; break; case 'f': ch = '\f'; break;
                case 'v': ch = '\v'; break;
                case '\\': case '\'': case '"': case '?': break;
                default: error("unsupported escape");
                }
            }
        }
        if (ch > 255) error("escape does not fit a byte");
        data[n++] = (unsigned char)ch;
    }
    data[n] = 0;
    ++pos;
    return (String){data, n};
}
static String string_bytes(void) {
    String str = decode_literal();
    while (current()->text[0] == '"') {
        String next = decode_literal();
        str.data = resize(str.data, str.length + next.length + 1);
        memcpy(str.data + str.length, next.data, next.length + 1);
        str.length += next.length;
        free(next.data);
    }
    return str;
}
static Expr string_literal(void) {
    String str = string_bytes();
    if (str.length >= INT32_MAX) error("string is too long");
    strings = resize(strings, (nstrings + 1) * sizeof(*strings));
    strings[nstrings] = str;
    if (macos)
        emit("    adrp x0, Lstr%zu@PAGE\n    add x0, x0, Lstr%zu@PAGEOFF\n", nstrings, nstrings);
    else
        emit("    adrp x0, Lstr%zu\n    add x0, x0, :lo12:Lstr%zu\n", nstrings, nstrings);
    ++nstrings;
    return (Expr){derived(TY_ARRAY, &char_type, (int)str.length + 1), 1, -1, 0};
}
static Expr primary(void) {
    if (take("(")) { Expr e = expression(); expect(")"); return e; }
    if (current()->text[0] == '"') return string_literal();
    if (current()->text[0] == '\'') {
        String str = decode_literal();
        if (str.length != 1) error("character literal must contain one byte");
        int n = str.data[0];
        if (macos && n >= 128) n -= 256;
        free(str.data);
        literal((uint32_t)n);
        return (Expr){&int_type, 0, -1, n == 0};
    }
    if (isdigit((unsigned char)*current()->text)) {
        int n = decimal(); literal((uint32_t)n);
        return (Expr){&int_type, 0, -1, n == 0};
    }
    if (!identifier(current()->text)) error("expected an integer expression or object");
    char *s = current()->text;
    int local = find_local(s);
    int global = find_global(s);
    if ((local >= 0 && locals[local].offset == -1) ||
        (local < 0 && global >= 0 && globals[global].offset == -1))
        error("typedef name is not an expression");
    if (pos + 1 < ntokens && !strcmp(tokens[pos + 1].text, "(")) {
        if (local >= 0 || global >= 0) error("called name is an object, not a function");
        int function = find_function(s);
        if (function < 0) error("call to an undeclared function");
        Function *fn = &functions[function];
        int expected = fn->nparams;
        ++pos; expect("(");
        if (expected) emit("    sub sp, sp, #%d\n", expected * 16);
        int count = 0;
        if (!take(")")) {
            do {
                if (count == expected) error("wrong number of function arguments");
                convert(expression(), fn->params[count]);
                emit("    str x0, [sp, #%d]\n", count++ * 16);
            } while (take(","));
            expect(")");
        }
        if (count != expected) error("wrong number of function arguments");
        for (int i = 0; i < count; ++i)
            emit("    ldr %s%d, [sp, #%d]\n", fn->params[i]->kind == TY_PTR ? "x" : "w", i, i * 16);
        if (expected) emit("    add sp, sp, #%d\n", expected * 16);
        emit("    bl %s%s\n", macos ? "_" : "", s);
        if (fn->result->kind == TY_CHAR) narrow_char();
        return (Expr){fn->result, 0, -1, 0};
    }
    if (local < 0) {
        if (global < 0) error("unknown local or global variable");
        ++pos;
        if (macos)
            emit("    adrp x0, _%s@PAGE\n    add x0, x0, _%s@PAGEOFF\n", s, s);
        else
            emit("    adrp x0, %s\n    add x0, x0, :lo12:%s\n", s, s);
        return (Expr){globals[global].type, 1, -1, 0};
    }
    ++pos;
    emit("    add x0, x29, #%d\n", locals[local].offset);
    return (Expr){locals[local].type, 1, local, 0};
}
/* Scale a signed int index in w0 without new instruction mnemonics. */
static void scale_index(int size) {
    emit("    sub x10, x10, x10\n    add x0, x10, w0, sxtw\n");
    if (size == 1) return;
    for (int bit = 0; size; ++bit, size >>= 1)
        if (size & 1) emit("    add x10, x10, x0, lsl #%d\n", bit);
    emit("    sub x0, x10, xzr\n");
}
static Expr binary_add(Expr left, Expr right, int subtract) {
    if (left.type->kind == TY_PTR && integer(right.type)) {
        if (!left.type->base->size) error("pointer arithmetic requires a complete object type");
        scale_index(left.type->base->size);
        emit("    %s x0, x9, x0\n", subtract ? "sub" : "add");
        return (Expr){left.type, 0, -1, 0};
    }
    if (!subtract && integer(left.type) && right.type->kind == TY_PTR) {
        emit("    sub x11, x0, xzr\n    sub x0, x9, xzr\n    sub x9, x11, xzr\n");
        return binary_add(right, left, 0);
    }
    if (!integer(left.type) || !integer(right.type)) error("unsupported pointer arithmetic");
    emit("    %s w0, w9, w0\n", subtract ? "sub" : "add");
    return (Expr){&int_type, 0, -1, 0};
}
static Expr postfix(void) {
    Expr e = primary();
    while (take("[")) {
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr index = value(expression());
        expect("]");
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        if (!(e.type->kind == TY_PTR || index.type->kind == TY_PTR))
            error("indexing requires a pointer or array");
        e = binary_add(e, index, 0);
        e = (Expr){e.type->base, 1, -1, 0};
    }
    return e;
}
static Expr unary(void) {
    if (take("&")) {
        Expr e = unary();
        if (!e.lvalue) error("address-of requires an object");
        return (Expr){pointer(e.type), 0, -1, 0};
    }
    if (take("*")) {
        Expr e = value(unary());
        if (e.type->kind != TY_PTR) error("dereference requires a pointer");
        if (!e.type->base->size) error("dereference requires a complete object type");
        return (Expr){e.type->base, 1, -1, 0};
    }
    int plus = take("+");
    if (plus || take("-")) {
        Expr e = value(unary());
        if (!integer(e.type)) error("unary arithmetic requires an integer");
        if (!plus) emit("    sub w0, wzr, w0\n");
        return (Expr){&int_type, 0, -1, e.zero};
    }
    return postfix();
}
static Expr additive(void) {
    Expr e = unary();
    while (!strcmp(current()->text, "+") || !strcmp(current()->text, "-")) {
        int subtract = take("-"); if (!subtract) expect("+");
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(unary());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        e = binary_add(e, right, subtract);
    }
    return e;
}

/* Compare w9 (left) with w0 (right). All short TBZ branches stay within
   this fixed-size sequence. Opposite signs are resolved before considering
   subtraction, so signed overflow cannot corrupt the ordering. */
static void comparison(const char *op) {
    size_t id = next_label++;
    if (!strcmp(op, "==") || !strcmp(op, "!=")) {
        emit("    sub w10, w9, w0\n    cbz w10, .Lcmp_yes%zu\n", id);
        int equal = !strcmp(op, "==");
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", !equal);
        emit("    cbz xzr, .Lcmp_end%zu\n.Lcmp_yes%zu:\n", id, id);
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", equal);
    } else {
        int swap = !strcmp(op, ">") || !strcmp(op, "<=");
        int invert = !strcmp(op, ">=") || !strcmp(op, "<=");
        const char *left = swap ? "w0" : "w9";
        const char *right = swap ? "w9" : "w0";
        emit("    sub w10, %s, %s\n", left, right);
        emit("    tbz %s, #31, .Lcmp_positive%zu\n", left, id);
        emit("    tbz %s, #31, .Lcmp_yes%zu\n", right, id);
        emit("    cbz xzr, .Lcmp_same%zu\n.Lcmp_positive%zu:\n", id, id);
        emit("    tbz %s, #31, .Lcmp_same%zu\n", right, id);
        emit("    cbz xzr, .Lcmp_no%zu\n.Lcmp_same%zu:\n", id, id);
        emit("    tbz w10, #31, .Lcmp_no%zu\n.Lcmp_yes%zu:\n", id, id);
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", !invert);
        emit("    cbz xzr, .Lcmp_end%zu\n.Lcmp_no%zu:\n", id, id);
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", invert);
    }
    emit(".Lcmp_end%zu:\n", id);
}

static void typed_comparison(Expr a, Expr b, const char *op) {
    if (integer(a.type) && integer(b.type)) { comparison(op); return; }
    if (strcmp(op, "==") && strcmp(op, "!=")) error("pointer ordering is not supported yet");
    if (!((a.type->kind == TY_PTR && same_type(a.type, b.type)) ||
          (a.type->kind == TY_PTR && integer(b.type) && b.zero) ||
          (b.type->kind == TY_PTR && integer(a.type) && a.zero)))
        error("incompatible pointer comparison");
    size_t id = next_label++;
    int equal = !strcmp(op, "==");
    emit("    sub x10, x9, x0\n    cbz x10, .Lpeq%zu\n", id);
    emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", !equal);
    emit("    cbz xzr, .Lpend%zu\n.Lpeq%zu:\n", id, id);
    emit("    sub w0, w0, w0\n    add w0, w0, #%d\n.Lpend%zu:\n", equal, id);
}
static Expr relational(void) {
    Expr e = additive();
    while (!strcmp(current()->text, "<") || !strcmp(current()->text, "<=") ||
           !strcmp(current()->text, ">") || !strcmp(current()->text, ">=")) {
        const char *op = tokens[pos++].text;
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(additive());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        typed_comparison(e, right, op);
        e = (Expr){&int_type, 0, -1, 0};
    }
    return e;
}
static Expr equality(void) {
    Expr e = relational();
    while (!strcmp(current()->text, "==") || !strcmp(current()->text, "!=")) {
        const char *op = tokens[pos++].text;
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(relational());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        typed_comparison(e, right, op);
        e = (Expr){&int_type, 0, -1, 0};
    }
    return e;
}
static Expr expression(void) {
    Expr e = equality();
    if (take("=")) {
        if (!e.lvalue || e.type->kind == TY_ARRAY)
            error("assignment requires a local variable or dereferenced object on the left");
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = convert(expression(), e.type);
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        store_value(e.type, "x9");
        right.zero = 0; /* An assignment is not an integer constant expression. */
        return right;
    }
    return e;
}

static void statement(void);

static void annotate_statement(void) {
    size_t end = pos;
    int braces = 0;
    while (end < ntokens && strcmp(tokens[end].text, "<eof>")) {
        if (!strcmp(tokens[end].text, ";") && !braces) break;
        if (!strcmp(tokens[end].text, "{")) ++braces;
        if (!strcmp(tokens[end].text, "}")) {
            if (!braces) break;
            --braces;
        }
        ++end;
    }
    if (end < ntokens && !strcmp(tokens[end].text, ";") &&
        tokens[pos].file == tokens[end].file)
        source_comment(body, pos, end);
}

static void block(int function_body) {
    expect("{");
    size_t saved_base = scope_base, saved_locals = nlocals;
    int saved_bytes = frame_bytes;
    scope_base = function_body ? 0 : nlocals;
    while (!take("}")) {
        if (!strcmp(current()->text, "<eof>")) error("expected '}' after block");
        if (type_start() || !strcmp(current()->text, "typedef")) {
            int is_alias = take("typedef");
            annotate_statement();
            Type *type = parse_type();
            if (nlocals == 256) error("too many active local variables (limit: 256)");
            char *s = name();
            int previous = find_local(s);
            type = array_suffix(type, 0);
            if (previous >= 0 && (size_t)previous >= scope_base) {
                if (is_alias && locals[previous].offset == -1 &&
                    same_type(locals[previous].type, type)) {
                    expect(";");
                    continue;
                }
                error("duplicate local variable");
            }
            if (is_alias) {
                if (type->kind == TY_ARRAY && !type->size) error("typedef array requires an explicit size");
                expect(";");
                locals[nlocals++] = (Local){s, -1, type, 1};
                continue;
            }
            if (type->kind == TY_VOID) error("object cannot have void type");
            int initialize = take("=");
            String bytes = {0};
            if (initialize && type->kind == TY_ARRAY && type->base->kind == TY_CHAR &&
                current()->text[0] == '"') {
                bytes = string_bytes();
                if (bytes.length > 4063) error("string initializer is too large");
                if (!type->size) type->size = (int)bytes.length + 1;
                if (bytes.length > (size_t)type->size) error("string initializer is too long for array");
            }
            if (!type->size) error("array requires an explicit size or a string initializer");
            size_t local = nlocals++;
            int offset = allocate(type);
            locals[local] = (Local){s, offset, type, 0};
            if (bytes.data) {
                for (int i = 0; i < type->size; ++i) {
                    literal(i < (int)bytes.length ? bytes.data[i] : 0);
                    emit("    add x9, x29, #%d\n", offset + i);
                    store_value(&char_type, "x9");
                }
                free(bytes.data);
            } else if (initialize && type->kind == TY_ARRAY) {
                expect("{");
                int count = type->size / type->base->size, i = 0;
                do {
                    if (i == count) error("too many array initializers");
                    convert(expression(), type->base);
                    emit("    add x9, x29, #%d\n", offset + i++ * type->base->size);
                    store_value(type->base, "x9");
                    if (!take(",")) break;
                } while (strcmp(current()->text, "}"));
                expect("}");
                for (; i < count; ++i) {
                    emit("    sub x0, x0, x0\n    add x9, x29, #%d\n", offset + i * type->base->size);
                    store_value(type->base, "x9");
                }
            } else if (initialize) {
                convert(expression(), type);
                emit("    add x9, x29, #%d\n", offset);
                store_value(type, "x9");
            }
            expect(";");
            locals[local].initialized = 1;
        } else {
            statement();
        }
    }
    nlocals = saved_locals;
    frame_bytes = saved_bytes;
    scope_base = saved_base;
}

static void condition_comment(size_t start) {
    size_t end = pos;
    int depth = 0;
    if (strcmp(current()->text, "(")) return;
    for (; end < ntokens; ++end) {
        if (!strcmp(tokens[end].text, "(")) ++depth;
        if (!strcmp(tokens[end].text, ")") && --depth == 0) {
            if (tokens[start].file == tokens[end].file)
                source_comment(body, start, end);
            return;
        }
    }
}

static void statement(void) {
    if (!strcmp(current()->text, "{")) { block(0); return; }
    size_t start = pos;
    if (take("if")) {
        size_t id = next_label++;
        condition_comment(start);
        expect("("); Expr cond = value(expression()); expect(")");
        emit("    cbz %s0, .Lelse%zu\n", cond.type->kind == TY_PTR ? "x" : "w", id);
        statement();
        if (take("else")) {
            emit("    cbz xzr, .Lendif%zu\n.Lelse%zu:\n", id, id);
            source_comment(body, pos - 1, pos - 1);
            statement();
            emit(".Lendif%zu:\n", id);
        } else {
            emit(".Lelse%zu:\n", id);
        }
        return;
    }
    if (take("while")) {
        size_t id = next_label++;
        emit(".Lwhile%zu:\n", id);
        condition_comment(start);
        expect("("); Expr cond = value(expression()); expect(")");
        emit("    cbz %s0, .Lendwhile%zu\n", cond.type->kind == TY_PTR ? "x" : "w", id);
        statement();
        emit("    cbz xzr, .Lwhile%zu\n.Lendwhile%zu:\n", id, id);
        return;
    }
    annotate_statement();
    if (take("return")) {
        if (functions[current_function].result->kind == TY_VOID) {
            if (strcmp(current()->text, ";")) error("void function cannot return a value");
        } else {
            if (!strcmp(current()->text, ";")) error("non-void function must return a value");
            convert(expression(), functions[current_function].result);
        }
        expect(";");
        emit("    cbz xzr, .Lreturn%zu\n", current_function);
    } else if (!take(";")) {
        Expr e = expression();
        if (e.type->kind != TY_VOID) value(e);
        expect(";");
    }
}

static void emit_function(size_t start, size_t open, size_t first_constant) {
    Function *fn = &functions[current_function];
    const char *prefix = macos ? "_" : "";
    int frame = (max_frame_bytes + 15) & ~15;
    FILE *out = program;
    fprintf(out, ".text\n.p2align 2\n.globl %s%s\n", prefix, fn->name);
    if (!macos) fprintf(out, ".type %s, %%function\n", fn->name);
    source_comment(out, start, open);
    fprintf(out, "%s%s:\n"
                 "    sub sp, sp, #%d\n"
                 "    str x29, [sp]\n"
                 "    str x30, [sp, #8]\n"
                 "    add x29, sp, #0\n", prefix, fn->name, frame);
    for (int i = 0; i < fn->nparams; ++i)
        fprintf(out, "    %s %s%d, [x29, #%d]\n",
                fn->params[i]->kind == TY_CHAR ? "strb" : "str",
                fn->params[i]->kind == TY_PTR ? "x" : "w", i, 16 + i * 8);
    if (fflush(body) || fseek(body, 0, SEEK_SET)) fatal("cannot rewind assembly buffer");
    int ch;
    while ((ch = fgetc(body)) != EOF) fputc(ch, out);
    if (ferror(body)) fatal("cannot read assembly buffer");
    fprintf(out, "    // Shared function epilogue.\n"
                 ".Lreturn%zu:\n"
                 "    ldr x30, [sp, #8]\n"
                 "    ldr x29, [sp]\n"
                 "    add sp, sp, #%d\n"
                 "    ret\n", current_function, frame);
    if (!macos) fprintf(out, ".size %s, .-%s\n", fn->name, fn->name);
    for (size_t i = first_constant; i < nconstants; ++i)
        fprintf(out, ".LC%zu:\n    .word 0x%08lx\n", i, (unsigned long)constants[i]);
}

static void parse(void) {
    int have_main = 0;
    while (strcmp(current()->text, "<eof>")) {
        size_t start = pos;
        int is_alias = take("typedef");
        Type *result = parse_type();
        char *s = name();
        if (is_alias || strcmp(current()->text, "(")) {
            result = array_suffix(result, 0);
            if (is_alias && result->kind == TY_ARRAY && !result->size)
                error("typedef array requires an explicit size");
            int previous = find_global(s);
            if (find_function(s) >= 0) error("name already declared as a function");
            if (previous >= 0 &&
                ((globals[previous].offset == -1) != is_alias ||
                 !same_type(globals[previous].type, result)))
                error("conflicting global declaration");
            if (!is_alias && (result->kind == TY_VOID || result->kind == TY_ARRAY))
                error("global object must have scalar type");
            int initialized = 0;
            uint32_t initial = 0;
            if (!is_alias && take("=")) {
                initialized = 1;
                int negative = take("-");
                if (!negative) take("+");
                if (current()->text[0] == '\'') {
                    String str = decode_literal();
                    if (str.length != 1) error("character literal must contain one byte");
                    initial = str.data[0];
                    if (macos && initial >= 128) initial -= 256;
                    free(str.data);
                } else initial = (uint32_t)decimal();
                if (negative) initial = 0u - initial;
                if (result->kind == TY_PTR && initial != 0)
                    error("global pointer initializer must be zero");
            }
            expect(";");
            if (previous < 0) {
                if (nglobals == 256) error("too many global declarations");
                previous = (int)nglobals++;
                globals[previous] = (Local){s, is_alias ? -1 : 0, result, 0};
            }
            if (initialized) {
                if (globals[previous].initialized) error("duplicate global definition");
                globals[previous].initialized = 1;
                global_values[previous] = initial;
            }
            continue;
        }
        if (find_global(s) >= 0) error("name already declared as an object or typedef");
        if (result->kind == TY_ARRAY) error("function cannot return an array");
        expect("(");
        char *params[8] = {0};
        Type *param_types[8] = {0};
        int nparams = 0, empty = 0;
        if (take(")")) {
            empty = 1;
        } else if ((alias_type(current()->text) == &void_type || !strcmp(current()->text, "void")) &&
                   !strcmp(tokens[pos + 1].text, ")")) {
            ++pos;
            expect(")");
        } else {
            do {
                if (nparams == 8) error("at most eight scalar parameters are supported");
                param_types[nparams] = parse_type();
                if (identifier(current()->text)) {
                    params[nparams] = name();
                    for (int i = 0; i < nparams; ++i)
                        if (params[i] && !strcmp(params[i], params[nparams]))
                            error("duplicate parameter name");
                }
                param_types[nparams] = array_suffix(param_types[nparams], 1);
                if (param_types[nparams]->kind == TY_VOID) error("parameter cannot have void type");
                if (param_types[nparams]->kind == TY_ARRAY)
                    param_types[nparams] = pointer(param_types[nparams]->base);
                if (params[nparams])
                    locals[nlocals++] = (Local){params[nparams], 0, param_types[nparams], 1};
                ++nparams;
            } while (take(","));
            expect(")");
        }
        nlocals = 0; /* End the prototype parameter scope. */
        int definition = !strcmp(current()->text, "{");
        if (empty && !definition)
            error("use (void) for a zero-parameter prototype");
        if (!strcmp(s, "main") && nparams)
            error("main parameters are not supported yet");
        if (!strcmp(s, "main") && result->kind != TY_INT) error("main must return int");
        int index = find_function(s);
        if (index < 0) {
            if (nfunctions == 256) error("too many function declarations");
            index = (int)nfunctions++;
            functions[index] = (Function){.name=s, .nparams=nparams, .result=result};
            for (int i = 0; i < nparams; ++i) functions[index].params[i] = param_types[i];
        } else if (functions[index].nparams != nparams || !same_type(functions[index].result, result)) {
            error("conflicting function declaration");
        }
        for (int i = 0; i < nparams; ++i)
            if (!same_type(functions[index].params[i], param_types[i])) error("conflicting function declaration");
        if (!definition) { expect(";"); continue; }
        if (functions[index].defined) {
            if (!strcmp(s, "main")) error("duplicate main definition");
            error("duplicate function definition");
        }
        functions[index].defined = 1; /* Visible during its own body: recursion. */
        current_function = (size_t)index;
        nlocals = (size_t)nparams;
        scope_base = 0;
        frame_bytes = max_frame_bytes = 16;
        for (int i = 0; i < nparams; ++i) {
            if (!params[i]) error("function definitions require parameter names");
            locals[i] = (Local){params[i], allocate(param_types[i]), param_types[i], 1};
        }
        body = tmpfile();
        if (!body) fatal("cannot create assembly buffer: %s", strerror(errno));
        body_chars = 0;
        size_t first_constant = nconstants, open = pos;
        block(1);
        if (!strcmp(s, "main")) {
            have_main = 1;
            fprintf(body, "    // Implicit return 0 when main falls through.\n");
        } else {
            fprintf(body, "    // Fallthrough: C does not guarantee a return value here.\n");
        }
        emit("    sub w0, w0, w0\n");
        /* Each function has its own nearby literal pool and return epilogue. */
        if (body_chars > 262144 || nconstants - first_constant > 16384)
            fatal("function exceeds code-size limit");
        emit_function(start, open, first_constant);
        fclose(body);
        nlocals = 0;
    }
    if (!have_main) error("expected an int main() definition");
    for (size_t i = 0; i < nglobals; ++i) {
        Local *g = &globals[i];
        if (g->offset == -1) continue;
        const char *prefix = macos ? "_" : "";
        fprintf(program, ".data\n.p2align %d\n.globl %s%s\n",
                g->type->size == 8 ? 3 : g->type->size == 4 ? 2 : 0, prefix, g->name);
        if (!macos) fprintf(program, ".type %s, %%object\n.size %s, %d\n", g->name, g->name, g->type->size);
        fprintf(program, "%s%s:\n    %s %lu\n", prefix, g->name,
                g->type->kind == TY_PTR ? ".quad" : g->type->kind == TY_CHAR ? ".byte" : ".word",
                (unsigned long)(g->type->kind == TY_CHAR ? global_values[i] & 255 : global_values[i]));
    }
    if (nstrings) {
        fprintf(program, macos ? ".section __TEXT,__const\n" : ".section .rodata\n");
        for (size_t i = 0; i < nstrings; ++i) {
            fprintf(program, "Lstr%zu:\n", i);
            for (size_t j = 0; j <= strings[i].length; ++j)
                fprintf(program, "    .byte %u\n", strings[i].data[j]);
        }
    }
    if (!macos) fprintf(program, ".section .note.GNU-stack,\"\",%%progbits\n");
}

int main(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
#ifdef __APPLE__
    macos = 1;
#endif
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            puts("usage: 4c [--target linux|macos] [-I directory] [-o output.s] input.c");
            return 0;
        }
        if (!strcmp(argv[i], "--target")) {
            if (++i == argc) fatal("--target needs linux or macos");
            if (!strcmp(argv[i], "macos")) macos = 1;
            else if (!strcmp(argv[i], "linux")) macos = 0;
            else fatal("unknown target: %s", argv[i]);
        } else if (!strcmp(argv[i], "-I")) {
            if (++i == argc) fatal("-I needs a directory");
            include_dir = argv[i];
        } else if (!strcmp(argv[i], "-o")) {
            if (++i == argc) fatal("-o needs a path");
            output = argv[i];
        } else if (argv[i][0] == '-') {
            fatal("unknown option: %s", argv[i]);
        } else {
            if (input) fatal("expected exactly one input file");
            input = argv[i];
        }
    }
    if (!input) fatal("no input file (use --help for usage)");
    lex(input, 0);
    program = tmpfile();
    if (!program) fatal("cannot create program buffer: %s", strerror(errno));
    parse();
    FILE *out = output ? fopen(output, "w") : stdout;
    if (!out) fatal("cannot open output %s: %s", output, strerror(errno));
    if (fflush(program) || fseek(program, 0, SEEK_SET)) fatal("cannot rewind program buffer");
    int ch;
    while ((ch = fgetc(program)) != EOF) fputc(ch, out);
    if (ferror(program)) fatal("cannot read program buffer");
    if (fflush(out) || ferror(out)) fatal("cannot write assembly output");
    if (output && fclose(out)) fatal("cannot close assembly output");
    fclose(program);
    return 0;
}
