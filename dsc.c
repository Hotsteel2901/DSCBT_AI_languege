/* DSC Language - Bytecode Interpreter v3.0
 * A Python-like scripting language with a built-in bytecode VM.
 *
 * v3.0 is a performance/safety rewrite aligned with Rust-grade engineering:
 *   - Result-style error propagation, no exit() from the front end
 *   - Bump-arena allocation for tokens/AST/interned strings
 *   - 8-byte instructions, fused opcodes, constant folding, DCE
 *   - 16-byte tagged values, stack frames on the value stack (zero-copy calls)
 *   - Buffered output, fast integer formatting, shortest-roundtrip floats
 *   - Full bounds/type/overflow checks and a leak-free teardown path
 *
 * Build: gcc -O2 -o dsc dsc.c
 * Usage: dsc <source.dscbt>
 */

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define DSC_LIKELY(x) (x)
#define DSC_UNLIKELY(x) (x)
#define DSC_NORETURN __declspec(noreturn)
#else
#define DSC_LIKELY(x) __builtin_expect(!!(x), 1)
#define DSC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define DSC_NORETURN __attribute__((noreturn))
#endif

/* ================================================================
 *  Section 1: Errors and allocation
 * ================================================================ */

typedef struct {
    int line;
    int col;
    char msg[256];
} DscErr;

static void err_set(DscErr* e, int line, int col, const char* fmt, ...) {
    va_list ap;
    if (e->msg[0]) return; /* keep the first error */
    e->line = line;
    e->col = col;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

static int err_has(const DscErr* e) { return e->msg[0] != 0; }

static DSC_NORETURN void oom(void) {
    fputs("错误: 内存不足\n", stderr);
    exit(4);
}

static void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

static void* xcalloc(size_t n, size_t sz) {
    void* p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

/* ================================================================
 *  Section 2: Arena allocator
 * ================================================================ */

typedef struct ABlock {
    struct ABlock* next;
    size_t used;
    size_t cap;
    unsigned char* mem;
} ABlock;

typedef struct {
    ABlock* head;
} Arena;

#define ARENA_BLOCK (64u * 1024u)

static void* arena_alloc(Arena* a, size_t n) {
    ABlock* b = a->head;
    size_t need = (n + 15u) & ~(size_t)15u;
    if (!b || b->used + need > b->cap) {
        size_t cap = ARENA_BLOCK;
        if (need > cap / 2) cap = need;
        b = xmalloc(sizeof(ABlock));
        b->mem = xmalloc(cap);
        b->cap = cap;
        b->used = 0;
        b->next = a->head;
        a->head = b;
    }
    {
        void* p = b->mem + b->used;
        b->used += need;
        return p;
    }
}

static char* arena_strndup(Arena* a, const char* s, size_t n) {
    char* p = arena_alloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void arena_free_all(Arena* a) {
    ABlock* b = a->head;
    while (b) {
        ABlock* next = b->next;
        free(b->mem);
        free(b);
        b = next;
    }
    a->head = NULL;
}

/* ================================================================
 *  Section 3: Interning and hashing
 * ================================================================ */

static uint64_t hash_bytes(const void* data, size_t n) {
    const unsigned char* p = data;
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint32_t hash_ptr(const void* p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

typedef struct {
    const char* s;
    uint32_t len;
    uint64_t hash;
} IEnt;

typedef struct {
    IEnt* slots;
    uint32_t cap;
    uint32_t count;
} Interner;

static void interner_init(Interner* it) {
    it->cap = 256;
    it->count = 0;
    it->slots = xcalloc(it->cap, sizeof(IEnt));
}

static void interner_rehash(Interner* it) {
    uint32_t oldcap = it->cap;
    IEnt* old = it->slots;
    uint32_t i;
    it->cap = oldcap * 2;
    it->slots = xcalloc(it->cap, sizeof(IEnt));
    for (i = 0; i < oldcap; i++) {
        if (old[i].s) {
            uint32_t j = (uint32_t)(old[i].hash & (it->cap - 1));
            while (it->slots[j].s) j = (j + 1) & (it->cap - 1);
            it->slots[j] = old[i];
        }
    }
    free(old);
}

static const char* intern_n(Interner* it, Arena* ar, const char* s, size_t len) {
    uint64_t h;
    uint32_t idx;
    if (len > UINT32_MAX) oom();
    h = hash_bytes(s, len);
    if ((it->count + 1) * 4 >= it->cap * 3) interner_rehash(it);
    idx = (uint32_t)(h & (it->cap - 1));
    for (;;) {
        IEnt* e = &it->slots[idx];
        if (!e->s) {
            char* copy = arena_strndup(ar, s, len);
            e->s = copy;
            e->len = (uint32_t)len;
            e->hash = h;
            it->count++;
            return copy;
        }
        if (e->hash == h && e->len == len && !memcmp(e->s, s, len))
            return e->s;
        idx = (idx + 1) & (it->cap - 1);
    }
}

static const char* intern_str(Interner* it, Arena* ar, const char* s) {
    return intern_n(it, ar, s, strlen(s));
}

static void interner_free(Interner* it) {
    free(it->slots);
    it->slots = NULL;
    it->cap = 0;
    it->count = 0;
}

/* Pointer-keyed map: interned strings -> int32 payload. */
#define PMAP_NONE INT32_MIN

typedef struct {
    const char** keys;
    int32_t* vals;
    uint32_t cap;
    uint32_t count;
} PMap;

static void pmap_init(PMap* m) {
    m->cap = 0;
    m->count = 0;
    m->keys = NULL;
    m->vals = NULL;
}

static void pmap_rehash(PMap* m, uint32_t cap) {
    const char** oldkeys = m->keys;
    int32_t* oldvals = m->vals;
    uint32_t oldcap = m->cap;
    uint32_t i;
    m->cap = cap;
    m->keys = xcalloc(cap, sizeof(char*));
    m->vals = xmalloc(sizeof(int32_t) * cap);
    for (i = 0; i < cap; i++) m->vals[i] = PMAP_NONE;
    for (i = 0; i < oldcap; i++) {
        if (oldkeys[i]) {
            uint32_t j = hash_ptr(oldkeys[i]) & (cap - 1);
            while (m->keys[j]) j = (j + 1) & (cap - 1);
            m->keys[j] = oldkeys[i];
            m->vals[j] = oldvals[i];
        }
    }
    free(oldkeys);
    free(oldvals);
}

static int32_t pmap_get(const PMap* m, const char* key) {
    uint32_t idx;
    if (!m->cap) return PMAP_NONE;
    idx = hash_ptr(key) & (m->cap - 1);
    for (;;) {
        const char* k = m->keys[idx];
        if (!k) return PMAP_NONE;
        if (k == key) return m->vals[idx];
        idx = (idx + 1) & (m->cap - 1);
    }
}

static void pmap_put(PMap* m, const char* key, int32_t val) {
    uint32_t idx;
    if (!m->cap) pmap_rehash(m, 32);
    if ((m->count + 1) * 10 >= m->cap * 7) pmap_rehash(m, m->cap * 2);
    idx = hash_ptr(key) & (m->cap - 1);
    for (;;) {
        const char* k = m->keys[idx];
        if (!k) {
            m->keys[idx] = key;
            m->vals[idx] = val;
            m->count++;
            return;
        }
        if (k == key) {
            m->vals[idx] = val;
            return;
        }
        idx = (idx + 1) & (m->cap - 1);
    }
}

static void pmap_invalidate_all(PMap* m) {
    uint32_t i;
    for (i = 0; i < m->cap; i++)
        if (m->keys[i]) m->vals[i] = -1;
}

static void pmap_free(PMap* m) {
    free(m->keys);
    free(m->vals);
    m->keys = NULL;
    m->vals = NULL;
    m->cap = 0;
    m->count = 0;
}

/* ================================================================
 *  Section 4: Lexer
 * ================================================================ */

enum {
    TK_EOF = 0, TK_NL, TK_INDENT, TK_DEDENT,
    TK_ID, TK_INT, TK_FLT, TK_STR,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PCT,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_GT, TK_LTE, TK_GTE,
    TK_LP, TK_RP, TK_LB, TK_RB, TK_LC, TK_RC,
    TK_COLON, TK_COMMA, TK_DOT,
    KW_TRUE, KW_FALSE,
    KW_IF, KW_ELS, KW_WHILE, KW_FOR, KW_IN,
    KW_DEF, KW_RETURN, KW_CLASS,
    KW_AND, KW_OR, KW_NOT,
    KW_INT, KW_FLT, KW_STR, KW_BOL, KW_VOID,
    KW_RANGE,
    TK_COUNT
};

static const char* const g_toknames[TK_COUNT] = {
    "EOF", "NL", "INDENT", "DEDENT", "ID", "INT", "FLT", "STR",
    "+", "-", "*", "/", "%", "=", "==", "!=", "<", ">", "<=", ">=",
    "(", ")", "[", "]", "{", "}", ":", ",", ".",
    "True", "False", "if", "els", "while", "for", "in",
    "def", "return", "class", "and", "or", "not",
    "int", "flt", "str", "bol", "void", "range"
};

typedef struct {
    const char* text;
    int32_t line;
    int32_t col;
    uint8_t type;
} Tok;

typedef struct {
    Tok* items;
    int32_t count;
    int32_t cap;
} TokVec;

typedef struct {
    const unsigned char* src;
    size_t len;
    size_t pos;
    int32_t line;
    int32_t col;
} Lxr;

typedef struct {
    const char* word;
    uint32_t len;
    uint64_t hash;
    int type;
} Keyword;

static Keyword g_keywords[32];
static int g_keyword_count;

static void keywords_init(void) {
    static const struct { const char* w; int t; } defs[] = {
        {"True", KW_TRUE}, {"False", KW_FALSE}, {"if", KW_IF},
        {"els", KW_ELS}, {"while", KW_WHILE}, {"for", KW_FOR},
        {"in", KW_IN}, {"def", KW_DEF}, {"return", KW_RETURN},
        {"class", KW_CLASS}, {"and", KW_AND}, {"or", KW_OR},
        {"not", KW_NOT}, {"int", KW_INT}, {"flt", KW_FLT},
        {"str", KW_STR}, {"bol", KW_BOL}, {"void", KW_VOID},
        {"range", KW_RANGE}
    };
    size_t i;
    g_keyword_count = (int)(sizeof(defs) / sizeof(defs[0]));
    for (i = 0; i < (size_t)g_keyword_count; i++) {
        g_keywords[i].word = defs[i].w;
        g_keywords[i].len = (uint32_t)strlen(defs[i].w);
        g_keywords[i].hash = hash_bytes(defs[i].w, g_keywords[i].len);
        g_keywords[i].type = defs[i].t;
    }
}

static int keyword_lookup(const char* s, uint32_t len, uint64_t h) {
    int i;
    for (i = 0; i < g_keyword_count; i++) {
        if (g_keywords[i].hash == h && g_keywords[i].len == len &&
            !memcmp(g_keywords[i].word, s, len))
            return g_keywords[i].type;
    }
    return TK_ID;
}

/* ASCII classification table (also removes ctype/negative-char UB). */
enum { CC_DIGIT = 1, CC_ALPHA = 2, CC_SPACE = 4 };
static uint8_t g_chclass[256];

static void chclass_init(void) {
    int i;
    for (i = 0; i < 256; i++) {
        uint8_t c = 0;
        if (i >= '0' && i <= '9') c |= CC_DIGIT;
        if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '_')
            c |= CC_ALPHA;
        if (i == ' ' || i == '\t') c |= CC_SPACE;
        g_chclass[i] = c;
    }
}

static void tv_push(TokVec* tv, int type, const char* text, int line, int col) {
    Tok* t;
    if (tv->count >= tv->cap) {
        tv->cap = tv->cap ? tv->cap * 2 : 256;
        tv->items = xrealloc(tv->items, sizeof(Tok) * (size_t)tv->cap);
    }
    t = &tv->items[tv->count++];
    t->text = text;
    t->line = line;
    t->col = col;
    t->type = (uint8_t)type;
}

typedef struct {
    int32_t* items;
    int32_t count;
    int32_t cap;
} IntVec;

static void iv_push(IntVec* v, int32_t x) {
    if (v->count >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->items = xrealloc(v->items, sizeof(int32_t) * (size_t)v->cap);
    }
    v->items[v->count++] = x;
}

static void iv_free(IntVec* v) {
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static int is_digit(unsigned char c) { return (g_chclass[c] & CC_DIGIT) != 0; }
static int is_alpha(unsigned char c) { return (g_chclass[c] & CC_ALPHA) != 0; }
static int is_ident(unsigned char c) {
    return (g_chclass[c] & (CC_ALPHA | CC_DIGIT)) != 0;
}
static int is_space(unsigned char c) { return (g_chclass[c] & CC_SPACE) != 0; }

static void lx_newline(Lxr* lx) {
    if (lx->src[lx->pos] == '\r') {
        lx->pos++;
        if (lx->pos < lx->len && lx->src[lx->pos] == '\n') lx->pos++;
    } else {
        lx->pos++;
    }
    lx->line++;
    lx->col = 1;
}

static int lex_all(const unsigned char* src, size_t len, Arena* ar,
                   Interner* in, TokVec* tv, DscErr* err) {
    Lxr lx;
    IntVec indents;
    int at_bol = 1;

    lx.src = src;
    lx.len = len;
    lx.pos = 0;
    lx.line = 1;
    lx.col = 1;

    indents.items = NULL;
    indents.count = 0;
    indents.cap = 0;
    iv_push(&indents, 0);

    /* UTF-8 BOM */
    if (len >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) {
        lx.pos = 3;
    }

    while (lx.pos < lx.len) {
        unsigned char c;
        if (at_bol) {
            int32_t indent = 0;
            int32_t cur;
            while (lx.pos < lx.len && is_space(src[lx.pos])) {
                indent += (src[lx.pos] == '\t') ? 4 : 1;
                lx.pos++;
                lx.col++;
            }
            if (lx.pos >= lx.len) break;
            c = src[lx.pos];
            if (c == '\r' || c == '\n') {
                lx_newline(&lx);
                continue;
            }
            if (c == '#' ||
                (c == '/' && lx.pos + 1 < lx.len && src[lx.pos + 1] == '/')) {
                while (lx.pos < lx.len && src[lx.pos] != '\n' && src[lx.pos] != '\r')
                    lx.pos++;
                continue;
            }
            cur = indents.items[indents.count - 1];
            if (indent > cur) {
                iv_push(&indents, indent);
                tv_push(tv, TK_INDENT, "", lx.line, lx.col);
            } else {
                while (indent < cur && indents.count > 1) {
                    indents.count--;
                    cur = indents.items[indents.count - 1];
                    tv_push(tv, TK_DEDENT, "", lx.line, lx.col);
                }
                if (indent != cur) {
                    err_set(err, lx.line, lx.col, "缩进不一致");
                    iv_free(&indents);
                    return -1;
                }
            }
            at_bol = 0;
        }

        c = src[lx.pos];
        if (c == '\r' || c == '\n') {
            int32_t ln = lx.line;
            int32_t co = lx.col;
            lx_newline(&lx);
            tv_push(tv, TK_NL, "\n", ln, co);
            at_bol = 1;
            continue;
        }
        if (is_space(c)) {
            lx.pos++;
            lx.col++;
            continue;
        }
        if (c == '#') {
            while (lx.pos < lx.len && src[lx.pos] != '\n' && src[lx.pos] != '\r')
                lx.pos++;
            continue;
        }
        if (c == '/' && lx.pos + 1 < lx.len && src[lx.pos + 1] == '/') {
            while (lx.pos < lx.len && src[lx.pos] != '\n' && src[lx.pos] != '\r')
                lx.pos++;
            continue;
        }

        if (c == '"') {
            size_t start = lx.pos + 1;
            size_t i = start;
            size_t outlen = 0;
            char* buf;
            size_t w = 0;
            int32_t ln = lx.line;
            int32_t co = lx.col;
            int closed = 0;
            /* First pass: validate and measure. */
            while (i < lx.len) {
                unsigned char ch = src[i];
                if (ch == '"') { closed = 1; break; }
                if (ch == '\n' || ch == '\r') break;
                if (ch == '\\' && i + 1 < lx.len) {
                    i += 2;
                    outlen++;
                } else {
                    i++;
                    outlen++;
                }
            }
            if (!closed) {
                err_set(err, lx.line, lx.col, "字符串未闭合");
                iv_free(&indents);
                return -1;
            }
            buf = arena_alloc(ar, outlen + 1);
            i = start;
            while (i < lx.len && src[i] != '"') {
                unsigned char ch = src[i];
                if (ch == '\\' && i + 1 < lx.len) {
                    unsigned char esc = src[i + 1];
                    switch (esc) {
                        case 'n': buf[w++] = '\n'; break;
                        case 't': buf[w++] = '\t'; break;
                        case 'r': buf[w++] = '\r'; break;
                        case '\\': buf[w++] = '\\'; break;
                        case '"': buf[w++] = '"'; break;
                        default: buf[w++] = (char)esc; break;
                    }
                    i += 2;
                } else {
                    buf[w++] = (char)ch;
                    i++;
                }
            }
            buf[w] = 0;
            lx.pos = i + 1; /* past closing quote */
            lx.col += (int32_t)(lx.pos - (start - 1));
            {
                const char* is = intern_n(in, ar, buf, w);
                tv_push(tv, TK_STR, is, ln, co);
            }
            continue;
        }

        if (is_digit(c)) {
            size_t start = lx.pos;
            int32_t ln = lx.line;
            int32_t co = lx.col;
            int isflt = 0;
            while (lx.pos < lx.len && is_digit(src[lx.pos])) {
                lx.pos++;
                lx.col++;
            }
            if (lx.pos + 1 < lx.len && src[lx.pos] == '.' && is_digit(src[lx.pos + 1])) {
                isflt = 1;
                lx.pos++;
                lx.col++;
                while (lx.pos < lx.len && is_digit(src[lx.pos])) {
                    lx.pos++;
                    lx.col++;
                }
            }
            {
                char* text = arena_strndup(ar, (const char*)src + start, lx.pos - start);
                tv_push(tv, isflt ? TK_FLT : TK_INT, text, ln, co);
            }
            continue;
        }

        if (is_alpha(c)) {
            size_t start = lx.pos;
            int32_t ln = lx.line;
            int32_t co = lx.col;
            uint64_t h;
            const char* is;
            int type;
            while (lx.pos < lx.len && is_ident(src[lx.pos])) {
                lx.pos++;
                lx.col++;
            }
            h = hash_bytes(src + start, lx.pos - start);
            type = keyword_lookup((const char*)src + start,
                                  (uint32_t)(lx.pos - start), h);
            is = intern_n(in, ar, (const char*)src + start, lx.pos - start);
            tv_push(tv, type, is, ln, co);
            continue;
        }

        {
            int32_t ln = lx.line;
            int32_t co = lx.col;
            lx.pos++;
            lx.col++;
            switch (c) {
                case '+': tv_push(tv, TK_PLUS, "+", ln, co); break;
                case '-': tv_push(tv, TK_MINUS, "-", ln, co); break;
                case '*': tv_push(tv, TK_STAR, "*", ln, co); break;
                case '/': tv_push(tv, TK_SLASH, "/", ln, co); break;
                case '%': tv_push(tv, TK_PCT, "%", ln, co); break;
                case '(': tv_push(tv, TK_LP, "(", ln, co); break;
                case ')': tv_push(tv, TK_RP, ")", ln, co); break;
                case '[': tv_push(tv, TK_LB, "[", ln, co); break;
                case ']': tv_push(tv, TK_RB, "]", ln, co); break;
                case '{': tv_push(tv, TK_LC, "{", ln, co); break;
                case '}': tv_push(tv, TK_RC, "}", ln, co); break;
                case ':': tv_push(tv, TK_COLON, ":", ln, co); break;
                case ',': tv_push(tv, TK_COMMA, ",", ln, co); break;
                case '.': tv_push(tv, TK_DOT, ".", ln, co); break;
                case '&': tv_push(tv, KW_AND, "&", ln, co); break;
                case '|': tv_push(tv, KW_OR, "|", ln, co); break;
                case '!':
                    if (lx.pos < lx.len && src[lx.pos] == '=') {
                        lx.pos++;
                        lx.col++;
                        tv_push(tv, TK_NEQ, "!=", ln, co);
                    } else {
                        tv_push(tv, KW_NOT, "!", ln, co);
                    }
                    break;
                case '=':
                    if (lx.pos < lx.len && src[lx.pos] == '=') {
                        lx.pos++;
                        lx.col++;
                        tv_push(tv, TK_EQEQ, "==", ln, co);
                    } else {
                        tv_push(tv, TK_EQ, "=", ln, co);
                    }
                    break;
                case '<':
                    if (lx.pos < lx.len && src[lx.pos] == '=') {
                        lx.pos++;
                        lx.col++;
                        tv_push(tv, TK_LTE, "<=", ln, co);
                    } else {
                        tv_push(tv, TK_LT, "<", ln, co);
                    }
                    break;
                case '>':
                    if (lx.pos < lx.len && src[lx.pos] == '=') {
                        lx.pos++;
                        lx.col++;
                        tv_push(tv, TK_GTE, ">=", ln, co);
                    } else {
                        tv_push(tv, TK_GT, ">", ln, co);
                    }
                    break;
                default:
                    err_set(err, ln, co, "非法字符 '%c'", (char)c);
                    iv_free(&indents);
                    return -1;
            }
        }
    }

    while (indents.count > 1) {
        indents.count--;
        tv_push(tv, TK_DEDENT, "", lx.line, lx.col);
    }
    tv_push(tv, TK_EOF, "", lx.line, lx.col);
    iv_free(&indents);
    return 0;
}

/* ================================================================
 *  Section 5: AST
 * ================================================================ */

enum {
    ND_PROG = 0, ND_ASSIGN, ND_IF, ND_WHILE, ND_FOR, ND_FN, ND_RET,
    ND_BLOCK, ND_BINOP, ND_UNOP, ND_ID, ND_INT, ND_FLT, ND_STR, ND_BOOL,
    ND_CALL, ND_ARR, ND_IDX, ND_FLD, ND_CLASS, ND_CLASS_LIT,
    ND_PARAM, ND_FIELD
};

typedef struct Node Node;
struct Node {
    Node* a;
    Node* b;
    Node* c;
    Node* next;
    const char* name;
    int64_t ival;
    double fval;
    int32_t line;
    int32_t col;
    uint8_t kind;
};

static Node* nd_new(Arena* ar, int kind, int line, int col) {
    Node* n = arena_alloc(ar, sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = (uint8_t)kind;
    n->line = line;
    n->col = col;
    return n;
}

static Node* nd_int(Arena* ar, int64_t v, int line, int col) {
    Node* n = nd_new(ar, ND_INT, line, col);
    n->ival = v;
    return n;
}

static Node* nd_flt(Arena* ar, double v, int line, int col) {
    Node* n = nd_new(ar, ND_FLT, line, col);
    n->fval = v;
    return n;
}

static Node* nd_str(Arena* ar, const char* s, int line, int col) {
    Node* n = nd_new(ar, ND_STR, line, col);
    n->name = s;
    return n;
}

static Node* nd_bool(Arena* ar, int v, int line, int col) {
    Node* n = nd_new(ar, ND_BOOL, line, col);
    n->ival = v ? 1 : 0;
    return n;
}

static Node* nd_id(Arena* ar, const char* s, int line, int col) {
    Node* n = nd_new(ar, ND_ID, line, col);
    n->name = s;
    return n;
}

static Node* nd_bin(Arena* ar, int op, Node* l, Node* r, int line, int col) {
    Node* n = nd_new(ar, ND_BINOP, line, col);
    n->ival = op;
    n->a = l;
    n->b = r;
    return n;
}

static Node* nd_un(Arena* ar, int op, Node* x, int line, int col) {
    Node* n = nd_new(ar, ND_UNOP, line, col);
    n->ival = op;
    n->a = x;
    return n;
}

static void list_push(Node** head, Node* n) {
    if (!*head) {
        *head = n;
        return;
    }
    {
        Node* p = *head;
        while (p->next) p = p->next;
        p->next = n;
    }
}

static int list_len(Node* n) {
    int k = 0;
    while (n) { k++; n = n->next; }
    return k;
}

/* ================================================================
 *  Section 6: Parser
 * ================================================================ */

typedef struct {
    Tok* toks;
    int32_t count;
    int32_t pos;
    Arena* ar;
    Interner* in;
    DscErr* err;
} Parser;

static Tok* pk(Parser* p) {
    return (p->pos < p->count) ? &p->toks[p->pos] : NULL;
}

static int at(Parser* p, int t) {
    Tok* k = pk(p);
    return k && k->type == t;
}

static Tok* take(Parser* p) {
    return (p->pos < p->count) ? &p->toks[p->pos++] : NULL;
}

static int accept(Parser* p, int t) {
    if (at(p, t)) {
        p->pos++;
        return 1;
    }
    return 0;
}

static Tok* expect(Parser* p, int t) {
    Tok* k = pk(p);
    if (k && k->type == t) {
        p->pos++;
        return k;
    }
    if (!err_has(p->err)) {
        if (k)
            err_set(p->err, k->line, k->col, "期望 %s，实际为 %s",
                    g_toknames[t], g_toknames[k->type]);
        else
            err_set(p->err, 0, 0, "期望 %s，但已到文件末尾", g_toknames[t]);
    }
    return NULL;
}

static Node* parse_expr(Parser* p);
static Node* parse_stmt(Parser* p);

static int is_type_kw(int t) {
    return t == KW_INT || t == KW_FLT || t == KW_STR || t == KW_BOL ||
           t == KW_VOID;
}

/* Returns the annotation token; class annotations are stored in *cls_name. */
static int parse_type_annot(Parser* p, const char** cls_name) {
    Tok* k = pk(p);
    *cls_name = NULL;
    if (!k) return 0;
    if (is_type_kw(k->type)) {
        take(p);
        return k->type;
    }
    if (k->type == TK_ID) {
        take(p);
        *cls_name = k->text;
        return TK_ID;
    }
    return 0;
}

static Node* parse_primary(Parser* p) {
    Tok* tk = pk(p);
    if (!tk) {
        err_set(p->err, 0, 0, "表达式意外结束");
        return NULL;
    }

    if (tk->type == TK_INT) {
        int64_t v;
        take(p);
        errno = 0;
        v = strtoll(tk->text, NULL, 10);
        if (errno == ERANGE) {
            err_set(p->err, tk->line, tk->col, "整数字面量超出范围");
            return NULL;
        }
        return nd_int(p->ar, v, tk->line, tk->col);
    }
    if (tk->type == TK_FLT) {
        double v;
        take(p);
        errno = 0;
        v = strtod(tk->text, NULL);
        if (errno == ERANGE) {
            err_set(p->err, tk->line, tk->col, "浮点字面量超出范围");
            return NULL;
        }
        return nd_flt(p->ar, v, tk->line, tk->col);
    }
    if (tk->type == TK_STR) {
        take(p);
        return nd_str(p->ar, tk->text, tk->line, tk->col);
    }
    if (tk->type == KW_TRUE) {
        take(p);
        return nd_bool(p->ar, 1, tk->line, tk->col);
    }
    if (tk->type == KW_FALSE) {
        take(p);
        return nd_bool(p->ar, 0, tk->line, tk->col);
    }

    if (tk->type == TK_LP) {
        Node* e;
        take(p);
        e = parse_expr(p);
        expect(p, TK_RP);
        return e;
    }

    if (tk->type == TK_LB) {
        Node* arr;
        take(p);
        arr = nd_new(p->ar, ND_ARR, tk->line, tk->col);
        if (!at(p, TK_RB)) {
            do {
                Node* e = parse_expr(p);
                if (!e) return NULL;
                list_push(&arr->a, e);
            } while (accept(p, TK_COMMA));
        }
        expect(p, TK_RB);
        return arr;
    }

    if (tk->type == TK_ID) {
        const char* nm = tk->text;
        int32_t ln = tk->line;
        int32_t co = tk->col;
        take(p);
        if (at(p, TK_LP)) {
            Node* call;
            take(p);
            call = nd_new(p->ar, ND_CALL, ln, co);
            call->name = nm;
            if (!at(p, TK_RP)) {
                do {
                    Node* e = parse_expr(p);
                    if (!e) return NULL;
                    list_push(&call->a, e);
                } while (accept(p, TK_COMMA));
            }
            expect(p, TK_RP);
            return call;
        }
        if (at(p, TK_LC)) {
            Node* cl;
            take(p);
            cl = nd_new(p->ar, ND_CLASS_LIT, ln, co);
            cl->name = nm;
            if (!at(p, TK_RC)) {
                do {
                    Tok* fn = expect(p, TK_ID);
                    Node* fld;
                    if (!fn) return NULL;
                    expect(p, TK_COLON);
                    fld = nd_new(p->ar, ND_FLD, fn->line, fn->col);
                    fld->name = fn->text;
                    fld->a = parse_expr(p);
                    if (!fld->a) return NULL;
                    list_push(&cl->a, fld);
                } while (accept(p, TK_COMMA));
            }
            expect(p, TK_RC);
            return cl;
        }
        return nd_id(p->ar, nm, ln, co);
    }

    err_set(p->err, tk->line, tk->col, "意外的记号 %s", g_toknames[tk->type]);
    return NULL;
}

static Node* parse_unary(Parser* p) {
    Tok* tk = pk(p);
    if (!tk) {
        err_set(p->err, 0, 0, "表达式意外结束");
        return NULL;
    }
    if (tk->type == KW_NOT) {
        Node* x;
        take(p);
        x = parse_unary(p);
        if (!x) return NULL;
        return nd_un(p->ar, KW_NOT, x, tk->line, tk->col);
    }
    if (tk->type == TK_MINUS) {
        Node* x;
        take(p);
        x = parse_unary(p);
        if (!x) return NULL;
        return nd_un(p->ar, TK_MINUS, x, tk->line, tk->col);
    }
    return parse_primary(p);
}

static Node* parse_postfix(Parser* p) {
    Node* e = parse_unary(p);
    if (!e) return NULL;
    for (;;) {
        if (at(p, TK_LB)) {
            Node* idx;
            Node* n;
            take(p);
            idx = parse_expr(p);
            if (!idx) return NULL;
            expect(p, TK_RB);
            n = nd_new(p->ar, ND_IDX, e->line, e->col);
            n->a = e;
            n->b = idx;
            e = n;
        } else if (at(p, TK_DOT)) {
            Tok* fn;
            Node* n;
            take(p);
            fn = expect(p, TK_ID);
            if (!fn) return NULL;
            n = nd_new(p->ar, ND_FLD, e->line, e->col);
            n->name = fn->text;
            n->a = e;
            e = n;
        } else {
            break;
        }
    }
    return e;
}

static Node* parse_mul(Parser* p) {
    Node* e = parse_postfix(p);
    if (!e) return NULL;
    while (at(p, TK_STAR) || at(p, TK_SLASH) || at(p, TK_PCT)) {
        int op = take(p)->type;
        Node* r = parse_postfix(p);
        if (!r) return NULL;
        e = nd_bin(p->ar, op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_add(Parser* p) {
    Node* e = parse_mul(p);
    if (!e) return NULL;
    while (at(p, TK_PLUS) || at(p, TK_MINUS)) {
        int op = take(p)->type;
        Node* r = parse_mul(p);
        if (!r) return NULL;
        e = nd_bin(p->ar, op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_cmp(Parser* p) {
    Node* e = parse_add(p);
    if (!e) return NULL;
    while (at(p, TK_LT) || at(p, TK_GT) || at(p, TK_LTE) || at(p, TK_GTE)) {
        int op = take(p)->type;
        Node* r = parse_add(p);
        if (!r) return NULL;
        e = nd_bin(p->ar, op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_eq(Parser* p) {
    Node* e = parse_cmp(p);
    if (!e) return NULL;
    while (at(p, TK_EQEQ) || at(p, TK_NEQ)) {
        int op = take(p)->type;
        Node* r = parse_cmp(p);
        if (!r) return NULL;
        e = nd_bin(p->ar, op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_and(Parser* p) {
    Node* e = parse_eq(p);
    if (!e) return NULL;
    while (at(p, KW_AND)) {
        int op = take(p)->type;
        Node* r = parse_eq(p);
        if (!r) return NULL;
        e = nd_bin(p->ar, op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_or(Parser* p) {
    Node* e = parse_and(p);
    if (!e) return NULL;
    while (at(p, KW_OR)) {
        int op = take(p)->type;
        Node* r = parse_and(p);
        if (!r) return NULL;
        e = nd_bin(p->ar, op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_expr(Parser* p) {
    return parse_or(p);
}

static Node* parse_block(Parser* p) {
    Node* blk;
    Tok* k = pk(p);
    int32_t ln = k ? k->line : 0;
    int32_t co = k ? k->col : 0;
    if (!expect(p, TK_COLON)) return NULL;
    blk = nd_new(p->ar, ND_BLOCK, ln, co);
    if (at(p, TK_NL)) {
        take(p);
        expect(p, TK_INDENT);
        while (!at(p, TK_DEDENT) && !at(p, TK_EOF)) {
            Node* st;
            if (at(p, TK_NL)) {
                take(p);
                continue;
            }
            st = parse_stmt(p);
            if (err_has(p->err)) return NULL;
            if (st) list_push(&blk->a, st);
            if (at(p, TK_NL)) take(p);
        }
        expect(p, TK_DEDENT);
    } else {
        Node* st = parse_stmt(p);
        if (err_has(p->err)) return NULL;
        blk->a = st;
    }
    return blk;
}

static Node* parse_if(Parser* p) {
    Tok* tk = take(p);
    Node* cond = parse_expr(p);
    Node* n;
    Node** tail;
    if (!cond) return NULL;
    n = nd_new(p->ar, ND_IF, tk->line, tk->col);
    n->a = cond;
    n->b = parse_block(p);
    if (!n->b) return NULL;
    tail = &n->c;
    while (at(p, KW_ELS)) {
        Tok* ek = take(p);
        if (at(p, KW_IF)) {
            Node* ei;
            take(p);
            ei = nd_new(p->ar, ND_IF, ek->line, ek->col);
            ei->a = parse_expr(p);
            if (!ei->a) return NULL;
            ei->b = parse_block(p);
            if (!ei->b) return NULL;
            *tail = ei;
            tail = &ei->c;
        } else {
            *tail = parse_block(p);
            if (!*tail) return NULL;
            break;
        }
    }
    return n;
}

static Node* parse_while(Parser* p) {
    Tok* tk = take(p);
    Node* cond = parse_expr(p);
    Node* n;
    if (!cond) return NULL;
    n = nd_new(p->ar, ND_WHILE, tk->line, tk->col);
    n->a = cond;
    n->b = parse_block(p);
    if (!n->b) return NULL;
    return n;
}

static Node* parse_for(Parser* p) {
    Tok* tk = take(p);
    Tok* var = expect(p, TK_ID);
    Node* n;
    if (!var) return NULL;
    expect(p, KW_IN);
    expect(p, KW_RANGE);
    expect(p, TK_LP);
    n = nd_new(p->ar, ND_FOR, tk->line, tk->col);
    n->name = var->text;
    n->a = parse_expr(p);
    if (!n->a) return NULL;
    expect(p, TK_RP);
    n->b = parse_block(p);
    if (!n->b) return NULL;
    return n;
}

static Node* parse_def(Parser* p) {
    Tok* tk = take(p);
    Tok* nm = expect(p, TK_ID);
    Node* fn;
    if (!nm) return NULL;
    fn = nd_new(p->ar, ND_FN, tk->line, tk->col);
    fn->name = nm->text;
    expect(p, TK_LP);
    if (!at(p, TK_RP)) {
        do {
            Tok* pn = expect(p, TK_ID);
            Node* pa;
            if (!pn) return NULL;
            pa = nd_new(p->ar, ND_PARAM, pn->line, pn->col);
            pa->name = pn->text;
            if (accept(p, TK_COLON)) {
                const char* cls = NULL;
                int ty = parse_type_annot(p, &cls);
                pa->ival = ty;
                if (cls) pa->b = nd_id(p->ar, cls, 0, 0);
            }
            list_push(&fn->a, pa);
        } while (accept(p, TK_COMMA));
    }
    expect(p, TK_RP);
    /* Optional return type: only a primitive keyword is unambiguous here. */
    if (at(p, TK_COLON)) {
        Tok* nx = (p->pos + 1 < p->count) ? &p->toks[p->pos + 1] : NULL;
        if (nx && is_type_kw(nx->type)) {
            take(p);
            fn->ival = take(p)->type;
        }
    }
    fn->b = parse_block(p);
    if (!fn->b) return NULL;
    return fn;
}

static Node* parse_return(Parser* p) {
    Tok* tk = take(p);
    Node* n = nd_new(p->ar, ND_RET, tk->line, tk->col);
    if (!at(p, TK_NL) && !at(p, TK_DEDENT) && !at(p, TK_EOF)) {
        n->a = parse_expr(p);
        if (!n->a) return NULL;
    }
    return n;
}

static Node* parse_class(Parser* p) {
    Tok* tk = take(p);
    Tok* nm = expect(p, TK_ID);
    Node* cl;
    if (!nm) return NULL;
    cl = nd_new(p->ar, ND_CLASS, tk->line, tk->col);
    cl->name = nm->text;
    if (!expect(p, TK_COLON)) return NULL;
    expect(p, TK_NL);
    expect(p, TK_INDENT);
    while (!at(p, TK_DEDENT) && !at(p, TK_EOF)) {
        Tok* fn = expect(p, TK_ID);
        Node* fd;
        if (!fn) return NULL;
        if (!expect(p, TK_COLON)) return NULL;
        fd = nd_new(p->ar, ND_FIELD, fn->line, fn->col);
        fd->name = fn->text;
        {
            const char* cls = NULL;
            int ty = parse_type_annot(p, &cls);
            if (!ty) ty = KW_INT; /* 与 v2 一致：省略标注默认 int */
            fd->ival = ty;
            if (cls) fd->b = nd_id(p->ar, cls, fn->line, fn->col);
        }
        list_push(&cl->a, fd);
        if (at(p, TK_NL)) take(p);
        else if (!at(p, TK_DEDENT)) {
            err_set(p->err, fn->line, fn->col, "类定义中应为换行");
            return NULL;
        }
    }
    expect(p, TK_DEDENT);
    return cl;
}

static Node* parse_stmt(Parser* p) {
    Tok* tk = pk(p);
    if (!tk || tk->type == TK_DEDENT || tk->type == TK_EOF) return NULL;
    if (tk->type == TK_NL) {
        take(p);
        return NULL;
    }
    if (tk->type == KW_IF) return parse_if(p);
    if (tk->type == KW_WHILE) return parse_while(p);
    if (tk->type == KW_FOR) return parse_for(p);
    if (tk->type == KW_DEF) return parse_def(p);
    if (tk->type == KW_RETURN) return parse_return(p);
    if (tk->type == KW_CLASS) return parse_class(p);

    {
        Node* e = parse_expr(p);
        if (!e) return NULL;
        if (at(p, TK_EQ)) {
            Node* n;
            take(p);
            if (e->kind != ND_ID && e->kind != ND_IDX && e->kind != ND_FLD) {
                err_set(p->err, e->line, e->col, "非法的赋值目标");
                return NULL;
            }
            n = nd_new(p->ar, ND_ASSIGN, e->line, e->col);
            n->a = e;
            n->b = parse_expr(p);
            if (!n->b) return NULL;
            return n;
        }
        return e;
    }
}

static Node* parse_program(Parser* p) {
    Node* prog = nd_new(p->ar, ND_PROG, 0, 0);
    while (p->pos < p->count && !at(p, TK_EOF)) {
        Node* st;
        if (at(p, TK_NL)) {
            take(p);
            continue;
        }
        st = parse_stmt(p);
        if (err_has(p->err)) return NULL;
        if (st) list_push(&prog->a, st);
        if (at(p, TK_NL)) take(p);
    }
    return prog;
}

/* ================================================================
 *  Section 7: Compiler
 * ================================================================ */

enum { VT_NIL = 0, VT_INT, VT_FLT, VT_STR, VT_BOL, VT_ARR, VT_OBJ };

enum {
    OP_HALT = 0,
    OP_PUSH_I, OP_PUSH_K, OP_PUSH_S, OP_PUSH_B, OP_PUSH_NIL,
    OP_PUSH_0, OP_PUSH_1,
    OP_LOAD, OP_STORE, OP_LOAD_G, OP_STORE_G,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_ADD_I, OP_SUB_I, OP_MUL_I,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_NOT, OP_NEG,
    OP_JMP, OP_JF, OP_JF_KEEP, OP_JT_KEEP,
    OP_POP,
    OP_CALL, OP_RET,
    OP_ARRAY, OP_GET, OP_SET, OP_LEN,
    OP_FGET, OP_FSET, OP_FGET_C, OP_FSET_C, OP_NEWOBJ,
    OP_PRINT, OP_INPUT, OP_TYPE,
    OP_INC, OP_ADD_ST, OP_SUB_ST, OP_MUL_ST, OP_DIV_ST, OP_MOD_ST,
    OP_ADD_ST_IDX, OP_FOP_C,
    OP_LT_JF, OP_LE_JF, OP_GT_JF, OP_GE_JF, OP_EQ_JF, OP_NEQ_JF
};

/* 8-byte instruction: [arg:32][line:16][op:8][aux:8] */
typedef struct {
    int32_t arg;
    uint16_t line;
    uint8_t op;
    uint8_t aux;
} Instr;

typedef struct {
    Instr* v;
    int32_t n;
    int32_t cap;
} Code;

typedef struct {
    int32_t type;      /* VT_INT or VT_FLT */
    int64_t bits;
} K64;

typedef struct {
    const char* name;
    int32_t nf;
    const char** fields;
    int32_t* ftype;
    int32_t* fclass;
} Class;

typedef struct {
    const char* name;
    Node* node;
    Code code;
    int32_t params;
    int32_t max_slots;
    int32_t max_stack;
} Func;

typedef struct {
    Arena* ar;
    Interner* in;
    DscErr* err;
    Code main_code;
    int32_t main_slots;
    int32_t main_max_stack;
    Func* funcs;
    int32_t nfunc;
    int32_t cfunc;
    Class* classes;
    int32_t ncls;
    int32_t ccls;
    const char** strs;
    int32_t nstr;
    int32_t cstr;
    K64* k64;
    int32_t nk64;
    int32_t ck64;
    PMap globals;   /* name -> global slot */
    PMap func_map;  /* name -> func index */
    PMap class_map; /* name -> class id */
    PMap gcls;      /* top-level name -> known class id (or -1) */
} Comp;

typedef struct {
    Comp* c;
    Code* code;
    PMap vars;     /* local name -> slot */
    PMap vcls;     /* local name -> known class id (or -1) */
    int32_t next_slot;
    int32_t max_slot;
    int is_main;
} FnCtx;

static void code_init(Code* c) {
    c->v = NULL;
    c->n = 0;
    c->cap = 0;
}

static int32_t code_emit(Code* c, int op, int32_t arg, int line, int aux) {
    Instr* ins;
    if (c->n >= c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->v = xrealloc(c->v, sizeof(Instr) * (size_t)c->cap);
    }
    ins = &c->v[c->n];
    ins->arg = arg;
    ins->line = (uint16_t)(line < 0 ? 0 : (line > 65535 ? 65535 : line));
    ins->op = (uint8_t)op;
    ins->aux = (uint8_t)aux;
    return c->n++;
}

static int op_is_jump(int op) {
    switch (op) {
        case OP_JMP: case OP_JF: case OP_JF_KEEP: case OP_JT_KEEP:
        case OP_LT_JF: case OP_LE_JF: case OP_GT_JF: case OP_GE_JF:
        case OP_EQ_JF: case OP_NEQ_JF:
            return 1;
        default:
            return 0;
    }
}

static void code_patch(Code* c, int32_t at, int32_t target) {
    c->v[at].arg = target;
    /* Peephole: drop a trailing jump to the immediately following address.
     * Only safe when no earlier jump already references this slot or beyond. */
    if (c->v[at].op == OP_JMP && target == at + 1 && at == c->n - 1) {
        int32_t i;
        int safe = 1;
        for (i = 0; i < at; i++) {
            if (op_is_jump(c->v[i].op) && c->v[i].arg >= at) {
                safe = 0;
                break;
            }
        }
        if (safe) c->n--;
    }
}

static void code_free(Code* c) {
    free(c->v);
    c->v = NULL;
    c->n = 0;
    c->cap = 0;
}

static int32_t str_index(Comp* c, const char* s) {
    int32_t i;
    for (i = 0; i < c->nstr; i++)
        if (c->strs[i] == s) return i;
    if (c->nstr >= c->cstr) {
        c->cstr = c->cstr ? c->cstr * 2 : 64;
        c->strs = xrealloc(c->strs, sizeof(char*) * (size_t)c->cstr);
    }
    c->strs[c->nstr] = s;
    return c->nstr++;
}

static int32_t k64_index(Comp* c, int32_t type, int64_t bits) {
    int32_t i;
    for (i = 0; i < c->nk64; i++) {
        if (c->k64[i].type == type && c->k64[i].bits == bits) return i;
    }
    if (c->nk64 >= c->ck64) {
        c->ck64 = c->ck64 ? c->ck64 * 2 : 32;
        c->k64 = xrealloc(c->k64, sizeof(K64) * (size_t)c->ck64);
    }
    c->k64[c->nk64].type = type;
    c->k64[c->nk64].bits = bits;
    return c->nk64++;
}

static int32_t global_slot(Comp* c, const char* name) {
    int32_t slot = pmap_get(&c->globals, name);
    if (slot == PMAP_NONE) {
        slot = (int32_t)c->globals.count;
        pmap_put(&c->globals, name, slot);
    }
    return slot;
}

static int32_t local_slot(FnCtx* fc, const char* name) {
    if (fc->is_main) return -1;
    return pmap_get(&fc->vars, name);
}

static int32_t alloc_slot(FnCtx* fc) {
    int32_t s = fc->next_slot++;
    if (fc->next_slot > fc->max_slot) fc->max_slot = fc->next_slot;
    return s;
}

static int32_t class_of_var(FnCtx* fc, const char* name) {
    int32_t v = fc->is_main ? pmap_get(&fc->c->gcls, name)
                            : pmap_get(&fc->vcls, name);
    return (v == PMAP_NONE) ? -1 : v;
}

static void set_class_of_var(FnCtx* fc, const char* name, int32_t cls) {
    if (fc->is_main) pmap_put(&fc->c->gcls, name, cls);
    else pmap_put(&fc->vcls, name, cls);
}

static int32_t class_id_by_name(Comp* c, const char* name) {
    int32_t id = pmap_get(&c->class_map, name);
    return id == PMAP_NONE ? -1 : id;
}

static int32_t class_field_index(Comp* c, int32_t cls, const char* name) {
    Class* cl;
    int32_t i;
    if (cls < 0 || cls >= c->ncls) return -1;
    cl = &c->classes[cls];
    for (i = 0; i < cl->nf; i++)
        if (cl->fields[i] == name || !strcmp(cl->fields[i], name)) return i;
    return -1;
}

/* Static class inference: only direct literals and tracked variables. */
static int32_t infer_class(FnCtx* fc, Node* n) {
    if (!n) return -1;
    if (n->kind == ND_CLASS_LIT) return class_id_by_name(fc->c, n->name);
    if (n->kind == ND_ID) return class_of_var(fc, n->name);
    return -1;
}

static int node_is_pure(Node* n) {
    if (!n) return 0;
    switch (n->kind) {
        case ND_INT: case ND_FLT: case ND_STR: case ND_BOOL: case ND_ID:
            return 1;
        case ND_UNOP:
            return node_is_pure(n->a);
        case ND_BINOP:
            return node_is_pure(n->a) && node_is_pure(n->b);
        case ND_IDX:
            return node_is_pure(n->a) && node_is_pure(n->b);
        case ND_FLD:
            return node_is_pure(n->a);
        default:
            return 0;
    }
}

/* Structural check for "x.field = x.field op pure" patterns. */
static int same_field_lvalue(Node* lv, Node* other) {
    if (!lv || !other || lv->kind != ND_FLD || other->kind != ND_FLD)
        return 0;
    if (lv->name != other->name && strcmp(lv->name, other->name)) return 0;
    if (lv->a && other->a && lv->a->kind == ND_ID && other->a->kind == ND_ID)
        return lv->a->name == other->a->name;
    return 0;
}

static int field_op_aux(int op) {
    switch (op) {
        case TK_PLUS: return 0;
        case TK_MINUS: return 1;
        case TK_STAR: return 2;
        case TK_SLASH: return 3;
        case TK_PCT: return 4;
        default: return -1;
    }
}

static int node_terminates(Node* n) {
    if (!n) return 0;
    switch (n->kind) {
        case ND_RET:
            return 1;
        case ND_BLOCK: {
            Node* s = n->a;
            Node* last = NULL;
            while (s) { last = s; s = s->next; }
            return last ? node_terminates(last) : 0;
        }
        case ND_IF:
            if (!n->c) return 0;
            if (!node_terminates(n->b)) return 0;
            if (n->c->kind == ND_IF) return node_terminates(n->c);
            return node_terminates(n->c);
        default:
            return 0;
    }
}

/* --- Static operand-stack depth analysis ---
 * Computes a conservative upper bound of transient operand-stack cells used
 * by an expression/statement.  This lets the VM size frames precisely and
 * avoid a per-instruction stack-capacity check on the hot path. */

static int32_t maxi32(int32_t a, int32_t b) { return a > b ? a : b; }

static int32_t expr_depth(Node* n);
static int32_t stmt_depth(Node* n);
static int32_t block_depth(Node* blk);

static int32_t expr_depth(Node* n) {
    int32_t i;
    int32_t peak;
    switch (n ? n->kind : ND_PROG) {
        case ND_INT: case ND_FLT: case ND_STR: case ND_BOOL: case ND_ID:
            return 1;
        case ND_UNOP:
            return expr_depth(n->a);
        case ND_BINOP:
            return maxi32(maxi32(expr_depth(n->a), 1 + expr_depth(n->b)), 2);
        case ND_CALL:
            peak = 1;
            i = 0;
            {
                Node* a;
                for (a = n->a; a; a = a->next) {
                    peak = maxi32(peak, i + expr_depth(a));
                    i++;
                }
            }
            return peak;
        case ND_IDX:
            return maxi32(maxi32(expr_depth(n->a), 1 + expr_depth(n->b)), 2);
        case ND_FLD:
            return expr_depth(n->a);
        case ND_ARR: {
            Node* e;
            peak = 1;
            i = 0;
            for (e = n->a; e; e = e->next) {
                peak = maxi32(peak, i + expr_depth(e));
                i++;
            }
            return peak;
        }
        case ND_CLASS_LIT: {
            Node* f;
            peak = 1;
            i = 0;
            for (f = n->a; f; f = f->next) {
                peak = maxi32(peak, i + expr_depth(f->a));
                i++;
            }
            return peak;
        }
        default:
            return 1;
    }
}

static int32_t block_depth(Node* blk) {
    Node* body = (blk && blk->kind == ND_BLOCK) ? blk->a : blk;
    Node* s;
    int32_t d = 0;
    for (s = body; s; s = s->next) d = maxi32(d, stmt_depth(s));
    return d;
}

static int32_t stmt_depth(Node* n) {
    if (!n) return 0;
    switch (n->kind) {
        case ND_ASSIGN: {
            Node* lv = n->a;
            int32_t dv = expr_depth(n->b);
            if (lv && lv->kind == ND_IDX)
                return maxi32(maxi32(expr_depth(lv->a), 1 + expr_depth(lv->b)),
                              2 + dv);
            if (lv && lv->kind == ND_FLD)
                return maxi32(expr_depth(lv->a), 1 + dv);
            return dv;
        }
        case ND_IF: {
            int32_t d = maxi32(expr_depth(n->a), block_depth(n->b));
            if (n->c)
                d = maxi32(d, n->c->kind == ND_IF ? stmt_depth(n->c)
                                                 : block_depth(n->c));
            return d;
        }
        case ND_WHILE:
            return maxi32(expr_depth(n->a), block_depth(n->b));
        case ND_FOR:
            return maxi32(maxi32(expr_depth(n->a), block_depth(n->b)), 2);
        case ND_RET:
            return n->a ? expr_depth(n->a) : 1;
        case ND_BLOCK:
            return block_depth(n);
        case ND_FN:
        case ND_CLASS:
            return 0;
        default:
            return expr_depth(n);
    }
}

/* --- Constant folding --- */
typedef struct {
    int type; /* VT_INT / VT_FLT / VT_STR / VT_BOL */
    int64_t i;
    double f;
    const char* s;
} KVal;

static int fold_expr(Comp* c, Node* n, KVal* out);

static int kval_truthy(const KVal* k) {
    switch (k->type) {
        case VT_INT: return k->i != 0;
        case VT_FLT: return k->f != 0.0;
        case VT_STR: return k->s && k->s[0];
        case VT_BOL: return k->i != 0;
        default: return 0;
    }
}

static int fold_bin(Comp* c, int op, const KVal* a, const KVal* b, KVal* out) {
    int ai = (a->type == VT_INT || a->type == VT_BOL);
    int bi = (b->type == VT_INT || b->type == VT_BOL);
    int an = ai || a->type == VT_FLT;
    int bn = bi || b->type == VT_FLT;

    if (op == TK_PLUS && (a->type == VT_STR || b->type == VT_STR)) {
        /* Fold only string+string to keep formatting rules identical. */
        if (a->type == VT_STR && b->type == VT_STR) {
            size_t la = strlen(a->s);
            size_t lb = strlen(b->s);
            char* s = arena_alloc(c->ar, la + lb + 1);
            memcpy(s, a->s, la);
            memcpy(s + la, b->s, lb);
            s[la + lb] = 0;
            out->type = VT_STR;
            out->s = s;
            return 1;
        }
        return 0;
    }

    if (op == TK_EQEQ || op == TK_NEQ) {
        int eq = 0;
        if (an && bn) {
            if (ai && bi) eq = a->i == b->i;
            else {
                double x = ai ? (double)a->i : a->f;
                double y = bi ? (double)b->i : b->f;
                eq = x == y;
            }
        } else if (a->type == VT_STR && b->type == VT_STR) {
            eq = !strcmp(a->s, b->s);
        } else if (a->type == VT_BOL && b->type == VT_BOL) {
            eq = a->i == b->i;
        }
        out->type = VT_BOL;
        out->i = (op == TK_EQEQ) ? eq : !eq;
        return 1;
    }

    if (op == TK_LT || op == TK_GT || op == TK_LTE || op == TK_GTE) {
        int r = 0;
        if (an && bn) {
            if (ai && bi) {
                int64_t x = a->i, y = b->i;
                r = op == TK_LT ? x < y : op == TK_GT ? x > y
                                                   : op == TK_LTE ? x <= y : x >= y;
            } else {
                double x = ai ? (double)a->i : a->f;
                double y = bi ? (double)b->i : b->f;
                r = op == TK_LT ? x < y : op == TK_GT ? x > y
                                                   : op == TK_LTE ? x <= y : x >= y;
            }
        } else if (a->type == VT_STR && b->type == VT_STR) {
            int cmp = strcmp(a->s, b->s);
            r = op == TK_LT ? cmp < 0 : op == TK_GT ? cmp > 0
                                                  : op == TK_LTE ? cmp <= 0 : cmp >= 0;
        } else {
            return 0;
        }
        out->type = VT_BOL;
        out->i = r;
        return 1;
    }

    if (op == KW_AND) {
        if (kval_truthy(a)) *out = *b;
        else *out = *a;
        return 1;
    }
    if (op == KW_OR) {
        if (kval_truthy(a)) *out = *a;
        else *out = *b;
        return 1;
    }

    if (!(an && bn)) return 0;

    if (op == TK_PLUS || op == TK_MINUS || op == TK_STAR) {
        if (ai && bi) {
            uint64_t x = (uint64_t)a->i, y = (uint64_t)b->i;
            out->type = VT_INT;
            if (op == TK_PLUS) out->i = (int64_t)(x + y);
            else if (op == TK_MINUS) out->i = (int64_t)(x - y);
            else out->i = (int64_t)(x * y);
        } else {
            double x = ai ? (double)a->i : a->f;
            double y = bi ? (double)b->i : b->f;
            out->type = VT_FLT;
            if (op == TK_PLUS) out->f = x + y;
            else if (op == TK_MINUS) out->f = x - y;
            else out->f = x * y;
        }
        return 1;
    }
    if (op == TK_SLASH) {
        if (ai && bi) {
            if (b->i == 0) {
                err_set(c->err, 0, 0, "常量表达式除以零");
                return -1;
            }
            if (a->i == INT64_MIN && b->i == -1) {
                err_set(c->err, 0, 0, "常量表达式整数溢出");
                return -1;
            }
            out->type = VT_INT;
            out->i = a->i / b->i;
        } else {
            double x = ai ? (double)a->i : a->f;
            double y = bi ? (double)b->i : b->f;
            out->type = VT_FLT;
            out->f = x / y;
        }
        return 1;
    }
    if (op == TK_PCT) {
        if (!(ai && bi)) return 0;
        if (b->i == 0) {
            err_set(c->err, 0, 0, "常量表达式取模零");
            return -1;
        }
        out->type = VT_INT;
        out->i = (a->i == INT64_MIN && b->i == -1) ? 0 : a->i % b->i;
        return 1;
    }
    return 0;
}

static int fold_expr(Comp* c, Node* n, KVal* out) {
    if (!n) return 0;
    switch (n->kind) {
        case ND_INT:
            out->type = VT_INT;
            out->i = n->ival;
            return 1;
        case ND_FLT:
            out->type = VT_FLT;
            out->f = n->fval;
            return 1;
        case ND_STR:
            out->type = VT_STR;
            out->s = n->name;
            return 1;
        case ND_BOOL:
            out->type = VT_BOL;
            out->i = n->ival ? 1 : 0;
            return 1;
        case ND_UNOP: {
            KVal k;
            int r = fold_expr(c, n->a, &k);
            if (r <= 0) return r;
            if (n->ival == KW_NOT) {
                out->type = VT_BOL;
                out->i = !kval_truthy(&k);
                return 1;
            }
            if (n->ival == TK_MINUS) {
                if (k.type == VT_INT || k.type == VT_BOL) {
                    out->type = VT_INT;
                    out->i = (int64_t)(0u - (uint64_t)k.i);
                    return 1;
                }
                if (k.type == VT_FLT) {
                    out->type = VT_FLT;
                    out->f = -k.f;
                    return 1;
                }
                return 0;
            }
            return 0;
        }
        case ND_BINOP: {
            KVal a, b;
            int ra, rb, r;
            ra = fold_expr(c, n->a, &a);
            if (ra <= 0) return ra;
            rb = fold_expr(c, n->b, &b);
            if (rb <= 0) return rb;
            r = fold_bin(c, (int)n->ival, &a, &b, out);
            if (r < 0 && c->err->line == 0) {
                c->err->line = n->line;
                c->err->col = n->col;
            }
            return r;
        }
        default:
            return 0;
    }
}

static void emit_kval(FnCtx* fc, const KVal* k, int line) {
    Comp* c = fc->c;
    switch (k->type) {
        case VT_INT:
            if (k->i == 0) code_emit(fc->code, OP_PUSH_0, 0, line, 0);
            else if (k->i == 1) code_emit(fc->code, OP_PUSH_1, 0, line, 0);
            else if (k->i >= INT32_MIN && k->i <= INT32_MAX)
                code_emit(fc->code, OP_PUSH_I, (int32_t)k->i, line, 0);
            else {
                int32_t ki = k64_index(c, VT_INT, k->i);
                code_emit(fc->code, OP_PUSH_K, ki, line, 0);
            }
            break;
        case VT_FLT: {
            int64_t bits;
            int32_t ki;
            memcpy(&bits, &k->f, sizeof(double));
            ki = k64_index(c, VT_FLT, bits);
            code_emit(fc->code, OP_PUSH_K, ki, line, 0);
            break;
        }
        case VT_STR:
            code_emit(fc->code, OP_PUSH_S, str_index(c, k->s), line, 0);
            break;
        case VT_BOL:
            code_emit(fc->code, OP_PUSH_B, k->i ? 1 : 0, line, 0);
            break;
        default:
            code_emit(fc->code, OP_PUSH_NIL, 0, line, 0);
            break;
    }
}

static void comp_expr(FnCtx* fc, Node* n, int need_value);
static void comp_stmt(FnCtx* fc, Node* n);
static int comp_block(FnCtx* fc, Node* blk);

static void emit_default_value(FnCtx* fc, Class* cl, int32_t idx, int line) {
    int ty = cl->ftype[idx];
    if (ty == KW_INT || ty == KW_BOL) code_emit(fc->code, OP_PUSH_0, 0, line, 0);
    else if (ty == KW_FLT) {
        KVal k;
        k.type = VT_FLT;
        k.f = 0.0;
        k.i = 0;
        k.s = NULL;
        emit_kval(fc, &k, line);
    } else {
        code_emit(fc->code, OP_PUSH_NIL, 0, line, 0);
    }
}

static void comp_class_get(FnCtx* fc, Node* n) {
    Comp* c = fc->c;
    int32_t cls = infer_class(fc, n->a);
    comp_expr(fc, n->a, 1);
    if (cls >= 0) {
        int32_t off = class_field_index(c, cls, n->name);
        if (off < 0) {
            err_set(c->err, n->line, n->col, "类型 %s 没有字段 %s",
                    c->classes[cls].name, n->name);
            code_emit(fc->code, OP_PUSH_NIL, 0, n->line, 0);
            return;
        }
        if (c->ncls > 65535 || off > 65535) {
            err_set(c->err, n->line, n->col, "类或字段数量超出限制");
            return;
        }
        code_emit(fc->code, OP_FGET_C, (cls << 16) | off, n->line, 0);
    } else {
        code_emit(fc->code, OP_FGET, str_index(c, n->name), n->line, 0);
    }
}

static void comp_class_lit(FnCtx* fc, Node* n) {
    Comp* c = fc->c;
    int32_t cls = class_id_by_name(c, n->name);
    Class* cl;
    Node* f;
    int32_t i;
    if (cls < 0) {
        err_set(c->err, n->line, n->col, "未定义的类型 %s", n->name);
        code_emit(fc->code, OP_PUSH_NIL, 0, n->line, 0);
        return;
    }
    cl = &c->classes[cls];
    /* Validate provided fields and detect duplicates. */
    for (f = n->a; f; f = f->next) {
        int32_t off = class_field_index(c, cls, f->name);
        Node* g;
        if (off < 0) {
            err_set(c->err, f->line, f->col, "类型 %s 没有字段 %s",
                    cl->name, f->name);
            return;
        }
        for (g = n->a; g != f; g = g->next) {
            if (!strcmp(g->name, f->name)) {
                err_set(c->err, f->line, f->col, "字段 %s 重复初始化", f->name);
                return;
            }
        }
    }
    for (i = 0; i < cl->nf; i++) {
        Node* val = NULL;
        for (f = n->a; f; f = f->next) {
            if (!strcmp(f->name, cl->fields[i])) { val = f; break; }
        }
        if (val) comp_expr(fc, val->a, 1);
        else emit_default_value(fc, cl, i, n->line);
    }
    code_emit(fc->code, OP_NEWOBJ, cls, n->line, 0);
}

static void comp_call(FnCtx* fc, Node* n, int need_value) {
    Comp* c = fc->c;
    int argc = list_len(n->a);
    Node* a;

    if (!strcmp(n->name, "print")) {
        if (argc > 255) {
            err_set(c->err, n->line, n->col, "print 参数过多");
            return;
        }
        for (a = n->a; a; a = a->next) comp_expr(fc, a, 1);
        code_emit(fc->code, OP_PRINT, 0, n->line, argc);
        if (need_value) code_emit(fc->code, OP_PUSH_NIL, 0, n->line, 0);
        return;
    }
    if (!strcmp(n->name, "input")) {
        if (argc > 1) {
            err_set(c->err, n->line, n->col, "input 只接受 0 或 1 个参数");
            return;
        }
        if (argc == 1) comp_expr(fc, n->a, 1);
        else code_emit(fc->code, OP_PUSH_S,
                       str_index(c, intern_str(c->in, c->ar, "")), n->line, 0);
        code_emit(fc->code, OP_INPUT, 0, n->line, 0);
        return;
    }
    if (!strcmp(n->name, "len") || !strcmp(n->name, "type")) {
        if (argc != 1) {
            err_set(c->err, n->line, n->col, "%s 需要且仅需要 1 个参数",
                    n->name);
            return;
        }
        comp_expr(fc, n->a, 1);
        code_emit(fc->code, !strcmp(n->name, "len") ? OP_LEN : OP_TYPE,
                  0, n->line, 0);
        return;
    }

    {
        int32_t fi = pmap_get(&c->func_map, n->name);
        Func* fn;
        if (fi == PMAP_NONE) {
            err_set(c->err, n->line, n->col, "未定义的函数 %s", n->name);
            return;
        }
        fn = &c->funcs[fi];
        if (argc != fn->params) {
            err_set(c->err, n->line, n->col,
                    "函数 %s 期望 %d 个参数，实际传入 %d 个",
                    n->name, fn->params, argc);
            return;
        }
        for (a = n->a; a; a = a->next) comp_expr(fc, a, 1);
        code_emit(fc->code, OP_CALL, fi, n->line, argc);
        /* A user call may mutate globals: invalidate static class facts. */
        pmap_invalidate_all(&c->gcls);
    }
}

static void comp_expr(FnCtx* fc, Node* n, int need_value) {
    Comp* c = fc->c;
    if (!n) {
        code_emit(fc->code, OP_PUSH_0, 0, 0, 0);
        return;
    }
    switch (n->kind) {
        case ND_INT: case ND_FLT: case ND_STR: case ND_BOOL: {
            KVal k;
            k.type = n->kind == ND_INT ? VT_INT
                   : n->kind == ND_FLT ? VT_FLT
                   : n->kind == ND_STR ? VT_STR : VT_BOL;
            k.i = n->ival;
            k.f = n->fval;
            k.s = n->name;
            emit_kval(fc, &k, n->line);
            return;
        }
        case ND_ID: {
            int32_t slot = local_slot(fc, n->name);
            if (slot >= 0) code_emit(fc->code, OP_LOAD, slot, n->line, 0);
            else code_emit(fc->code, OP_LOAD_G, global_slot(c, n->name),
                           n->line, 0);
            return;
        }
        case ND_BINOP: {
            KVal k;
            int fr = fold_expr(c, n, &k);
            if (fr < 0) return;
            if (fr > 0) {
                emit_kval(fc, &k, n->line);
                return;
            }
            if ((n->ival == TK_PLUS || n->ival == TK_MINUS ||
                 n->ival == TK_STAR) && n->b && n->b->kind == ND_INT &&
                n->b->ival >= INT32_MIN && n->b->ival <= INT32_MAX) {
                comp_expr(fc, n->a, 1);
                code_emit(fc->code,
                          n->ival == TK_PLUS ? OP_ADD_I
                          : n->ival == TK_MINUS ? OP_SUB_I : OP_MUL_I,
                          (int32_t)n->b->ival, n->line, 0);
                return;
            }
            if (n->ival == KW_AND) {
                int32_t j;
                comp_expr(fc, n->a, 1);
                j = code_emit(fc->code, OP_JF_KEEP, -1, n->line, 0);
                code_emit(fc->code, OP_POP, 0, n->line, 0);
                comp_expr(fc, n->b, 1);
                code_patch(fc->code, j, fc->code->n);
                return;
            }
            if (n->ival == KW_OR) {
                int32_t j;
                comp_expr(fc, n->a, 1);
                j = code_emit(fc->code, OP_JT_KEEP, -1, n->line, 0);
                code_emit(fc->code, OP_POP, 0, n->line, 0);
                comp_expr(fc, n->b, 1);
                code_patch(fc->code, j, fc->code->n);
                return;
            }
            comp_expr(fc, n->a, 1);
            comp_expr(fc, n->b, 1);
            switch (n->ival) {
                case TK_PLUS: code_emit(fc->code, OP_ADD, 0, n->line, 0); break;
                case TK_MINUS: code_emit(fc->code, OP_SUB, 0, n->line, 0); break;
                case TK_STAR: code_emit(fc->code, OP_MUL, 0, n->line, 0); break;
                case TK_SLASH: code_emit(fc->code, OP_DIV, 0, n->line, 0); break;
                case TK_PCT: code_emit(fc->code, OP_MOD, 0, n->line, 0); break;
                case TK_EQEQ: code_emit(fc->code, OP_EQ, 0, n->line, 0); break;
                case TK_NEQ: code_emit(fc->code, OP_NEQ, 0, n->line, 0); break;
                case TK_LT: code_emit(fc->code, OP_LT, 0, n->line, 0); break;
                case TK_GT: code_emit(fc->code, OP_GT, 0, n->line, 0); break;
                case TK_LTE: code_emit(fc->code, OP_LTE, 0, n->line, 0); break;
                case TK_GTE: code_emit(fc->code, OP_GTE, 0, n->line, 0); break;
                default: err_set(c->err, n->line, n->col, "未知运算符"); break;
            }
            return;
        }
        case ND_UNOP: {
            KVal k;
            int fr = fold_expr(c, n, &k);
            if (fr < 0) return;
            if (fr > 0) {
                emit_kval(fc, &k, n->line);
                return;
            }
            comp_expr(fc, n->a, 1);
            code_emit(fc->code, n->ival == KW_NOT ? OP_NOT : OP_NEG,
                      0, n->line, 0);
            return;
        }
        case ND_CALL:
            comp_call(fc, n, need_value);
            return;
        case ND_IDX:
            comp_expr(fc, n->a, 1);
            comp_expr(fc, n->b, 1);
            code_emit(fc->code, OP_GET, 0, n->line, 0);
            return;
        case ND_FLD:
            comp_class_get(fc, n);
            return;
        case ND_ARR: {
            Node* e;
            int32_t cnt = 0;
            for (e = n->a; e; e = e->next) {
                comp_expr(fc, e, 1);
                cnt++;
            }
            code_emit(fc->code, OP_ARRAY, cnt, n->line, 0);
            return;
        }
        case ND_CLASS_LIT:
            comp_class_lit(fc, n);
            return;
        default:
            code_emit(fc->code, OP_PUSH_NIL, 0, n->line, 0);
            return;
    }
}

/* Resolve assignment storage: local if it exists, else a pre-existing global,
 * else a new local (in functions) or a new global (at module scope). */
static void resolve_store(FnCtx* fc, const char* name, int32_t* slot,
                          int* is_global) {
    if (!fc->is_main) {
        int32_t s = pmap_get(&fc->vars, name);
        if (s != PMAP_NONE) {
            *slot = s;
            *is_global = 0;
            return;
        }
    }
    {
        int32_t g = pmap_get(&fc->c->globals, name);
        if (fc->is_main || g != PMAP_NONE) {
            *slot = (g == PMAP_NONE) ? global_slot(fc->c, name) : g;
            *is_global = 1;
            return;
        }
    }
    {
        int32_t s = alloc_slot(fc);
        pmap_put(&fc->vars, name, s);
        *slot = s;
        *is_global = 0;
    }
}

static int binop_store_op(int op) {
    switch (op) {
        case TK_PLUS: return OP_ADD_ST;
        case TK_MINUS: return OP_SUB_ST;
        case TK_STAR: return OP_MUL_ST;
        case TK_SLASH: return OP_DIV_ST;
        case TK_PCT: return OP_MOD_ST;
        default: return -1;
    }
}

static void comp_assign(FnCtx* fc, Node* lv, Node* rv) {
    Comp* c = fc->c;
    if (lv->kind == ND_ID) {
        int32_t slot;
        int is_g;
        int32_t cls = infer_class(fc, rv);
        resolve_store(fc, lv->name, &slot, &is_g);
        if (cls < 0) cls = -1;
        {
            int32_t prev = class_of_var(fc, lv->name);
            if (prev >= 0 && prev != cls) cls = -1;
        }
        set_class_of_var(fc, lv->name, cls);

        if (rv && rv->kind == ND_BINOP && rv->a &&
            rv->a->kind == ND_ID && !strcmp(rv->a->name, lv->name)) {
            if (rv->ival == TK_PLUS && rv->b && rv->b->kind == ND_INT &&
                rv->b->ival == 1) {
                code_emit(fc->code, OP_INC, slot, lv->line, is_g ? 1 : 0);
                return;
            }
            if (rv->ival == TK_PLUS && rv->b && rv->b->kind == ND_IDX &&
                node_is_pure(rv->b->a) && node_is_pure(rv->b->b)) {
                comp_expr(fc, rv->b->a, 1);
                comp_expr(fc, rv->b->b, 1);
                code_emit(fc->code, OP_ADD_ST_IDX, slot, lv->line,
                          is_g ? 1 : 0);
                return;
            }
            if (node_is_pure(rv->b)) {
                int sop = binop_store_op((int)rv->ival);
                if (sop >= 0) {
                    comp_expr(fc, rv->b, 1);
                    code_emit(fc->code, sop, slot, lv->line, is_g ? 1 : 0);
                    return;
                }
            }
        }
        comp_expr(fc, rv, 1);
        if (is_g) code_emit(fc->code, OP_STORE_G, slot, lv->line, 0);
        else code_emit(fc->code, OP_STORE, slot, lv->line, 0);
        return;
    }
    if (lv->kind == ND_IDX) {
        comp_expr(fc, lv->a, 1);
        comp_expr(fc, lv->b, 1);
        comp_expr(fc, rv, 1);
        code_emit(fc->code, OP_SET, 0, lv->line, 0);
        return;
    }
    if (lv->kind == ND_FLD) {
        int32_t cls = infer_class(fc, lv->a);
        int32_t off = -1;
        if (cls >= 0) {
            off = class_field_index(c, cls, lv->name);
            if (off < 0) {
                err_set(c->err, lv->line, lv->col, "类型 %s 没有字段 %s",
                        c->classes[cls].name, lv->name);
                return;
            }
        }
        if (rv && rv->kind == ND_BINOP && cls >= 0 && off >= 0 &&
            field_op_aux((int)rv->ival) >= 0 &&
            same_field_lvalue(lv, rv->a) && node_is_pure(rv->b)) {
            comp_expr(fc, lv->a, 1);
            comp_expr(fc, rv->b, 1);
            code_emit(fc->code, OP_FOP_C, (cls << 16) | off, lv->line,
                      (int)field_op_aux((int)rv->ival));
            return;
        }
        comp_expr(fc, lv->a, 1);
        comp_expr(fc, rv, 1);
        if (cls >= 0) code_emit(fc->code, OP_FSET_C, (cls << 16) | off,
                                lv->line, 0);
        else code_emit(fc->code, OP_FSET, str_index(c, lv->name), lv->line, 0);
        return;
    }
    err_set(c->err, lv->line, lv->col, "非法的赋值目标");
}

static void comp_cond_jump(FnCtx* fc, Node* cond) {
    int fused = -1;
    if (cond && cond->kind == ND_BINOP) {
        switch (cond->ival) {
            case TK_LT: fused = OP_LT_JF; break;
            case TK_LTE: fused = OP_LE_JF; break;
            case TK_GT: fused = OP_GT_JF; break;
            case TK_GTE: fused = OP_GE_JF; break;
            case TK_EQEQ: fused = OP_EQ_JF; break;
            case TK_NEQ: fused = OP_NEQ_JF; break;
            default: break;
        }
    }
    if (fused >= 0) {
        comp_expr(fc, cond->a, 1);
        comp_expr(fc, cond->b, 1);
        code_emit(fc->code, fused, -1, cond->line, 0);
    } else {
        comp_expr(fc, cond, 1);
        code_emit(fc->code, OP_JF, -1, cond ? cond->line : 0, 0);
    }
}

static int const_condition(Comp* c, Node* cond, int* truth) {
    KVal k;
    int r;
    if (!cond) return 0;
    r = fold_expr(c, cond, &k);
    if (r < 0) return -1;
    if (r == 0) return 0;
    *truth = kval_truthy(&k);
    return 1;
}

static void comp_if(FnCtx* fc, Node* n) {
    int truth = 0;
    int cc = const_condition(fc->c, n->a, &truth);
    if (cc < 0) return;
    if (cc == 1) {
        if (truth) {
            comp_block(fc, n->b);
        } else if (n->c) {
            if (n->c->kind == ND_IF) comp_stmt(fc, n->c);
            else comp_block(fc, n->c);
        }
        return;
    }
    {
        int32_t jf;
        comp_cond_jump(fc, n->a);
        jf = fc->code->n - 1;
        comp_block(fc, n->b);
        if (n->c) {
            int32_t jend = code_emit(fc->code, OP_JMP, -1, n->line, 0);
            code_patch(fc->code, jf, fc->code->n);
            if (n->c->kind == ND_IF) comp_stmt(fc, n->c);
            else comp_block(fc, n->c);
            code_patch(fc->code, jend, fc->code->n);
        } else {
            code_patch(fc->code, jf, fc->code->n);
        }
    }
}

static void comp_while(FnCtx* fc, Node* n) {
    int truth = 0;
    int cc = const_condition(fc->c, n->a, &truth);
    int32_t loop;
    if (cc < 0) return;
    loop = fc->code->n;
    if (cc == 1) {
        if (!truth) return;
        comp_block(fc, n->b);
        code_emit(fc->code, OP_JMP, loop, n->line, 0);
        return;
    }
    {
        int32_t jf;
        comp_cond_jump(fc, n->a);
        jf = fc->code->n - 1;
        comp_block(fc, n->b);
        code_emit(fc->code, OP_JMP, loop, n->line, 0);
        code_patch(fc->code, jf, fc->code->n);
    }
}

static int block_assigns(Node* blk, const char* name) {
    Node* body = (blk && blk->kind == ND_BLOCK) ? blk->a : blk;
    Node* s;
    for (s = body; s; s = s->next) {
        switch (s->kind) {
            case ND_ASSIGN:
                if (s->a && s->a->kind == ND_ID &&
                    !strcmp(s->a->name, name))
                    return 1;
                break;
            case ND_IF:
                if (block_assigns(s->b, name)) return 1;
                if (s->c && block_assigns(s->c, name)) return 1;
                break;
            case ND_WHILE:
                if (block_assigns(s->b, name)) return 1;
                break;
            case ND_FOR:
                if (s->name == name || !strcmp(s->name, name)) return 1;
                if (block_assigns(s->b, name)) return 1;
                break;
            case ND_BLOCK:
                if (block_assigns(s, name)) return 1;
                break;
            default:
                break;
        }
    }
    return 0;
}

static void comp_for(FnCtx* fc, Node* n) {
    int32_t var_slot;
    int var_g;
    int32_t limit_slot;
    int32_t loop;
    int32_t jf;
    if (fc->is_main) {
        resolve_store(fc, n->name, &var_slot, &var_g);
    } else {
        /* 函数内循环变量始终为局部槽，避免意外写入模块全局变量。 */
        var_slot = alloc_slot(fc);
        pmap_put(&fc->vars, n->name, var_slot);
        var_g = 0;
        set_class_of_var(fc, n->name, -1);
    }
    limit_slot = alloc_slot(fc);

    comp_expr(fc, n->a, 1);
    code_emit(fc->code, OP_STORE, limit_slot, n->line, 0);
    code_emit(fc->code, OP_PUSH_0, 0, n->line, 0);
    code_emit(fc->code, var_g ? OP_STORE_G : OP_STORE, var_slot, n->line, 0);

    if (block_assigns(n->b, n->name)) {
        /* 循环体修改了循环变量：使用隐藏计数器，保证迭代次数稳定（Python 语义）。 */
        int32_t counter = alloc_slot(fc);
        code_emit(fc->code, OP_PUSH_0, 0, n->line, 0);
        code_emit(fc->code, OP_STORE, counter, n->line, 0);
        set_class_of_var(fc, n->name, -1);
        loop = fc->code->n;
        code_emit(fc->code, OP_LOAD, counter, n->line, 0);
        code_emit(fc->code, OP_LOAD, limit_slot, n->line, 0);
        jf = code_emit(fc->code, OP_LT_JF, -1, n->line, 0);
        code_emit(fc->code, OP_LOAD, counter, n->line, 0);
        code_emit(fc->code, var_g ? OP_STORE_G : OP_STORE, var_slot, n->line, 0);
        comp_block(fc, n->b);
        code_emit(fc->code, OP_INC, counter, n->line, 0);
        code_emit(fc->code, OP_JMP, loop, n->line, 0);
        code_patch(fc->code, jf, fc->code->n);
        return;
    }

    loop = fc->code->n;
    code_emit(fc->code, var_g ? OP_LOAD_G : OP_LOAD, var_slot, n->line, 0);
    code_emit(fc->code, OP_LOAD, limit_slot, n->line, 0);
    jf = code_emit(fc->code, OP_LT_JF, -1, n->line, 0);
    comp_block(fc, n->b);
    code_emit(fc->code, OP_INC, var_slot, n->line, var_g ? 1 : 0);
    code_emit(fc->code, OP_JMP, loop, n->line, 0);
    code_patch(fc->code, jf, fc->code->n);
}

static void comp_stmt(FnCtx* fc, Node* n) {
    Comp* c = fc->c;
    if (!n) return;
    switch (n->kind) {
        case ND_ASSIGN:
            comp_assign(fc, n->a, n->b);
            return;
        case ND_IF:
            comp_if(fc, n);
            return;
        case ND_WHILE:
            comp_while(fc, n);
            return;
        case ND_FOR:
            comp_for(fc, n);
            return;
        case ND_RET:
            if (fc->is_main) {
                err_set(c->err, n->line, n->col, "return 不能出现在顶层");
                return;
            }
            if (n->a) comp_expr(fc, n->a, 1);
            else code_emit(fc->code, OP_PUSH_0, 0, n->line, 0);
            code_emit(fc->code, OP_RET, 0, n->line, 0);
            return;
        case ND_BLOCK:
            comp_block(fc, n);
            return;
        case ND_FN:
        case ND_CLASS:
            err_set(c->err, n->line, n->col, "函数/类定义必须位于顶层");
            return;
        case ND_INT: case ND_FLT: case ND_STR: case ND_BOOL: case ND_ID:
            return; /* pure expression statement: no code */
        default:
            comp_expr(fc, n, 1);
            code_emit(fc->code, OP_POP, 0, n->line, 0);
            return;
    }
}

static int comp_block(FnCtx* fc, Node* blk) {
    Node* body = (blk && blk->kind == ND_BLOCK) ? blk->a : blk;
    Node* s;
    for (s = body; s; s = s->next) {
        comp_stmt(fc, s);
        if (err_has(fc->c->err)) return 1;
        if (node_terminates(s)) return 1; /* dead code elimination */
    }
    return 0;
}

static void register_functions(Comp* c, Node* prog) {
    Node* s;
    for (s = prog->a; s; s = s->next) {
        if (s->kind == ND_FN) {
            int32_t idx;
            if (pmap_get(&c->func_map, s->name) != PMAP_NONE) {
                err_set(c->err, s->line, s->col, "函数 %s 重复定义", s->name);
                return;
            }
            if (c->nfunc >= c->cfunc) {
                c->cfunc = c->cfunc ? c->cfunc * 2 : 16;
                c->funcs = xrealloc(c->funcs, sizeof(Func) * (size_t)c->cfunc);
            }
            idx = c->nfunc++;
            memset(&c->funcs[idx], 0, sizeof(Func));
            c->funcs[idx].name = s->name;
            c->funcs[idx].node = s;
            c->funcs[idx].params = list_len(s->a);
            if (c->funcs[idx].params > 255) {
                err_set(c->err, s->line, s->col, "函数参数过多");
                return;
            }
            code_init(&c->funcs[idx].code);
            pmap_put(&c->func_map, s->name, idx);
        }
    }
}

static int register_classes(Comp* c, Node* prog) {
    Node* s;
    for (s = prog->a; s; s = s->next) {
        if (s->kind == ND_CLASS) {
            int32_t id;
            if (pmap_get(&c->class_map, s->name) != PMAP_NONE) {
                err_set(c->err, s->line, s->col, "类型 %s 重复定义", s->name);
                return -1;
            }
            if (c->ncls >= 65535) {
                err_set(c->err, s->line, s->col, "类型数量超出限制");
                return -1;
            }
            if (c->ncls >= c->ccls) {
                c->ccls = c->ccls ? c->ccls * 2 : 8;
                c->classes = xrealloc(c->classes,
                                      sizeof(Class) * (size_t)c->ccls);
            }
            id = c->ncls++;
            memset(&c->classes[id], 0, sizeof(Class));
            c->classes[id].name = s->name;
            c->classes[id].nf = list_len(s->a);
            pmap_put(&c->class_map, s->name, id);
        }
    }
    for (s = prog->a; s; s = s->next) {
        if (s->kind == ND_CLASS) {
            int32_t id = class_id_by_name(c, s->name);
            Class* cl = &c->classes[id];
            Node* f;
            int32_t i = 0;
            if (cl->nf > 65535) {
                err_set(c->err, s->line, s->col, "类型字段过多");
                return -1;
            }
            cl->fields = arena_alloc(c->ar, sizeof(char*) * (size_t)(cl->nf ? cl->nf : 1));
            cl->ftype = arena_alloc(c->ar, sizeof(int32_t) * (size_t)(cl->nf ? cl->nf : 1));
            cl->fclass = arena_alloc(c->ar, sizeof(int32_t) * (size_t)(cl->nf ? cl->nf : 1));
            for (f = s->a; f; f = f->next) {
                int32_t j;
                cl->fields[i] = f->name;
                for (j = 0; j < i; j++) {
                    if (!strcmp(cl->fields[j], f->name)) {
                        err_set(c->err, f->line, f->col, "字段 %s 重复定义",
                                f->name);
                        return -1;
                    }
                }
                if (f->ival == TK_ID) {
                    int32_t cid = f->b ? class_id_by_name(c, f->b->name) : -1;
                    if (cid < 0) {
                        err_set(c->err, f->line, f->col, "未定义的类型 %s",
                                f->b ? f->b->name : "?");
                        return -1;
                    }
                    cl->ftype[i] = KW_VOID;
                    cl->fclass[i] = cid;
                } else {
                    cl->ftype[i] = (int32_t)f->ival;
                    cl->fclass[i] = -1;
                }
                i++;
            }
        }
    }
    return 0;
}

static void scan_globals(Comp* c, Node* n) {
    if (!n) return;
    switch (n->kind) {
        case ND_ASSIGN:
            if (n->a && n->a->kind == ND_ID) global_slot(c, n->a->name);
            return;
        case ND_IF:
            scan_globals(c, n->b);
            if (n->c) scan_globals(c, n->c);
            return;
        case ND_WHILE:
            scan_globals(c, n->b);
            return;
        case ND_FOR:
            global_slot(c, n->name);
            scan_globals(c, n->b);
            return;
        case ND_BLOCK: {
            Node* s;
            for (s = n->a; s; s = s->next) scan_globals(c, s);
            return;
        }
        default:
            return;
    }
}

static void comp_function(Comp* c, Func* f) {
    FnCtx fc;
    int32_t i = 0;
    Node* p;
    int terminated;
    memset(&fc, 0, sizeof(fc));
    fc.c = c;
    fc.code = &f->code;
    pmap_init(&fc.vars);
    pmap_init(&fc.vcls);
    for (p = f->node->a; p; p = p->next) {
        if (pmap_get(&fc.vars, p->name) != PMAP_NONE) {
            err_set(c->err, p->line, p->col, "参数 %s 重复", p->name);
            break;
        }
        pmap_put(&fc.vars, p->name, i++);
    }
    fc.next_slot = i;
    fc.max_slot = i;
    terminated = comp_block(&fc, f->node->b);
    if (!terminated) {
        code_emit(fc.code, OP_PUSH_0, 0, 0, 0);
        code_emit(fc.code, OP_RET, 0, 0, 0);
    }
    if (fc.max_slot < f->params) fc.max_slot = f->params;
    f->max_slots = fc.max_slot;
    f->max_stack = block_depth(f->node->b) + 2;
    pmap_free(&fc.vars);
    pmap_free(&fc.vcls);
}

static void comp_program(Comp* c, Node* prog) {
    FnCtx fc;
    Node* s;
    int32_t i;
    register_functions(c, prog);
    if (err_has(c->err)) return;
    if (register_classes(c, prog) < 0) return;
    for (s = prog->a; s; s = s->next) scan_globals(c, s);
    for (i = 0; i < c->nfunc; i++) {
        comp_function(c, &c->funcs[i]);
        if (err_has(c->err)) return;
    }
    memset(&fc, 0, sizeof(fc));
    fc.c = c;
    fc.code = &c->main_code;
    fc.is_main = 1;
    pmap_init(&fc.vars);
    pmap_init(&fc.vcls);
    c->main_max_stack = 1;
    for (s = prog->a; s; s = s->next) {
        if (s->kind == ND_FN || s->kind == ND_CLASS) continue;
        c->main_max_stack = maxi32(c->main_max_stack, stmt_depth(s));
        comp_stmt(&fc, s);
        if (err_has(c->err)) break;
    }
    c->main_max_stack += 2;
    code_emit(&c->main_code, OP_HALT, 0, 0, 0);
    c->main_slots = fc.max_slot;
    pmap_free(&fc.vars);
    pmap_free(&fc.vcls);
}

/* ================================================================
 *  Section 8: Runtime values, heap and output
 * ================================================================ */

typedef struct {
    union {
        int64_t i;
        double f;
        const char* s;
        void* p;
    } u;
    uint8_t type;
} Val;

typedef struct {
    int32_t len;
    int32_t cap;
    Val* data;
} Arr;

typedef struct {
    int32_t cls;
    int32_t nf;
    Val fields[];
} Obj;

typedef struct {
    Instr* ret_ip;
    Instr* ret_code;
    int32_t base;
} Frame;

enum { HK_STR = 1, HK_ARR, HK_OBJ };

typedef struct {
    void** ptrs;
    uint8_t* kinds;
    int32_t count;
    int32_t cap;
} Heap;

typedef struct {
    FILE* f;
    char* buf;
    size_t len;
    int error;
} Out;

typedef struct {
    Val* stack;
    int32_t cap;
    Val* globals;
    int32_t nglobals;
    Frame* frames;
    int32_t cframes;
    Heap heap;
    Out out;
    Arena* ar;
    const char** strs;
    int32_t nstr;
    Func* funcs;
    int32_t nfunc;
    Class* classes;
    int32_t ncls;
    K64* k64;
    int32_t nk64;
    DscErr* err;
} Vm;

static Val v_nil(void) {
    Val v;
    v.u.i = 0;
    v.type = VT_NIL;
    return v;
}

static Val v_int(int64_t x) {
    Val v;
    v.u.i = x;
    v.type = VT_INT;
    return v;
}

static Val v_flt(double x) {
    Val v;
    v.u.f = x;
    v.type = VT_FLT;
    return v;
}

static Val v_str(const char* s) {
    Val v;
    v.u.s = s;
    v.type = VT_STR;
    return v;
}

static Val v_bol(int b) {
    Val v;
    v.u.i = b ? 1 : 0;
    v.type = VT_BOL;
    return v;
}

static Val v_arr(Arr* a) {
    Val v;
    v.u.p = a;
    v.type = VT_ARR;
    return v;
}

static Val v_obj(Obj* o) {
    Val v;
    v.u.p = o;
    v.type = VT_OBJ;
    return v;
}

static const char* val_typename(Val v) {
    switch (v.type) {
        case VT_INT: return "int";
        case VT_FLT: return "float";
        case VT_STR: return "str";
        case VT_BOL: return "bool";
        case VT_ARR: return "array";
        case VT_OBJ: return "object";
        default: return "nil";
    }
}

static void heap_add(Vm* vm, void* p, int kind) {
    Heap* h = &vm->heap;
    if (h->count >= h->cap) {
        h->cap = h->cap ? h->cap * 2 : 64;
        h->ptrs = xrealloc(h->ptrs, sizeof(void*) * (size_t)h->cap);
        h->kinds = xrealloc(h->kinds, sizeof(uint8_t) * (size_t)h->cap);
    }
    h->ptrs[h->count] = p;
    h->kinds[h->count] = (uint8_t)kind;
    h->count++;
}

static void heap_free_all(Heap* h) {
    int32_t i;
    for (i = 0; i < h->count; i++) {
        if (h->kinds[i] == HK_ARR) {
            Arr* a = h->ptrs[i];
            free(a->data);
        }
        free(h->ptrs[i]);
    }
    free(h->ptrs);
    free(h->kinds);
    h->ptrs = NULL;
    h->kinds = NULL;
    h->count = 0;
    h->cap = 0;
}

static void out_flush(Out* o) {
    if (o->len) {
        if (fwrite(o->buf, 1, o->len, o->f) != o->len) o->error = 1;
        o->len = 0;
    }
}

static void out_write(Out* o, const char* s, size_t n) {
    while (n) {
        size_t room = 65536u - o->len;
        size_t k;
        if (!room) {
            out_flush(o);
            room = 65536u;
        }
        k = (n < room) ? n : room;
        memcpy(o->buf + o->len, s, k);
        o->len += k;
        s += k;
        n -= k;
    }
}

static void out_ch(Out* o, char c) {
    if (o->len == 65536u) out_flush(o);
    o->buf[o->len++] = c;
}

static void out_str(Out* o, const char* s) {
    if (s) out_write(o, s, strlen(s));
}

static int fmt_i64(char* dst, int64_t v) {
    char tmp[24];
    int n = 0;
    int len = 0;
    uint64_t u = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    do {
        tmp[n++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u);
    if (v < 0) dst[len++] = '-';
    while (n) dst[len++] = tmp[--n];
    return len;
}

static int fmt_double(char* dst, size_t cap, double v) {
    int prec;
    if (v != v) {
        memcpy(dst, "nan", 4);
        return 3;
    }
    if (v > DBL_MAX) {
        memcpy(dst, "inf", 4);
        return 3;
    }
    if (v < -DBL_MAX) {
        memcpy(dst, "-inf", 5);
        return 4;
    }
    for (prec = 1; prec <= 17; prec++) {
        int n = snprintf(dst, cap, "%.*g", prec, v);
        if (n > 0 && (size_t)n < cap && strtod(dst, NULL) == v) return n;
    }
    {
        int n = snprintf(dst, cap, "%.17g", v);
        return n > 0 ? n : 0;
    }
}

static void out_i64(Out* o, int64_t v) {
    char tmp[24];
    out_write(o, tmp, (size_t)fmt_i64(tmp, v));
}

static void out_double(Out* o, double v) {
    char tmp[64];
    out_write(o, tmp, (size_t)fmt_double(tmp, sizeof(tmp), v));
}

static void out_val(Out* o, Val v, int depth) {
    int32_t i;
    switch (v.type) {
        case VT_INT:
            out_i64(o, v.u.i);
            break;
        case VT_FLT:
            out_double(o, v.u.f);
            break;
        case VT_STR:
            out_str(o, v.u.s ? v.u.s : "nil");
            break;
        case VT_BOL:
            out_str(o, v.u.i ? "True" : "False");
            break;
        case VT_ARR: {
            Arr* a = v.u.p;
            if (depth >= 8) {
                out_str(o, "[...]");
                break;
            }
            out_ch(o, '[');
            for (i = 0; i < a->len; i++) {
                if (i) out_str(o, ", ");
                out_val(o, a->data[i], depth + 1);
            }
            out_ch(o, ']');
            break;
        }
        case VT_OBJ:
            out_str(o, "{object}");
            break;
        default:
            out_str(o, "nil");
            break;
    }
}

static int rt_error(Vm* vm, int line, const char* fmt, ...) {
    va_list ap;
    if (!vm->err->msg[0]) {
        vm->err->line = line;
        vm->err->col = 0;
        va_start(ap, fmt);
        vsnprintf(vm->err->msg, sizeof(vm->err->msg), fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int is_intlike(Val v) {
    return v.type == VT_INT || v.type == VT_BOL;
}

static int is_num(Val v) {
    return v.type == VT_INT || v.type == VT_BOL || v.type == VT_FLT;
}

static double as_d(Val v) {
    return v.type == VT_FLT ? v.u.f : (double)v.u.i;
}

static int val_truthy(Val v) {
    switch (v.type) {
        case VT_INT: return v.u.i != 0;
        case VT_FLT: return v.u.f != 0.0;
        case VT_STR: return v.u.s && v.u.s[0];
        case VT_BOL: return v.u.i != 0;
        case VT_ARR: return v.u.p != NULL;
        case VT_OBJ: return v.u.p != NULL;
        default: return 0;
    }
}

static int val_eq(Val a, Val b) {
    if (is_num(a) && is_num(b)) {
        if (is_intlike(a) && is_intlike(b)) return a.u.i == b.u.i;
        return as_d(a) == as_d(b);
    }
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VT_NIL: return 1;
        case VT_STR:
            return a.u.s == b.u.s ||
                   (a.u.s && b.u.s && !strcmp(a.u.s, b.u.s));
        case VT_ARR:
        case VT_OBJ:
            return a.u.p == b.u.p;
        default:
            return 0;
    }
}

static size_t concat_len(Val v) {
    char tmp[64];
    switch (v.type) {
        case VT_STR: return strlen(v.u.s ? v.u.s : "");
        case VT_INT: return (size_t)fmt_i64(tmp, v.u.i);
        case VT_FLT: return (size_t)fmt_double(tmp, sizeof(tmp), v.u.f);
        case VT_BOL: return v.u.i ? 4u : 5u;
        default: return 3u;
    }
}

static void concat_write(char* dst, Val v) {
    char tmp[64];
    switch (v.type) {
        case VT_STR:
            if (v.u.s) memcpy(dst, v.u.s, strlen(v.u.s));
            break;
        case VT_INT:
            memcpy(dst, tmp, (size_t)fmt_i64(tmp, v.u.i));
            break;
        case VT_FLT:
            memcpy(dst, tmp, (size_t)fmt_double(tmp, sizeof(tmp), v.u.f));
            break;
        case VT_BOL:
            if (v.u.i) memcpy(dst, "True", 4);
            else memcpy(dst, "False", 5);
            break;
        default:
            memcpy(dst, "nil", 3);
            break;
    }
}

static int val_concat(Vm* vm, Val a, Val b, Val* out, int line) {
    size_t la = concat_len(a);
    size_t lb = concat_len(b);
    char* s;
    if (la > SIZE_MAX - lb - 1) return rt_error(vm, line, "字符串过长");
    s = arena_alloc(vm->ar, la + lb + 1);
    concat_write(s, a);
    concat_write(s + la, b);
    s[la + lb] = 0;
    *out = v_str(s);
    return 0;
}

static int val_add(Vm* vm, Val a, Val b, Val* out, int line) {
    if (is_intlike(a) && is_intlike(b)) {
        *out = v_int((int64_t)((uint64_t)a.u.i + (uint64_t)b.u.i));
        return 0;
    }
    if (a.type == VT_STR || b.type == VT_STR)
        return val_concat(vm, a, b, out, line);
    if (is_num(a) && is_num(b)) {
        *out = v_flt(as_d(a) + as_d(b));
        return 0;
    }
    return rt_error(vm, line, "不支持的操作数类型: %s + %s",
                    val_typename(a), val_typename(b));
}

static int val_sub(Vm* vm, Val a, Val b, Val* out, int line) {
    if (is_intlike(a) && is_intlike(b)) {
        *out = v_int((int64_t)((uint64_t)a.u.i - (uint64_t)b.u.i));
        return 0;
    }
    if (is_num(a) && is_num(b)) {
        *out = v_flt(as_d(a) - as_d(b));
        return 0;
    }
    return rt_error(vm, line, "不支持的操作数类型: %s - %s",
                    val_typename(a), val_typename(b));
}

static int val_mul(Vm* vm, Val a, Val b, Val* out, int line) {
    if (is_intlike(a) && is_intlike(b)) {
        *out = v_int((int64_t)((uint64_t)a.u.i * (uint64_t)b.u.i));
        return 0;
    }
    if (is_num(a) && is_num(b)) {
        *out = v_flt(as_d(a) * as_d(b));
        return 0;
    }
    return rt_error(vm, line, "不支持的操作数类型: %s * %s",
                    val_typename(a), val_typename(b));
}

static int val_div(Vm* vm, Val a, Val b, Val* out, int line) {
    if (is_intlike(a) && is_intlike(b)) {
        if (b.u.i == 0) return rt_error(vm, line, "整数除以零");
        if (a.u.i == INT64_MIN && b.u.i == -1)
            return rt_error(vm, line, "整数除法溢出");
        *out = v_int(a.u.i / b.u.i);
        return 0;
    }
    if (is_num(a) && is_num(b)) {
        *out = v_flt(as_d(a) / as_d(b));
        return 0;
    }
    return rt_error(vm, line, "不支持的操作数类型: %s / %s",
                    val_typename(a), val_typename(b));
}

static int val_mod(Vm* vm, Val a, Val b, Val* out, int line) {
    if (is_intlike(a) && is_intlike(b)) {
        if (b.u.i == 0) return rt_error(vm, line, "整数取模零");
        *out = v_int((a.u.i == INT64_MIN && b.u.i == -1) ? 0 : a.u.i % b.u.i);
        return 0;
    }
    return rt_error(vm, line, "取模运算要求整数操作数");
}

static int val_neg(Vm* vm, Val a, Val* out, int line) {
    if (is_intlike(a)) {
        *out = v_int((int64_t)(0u - (uint64_t)a.u.i));
        return 0;
    }
    if (a.type == VT_FLT) {
        *out = v_flt(-a.u.f);
        return 0;
    }
    return rt_error(vm, line, "不能对 %s 取负", val_typename(a));
}

static int val_compare(Vm* vm, Val a, Val b, int op, int* out, int line) {
    int r;
    if (is_num(a) && is_num(b)) {
        if (is_intlike(a) && is_intlike(b)) {
            int64_t x = a.u.i;
            int64_t y = b.u.i;
            r = op == TK_LT ? x < y
              : op == TK_GT ? x > y
              : op == TK_LTE ? x <= y : x >= y;
        } else {
            double x = as_d(a);
            double y = as_d(b);
            r = op == TK_LT ? x < y
              : op == TK_GT ? x > y
              : op == TK_LTE ? x <= y : x >= y;
        }
    } else if (a.type == VT_STR && b.type == VT_STR) {
        int cmp = strcmp(a.u.s ? a.u.s : "", b.u.s ? b.u.s : "");
        r = op == TK_LT ? cmp < 0
          : op == TK_GT ? cmp > 0
          : op == TK_LTE ? cmp <= 0 : cmp >= 0;
    } else {
        return rt_error(vm, line, "无法比较 %s 和 %s",
                        val_typename(a), val_typename(b));
    }
    *out = r;
    return 0;
}

static int val_length(Vm* vm, Val v, int64_t* out, int line) {
    switch (v.type) {
        case VT_ARR:
            *out = ((Arr*)v.u.p)->len;
            return 0;
        case VT_STR:
            *out = (int64_t)strlen(v.u.s ? v.u.s : "");
            return 0;
        case VT_OBJ:
            *out = ((Obj*)v.u.p)->nf;
            return 0;
        default:
            return rt_error(vm, line, "len() 不支持 %s", val_typename(v));
    }
}

static int32_t field_lookup(const Class* cl, const char* name) {
    int32_t i;
    for (i = 0; i < cl->nf; i++)
        if (cl->fields[i] == name || !strcmp(cl->fields[i], name)) return i;
    return -1;
}

/* ================================================================
 *  Section 9: Virtual machine
 * ================================================================ */

static Val* stack_grow(Vm* vm, Val* old, int32_t* cap, int32_t need) {
    int32_t nc = *cap ? *cap : 1024;
    while (nc < need) {
        if (nc > INT32_MAX / 2) oom();
        nc *= 2;
    }
    old = xrealloc(old, sizeof(Val) * (size_t)nc);
    *cap = nc;
    vm->stack = old;
    return old;
}

static int vm_run(Vm* vm, Instr* start, int32_t main_slots,
                  int32_t main_max_stack) {
    Val* stack = vm->stack;
    int32_t cap = vm->cap;
    int32_t sp = main_slots;
    int32_t base = 0;
    Instr* code = start;
    Instr* ip = start;
    Frame* frames = vm->frames;
    int32_t cframes = vm->cframes;
    int32_t nframes = 0;
    Val* globals = vm->globals;
    Out* out = &vm->out;
    int32_t i;
    int status = 0;

    if (main_slots + main_max_stack + 4 > cap)
        stack = stack_grow(vm, stack, &cap, main_slots + main_max_stack + 4);
    for (i = 0; i < main_slots; i++) stack[i] = v_nil();

#define BIN_NUM(op_, fn_)                                                   \
    do {                                                                    \
        Val b_ = stack[--sp], a_ = stack[--sp];                             \
        if (DSC_LIKELY(a_.type == VT_INT && b_.type == VT_INT)) {           \
            uint64_t x_ = (uint64_t)a_.u.i, y_ = (uint64_t)b_.u.i;          \
            stack[sp].type = VT_INT;                                        \
            stack[sp++].u.i = (int64_t)(x_ op_ y_);                         \
        } else {                                                            \
            Val r_;                                                         \
            if (fn_(vm, a_, b_, &r_, inst->line) < 0) goto fail;            \
            stack[sp++] = r_;                                               \
        }                                                                   \
    } while (0)

    for (;;) {
        Instr* inst = ip++;
        switch (inst->op) {
            case OP_HALT:
                status = 0;
                goto done;

            case OP_PUSH_I:
                stack[sp].type = VT_INT;
                stack[sp++].u.i = inst->arg;
                break;
            case OP_PUSH_K: {
                K64* k = &vm->k64[inst->arg];
                if (k->type == VT_INT) {
                    stack[sp].type = VT_INT;
                    stack[sp++].u.i = k->bits;
                } else {
                    double d;
                    memcpy(&d, &k->bits, sizeof(double));
                    stack[sp].type = VT_FLT;
                    stack[sp++].u.f = d;
                }
                break;
            }
            case OP_PUSH_S:
                stack[sp].type = VT_STR;
                stack[sp++].u.s = vm->strs[inst->arg];
                break;
            case OP_PUSH_B:
                stack[sp].type = VT_BOL;
                stack[sp++].u.i = inst->arg ? 1 : 0;
                break;
            case OP_PUSH_NIL:
                stack[sp++] = v_nil();
                break;
            case OP_PUSH_0:
                stack[sp].type = VT_INT;
                stack[sp++].u.i = 0;
                break;
            case OP_PUSH_1:
                stack[sp].type = VT_INT;
                stack[sp++].u.i = 1;
                break;

            case OP_LOAD:
                stack[sp++] = stack[base + inst->arg];
                break;
            case OP_STORE:
                stack[base + inst->arg] = stack[--sp];
                break;
            case OP_LOAD_G:
                stack[sp++] = globals[inst->arg];
                break;
            case OP_STORE_G:
                globals[inst->arg] = stack[--sp];
                break;

            case OP_ADD:
                BIN_NUM(+, val_add);
                break;
            case OP_SUB:
                BIN_NUM(-, val_sub);
                break;
            case OP_MUL:
                BIN_NUM(*, val_mul);
                break;
            case OP_DIV: {
                Val b = stack[--sp], a = stack[--sp];
                Val r;
                if (val_div(vm, a, b, &r, inst->line) < 0) goto fail;
                stack[sp++] = r;
                break;
            }
            case OP_MOD: {
                Val b = stack[--sp], a = stack[--sp];
                Val r;
                if (val_mod(vm, a, b, &r, inst->line) < 0) goto fail;
                stack[sp++] = r;
                break;
            }

            case OP_ADD_I:
            case OP_SUB_I:
            case OP_MUL_I: {
                Val a = stack[--sp];
                int64_t imm = inst->arg;
                if (DSC_LIKELY(a.type == VT_INT)) {
                    uint64_t x = (uint64_t)a.u.i, y = (uint64_t)imm;
                    a.u.i = (int64_t)(inst->op == OP_ADD_I ? x + y
                                      : inst->op == OP_SUB_I ? x - y : x * y);
                    stack[sp++] = a;
                } else {
                    Val r;
                    Val b = v_int(imm);
                    int rc = inst->op == OP_ADD_I ? val_add(vm, a, b, &r, inst->line)
                           : inst->op == OP_SUB_I ? val_sub(vm, a, b, &r, inst->line)
                                                  : val_mul(vm, a, b, &r, inst->line);
                    if (rc < 0) goto fail;
                    stack[sp++] = r;
                }
                break;
            }

            case OP_EQ:
            case OP_NEQ: {
                Val b = stack[--sp], a = stack[--sp];
                int eq = val_eq(a, b);
                stack[sp++] = v_bol(inst->op == OP_EQ ? eq : !eq);
                break;
            }
            case OP_LT:
            case OP_GT:
            case OP_LTE:
            case OP_GTE: {
                Val b = stack[--sp], a = stack[--sp];
                int r;
                if (DSC_LIKELY(a.type == VT_INT && b.type == VT_INT)) {
                    if (inst->op == OP_LT) r = a.u.i < b.u.i;
                    else if (inst->op == OP_GT) r = a.u.i > b.u.i;
                    else if (inst->op == OP_LTE) r = a.u.i <= b.u.i;
                    else r = a.u.i >= b.u.i;
                } else {
                    int tk = inst->op == OP_LT ? TK_LT
                           : inst->op == OP_GT ? TK_GT
                           : inst->op == OP_LTE ? TK_LTE : TK_GTE;
                    if (val_compare(vm, a, b, tk, &r, inst->line) < 0)
                        goto fail;
                }
                stack[sp++] = v_bol(r);
                break;
            }

            case OP_NOT: {
                Val a = stack[--sp];
                stack[sp++] = v_bol(!val_truthy(a));
                break;
            }
            case OP_NEG: {
                Val a = stack[--sp];
                Val r;
                if (val_neg(vm, a, &r, inst->line) < 0) goto fail;
                stack[sp++] = r;
                break;
            }

            case OP_JMP:
                ip = code + inst->arg;
                break;
            case OP_JF:
                if (!val_truthy(stack[--sp])) ip = code + inst->arg;
                break;
            case OP_JF_KEEP:
                if (!val_truthy(stack[sp - 1])) ip = code + inst->arg;
                break;
            case OP_JT_KEEP:
                if (val_truthy(stack[sp - 1])) ip = code + inst->arg;
                break;
            case OP_POP:
                sp--;
                break;

            case OP_CALL: {
                Func* f = &vm->funcs[inst->arg];
                int32_t argc = inst->aux;
                int32_t nb;
                if (argc != f->params) {
                    rt_error(vm, inst->line, "参数数量不匹配");
                    goto fail;
                }
                nb = sp - argc;
                if (nb + f->max_slots + f->max_stack + 4 > cap)
                    stack = stack_grow(vm, stack, &cap,
                                       nb + f->max_slots + f->max_stack + 4);
                for (i = sp; i < nb + f->max_slots; i++) stack[i] = v_nil();
                sp = nb + f->max_slots;
                if (nframes >= cframes) {
                    cframes = cframes ? cframes * 2 : 64;
                    frames = xrealloc(frames, sizeof(Frame) * (size_t)cframes);
                }
                frames[nframes].ret_ip = ip;
                frames[nframes].ret_code = code;
                frames[nframes].base = base;
                nframes++;
                base = nb;
                code = f->code.v;
                ip = f->code.v;
                break;
            }
            case OP_RET: {
                Val r = stack[--sp];
                int32_t rb = base;
                if (nframes == 0) {
                    status = 0;
                    goto done;
                }
                nframes--;
                ip = frames[nframes].ret_ip;
                code = frames[nframes].ret_code;
                base = frames[nframes].base;
                sp = rb;
                stack[sp++] = r;
                break;
            }

            case OP_ARRAY: {
                int32_t n = inst->arg;
                Arr* a = xmalloc(sizeof(Arr));
                a->len = n;
                a->cap = n;
                a->data = n ? xmalloc(sizeof(Val) * (size_t)n) : NULL;
                for (i = n - 1; i >= 0; i--) a->data[i] = stack[--sp];
                heap_add(vm, a, HK_ARR);
                stack[sp++] = v_arr(a);
                break;
            }
            case OP_GET: {
                Val iv = stack[--sp];
                Val av = stack[--sp];
                Arr* a;
                if (av.type != VT_ARR) {
                    rt_error(vm, inst->line, "下标访问要求数组，实际为 %s",
                             val_typename(av));
                    goto fail;
                }
                if (iv.type != VT_INT) {
                    rt_error(vm, inst->line, "数组下标必须是整数");
                    goto fail;
                }
                a = av.u.p;
                if (iv.u.i < 0 || iv.u.i >= a->len) {
                    rt_error(vm, inst->line, "数组下标越界: %lld (长度 %d)",
                             (long long)iv.u.i, a->len);
                    goto fail;
                }
                stack[sp++] = a->data[iv.u.i];
                break;
            }
            case OP_SET: {
                Val vv = stack[--sp];
                Val iv = stack[--sp];
                Val av = stack[--sp];
                Arr* a;
                if (av.type != VT_ARR) {
                    rt_error(vm, inst->line, "下标赋值要求数组，实际为 %s",
                             val_typename(av));
                    goto fail;
                }
                if (iv.type != VT_INT) {
                    rt_error(vm, inst->line, "数组下标必须是整数");
                    goto fail;
                }
                a = av.u.p;
                if (iv.u.i < 0 || iv.u.i >= a->len) {
                    rt_error(vm, inst->line, "数组下标越界: %lld (长度 %d)",
                             (long long)iv.u.i, a->len);
                    goto fail;
                }
                a->data[iv.u.i] = vv;
                break;
            }
            case OP_LEN: {
                Val v = stack[--sp];
                int64_t n = 0;
                if (val_length(vm, v, &n, inst->line) < 0) goto fail;
                stack[sp++] = v_int(n);
                break;
            }

            case OP_FGET: {
                Val ov = stack[--sp];
                Obj* o;
                int32_t idx;
                if (ov.type != VT_OBJ) {
                    rt_error(vm, inst->line, "字段访问要求对象，实际为 %s",
                             val_typename(ov));
                    goto fail;
                }
                o = ov.u.p;
                if (o->cls < 0 || o->cls >= vm->ncls) {
                    rt_error(vm, inst->line, "对象类型无效");
                    goto fail;
                }
                idx = field_lookup(&vm->classes[o->cls], vm->strs[inst->arg]);
                if (idx < 0) {
                    rt_error(vm, inst->line, "类型 %s 没有字段 %s",
                             vm->classes[o->cls].name, vm->strs[inst->arg]);
                    goto fail;
                }
                stack[sp++] = o->fields[idx];
                break;
            }
            case OP_FSET: {
                Val vv = stack[--sp];
                Val ov = stack[--sp];
                Obj* o;
                int32_t idx;
                if (ov.type != VT_OBJ) {
                    rt_error(vm, inst->line, "字段赋值要求对象，实际为 %s",
                             val_typename(ov));
                    goto fail;
                }
                o = ov.u.p;
                if (o->cls < 0 || o->cls >= vm->ncls) {
                    rt_error(vm, inst->line, "对象类型无效");
                    goto fail;
                }
                idx = field_lookup(&vm->classes[o->cls], vm->strs[inst->arg]);
                if (idx < 0) {
                    rt_error(vm, inst->line, "类型 %s 没有字段 %s",
                             vm->classes[o->cls].name, vm->strs[inst->arg]);
                    goto fail;
                }
                o->fields[idx] = vv;
                break;
            }
            case OP_FGET_C: {
                int32_t cls = (uint32_t)inst->arg >> 16;
                int32_t off = inst->arg & 0xFFFF;
                Val ov = stack[--sp];
                Obj* o;
                if (ov.type != VT_OBJ) {
                    rt_error(vm, inst->line, "字段访问要求对象，实际为 %s",
                             val_typename(ov));
                    goto fail;
                }
                o = ov.u.p;
                if (o->cls != cls || off >= o->nf) {
                    rt_error(vm, inst->line, "字段访问类型不匹配");
                    goto fail;
                }
                stack[sp++] = o->fields[off];
                break;
            }
            case OP_FSET_C: {
                int32_t cls = (uint32_t)inst->arg >> 16;
                int32_t off = inst->arg & 0xFFFF;
                Val vv = stack[--sp];
                Val ov = stack[--sp];
                Obj* o;
                if (ov.type != VT_OBJ) {
                    rt_error(vm, inst->line, "字段赋值要求对象，实际为 %s",
                             val_typename(ov));
                    goto fail;
                }
                o = ov.u.p;
                if (o->cls != cls || off >= o->nf) {
                    rt_error(vm, inst->line, "字段赋值类型不匹配");
                    goto fail;
                }
                o->fields[off] = vv;
                break;
            }
            case OP_NEWOBJ: {
                int32_t cls = inst->arg;
                Class* cl;
                Obj* o;
                if (cls < 0 || cls >= vm->ncls) {
                    rt_error(vm, inst->line, "类型编号无效");
                    goto fail;
                }
                cl = &vm->classes[cls];
                o = xmalloc(sizeof(Obj) + sizeof(Val) * (size_t)cl->nf);
                o->cls = cls;
                o->nf = cl->nf;
                for (i = cl->nf - 1; i >= 0; i--) o->fields[i] = stack[--sp];
                heap_add(vm, o, HK_OBJ);
                stack[sp++] = v_obj(o);
                break;
            }

            case OP_PRINT: {
                int32_t n = inst->aux;
                Val* vs;
                if (n > sp) {
                    rt_error(vm, inst->line, "内部错误: 打印栈不平衡");
                    goto fail;
                }
                vs = &stack[sp - n];
                for (i = 0; i < n; i++) {
                    if (i) out_ch(out, ' ');
                    out_val(out, vs[i], 0);
                }
                out_ch(out, '\n');
                sp -= n;
                break;
            }
            case OP_INPUT: {
                Val prompt = stack[--sp];
                char linebuf[256];
                int64_t v = 0;
                if (prompt.type == VT_STR) out_str(out, prompt.u.s ? prompt.u.s : "");
                else if (prompt.type != VT_NIL) out_val(out, prompt, 0);
                out_flush(out);
                fflush(stdout);
                if (fgets(linebuf, sizeof(linebuf), stdin)) {
                    int64_t parsed;
                    char* endp = NULL;
                    errno = 0;
                    parsed = strtoll(linebuf, &endp, 10);
                    if (endp && endp != linebuf) v = parsed;
                    if (!strchr(linebuf, '\n')) {
                        int ch;
                        while ((ch = fgetc(stdin)) != EOF && ch != '\n') { }
                    }
                }
                stack[sp++] = v_int(v);
                break;
            }
            case OP_TYPE: {
                Val v = stack[--sp];
                stack[sp++] = v_str(val_typename(v));
                break;
            }

            case OP_INC: {
                Val* p = (inst->aux & 1) ? &globals[inst->arg]
                                          : &stack[base + inst->arg];
                if (p->type == VT_INT || p->type == VT_BOL) {
                    p->type = VT_INT;
                    p->u.i = (int64_t)((uint64_t)p->u.i + 1u);
                } else if (p->type == VT_FLT) {
                    p->u.f += 1.0;
                } else {
                    Val one = v_int(1);
                    Val r;
                    if (val_add(vm, *p, one, &r, inst->line) < 0) goto fail;
                    *p = r;
                }
                break;
            }
            case OP_ADD_ST:
            case OP_SUB_ST:
            case OP_MUL_ST:
            case OP_DIV_ST:
            case OP_MOD_ST: {
                Val rhs = stack[--sp];
                Val* p = (inst->aux & 1) ? &globals[inst->arg]
                                          : &stack[base + inst->arg];
                Val r;
                int rc;
                if (p->type == VT_INT && rhs.type == VT_INT &&
                    (inst->op == OP_ADD_ST || inst->op == OP_SUB_ST ||
                     inst->op == OP_MUL_ST)) {
                    uint64_t x = (uint64_t)p->u.i, y = (uint64_t)rhs.u.i;
                    p->u.i = (int64_t)(inst->op == OP_ADD_ST ? x + y
                                     : inst->op == OP_SUB_ST ? x - y : x * y);
                    break;
                }
                if (inst->op == OP_ADD_ST) rc = val_add(vm, *p, rhs, &r, inst->line);
                else if (inst->op == OP_SUB_ST) rc = val_sub(vm, *p, rhs, &r, inst->line);
                else if (inst->op == OP_MUL_ST) rc = val_mul(vm, *p, rhs, &r, inst->line);
                else if (inst->op == OP_DIV_ST) rc = val_div(vm, *p, rhs, &r, inst->line);
                else rc = val_mod(vm, *p, rhs, &r, inst->line);
                if (rc < 0) goto fail;
                *p = r;
                break;
            }

            case OP_ADD_ST_IDX: {
                Val iv = stack[--sp];
                Val av = stack[--sp];
                Val* p = (inst->aux & 1) ? &globals[inst->arg]
                                          : &stack[base + inst->arg];
                Arr* a;
                Val elem;
                if (av.type != VT_ARR) {
                    rt_error(vm, inst->line, "下标访问要求数组，实际为 %s",
                             val_typename(av));
                    goto fail;
                }
                if (iv.type != VT_INT) {
                    rt_error(vm, inst->line, "数组下标必须是整数");
                    goto fail;
                }
                a = av.u.p;
                if (iv.u.i < 0 || iv.u.i >= a->len) {
                    rt_error(vm, inst->line, "数组下标越界: %lld (长度 %d)",
                             (long long)iv.u.i, a->len);
                    goto fail;
                }
                elem = a->data[iv.u.i];
                if (p->type == VT_INT && elem.type == VT_INT) {
                    p->u.i = (int64_t)((uint64_t)p->u.i + (uint64_t)elem.u.i);
                } else {
                    Val r;
                    if (val_add(vm, *p, elem, &r, inst->line) < 0) goto fail;
                    *p = r;
                }
                break;
            }

            case OP_FOP_C: {
                int32_t cls = (uint32_t)inst->arg >> 16;
                int32_t off = inst->arg & 0xFFFF;
                Val rhs = stack[--sp];
                Val ov = stack[--sp];
                Obj* o;
                Val* p;
                Val r;
                int rc;
                if (ov.type != VT_OBJ) {
                    rt_error(vm, inst->line, "字段赋值要求对象，实际为 %s",
                             val_typename(ov));
                    goto fail;
                }
                o = ov.u.p;
                if (o->cls != cls || off >= o->nf) {
                    rt_error(vm, inst->line, "字段赋值类型不匹配");
                    goto fail;
                }
                p = &o->fields[off];
                if (p->type == VT_INT && rhs.type == VT_INT &&
                    (inst->aux == 0 || inst->aux == 1 || inst->aux == 2)) {
                    uint64_t x = (uint64_t)p->u.i, y = (uint64_t)rhs.u.i;
                    p->u.i = (int64_t)(inst->aux == 0 ? x + y
                                     : inst->aux == 1 ? x - y : x * y);
                    break;
                }
                if (inst->aux == 0) rc = val_add(vm, *p, rhs, &r, inst->line);
                else if (inst->aux == 1) rc = val_sub(vm, *p, rhs, &r, inst->line);
                else if (inst->aux == 2) rc = val_mul(vm, *p, rhs, &r, inst->line);
                else if (inst->aux == 3) rc = val_div(vm, *p, rhs, &r, inst->line);
                else rc = val_mod(vm, *p, rhs, &r, inst->line);
                if (rc < 0) goto fail;
                *p = r;
                break;
            }

            case OP_LT_JF:
            case OP_LE_JF:
            case OP_GT_JF:
            case OP_GE_JF: {
                Val b = stack[--sp], a = stack[--sp];
                int r;
                if (DSC_LIKELY(a.type == VT_INT && b.type == VT_INT)) {
                    if (inst->op == OP_LT_JF) r = a.u.i < b.u.i;
                    else if (inst->op == OP_LE_JF) r = a.u.i <= b.u.i;
                    else if (inst->op == OP_GT_JF) r = a.u.i > b.u.i;
                    else r = a.u.i >= b.u.i;
                } else {
                    int tk = inst->op == OP_LT_JF ? TK_LT
                           : inst->op == OP_LE_JF ? TK_LTE
                           : inst->op == OP_GT_JF ? TK_GT : TK_GTE;
                    if (val_compare(vm, a, b, tk, &r, inst->line) < 0)
                        goto fail;
                }
                if (!r) ip = code + inst->arg;
                break;
            }
            case OP_EQ_JF:
            case OP_NEQ_JF: {
                Val b = stack[--sp], a = stack[--sp];
                int eq = val_eq(a, b);
                if (inst->op == OP_EQ_JF ? !eq : eq) ip = code + inst->arg;
                break;
            }

            default:
                rt_error(vm, inst->line, "未知操作码 %d", inst->op);
                goto fail;
        }
    }

fail:
    status = -1;
done:
    out_flush(out);
    if (status == 0 && out->error) {
        rt_error(vm, 0, "输出写入失败");
        status = -1;
    }
    vm->stack = stack;
    vm->cap = cap;
    vm->frames = frames;
    vm->cframes = cframes;
    return status;

#undef BIN_NUM
}

/* ================================================================
 *  Section 10: Driver
 * ================================================================ */

static char* read_file(const char* path, size_t* out_len, char* msg,
                       size_t msg_cap) {
    FILE* f = fopen(path, "rb");
    long sz;
    char* buf;
    size_t rd;
    if (!f) {
        snprintf(msg, msg_cap, "无法打开文件 %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(msg, msg_cap, "无法定位文件末尾 %s", path);
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        snprintf(msg, msg_cap, "无法获取文件大小 %s", path);
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        snprintf(msg, msg_cap, "无法回到文件开头 %s", path);
        fclose(f);
        return NULL;
    }
    buf = xmalloc((size_t)sz + 1);
    rd = fread(buf, 1, (size_t)sz, f);
    if (rd != (size_t)sz) {
        snprintf(msg, msg_cap, "读取文件失败 %s", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = 0;
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static void comp_free(Comp* c) {
    int32_t i;
    code_free(&c->main_code);
    for (i = 0; i < c->nfunc; i++) code_free(&c->funcs[i].code);
    free(c->funcs);
    free(c->classes);
    free(c->strs);
    free(c->k64);
    c->funcs = NULL;
    c->classes = NULL;
    c->strs = NULL;
    c->k64 = NULL;
    pmap_free(&c->globals);
    pmap_free(&c->func_map);
    pmap_free(&c->class_map);
    pmap_free(&c->gcls);
}

static void print_err(const DscErr* e) {
    if (e->line > 0)
        fprintf(stderr, "错误 [%d:%d]: %s\n", e->line, e->col, e->msg);
    else
        fprintf(stderr, "错误: %s\n", e->msg);
}

static void usage(FILE* f) {
    fputs("DSC v3.0 - 字节码解释器\n"
          "用法: dsc <源文件.dscbt>\n"
          "选项:\n"
          "  -h, --help     显示帮助\n"
          "  -v, --version  显示版本\n", f);
}

int main(int argc, char** argv) {
    const char* path = NULL;
    char msg[512];
    char* src;
    size_t src_len = 0;
    Arena arena;
    Interner interner;
    TokVec tv;
    DscErr err;
    Parser parser;
    Node* prog;
    Comp comp;
    Vm vm;
    int status;

    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
        fputs("dsc 3.0\n", stdout);
        return 0;
    }
    if (argv[1][0] == '-') {
        fprintf(stderr, "错误: 未知选项 %s\n", argv[1]);
        usage(stderr);
        return 1;
    }
    path = argv[1];

    keywords_init();
    chclass_init();

    src = read_file(path, &src_len, msg, sizeof(msg));
    if (!src) {
        fprintf(stderr, "错误: %s\n", msg);
        return 1;
    }

    memset(&arena, 0, sizeof(arena));
    memset(&tv, 0, sizeof(tv));
    memset(&err, 0, sizeof(err));
    interner_init(&interner);

    if (lex_all((const unsigned char*)src, src_len, &arena, &interner, &tv,
                &err) < 0) {
        print_err(&err);
        free(src);
        free(tv.items);
        interner_free(&interner);
        arena_free_all(&arena);
        return 2;
    }
    free(src);
    src = NULL;

    memset(&parser, 0, sizeof(parser));
    parser.toks = tv.items;
    parser.count = tv.count;
    parser.ar = &arena;
    parser.in = &interner;
    parser.err = &err;
    prog = parse_program(&parser);
    free(tv.items);
    tv.items = NULL;
    if (err_has(&err) || !prog) {
        print_err(&err);
        interner_free(&interner);
        arena_free_all(&arena);
        return 2;
    }

    memset(&comp, 0, sizeof(comp));
    comp.ar = &arena;
    comp.in = &interner;
    comp.err = &err;
    pmap_init(&comp.globals);
    pmap_init(&comp.func_map);
    pmap_init(&comp.class_map);
    pmap_init(&comp.gcls);
    comp_program(&comp, prog);
    if (err_has(&err)) {
        print_err(&err);
        comp_free(&comp);
        interner_free(&interner);
        arena_free_all(&arena);
        return 2;
    }

    memset(&vm, 0, sizeof(vm));
    vm.ar = &arena;
    vm.strs = comp.strs;
    vm.nstr = comp.nstr;
    vm.funcs = comp.funcs;
    vm.nfunc = comp.nfunc;
    vm.classes = comp.classes;
    vm.ncls = comp.ncls;
    vm.k64 = comp.k64;
    vm.nk64 = comp.nk64;
    vm.nglobals = (int32_t)comp.globals.count;
    vm.err = &err;
    vm.globals = xmalloc(sizeof(Val) * (size_t)(vm.nglobals ? vm.nglobals : 1));
    {
        int32_t i;
        for (i = 0; i < vm.nglobals; i++) vm.globals[i] = v_nil();
    }
    vm.stack = xmalloc(sizeof(Val) * 4096u);
    vm.cap = 4096;
    vm.frames = NULL;
    vm.cframes = 0;
    vm.out.f = stdout;
    vm.out.buf = xmalloc(65536u);
    vm.out.len = 0;
    vm.out.error = 0;

    status = vm_run(&vm, comp.main_code.v, comp.main_slots,
                    comp.main_max_stack);

    heap_free_all(&vm.heap);
    free(vm.stack);
    free(vm.globals);
    free(vm.frames);
    free(vm.out.buf);
    comp_free(&comp);
    interner_free(&interner);
    arena_free_all(&arena);

    if (status != 0) {
        print_err(&err);
        return 3;
    }
    return 0;
}

