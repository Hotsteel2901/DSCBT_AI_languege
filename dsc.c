/* DSC Language - Bytecode Interpreter v2.0
 * A Python-like scripting language with built-in bytecode execution.
 * No external C compiler needed at runtime.
 * Usage: dsc <source.dscbt>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

/* ================================================================
 *  Section 1: Utilities
 * ================================================================ */

static void dsc_error(int line, int col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "\xe9\x94\x99\xe8\xaf\xaf [%d:%d]: ", line, col);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

/* --- Dynamic String --- */
typedef struct { char* s; int len; int cap; } SBuf;

static SBuf* sb_new(void) {
    SBuf* b = malloc(sizeof(SBuf));
    b->cap = 256; b->s = malloc(b->cap); b->s[0] = 0; b->len = 0;
    return b;
}

static void sb_push(SBuf* b, const char* str) {
    int n = (int)strlen(str);
    while (b->len + n + 1 > b->cap) { b->cap *= 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->len, str, n); b->len += n; b->s[b->len] = 0;
}

static void sb_ch(SBuf* b, char c) {
    while (b->len + 2 > b->cap) { b->cap *= 2; b->s = realloc(b->s, b->cap); }
    b->s[b->len++] = c; b->s[b->len] = 0;
}

static void sb_int(SBuf* b, int64_t v) { char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v); sb_push(b, buf); }
static void sb_flt(SBuf* b, double v) { char buf[64]; snprintf(buf, sizeof(buf), "%.16g", v); sb_push(b, buf); }
static void sb_free(SBuf* b) { free(b->s); free(b); }

/* ================================================================
 *  Section 2: Lexer
 * ================================================================ */

enum {
    TK_EOF=0, TK_NL, TK_INDENT, TK_DEDENT,
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
};

static const char* tk_name(int t) {
    static const char* names[] = {
        "EOF","NL","INDENT","DEDENT","ID","INT","FLT","STR",
        "+","-","*","/","%","=","==","!=","<",">","<=",">=",
        "(",")","[","]","{","}",":",",",".",
        "True","False","if","els","while","for","in",
        "def","return","class","and","or","not",
        "int","flt","str","bol","void","range",
    };
    return names[t];
}

typedef struct { int type; char* text; int line, col; } Tok;

static Tok* tok_new(int t, const char* s, int ln, int co) {
    Tok* tk = malloc(sizeof(Tok));
    tk->type = t; tk->text = strdup(s ? s : ""); tk->line = ln; tk->col = co;
    return tk;
}

static void tok_free(Tok* tk) { free(tk->text); free(tk); }

typedef struct { const char* src; int pos, len, line, col; } Lxr;

static int lookup_kw(const char* w) {
    if (!strcmp(w,"True")) return KW_TRUE;
    if (!strcmp(w,"False")) return KW_FALSE;
    if (!strcmp(w,"if")) return KW_IF;
    if (!strcmp(w,"els")) return KW_ELS;
    if (!strcmp(w,"while")) return KW_WHILE;
    if (!strcmp(w,"for")) return KW_FOR;
    if (!strcmp(w,"in")) return KW_IN;
    if (!strcmp(w,"def")) return KW_DEF;
    if (!strcmp(w,"return")) return KW_RETURN;
    if (!strcmp(w,"class")) return KW_CLASS;
    if (!strcmp(w,"and")) return KW_AND;
    if (!strcmp(w,"or")) return KW_OR;
    if (!strcmp(w,"not")) return KW_NOT;
    if (!strcmp(w,"int")) return KW_INT;
    if (!strcmp(w,"flt")) return KW_FLT;
    if (!strcmp(w,"str")) return KW_STR;
    if (!strcmp(w,"bol")) return KW_BOL;
    if (!strcmp(w,"void")) return KW_VOID;
    if (!strcmp(w,"range")) return KW_RANGE;
    return TK_ID;
}

static Tok* lex_one(Lxr* lx) {
    while (lx->pos < lx->len && lx->src[lx->pos] == ' ') { lx->pos++; lx->col++; }
    if (lx->pos >= lx->len) return tok_new(TK_EOF, "", lx->line, lx->col);
    char c = lx->src[lx->pos];

    if (c == '\n' || c == '\r') {
        if (c == '\r') { lx->pos++; if (lx->pos < lx->len && lx->src[lx->pos] == '\n') lx->pos++; }
        else lx->pos++;
        int co = lx->col; lx->col = 1; lx->line++;
        return tok_new(TK_NL, "\n", lx->line - 1, co);
    }

    if (c == '"') {
        lx->pos++; lx->col++;
        SBuf* sb = sb_new();
        while (lx->pos < lx->len && lx->src[lx->pos] != '"' && lx->src[lx->pos] != '\n') {
            if (lx->src[lx->pos] == '\\' && lx->pos + 1 < lx->len) {
                lx->pos++; lx->col++;
                switch (lx->src[lx->pos]) {
                    case 'n': sb_ch(sb, '\n'); break; case 't': sb_ch(sb, '\t'); break;
                    case 'r': sb_ch(sb, '\r'); break; case '\\': sb_ch(sb, '\\'); break;
                    case '"': sb_ch(sb, '"'); break; default: sb_ch(sb, lx->src[lx->pos]); break;
                }
            } else { sb_ch(sb, lx->src[lx->pos]); }
            lx->pos++; lx->col++;
        }
        if (lx->pos >= lx->len || lx->src[lx->pos] == '\n')
            dsc_error(lx->line, lx->col, "unterminated string");
        lx->pos++; lx->col++;
        Tok* tk = tok_new(TK_STR, sb->s, lx->line, lx->col);
        sb_free(sb);
        return tk;
    }

    if (isdigit(c)) {
        int fl = 0, ln = lx->line, co = lx->col;
        SBuf* sb = sb_new();
        while (lx->pos < lx->len && isdigit(lx->src[lx->pos])) { sb_ch(sb, lx->src[lx->pos]); lx->pos++; lx->col++; }
        if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '.' && isdigit(lx->src[lx->pos + 1])) {
            fl = 1; sb_ch(sb, '.'); lx->pos++; lx->col++;
            while (lx->pos < lx->len && isdigit(lx->src[lx->pos])) { sb_ch(sb, lx->src[lx->pos]); lx->pos++; lx->col++; }
        }
        Tok* tk = tok_new(fl ? TK_FLT : TK_INT, sb->s, ln, co);
        sb_free(sb);
        return tk;
    }

    if (isalpha(c) || c == '_') {
        int ln = lx->line, co = lx->col;
        SBuf* sb = sb_new();
        while (lx->pos < lx->len && (isalnum(lx->src[lx->pos]) || lx->src[lx->pos] == '_')) {
            sb_ch(sb, lx->src[lx->pos]); lx->pos++; lx->col++;
        }
        int t = lookup_kw(sb->s);
        Tok* tk = tok_new(t, sb->s, ln, co);
        sb_free(sb);
        return tk;
    }

    /* Comments: // or # */
    if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
        while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
        return lex_one(lx);
    }
    if (c == '#') {
        while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
        return lex_one(lx);
    }

    int ln = lx->line, co = lx->col; lx->pos++; lx->col++;
    switch (c) {
        case '+': return tok_new(TK_PLUS, "+", ln, co);
        case '-': return tok_new(TK_MINUS, "-", ln, co);
        case '*': return tok_new(TK_STAR, "*", ln, co);
        case '/': return tok_new(TK_SLASH, "/", ln, co);
        case '%': return tok_new(TK_PCT, "%", ln, co);
        case '(': return tok_new(TK_LP, "(", ln, co);
        case ')': return tok_new(TK_RP, ")", ln, co);
        case '[': return tok_new(TK_LB, "[", ln, co);
        case ']': return tok_new(TK_RB, "]", ln, co);
        case '{': return tok_new(TK_LC, "{", ln, co);
        case '}': return tok_new(TK_RC, "}", ln, co);
        case ':': return tok_new(TK_COLON, ":", ln, co);
        case ',': return tok_new(TK_COMMA, ",", ln, co);
        case '.': return tok_new(TK_DOT, ".", ln, co);
        case '&': return tok_new(KW_AND, "&", ln, co);
        case '|': return tok_new(KW_OR, "|", ln, co);
        case '!':
            if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tok_new(TK_NEQ, "!=", ln, co); }
            return tok_new(KW_NOT, "!", ln, co);
        case '=':
            if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tok_new(TK_EQEQ, "==", ln, co); }
            return tok_new(TK_EQ, "=", ln, co);
        case '<':
            if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tok_new(TK_LTE, "<=", ln, co); }
            return tok_new(TK_LT, "<", ln, co);
        case '>':
            if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tok_new(TK_GTE, ">=", ln, co); }
            return tok_new(TK_GT, ">", ln, co);
        default:
            dsc_error(ln, co, "unexpected character '%c'", c);
    }
    return NULL;
}

static Tok** tokenize(const char* src, int* out_count) {
    int cap = 512, n = 0;
    Tok** toks = malloc(sizeof(Tok*) * cap);
    Lxr lx = {src, 0, (int)strlen(src), 1, 1};
    int istk[64], id = 1; istk[0] = 0;
    int at_bol = 1;

    while (1) {
        if (at_bol) {
            int indent = 0;
            while (lx.pos < lx.len && (lx.src[lx.pos] == ' ' || lx.src[lx.pos] == '\t')) {
                indent += (lx.src[lx.pos] == '\t') ? 4 : 1;
                lx.pos++; lx.col++;
            }
            if (lx.pos >= lx.len) break;
            if (lx.src[lx.pos] == '\n' || lx.src[lx.pos] == '\r') {
                if (lx.src[lx.pos] == '\r') lx.pos++;
                if (lx.pos < lx.len && lx.src[lx.pos] == '\n') lx.pos++;
                lx.line++; lx.col = 1;
                continue;
            }
            /* Comment-only line */
            if (lx.src[lx.pos] == '#' ||
                (lx.src[lx.pos] == '/' && lx.pos + 1 < lx.len && lx.src[lx.pos + 1] == '/')) {
                Tok* t = lex_one(&lx); tok_free(t);
                continue;
            }
            int cur = istk[id - 1];
            if (indent > cur) {
                if (n >= cap) { cap *= 2; toks = realloc(toks, sizeof(Tok*) * cap); }
                toks[n++] = tok_new(TK_INDENT, "", lx.line, lx.col);
                if (id < 64) istk[id++] = indent;
                cur = indent;
            }
            while (indent < cur && id > 1) {
                if (n >= cap) { cap *= 2; toks = realloc(toks, sizeof(Tok*) * cap); }
                toks[n++] = tok_new(TK_DEDENT, "", lx.line, lx.col);
                id--; cur = istk[id - 1];
            }
            if (indent != cur) dsc_error(lx.line, lx.col, "inconsistent indentation");
            at_bol = 0;
        }

        Tok* t = lex_one(&lx);
        if (t->type == TK_EOF) { tok_free(t); break; }
        if (t->type == TK_NL) at_bol = 1;
        if (n >= cap) { cap *= 2; toks = realloc(toks, sizeof(Tok*) * cap); }
        toks[n++] = t;
    }

    while (id > 1) {
        if (n >= cap) { cap *= 2; toks = realloc(toks, sizeof(Tok*) * cap); }
        toks[n++] = tok_new(TK_DEDENT, "", lx.line, lx.col);
        id--;
    }
    if (n >= cap) { cap *= 2; toks = realloc(toks, sizeof(Tok*) * cap); }
    toks[n++] = tok_new(TK_EOF, "", lx.line, lx.col);
    *out_count = n;
    return toks;
}

/* ================================================================
 *  Section 3: AST
 * ================================================================ */

enum {
    ND_PROG, ND_VAR, ND_ASSIGN, ND_IF, ND_WHILE, ND_FOR, ND_FN, ND_RET, ND_BLOCK,
    ND_PRINT, ND_BINOP, ND_UNOP, ND_ID, ND_INT, ND_FLT, ND_STR, ND_BOOL,
    ND_CALL, ND_ARR, ND_IDX, ND_FLD, ND_CLASS, ND_CLASS_LIT,
};

enum { TY_NONE = 0, TY_INT, TY_FLT, TY_STR, TY_BOL, TY_VOID, TY_CLASS };

typedef struct Node {
    int kind, ty;
    char* name;
    int64_t ival;
    double fval;
    struct Node *a, *b, *c, *d, *next;
    int line, col;
} Node;

static Node* nd_new(int kind, int ln, int co) {
    Node* n = calloc(1, sizeof(Node)); n->kind = kind; n->ty = TY_NONE; n->line = ln; n->col = co; return n;
}
static Node* nd_id(const char* n, int ln, int co) { Node* x = nd_new(ND_ID, ln, co); x->name = strdup(n); return x; }
static Node* nd_int_v(int64_t v, int ln, int co) { Node* x = nd_new(ND_INT, ln, co); x->ival = v; x->ty = TY_INT; return x; }
static Node* nd_flt_v(double v, int ln, int co) { Node* x = nd_new(ND_FLT, ln, co); x->fval = v; x->ty = TY_FLT; return x; }
static Node* nd_str_v(const char* v, int ln, int co) { Node* x = nd_new(ND_STR, ln, co); x->name = strdup(v); x->ty = TY_STR; return x; }
static Node* nd_bool_v(int v, int ln, int co) { Node* x = nd_new(ND_BOOL, ln, co); x->ival = v; x->ty = TY_BOL; return x; }
static Node* nd_binop(int op, Node* l, Node* r, int ln, int co) { Node* x = nd_new(ND_BINOP, ln, co); x->ival = op; x->a = l; x->b = r; return x; }
static Node* nd_unop(int op, Node* ch, int ln, int co) { Node* x = nd_new(ND_UNOP, ln, co); x->ival = op; x->a = ch; return x; }

/* ================================================================
 *  Section 4: Symbol Table
 * ================================================================ */

typedef struct { char* name; int ty; int is_fn; } Sym;
typedef struct Scope { Sym* syms; int count, cap; struct Scope* parent; } Scope;

static Scope* sc_new(Scope* par) {
    Scope* s = calloc(1, sizeof(Scope));
    s->cap = 32; s->syms = malloc(sizeof(Sym) * 32); s->parent = par;
    return s;
}

static Sym* sc_find(Scope* s, const char* name) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->syms[i].name, name)) return &s->syms[i];
    return s->parent ? sc_find(s->parent, name) : NULL;
}

static void sc_add(Scope* s, const char* name, int ty, int is_fn) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->syms[i].name, name)) { s->syms[i].ty = ty; s->syms[i].is_fn = is_fn; return; }
    if (s->count >= s->cap) { s->cap *= 2; s->syms = realloc(s->syms, sizeof(Sym) * s->cap); }
    s->syms[s->count].name = strdup(name); s->syms[s->count].ty = ty; s->syms[s->count].is_fn = is_fn;
    s->count++;
}

static void sc_free(Scope* s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) free(s->syms[i].name);
    free(s->syms);
    sc_free(s->parent);
    free(s);
}

/* ================================================================
 *  Section 5: Parser
 * ================================================================ */

typedef struct { Tok** toks; int pos, count; Scope* global; Scope* local; } Parser;

#define peek(p) ((p)->pos < (p)->count ? (p)->toks[(p)->pos] : NULL)
#define nxt_tok(p) ((p)->pos + 1 < (p)->count ? (p)->toks[(p)->pos + 1] : NULL)
#define consume(p) ((p)->pos < (p)->count ? (p)->toks[(p)->pos++] : NULL)
#define is_(p, t) (peek(p) && peek(p)->type == (t))
#define is_typ_kw(p) (peek(p) && (peek(p)->type == KW_INT || peek(p)->type == KW_FLT || peek(p)->type == KW_STR || peek(p)->type == KW_BOL))

static Tok* expect_tok(Parser* p, int t) {
    if (is_(p, t)) return consume(p);
    Tok* tk = peek(p);
    dsc_error(tk ? tk->line : 0, tk ? tk->col : 0, "expected %s, got %s", tk_name(t), tk ? tk_name(tk->type) : "EOF");
    return NULL;
}

static int typ_kw_to_int(int kw) {
    switch (kw) { case KW_INT: return TY_INT; case KW_FLT: return TY_FLT; case KW_STR: return TY_STR; case KW_BOL: return TY_BOL; default: return TY_INT; }
}

/* Forward declarations for recursive descent */
static Node* parse_expr(Parser* p);
static Node* parse_stmt(Parser* p);
static Node* parse_block(Parser* p);

/* --- Primary --- */
static Node* parse_unary(Parser* p);

static Node* parse_primary(Parser* p) {
    Tok* tk = peek(p);
    if (!tk) dsc_error(0, 0, "unexpected end of input");

    if (tk->type == TK_INT) { consume(p); return nd_int_v(strtoll(tk->text, NULL, 10), tk->line, tk->col); }
    if (tk->type == TK_FLT) { consume(p); return nd_flt_v(strtod(tk->text, NULL), tk->line, tk->col); }
    if (tk->type == TK_STR) { consume(p); return nd_str_v(tk->text, tk->line, tk->col); }
    if (tk->type == KW_TRUE) { consume(p); return nd_bool_v(1, tk->line, tk->col); }
    if (tk->type == KW_FALSE) { consume(p); return nd_bool_v(0, tk->line, tk->col); }

    if (tk->type == TK_LP) {
        consume(p);
        Node* e = parse_expr(p);
        expect_tok(p, TK_RP);
        return e;
    }

    if (tk->type == TK_LB) {
        consume(p);
        Node* arr = nd_new(ND_ARR, tk->line, tk->col);
        Node** tail = &arr->a;
        if (!is_(p, TK_RB)) {
            do { *tail = parse_expr(p); tail = &(*tail)->next; } while (is_(p, TK_COMMA) && (consume(p), 1));
        }
        expect_tok(p, TK_RB);
        return arr;
    }

    if (tk->type == TK_ID) {
        char* nm = strdup(tk->text);
        int ln = tk->line, co = tk->col;
        consume(p);

        /* Function call: name( */
        if (is_(p, TK_LP)) {
            consume(p);
            Node* call = nd_new(ND_CALL, ln, co);
            call->name = nm;
            if (!is_(p, TK_RP)) {
                Node** tail = &call->a;
                do { *tail = parse_expr(p); tail = &(*tail)->next; } while (is_(p, TK_COMMA) && (consume(p), 1));
            }
            expect_tok(p, TK_RP);
            return call;
        }

        /* Class literal: Name{ field: expr, ... } */
        if (is_(p, TK_LC)) {
            consume(p);
            Node* cl = nd_new(ND_CLASS_LIT, ln, co);
            cl->name = nm;
            Node** tail = &cl->a;
            if (!is_(p, TK_RC)) {
                do {
                    Tok* fn = expect_tok(p, TK_ID);
                    expect_tok(p, TK_COLON);
                    Node* fld = nd_new(ND_FLD, fn->line, fn->col);
                    fld->name = strdup(fn->text);
                    fld->a = parse_expr(p);
                    *tail = fld; tail = &fld->next;
                } while (is_(p, TK_COMMA) && (consume(p), 1));
            }
            expect_tok(p, TK_RC);
            cl->ty = TY_CLASS;
            return cl;
        }

        /* Plain identifier — lookup deferred to compiler */
        Node* id = nd_id(nm, ln, co);
        Sym* s = sc_find(p->local, nm);
        if (!s) s = sc_find(p->global, nm);
        if (s) id->ty = s->ty;
        /* If not found, ty stays TY_NONE — compiler will resolve it */
        free(nm);
        return id;
    }

    dsc_error(tk->line, tk->col, "unexpected token '%s'", tk_name(tk->type));
    return NULL;
}

/* --- Unary (not, -) --- */
static Node* parse_unary(Parser* p) {
    Tok* tk = peek(p);
    if (!tk) dsc_error(0, 0, "unexpected EOF");

    if (tk->type == KW_NOT) {
        consume(p);
        Node* x = parse_unary(p);
        Node* n = nd_unop(KW_NOT, x, tk->line, tk->col);
        n->ty = TY_BOL;
        return n;
    }

    if (tk->type == TK_MINUS) {
        consume(p);
        Node* x = parse_unary(p);
        Node* n = nd_unop(TK_MINUS, x, tk->line, tk->col);
        n->ty = x->ty;
        return n;
    }

    return parse_primary(p);
}

/* --- Postfix (index, field) --- */
static Node* parse_postfix(Parser* p) {
    Node* e = parse_unary(p);
    while (1) {
        if (is_(p, TK_LB)) {
            consume(p);
            Node* idx = parse_expr(p);
            expect_tok(p, TK_RB);
            Node* n = nd_new(ND_IDX, e->line, e->col);
            n->a = e; n->b = idx;
            e = n;
        } else if (is_(p, TK_DOT)) {
            consume(p);
            Tok* fn = expect_tok(p, TK_ID);
            Node* n = nd_new(ND_FLD, e->line, e->col);
            n->name = strdup(fn->text); n->a = e;
            e = n;
        } else break;
    }
    return e;
}

/* --- Precedence climbing --- */
static Node* parse_mul(Parser* p) {
    Node* e = parse_postfix(p);
    while (is_(p, TK_STAR) || is_(p, TK_SLASH) || is_(p, TK_PCT)) {
        int op = consume(p)->type;
        Node* r = parse_postfix(p);
        e = nd_binop(op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_add(Parser* p) {
    Node* e = parse_mul(p);
    while (is_(p, TK_PLUS) || is_(p, TK_MINUS)) {
        int op = consume(p)->type;
        Node* r = parse_mul(p);
        e = nd_binop(op, e, r, e->line, e->col);
    }
    return e;
}

static Node* parse_cmp(Parser* p) {
    Node* e = parse_add(p);
    while (is_(p, TK_LT) || is_(p, TK_GT) || is_(p, TK_LTE) || is_(p, TK_GTE)) {
        int op = consume(p)->type;
        Node* r = parse_add(p);
        Node* n = nd_binop(op, e, r, e->line, e->col);
        n->ty = TY_BOL;
        e = n;
    }
    return e;
}

static Node* parse_eq2(Parser* p) {
    Node* e = parse_cmp(p);
    while (is_(p, TK_EQEQ) || is_(p, TK_NEQ)) {
        int op = consume(p)->type;
        Node* r = parse_cmp(p);
        Node* n = nd_binop(op, e, r, e->line, e->col);
        n->ty = TY_BOL;
        e = n;
    }
    return e;
}

static Node* parse_and2(Parser* p) {
    Node* e = parse_eq2(p);
    while (is_(p, KW_AND)) {
        int op = consume(p)->type;
        Node* r = parse_eq2(p);
        Node* n = nd_binop(op, e, r, e->line, e->col);
        n->ty = TY_BOL;
        e = n;
    }
    return e;
}

static Node* parse_or2(Parser* p) {
    Node* e = parse_and2(p);
    while (is_(p, KW_OR)) {
        int op = consume(p)->type;
        Node* r = parse_and2(p);
        Node* n = nd_binop(op, e, r, e->line, e->col);
        n->ty = TY_BOL;
        e = n;
    }
    return e;
}

static Node* parse_expr(Parser* p) {
    return parse_or2(p);
}

/* --- Statements --- */

static Node* parse_assign(Parser* p, Node* lv) {
    Node* n = nd_new(ND_ASSIGN, lv->line, lv->col);
    n->a = lv; n->b = parse_expr(p);
    return n;
}

static Node* parse_if(Parser* p) {
    Tok* tk = consume(p);
    Node* cond = parse_expr(p);
    Node* n = nd_new(ND_IF, tk->line, tk->col);
    n->a = cond; n->b = parse_block(p);
    Node** tail = &n->c;
    while (is_(p, KW_ELS)) {
        Tok* ek = consume(p);
        if (is_(p, KW_IF)) {
            consume(p);
            Node* eif = nd_new(ND_IF, ek->line, ek->col);
            eif->a = parse_expr(p);
            eif->b = parse_block(p);
            *tail = eif; tail = &eif->c;
        } else {
            *tail = parse_block(p);
            break;
        }
    }
    return n;
}

static Node* parse_while(Parser* p) {
    Tok* tk = consume(p);
    Node* cond = parse_expr(p);
    Node* n = nd_new(ND_WHILE, tk->line, tk->col);
    n->a = cond; n->b = parse_block(p);
    return n;
}

static Node* parse_for(Parser* p) {
    Tok* tk = consume(p);
    Tok* var = expect_tok(p, TK_ID);
    expect_tok(p, KW_IN);
    expect_tok(p, KW_RANGE);
    expect_tok(p, TK_LP);
    Node* limit = parse_expr(p);
    expect_tok(p, TK_RP);
    Node* n = nd_new(ND_FOR, tk->line, tk->col);
    n->name = strdup(var->text);
    n->a = limit;
    n->b = parse_block(p);
    return n;
}

static Node* parse_def(Parser* p) {
    Tok* tk = consume(p);
    Tok* nm = expect_tok(p, TK_ID);
    Node* fn = nd_new(ND_FN, tk->line, tk->col);
    fn->name = strdup(nm->text);
    Node** tail = &fn->a;
    expect_tok(p, TK_LP);
    if (!is_(p, TK_RP)) {
        do {
            Tok* pn = expect_tok(p, TK_ID);
            Node* pa = nd_new(ND_VAR, pn->line, pn->col);
            pa->name = strdup(pn->text);
            pa->ty = TY_INT;
            if (is_(p, TK_COLON)) {
                consume(p);
                if (is_typ_kw(p)) pa->ty = typ_kw_to_int(consume(p)->type);
            }
            sc_add(p->local, pn->text, pa->ty, 0);
            *tail = pa; tail = &pa->next;
        } while (is_(p, TK_COMMA) && (consume(p), 1));
    }
    expect_tok(p, TK_RP);

    if (is_(p, TK_COLON) && nxt_tok(p) && (nxt_tok(p)->type == KW_INT || nxt_tok(p)->type == KW_FLT || nxt_tok(p)->type == KW_STR || nxt_tok(p)->type == KW_BOL || nxt_tok(p)->type == KW_VOID)) {
        consume(p);
        fn->ty = typ_kw_to_int(consume(p)->type);
    } else {
        fn->ty = TY_INT;
    }
    sc_add(p->global, nm->text, fn->ty, 1);
    fn->b = parse_block(p);
    return fn;
}

static Node* parse_ret(Parser* p) {
    Tok* tk = consume(p);
    Node* n = nd_new(ND_RET, tk->line, tk->col);
    if (!is_(p, TK_NL) && !is_(p, TK_DEDENT) && !is_(p, TK_EOF))
        n->a = parse_expr(p);
    return n;
}

static Node* parse_class(Parser* p) {
    Tok* tk = consume(p);
    Tok* nm = expect_tok(p, TK_ID);
    Node* cl = nd_new(ND_CLASS, tk->line, tk->col);
    cl->name = strdup(nm->text);
    sc_add(p->global, nm->text, TY_CLASS, 0);
    expect_tok(p, TK_COLON);
    expect_tok(p, TK_NL);
    expect_tok(p, TK_INDENT);
    Node** tail = &cl->a;
    while (!is_(p, TK_DEDENT) && !is_(p, TK_EOF)) {
        Tok* fn = expect_tok(p, TK_ID);
        expect_tok(p, TK_COLON);
        Node* fld = nd_new(ND_VAR, fn->line, fn->col);
        fld->name = strdup(fn->text);
        if (is_typ_kw(p)) fld->ty = typ_kw_to_int(consume(p)->type);
        else if (is_(p, TK_ID)) { fld->ty = TY_CLASS; fld->b = nd_id(consume(p)->text, 0, 0); }
        else fld->ty = TY_INT;
        *tail = fld; tail = &fld->next;
        if (is_(p, TK_NL)) consume(p);
        else if (!is_(p, TK_DEDENT)) dsc_error(fn->line, fn->col, "expected newline in class body");
    }
    expect_tok(p, TK_DEDENT);
    return cl;
}

static Node* parse_block(Parser* p) {
    if (!is_(p, TK_COLON)) {
        Tok* tk = peek(p);
        dsc_error(tk ? tk->line : 0, tk ? tk->col : 0, "expected ':' before block");
    }
    consume(p);
    Node* blk = nd_new(ND_BLOCK, peek(p) ? peek(p)->line : 0, peek(p) ? peek(p)->col : 0);
    Node** tail = &blk->a;

    if (is_(p, TK_NL)) {
        consume(p);
        expect_tok(p, TK_INDENT);
        while (!is_(p, TK_DEDENT) && !is_(p, TK_EOF)) {
            Node* st = parse_stmt(p);
            if (st) { *tail = st; while (*tail && (*tail)->next) tail = &(*tail)->next; if (*tail) tail = &(*tail)->next; }
            if (is_(p, TK_NL)) consume(p);
        }
        expect_tok(p, TK_DEDENT);
    } else {
        blk->a = parse_stmt(p);
    }
    return blk;
}

static Node* parse_stmt(Parser* p) {
    Tok* tk = peek(p);
    if (!tk || tk->type == TK_DEDENT || tk->type == TK_EOF) return NULL;

    if (tk->type == KW_IF) return parse_if(p);
    if (tk->type == KW_WHILE) return parse_while(p);
    if (tk->type == KW_FOR) return parse_for(p);
    if (tk->type == KW_DEF) return parse_def(p);
    if (tk->type == KW_RETURN) return parse_ret(p);
    if (tk->type == KW_CLASS) return parse_class(p);

    /* Expression or assignment */
    Node* e = parse_expr(p);
    if (is_(p, TK_EQ)) { consume(p);
        /* If it's a bare identifier not yet defined, treat as var declaration */
        if (e->kind == ND_ID && !sc_find(p->local, e->name) && !sc_find(p->global, e->name)) {
            Node* rv = parse_expr(p);
            Node* n = nd_new(ND_VAR, e->line, e->col);
            n->name = strdup(e->name);
            n->a = rv;
            n->ty = rv->ty;
            sc_add(p->local, n->name, rv->ty, 0);
            free(e->name); free(e);
            return n;
        }
        return parse_assign(p, e);
    }
    return e;
}

static Node* parse_program(Parser* p) {
    Node* prog = nd_new(ND_PROG, 0, 0);
    Node** tail = &prog->a;
    while (p->pos < p->count && peek(p)->type != TK_EOF) {
        Node* st = parse_stmt(p);
        if (st) { *tail = st; while (*tail) { tail = &(*tail)->next; } }
        if (is_(p, TK_NL)) consume(p);
    }
    return prog;
}

static Node* parse(Tok** toks, int count) {
    Parser p = {toks, 0, count, NULL, NULL};
    p.global = sc_new(NULL);
    p.local = p.global;

    /* Pass 1: collect function and class names */
    int saved = p.pos;
    while (p.pos < count) {
        Tok* tk = p.toks[p.pos];
        if (tk->type == KW_DEF) {
            p.pos++;
            Tok* nm = expect_tok(&p, TK_ID);
            sc_add(p.global, nm->text, TY_INT, 1);
            while (p.pos < count && p.toks[p.pos]->type != TK_COLON) p.pos++;
            if (p.pos < count) p.pos++;
            int depth = 1;
            while (p.pos < count && depth > 0) {
                if (p.toks[p.pos]->type == TK_INDENT) depth++;
                else if (p.toks[p.pos]->type == TK_DEDENT) depth--;
                p.pos++;
            }
        } else if (tk->type == KW_CLASS) {
            p.pos++;
            Tok* nm = expect_tok(&p, TK_ID);
            sc_add(p.global, nm->text, TY_CLASS, 0);
            while (p.pos < count && p.toks[p.pos]->type != TK_DEDENT) p.pos++;
        } else {
            p.pos++;
        }
    }
    p.pos = saved;
    p.local = sc_new(p.global);

    Node* prog = parse_program(&p);
    sc_free(p.local);
    return prog;
}

/* ================================================================
 *  Section 6: Bytecode Compiler
 * ================================================================ */

/* --- Opcodes --- */
enum {
    OP_HALT = 0,
    OP_PUSH_INT, OP_PUSH_FLT, OP_PUSH_STR, OP_PUSH_BOL,
    OP_LOAD, OP_STORE,
    OP_LOAD_GBL, OP_STORE_GBL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_AND, OP_OR, OP_NOT, OP_NEG,
    OP_JMP, OP_JMP_IF_FALSE, OP_JMP_IF_TRUE,
    OP_POP, OP_DUP,
    OP_CALL, OP_RET,
    OP_ARRAY, OP_ARR_GET, OP_ARR_SET, OP_ARR_LEN,
    OP_FIELD_GET, OP_FIELD_SET, OP_MAKE_OBJ,
    OP_PRINT, OP_INPUT,
    OP_TYPE,
};

/* --- Instruction --- */
typedef struct { uint8_t op; int64_t arg; } Instr;

/* --- Code buffer --- */
typedef struct { Instr* code; int count, cap; } Code;

static void code_init(Code* c) { c->cap = 256; c->code = malloc(sizeof(Instr) * c->cap); c->count = 0; }

static int emit(Code* c, uint8_t op, int64_t arg) {
    if (c->count >= c->cap) { c->cap *= 2; c->code = realloc(c->code, sizeof(Instr) * c->cap); }
    c->code[c->count].op = op; c->code[c->count].arg = arg;
    return c->count++;
}

static int emit0(Code* c, uint8_t op) { return emit(c, op, 0); }

static void patch(Code* c, int addr, int64_t arg) { c->code[addr].arg = arg; }

static void code_free(Code* c) { free(c->code); }

/* --- Constant pool --- */
typedef struct { char** strs; int count, cap; } CP;

static void cp_init(CP* p) { p->cap = 64; p->strs = malloc(sizeof(char*) * p->cap); p->count = 0; }

static int cp_str(CP* p, const char* s) {
    if (p->count >= p->cap) { p->cap *= 2; p->strs = realloc(p->strs, sizeof(char*) * p->cap); }
    p->strs[p->count] = strdup(s);
    return p->count++;
}

static void cp_free(CP* p) { for (int i = 0; i < p->count; i++) free(p->strs[i]); free(p->strs); }

/* --- Function info --- */
typedef struct { char* name; Code code; int params; } Func;
typedef struct { Func* f; int count, cap; } FT;

static void ft_init(FT* t) { t->cap = 16; t->f = calloc(t->cap, sizeof(Func)); t->count = 0; }

static int ft_add(FT* t, const char* name, int params) {
    if (t->count >= t->cap) { t->cap *= 2; t->f = realloc(t->f, sizeof(Func) * t->cap); }
    Func* fn = &t->f[t->count];
    fn->name = strdup(name); code_init(&fn->code); fn->params = params;
    return t->count++;
}

static void ft_free(FT* t) { for (int i = 0; i < t->count; i++) { free(t->f[i].name); code_free(&t->f[i].code); } free(t->f); }

/* --- Struct info --- */
typedef struct { char* name; char** fields; int* ftypes; int nf; } StructDef;
typedef struct { StructDef* d; int count, cap; } ST;

static void st_init(ST* t) { t->cap = 8; t->d = calloc(t->cap, sizeof(StructDef)); t->count = 0; }

static int st_add(ST* t, const char* name) {
    if (t->count >= t->cap) { t->cap *= 2; t->d = realloc(t->d, sizeof(StructDef) * t->cap); }
    StructDef* s = &t->d[t->count];
    s->name = strdup(name); s->fields = NULL; s->ftypes = NULL; s->nf = 0;
    return t->count++;
}

static void st_free(ST* t) { for (int i = 0; i < t->count; i++) { free(t->d[i].name); free(t->d[i].fields); free(t->d[i].ftypes); } free(t->d); }

/* --- Compiler context --- */
typedef struct {
    Code* out;
    CP* cp;
    FT* ft;
    ST* st;
} Comp;

/* --- Compile-time variable tracking --- */
typedef struct VarEntry { char* name; int slot; struct VarEntry* next; } VarEntry;

static int next_slot(VarEntry* vars) {
    int max = 0;
    for (VarEntry* v = vars; v; v = v->next) if (v->slot >= max) max = v->slot + 1;
    return max;
}

static void add_var(VarEntry** vars, const char* name, VarEntry* entry) {
    entry->name = strdup(name);
    entry->slot = next_slot(*vars);
    entry->next = *vars;
    *vars = entry;
}

static int find_slot(VarEntry* vars, const char* name) {
    for (VarEntry* v = vars; v; v = v->next)
        if (!strcmp(v->name, name)) return v->slot;
    return -1;
}

static void free_vars(VarEntry* vars) {
    while (vars) { VarEntry* n = vars->next; free(vars->name); free(vars); vars = n; }
}

/* --- Forward declarations --- */
static void comp_expr(Comp* c, Node* n, VarEntry* vars);
static void comp_stmt(Comp* c, Node* n, VarEntry** vars);
static void comp_block(Comp* c, Node* blk, VarEntry** vars);

/* --- Expression compiler --- */
static void comp_expr(Comp* c, Node* n, VarEntry* vars) {
    if (!n) { emit0(c->out, OP_PUSH_INT); return; }

    switch (n->kind) {
        case ND_INT:
            emit(c->out, OP_PUSH_INT, n->ival);
            break;

        case ND_FLT: {
            int64_t raw; memcpy(&raw, &n->fval, sizeof(double));
            emit(c->out, OP_PUSH_FLT, raw);
            break; }

        case ND_STR: {
            int idx = cp_str(c->cp, n->name);
            emit(c->out, OP_PUSH_STR, idx);
            break; }

        case ND_BOOL:
            emit(c->out, OP_PUSH_BOL, n->ival ? 1 : 0);
            break;

        case ND_ID: {
            int slot = find_slot(vars, n->name);
            if (slot >= 0) emit(c->out, OP_LOAD, slot);
            else { int idx = cp_str(c->cp, n->name); emit(c->out, OP_LOAD_GBL, idx); }
            break; }

        case ND_BINOP: {
            if (n->ival == KW_AND) {
                comp_expr(c, n->a, vars);
                emit0(c->out, OP_DUP);
                int jmp = emit(c->out, OP_JMP_IF_FALSE, -1);
                emit0(c->out, OP_POP);
                comp_expr(c, n->b, vars);
                patch(c->out, jmp, c->out->count);
            } else if (n->ival == KW_OR) {
                comp_expr(c, n->a, vars);
                emit0(c->out, OP_DUP);
                int jmp = emit(c->out, OP_JMP_IF_TRUE, -1);
                emit0(c->out, OP_POP);
                comp_expr(c, n->b, vars);
                patch(c->out, jmp, c->out->count);
            } else {
                comp_expr(c, n->a, vars);
                comp_expr(c, n->b, vars);
                switch (n->ival) {
                    case TK_PLUS: emit0(c->out, OP_ADD); break;
                    case TK_MINUS: emit0(c->out, OP_SUB); break;
                    case TK_STAR: emit0(c->out, OP_MUL); break;
                    case TK_SLASH: emit0(c->out, OP_DIV); break;
                    case TK_PCT: emit0(c->out, OP_MOD); break;
                    case TK_EQEQ: emit0(c->out, OP_EQ); break;
                    case TK_NEQ: emit0(c->out, OP_NEQ); break;
                    case TK_LT: emit0(c->out, OP_LT); break;
                    case TK_GT: emit0(c->out, OP_GT); break;
                    case TK_LTE: emit0(c->out, OP_LTE); break;
                    case TK_GTE: emit0(c->out, OP_GTE); break;
                }
            }
            break; }

        case ND_UNOP:
            comp_expr(c, n->a, vars);
            if (n->ival == KW_NOT) emit0(c->out, OP_NOT);
            else emit0(c->out, OP_NEG);
            break;

        case ND_CALL: {
            if (!strcmp(n->name, "print")) {
                comp_expr(c, n->a, vars);
                emit0(c->out, OP_PRINT);
            } else if (!strcmp(n->name, "input")) {
                if (n->a) comp_expr(c, n->a, vars);
                else emit(c->out, OP_PUSH_STR, -1);
                emit0(c->out, OP_INPUT);
            } else if (!strcmp(n->name, "len")) {
                comp_expr(c, n->a, vars);
                emit0(c->out, OP_ARR_LEN);
            } else if (!strcmp(n->name, "type")) {
                comp_expr(c, n->a, vars);
                emit0(c->out, OP_TYPE);
            } else {
                int argc = 0; Node* arg = n->a;
                while (arg) { comp_expr(c, arg, vars); arg = arg->next; argc++; }
                int fi = -1;
                for (int i = 0; i < c->ft->count; i++)
                    if (!strcmp(c->ft->f[i].name, n->name)) { fi = i; break; }
                emit(c->out, OP_CALL, fi | (argc << 16));
            }
            break; }

        case ND_IDX:
            comp_expr(c, n->a, vars);
            comp_expr(c, n->b, vars);
            emit0(c->out, OP_ARR_GET);
            break;

        case ND_FLD: {
            comp_expr(c, n->a, vars);
            int fi = 0;
            for (int s = 0; s < c->st->count; s++) {
                for (int f = 0; f < c->st->d[s].nf; f++) {
                    if (!strcmp(c->st->d[s].fields[f], n->name)) { fi = f; goto found_fi; }
                }
            }
            found_fi:
            emit(c->out, OP_FIELD_GET, fi);
            break; }

        case ND_ARR: {
            int ne = 0; Node* e = n->a;
            while (e) { comp_expr(c, e, vars); e = e->next; ne++; }
            emit(c->out, OP_ARRAY, ne);
            break; }

        case ND_CLASS_LIT: {
            int nf = 0; Node* f = n->a;
            while (f) { comp_expr(c, f->a, vars); f = f->next; nf++; }
            int si = -1;
            for (int i = 0; i < c->st->count; i++)
                if (!strcmp(c->st->d[i].name, n->name)) { si = i; break; }
            emit(c->out, OP_MAKE_OBJ, si | (nf << 16));
            break; }

        default:
            emit0(c->out, OP_PUSH_INT);
            break;
    }
}

static void comp_assign(Comp* c, Node* lv, Node* rv, VarEntry** vars) {
    if (lv->kind == ND_IDX) {
        comp_expr(c, lv->a, *vars);
        comp_expr(c, lv->b, *vars);
        comp_expr(c, rv, *vars);
        emit0(c->out, OP_ARR_SET);
    } else if (lv->kind == ND_FLD) {
        comp_expr(c, lv->a, *vars);
        comp_expr(c, rv, *vars);
        int fi = 0;
        for (int s = 0; s < c->st->count; s++) {
            for (int f = 0; f < c->st->d[s].nf; f++) {
                if (!strcmp(c->st->d[s].fields[f], lv->name)) { fi = f; goto found_fs; }
            }
        }
        found_fs:
        emit(c->out, OP_FIELD_SET, fi);
    } else {
        comp_expr(c, rv, *vars);
        int slot = find_slot(*vars, lv->name);
        if (slot >= 0) emit(c->out, OP_STORE, slot);
        else { int idx = cp_str(c->cp, lv->name); emit(c->out, OP_STORE_GBL, idx); }
    }
}

static void comp_stmt(Comp* c, Node* n, VarEntry** vars) {
    if (!n) return;

    switch (n->kind) {
        case ND_VAR: {
            comp_expr(c, n->a, *vars);
            VarEntry* ve = malloc(sizeof(VarEntry));
            ve->next = NULL;
            add_var(vars, n->name, ve);
            emit(c->out, OP_STORE, ve->slot);
            break; }

        case ND_ASSIGN:
            comp_assign(c, n->a, n->b, vars);
            break;

        case ND_IF: {
            comp_expr(c, n->a, *vars);
            int j1 = emit(c->out, OP_JMP_IF_FALSE, -1);
            comp_block(c, n->b, vars);
            if (n->c) {
                int j2 = emit(c->out, OP_JMP, -1);
                patch(c->out, j1, c->out->count);
                if (n->c->kind == ND_IF) comp_stmt(c, n->c, vars);
                else comp_block(c, n->c, vars);
                patch(c->out, j2, c->out->count);
            } else {
                patch(c->out, j1, c->out->count);
            }
            break; }

        case ND_WHILE: {
            int loop = c->out->count;
            comp_expr(c, n->a, *vars);
            int j1 = emit(c->out, OP_JMP_IF_FALSE, -1);
            comp_block(c, n->b, vars);
            emit(c->out, OP_JMP, loop);
            patch(c->out, j1, c->out->count);
            break; }

        case ND_FOR: {
            /* for x in range(n): body -> desugar to counter loop */
            VarEntry* ve = malloc(sizeof(VarEntry)); ve->next = NULL;
            add_var(vars, n->name, ve);
            int var_slot = ve->slot;

            /* Compile limit expression */
            comp_expr(c, n->a, *vars);
            VarEntry* lim = malloc(sizeof(VarEntry)); lim->next = NULL;
            add_var(vars, "__limit__", lim);
            emit(c->out, OP_STORE, lim->slot);

            emit0(c->out, OP_PUSH_INT);
            emit(c->out, OP_STORE, var_slot);

            int loop = c->out->count;
            /* condition: var < limit */
            emit(c->out, OP_LOAD, var_slot);
            emit(c->out, OP_LOAD, lim->slot);
            emit0(c->out, OP_LT);
            int j1 = emit(c->out, OP_JMP_IF_FALSE, -1);

            comp_block(c, n->b, vars);

            /* increment: var = var + 1 */
            emit(c->out, OP_LOAD, var_slot);
            emit(c->out, OP_PUSH_INT, 1);
            emit0(c->out, OP_ADD);
            emit(c->out, OP_STORE, var_slot);

            emit(c->out, OP_JMP, loop);
            patch(c->out, j1, c->out->count);
            break; }

        case ND_RET:
            if (n->a) comp_expr(c, n->a, *vars);
            else emit0(c->out, OP_PUSH_INT);
            emit0(c->out, OP_RET);
            break;

        case ND_PRINT:
            if (n->a && n->a->kind == ND_CALL && n->a->name && !strcmp(n->a->name, "print")) {
                comp_expr(c, n->a->a, *vars);
            } else {
                comp_expr(c, n->a, *vars);
            }
            emit0(c->out, OP_PRINT);
            break;

        case ND_FN: {
            int fi = ft_add(c->ft, n->name, 0);
            int pc = 0; Node* pa = n->a; while (pa) { pc++; pa = pa->next; }
            c->ft->f[fi].params = pc;

            Code* saved = c->out;
            c->out = &c->ft->f[fi].code;

            VarEntry* fvars = NULL;
            pa = n->a; int slot = 0;
            while (pa) {
                VarEntry* ve = malloc(sizeof(VarEntry));
                ve->name = strdup(pa->name);
                ve->slot = slot;
                ve->next = fvars;
                fvars = ve;
                slot++;
                pa = pa->next;
            }

            if (n->b) comp_block(c, n->b, &fvars);
            emit0(c->out, OP_PUSH_INT);
            emit0(c->out, OP_RET);
            free_vars(fvars);

            c->out = saved;
            break; }

        case ND_CLASS: {
            int si = st_add(c->st, n->name);
            int nf = 0; Node* f = n->a; while (f) { nf++; f = f->next; }
            c->st->d[si].nf = nf;
            c->st->d[si].fields = malloc(sizeof(char*) * nf);
            c->st->d[si].ftypes = malloc(sizeof(int) * nf);
            f = n->a; int i = 0;
            while (f) {
                c->st->d[si].fields[i] = strdup(f->name);
                c->st->d[si].ftypes[i] = f->ty;
                i++; f = f->next;
            }
            break; }

        default:
            /* Expression statement (e.g., bare function call) */
            comp_expr(c, n, *vars);
            break;
    }
}

static void comp_block(Comp* c, Node* blk, VarEntry** vars) {
    if (!blk) return;
    Node* body = (blk->kind == ND_BLOCK) ? blk->a : blk;
    for (Node* s = body; s; s = s->next)
        comp_stmt(c, s, vars);
}

/* ================================================================
 *  Section 7: VM Runtime
 * ================================================================ */

/* Value representation */
enum { VT_NIL, VT_INT, VT_FLT, VT_STR, VT_BOL, VT_ARR, VT_OBJ };

typedef struct { int type; int64_t i; double f; const char* s; int b; void* p; } Val;

static Val val_nil(void) { Val v = {0}; v.type = VT_NIL; return v; }
static Val val_int(int64_t i) { Val v = {0}; v.type = VT_INT; v.i = i; return v; }
static Val val_flt(double f) { Val v = {0}; v.type = VT_FLT; v.f = f; return v; }
static Val val_str(const char* s) { Val v = {0}; v.type = VT_STR; v.s = s; return v; }
static Val val_bol(int b) { Val v = {0}; v.type = VT_BOL; v.b = b ? 1 : 0; return v; }
static Val val_arr(void* p) { Val v = {0}; v.type = VT_ARR; v.p = p; return v; }
static Val val_obj(void* p) { Val v = {0}; v.type = VT_OBJ; v.p = p; return v; }

static int val_to_bol(Val v) {
    switch (v.type) {
        case VT_INT: return v.i != 0;
        case VT_FLT: return v.f != 0.0;
        case VT_STR: return v.s && v.s[0];
        case VT_BOL: return v.b;
        case VT_ARR: return (v.p != NULL);
        case VT_OBJ: return 1;
        default: return 0;
    }
}

static const char* val_typename(Val v) {
    switch (v.type) {
        case VT_INT: return "int"; case VT_FLT: return "float";
        case VT_STR: return "str"; case VT_BOL: return "bool";
        case VT_ARR: return "array"; case VT_OBJ: return "object";
        default: return "nil";
    }
}

/* Array and Object storage */
typedef struct { int64_t* data; int len, cap; } VArr;

/* Object field storage with type tag */
typedef struct { int64_t val; int type; } Fld;

/* Heap tracking */
static void** g_heap_objs = NULL;
static int g_heap_count = 0;
static int g_heap_cap = 0;

static void* heap_track(void* p) {
    if (!p) return p;
    if (g_heap_count >= g_heap_cap) {
        g_heap_cap = g_heap_cap ? g_heap_cap * 2 : 64;
        g_heap_objs = realloc(g_heap_objs, sizeof(void*) * g_heap_cap);
    }
    g_heap_objs[g_heap_count++] = p;
    return p;
}

static void heap_cleanup(void) {
    for (int i = 0; i < g_heap_count; i++) free(g_heap_objs[i]);
    free(g_heap_objs);
    g_heap_objs = NULL; g_heap_count = 0; g_heap_cap = 0;
}

/* Global variable storage */
typedef struct { char* name; Val val; } GlobalVar;

static int g_gvar_count = 0;
static GlobalVar* g_gvars = NULL;

static Val gvar_get(const char* name) {
    for (int i = 0; i < g_gvar_count; i++)
        if (!strcmp(g_gvars[i].name, name)) return g_gvars[i].val;
    return val_nil();
}

static void gvar_set(const char* name, Val v) {
    for (int i = 0; i < g_gvar_count; i++)
        if (!strcmp(g_gvars[i].name, name)) { g_gvars[i].val = v; return; }
    if (g_gvar_count >= 256) return;
    if (!g_gvars) g_gvars = malloc(sizeof(GlobalVar) * 256);
    g_gvars[g_gvar_count].name = strdup(name);
    g_gvars[g_gvar_count].val = v;
    g_gvar_count++;
}

static void gvar_cleanup(void) {
    for (int i = 0; i < g_gvar_count; i++) free(g_gvars[i].name);
    free(g_gvars);
    g_gvars = NULL; g_gvar_count = 0;
}

/* --- VM Execution --- */
static int vm_exec(Instr* code, int code_len, CP* cp, FT* ft, ST* st) {
    (void)st;  /* struct table used only at compile time */
    Val* stack = malloc(sizeof(Val) * 8192);
    int sp = 0;
    Val* locals = malloc(sizeof(Val) * 512);
    int local_count = 0;
    Instr* ip = code;
    Instr* code_end = code + code_len;
    Instr* cur_code = code;  /* base of currently executing code segment */

    typedef struct { Instr* ip; Instr* end; int local_count; int sp; Val* locals_buf; Instr* cur_code; } CallFrame;
    CallFrame cframes[64];
    int cf_count = 0;

    while (ip < code_end) {
        Instr* inst = ip++;
        switch (inst->op) {
            case OP_HALT: return 0;

            case OP_PUSH_INT: stack[sp++] = val_int(inst->arg); break;
            case OP_PUSH_FLT: { double d; memcpy(&d, &inst->arg, 8); stack[sp++] = val_flt(d); break; }
            case OP_PUSH_STR: stack[sp++] = (inst->arg < 0) ? val_str(NULL) : val_str(cp->strs[inst->arg]); break;
            case OP_PUSH_BOL: stack[sp++] = val_bol(inst->arg ? 1 : 0); break;

            case OP_LOAD: stack[sp++] = locals[(int)inst->arg]; break;
            case OP_STORE: locals[(int)inst->arg] = stack[--sp]; if ((int)inst->arg + 1 > local_count) local_count = (int)inst->arg + 1; break;
            case OP_LOAD_GBL: stack[sp++] = gvar_get(cp->strs[inst->arg]); break;
            case OP_STORE_GBL: gvar_set(cp->strs[inst->arg], stack[--sp]); break;

            case OP_ADD: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_STR || b.type == VT_STR) {
                    SBuf* sb = sb_new();
                    if (a.type == VT_STR) sb_push(sb, a.s); else {
                        switch (a.type) { case VT_INT: sb_int(sb, a.i); break; case VT_FLT: sb_flt(sb, a.f); break;
                            case VT_BOL: sb_push(sb, a.b ? "True" : "False"); break; default: sb_push(sb, "nil"); break; }
                    }
                    if (b.type == VT_STR) sb_push(sb, b.s); else {
                        switch (b.type) { case VT_INT: sb_int(sb, b.i); break; case VT_FLT: sb_flt(sb, b.f); break;
                            case VT_BOL: sb_push(sb, b.b ? "True" : "False"); break; default: sb_push(sb, "nil"); break; }
                    }
                    char* s = heap_track(strdup(sb->s)); sb_free(sb);
                    stack[sp++] = val_str(s);
                } else if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_flt(va + vb);
                } else stack[sp++] = val_int(a.i + b.i);
                break; }

            case OP_SUB: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_flt(va - vb);
                } else stack[sp++] = val_int(a.i - b.i);
                break; }

            case OP_MUL: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_flt(va * vb);
                } else stack[sp++] = val_int(a.i * b.i);
                break; }

            case OP_DIV: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_flt(va / vb);
                } else stack[sp++] = val_int(a.i / b.i);
                break; }

            case OP_MOD: {
                Val b = stack[--sp], a = stack[--sp];
                stack[sp++] = val_int(a.i % b.i);
                break; }

            case OP_EQ: {
                Val b = stack[--sp], a = stack[--sp]; int eq = 0;
                if (a.type != b.type) eq = 0;
                else switch (a.type) {
                    case VT_INT: eq = a.i == b.i; break;
                    case VT_FLT: eq = a.f == b.f; break;
                    case VT_STR: eq = !strcmp(a.s ? a.s : "", b.s ? b.s : ""); break;
                    case VT_BOL: eq = a.b == b.b; break;
                    default: eq = 0;
                }
                stack[sp++] = val_bol(eq);
                break; }

            case OP_NEQ: {
                Val b = stack[--sp], a = stack[--sp]; int eq = 0;
                if (a.type != b.type) eq = 1;
                else switch (a.type) {
                    case VT_INT: eq = a.i != b.i; break;
                    case VT_FLT: eq = a.f != b.f; break;
                    case VT_STR: eq = strcmp(a.s ? a.s : "", b.s ? b.s : "") != 0; break;
                    case VT_BOL: eq = a.b != b.b; break;
                    default: eq = 1;
                }
                stack[sp++] = val_bol(eq);
                break; }

            case OP_LT: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_bol(va < vb);
                } else stack[sp++] = val_bol(a.i < b.i);
                break; }

            case OP_GT: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_bol(va > vb);
                } else stack[sp++] = val_bol(a.i > b.i);
                break; }

            case OP_LTE: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_bol(va <= vb);
                } else stack[sp++] = val_bol(a.i <= b.i);
                break; }

            case OP_GTE: {
                Val b = stack[--sp], a = stack[--sp];
                if (a.type == VT_FLT || b.type == VT_FLT) {
                    double va = a.type == VT_FLT ? a.f : (double)a.i;
                    double vb = b.type == VT_FLT ? b.f : (double)b.i;
                    stack[sp++] = val_bol(va >= vb);
                } else stack[sp++] = val_bol(a.i >= b.i);
                break; }

            case OP_NOT: {
                Val a = stack[--sp];
                stack[sp++] = val_bol(!val_to_bol(a));
                break; }

            case OP_NEG: {
                Val a = stack[--sp];
                stack[sp++] = (a.type == VT_FLT) ? val_flt(-a.f) : val_int(-a.i);
                break; }

            case OP_JMP: ip = cur_code + inst->arg; break;
            case OP_JMP_IF_FALSE: { Val v = stack[--sp]; if (!val_to_bol(v)) ip = cur_code + inst->arg; break; }
            case OP_JMP_IF_TRUE: { Val v = stack[--sp]; if (val_to_bol(v)) ip = cur_code + inst->arg; break; }

            case OP_POP: sp--; break;
            case OP_DUP: stack[sp] = stack[sp - 1]; sp++; break;

            case OP_CALL: {
                int fi = (int)(inst->arg & 0xFFFF);
                int argc = (int)((inst->arg >> 16) & 0xFFFF);
                Func* fn = &ft->f[fi];
                int ret_pos = sp - argc;
                /* Save parent's locals before overwriting */
                int saved_lc = local_count;
                Val* saved_locals = NULL;
                if (saved_lc > 0) {
                    saved_locals = malloc(sizeof(Val) * saved_lc);
                    memcpy(saved_locals, locals, saved_lc * sizeof(Val));
                }
                /* Copy arguments into locals as function params */
                for (int i = argc - 1; i >= 0; i--)
                    locals[i] = stack[sp - (argc - i)];
                local_count = argc;
                sp -= argc;
                /* Push call frame */
                cframes[cf_count].ip = ip;
                cframes[cf_count].end = code_end;
                cframes[cf_count].sp = ret_pos;
                cframes[cf_count].local_count = saved_lc;
                cframes[cf_count].locals_buf = saved_locals;
                cframes[cf_count].cur_code = cur_code;
                cf_count++;
                cur_code = fn->code.code;
                ip = fn->code.code;
                code_end = fn->code.code + fn->code.count;
                break; }

            case OP_RET: {
                Val ret = stack[--sp];
                if (cf_count > 0) {
                    cf_count--;
                    ip = cframes[cf_count].ip;
                    code_end = cframes[cf_count].end;
                    cur_code = cframes[cf_count].cur_code;
                    sp = cframes[cf_count].sp;
                    int parent_lc = cframes[cf_count].local_count;
                    if (parent_lc > 0 && cframes[cf_count].locals_buf) {
                        memcpy(locals, cframes[cf_count].locals_buf, parent_lc * sizeof(Val));
                    }
                    local_count = parent_lc;
                    free(cframes[cf_count].locals_buf);
                    cframes[cf_count].locals_buf = NULL;
                    stack[sp++] = ret;
                } else {
                    return 0;
                }
                break; }

            case OP_ARRAY: {
                int ne = (int)inst->arg;
                VArr* arr = heap_track(malloc(sizeof(VArr)));
                arr->cap = ne > 0 ? ne : 4;
                arr->data = malloc(sizeof(int64_t) * arr->cap);
                arr->len = ne;
                for (int i = ne - 1; i >= 0; i--) {
                    Val v = stack[--sp];
                    arr->data[i] = v.type == VT_INT ? v.i : (v.type == VT_FLT ? (int64_t)v.f : 0);
                }
                stack[sp++] = val_arr(arr);
                break; }

            case OP_ARR_GET: {
                Val iv = stack[--sp], av = stack[--sp];
                VArr* arr = av.p;
                int idx = (int)iv.i;
                if (idx < 0 || idx >= arr->len) {
                    fprintf(stderr, "\xe9\x94\x99\xe8\xaf\xaf: array index %d out of bounds (len=%d)\n", idx, arr->len);
                    return 1;
                }
                stack[sp++] = val_int(arr->data[idx]);
                break; }

            case OP_ARR_SET: {
                Val vv = stack[--sp], iv = stack[--sp], av = stack[--sp];
                VArr* arr = av.p;
                int idx = (int)iv.i;
                if (idx < 0 || idx >= arr->len) {
                    fprintf(stderr, "\xe9\x94\x99\xe8\xaf\xaf: array index %d out of bounds (len=%d)\n", idx, arr->len);
                    return 1;
                }
                arr->data[idx] = vv.type == VT_INT ? vv.i : (vv.type == VT_FLT ? (int64_t)vv.f : 0);
                break; }

            case OP_ARR_LEN: {
                Val av = stack[--sp];
                VArr* arr = av.p;
                stack[sp++] = val_int(arr->len);
                break; }

            case OP_FIELD_GET: {
                Val ov = stack[--sp];
                Fld* obj = ov.p;
                Fld* f = &obj[inst->arg];
                switch (f->type) {
                    case VT_INT: stack[sp++] = val_int(f->val); break;
                    case VT_FLT: { double d; memcpy(&d, &f->val, 8); stack[sp++] = val_flt(d); break; }
                    case VT_BOL: stack[sp++] = val_bol((int)f->val); break;
                    case VT_STR: stack[sp++] = val_str((const char*)(uintptr_t)f->val); break;
                    case VT_ARR: stack[sp++] = val_arr((void*)(uintptr_t)f->val); break;
                    case VT_OBJ: stack[sp++] = val_obj((void*)(uintptr_t)f->val); break;
                    default: stack[sp++] = val_int(0); break;
                }
                break; }

            case OP_FIELD_SET: {
                Val vv = stack[--sp], ov = stack[--sp];
                Fld* obj = ov.p;
                Fld* f = &obj[inst->arg];
                f->type = vv.type;
                switch (vv.type) {
                    case VT_INT: f->val = vv.i; break;
                    case VT_FLT: memcpy(&f->val, &vv.f, 8); break;
                    case VT_BOL: f->val = vv.b; break;
                    case VT_STR: case VT_ARR: case VT_OBJ: f->val = (int64_t)(uintptr_t)vv.p; break;
                    default: f->val = 0; break;
                }
                break; }

            case OP_MAKE_OBJ: {
                int nf = (int)((inst->arg >> 16) & 0xFFFF);
                Fld* obj = heap_track(malloc(sizeof(Fld) * nf));
                for (int i = nf - 1; i >= 0; i--) {
                    Val v = stack[--sp];
                    obj[i].type = v.type;
                    switch (v.type) {
                        case VT_INT: obj[i].val = v.i; break;
                        case VT_FLT: memcpy(&obj[i].val, &v.f, 8); break;
                        case VT_BOL: obj[i].val = v.b; break;
                        case VT_STR: case VT_ARR: case VT_OBJ: obj[i].val = (int64_t)(uintptr_t)v.p; break;
                        default: obj[i].val = 0; break;
                    }
                }
                stack[sp++] = val_obj(obj);
                break; }

            case OP_PRINT: {
                Val v = stack[--sp];
                switch (v.type) {
                    case VT_INT: printf("%lld", (long long)v.i); break;
                    case VT_FLT: printf("%g", v.f); break;
                    case VT_STR: printf("%s", v.s ? v.s : "nil"); break;
                    case VT_BOL: printf("%s", v.b ? "True" : "False"); break;
                    case VT_ARR: {
                        VArr* arr = v.p;
                        printf("[");
                        for (int i = 0; i < arr->len; i++) {
                            if (i) printf(", ");
                            printf("%lld", (long long)arr->data[i]);
                        }
                        printf("]");
                        break; }
                    case VT_OBJ: printf("{object}"); break;
                    default: printf("nil"); break;
                }
                printf("\n");
                break; }

            case OP_INPUT: {
                const char* prompt = (inst->arg < 0) ? NULL : cp->strs[inst->arg];
                if (prompt) printf("%s", prompt);
                int64_t v;
                if (scanf("%lld", &v) != 1) v = 0;
                stack[sp++] = val_int(v);
                break; }

            case OP_TYPE: {
                Val v = stack[--sp];
                stack[sp++] = val_str(val_typename(v));
                break; }

            default:
                fprintf(stderr, "\xe9\x94\x99\xe8\xaf\xaf: unknown opcode %d\n", inst->op);
                goto vm_cleanup;
        }
    }

vm_cleanup:
    /* Free any still-active call frame locals buffers */
    for (int i = 0; i < cf_count; i++) free(cframes[i].locals_buf);
    free(stack);
    free(locals);
    return 0;
}

/* ================================================================
 *  Section 8: Main
 * ================================================================ */

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = malloc(sz + 1); fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    return buf;
}

int run_dsc(const char* file) {
    char* src = read_file(file);
    if (!src) { fprintf(stderr, "\xe9\x94\x99\xe8\xaf\xaf: cannot read %s\n", file); return 1; }

    printf("\xe4\xbf\xa1\xe6\x81\xaf: parsing...\n");
    int tc;
    Tok** toks = tokenize(src, &tc);
    Node* prog = parse(toks, tc);

    printf("\xe4\xbf\xa1\xe6\x81\xaf: compiling...\n");
    CP cp; cp_init(&cp);
    FT ft; ft_init(&ft);
    ST st; st_init(&st);
    Code main_code; code_init(&main_code);

    Comp comp;
    comp.out = &main_code;
    comp.cp = &cp;
    comp.ft = &ft;
    comp.st = &st;

    VarEntry* vars = NULL;

    /* First pass: compile functions and classes */
    for (Node* s = prog->a; s; s = s->next) {
        if (s->kind == ND_FN || s->kind == ND_CLASS)
            comp_stmt(&comp, s, &vars);
    }
    /* Second pass: compile top-level code */
    for (Node* s = prog->a; s; s = s->next) {
        if (s->kind != ND_FN && s->kind != ND_CLASS)
            comp_stmt(&comp, s, &vars);
    }
    emit0(&main_code, OP_HALT);
    free_vars(vars);

    printf("\xe4\xbf\xa1\xe6\x81\xaf: running...\n");
    int ret = vm_exec(main_code.code, main_code.count, &cp, &ft, &st);

    heap_cleanup();
    gvar_cleanup();
    cp_free(&cp); ft_free(&ft); st_free(&st);
    code_free(&main_code);
    free(src);
    for (int i = 0; i < tc; i++) tok_free(toks[i]);
    free(toks);

    return ret;
}

int main(int argc, char** argv) {
    fprintf(stderr, "DSC v2.0 - Bytecode VM\n");
    if (argc < 2) { fprintf(stderr, "Usage: dsc <source.dscbt>\n"); return 1; }
    return run_dsc(argv[1]);
}
