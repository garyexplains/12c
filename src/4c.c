#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "softfloat.h"

_Static_assert(sizeof(double) == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 &&
               DBL_MAX_EXP == 1024, "4c requires a binary64 host double");

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
    uint64_t hidden_macros[2]; /* Macros disabled while rescanning this token. */
    int space_before; /* Preserve whitespace through macro argument prescan. */
} Token;

typedef enum { TY_BOOL, TY_CHAR, TY_INT, TY_UINT, TY_LONG, TY_ULONG, TY_DOUBLE,
                TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_VOID, TY_SCHAR, TY_UCHAR, TY_SHORT, TY_USHORT } TypeKind;
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
static Type double_type = {TY_DOUBLE, NULL, 8, 8, NULL, 0};
static Type schar_type = {TY_SCHAR, NULL, 1, 1, NULL, 0};
static Type uchar_type = {TY_UCHAR, NULL, 1, 1, NULL, 0};
static Type short_type = {TY_SHORT, NULL, 2, 2, NULL, 0};
static Type ushort_type = {TY_USHORT, NULL, 2, 2, NULL, 0};
static int need_softfloat, compiling_softfloat;
static int alignment(Type *t) {
    switch (t->kind) {
    case TY_PTR: case TY_LONG: case TY_ULONG: case TY_DOUBLE: return 8;
    case TY_INT: case TY_UINT: return 4;
    case TY_SHORT: case TY_USHORT: return 2;
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
static int suppress_emit; /* _Static_assert: drop assembly from constant expression. */

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
static int global_static[256];
typedef struct { uint64_t value; Type *type; int is_string; } InitEntry;
static InitEntry *global_inits[256];
static size_t global_init_counts[256];
static size_t nglobals;
static int find_local(const char *s);
static int find_global(const char *s);
static int find_function(const char *s);
static int decimal(void);
static void comparison(Type *t, const char *op);
static int identifier(const char *s);
static Type *parse_abstract(void);
static int const_expr_or(uint64_t *out);
static int enum_constant(const char *name, int *value);
static String decode_literal(void);
static String string_bytes(void);
static void fatal(const char *fmt, ...);
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
/* Token-level macro table: object-like and parameterized function-like
   macros, expanded with rescanning after lexing. Function-like macros
   record their parameter names and a variadic flag (last param is ...). */
typedef struct {
    char *name;
    Token *body;
    size_t nbody;
    int nparams;
    char *params[16];
    int variadic;
    int function_like;
} Macro;
static Macro macros[128];
static size_t nmacros;
/* case-label records for the innermost switch, per nesting level. */
#define MAX_CASES 64
typedef struct { size_t label; int value; } CaseLabel;
static size_t switch_ids[8];
static CaseLabel switch_cases[8][MAX_CASES];
static int switch_case_count[8];
static int switch_has_default[8];
static int switch_depth;
/* break targets: each loop or switch pushes its end label name. */
static char break_names[32][32];
static int break_depth;
/* continue targets: each loop pushes a label that re-enters iteration. */
static char continue_names[32][32];
static int continue_depth;
static void fatal(const char *fmt, ...);
static void push_break(const char *kind, size_t id) {
    if (break_depth == 32) fatal("break nesting is too deep");
    snprintf(break_names[break_depth], sizeof(break_names[0]), kind, id);
    ++break_depth;
}
static void push_continue(const char *kind, size_t id) {
    if (continue_depth == 32) fatal("continue nesting is too deep");
    snprintf(continue_names[continue_depth], sizeof(continue_names[0]), kind, id);
    ++continue_depth;
}
static void pop_break(void) { --break_depth; }
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
           !strcmp(s, "long") || !strcmp(s, "short") || !strcmp(s, "signed") || !strcmp(s, "unsigned") ||
           !strcmp(s, "enum") || !strcmp(s, "struct") ||
           !strcmp(s, "union") || !strcmp(s, "const") || !strcmp(s, "double") || alias_type(s) != NULL;
}
typedef struct {
    char *name;
    int nparams;
    int variadic;
    int defined;
    int internal;
    int static_linkage;
    int va_save_offset;   /* offset of x0..x7 save area from x29 (variadic only) */
    int fp_save_offset;   /* offset of d0..d7 save area from x29 (variadic only) */
    int result_ptr_offset; /* frame slot for the x8 indirect result (big aggregates) */
    Type *result;
    Type *params[8];
} Function;

static Function functions[288]; /* User limit plus private software routines. */
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
    int space = ntokens && (tokens[ntokens-1].file != file ||
                tokens[ntokens-1].source + strlen(tokens[ntokens-1].text) != s);
    tokens[ntokens++] = (Token){copy(s, len), file, line, s, {0, 0}, space};
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
static void lex_source(const char *path, char *data, int depth) {
    if (depth > 8) fatal("%s: include nesting too deep", path);
    char *p = data;
    int line = 1;
    while (*p) {
        if (*p == '\n') { ++line; ++p; continue; }
        if (isspace((unsigned char)*p)) { ++p; continue; }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') ++p;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            int start = line;
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') { ++line; }
                ++p;
            }
            if (!*p) fatal("%s:%d: unterminated comment", path, start);
            p += 2;
            continue;
        }
        /* '#' may appear inside a macro body as the stringification operator. */
        char *start = p;
        if (p[0] == '#' && p[1] == '#') {
            p += 2;
        } else if (*p == '"' || *p == '\'') {
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
        } else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            do {
                ++p;
                if ((*p == '+' || *p == '-') && (p[-1] == 'e' || p[-1] == 'E')) ++p;
            } while (isalnum((unsigned char)*p) || *p == '_' || *p == '.');
        } else if ((p[0] == '<' || p[0] == '>') && p[1] == p[0]) {
            p += 2;
            if (*p == '=') ++p;
        } else if ((strchr("=!<>", p[0]) && p[1] == '=')) {
            p += 2;
        } else if (p[0] == '-' && p[1] == '>') {
            p += 2;
        } else if (p[0] == '&' && p[1] == '&') {
            p += 2;
        } else if (p[0] == '|' && p[1] == '|') {
            p += 2;
        } else if (p[0] == '.' && p[1] == '.' && p[2] == '.') {
            p += 3;
        } else if (strchr("+-*/%&|^", p[0]) && p[1] == '=') {
            p += 2;
        } else if ((p[0] == '+' && p[1] == '+') ||
                   (p[0] == '-' && p[1] == '-')) {
            p += 2;
        } else if (*p == '#' && p[1] == '#') {
            /* Token-paste operator in macro bodies. */
            p += 2;
        } else if (*p == '.') {
            ++p;
        } else if (strchr("(){}[];=,+-<>*&/!|:#?%~^", *p)) {
            ++p;
        } else {
            fatal("%s:%d: unsupported character '%c'", path, line, *p);
        }
        size_t len = 0;
        for (const char *q = start; q != p && *q; ++q) ++len;
        token(start, len, path, line);
    }
    if (!depth) token("<eof>", 5, path, line);
    /* Tokens retain source spans for assembly comments until compilation ends. */
}

static void lex(const char *path, int depth) {
    lex_source(path, read_file(path), depth);
}

static void splice_tokens(size_t at, size_t consumed, const Token *newtoks, size_t nnew) {
    Token *result;
    size_t tail = ntokens - at - consumed;
    size_t total = at + nnew + tail;
    result = resize(NULL, (total ? total : 1) * sizeof(Token));
    if (at) memcpy(result, tokens, at * sizeof(Token));
    if (nnew) memcpy(result + at, newtoks, nnew * sizeof(Token));
    if (tail) memcpy(result + at + nnew, tokens + at + consumed, tail * sizeof(Token));
    free(tokens);
    tokens = result;
    capacity = total ? total : 1;
    ntokens = total;
}
static int macro_defined(const char *name) {
    if (!strcmp(name, "__APPLE__")) return macos;
    if (!strcmp(name, "__linux__")) return !macos;
    for (size_t k = 0; k < nmacros; ++k)
        if (!strcmp(macros[k].name, name)) return 1;
    return 0;
}
/* Stringify token spellings, not source spans: expanded arguments can contain
   tokens originating in several different macro definitions. */
static char *stringify_argument(Token **args, size_t *lengths, int first, int end) {
    size_t capacity = 3;
    for (int p = first; p < end; ++p) {
        capacity += 2;
        for (size_t q = 0; q < lengths[p]; ++q) capacity += 2 * strlen(args[p][q].text) + 1;
    }
    char *text = resize(NULL, capacity);
    size_t used = 0;
    text[used++] = '"';
    for (int p = first; p < end; ++p) {
        for (int q = p > first ? -1 : 0; q < (int)lengths[p]; ++q) {
            Token *t = &args[p][q];
            if (used > 1 && t->space_before) text[used++] = ' ';
            for (const char *r = t->text; *r; ++r) {
                if (*r == '"' || *r == '\\') text[used++] = '\\';
                text[used++] = *r;
            }
        }
    }
    text[used++] = '"';
    text[used] = 0;
    return text;
}
static void preprocess(void);
static int argument_expansion_depth;
/* Expand arguments before disabling the outer macro. Keep the original tokens
   for # and ##, which deliberately suppress this prescan. */
static Token *expanded_argument(Token *argument, size_t length, size_t *result_length) {
    if (++argument_expansion_depth > 128) fatal("macro argument nesting limit exceeded");
    Token *saved = tokens;
    size_t saved_count = ntokens, saved_capacity = capacity;
    tokens = resize(NULL, (length + 1) * sizeof(Token));
    for (size_t i = 0; i < length; ++i) tokens[i] = argument[i];
    tokens[length] = (Token){"<eof>", "<macro argument>", 1, "", {0, 0}, 0};
    ntokens = length + 1; capacity = ntokens;
    preprocess();
    Token *result = tokens;
    *result_length = ntokens - 1;
    tokens = saved; ntokens = saved_count; capacity = saved_capacity;
    --argument_expansion_depth;
    return result;
}
/* Expand in source order. Replacement tokens inherit a hide set, preventing
   self/mutual recursion while still allowing rescanning of nested macros. */
static void preprocess(void) {
    size_t expansions = 0;
    int depth = 0, active = 1;
    int parent[128], selected[128], saw_else[128];
    const char *conditional_file[128];
    for (size_t i = 0; i < ntokens; ) {
        if (strcmp(tokens[i].text, "#")) {
            if (!active && strcmp(tokens[i].text, "<eof>")) {
                size_t end = i + 1;
                while (end < ntokens && strcmp(tokens[end].text, "#") &&
                       strcmp(tokens[end].text, "<eof>")) ++end;
                splice_tokens(i, end - i, NULL, 0);
                continue;
            }
            size_t k;
            for (k = 0; k < nmacros; ++k)
                if (!strcmp(macros[k].name, tokens[i].text) &&
                    !(tokens[i].hidden_macros[k / 64] & (UINT64_C(1) << (k % 64))))
                    break;
            if (k == nmacros) { ++i; continue; }
            Macro *m = &macros[k];
            size_t consumed = 1;
            /* Parse function-like argument list if this macro has parameters. */
            Token **args = NULL;
            size_t *args_len = NULL;
            int nargs = 0;
            /* Zero-arg function-like: require "()" right after name. */
            if (m->function_like && !m->nparams && !m->variadic) {
                if (i + 2 >= ntokens || strcmp(tokens[i + 1].text, "(") ||
                    strcmp(tokens[i + 2].text, ")")) {
                    ++i; continue;
                }
                consumed = 3;
            } else if (m->nparams || m->variadic) {
                if (i + 1 >= ntokens || strcmp(tokens[i + 1].text, "(")) { ++i; continue; }
                /* Collect argument tokens by splitting at top-level commas. The
                   opening paren has already been consumed; depth=1 inside it. */
                int depth = 1;
                size_t start = i + 2;
                size_t j;
                for (j = start; j < ntokens; ++j) {
                    if (!strcmp(tokens[j].text, "(")) ++depth;
                    else if (!strcmp(tokens[j].text, ")")) { --depth; if (!depth) break; }
                    else if (!strcmp(tokens[j].text, ",") && depth == 1) ++nargs;
                }
                if (j == ntokens) fatal("%s:%d: macro argument list is not closed",
                                        tokens[i].file, tokens[i].line);
                nargs = nargs + 1;
                /* Re-walk to capture each arg's token range. */
                args = resize(NULL, (nargs ? nargs : 1) * sizeof(Token *));
                args_len = resize(NULL, (nargs ? nargs : 1) * sizeof(size_t));
                depth = 1;
                int idx = 0;
                size_t a = start;
                for (j = start; j < ntokens; ++j) {
                    if (!strcmp(tokens[j].text, "(")) { ++depth; continue; }
                    if (!strcmp(tokens[j].text, ")")) { --depth; if (!depth) break; }
                    if (!strcmp(tokens[j].text, ",") && depth == 1) {
                        args[idx] = &tokens[a];
                        args_len[idx] = (size_t)(j - a);
                        ++idx;
                        a = j + 1;
                    }
                }
                args[idx] = &tokens[a];
                args_len[idx] = (size_t)(j - a);
                ++idx;
                consumed = j - i + 1;
                int expected = m->nparams + (m->variadic ? 1 : 0);
                if (m->variadic) {
                    if (idx < m->nparams)
                        fatal("%s:%d: too few macro arguments", tokens[i].file, tokens[i].line);
                } else if (idx != expected) {
                    fatal("%s:%d: wrong number of macro arguments",
                          tokens[i].file, tokens[i].line);
                }
            }
            if (++expansions > 10000 || m->nbody > 100000 ||
                ntokens - consumed > 100000 - m->nbody)
                fatal("%s:%d: macro expansion limit exceeded", tokens[i].file, tokens[i].line);
            /* Build the replacement token stream, substituting parameters and
               handling #x stringification and __VA_ARGS__. */
            Token **expanded = resize(NULL, (nargs + 1) * sizeof(Token *));
            size_t *expanded_len = resize(NULL, (nargs + 1) * sizeof(size_t));
            for (int p = 0; p < nargs; ++p) { expanded[p] = NULL; expanded_len[p] = 0; }
            for (size_t j = 0; j < m->nbody; ++j) {
                if ((j && (!strcmp(m->body[j-1].text, "#") || !strcmp(m->body[j-1].text, "##"))) ||
                    (j + 1 < m->nbody && !strcmp(m->body[j+1].text, "##"))) continue;
                for (int p = 0; p < nargs; ++p) {
                    const char *param = p < m->nparams ? m->params[p] : "__VA_ARGS__";
                    if (!strcmp(param, m->body[j].text) && !expanded[p])
                        expanded[p] = expanded_argument(args[p], args_len[p], &expanded_len[p]);
                }
            }
            size_t argument_tokens = 1;
            for (int p = 0; p < nargs; ++p)
                argument_tokens += args_len[p] + expanded_len[p] + 1;
            if (m->nbody && argument_tokens > 100000 / m->nbody)
                fatal("macro replacement exceeds token limit");
            size_t replacement_capacity = m->nbody ? m->nbody * argument_tokens : 1;
            Token *replacement = resize(NULL, replacement_capacity * sizeof(Token));
            size_t rlen = 0;
            int actual_args = nargs;
            for (size_t j = 0; j < m->nbody; ++j) {
                Token *t = &m->body[j];
                /* #x stringification: # followed by a parameter token. */
                if (!strcmp(t->text, "#") && j + 1 < m->nbody) {
                    Token *nxt = &m->body[j + 1];
                    int p = -1;
                    if (!strcmp(nxt->text, "__VA_ARGS__")) {
                        p = m->nparams; /* first variadic slice */
                    } else {
                        for (int pi = 0; pi < m->nparams; ++pi)
                            if (!strcmp(nxt->text, m->params[pi])) { p = pi; break; }
                    }
                    if (p >= 0 && p < actual_args) {
                        int last = !strcmp(nxt->text, "__VA_ARGS__") && m->variadic ? nargs : p + 1;
                        char *lit = stringify_argument(args, args_len, p, last);
                        replacement[rlen] = *nxt;
                        replacement[rlen].text = lit;
                        replacement[rlen].source = lit;
                        for (int word = 0; word < 2; ++word)
                            replacement[rlen].hidden_macros[word] |= tokens[i].hidden_macros[word];
                        replacement[rlen].hidden_macros[k / 64] |= UINT64_C(1) << (k % 64);
                        ++rlen;
                        ++j; /* skip the parameter token */
                        continue;
                    }
                }
                /* __VA_ARGS__ expands to every variadic slice, rejoining
                   them with comma tokens so substitution preserves them. */
                if (m->variadic && !strcmp(t->text, "__VA_ARGS__")) {
                    for (int s = m->nparams; s < nargs; ++s) {
                        if (s > m->nparams) {
                            replacement[rlen] = *t; /* comma separator */
                            replacement[rlen].text = copy(",", 1);
                            for (int word = 0; word < 2; ++word)
                                replacement[rlen].hidden_macros[word] |= tokens[i].hidden_macros[word];
                            replacement[rlen].hidden_macros[k / 64] |= UINT64_C(1) << (k % 64);
                            ++rlen;
                        }
                        Token *part = expanded[s] ? expanded[s] : args[s];
                        size_t part_len = expanded[s] ? expanded_len[s] : args_len[s];
                        for (size_t q = 0; q < part_len; ++q) {
                            replacement[rlen] = part[q];
                            for (int word = 0; word < 2; ++word)
                                replacement[rlen].hidden_macros[word] |= tokens[i].hidden_macros[word];
                            replacement[rlen].hidden_macros[k / 64] |= UINT64_C(1) << (k % 64);
                            ++rlen;
                        }
                    }
                    continue;
                }
                /* Parameter substitution. */
                int p = -1;
                for (int pi = 0; pi < m->nparams; ++pi)
                    if (!strcmp(t->text, m->params[pi])) { p = pi; break; }
                if (p >= 0) {
                    int paste = (j && !strcmp(m->body[j-1].text, "##")) ||
                                (j + 1 < m->nbody && !strcmp(m->body[j+1].text, "##"));
                    Token *part = !paste && expanded[p] ? expanded[p] : args[p];
                    size_t part_len = !paste && expanded[p] ? expanded_len[p] : args_len[p];
                    for (size_t q = 0; q < part_len; ++q) {
                        replacement[rlen] = part[q];
                        if (!q) replacement[rlen].space_before = t->space_before;
                        for (int word = 0; word < 2; ++word)
                            replacement[rlen].hidden_macros[word] |= tokens[i].hidden_macros[word];
                        replacement[rlen].hidden_macros[k / 64] |= UINT64_C(1) << (k % 64);
                        ++rlen;
                    }
                    continue;
                }
                /* Plain token: copy through. */
                replacement[rlen] = *t;
                for (int word = 0; word < 2; ++word)
                    replacement[rlen].hidden_macros[word] |= tokens[i].hidden_macros[word];
                replacement[rlen].hidden_macros[k / 64] |= UINT64_C(1) << (k % 64);
                ++rlen;
            }
            /* Paste adjacent preprocessing tokens before rescanning the result. */
            for (size_t q = 0; q < rlen; ++q) {
                if (strcmp(replacement[q].text, "##")) continue;
                if (!q || q + 1 == rlen) fatal("token paste requires two operands");
                Token *left = &replacement[q - 1], *right = &replacement[q + 1];
                size_t length = strlen(left->text) + strlen(right->text);
                char *joined = resize(NULL, length + 1);
                strcpy(joined, left->text);
                strcpy(joined + strlen(left->text), right->text);
                Token *saved = tokens;
                size_t saved_count = ntokens, saved_capacity = capacity;
                tokens = NULL; ntokens = 0; capacity = 0;
                lex_source("<token paste>", joined, 1);
                if (ntokens != 1) fatal("token paste must form one token");
                free(tokens);
                tokens = saved; ntokens = saved_count; capacity = saved_capacity;
                left->text = joined;
                left->source = joined;
                for (int word = 0; word < 2; ++word)
                    left->hidden_macros[word] |= right->hidden_macros[word];
                for (size_t r = q; r + 2 < rlen; ++r) replacement[r] = replacement[r + 2];
                rlen -= 2;
                --q;
            }
            for (int p = 0; p < nargs; ++p) free(expanded[p]);
            free(expanded);
            free(expanded_len);
            splice_tokens(i, consumed, replacement, rlen);
            free(replacement);
            free(args);
            free(args_len);
            continue;
        }
        int line = tokens[i].line;
        size_t end = i + 1;
        while (end < ntokens && tokens[end].line == line &&
               tokens[end].file == tokens[i].file && strcmp(tokens[end].text, "<eof>")) ++end;
        if (i + 1 >= end) fatal("%s:%d: incomplete preprocessor directive",
                                tokens[i].file, line);
        const char *dir = tokens[i + 1].text;
        if (!strcmp(dir, "ifdef") || !strcmp(dir, "ifndef")) {
            if (i + 3 != end) fatal("malformed conditional directive");
            if (depth == 128) fatal("preprocessor conditional nesting limit exceeded");
            parent[depth] = active;
            selected[depth] = macro_defined(tokens[i + 2].text);
            if (!strcmp(dir, "ifndef")) selected[depth] = !selected[depth];
            saw_else[depth] = 0;
            conditional_file[depth] = tokens[i].file;
            active = active && selected[depth];
            ++depth;
            splice_tokens(i, end - i, NULL, 0);
            continue;
        }
        if (!strcmp(dir, "else") || !strcmp(dir, "endif")) {
            if (!depth || conditional_file[depth - 1] != tokens[i].file)
                fatal("unmatched preprocessor conditional directive");
            if (i + 2 != end) fatal("unexpected tokens after conditional directive");
            if (!strcmp(dir, "else")) {
                if (saw_else[depth - 1]) fatal("duplicate #else");
                saw_else[depth - 1] = 1;
                active = parent[depth - 1] && !selected[depth - 1];
            } else active = parent[--depth];
            splice_tokens(i, end - i, NULL, 0);
            continue;
        }
        if (!active) {
            splice_tokens(i, end - i, NULL, 0);
            continue;
        }
        if (!strcmp(dir, "undef")) {
            if (i + 3 != end) fatal("malformed #undef");
            for (size_t k = 0; k < nmacros; ++k)
                if (!strcmp(macros[k].name, tokens[i + 2].text))
                    macros[k].name = ""; /* Keep hide-set indices stable. */
            splice_tokens(i, end - i, NULL, 0);
            continue;
        }
        if (!strcmp(dir, "include")) {
            if (++expansions > 10000)
                fatal("%s:%d: include expansion limit exceeded", tokens[i].file, line);
            int quoted = i + 3 == end && tokens[i + 2].text[0] == '"';
            if (!quoted && (i + 4 > end || strcmp(tokens[i + 2].text, "<") ||
                strcmp(tokens[end - 1].text, ">")))
                fatal("%s:%d: malformed #include", tokens[i].file, line);
            size_t cap = 64, len = 0;
            char *name = resize(NULL, cap);
            if (quoted) {
                free(name);
                len = strlen(tokens[i + 2].text) - 2;
                name = copy(tokens[i + 2].text + 1, len);
            } else {
                for (size_t j = i + 3; j + 1 < end; ++j) {
                    size_t need = strlen(tokens[j].text);
                    while (len + need + 1 > cap) { cap *= 2; name = resize(name, cap); }
                    memcpy(name + len, tokens[j].text, need);
                    len += need;
                }
                name[len] = 0;
            }
            size_t prefix = 0;
            if (quoted) {
                for (const char *p = tokens[i].file; *p; ++p)
                    if (*p == '/') prefix = (size_t)(p - tokens[i].file) + 1;
            }
            size_t size = strlen(include_dir) + strlen(tokens[i].file) + len + 3;
            char *header = resize(NULL, size);
            FILE *probe = NULL;
            if (quoted) {
                memcpy(header, tokens[i].file, prefix);
                strcpy(header + prefix, name);
                probe = fopen(header, "r");
            }
            if (probe) fclose(probe);
            else snprintf(header, size, "%s/%s", include_dir, name);
            free(name);
            Token *outer = tokens;
            size_t outer_n = ntokens, outer_cap = capacity, outer_pos = pos;
            tokens = NULL;
            ntokens = 0;
            capacity = 0;
            lex(header, 1);
            Token *inner = tokens;
            size_t inner_n = ntokens;
            tokens = outer;
            ntokens = outer_n;
            capacity = outer_cap;
            pos = outer_pos;
            splice_tokens(i, end - i, inner, inner_n);
            free(inner);
            continue;
        }
        if (!strcmp(dir, "define")) {
            if (i + 3 > end) fatal("%s:%d: malformed #define", tokens[i].file, line);
            const char *name = tokens[i + 2].text;
            int nparams = 0, variadic = 0;
            char *params[16];
            size_t first = i + 3;
            if (first < end && !strcmp(tokens[first].text, "(") &&
                tokens[first].source == tokens[i + 2].source + strlen(name)) {
                /* Function-like macro. Find the matching ')' on this line. */
                size_t paren_open = first;
                int depth = 0;
                size_t j;
                for (j = paren_open; j < end; ++j) {
                    if (!strcmp(tokens[j].text, "(")) ++depth;
                    else if (!strcmp(tokens[j].text, ")")) { --depth; if (!depth) break; }
                }
                if (j == end) fatal("%s:%d: malformed macro parameter list",
                                    tokens[i].file, line);
                size_t paren_close = j;
                if (paren_close > paren_open + 1) {
                    /* Parse parameters between ( and ). */
                    size_t p = paren_open + 1;
                    while (p < paren_close) {
                        if (!strcmp(tokens[p].text, "...")) {
                            if (variadic || p != paren_close - 1)
                                fatal("%s:%d: ... must be the last parameter",
                                      tokens[i].file, line);
                            variadic = 1;
                            ++p;
                            if (p != paren_close)
                                fatal("%s:%d: trailing tokens after ...",
                                      tokens[i].file, line);
                            break;
                        }
                        if (nparams == 16) fatal("%s:%d: too many macro parameters",
                                                 tokens[i].file, line);
                        if (!identifier(tokens[p].text))
                            fatal("%s:%d: expected a macro parameter name",
                                  tokens[i].file, line);
                        params[nparams++] = copy(tokens[p].text, strlen(tokens[p].text));
                        ++p;
                        if (p < paren_close) {
                            if (strcmp(tokens[p].text, ","))
                                fatal("%s:%d: expected ',' between parameters",
                                      tokens[i].file, line);
                            ++p;
                        }
                    }
                }
                first = paren_close + 1;
            }
            if (nmacros == 128) fatal("%s:%d: too many macros", tokens[i].file, line);
            Macro *m = &macros[nmacros++];
            m->name = copy(name, strlen(name));
            m->nparams = nparams;
            m->variadic = variadic;
            m->function_like = (nparams || variadic) ||
                (first > i + 3); /* ( was consumed above */
            for (int k = 0; k < nparams; ++k) m->params[k] = params[k];
            m->nbody = end - first;
            m->body = m->nbody ? resize(NULL, m->nbody * sizeof(Token)) : NULL;
            for (size_t j = 0; j < m->nbody; ++j) {
                m->body[j] = tokens[first + j];
                /* Copy text — the original tokens are freed by splice_tokens. */
                m->body[j].text = copy(tokens[first + j].text,
                                       strlen(tokens[first + j].text));
            }
            splice_tokens(i, end - i, NULL, 0);
            continue;
        }
        fatal("%s:%d: unsupported preprocessor directive", tokens[i].file, line);
    }
    if (depth) fatal("unterminated preprocessor conditional");
}
static Token *current(void) { return &tokens[pos]; }

static void error(const char *message) {
    fprintf(stderr, "[DBG] error at pos=%zu file=%s line=%d\n", pos,
            current()->file, current()->line);
    for (size_t q = pos > 4 ? pos - 4 : 0; q < pos + 6 && q < ntokens; ++q)
        fprintf(stderr, "[DBG]   tok[%zu] line=%d '%s'\n", q, tokens[q].line, tokens[q].text);
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
    if (!compiling_softfloat && !strncmp(current()->text, "__4c_", 5))
        error("identifier prefix __4c_ is reserved for compiler routines");
    return tokens[pos++].text;
}

static Type *derived(TypeKind kind, Type *base, int size) {
    Type *t = resize(NULL, sizeof(*t));
    *t = (Type){kind, base, size, 0, NULL, 0};
    return t;
}
static Type *pointer(Type *base) { return derived(TY_PTR, base, 8); }
static int same_type(Type *a, Type *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    /* Record identity belongs to its declaration, not its byte layout. */
    if (a->kind == TY_STRUCT || a->kind == TY_UNION) return 0;
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
    t->size = ((is_union ? max_size : offset) + t->align - 1) & ~(t->align - 1);
}
static int integer(Type *t) {
    switch (t->kind) {
    case TY_BOOL: case TY_CHAR: case TY_SCHAR: case TY_UCHAR: case TY_SHORT: case TY_USHORT: case TY_INT: case TY_UINT:
    case TY_LONG: case TY_ULONG: return 1;
    default: return 0;
    }
}
/* Values of wide types live in x registers; narrower ones in w registers. */
static int wide(Type *t) {
    return t->kind == TY_PTR || t->kind == TY_LONG || t->kind == TY_ULONG || t->kind == TY_DOUBLE;
}
static int aggregate(Type *t) {
    return t->kind == TY_STRUCT || t->kind == TY_UNION;
}
/* Homogeneous double aggregates use consecutive FP registers, including
   nested records/arrays. Other aggregates larger than 16 bytes travel by
   pointer to a caller-owned copy. */
static int hfa_count(Type *t) {
    if (t->kind == TY_DOUBLE) return 1;
    if (t->kind == TY_ARRAY) {
        int n = hfa_count(t->base);
        if (!n || !t->base->size) return 0;
        n *= t->size / t->base->size;
        return n <= 4 ? n : 0;
    }
    if (!aggregate(t)) return 0;
    int count = 0;
    for (int i = 0; i < t->nmembers; ++i) {
        int n = hfa_count(t->members[i].type);
        if (!n) return 0;
        if (t->kind == TY_UNION) { if (n > count) count = n; }
        else count += n;
    }
    return count <= 4 && t->size == count * 8 ? count : 0;
}
static int indirect_aggregate(Type *t) {
    return aggregate(t) && t->size > 16 && !hfa_count(t);
}
typedef struct { int gp, fp, stack, count, indirect; } ArgPlace;
static ArgPlace argument_place(Type *t, int *gp, int *fp, int *stack) {
    ArgPlace p = {-1, -1, -1, 1, indirect_aggregate(t)};
    int hfa = hfa_count(t);
    int bytes = p.indirect ? 8 : (t->size + 7) & ~7;
    if (hfa) {
        p.count = hfa;
        if (*fp + hfa <= 8) { p.fp = *fp; *fp += hfa; return p; }
        *fp = 8;
    } else {
        p.count = aggregate(t) && !p.indirect ? (t->size + 7) / 8 : 1;
        if (*gp + p.count <= 8) { p.gp = *gp; *gp += p.count; return p; }
        *gp = 8;
    }
    p.stack = *stack;
    *stack += bytes;
    return p;
}
static int unsigned_type(Type *t) {
    return t->kind == TY_UINT || t->kind == TY_ULONG || t->kind == TY_UCHAR || t->kind == TY_USHORT ||
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
    if (integer(t) && t->size < 4) return &int_type;
    return t;
}
typedef struct { uint64_t value; Type *type; } IntConst;
static IntConst int_literal(void) {
    const char *s = current()->text;
    if (!isdigit((unsigned char)*s))
        error("expected an integer literal");
    int is_hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (s[0] == '0' && !is_hex && isdigit((unsigned char)s[1]))
        error("expected a decimal integer from 0 to 2147483647");
    int is_long = 0, is_unsigned = 0;
    for (const char *p = s + (is_hex ? 2 : 0); *p; ++p) {
        if (is_hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p)) continue;
        if (*p == 'L' || *p == 'l') is_long = 1;
        else if (*p == 'U' || *p == 'u') is_unsigned = 1;
        else error("expected an integer literal");
    }
    errno = 0;
    char *end;
    unsigned long long n = strtoull(is_hex ? s + 2 : s, &end, is_hex ? 16 : 10);
    if (errno) error("integer literal is out of range");
    const char *digits = is_hex ? s + 2 : s;
    size_t dlen = (size_t)(end - digits);
    if (is_hex ? !dlen || strspn(digits, "0123456789abcdefABCDEF") != dlen
               : strspn(digits, "0123456789") != dlen)
        error("expected an integer literal");
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

static int floating_literal(void) {
    const char *s = current()->text;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return 0;
    if (strpbrk(s, "uUlL")) return 0;
    return (isdigit((unsigned char)*s) || *s == '.') &&
           (strchr(s, '.') || strchr(s, 'e') || strchr(s, 'E'));
}

static uint64_t double_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* The compiler never changes the process's initial C locale or rounding mode.
   strtod supplies correctly rounded binary64 literals, including subnormals. */
static uint64_t double_literal(void) {
    const char *s = current()->text, *p = s;
    int digits = 0;
    while (isdigit((unsigned char)*p)) { ++p; ++digits; }
    if (*p == '.') { ++p; while (isdigit((unsigned char)*p)) { ++p; ++digits; } }
    if (!digits) error("invalid decimal double literal");
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (!isdigit((unsigned char)*p)) error("invalid double exponent");
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p) error("invalid decimal double literal or unsupported suffix");
    if (double_bits(1.0) != UINT64_C(0x3ff0000000000000))
        fatal("unsupported host binary64 representation");
    char *end;
    uint64_t bits = double_bits(strtod(s, &end));
    if (*end || bits >= UINT64_C(0x7ff0000000000000)) error("double literal overflows binary64");
    ++pos;
    return bits;
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
    if (current()->text[0] == '\'') {
        /* Character constants work in case labels and enumerator values. */
        String str = decode_literal();
        if (str.length != 1) error("character literal must contain one byte");
        value = (unsigned char)str.data[0];
        if (macos && value >= 128) value -= 256;
        free(str.data);
    } else if (isdigit((unsigned char)*current()->text)) value = decimal();
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
        if (existing < 0) {
            /* Forward declaration: register an incomplete tagged type. */
            if (ntags == 256) error("too many struct tags");
            tags[ntags++] = (Tag){tag, derived(is_union ? TY_UNION : TY_STRUCT, NULL, 0), tag_depth};
            existing = (int)ntags - 1;
        }
        return tags[existing].type;
    }
    int existing = tag ? find_tag(tag) : -1;
    if (existing >= 0 && tags[existing].depth == tag_depth) {
        Type *old = tags[existing].type;
        if (old->nmembers || old->kind != (is_union ? TY_UNION : TY_STRUCT))
            error("duplicate struct tag");
    }
    Type *t;
    if (existing >= 0 && tags[existing].depth == tag_depth) {
        /* Complete a previously forward-declared tagged type in place. */
        t = tags[existing].type;
    } else {
        t = derived(is_union ? TY_UNION : TY_STRUCT, NULL, 0);
        if (tag) {
            if (ntags == 256) error("too many struct tags");
            tags[ntags++] = (Tag){tag, t, tag_depth};
        }
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
    while (take("const")) {}
    if (take("int")) t = &int_type;
    else if (take("char")) t = &char_type;
    else if (take("short")) { take("int"); t = &short_type; }
    else if (take("signed")) {
        if (take("char")) t = &schar_type;
        else if (take("short")) { take("int"); t = &short_type; }
        else if (take("long")) { take("long"); take("int"); t = &long_type; }
        else { take("int"); t = &int_type; }
    }
    else if (take("void")) t = &void_type;
    else if (take("_Bool")) t = &bool_type;
    else if (take("double")) {
        if (macos) error("double support currently requires the Linux target");
        t = &double_type;
    }
    else if (take("long")) {
        if (take("long")) {
            if (take("unsigned")) t = &ulong_type;
            else if (take("signed")) { t = &long_type; }
            else t = &long_type;
            take("int");
        } else if (take("unsigned")) t = &ulong_type;
        else t = &long_type;
        take("int");
    } else if (take("unsigned")) {
        if (take("char")) t = &uchar_type;
        else if (take("short")) { take("int"); t = &ushort_type; }
        else if (take("long")) {
            if (take("long")) t = &ulong_type;
            else t = &ulong_type;
            take("int");
        }
        else { t = &uint_type; take("int"); }
    } else if (take("enum")) t = enum_specifier();
    else if (take("struct")) t = struct_specifier(0);
    else if (take("union")) t = struct_specifier(1);
    else if ((t = alias_type(current()->text))) ++pos;
    else { error("expected 'int', 'char', 'void', or a typedef name"); return NULL; }
    return t;
}
static Type *consume_stars(Type *t) {
    while (take("*")) {
        t = pointer(t);
        while (take("const")) {} /* ignore trailing qualifier */
    }
    return t;
}
/* Constant-expression evaluator for _Static_assert. Supports integer
   literals, sizeof(type), the comparison operators, &&, ||, !, unary +/-
   and binary +/- on constants. Sufficient for the host binary64 check. */
static int const_expr_primary(uint64_t *out) {
    if (!strcmp(current()->text, "(")) {
        ++pos;
        int v = const_expr_or(out);
        expect(")");
        return v;
    }
    if (!strcmp(current()->text, "sizeof")) {
        ++pos;
        expect("(");
        if (!type_start()) error("sizeof requires a type in parentheses");
        Type *t = parse_abstract();
        expect(")");
        *out = (uint64_t)t->size;
        return 1;
    }
    const char *s = current()->text;
    if (!isdigit((unsigned char)*s))
        error("expected a constant expression");
    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, 0);
    if (errno || end == s) error("expected a constant expression");
    ++pos;
    *out = n;
    return 1;
}
static int const_expr_unary(uint64_t *out) {
    if (take("!")) {
        uint64_t v;
        const_expr_unary(&v);
        *out = !v;
        return 1;
    }
    if (take("+")) return const_expr_unary(out);
    if (take("-")) {
        uint64_t v;
        const_expr_unary(&v);
        *out = -(uint64_t)v;
        return 1;
    }
    return const_expr_primary(out);
}
static int const_expr_mul(uint64_t *out) {
    const_expr_unary(out);
    while (!strcmp(current()->text, "*") ||
           !strcmp(current()->text, "/") ||
           !strcmp(current()->text, "%")) {
        const char *op = tokens[pos++].text;
        uint64_t r;
        const_expr_unary(&r);
        if (!strcmp(op, "*")) *out *= r;
        else if (!strcmp(op, "/")) *out /= r;
        else *out %= r;
    }
    return 1;
}
static int const_expr_add(uint64_t *out) {
    const_expr_mul(out);
    while (!strcmp(current()->text, "+") ||
           !strcmp(current()->text, "-")) {
        const char *op = tokens[pos++].text;
        uint64_t r;
        const_expr_mul(&r);
        if (!strcmp(op, "+")) *out += r;
        else *out -= r;
    }
    return 1;
}
static int const_expr_rel(uint64_t *out) {
    const_expr_add(out);
    const char *t = current()->text;
    if (!strcmp(t, "<") || !strcmp(t, "<=") ||
        !strcmp(t, ">") || !strcmp(t, ">=")) {
        ++pos;
        uint64_t r;
        const_expr_add(&r);
        int lt = *out < r, eq = *out == r;
        if (!strcmp(t, "<")) *out = lt;
        else if (!strcmp(t, "<=")) *out = lt || eq;
        else if (!strcmp(t, ">")) *out = !lt && !eq;
        else *out = !lt;
    }
    return 1;
}
static int const_expr_eq(uint64_t *out) {
    const_expr_rel(out);
    const char *t = current()->text;
    if (!strcmp(t, "==") || !strcmp(t, "!=")) {
        ++pos;
        uint64_t r;
        const_expr_rel(&r);
        int eq = *out == r;
        *out = !strcmp(t, "==") ? eq : !eq;
    }
    return 1;
}
static int const_expr_and(uint64_t *out) {
    const_expr_eq(out);
    while (!strcmp(current()->text, "&&")) {
        ++pos;
        uint64_t r;
        const_expr_eq(&r);
        *out = (*out && r) ? 1 : 0;
    }
    return 1;
}
static int const_expr_or(uint64_t *out) {
    const_expr_and(out);
    while (!strcmp(current()->text, "||")) {
        ++pos;
        uint64_t r;
        const_expr_and(&r);
        *out = (*out || r) ? 1 : 0;
    }
    return 1;
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
        base = pointer(base);
        while (take("const")) {} /* ignore */
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
/* Parse a constant initializer for a global. Strings are registered with the
   string table so they can be emitted in .rodata later. Returns the number
   of InitEntry slots written. */
static size_t parse_initializer(Type *t, InitEntry *out, size_t out_max);
static size_t parse_one_initializer(Type *t, InitEntry *out, size_t out_max) {
    (void)out_max;
    if (!t) error("initializer for incomplete type");
    /* String literal as a pointer. */
    if (current()->text[0] == '"' && t->kind == TY_PTR) {
        String str = string_bytes();
        size_t idx = nstrings++;
        strings = resize(strings, nstrings * sizeof(String));
        strings[idx] = str;
        out[0] = (InitEntry){(uint64_t)idx, t, 1};
        return 1;
    }
    /* NULL pointer constant or (void*)0 / (char*)0. */
    if (t->kind == TY_PTR) {
        if (current()->text[0] == '(' && pos + 1 < ntokens &&
            (!strcmp(tokens[pos + 1].text, "void") || !strcmp(tokens[pos + 1].text, "char"))) {
            ++pos;
            parse_abstract();
            expect(")");
            IntConst z = int_literal();
            if (z.value != 0) error("global pointer initializer must be zero");
            out[0] = (InitEntry){0, t, 0};
            return 1;
        }
        if (!strcmp(current()->text, "NULL")) {
            ++pos;
            out[0] = (InitEntry){0, t, 0};
            return 1;
        }
        IntConst z = int_literal();
        if (z.value != 0) error("global pointer initializer must be zero");
        out[0] = (InitEntry){0, t, 0};
        return 1;
    }
    /* Unary +/- followed by an integer or floating literal. */
    if (!strcmp(current()->text, "-")) {
        ++pos;
        if (floating_literal()) {
            uint64_t v = double_literal();
            out[0] = (InitEntry){v ^ UINT64_C(0x8000000000000000), &double_type, 0};
        } else if (t->kind == TY_DOUBLE) {
            IntConst c = int_literal();
            double value;
            if (c.type->kind == TY_INT) value = (double)(int32_t)c.value;
            else if (c.type->kind == TY_UINT) value = (double)(uint32_t)c.value;
            else if (c.type->kind == TY_LONG) value = (double)(int64_t)c.value;
            else value = (double)c.value;
            out[0] = (InitEntry){double_bits(-value), &double_type, 0};
        } else {
            IntConst c = int_literal();
            uint64_t v = (uint64_t)(-(int64_t)c.value);
            if (t->kind == TY_BOOL) v = v != 0;
            out[0] = (InitEntry){v, t, 0};
        }
        return 1;
    }
    if (!strcmp(current()->text, "+")) {
        ++pos;
        if (floating_literal()) {
            out[0] = (InitEntry){double_literal(), &double_type, 0};
        } else if (t->kind == TY_DOUBLE) {
            IntConst c = int_literal();
            double value;
            if (c.type->kind == TY_INT) value = (double)(int32_t)c.value;
            else if (c.type->kind == TY_UINT) value = (double)(uint32_t)c.value;
            else if (c.type->kind == TY_LONG) value = (double)(int64_t)c.value;
            else value = (double)c.value;
            out[0] = (InitEntry){double_bits(value), &double_type, 0};
        } else {
            IntConst c = int_literal();
            out[0] = (InitEntry){c.value, t, 0};
        }
        return 1;
    }
    /* Cast: (type)literal. */
    if (!strcmp(current()->text, "(")) {
        ++pos;
        expect("(");
        Type *cast = parse_abstract();
        expect(")");
        IntConst c = int_literal();
        out[0] = (InitEntry){c.value, cast, 0};
        return 1;
    }
    if (current()->text[0] == '\'') {
        String str = decode_literal();
        if (str.length != 1) error("character literal must contain one byte");
        uint64_t v = (uint8_t)str.data[0];
        if (macos && v >= 128) v -= 256;
        free(str.data);
        out[0] = (InitEntry){v, &char_type, 0};
        return 1;
    }
    if (isdigit((unsigned char)current()->text[0])) {
        IntConst c = int_literal();
        /* Choose a target type if t is unknown: prefer narrower. */
        if (t->kind == TY_BOOL) {
            out[0] = (InitEntry){c.value != 0, t, 0};
            return 1;
        }
        if (t->kind == TY_CHAR) {
            out[0] = (InitEntry){c.value & 0xFF, t, 0};
            return 1;
        }
        out[0] = (InitEntry){c.value, t, 0};
        return 1;
    }
    /* Enum constant or zero identifier. */
    if (!strcmp(current()->text, "0") || !strcmp(current()->text, "0L") ||
        !strcmp(current()->text, "0UL")) {
        int_literal();
        out[0] = (InitEntry){0, t, 0};
        return 1;
    }
    if (isalpha((unsigned char)current()->text[0]) || current()->text[0] == '_') {
        int value;
        if (enum_constant(current()->text, &value)) {
            ++pos;
            out[0] = (InitEntry){(uint64_t)(int32_t)value, &int_type, 0};
            return 1;
        }
        if (!strcmp(current()->text, "NULL") && t->kind == TY_PTR) {
            ++pos;
            out[0] = (InitEntry){0, t, 0};
            return 1;
        }
        if (!strcmp(current()->text, "true") || !strcmp(current()->text, "false")) {
            int v = !strcmp(current()->text, "true");
            ++pos;
            out[0] = (InitEntry){v, &bool_type, 0};
            return 1;
        }
        error("expected a constant initializer");
        return 0;
    }
    error("expected a constant initializer");
    return 0;
}
static size_t parse_initializer(Type *t, InitEntry *out, size_t out_max) {
    size_t pos = 0;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        for (int m = 0; m < t->nmembers; ++m) {
            if (m > 0) {
                if (!take(",")) break;
            }
            if (pos >= out_max) error("initializer is too large");
            if (!strcmp(current()->text, "{")) {
                ++pos;
                pos += parse_initializer(t->members[m].type, out + pos, out_max - pos);
                if (!take("}")) error("expected '}' at end of initializer");
            } else {
                pos += parse_one_initializer(t->members[m].type, out + pos, out_max - pos);
            }
            if (t->kind == TY_UNION && m == 0) {
                /* Union: only first member's data is meaningful; the type's
                   full size is allocated. Other members are zero-padding. */
                while (pos * 8 < (size_t)t->size) {
                    out[pos++] = (InitEntry){0, &int_type, 0};
                }
            }
        }
        take(",");
    } else if (t->kind == TY_ARRAY) {
        if (t->base->kind == TY_CHAR && current()->text[0] == '"') {
            String str = string_bytes();
            size_t to_copy = str.length;
            if (to_copy > (size_t)t->size) to_copy = t->size;
            for (size_t i = 0; i < to_copy; ++i) {
                out[pos++] = (InitEntry){(uint8_t)str.data[i], &char_type, 0};
            }
            while (pos < (size_t)(t->size / (t->base->size ? t->base->size : 1))) {
                out[pos++] = (InitEntry){0, &char_type, 0};
            }
            return pos;
        }
        int base_size = t->base->size ? t->base->size : 1;
        int nelems = t->size / base_size;
        int count = 0;
        int inferred = (nelems == 0);
        if (strcmp(current()->text, "}")) {
            for (;;) {
                if (!inferred && count >= nelems) error("too many array initializers");
                if (!strcmp(current()->text, "{")) {
                    ++pos;
                    pos += parse_initializer(t->base, out + pos, out_max - pos);
                    if (!take("}")) error("expected '}' at end of initializer");
                } else {
                    pos += parse_one_initializer(t->base, out + pos, out_max - pos);
                }
                ++count;
                if (!take(",")) break;
                if (!strcmp(current()->text, "}")) break;
            }
        }
        if (inferred) nelems = count;
        while (count < nelems) {
            out[pos++] = (InitEntry){0, t->base, 0};
            ++count;
        }
        if (inferred) t->size = nelems * base_size;
    } else {
        /* Scalar initializer (or single-element brace).
           `{value}` around a scalar is equivalent to `value`. */
        if (!strcmp(current()->text, "{")) {
            ++pos;
            pos += parse_one_initializer(t, out + pos, out_max - pos);
            if (!take("}")) error("expected '}' at end of initializer");
        } else {
            pos += parse_one_initializer(t, out + pos, out_max - pos);
        }
    }
    return pos;
}
/* Emit one initializer entry. */
static void emit_init_entry(InitEntry e) {
    Type *t = e.type;
    if (!t) t = &int_type;
    if (t->kind == TY_PTR || t->kind == TY_LONG || t->kind == TY_ULONG ||
        t->kind == TY_DOUBLE) {
        if (e.is_string) {
            fprintf(program, "    .quad Lstr%llu\n", (unsigned long long)e.value);
        } else {
            fprintf(program, "    .quad %llu\n", (unsigned long long)e.value);
        }
    } else if (t->kind == TY_INT || t->kind == TY_UINT) {
        fprintf(program, "    .word %llu\n",
                (unsigned long long)(int32_t)e.value);
    } else if (t->size == 2) {
        fprintf(program, "    .hword %u\n", (unsigned)(e.value & 65535));
    } else if (t->size == 1) {
        fprintf(program, "    .byte %llu\n", (unsigned long long)(e.value & 0xFF));
    } else {
        fprintf(program, "    .word %llu\n", (unsigned long long)e.value);
    }
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
    if (suppress_emit) return;
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
    fprintf(out, "    //\n    // C line %d: ", tokens[first].line);
    int space = 0;
    for (; p < end; ++p) {
        if (isspace((unsigned char)*p)) { space = 1; continue; }
        if (space) fputc(' ', out);
        space = 0;
        fputc((unsigned char)*p, out);
    }
    fputs("\n    //\n", out);
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
static Expr assignment_expr(void);
static Expr unary(void);
/* Load short integers with bytes so the twelve-mnemonic vocabulary is kept. */
static void load_small(Type *t, const char *addr, int reg) {
    if (t->size == 2) emit("    ldrb w13, [%s, #1]\n", addr);
    emit("    ldrb w%d, [%s]\n", reg, addr);
    if (t->size == 2) emit("    add w%d, w%d, w13, lsl #8\n", reg, reg);
    if (t->kind != TY_BOOL && !unsigned_type(t)) {
        size_t id = next_label++;
        emit("    tbz w%d, #%d, .Lchar%zu\n", reg, t->size * 8 - 1, id);
        if (t->size == 1) emit("    sub w%d, w%d, #256\n", reg, reg);
        else emit("    sub w%d, w%d, #16, lsl #12\n", reg, reg);
        emit(".Lchar%zu:\n", id);
    }
}
/* Load a value from the address held in the given register into w9/x9. */
static void load_value_reg(Type *t, const char *addr) {
    if (wide(t)) emit("    ldr x9, [%s]\n", addr);
    else if (t->kind == TY_INT || t->kind == TY_UINT) emit("    ldr w9, [%s]\n", addr);
    else load_small(t, addr, 9);
}
static Expr value(Expr e) {
    if (e.type->kind == TY_VOID) error("void expression has no value");
    if (e.type->kind == TY_ARRAY) {
        e.type = pointer(e.type->base);
    } else if (e.type->kind == TY_STRUCT || e.type->kind == TY_UNION) {
        /* Aggregates decay to their address; no register load. */
    } else if (e.lvalue) {
        if (!suppress_emit && e.local >= 0 && !locals[e.local].initialized)
            error("unknown local value in self-initializer");
        if (wide(e.type)) emit("    ldr x0, [x0]\n");
        else if (e.type->kind == TY_INT || e.type->kind == TY_UINT)
            emit("    ldr w0, [x0]\n");
        else load_small(e.type, "x0", 0);
    }
    e.lvalue = 0;
    e.local = -1;
    return e;
}
static Expr condition_value(Expr e) {
    e = value(e);
    if (e.type->kind == TY_DOUBLE) emit("    add x0, x0, x0\n");
    return e;
}
static void narrow_integer(Type *t) {
    emit("    sub sp, sp, #16\n    str w0, [sp]\n");
    load_small(t, "sp", 0);
    emit("    add sp, sp, #16\n");
}
/* Normalize any scalar value in x0/w0 to exactly 0 or 1 for _Bool. */
static void normalize_bool(Type *from) {
    size_t id = next_label++;
    if (from->kind == TY_DOUBLE) emit("    add x0, x0, x0\n"); /* Ignore zero's sign. */
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
    if (to->kind == TY_DOUBLE) {
        if (e.type->kind != TY_DOUBLE) {
            if (!integer(e.type)) error("double conversion requires an integer or double");
            if (!wide(e.type)) extend_reg(e.type, "w0", "x0");
            need_softfloat = 1;
            emit("    bl .L__4c_sf_%s64\n", unsigned_type(e.type) ? "u" : "i");
        }
        return (Expr){to, 0, -1, 0};
    }
    if (to->kind == TY_PTR) {
        int compatible = same_type(e.type, to) || (integer(e.type) && e.zero);
        if (!compatible && e.type->kind == TY_PTR &&
            (e.type->base->kind == TY_VOID || to->base->kind == TY_VOID))
            compatible = 1;
        if (!compatible) error("incompatible pointer conversion");
        return (Expr){to, 0, -1, e.zero};
    }
    if (to->kind == TY_STRUCT || to->kind == TY_UNION) {
        if (!same_type(e.type, to)) error("incompatible aggregate assignment");
        return (Expr){to, 0, -1, 0};
    }
    if (e.type->kind == TY_STRUCT || e.type->kind == TY_UNION)
        error("cannot convert aggregate to a scalar");
    if (to->kind == TY_BOOL) {
        if (!integer(e.type) && e.type->kind != TY_PTR && e.type->kind != TY_DOUBLE)
            error("cannot convert to _Bool");
        if (e.type->kind != TY_BOOL) normalize_bool(e.type);
        return (Expr){to, 0, -1, e.zero};
    }
    if (e.type->kind == TY_DOUBLE) error("double-to-integer conversion is not supported yet");
    if (!integer(e.type)) error("cannot convert pointer to integer");
    if (integer(to) && to->size < 4) narrow_integer(to);
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
    if (t->size == 2) {
        emit("    sub sp, sp, #16\n    str w0, [sp]\n    ldrb w13, [sp, #1]\n    add sp, sp, #16\n");
        emit("    strb w0, [%s]\n    strb w13, [%s, #1]\n", address, address);
    } else emit("    %s %s0, [%s]\n", t->size == 1 ? "strb" : "str", wide(t) ? "x" : "w", address);

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
/* Helper: parse a brace-wrapped initializer and store its single value into
   the given frame address. Used for nested scalar-in-braces initializers. */
static void parse_compound_inner(Type *t, const char *addr) {
    Expr e = expression();
    convert(e, t);
    emit("    add x9, %s\n", addr);
    store_value(t, "x9");
}
static Expr primary(void) {
    /* va_start(ap, last), va_end(ap), va_copy(dest, src) are syntactic forms
       with compiler-known semantics on AAPCS64. They consume the token
       stream and emit code (or no code) at the use site. */
    if (!strcmp(current()->text, "va_start")) {
        ++pos;
        expect("(");
        if (!identifier(current()->text)) error("expected a va_list identifier");
        char *ap_name = tokens[pos].text;
        int ap_local = find_local(ap_name);
        if (ap_local < 0) error("va_start first argument must be a local variable");
        ++pos;
        expect(",");
        if (!identifier(current()->text)) error("expected a parameter name");
        ++pos;
        expect(")");
        Function *fn = &functions[current_function];
        if (!fn->variadic || macos) error("va_start requires a Linux variadic function");
        int tag_off = allocate(derived(TY_ARRAY, &char_type, 32));
        int save_end = fn->va_save_offset + 64;
        /* ap = &tag = x29 + tag_off */
        emit("    add x9, x29, #%d\n", tag_off);
        emit("    str x9, [x29, #%d]\n", locals[ap_local].offset);
        emit("    ldr x9, [x29, #%d]\n", locals[ap_local].offset);
        /* tag->stack = x29 + frame (caller's overflow area) */
        emit("    add x10, x29, #.Lframe%zu\n", current_function);
        emit("    str x10, [x9]\n");
        /* tag->gr_top = x29 + save_end (just past x7's slot) */
        emit("    add x10, x29, #%d\n", save_end);
        emit("    str x10, [x9, #8]\n");
        /* tag->vr_top = x29 + fp_save_end (just past d7's 16-byte slot) */
        int fp_end = fn->fp_save_offset + 128;
        emit("    add x10, x29, #%d\n", fp_end);
        emit("    str x10, [x9, #16]\n");
        /* tag->gr_offset = -8 * (8 - named GP regs), increasing per va_arg. */
        int named_gp = 0, named_fp = 0;
        for (int i = 0; i < fn->nparams; ++i) {
            if (fn->params[i]->kind == TY_DOUBLE) ++named_fp;
            else ++named_gp;
        }
        int gr_off_val = 8 * named_gp - 64;
        /* sub with an immediate cannot name xzr as its first operand. */
        emit("    sub x10, x10, x10\n");
        if (gr_off_val < 0)
            emit("    sub x10, x10, #%d\n", -gr_off_val);
        else
            emit("    add x10, x10, #%d\n", gr_off_val);
        emit("    str w10, [x9, #24]\n");
        /* Skip the named FP arguments in their 16-byte register-save slots. */
        emit("    sub x10, x10, x10\n");
        emit("    sub x10, x10, #%d\n", 128 - named_fp * 16);
        emit("    str w10, [x9, #28]\n");
        return (Expr){&void_type, 0, -1, 1};
    }
    if (!strcmp(current()->text, "va_end")) {
        ++pos;
        expect("(");
        if (!identifier(current()->text)) error("expected a va_list identifier");
        ++pos;
        expect(")");
        /* va_end is a no-op on AAPCS64 */
        return (Expr){&void_type, 0, -1, 1};
    }
    if (!strcmp(current()->text, "va_copy")) {
        ++pos;
        expect("(");
        if (!identifier(current()->text)) error("expected a va_list identifier");
        char *dst_name = tokens[pos].text;
        int dst_local = find_local(dst_name);
        if (dst_local < 0) error("va_copy destination must be a local variable");
        ++pos;
        expect(",");
        if (!identifier(current()->text)) error("expected a va_list identifier");
        char *src_name = tokens[pos].text;
        int src_local = find_local(src_name);
        if (src_local < 0) error("va_copy source must be a local variable");
        ++pos;
        expect(")");
        /* Each copy owns an independent 32-byte AAPCS64 cursor. */
        if (macos) error("va_copy currently requires Linux");
        int tag_off = allocate(derived(TY_ARRAY, &char_type, 32));
        emit("    ldr x9, [x29, #%d]\n", locals[src_local].offset);
        emit("    add x10, x29, #%d\n", tag_off);
        for (int off = 0; off < 32; off += 8)
            emit("    ldr x11, [x9, #%d]\n    str x11, [x10, #%d]\n", off, off);
        emit("    str x10, [x29, #%d]\n", locals[dst_local].offset);
        return (Expr){&void_type, 0, -1, 1};
    }
    if (!strcmp(current()->text, "sizeof")) {
        ++pos;
        expect("(");
        if (type_start()) {
            /* sizeof(type) — the type may be followed by '*' (pointer) or
               directly by ')'. parse_abstract handles both. */
            Type *t = parse_abstract();
            expect(")");
            /* ldr w0 zero-extends into x0, matching size_t semantics. */
            literal((uint32_t)t->size);
            return (Expr){&ulong_type, 0, -1, t->size == 0};
        }
        /* sizeof does not evaluate its operand (VLAs are not supported). */
        int saved_suppress = suppress_emit, saved_frame = frame_bytes;
        int saved_max = max_frame_bytes, saved_softfloat = need_softfloat;
        size_t saved_constants = nconstants;
        suppress_emit = 1;
        Expr e = expression();
        suppress_emit = saved_suppress;
        frame_bytes = saved_frame;
        max_frame_bytes = saved_max;
        need_softfloat = saved_softfloat;
        nconstants = saved_constants;
        expect(")");
        literal((uint32_t)e.type->size);
        return (Expr){&ulong_type, 0, -1, e.type->size == 0};
    }
    if (take("(")) {
        if (type_start()) {
            Type *to = parse_abstract();
            expect(")");
            /* Compound literal: (Type){...} — allocate the object on the
               stack, initialize its fields from arbitrary expressions, and
               return its address as an lvalue. */
            if (!strcmp(current()->text, "{")) {
                ++pos;
                int align = alignment(to);
                int offset = (frame_bytes + align - 1) & ~(align - 1);
                frame_bytes = offset + to->size;
                if (frame_bytes > 4080) error("locals exceed the 4080-byte frame limit");
                if (frame_bytes > max_frame_bytes) max_frame_bytes = frame_bytes;
                /* Initialize each member positionally. */
                if (to->kind == TY_STRUCT || to->kind == TY_UNION) {
                    int initialized_member[32] = {0};
                    for (int m = 0; m < to->nmembers; ++m) {
                        if (m > 0) {
                            if (!take(",")) break;
                        }
                        int moff = offset + to->members[m].offset;
                        int msize = to->members[m].type->size;
                        /* Designator: '.name' selects that member. Members
                           with no initializer remain zero (handled below). */
                        if (!strcmp(current()->text, ".")) {
                            ++pos;
                            char *dname = tokens[pos].text;
                            ++pos;
                            int found = -1;
                            for (int q = 0; q < to->nmembers; ++q)
                                if (!strcmp(to->members[q].name, dname)) { found = q; break; }
                            if (found < 0) error("unknown member in designated initializer");
                            expect("=");
                            m = found;
                            moff = offset + to->members[m].offset;
                        }
                        initialized_member[m] = 1;
                        if (to->kind == TY_UNION && m > 0 && strcmp(current()->text, ".")) {
                            /* Skip past unused union members. */
                            while (m < to->nmembers &&
                                   !strcmp(current()->text, ",")) {
                                ++pos;
                                ++m;
                            }
                            break;
                        }
                        if (to->members[m].type->kind == TY_ARRAY &&
                            !strcmp(current()->text, "{")) {
                            /* Array member initialized with brace list. */
                            ++pos;
                            Type *bt = to->members[m].type->base;
                            int bsize = bt->size ? bt->size : 1;
                            int nelems = to->members[m].type->size / bsize;
                            int idx = 0;
                            while (idx < nelems &&
                                   strcmp(current()->text, "}")) {
                                char inner_addr[32];
                                snprintf(inner_addr, sizeof(inner_addr),
                                         "x29, #%d", moff + idx * bsize);
                                if (bt->kind == TY_CHAR &&
                                    current()->text[0] == '"') {
                                    String str = string_bytes();
                                    int to_copy = str.length;
                                    if (to_copy > nelems - idx)
                                        to_copy = nelems - idx;
                                    for (int q = 0; q < to_copy; ++q) {
                                        emit("    sub x10, x10, x10\n");
                                        emit("    add w10, w10, #%d\n", (unsigned char)str.data[q]);
                                        snprintf(inner_addr + 9,
                                                 sizeof(inner_addr) - 9,
                                                 "%d", moff + idx * bsize + q);
                                        emit("    strb w10, [x29, #%d]\n",
                                             moff + idx * bsize + q);
                                    }
                                    idx += to_copy;
                                } else {
                                    assignment_expr();
                                    emit("    add x9, %s\n", inner_addr);
                                    store_value(bt, "x9");
                                    ++idx;
                                }
                                if (!take(",")) break;
                            }
                            if (!take("}")) error("expected '}' at end of array initializer");
                            (void)msize;
                            continue;  /* Don't fall into scalar parse. */
                        } else if (!strcmp(current()->text, "{")) {
                            /* Nested aggregate or brace-wrapped scalar. */
                            ++pos;
                            char inner_addr[32];
                            snprintf(inner_addr, sizeof(inner_addr),
                                     "x29, #%d", moff);
                            parse_compound_inner(to->members[m].type, inner_addr);
                            if (!take("}")) error("expected '}' at end of initializer");
                            continue;
                        } else {
                            Expr e = assignment_expr();
                            convert(e, to->members[m].type);
                            char inner_addr[32];
                            snprintf(inner_addr, sizeof(inner_addr),
                                     "x29, #%d", moff);
                            emit("    add x9, %s\n", inner_addr);
                            store_value(to->members[m].type, "x9");
                            (void)msize;
                        }
                    }
                    /* Zero-fill members the initializer did not mention. */
                    for (int m = 0; m < to->nmembers; ++m)
                        if (!initialized_member[m]) {
                            Type *mt = to->members[m].type;
                            int words = (mt->size + 7) / 8;
                            for (int w = 0; w < words; ++w) {
                                emit("    sub x0, x0, x0\n");
                                emit("    add x9, x29, #%d\n",
                                     offset + to->members[m].offset + w * 8);
                                int rem = mt->size - w * 8;
                                if (rem >= 8) emit("    str x0, [x9]\n");
                                else
                                    for (int b = 0; b < rem; ++b)
                                        emit("    add x9, x29, #%d\n    strb w0, [x9]\n",
                                             offset + to->members[m].offset + w * 8 + b);
                            }
                        }
                    take(",");
                } else if (to->kind == TY_ARRAY) {
                    /* Array compound literal: positional elements. */
                    int base_size = to->base->size;
                    int count = 0;
                    if (strcmp(current()->text, "}")) {
                        for (;;) {
                            int idx = count * base_size;
                            int moff = offset + idx;
                            assignment_expr();
                            char inner_addr[32];
                            snprintf(inner_addr, sizeof(inner_addr),
                                     "x29, #%d", moff);
                            emit("    add x9, %s\n", inner_addr);
                            store_value(to->base, "x9");
                            ++count;
                            if (!take(",")) break;
                            if (!strcmp(current()->text, "}")) break;
                        }
                    }
                } else {
                    /* Scalar compound literal. */
                    Expr e = assignment_expr();
                    convert(e, to);
                    char inner_addr[32];
                    snprintf(inner_addr, sizeof(inner_addr), "x29, #%d", offset);
                    emit("    add x9, %s\n", inner_addr);
                    store_value(to, "x9");
                }
                if (!take("}")) error("expected '}' at end of compound literal");
                /* Place the compound literal's address in x0 so the value()/
                   store_value() path that follows in assignment can copy from it. */
                emit("    add x0, x29, #%d\n", offset);
                return (Expr){to, 1, -1, 0};  /* lvalue pointing at offset */
            }
            Expr operand = unary();
            if (to->kind == TY_VOID) {
                if (operand.type->kind != TY_VOID) value(operand);
                return (Expr){&void_type, 0, -1, 0};
            }
            if (to->kind == TY_PTR &&
                (operand.type->kind == TY_PTR || operand.type->kind == TY_ARRAY)) {
                value(operand);
                return (Expr){to, 0, -1, 0};
            }
            return convert(operand, to);
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
    if (floating_literal()) {
        if (macos) error("double support currently requires the Linux target");
        literal64(double_literal());
        return (Expr){&double_type, 0, -1, 0};
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
        /* A function returning an aggregate larger than sixteen bytes
           receives the caller's result buffer address in x8. Reserve the
           frame slot for that buffer now. */
        int result_off = -1;
        if (aggregate(fn->result)) {
            int align = alignment(fn->result);
            result_off = (frame_bytes + align - 1) & ~(align - 1);
            frame_bytes = result_off + fn->result->size;
            if (frame_bytes > 4080) error("locals exceed the 4080-byte frame limit");
            if (frame_bytes > max_frame_bytes) max_frame_bytes = frame_bytes;
        }
        int call_frame = frame_bytes;
        ArgPlace places[8];
        int gp_count = 0, fp_count = 0, running_stack = 0;
        for (int i = 0; i < expected; ++i)
            places[i] = argument_place(fn->params[i], &gp_count, &fp_count, &running_stack);
        int stack_bytes = (running_stack + 15) & ~15;
        if (stack_bytes + 128 > 4095) error("arguments exceed staging area");
        /* One slot holds a scalar or an address of a private aggregate copy. */
        int stage = 8;
        emit("    sub sp, sp, #%d\n", stage * 16 + stack_bytes);
        Type *arg_types[8] = {0};
        int count = 0;
        if (!take(")")) {
            for (;;) {
                if (count == 8) error("at most eight function arguments");
                if (count < expected) {
                    Type *pt = fn->params[count];
                    convert(assignment_expr(), pt);
                    if (aggregate(pt)) {
                        int copy_off = allocate(derived(TY_ARRAY, &char_type, (pt->size + 7) & ~7));
                        emit("    add x9, x29, #%d\n", copy_off);
                        copy_object(pt, "x9", "x0");
                        emit("    add x0, x29, #%d\n", copy_off);
                    }
                    emit("    str x0, [sp, #%d]\n", count * 16);
                } else {
                    if (!fn->variadic)
                        error("wrong number of function arguments");
                    /* Default argument promotions for variadic slots. */
                    Expr a = value(assignment_expr());
                    if (integer(a.type) && a.type->size < 4 && a.type->kind != TY_BOOL) narrow_integer(a.type);
                    else if (a.type->kind == TY_STRUCT || a.type->kind == TY_UNION)
                        error("aggregate arguments are not supported yet");
                    arg_types[count] = promote(a.type);
                    places[count] = argument_place(arg_types[count], &gp_count, &fp_count, &running_stack);
                    if (places[count].stack >= 0) error("too many variadic register arguments");
                    /* Unnamed slots stage sequentially in the full area. */
                    emit("    str x0, [sp, #%d]\n", count * 16);
                }
                ++count;
                if (!take(",")) break;
            }
            expect(")");
        }
        if (fn->variadic) {
            if (count < expected) error("wrong number of function arguments");
        } else if (count != expected) {
            error("wrong number of function arguments");
        }
        /* Prepare overflow storage before loading any argument registers. */
        for (int i = 0; i < count; ++i) {
            ArgPlace place = places[i];
            Type *type = i < expected ? fn->params[i] : arg_types[i];
            if (place.stack < 0) continue;
            emit("    ldr x9, [sp, #%d]\n", i * 16);
            if (aggregate(type) && !place.indirect) {
                for (int j = 0; j < type->size; ++j)
                    emit("    ldrb w10, [x9, #%d]\n    strb w10, [sp, #%d]\n",
                         j, stage * 16 + place.stack + j);
            } else emit("    str x9, [sp, #%d]\n", stage * 16 + place.stack);
        }
        for (int i = 0; i < count; ++i) {
            ArgPlace place = places[i];
            Type *type = i < expected ? fn->params[i] : arg_types[i];
            if (place.stack >= 0) continue;
            if (aggregate(type) && !place.indirect) {
                emit("    ldr x9, [sp, #%d]\n", i * 16);
                for (int j = 0; j < place.count; ++j)
                    emit("    ldr %s%d, [x9, #%d]\n", place.fp >= 0 ? "d" : "x",
                         (place.fp >= 0 ? place.fp : place.gp) + j, j * 8);
            } else {
                emit("    ldr %s%d, [sp, #%d]\n",
                     place.fp >= 0 ? "d" : (wide(type) || place.indirect) ? "x" : "w",
                     place.fp >= 0 ? place.fp : place.gp, i * 16);
            }
        }
        if (stage) emit("    add sp, sp, #%d\n", stage * 16);
        if (indirect_aggregate(fn->result))
            emit("    add x8, x29, #%d\n", result_off);
        emit("    bl %s%s\n", fn->internal ? ".L" : macos ? "_" : "", s);
        if (stack_bytes) emit("    add sp, sp, #%d\n", stack_bytes);
        if (aggregate(fn->result)) {
            if (hfa_count(fn->result)) {
                for (int j = 0; j < hfa_count(fn->result); ++j)
                    emit("    str d%d, [x29, #%d]\n", j, result_off + j * 8);
            } else if (fn->result->size <= 16) {
                emit("    str x0, [x29, #%d]\n", result_off);
                if (fn->result->size > 8)
                    emit("    str x1, [x29, #%d]\n", result_off + 8);
            }
            emit("    add x0, x29, #%d\n", result_off);
        } else if (fn->result->kind == TY_DOUBLE)
            emit("    sub sp, sp, #16\n    str d0, [sp]\n    ldr x0, [sp]\n    add sp, sp, #16\n");
        if (integer(fn->result) && fn->result->size < 4 && fn->result->kind != TY_BOOL) narrow_integer(fn->result);
        else if (fn->result->kind == TY_BOOL) normalize_bool(&int_type);
        frame_bytes = call_frame;
        return (Expr){fn->result, 0, -1, 0};
    }
    if (local >= 0 && locals[local].offset == -4) {
        global = locals[local].value;
        s = globals[global].name;
        local = -1;
    }
    if (local < 0) {
        if (global < 0) error("unknown local or global variable");
        ++pos;
        if (globals[global].offset == -3) {
            /* External objects resolve through the global offset table. */
            if (macos)
                emit("    adrp x0, _%s@GOTPAGE\n    ldr x0, [x0, _%s@GOTPAGEOFF]\n", s, s);
            else
                emit("    adrp x0, :got:%s\n    ldr x0, [x0, :got_lo12:%s]\n", s, s);
        } else if (macos) {
            emit("    adrp x0, _%s@PAGE\n    add x0, x0, _%s@PAGEOFF\n", s, s);
        } else {
            emit("    adrp x0, %s\n    add x0, x0, :lo12:%s\n", s, s);
        }
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
static void emit_divide(Type *t, int remainder);
static Expr binary_add(Expr left, Expr right, int subtract) {
    if (subtract && left.type->kind == TY_PTR && right.type->kind == TY_PTR) {
        if (!same_type(left.type, right.type) || !left.type->base->size)
            error("pointer subtraction requires compatible complete object types");
        emit("    sub x9, x9, x0\n");
        if (left.type->base->size == 1) emit("    sub x0, x9, xzr\n");
        else {
            literal64_to("x0", (uint64_t)left.type->base->size);
            emit_divide(&long_type, 0);
        }
        return (Expr){&long_type, 0, -1, 0};
    }
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
            Expr index = value(assignment_expr());
            expect("]");
            emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
            if (!(e.type->kind == TY_PTR || index.type->kind == TY_PTR))
                error("indexing requires a pointer or array");
            e = binary_add(e, index, 0);
            e = (Expr){e.type->base, 1, -1, 0};
            continue;
        }
        int increment = take("++");
        if (increment || take("--")) {
            if (!e.lvalue || e.type->kind == TY_ARRAY || e.type->kind == TY_DOUBLE ||
                (!integer(e.type) && e.type->kind != TY_PTR))
                error("postfix increment requires an assignable integer or pointer");
            emit("    sub sp, sp, #16\n    str x0, [sp]\n");
            Expr old = value(e);
            emit("    str x0, [sp, #8]\n    sub x9, x0, xzr\n");
            literal(1);
            Expr updated = binary_add(old, (Expr){&int_type, 0, -1, 0}, !increment);
            convert(updated, e.type);
            emit("    ldr x11, [sp]\n");
            store_value(e.type, "x11");
            emit("    ldr x0, [sp, #8]\n    add sp, sp, #16\n");
            e = (Expr){e.type, 0, -1, 0};
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
    if (take("~")) {
        Expr e = value(unary());
        if (!integer(e.type)) error("bitwise complement requires an integer");
        Type *t = promote(e.type);
        const char *r = wide(t) ? "x" : "w";
        emit("    sub %s0, %szr, %s0\n    sub %s0, %s0, #1\n", r, r, r, r, r);
        return (Expr){t, 0, -1, 0};
    }
    if (take("!")) {
        Expr e = value(unary());
        if (e.type->kind == TY_DOUBLE) emit("    add x0, x0, x0\n");
        if (wide(e.type)) {
            emit("    sub x9, x0, xzr\n    sub x0, x0, x0\n");
            comparison(&ulong_type, "==");
        } else {
            emit("    sub w9, w0, wzr\n    sub w0, w0, w0\n");
            comparison(&int_type, "==");
        }
        return (Expr){&int_type, 0, -1, 0};
    }
    int plus = take("+");
    if (plus || take("-")) {
        Expr e = value(unary());
        if (e.type->kind == TY_DOUBLE) {
            if (!plus) {
                literal64_to("x11", UINT64_C(0x8000000000000000));
                emit("    add x0, x0, x11\n");
            }
            return e;
        }
        if (!integer(e.type)) error("unary arithmetic requires an integer");
        Type *t = promote(e.type);
        if (!plus) emit(wide(t) ? "    sub x0, xzr, x0\n" : "    sub w0, wzr, w0\n");
        return (Expr){t, 0, -1, e.zero};
    }
    int inc = take("++");
    if (inc || take("--")) {
        Expr e = unary();
        if (!e.lvalue || e.type->kind == TY_ARRAY)
            error("increment and decrement require an assignable object");
        if (e.type->kind == TY_DOUBLE) error("double increment is not supported yet");
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        emit("    ldr x9, [sp]\n");
        load_value_reg(e.type, "x9");
        literal(1);
        Expr one = wide(e.type) ? (Expr){&long_type, 0, -1, 0}
                                : (Expr){&int_type, 0, -1, 0};
        if (wide(e.type)) extend_reg(&int_type, "w0", "x0");
        Expr res = binary_add((Expr){e.type, 0, -1, 0}, one, !inc);
        res = convert(res, e.type);
        emit("    ldr x11, [sp]\n    add sp, sp, #16\n");
        store_value(e.type, "x11");
        return (Expr){e.type, 0, -1, 0};
    }
    return postfix();
}
/* Multiply the left operand (w9/x9) by the right operand (w0/x0) at the
   common type's width. Two's-complement wrapping makes signed and
   unsigned multiplication identical modulo the width, so the raw bit
   patterns are multiplied: each fixed multiplier bit is tested with TBZ
   and the shifted multiplicand is accumulated into x11 with a shifted
    ADD. Carries above the width cannot corrupt the low bits, so the
    result is read back at the width's register form. */
static void emit_multiply(Type *t) {
    size_t id = next_label++;
    int bits = wide(t) ? 64 : 32;
    const char *r = wide(t) ? "x0" : "w0";
    emit("    sub x11, x11, x11\n");
    for (int i = 0; i < bits; ++i) {
        emit("    tbz %s, #%d, .Lmul%zu_%d\n", r, i, id, i);
        emit("    add x11, x11, x9, lsl #%d\n", i);
        emit(".Lmul%zu_%d:\n", id, i);
    }
    emit("    sub x0, x11, xzr\n");
}
/* Restoring unsigned division consumes one dividend bit per step. Compare
   the top bits before subtracting so unsigned 64-bit values work too.
   Signed operands use unsigned magnitudes; remainder follows the dividend. */
static void emit_divide(Type *t, int remainder) {
    size_t id = next_label++;
    int bits = wide(t) ? 64 : 32;
    const char *r = wide(t) ? "x" : "w";
    if (!wide(t)) {
        emit("    sub x13, x13, x13\n    add x9, x13, w9, uxtw\n");
        emit("    add x0, x13, w0, uxtw\n");
    }
    if (!unsigned_type(t)) {
        emit("    sub x13, x13, x13\n    sub x14, x14, x14\n");
        emit("    tbz %s9, #%d, .Ldsa%zu\n", r, bits - 1, id);
        emit("    add x13, x13, #1\n    sub %s9, %szr, %s9\n", r, r, r);
        emit(".Ldsa%zu:\n", id);
        emit("    tbz %s0, #%d, .Ldsb%zu\n", r, bits - 1, id);
        emit("    add x14, x14, #1\n    sub %s0, %szr, %s0\n", r, r, r);
        emit(".Ldsb%zu:\n", id);
    }
    emit("    sub x10, x10, x10\n    sub x11, x11, x11\n");
    for (int i = bits - 1; i >= 0; --i) {
        emit("    add x10, x10, x10\n");
        emit("    tbz %s9, #%d, .Ldva%zu_%d\n", r, i, id, i);
        emit("    add x10, x10, #1\n.Ldva%zu_%d:\n", id, i);
        emit("    tbz x10, #63, .Ldvl%zu_%d\n", id, i);
        emit("    tbz x0, #63, .Ldvt%zu_%d\n", id, i);
        emit("    cbz xzr, .Ldvc%zu_%d\n.Ldvl%zu_%d:\n", id, i, id, i);
        emit("    tbz x0, #63, .Ldvc%zu_%d\n", id, i);
        emit("    cbz xzr, .Ldvn%zu_%d\n.Ldvc%zu_%d:\n", id, i, id, i);
        emit("    sub x12, x10, x0\n");
        emit("    tbz x12, #63, .Ldvt%zu_%d\n", id, i);
        emit("    cbz xzr, .Ldvn%zu_%d\n", id, i);
        emit(".Ldvt%zu_%d:\n    sub x10, x10, x0\n", id, i);
        emit("    sub x12, x12, x12\n    add x12, x12, #1\n");
        emit("    add x11, x11, x12, lsl #%d\n", i);
        emit(".Ldvn%zu_%d:\n", id, i);
    }
    emit("    sub %s0, %s%d, %szr\n", r, r, remainder ? 10 : 11, r);
    if (!unsigned_type(t)) {
        emit("    tbz x13, #0, .Ldvs%zu\n    sub %s0, %szr, %s0\n.Ldvs%zu:\n", id, r, r, r, id);
        if (!remainder)
            emit("    tbz x14, #0, .Ldvo%zu\n    sub %s0, %szr, %s0\n.Ldvo%zu:\n", id, r, r, r, id);
    }
}
/* Multiplication and division bind more tightly than addition. */
static Expr multiplicative(void) {
    Expr e = unary();
    for (;;) {
        int star = take("*");
        int divide = !star && take("/");
        int remainder = !star && !divide && take("%");
        if (!star && !divide && !remainder) break;
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(unary());
        if (e.type->kind == TY_DOUBLE || right.type->kind == TY_DOUBLE) {
            if (remainder) error("remainder requires integers");
            convert(right, &double_type);
            emit("    ldr x9, [sp]\n    str x0, [sp]\n    sub x0, x9, xzr\n");
            convert(e, &double_type);
            emit("    ldr x1, [sp]\n    add sp, sp, #16\n");
            literal_reg("w2", !star);
            need_softfloat = 1;
            emit("    bl .L__4c_sf_binary\n");
            e = (Expr){&double_type, 0, -1, 0};
            continue;
        }
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        if (!integer(e.type) || !integer(right.type))
            error("multiplication and division require integers");
        Type *t = common_integer(e.type, right.type);
        if (wide(t)) {
            if (!wide(e.type)) extend_reg(e.type, "w9", "x9");
            if (!wide(right.type)) extend_reg(right.type, "w0", "x0");
        }
        if (star) {
            emit_multiply(t);
        } else {
            emit_divide(t, remainder);
        }
        e = (Expr){t, 0, -1, 0};
    }
    return e;
}
static Expr additive(void) {
    Expr e = multiplicative();
    while (!strcmp(current()->text, "+") || !strcmp(current()->text, "-")) {
        int subtract = take("-"); if (!subtract) expect("+");
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(multiplicative());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        e = binary_add(e, right, subtract);
    }
    return e;
}

/* Shift repeatedly by one bit. ADD doubles for left shift; right shift
   rebuilds the bit pattern using TBZ and shifted ADD, preserving a signed
   operand's top bit. Counts outside [0,width) are undefined C. */
static Expr shift_value(Expr left, Expr right, int to_right) {
    if (!integer(left.type) || !integer(right.type)) error("shift requires integers");
    Type *t = promote(left.type);
    const char *r = wide(t) ? "x" : "w";
    int bits = wide(t) ? 64 : 32;
    size_t id = next_label++;
    emit("    sub w14, w0, wzr\n.Lshift%zu:\n", id);
    emit("    cbz w14, .Lshift%zu_end\n", id);
    if (!to_right) emit("    add %s9, %s9, %s9\n", r, r, r);
    else {
        emit("    sub x11, x11, x11\n    sub x12, x12, x12\n    add x12, x12, #1\n");
        for (int i = 1; i < bits; ++i) {
            emit("    tbz %s9, #%d, .Lshift%zu_bit%d\n", r, i, id, i);
            emit("    add %s11, %s11, %s12, lsl #%d\n.Lshift%zu_bit%d:\n", r, r, r, i - 1, id, i);
        }
        if (!unsigned_type(t)) {
            emit("    tbz %s9, #%d, .Lshift%zu_sign\n", r, bits - 1, id);
            emit("    add %s11, %s11, %s12, lsl #%d\n.Lshift%zu_sign:\n", r, r, r, bits - 1, id);
        }
        emit("    sub %s9, %s11, %szr\n", r, r, r);
    }
    emit("    sub w14, w14, #1\n    cbz xzr, .Lshift%zu\n.Lshift%zu_end:\n", id, id);
    emit("    sub %s0, %s9, %szr\n", r, r, r);
    return (Expr){t, 0, -1, 0};
}
static Expr shift(void) {
    Expr e = additive();
    while (!strcmp(current()->text, "<<") || !strcmp(current()->text, ">>")) {
        int rightward = take(">>"); if (!rightward) expect("<<");
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(additive());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        e = shift_value(e, right, rightward);
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
    if (a.type->kind == TY_DOUBLE || b.type->kind == TY_DOUBLE)
        error("double comparisons are not supported yet");
    if (integer(a.type) && integer(b.type)) {
        Type *t = common_integer(a.type, b.type);
        if (wide(t)) {
            if (!wide(a.type)) extend_reg(a.type, "w9", "x9");
            if (!wide(b.type)) extend_reg(b.type, "w0", "x0");
        }
        comparison(t, op);
        return;
    }
    if (strcmp(op, "==") && strcmp(op, "!=")) {
        if (a.type->kind != TY_PTR || b.type->kind != TY_PTR ||
            !same_type(a.type, b.type) || a.type->base->kind == TY_VOID)
            error("pointer ordering requires compatible object pointers");
        comparison(&ulong_type, op);
        return;
    }
    if (!((a.type->kind == TY_PTR && b.type->kind == TY_PTR &&
           (same_type(a.type, b.type) || a.type->base->kind == TY_VOID ||
            b.type->base->kind == TY_VOID)) ||
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
    Expr e = shift();
    while (!strcmp(current()->text, "<") || !strcmp(current()->text, "<=") ||
           !strcmp(current()->text, ">") || !strcmp(current()->text, ">=")) {
        const char *op = tokens[pos++].text;
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(shift());
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
/* Construct AND/XOR one bit at a time; shifted ADD sets a bit known clear. */
static Expr bit_combine(Expr left, Expr right, int exclusive) {
    if (!integer(left.type) || !integer(right.type)) error("bitwise operation requires integers");
    Type *t = common_integer(left.type, right.type);
    const char *r = wide(t) ? "x" : "w";
    if (wide(t)) {
        if (!wide(left.type)) extend_reg(left.type, "w9", "x9");
        if (!wide(right.type)) extend_reg(right.type, "w0", "x0");
    }
    size_t id = next_label++;
    emit("    sub x11, x11, x11\n    sub x12, x12, x12\n    add x12, x12, #1\n");
    for (int bit = 0; bit < (wide(t) ? 64 : 32); ++bit) {
        if (exclusive == 2) {
            emit("    tbz %s9, #%d, .Lbit%zu_leftzero%d\n", r, bit, id, bit);
            emit("    cbz xzr, .Lbit%zu_set%d\n.Lbit%zu_leftzero%d:\n", id, bit, id, bit);
            emit("    tbz %s0, #%d, .Lbit%zu_end%d\n", r, bit, id, bit);
        } else if (exclusive) {
            emit("    tbz %s9, #%d, .Lbit%zu_leftzero%d\n", r, bit, id, bit);
            emit("    tbz %s0, #%d, .Lbit%zu_set%d\n", r, bit, id, bit);
            emit("    cbz xzr, .Lbit%zu_end%d\n.Lbit%zu_leftzero%d:\n", id, bit, id, bit);
            emit("    tbz %s0, #%d, .Lbit%zu_end%d\n", r, bit, id, bit);
        } else {
            emit("    tbz %s9, #%d, .Lbit%zu_end%d\n", r, bit, id, bit);
            emit("    tbz %s0, #%d, .Lbit%zu_end%d\n", r, bit, id, bit);
        }
        emit(".Lbit%zu_set%d:\n    add %s11, %s11, %s12, lsl #%d\n", id, bit, r, r, r, bit);
        emit(".Lbit%zu_end%d:\n", id, bit);
    }
    emit("    sub %s0, %s11, %szr\n", r, r, r);
    return (Expr){t, 0, -1, 0};
}
static Expr bit_and(void) {
    Expr e = equality();
    while (take("&")) {
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(equality());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        e = bit_combine(e, right, 0);
    }
    return e;
}
static Expr bit_xor(void) {
    Expr e = bit_and();
    while (take("^")) {
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(bit_and());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        e = bit_combine(e, right, 1);
    }
    return e;
}
/* Bitwise OR: result bit i is set when either operand's bit i is set.
   Starting from the left operand's pattern, each right-operand bit is
   tested with TBZ; if it is set and the left bit is clear, the result
   gains that bit through an immediate or literal-pool ADD. */
static Expr bit_or(void) {
    Expr e = bit_xor();
    while (take("|")) {
        size_t id = next_label++;
        e = value(e);
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(bit_xor());
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        if (!integer(e.type) || !integer(right.type))
            error("bitwise or requires integers");
        Type *t = common_integer(e.type, right.type);
        int bits = wide(t) ? 64 : 32;
        const char *a = wide(t) ? "x9" : "w9";
        const char *b = wide(t) ? "x0" : "w0";
        if (!wide(t)) {
            /* Normalize the low 32 bits; upper bits must be zero. */
            emit("    sub x10, x10, x10\n    add x9, x10, w9, uxtw\n");
        } else {
            if (!wide(e.type)) extend_reg(e.type, "w9", "x9");
            if (!wide(right.type)) extend_reg(right.type, "w0", "x0");
        }
        for (int i = 0; i < bits; ++i) {
            emit("    tbz %s, #%d, .Lbor%zu_e%d\n", b, i, id, i);
            emit("    tbz %s, #%d, .Lbor%zu_%d\n", a, i, id, i);
            emit("    cbz xzr, .Lbor%zu_e%d\n", id, i);
            emit(".Lbor%zu_%d:\n", id, i);
            if (i < 12) emit("    add %s, %s, #%d\n", a, a, 1 << i);
            else if (wide(t)) {
                literal64_to("x11", UINT64_C(1) << i);
                emit("    add %s, %s, x11\n", a, a);
            } else {
                literal_reg("w10", 1u << i);
                emit("    add %s, %s, w10\n", a, a);
            }
            emit(".Lbor%zu_e%d:\n", id, i);
        }
        if (wide(t)) emit("    sub x0, x9, xzr\n");
        else emit("    sub w0, w9, wzr\n");
        e = (Expr){t, 0, -1, 0};
    }
    return e;
}
/* Logical AND yields 1 only when both sides are nonzero; the right side
   is evaluated only after the left side proved nonzero. */
static Expr logical_and(void) {
    Expr e = bit_or();
    while (take("&&")) {
        size_t id = next_label++;
        e = condition_value(e);
        emit(wide(e.type) ? "    cbz x0, .Land%zu\n" : "    cbz w0, .Land%zu\n", id);
        Expr right = value(bit_or());
        normalize_bool(right.type);
        emit(".Land%zu:\n", id);
        e = (Expr){&int_type, 0, -1, 0};
    }
    return e;
}
/* Logical OR yields 1 when either side is nonzero; the right side runs
   only after the left side proved zero. */
static Expr logical_or(void) {
    Expr e = logical_and();
    while (take("||")) {
        size_t id = next_label++;
        e = condition_value(e);
        emit(wide(e.type) ? "    cbz x0, .Lor%zu\n" : "    cbz w0, .Lor%zu\n", id);
        literal_reg("w0", 1);
        emit("    cbz xzr, .Lor%zu_end\n.Lor%zu:\n", id, id);
        Expr right = value(logical_and());
        normalize_bool(right.type);
        emit(".Lor%zu_end:\n", id);
        e = (Expr){&int_type, 0, -1, 0};
    }
    return e;
}
/* Delay each arm's conversion until both types are known. Only the selected
   arm executes; its value reaches its own conversion block in x0. */
static Expr conditional(void) {
    Expr e = logical_or();
    if (!take("?")) return e;
    size_t id = next_label++;
    e = condition_value(e);
    emit("    cbz %s0, .Lcond%zu_false\n", wide(e.type) ? "x" : "w", id);
    Expr yes = expression();
    if (yes.type->kind != TY_VOID) yes = value(yes);
    expect(":");
    emit("    cbz xzr, .Lcond%zu_true_convert\n.Lcond%zu_false:\n", id, id);
    Expr no = conditional();
    if (no.type->kind != TY_VOID) no = value(no);
    Type *t = NULL;
    if (integer(yes.type) && integer(no.type)) t = common_integer(yes.type, no.type);
    else if ((integer(yes.type) || yes.type->kind == TY_DOUBLE) &&
             (integer(no.type) || no.type->kind == TY_DOUBLE)) t = &double_type;
    else if (same_type(yes.type, no.type)) t = yes.type;
    else if (yes.type->kind == TY_PTR && integer(no.type) && no.zero) t = yes.type;
    else if (no.type->kind == TY_PTR && integer(yes.type) && yes.zero) t = no.type;
    else if (yes.type->kind == TY_PTR && no.type->kind == TY_PTR &&
             (yes.type->base->kind == TY_VOID || no.type->base->kind == TY_VOID))
        t = pointer(&void_type);
    else error("incompatible conditional operands");
    if (t->kind != TY_VOID) convert(no, t);
    emit("    cbz xzr, .Lcond%zu_end\n.Lcond%zu_true_convert:\n", id, id);
    if (t->kind != TY_VOID) convert(yes, t);
    emit(".Lcond%zu_end:\n", id);
    return (Expr){t, 0, -1, 0};
}
/* Assignment expression: conditional expressions joined by '=' and compound
   assignment operators. Call arguments and subscripts stop at this level so
   that a ',' separates them instead of forming a comma expression. */
static Expr assignment_expr(void) {
    Expr e = conditional();
    if (take("=")) {
        if (!e.lvalue || e.type->kind == TY_ARRAY)
            error("assignment requires a local variable or dereferenced object on the left");
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = convert(assignment_expr(), e.type);
        emit("    ldr x9, [sp]\n    add sp, sp, #16\n");
        store_value(e.type, "x9");
        right.zero = 0; /* An assignment is not an integer constant expression. */
        return right;
    }
    const char *op = current()->text;
    if (!strcmp(op, "+=") || !strcmp(op, "-=") || !strcmp(op, "*=") ||
        !strcmp(op, "/=") || !strcmp(op, "%=") || !strcmp(op, "&=") ||
        !strcmp(op, "^=") || !strcmp(op, "|=") || !strcmp(op, "<<=") || !strcmp(op, ">>=")) {
        ++pos;
        if (!e.lvalue || e.type->kind == TY_ARRAY)
            error("compound assignment requires an assignable object");
        /* Keep the destination address across evaluation of the right operand. */
        emit("    sub sp, sp, #16\n    str x0, [sp]\n");
        Expr right = value(assignment_expr());
        emit("    ldr x9, [sp]\n");
        load_value_reg(e.type, "x9");
        Expr left = (Expr){e.type, 0, -1, 0}, res;
        if (*op == '+' || *op == '-') res = binary_add(left, right, *op == '-');
        else if (*op == '<' || *op == '>') res = shift_value(left, right, *op == '>');
        else if (*op == '&' || *op == '^' || *op == '|')
            res = bit_combine(left, right, *op == '|' ? 2 : *op == '^');
        else {
            if (!integer(left.type) || !integer(right.type))
                error("compound multiplication, division and remainder currently require integers");
            Type *t = common_integer(left.type, right.type);
            if (wide(t)) {
                if (!wide(left.type)) extend_reg(left.type, "w9", "x9");
                if (!wide(right.type)) extend_reg(right.type, "w0", "x0");
            }
            if (*op == '*') emit_multiply(t);
            else emit_divide(t, *op == '%');
            res = (Expr){t, 0, -1, 0};
        }
        convert(res, e.type);
        emit("    ldr x11, [sp]\n    add sp, sp, #16\n");
        store_value(e.type, "x11");
        return (Expr){e.type, 0, -1, 0};
    }
    return e;
}
/* Full expression: assignment expressions joined by the comma operator.
   Each left operand is evaluated for side effects and discarded. */
static Expr expression(void) {
    Expr e = assignment_expr();
    while (take(",")) {
        if (e.type->kind != TY_VOID) value(e);
        e = assignment_expr();
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
        if (type_start() || !strcmp(current()->text, "typedef") || !strcmp(current()->text, "static")) {
            int is_alias = take("typedef");
            int is_static = take("static");
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
                type = array_suffix(type, 0, is_static);
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
                } else if (is_static) {
                    if (nglobals == 256) error("too many static objects");
                    if (type->kind == TY_VOID) error("object cannot have void type");
                    size_t g = nglobals++;
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "__4c_static_%zu", g);
                    globals[g] = (Local){copy(buffer, strlen(buffer)), 0, type, 1, 0};
                    global_static[g] = 1;
                    locals[nlocals++] = (Local){s, -4, type, 1, (int)g};
                    if (take("=")) {
                        size_t room = ntokens - pos + 1;
                        InitEntry *entries = resize(NULL, room * sizeof(InitEntry));
                        size_t count;
                        if (take("{")) {
                            count = parse_initializer(type, entries, room);
                            expect("}");
                        } else count = parse_one_initializer(type, entries, room);
                        global_inits[g] = entries;
                        global_init_counts[g] = count;
                    }
                    if (!type->size) error("static object has incomplete type");
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
                    int initialized_member[32] = {0};
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
                            convert(assignment_expr(), type->base);
                            emit("    add x9, x29, #%d\n", offset + i++ * type->base->size);
                            store_value(type->base, "x9");
                            if (!take(",")) break;
                        } while (strcmp(current()->text, "}"));
                        expect("}");
                        for (; i < count; ++i) {
                            emit("    sub x0, x0, x0\n    add x9, x29, #%d\n", offset + i * type->base->size);
                            store_value(type->base, "x9");
                        }
                    } else if (initialize && (type->kind == TY_STRUCT ||
                                              type->kind == TY_UNION)) {
                        /* Positional brace initializer for a local record:
                           one assignment-expression per member in order. */
                        if (!strcmp(current()->text, "{")) {
                        expect("{");
                        for (int m = 0; m < type->nmembers; ++m) {
                            if (m > 0) {
                                if (!take(",")) break;
                                if (!strcmp(current()->text, "}")) break;
                            }
                            Member *mem = &type->members[m];
                            int moff = offset + mem->offset;
                            convert(assignment_expr(), mem->type);
                            emit("    add x9, x29, #%d\n", moff);
                            store_value(mem->type, "x9");
                            initialized_member[m] = 1;
                        }
                        take(",");
                        expect("}");
                        /* Zero-fill members after the last initializer. */
                        for (int m = 0; m < type->nmembers; ++m)
                            if (!initialized_member[m]) {
                                /* Zero the member's bytes with a scalar loop. */
                                Type *mt = type->members[m].type;
                                int words = (mt->size + 7) / 8;
                                for (int w = 0; w < words; ++w) {
                                    emit("    sub x0, x0, x0\n");
                                    emit("    add x9, x29, #%d\n",
                                         offset + type->members[m].offset + w * 8);
                                    int rem = mt->size - w * 8;
                                    if (rem >= 8) {
                                        emit("    str x0, [x9]\n");
                                    } else {
                                        for (int b = 0; b < rem; ++b) {
                                            emit("    add x9, x29, #%d\n",
                                                 offset + type->members[m].offset + w * 8 + b);
                                            emit("    strb w0, [x9]\n");
                                        }
                                    }
                                }
                            }
                        } else {
                            /* Aggregate initializer from an expression, such
                               as a call returning the record by value. */
                            convert(assignment_expr(), type);
                            emit("    add x9, x29, #%d\n", offset);
                            store_value(type, "x9");
                        }
                    } else if (initialize) {
                        convert(assignment_expr(), type);
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
        expect("("); Expr cond = condition_value(expression()); expect(")");
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
        expect("("); Expr cond = condition_value(expression()); expect(")");
        emit("    cbz %s0, .Lendwhile%zu\n", wide(cond.type) ? "x" : "w", id);
        push_break(".Lendwhile%zu", id);
        push_continue(".Lwhile%zu", id);
        statement();
        pop_break();
        --continue_depth;
        emit("    cbz xzr, .Lwhile%zu\n.Lendwhile%zu:\n", id, id);
        return;
    }
    if (take("for")) {
        size_t id = next_label++;
        condition_comment(start);
        expect("(");
        size_t saved_locals = nlocals, saved_base = scope_base, saved_tags = ntags;
        int saved_frame = frame_bytes;
        scope_base = nlocals;
        ++tag_depth;
        if (type_start()) {
            Type *base = parse_specs();
            for (;;) {
                Type *type = consume_stars(base);
                char *s = name();
                Type *dec = array_suffix(type, 0, 0);
                if (dec->kind == TY_VOID) error("for-loop variable cannot have void type");
                if (!dec->size) error("for-loop variable has incomplete type");
                int previous = find_local(s);
                if (previous >= 0 && (size_t)previous >= scope_base) error("duplicate loop variable");
                if (nlocals == 256) error("too many active local variables");
                size_t local = nlocals++;
                int offset = allocate(dec);
                locals[local] = (Local){s, offset, dec, 0, 0};
                if (take("=")) {
                    convert(assignment_expr(), dec);
                    emit("    add x9, x29, #%d\n", offset);
                    store_value(dec, "x9");
                }
                locals[local].initialized = 1;
                if (!take(",")) break;
            }
        } else if (strcmp(current()->text, ";")) {
            Expr init = expression();
            if (init.type->kind != TY_VOID) value(init);
        }
        expect(";");
        emit(".Lfor%zu:\n", id);
        if (strcmp(current()->text, ";")) {
            Expr cond = condition_value(expression());
            emit("    cbz %s0, .Lendfor%zu\n", wide(cond.type) ? "x" : "w", id);
        }
        expect(";");
        /* Parse the step now but emit it after the body: buffer its code. */
        FILE *main_stream = body, *step_buffer = NULL;
        if (strcmp(current()->text, ")")) {
            step_buffer = tmpfile();
            if (!step_buffer) fatal("cannot create step buffer: %s", strerror(errno));
            body = step_buffer;
            Expr step = expression();
            if (step.type->kind != TY_VOID) value(step);
        }
        expect(")");
        body = main_stream;
        push_break(".Lendfor%zu", id);
        push_continue(".Lfor_step%zu", id);
        statement();
        pop_break();
        --continue_depth;
        emit(".Lfor_step%zu:\n", id);
        if (step_buffer) {
            if (fflush(step_buffer) || fseek(step_buffer, 0, SEEK_SET))
                fatal("cannot rewind step buffer");
            int sch;
            while ((sch = fgetc(step_buffer)) != EOF) fputc(sch, body);
            if (ferror(body)) fatal("cannot write assembly buffer");
            fclose(step_buffer);
        }
        emit("    cbz xzr, .Lfor%zu\n.Lendfor%zu:\n", id, id);
        nlocals = saved_locals;
        scope_base = saved_base;
        frame_bytes = saved_frame;
        ntags = saved_tags;
        --tag_depth;
        return;
    }
    if (take("do")) {
        size_t id = next_label++;
        emit(".Ldo%zu:\n", id);
        push_break(".Ldo_end%zu", id);
        push_continue(".Ldo_cond%zu", id);
        statement();
        pop_break();
        --continue_depth;
        expect("while");
        emit(".Ldo_cond%zu:\n", id);
        condition_comment(pos);
        expect("("); Expr cond = condition_value(expression()); expect(")");
        emit("    cbz %s0, .Ldo_end%zu\n", wide(cond.type) ? "x" : "w", id);
        emit("    cbz xzr, .Ldo%zu\n.Ldo_end%zu:\n", id, id);
        expect(";");
        return;
    }
    if (take("switch")) {
        size_t id = next_label++;
        condition_comment(start);
        expect("(");
        Expr v = value(expression());
        expect(")");
        if (!(integer(v.type) || v.type->kind == TY_PTR))
            error("switch requires an integer or pointer expression");
        if (integer(v.type)) v = convert(v, promote(v.type));
        int slot = allocate(v.type);
        emit("    add x10, x29, #%d\n", slot);
        store_value(v.type, "x10");
        emit("    cbz xzr, .Lsw%zu_disp\n", id);
        if (switch_depth + 1 >= (int)(sizeof(switch_ids) / sizeof(switch_ids[0])))
            error("switch nesting is too deep");
        ++switch_depth;
        switch_ids[switch_depth] = id;
        switch_case_count[switch_depth] = 0;
        switch_has_default[switch_depth] = 0;
        push_break(".Lsw%zu_end", id);
        statement();
        pop_break();
        emit("    cbz xzr, .Lsw%zu_end\n", id);
        emit(".Lsw%zu_disp:\n", id);
        emit("    add x9, x29, #%d\n", slot);
        if (wide(v.type)) emit("    ldr x9, [x9]\n");
        else emit("    ldr w9, [x9]\n");
        int count = switch_case_count[switch_depth];
        for (int i = 0; i < count; ++i) {
            int val = switch_cases[switch_depth][i].value;
            size_t label = switch_cases[switch_depth][i].label;
            literal_reg("w0", (uint32_t)val);
            if (wide(v.type)) {
                emit("    sub x10, x10, x10\n    add x0, x10, w0, sxtw\n");
            }
            comparison(v.type, "==");
            emit("    cbz w0, .Lsw%zu_n%d\n", id, i);
            emit("    cbz xzr, .Lcse%zu_%zu\n", id, label);
            emit(".Lsw%zu_n%d:\n", id, i);
        }
        if (switch_has_default[switch_depth]) emit("    cbz xzr, .Ldfl%zu\n", id);
        else emit("    cbz xzr, .Lsw%zu_end\n", id);
        emit(".Lsw%zu_end:\n", id);
        --switch_depth;
        return;
    }
    annotate_statement();
    if (take("break")) {
        expect(";");
        if (!break_depth) error("break outside a loop or switch");
        emit("    cbz xzr, %s\n", break_names[break_depth - 1]);
        return;
    }
    if (take("continue")) {
        expect(";");
        if (!continue_depth) error("continue outside a loop");
        emit("    cbz xzr, %s\n", continue_names[continue_depth - 1]);
        return;
    }
    if (take("case") || take("default")) {
        int is_default = !strcmp(tokens[pos - 1].text, "default");
        if (!switch_depth)
            error(is_default ? "default label outside a switch"
                             : "case label outside a switch");
        int value = 0;
        if (!is_default) {
            if (switch_case_count[switch_depth] >= MAX_CASES)
                error("too many case labels");
            value = enum_value();
        }
        expect(":");
        size_t id = switch_ids[switch_depth];
        if (is_default) {
            if (switch_has_default[switch_depth]) error("duplicate default label");
            switch_has_default[switch_depth] = 1;
            emit(".Ldfl%zu:\n", id);
        } else {
            size_t label = next_label++;
            for (int i = 0; i < switch_case_count[switch_depth]; ++i)
                if (switch_cases[switch_depth][i].value == value)
                    error("duplicate case value");
            switch_cases[switch_depth][switch_case_count[switch_depth]++] =
                (CaseLabel){label, value};
            emit(".Lcse%zu_%zu:\n", id, label);
        }
        /* Fallthrough: the following statements belong to this case. */
        return;
    }
    if (take("return")) {
        Type *rt = functions[current_function].result;
        if (rt->kind == TY_VOID) {
            if (strcmp(current()->text, ";")) error("void function cannot return a value");
        } else {
            if (!strcmp(current()->text, ";")) error("non-void function must return a value");
            convert(expression(), rt);
            if (aggregate(rt)) {
                if (hfa_count(rt)) {
                    for (int j = 0; j < hfa_count(rt); ++j)
                        emit("    ldr d%d, [x0, #%d]\n", j, j * 8);
                } else if (rt->size > 16) {
                    /* x8 held the caller's result buffer; copy bytes there. */
                    emit("    ldr x9, [x29, #%d]\n",
                         functions[current_function].result_ptr_offset);
                    copy_object(rt, "x9", "x0");
                } else {
                    /* Pack only the object's bytes, avoiding an overread of
                       short or oddly-sized records at a page boundary. */
                    emit("    sub x9, x0, xzr\n");
                    for (int reg = 0; reg < (rt->size + 7) / 8; ++reg) {
                        emit("    sub x%d, x%d, x%d\n", reg, reg, reg);
                        for (int j = 0; j < 8 && reg * 8 + j < rt->size; ++j)
                            emit("    ldrb w13, [x9, #%d]\n    add x%d, x%d, x13, lsl #%d\n",
                                 reg * 8 + j, reg, reg, j * 8);
                    }
                }
            }
        }
        expect(";");
        emit("    cbz xzr, .Lreturn%zu\n", current_function);
    } else if (!take(";")) {
        Expr e = expression();
        if (e.type->kind != TY_VOID) value(e);
        expect(";");
    }
}

/* Count encoded instructions in the finished body, including buffered loop
   steps. Comments and label spelling do not consume instruction range. */
static size_t function_code_bytes(void) {
    if (fflush(body) || fseek(body, 0, SEEK_SET)) fatal("cannot rewind assembly buffer");
    size_t bytes = 0;
    int ch, start = 1;
    while ((ch = fgetc(body)) != EOF) {
        if (ch == '\n') { start = 1; continue; }
        if (!start || isspace(ch)) continue;
        start = 0;
        if (ch >= 'a' && ch <= 'z') bytes += 4;
    }
    if (ferror(body)) fatal("cannot measure assembly buffer");
    return bytes;
}

static void emit_function(size_t start, size_t open, size_t first_constant) {
    Function *fn = &functions[current_function];
    const char *prefix = fn->internal ? ".L" : macos ? "_" : "";
    int frame = (max_frame_bytes + 15) & ~15;
    FILE *out = program;
    fprintf(out, ".text\n.p2align 2\n.set .Lframe%zu, %d\n", current_function, frame);
    if (!fn->internal && !fn->static_linkage) fprintf(out, ".globl %s%s\n", prefix, fn->name);
    if (!macos) fprintf(out, ".type %s%s, %%function\n", prefix, fn->name);
    source_comment(out, start, open);
    fprintf(out, "%s%s:\n"
                 "    sub sp, sp, #%d\n"
                 "    str x29, [sp]\n"
                 "    str x30, [sp, #8]\n"
                 "    add x29, sp, #0\n", prefix, fn->name, frame);
    /* Save incoming registers before copying named parameters to their own
       slots. FP and GP argument sequences advance independently. */
    if (fn->variadic) {
        int save_off = fn->va_save_offset;
        for (int i = 0; i < 8; ++i)
            fprintf(out, "    str x%d, [x29, #%d]\n", i, save_off + i * 8);
        int fp_off = fn->fp_save_offset;
        for (int i = 0; i < 8; ++i)
            fprintf(out, "    str d%d, [x29, #%d]\n", i, fp_off + i * 16);
    }
    /* Save the indirect-result pointer for functions returning large
       aggregates; the caller's buffer address arrives in x8. */
    if (fn->result_ptr_offset)
        fprintf(out, "    str x8, [x29, #%d]\n", fn->result_ptr_offset);
    int gp = 0, fp = 0, stack_running = 0;
    for (int i = 0; i < fn->nparams; ++i) {
        Type *pt = fn->params[i];
        int off = locals[i].offset;
        ArgPlace place = argument_place(pt, &gp, &fp, &stack_running);
        if (place.indirect || place.stack >= 0) {
            if (place.stack >= 0) {
                fprintf(out, "    add x9, x29, #%d\n", frame);
                if (place.stack) fprintf(out, "    add x9, x9, #%d\n", place.stack);
                if (place.indirect) fprintf(out, "    ldr x9, [x9]\n");
            } else fprintf(out, "    sub x9, x%d, xzr\n", place.gp);
            for (int j = 0; j < pt->size; ++j)
                fprintf(out, "    ldrb w12, [x9, #%d]\n    strb w12, [x29, #%d]\n", j, off + j);
        } else if (aggregate(pt)) {
            for (int j = 0; j < place.count; ++j)
                fprintf(out, "    str %s%d, [x29, #%d]\n", place.fp >= 0 ? "d" : "x",
                        (place.fp >= 0 ? place.fp : place.gp) + j, off + j * 8);
        } else {
            fprintf(out, "    %s %s%d, [x29, #%d]\n",
                    pt->size == 1 ? "strb" : "str",
                    place.fp >= 0 ? "d" : wide(pt) ? "x" : "w",
                    place.fp >= 0 ? place.fp : place.gp, off);
        }
    }
    if (fflush(body) || fseek(body, 0, SEEK_SET)) fatal("cannot rewind assembly buffer");
    int ch;
    while ((ch = fgetc(body)) != EOF) fputc(ch, out);
    if (ferror(body)) fatal("cannot read assembly buffer");
    fprintf(out, "    // Shared function epilogue.\n.Lreturn%zu:\n", current_function);
    if (fn->result->kind == TY_DOUBLE)
        fprintf(out, "    sub sp, sp, #16\n    str x0, [sp]\n    ldr d0, [sp]\n    add sp, sp, #16\n");
    fprintf(out, "    ldr x30, [sp, #8]\n"
                 "    ldr x29, [sp]\n"
                 "    add sp, sp, #%d\n"
                 "    ret\n", frame);
    if (!macos) fprintf(out, ".size %s%s, .-%s%s\n", prefix, fn->name, prefix, fn->name);
    for (size_t i = first_constant; i < nconstants; ++i)
        fprintf(out, ".LC%zu:\n    .word 0x%08lx\n", i, (unsigned long)constants[i]);
}

static void parse(void) {
    int have_main = 0;
    global_scope = 1;
    for (;;) {
        if (!strcmp(current()->text, "<eof>")) {
            if (!need_softfloat || compiling_softfloat) break;
            compiling_softfloat = 1;
            tokens = NULL; ntokens = capacity = pos = 0;
            size_t length = 0;
            for (size_t i = 0; i < sizeof(softfloat_source)/sizeof(*softfloat_source); ++i)
                length += strlen(softfloat_source[i]);
            char *source = resize(NULL, length + 1), *end = source;
            for (size_t i = 0; i < sizeof(softfloat_source)/sizeof(*softfloat_source); ++i) {
                size_t n = strlen(softfloat_source[i]);
                memcpy(end, softfloat_source[i], n); end += n;
            }
            *end = 0;
            lex_source("<4c software binary64>", source, 0);
        }
        if (!strcmp(current()->text, "_Static_assert")) {
            ++pos;
            expect("(");
            uint64_t value = 0;
            suppress_emit = 1;
            const_expr_or(&value);
            suppress_emit = 0;
            expect(",");
            if (current()->text[0] != '"') error("_Static_assert message must be a string literal");
            String msg = decode_literal();
            expect(")");
            expect(";");
            if (!value)
                fatal("static assertion failed: %.*s", (int)msg.length, msg.data);
            continue;
        }
        size_t start = pos;
        int is_alias = take("typedef");
        int is_static = take("static");
        int is_extern = take("extern");
        if (is_static && (is_extern || is_alias)) error("conflicting storage classes");
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
            expect("(");
            char *params[8] = {0};
            Type *param_types[8] = {0};
            int nparams = 0, empty = 0, variadic = 0;
            if (take(")")) {
                empty = 1;
            } else if ((alias_type(current()->text) == &void_type || !strcmp(current()->text, "void")) &&
                       !strcmp(tokens[pos + 1].text, ")")) {
                ++pos;
                expect(")");
            } else {
                for (;;) {
                    if (nparams == 8) error("at most eight scalar parameters are supported");
                    if (!strcmp(current()->text, "...")) {
                        if (!nparams)
                            error("variadic functions require at least one fixed parameter");
                        variadic = 1;
                        ++pos;
                        break;
                    }
                    Type *param_base = parse_specs();
                    param_types[nparams] = parse_declarator(param_base, &params[nparams], 1);
                    for (int i = 0; i < nparams; ++i)
                        if (params[i] && params[nparams] && !strcmp(params[i], params[nparams]))
                            error("duplicate parameter name");
                    if (param_types[nparams]->kind == TY_VOID) error("parameter cannot have void type");
                    if (params[nparams])
                        locals[nlocals++] = (Local){params[nparams], 0, param_types[nparams], 1, 0};
                    ++nparams;
                    if (!take(",")) break;
                }
                expect(")");
            }
            nlocals = 0; /* End the prototype parameter scope. */
            int definition = !strcmp(current()->text, "{");
            if (empty && !definition)
                error("use (void) for a zero-parameter prototype");
            if (!strcmp(s, "main") && result->kind != TY_INT) error("main must return int");
            int index = find_function(s);
            if (index < 0) {
                if (nfunctions == (compiling_softfloat ? 288u : 256u)) error("too many function declarations");
                index = (int)nfunctions++;
                functions[index] = (Function){.name=s, .nparams=nparams, .variadic=variadic,
                    .result=result, .internal=compiling_softfloat, .static_linkage=is_static};
                for (int i = 0; i < nparams; ++i) functions[index].params[i] = param_types[i];
            } else if (functions[index].nparams != nparams ||
                       functions[index].variadic != variadic ||
                       !same_type(functions[index].result, result)) {
                error("conflicting function declaration");
            }
            if (is_static && !functions[index].static_linkage)
                error("static declaration follows external function declaration");
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
            /* For variadic definitions, allocate a save area for x0..x7, one
               for d0..d7, and a __va_list_tag in the frame. The GP save area
               overlaps named-parameter slots (x_i = named param i). */
            if (variadic) {
                functions[index].va_save_offset = 16;
                functions[index].fp_save_offset = 16 + 64;
                int overhead = functions[index].fp_save_offset + 128;
                frame_bytes = max_frame_bytes = overhead;
            }
            global_scope = 0;
            for (int i = 0; i < nparams; ++i) {
                if (!params[i]) error("function definitions require parameter names");
                if (variadic && (param_types[i]->kind == TY_STRUCT ||
                                 param_types[i]->kind == TY_UNION))
                    error("variadic functions cannot take aggregate parameters yet");
                locals[i] = (Local){params[i], allocate(param_types[i]), param_types[i], 1, 0};
            }
            /* Body locals must start above the variadic save areas so that
               va_list bookkeeping is not clobbered by user locals. */
            if (variadic) {
                int overhead = functions[index].fp_save_offset + 128;
                if (frame_bytes < overhead) frame_bytes = overhead;
                if (max_frame_bytes < overhead) max_frame_bytes = overhead;
            }
            /* Functions returning aggregates larger than sixteen bytes use
               the x8 indirect-result convention: reserve a slot to hold the
               caller's buffer address across the body. */
            if (indirect_aggregate(result))
                functions[index].result_ptr_offset = allocate(&long_type);
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
            size_t code_bytes = function_code_bytes();
            /* LDR literals and CBZ have a signed one-megabyte reach. Reserve
               4 KiB for the prologue, epilogue and alignment around the body. */
            if (code_bytes + (nconstants - first_constant) * 4 + 4096 >= 1048576)
                fatal("function %s exceeds code-size limit (%zu instruction bytes, %zu constants)",
                      s, code_bytes, nconstants - first_constant);
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
            if (is_extern) {
                if (take("=")) error("extern declarations cannot have an initializer");
            }
            int initialized = 0;
            InitEntry *init_entries = NULL;
            size_t init_count = 0;
            if (!is_alias && take("=")) {
                initialized = 1;
                int nentries = (type->size ? type->size : 8) + 16;
                init_entries = resize(NULL, nentries * sizeof(InitEntry));
                if (!strcmp(current()->text, "{")) {
                    ++pos;
                    init_count = parse_initializer(type, init_entries, nentries);
                    if (!take("}")) error("expected '}' at end of initializer");
                } else {
                    init_count = parse_one_initializer(type, init_entries, nentries);
                }
            }
            if (previous >= 0 && !is_alias &&
                ((is_static && !global_static[previous]) ||
                 (!is_static && !is_extern && global_static[previous])))
                error("conflicting object linkage");
            if (previous < 0) {
                if (nglobals == 256) error("too many global declarations");
                previous = (int)nglobals++;
                globals[previous] = (Local){s, is_alias ? -1 : (is_extern ? -3 : 0), type, 0, 0};
                global_static[previous] = is_static;
            }
            if (!is_alias && !is_extern && globals[previous].offset == -3)
                globals[previous].offset = 0;
            if (initialized) {
                if (globals[previous].initialized) error("duplicate global definition");
                globals[previous].initialized = 1;
                global_inits[previous] = init_entries;
                global_init_counts[previous] = init_count;
            }
            if (!take(",")) break;
            result = consume_stars(base);
            s = name();
        }
        expect(";");
    }
    if (!have_main) error("expected an int main() definition");
    if (need_softfloat) {
        /* One-bit unsigned shift, synthesized from bit tests and shifted ADD.
           This primitive is private to the software routines; it is not C >>. */
        fprintf(program, ".text\n.p2align 2\n.type .L__4c_sf_shr1, %%function\n.L__4c_sf_shr1:\n"
                         "    sub x9, x9, x9\n    sub x10, x10, x10\n    add x10, x10, #1\n");
        for (int bit = 1; bit < 64; ++bit)
            fprintf(program, "    tbz x0, #%d, .Lsf_shift%d\n    add x9, x9, x10, lsl #%d\n.Lsf_shift%d:\n",
                    bit, bit, bit - 1, bit);
        fprintf(program, "    sub x0, x9, xzr\n    ret\n.size .L__4c_sf_shr1, .-.L__4c_sf_shr1\n");
    }
    for (size_t i = 0; i < nglobals; ++i) {
        Local *g = &globals[i];
        if (g->offset < 0) continue; /* typedefs and enum constants */
        const char *prefix = macos ? "_" : "";
        int a = alignment(g->type), p2 = 0;
        while ((1 << p2) < a) ++p2;
        fprintf(program, ".data\n.p2align %d\n", p2);
        if (!global_static[i]) fprintf(program, ".globl %s%s\n", prefix, g->name);
        if (!macos) fprintf(program, ".type %s, %%object\n.size %s, %d\n", g->name, g->name, g->type->size);
        fprintf(program, "%s%s:\n", prefix, g->name);
        if (g->initialized && global_inits[i]) {
            size_t n = global_init_counts[i];
            InitEntry *entries = global_inits[i];
            /* Lay the initializer out at the type's true member offsets so
               struct padding between fields is preserved (e.g. the pad after
               an int kind member before an 8-byte pointer member). */
            size_t cursor = 0;
            if (g->type->kind == TY_STRUCT || g->type->kind == TY_UNION) {
                for (int m = 0; m < g->type->nmembers; ++m) {
                    Member *mem = &g->type->members[m];
                    size_t moff = (size_t)mem->offset;
                    size_t msize = (size_t)mem->type->size;
                    while (cursor < moff) {
                        fprintf(program, "    .byte 0\n");
                        ++cursor;
                    }
                    if (m < (int)n) emit_init_entry(entries[m]);
                    cursor += msize;
                }
            } else {
                for (size_t j = 0; j < n; ++j) emit_init_entry(entries[j]);
                for (size_t j = 0; j < n; ++j) {
                    Type *t = entries[j].type;
                    if (!t) t = &int_type;
                    switch (t->kind) {
                    case TY_PTR: case TY_ULONG: case TY_LONG: case TY_DOUBLE:
                        cursor += 8; break;
                    case TY_INT: case TY_UINT:
                        cursor += 4; break;
                    case TY_BOOL: case TY_CHAR: case TY_SCHAR: case TY_UCHAR:
                        cursor += 1; break;
                    case TY_SHORT: case TY_USHORT:
                        cursor += 2; break;
                    default:
                        cursor += 4; break;
                    }
                }
            }
            while (cursor < (size_t)g->type->size) {
                fprintf(program, "    .byte 0\n");
                ++cursor;
            }
        } else {
            fprintf(program, "    .zero %d\n", g->type->size);
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

#include "asm_comments.h"

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
    preprocess();
    program = tmpfile();
    if (!program) fatal("cannot create program buffer: %s", strerror(errno));
    parse();
    FILE *out = output ? fopen(output, "w") : stdout;
    if (!out) fatal("cannot open output %s: %s", output, strerror(errno));
    if (fflush(program) || fseek(program, 0, SEEK_SET)) fatal("cannot rewind program buffer");
    write_annotated_assembly(program, out);
    if (ferror(program)) fatal("cannot read program buffer");
    if (fflush(out) || ferror(out)) fatal("cannot write assembly output");
    if (output && fclose(out)) fatal("cannot close assembly output");
    fclose(program);
    return 0;
}
