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

typedef enum { TY_BOOL, TY_CHAR, TY_INT, TY_UINT, TY_LONG, TY_ULONG,
                TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_VOID } TypeKind;
typedef struct Member Member;
typedef struct Type {
    TypeKind kind;
    struct Type *base;
    int size;
    int align;
    Member *members;
    int nmembers;
} Type;
typedef struct Member {
    char *name;
    Type *type;
    int offset;
} Member;
static Type void_type = {TY_VOID, NULL, 0, 1, NULL, 0};
static Type int_type = {TY_INT, NULL, 4, 4, NULL, 0},
            char_type = {TY_CHAR, NULL, 1, 1, NULL, 0};
static Type bool_type = {TY_BOOL, NULL, 1, 1, NULL, 0},
            uint_type = {TY_UINT, NULL, 4, 4, NULL, 0};
static Type long_type = {TY_LONG, NULL, 8, 8, NULL, 0},
            ulong_type = {TY_ULONG, NULL, 8, 8, NULL, 0};
static int alignment(Type *t) {
    switch (t->kind) {
    case TY_PTR: case TY_LONG: case TY_ULONG: return 8;
    case TY_INT: case TY_UINT: return 4;
    case TY_ARRAY: return alignment(t->base);
    case TY_STRUCT: case TY_UNION: return t->align;
    default: return 1; /* TY_BOOL, TY_CHAR, TY_VOID */
    }
}
typedef struct { Type *type; int lvalue, local, zero; } Expr;
typedef struct { unsigned char *data; size_t length; } String;
static String *strings;
static size_t nstrings;
static int frame_bytes, max_frame_bytes;
static int macos;

typedef struct {
    char *name;
    int offset; /* >=0 stack offset; -1 typedef; -2 enum constant */
    Type *type;
    int initialized;
    int value; /* enum constant value when offset == -2 */
} Local;

static Token *tokens;
static size_t ntokens, capacity, pos;
static Local locals[256];
static size_t nlocals, scope_base, next_label;
/* An offset of -1 marks a typedef in the ordinary identifier namespace. */
static Local globals[256];
static uint64_t global_values[256];
static size_t nglobals;
static int find_local(const char *s);
static int find_global(const char *s);
static int find_function(const char *s);
static int decimal(void);
static int find_global(const char *s) {
    for (size_t i = 0; i < nglobals; ++i)
        if (!strcmp(globals[i].name, s)) return (int)i;
    return -1;
}
/* Enum tag names live in their own namespace with block scoping: each
   tag records its nesting depth, and redefinition is an error only in
   the same scope. Lookup prefers the innermost tag. */
typedef struct { char *name; Type *type; int depth; } Tag;
static Tag tags[256];
static size_t ntags;
static int tag_depth;
/* Set while parsing translation-unit declarations; enumerator constants
   defined there use the global namespace instead of a block's locals. */
static int global_scope;
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
           !strcmp(s, "void") || !strcmp(s, "_Bool") ||
           !strcmp(s, "long") || !strcmp(s, "unsigned") ||
           !strcmp(s, "enum") || !strcmp(s, "struct") ||
           !strcmp(s, "union") || alias_type(s) != NULL;
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
        } else if (p[0] == '-' && p[1] == '>') {
            p += 2;
        } else if (*p == '.') {
            ++p;
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
    *t = (Type){kind, base, size, 0, NULL, 0};
    return t;
}
static Type *pointer(Type *base) { return derived(TY_PTR, base, 8); }
static int same_type(Type *a, Type *b) {
    return a->kind == b->kind && a->size == b->size &&
           (!a->base || same_type(a->base, b->base));
}
static void add_member(Type *t, char *name, Type *type) {
    for (int i = 0; i < t->nmembers; ++i)
        if (!strcmp(t->members[i].name, name)) error("duplicate member");
    t->members = resize(t->members, ((size_t)t->nmembers + 1) * sizeof(Member));
    t->members[t->nmembers++] = (Member){name, type, 0};
}
static Member *find_member(Type *t, const char *name) {
    for (int i = 0; i < t->nmembers; ++i)
        if (!strcmp(t->members[i].name, name)) return &t->members[i];
    return NULL;
}
/* Assign offsets, size, and alignment for a struct or union body. */
static void layout_record(Type *t, int is_union) {
    int max_size = 0, max_align = 0, offset = 0;
    for (int i = 0; i < t->nmembers; ++i) {
        Member *m = &t->members[i];
        int a = alignment(m->type);
        if (a > max_align) max_align = a;
        if (is_union) {
            m->offset = 0;
        } else {
            offset = (offset + a - 1) & ~(a - 1);
            m->offset = offset;
            offset += m->type->size;
        }
        if (m->type->size > max_size) max_size = m->type->size;
    }
    t->align = max_align ? max_align : 1;
    t->size = is_union ? max_size : (offset + t->align - 1) & ~(t->align - 1);
}
static int integer(Type *t) {
    switch (t->kind) {
    case TY_BOOL: case TY_CHAR: case TY_INT: case TY_UINT:
    case TY_LONG: case TY_ULONG: return 1;
    default: return 0;
    }
}
/* Values of wide types live in x registers; narrower ones in w registers. */
static int wide(Type *t) {
    return t->kind == TY_PTR || t->kind == TY_LONG || t->kind == TY_ULONG;
}
static int unsigned_type(Type *t) {
    return t->kind == TY_UINT || t->kind == TY_ULONG ||
           (t->kind == TY_CHAR && !macos);
}
/* Usual arithmetic conversions for the supported integer types. */
static Type *common_integer(Type *a, Type *b) {
    if (a->kind == TY_ULONG || b->kind == TY_ULONG) return &ulong_type;
    if (a->kind == TY_LONG || b->kind == TY_LONG) return &long_type;
    if (a->kind == TY_UINT || b->kind == TY_UINT) return &uint_type;
    return &int_type;
}
static Type *promote(Type *t) {
    if (t->kind == TY_BOOL || t->kind == TY_CHAR) return &int_type;
    return t;
}
typedef struct { uint64_t value; Type *type; } IntConst;
static IntConst int_literal(void) {
    const char *s = current()->text;
    if (!isdigit((unsigned char)*s) || (s[0] == '0' && isdigit((unsigned char)s[1])))
        error("expected a decimal integer from 0 to 2147483647");
    int is_long = 0, is_unsigned = 0;
    for (const char *p = s; *p; ++p) {
        if (isdigit((unsigned char)*p)) continue;
        if (*p == 'L' || *p == 'l') {
            if (is_long) error("duplicate integer suffix");
            is_long = 1;
        } else if (*p == 'U' || *p == 'u') {
            if (is_unsigned) error("duplicate integer suffix");
            is_unsigned = 1;
        } else {
            error("expected a decimal integer from 0 to 2147483647");
        }
    }
    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, 10);
    if (end != s + strspn(s, "0123456789") || errno)
        error("expected a decimal integer from 0 to 2147483647");
    if (!is_long && !is_unsigned && n > INT32_MAX)
        error("expected a decimal integer from 0 to 2147483647");
    if (is_long && !is_unsigned && n > INT64_MAX)
        error("integer literal is too large for long");
    if (!is_long && is_unsigned && n > UINT32_MAX)
        error("integer literal is too large for unsigned int");
    ++pos;
    Type *t = is_long ? (is_unsigned ? &ulong_type : &long_type)
                      : (is_unsigned ? &uint_type : &int_type);
    return (IntConst){n, t};
}
static int find_tag(const char *s) {
    for (size_t i = ntags; i > 0; --i)
        if (!strcmp(tags[i - 1].name, s)) return (int)i - 1;
    return -1;
}
static int enum_constant(const char *s, int *value) {
    int i = find_local(s);
    if (i >= 0) {
        if (locals[i].offset != -2) return 0;
        *value = locals[i].value;
        return 1;
    }
    i = find_global(s);
    if (i >= 0 && globals[i].offset == -2) {
        *value = globals[i].value;
        return 1;
    }
    return 0;
}
static void define_enum_constant(char *s, int value) {
    if (global_scope) {
        if (find_global(s) >= 0 || find_function(s) >= 0)
            error("duplicate enumerator");
        if (nglobals == 256) error("too many global declarations");
        globals[nglobals++] = (Local){s, -2, &int_type, 1, value};
    } else {
        if (find_local(s) >= 0) error("duplicate enumerator");
        if (nlocals == 256) error("too many active local variables (limit: 256)");
        locals[nlocals++] = (Local){s, -2, &int_type, 1, value};
    }
}
static int enum_value(void) {
    int negative = 0;
    if (take("-")) negative = 1;
    else take("+");
    int value;
    if (isdigit((unsigned char)*current()->text)) value = decimal();
    else if (identifier(current()->text) && enum_constant(current()->text, &value))
        ++pos;
    else error("expected an enumerator value");
    return negative ? -value : value;
}
/* enum-specifier: 'enum' [tag] ['{' enumerator-list '}']. The type is
   int-compatible; tags are file-scope and cannot be redefined. */
static Type *enum_specifier(void) {
    char *tag = NULL;
    if (identifier(current()->text)) tag = tokens[pos++].text;
    if (!take("{")) {
        if (!tag) error("expected an enumerator list or enum tag");
        if (find_tag(tag) < 0) error("unknown enum tag");
        return &int_type;
    }
    if (tag) {
        int existing = find_tag(tag);
        if (existing >= 0 && tags[existing].depth == tag_depth)
            error("duplicate enum tag");
        if (ntags == 256) error("too many enum tags");
        tags[ntags++] = (Tag){tag, &int_type, tag_depth};
    }
    int value = 0;
    do {
        if (!identifier(current()->text)) error("expected an enumerator name");
        char *s = tokens[pos++].text;
        if (take("=")) value = enum_value();
        define_enum_constant(s, value);
        ++value;
    } while (take(","));
    expect("}");
    return &int_type;
}
/* struct-or-union-specifier: ('struct'|'union') [tag] ['{' members '}'].
   A named tag is registered before its members are parsed so that
   pointers to the record inside its own body (self-reference) work; the
   type node starts incomplete (size 0) and is filled in after layout. */
static Type *parse_specs(void);
static Type *consume_stars(Type *t);
static Type *array_suffix(Type *t, int parameter, int global);
static Type *struct_specifier(int is_union) {
    char *tag = NULL;
    if (identifier(current()->text)) tag = tokens[pos++].text;
    if (!take("{")) {
        if (!tag) error("expected a struct tag or member list");
        int existing = find_tag(tag);
        if (existing < 0) error("unknown struct tag");
        return tags[existing].type;
    }
    Type *t = derived(is_union ? TY_UNION : TY_STRUCT, NULL, 0);
    if (tag) {
        int existing = find_tag(tag);
        if (existing >= 0 && tags[existing].depth == tag_depth)
            error("duplicate struct tag");
        if (ntags == 256) error("too many struct tags");
        tags[ntags++] = (Tag){tag, t, tag_depth};
    }
    while (strcmp(current()->text, "}")) {
        if (!strcmp(current()->text, "<eof>"))
            error("expected '}' after struct body");
        Type *base = parse_specs();
        for (;;) {
            Type *mt = consume_stars(base);
            char *s = name();
            mt = array_suffix(mt, 0, 1);
            if (mt->kind == TY_VOID) error("member cannot have void type");
            if (!mt->size) error("member has incomplete type");
            add_member(t, s, mt);
            if (!take(",")) break;
        }
        expect(";");
    }
    expect("}");
    if (!t->nmembers) error("struct must have at least one member");
    layout_record(t, is_union);
    return t;
}
static Type *parse_specs(void) {
    Type *t;
    if (take("int")) t = &int_type;
    else if (take("char")) t = &char_type;
    else if (take("void")) t = &void_type;
    else if (take("_Bool")) t = &bool_type;
    else if (take("long")) {
        if (take("unsigned")) t = &ulong_type;
        else t = &long_type;
        take("int");
    } else if (take("unsigned")) {
        if (take("long")) t = &ulong_type;
        else t = &uint_type;
        take("int");
    } else if (take("enum")) t = enum_specifier();
    else if (take("struct")) t = struct_specifier(0);
    else if (take("union")) t = struct_specifier(1);
    else if ((t = alias_type(current()->text))) ++pos;
    else { error("expected 'int', 'char', 'void', or a typedef name"); return NULL; }
    return t;
}
static Type *consume_stars(Type *t) {
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
/* Array declarator suffixes: one or more '[n]' brackets. Dimensions nest
   so that the first bracket is the outermost array (C row-major order).
   A missing dimension is allowed only for the outermost bracket, giving
   an incomplete array completed by a string initializer. Parameters decay
   to a pointer to the element type. Globals skip the local frame limit. */
static Type *array_suffix(Type *t, int parameter, int global) {
    int dims[16], ndims = 0;
    while (take("[")) {
        if (!t->size) error("array element type must be complete");
        int n = -1;
        if (strcmp(current()->text, "]")) n = decimal();
        expect("]");
        if (ndims == 16) error("too many array dimensions");
        dims[ndims++] = n;
    }
    Type *result = t;
    if (ndims) {
        if (ndims > 1)
            for (int i = 1; i < ndims; ++i)
                if (dims[i] == -1) error("inner array dimensions require an explicit size");
        for (int i = ndims - 1; i >= 0; --i) {
            int n = dims[i];
            if (n == -1) {
                result = derived(TY_ARRAY, result, 0);
                continue;
            }
            if (n <= 0 || (unsigned long)n * (unsigned long)result->size > 0x7FFFFFFFu)
                error("array size must fit the 4080-byte frame limit");
            if (!global && (unsigned long)n * (unsigned long)result->size > 4064u)
                error("array size must fit the 4080-byte frame limit");
            result = derived(TY_ARRAY, result, n * result->size);
        }
    }
    /* Parameters decay exactly one array level to the element type. */
    if (parameter && result->kind == TY_ARRAY) return pointer(result->base);
    return result;
}
/* Declarator: '*'* [name] ('[' ... ']')? Objects require a name; parameter
   and abstract names may be omitted. */
static Type *parse_declarator(Type *base, char **name_out, int parameter) {
    while (take("*")) {
        if (base->kind == TY_VOID) error("void pointers are not supported yet");
        base = pointer(base);
    }
    char *s = NULL;
    if (identifier(current()->text)) s = tokens[pos++].text;
    *name_out = s;
    return array_suffix(base, parameter, parameter);
}
/* Abstract declarator for casts: specifiers and '*' only. */
static Type *parse_abstract(void) {
    Type *t = parse_specs();
    while (take("*")) {
        if (t->kind == TY_VOID) error("void pointers are not supported yet");
        t = pointer(t);
    }
    if (identifier(current()->text) || !strcmp(current()->text, "["))
        error("declarators in casts are not supported");
    return t;
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

static void pool(uint32_t value) {
    constants = resize(constants, (nconstants + 1) * sizeof(*constants));
    constants[nconstants] = value;
}
static void literal(uint32_t value) {
    pool(value);
    emit("    ldr w0, .LC%zu\n", nconstants++);
}
static void literal_reg(const char *reg, uint32_t value) {
    pool(value);
    emit("    ldr %s, .LC%zu\n", reg, nconstants++);
}
/* Load a 64-bit constant using only 32-bit literal-pool entries and a
   shifted ADD, keeping the pool word-aligned. */
static void literal64(uint64_t value) {
    literal_reg("w0", (uint32_t)(value >> 32));
    emit("    sub x10, x10, x10\n    add x0, x10, x0, lsl #32\n");
    literal_reg("w10", (uint32_t)value);
    emit("    add x0, x0, x10\n");
}
/* Same, into an arbitrary x register other than x10 (x10 is scratch). */
static void literal64_to(const char *xreg, uint64_t value) {
    char wreg[8];
    snprintf(wreg, sizeof(wreg), "w%s", xreg + 1);
    literal_reg(wreg, (uint32_t)(value >> 32));
    emit("    sub x10, x10, x10\n    add %s, x10, %s, lsl #32\n", xreg, xreg);
    literal_reg("w10", (uint32_t)value);
    emit("    add %s, %s, x10\n", xreg, xreg);
}

static Expr expression(void);
static Expr unary(void);
static Expr value(Expr e) {
    if (e.type->kind == TY_VOID) error("void expression has no value");
    if (e.type->kind == TY_ARRAY) {
        e.type = pointer(e.type->base);
    } else if (e.type->kind == TY_STRUCT || e.type->kind == TY_UNION) {
        /* Aggregates decay to their address; no register load. */
    } else if (e.lvalue) {
        if (e.local >= 0 && !locals[e.local].initialized)
            error("unknown local value in self-initializer");
        if (wide(e.type)) emit("    ldr x0, [x0]\n");
        else if (e.type->kind == TY_INT || e.type->kind == TY_UINT)
            emit("    ldr w0, [x0]\n");
        else { /* TY_CHAR and TY_BOOL load as one byte */
            emit("    ldrb w0, [x0]\n");
            if (macos && e.type->kind == TY_CHAR) {
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
/* Normalize any scalar value in x0/w0 to exactly 0 or 1 for _Bool. */
static void normalize_bool(Type *from) {
    size_t id = next_label++;
    emit(wide(from) ? "    cbz x0, .Lbool%zu\n" : "    cbz w0, .Lbool%zu\n", id);
    literal_reg("w0", 1);
    emit("    cbz xzr, .Lbool_end%zu\n.Lbool%zu:\n", id, id);
    emit("    sub w0, w0, w0\n.Lbool_end%zu:\n", id);
}
static void extend_reg(Type *src, const char *wreg, const char *xreg) {
    emit("    sub x10, x10, x10\n");
    emit("    add %s, x10, %s, %s\n", xreg, wreg,
         unsigned_type(src) ? "uxtw" : "sxtw");
}
static Expr convert(Expr e, Type *to) {
    e = value(e);
    if (to->kind == TY_VOID) error("cannot convert to void");
    if (to->kind == TY_PTR) {
        if (!(same_type(e.type, to) || (integer(e.type) && e.zero)))
            error("incompatible pointer conversion");
        return (Expr){to, 0, -1, e.zero};
    }
    if (to->kind == TY_STRUCT || to->kind == TY_UNION) {
        if (!same_type(e.type, to)) error("incompatible aggregate assignment");
        return (Expr){to, 0, -1, 0};
    }
    if (e.type->kind == TY_STRUCT || e.type->kind == TY_UNION)
        error("cannot convert aggregate to a scalar");
    if (to->kind == TY_BOOL) {
        if (!integer(e.type) && e.type->kind != TY_PTR)
            error("cannot convert to _Bool");
        if (e.type->kind != TY_BOOL) normalize_bool(e.type);
        return (Expr){to, 0, -1, e.zero};
    }
    if (!integer(e.type)) error("cannot convert pointer to integer");
    if (to->kind == TY_CHAR) narrow_char();
    else if (to->kind == TY_LONG || to->kind == TY_ULONG) {
        if (!wide(e.type)) extend_reg(e.type, "w0", "x0");
    }
    /* 32-bit int/uint targets need no emission: w-register forms ignore
       the upper half, and char/bool bytes are already 0-extended. */
    return (Expr){to, 0, -1, e.zero};
}
/* Copy an aggregate byte by byte from the address in x0 to the address
   in the given register (x9 at assignment sites). Uses only register
   offset addressing and the allowed mnemonics. */
static void copy_object(Type *t, const char *dest, const char *src) {
    int size = t->size;
    if (!size) return;
    size_t id = next_label++;
    emit("    sub x11, x11, x11\n");
    emit(".Lcopy%zu:\n", id);
    emit("    ldrb w10, [%s, x11]\n", src);
    emit("    strb w10, [%s, x11]\n", dest);
    emit("    add x11, x11, #1\n");
    emit("    sub x10, x11, #%d\n", size);
    emit("    cbz x10, .Lcopy_end%zu\n", id);
    emit("    cbz xzr, .Lcopy%zu\n", id);
    emit(".Lcopy_end%zu:\n", id);
}
static void store_value(Type *t, const char *address) {
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        copy_object(t, address, "x0");
        return;
    }
    emit("    %s %s0, [%s]\n",
         t->kind == TY_CHAR || t->kind == TY_BOOL ? "strb" : "str",
         wide(t) ? "x" : "w", address);
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
    if (!strcmp(current()->text, "sizeof")) {
        ++pos;
        expect("(");
        if (!type_start()) error("sizeof requires a type in parentheses");
        Type *t = parse_abstract();
        expect(")");
        /* ldr w0 zero-extends into x0, matching size_t semantics. */
        literal((uint32_t)t->size);
        return (Expr){&ulong_type, 0, -1, t->size == 0};
    }
    if (take("(")) {
        if (type_start()) {
            Type *to = parse_abstract();
            expect(")");
            return convert(unary(), to);
        }
        Expr e = expression();
        expect(")");
        return e;
    }
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
        IntConst c = int_literal();
        if (wide(c.type)) literal64(c.value);
        else literal((uint32_t)c.value);
        return (Expr){c.type, 0, -1, c.value == 0};
    }
    if (!identifier(current()->text)) error("expected an integer expression or object");
    char *s = current()->text;
    int local = find_local(s);
    int global = find_global(s);
    if ((local >= 0 && locals[local].offset == -1) ||
        (local < 0 && global >= 0 && globals[global].offset == -1))
        error("typedef name is not an expression");
    if (local >= 0 && locals[local].offset == -2) {
        ++pos;
        literal((uint32_t)locals[local].value);
        return (Expr){&int_type, 0, -1, locals[local].value == 0};
    }
    if (local < 0 && global >= 0 && globals[global].offset == -2) {
        ++pos;
        literal((uint32_t)globals[global].value);
        return (Expr){&int_type, 0, -1, globals[global].value == 0};
    }
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
            emit("    ldr %s%d, [sp, #%d]\n", wide(fn->params[i]) ? "x" : "w", i, i * 16);
        if (expected) emit("    add sp, sp, #%d\n", expected * 16);
        emit("    bl %s%s\n", macos ? "_" : "", s);
        if (fn->result->kind == TY_CHAR) narrow_char();
        else if (fn->result->kind == TY_BOOL) normalize_bool(&int_type);
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
/* Scale an integer index into a byte offset without new mnemonics.
   The index arrives in w0 (narrow) or x0 (long); wide indices are kept. */
static void scale_index(int size, Type *index) {
    emit("    sub x10, x10, x10\n");
    if (wide(index)) emit("    sub x0, x0, xzr\n");
    else emit("    add x0, x10, w0, %s\n", unsigned_type(index) ? "uxtw" : "sxtw");
    if (size == 1) return;
    for (int bit = 0; size; ++bit, size >>= 1)
        if (size & 1) emit("    add x10, x10, x0, lsl #%d\n", bit);
    emit("    sub x0, x10, xzr\n");
}
static Expr binary_add(Expr left, Expr right, int subtract) {
    if (left.type->kind == TY_PTR && integer(right.type)) {
        if (!left.type->base->size) error("pointer arithmetic requires a complete object type");
        scale_index(left.type->base->size, right.type);
        emit("    %s x0, x9, x0\n", subtract ? "sub" : "add");
        return (Expr){left.type, 0, -1, 0};
    }
    if (!subtract && integer(left.type) && right.type->kind == TY_PTR) {
        emit("    sub x11, x0, xzr\n    sub x0, x9, xzr\n    sub x9, x11, xzr\n");
        return binary_add(right, left, 0);
    }
    if (!integer(left.type) || !integer(right.type)) error("unsupported pointer arithmetic");
    Type *t = common_integer(left.type, right.type);
    if (wide(t)) {
        if (!wide(left.type)) extend_reg(left.type, "w9", "x9");
        if (!wide(right.type)) extend_reg(right.type, "w0", "x0");
        emit("    %s x0, x9, x0\n", subtract ? "sub" : "add");
    } else {
        emit("    %s w0, w9, w0\n", subtract ? "sub" : "add");
    }
    return (Expr){t, 0, -1, 0};
}
static Expr postfix(void) {
    Expr e = primary();
    for (;;) {
        if (take("[")) {
            e = value(e);
            emit("    sub sp, sp, #16\n    str x0, [sp]\n");
            Expr index = value(expression());
            expect("]");
            emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
            if (!(e.type->kind == TY_PTR || index.type->kind == TY_PTR))
                error("indexing requires a pointer or array");
            e = binary_add(e, index, 0);
            e = (Expr){e.type->base, 1, -1, 0};
            continue;
        }
        int arrow = take("->");
        if (!arrow && !take(".")) break;
        if (arrow) {
            e = value(e);
            if (e.type->kind != TY_PTR ||
                (e.type->base->kind != TY_STRUCT && e.type->base->kind != TY_UNION))
                error("arrow access requires a pointer to a struct or union");
            e.type = e.type->base;
        } else {
            if (!e.lvalue ||
                (e.type->kind != TY_STRUCT && e.type->kind != TY_UNION))
                error("member access requires a struct or union object");
        }
        char *m = name();
        Member *mm = find_member(e.type, m);
        if (!mm) error("unknown member");
        if (mm->offset > 4095) error("member offset exceeds the 4095 immediate limit");
        emit("    add x0, x0, #%d\n", mm->offset);
        e = (Expr){mm->type, 1, -1, 0};
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
        Type *t = promote(e.type);
        if (!plus) emit(wide(t) ? "    sub x0, xzr, x0\n" : "    sub w0, wzr, w0\n");
        return (Expr){t, 0, -1, e.zero};
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

/* Compare the left operand in w9/x9 with the right operand in w0/x0 at
   the width and signedness of the common type. All short TBZ branches
   stay within this fixed-size sequence. Opposite signs are resolved
   before considering subtraction, so signed overflow cannot corrupt the
   ordering. Unsigned operands are pre-adjusted by adding 2^31 (or 2^63),
   which maps unsigned order onto signed order. */
static void comparison(Type *t, const char *op) {
    size_t id = next_label++;
    int w = !wide(t);
    int bit = w ? 31 : 63;
    const char *l = w ? "w9" : "x9";
    const char *r = w ? "w0" : "x0";
    const char *s = w ? "w10" : "x10";
    if (!strcmp(op, "==") || !strcmp(op, "!=")) {
        emit("    sub %s, %s, %s\n    cbz %s, .Lcmp_yes%zu\n", s, l, r, s, id);
        int equal = !strcmp(op, "==");
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", !equal);
        emit("    cbz xzr, .Lcmp_end%zu\n.Lcmp_yes%zu:\n", id, id);
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", equal);
    } else {
        int swap = !strcmp(op, ">") || !strcmp(op, "<=");
        int invert = !strcmp(op, ">=") || !strcmp(op, "<=");
        const char *left = swap ? r : l;
        const char *right = swap ? l : r;
        if (t->kind == TY_UINT || t->kind == TY_ULONG) {
            if (w) {
                literal_reg("w10", 0x80000000u);
                emit("    add %s, %s, w10\n    add %s, %s, w10\n", l, l, r, r);
            } else {
                literal64_to("x11", 0x8000000000000000ull);
                emit("    add %s, %s, x11\n    add %s, %s, x11\n", l, l, r, r);
            }
        }
        emit("    sub %s, %s, %s\n", s, left, right);
        emit("    tbz %s, #%d, .Lcmp_positive%zu\n", left, bit, id);
        emit("    tbz %s, #%d, .Lcmp_yes%zu\n", right, bit, id);
        emit("    cbz xzr, .Lcmp_same%zu\n.Lcmp_positive%zu:\n", id, id);
        emit("    tbz %s, #%d, .Lcmp_same%zu\n", right, bit, id);
        emit("    cbz xzr, .Lcmp_no%zu\n.Lcmp_same%zu:\n", id, id);
        emit("    tbz %s, #%d, .Lcmp_no%zu\n.Lcmp_yes%zu:\n", s, bit, id, id);
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", !invert);
        emit("    cbz xzr, .Lcmp_end%zu\n.Lcmp_no%zu:\n", id, id);
        emit("    sub w0, w0, w0\n    add w0, w0, #%d\n", invert);
    }
    emit(".Lcmp_end%zu:\n", id);
}

static void typed_comparison(Expr a, Expr b, const char *op) {
    if (integer(a.type) && integer(b.type)) {
        comparison(common_integer(a.type, b.type), op);
        return;
    }
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
    size_t saved_base = scope_base, saved_locals = nlocals, saved_tags = ntags;
    int saved_bytes = frame_bytes;
    scope_base = function_body ? 0 : nlocals;
    ++tag_depth;
    while (!take("}")) {
        if (!strcmp(current()->text, "<eof>")) error("expected '}' after block");
        if (type_start() || !strcmp(current()->text, "typedef")) {
            int is_alias = take("typedef");
            annotate_statement();
            Type *base = parse_specs();
            if (!strcmp(current()->text, ";")) {
                expect(";");
                continue;
            }
            for (;;) {
                Type *type = consume_stars(base);
                if (nlocals == 256) error("too many active local variables (limit: 256)");
                char *s = name();
                int previous = find_local(s);
                type = array_suffix(type, 0, 0);
                if (previous >= 0 && (size_t)previous >= scope_base) {
                    if (is_alias && locals[previous].offset == -1 &&
                        same_type(locals[previous].type, type) &&
                        !strcmp(current()->text, ";"))
                        break;
                    error("duplicate local variable");
                }
                if (is_alias) {
                    if (type->kind == TY_ARRAY && !type->size)
                        error("typedef array requires an explicit size");
                    locals[nlocals++] = (Local){s, -1, type, 1, 0};
                } else {
                    if (type->kind == TY_VOID) error("object cannot have void type");
                    if (type->kind == TY_STRUCT || type->kind == TY_UNION)
                        if (!type->size) error("object has incomplete type");
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
                    locals[local] = (Local){s, offset, type, 0, 0};
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
                    locals[local].initialized = 1;
                }
                if (!take(",")) break;
            }
            expect(";");
        } else {
            statement();
        }
    }
    nlocals = saved_locals;
    frame_bytes = saved_bytes;
    scope_base = saved_base;
    ntags = saved_tags;
    --tag_depth;
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
        emit("    cbz %s0, .Lelse%zu\n", wide(cond.type) ? "x" : "w", id);
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
        emit("    cbz %s0, .Lendwhile%zu\n", wide(cond.type) ? "x" : "w", id);
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
                fn->params[i]->kind == TY_CHAR || fn->params[i]->kind == TY_BOOL ? "strb" : "str",
                wide(fn->params[i]) ? "x" : "w", i, 16 + i * 8);
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
    global_scope = 1;
    while (strcmp(current()->text, "<eof>")) {
        size_t start = pos;
        int is_alias = take("typedef");
        Type *base = parse_specs();
        Type *result = consume_stars(base);
        if (!strcmp(current()->text, ";")) {
            /* Bare specifier: a structurally complete enum tag definition. */
            expect(";");
            continue;
        }
        char *s = name();
        if (!is_alias && !strcmp(current()->text, "(")) {
            if (find_global(s) >= 0) error("name already declared as an object or typedef");
            if (result->kind == TY_ARRAY) error("function cannot return an array");
            if (result->kind == TY_STRUCT || result->kind == TY_UNION)
                error("aggregate results are not supported yet");
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
                    Type *param_base = parse_specs();
                    param_types[nparams] = parse_declarator(param_base, &params[nparams], 1);
                    for (int i = 0; i < nparams; ++i)
                        if (params[i] && params[nparams] && !strcmp(params[i], params[nparams]))
                            error("duplicate parameter name");
                    if (param_types[nparams]->kind == TY_VOID) error("parameter cannot have void type");
                    if (param_types[nparams]->kind == TY_STRUCT ||
                        param_types[nparams]->kind == TY_UNION)
                        error("aggregate parameters are not supported yet");
                    if (params[nparams])
                        locals[nlocals++] = (Local){params[nparams], 0, param_types[nparams], 1, 0};
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
            global_scope = 0;
            for (int i = 0; i < nparams; ++i) {
                if (!params[i]) error("function definitions require parameter names");
                locals[i] = (Local){params[i], allocate(param_types[i]), param_types[i], 1, 0};
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
            continue;
        }
        /* Object or typedef declaration with comma-separated declarators. */
        for (;;) {
            Type *type = array_suffix(result, 0, 1);
            if (is_alias && type->kind == TY_ARRAY && !type->size)
                error("typedef array requires an explicit size");
            int previous = find_global(s);
            if (find_function(s) >= 0) error("name already declared as a function");
            if (previous >= 0 &&
                ((globals[previous].offset == -1) != is_alias ||
                 !same_type(globals[previous].type, type)))
                error("conflicting global declaration");
            if (!is_alias && type->kind == TY_VOID)
                error("global object must have scalar type");
            if (type->kind == TY_ARRAY || type->kind == TY_STRUCT || type->kind == TY_UNION)
                if (take("=")) error("global aggregate initializers are not supported yet");
            int initialized = 0;
            uint64_t initial = 0;
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
                } else {
                    IntConst c = int_literal();
                    initial = c.value;
                }
                if (negative) initial = 0 - initial;
                if (type->kind == TY_PTR && initial != 0)
                    error("global pointer initializer must be zero");
            }
            if (previous < 0) {
                if (nglobals == 256) error("too many global declarations");
                previous = (int)nglobals++;
                globals[previous] = (Local){s, is_alias ? -1 : 0, type, 0, 0};
            }
            if (initialized) {
                if (globals[previous].initialized) error("duplicate global definition");
                globals[previous].initialized = 1;
                global_values[previous] = initial;
            }
            if (!take(",")) break;
            result = consume_stars(base);
            s = name();
        }
        expect(";");
    }
    if (!have_main) error("expected an int main() definition");
    for (size_t i = 0; i < nglobals; ++i) {
        Local *g = &globals[i];
        if (g->offset < 0) continue; /* typedefs and enum constants */
        const char *prefix = macos ? "_" : "";
        int a = alignment(g->type), p2 = 0;
        while ((1 << p2) < a) ++p2;
        fprintf(program, ".data\n.p2align %d\n.globl %s%s\n", p2, prefix, g->name);
        if (!macos) fprintf(program, ".type %s, %%object\n.size %s, %d\n", g->name, g->name, g->type->size);
        fprintf(program, "%s%s:\n", prefix, g->name);
        if (g->type->kind == TY_ARRAY || g->type->kind == TY_STRUCT ||
            g->type->kind == TY_UNION) {
            fprintf(program, "    .zero %d\n", g->type->size);
        } else {
            fprintf(program, "    %s %llu\n",
                    g->type->kind == TY_PTR || g->type->kind == TY_LONG ||
                            g->type->kind == TY_ULONG
                        ? ".quad"
                        : g->type->kind == TY_CHAR ? ".byte" : ".word",
                    (unsigned long long)(g->type->kind == TY_CHAR
                                             ? global_values[i] & 255
                                             : global_values[i]));
        }
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
