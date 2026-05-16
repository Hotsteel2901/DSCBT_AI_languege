/* DSCBT Compiler - dsc.c
 * Compiles .dscbt source files to native executables via C code generation.
 * Usage: dsc <source.dscbt> [output.exe]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>

/* ================================================================
 *  Error handling
 * ================================================================ */
static void error_at(int line, int col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "\346\227\245\345\277\227 [%d:%d]: ", line, col);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

/* ================================================================
 *  Dynamic string
 * ================================================================ */
typedef struct { char* data; int len; int cap; } DStr;
static DStr* dstr_new(void) { DStr* s = malloc(sizeof(DStr)); s->cap = 128; s->data = malloc(s->cap); s->data[0] = 0; s->len = 0; return s; }
static void dstr_grow(DStr* s, int need) { while (s->len + need + 1 > s->cap) { s->cap *= 2; s->data = realloc(s->data, s->cap); } }
static void dstr_add(DStr* s, const char* str) { int n = (int)strlen(str); dstr_grow(s, n); strcpy(s->data + s->len, str); s->len += n; }
static void dstr_addc(DStr* s, char c) { dstr_grow(s, 1); s->data[s->len++] = c; s->data[s->len] = 0; }
static void dstr_addi(DStr* s, int64_t v) { char b[32]; snprintf(b, sizeof(b), "%lld", (long long)v); dstr_add(s, b); }
static void dstr_addd(DStr* s, double v) { char b[64]; snprintf(b, sizeof(b), "%.16g", v); dstr_add(s, b); }
static void dstr_free(DStr* s) { free(s->data); free(s); }

/* ================================================================
 *  Token types
 * ================================================================ */
enum {
    TK_EOF=0, TK_NL, TK_INDENT, TK_DEDENT,
    TK_IDENT, TK_INT, TK_FLT, TK_STR,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PCT,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_GT, TK_LTE, TK_GTE,
    TK_LP, TK_RP, TK_LB, TK_RB, TK_LC, TK_RC,
    TK_COLON, TK_SEMI, TK_COMMA, TK_DOT, TK_HASH,
    TK_AMP, TK_PIPE, TK_BANG,
    KW_LET, KW_FN, KW_RET, KW_IF, KW_ELS, KW_WHL, KW_FOR,
    KW_PRT, KW_INP, KW_TRU, KW_FAL, KW_TYP,
    KW_INT, KW_FLT, KW_STR, KW_BOL, KW_VOID,
};
static const char* tk_names[] = {
    "EOF","NL","INDENT","DEDENT","ID","INT","FLT","STR",
    "+","-","*","/","%","=","==","!=","<",">","<=",">=",
    "(",")","[","]","{","}",":",";",",",".","#","&","|","!",
    "let","fn","ret","if","els","whl","for","prt","inp","tru","fal","typ",
    "int","flt","str","bol","void",
};

typedef struct { int type; char* text; int line, col; } Token;
static Token* tk_new(int t, const char* s, int ln, int co) {
    Token* tk = malloc(sizeof(Token)); tk->type = t; tk->text = strdup(s?s:""); tk->line = ln; tk->col = co; return tk;
}
static void tk_free(Token* tk) { free(tk->text); free(tk); }

/* ================================================================
 *  Lexer
 * ================================================================ */
typedef struct {
    const char* src; int pos, len, line, col;
} Lexer;

static int is_id0(int c) { return isalpha(c) || c == '_'; }
static int is_id1(int c) { return isalpha(c) || isdigit(c) || c == '_'; }

static int kw_lookup(const char* w) {
    if (!strcmp(w,"let")) return KW_LET; if (!strcmp(w,"fn")) return KW_FN;
    if (!strcmp(w,"ret")) return KW_RET; if (!strcmp(w,"if")) return KW_IF;
    if (!strcmp(w,"els")) return KW_ELS; if (!strcmp(w,"whl")) return KW_WHL;
    if (!strcmp(w,"for")) return KW_FOR; if (!strcmp(w,"prt")) return KW_PRT;
    if (!strcmp(w,"inp")) return KW_INP; if (!strcmp(w,"tru")) return KW_TRU;
    if (!strcmp(w,"fal")) return KW_FAL; if (!strcmp(w,"typ")) return KW_TYP;
    if (!strcmp(w,"int")) return KW_INT; if (!strcmp(w,"flt")) return KW_FLT;
    if (!strcmp(w,"str")) return KW_STR; if (!strcmp(w,"bol")) return KW_BOL;
    if (!strcmp(w,"void")) return KW_VOID; return TK_IDENT;
}

static Token* lex_one(Lexer* lx) {
    while (lx->pos < lx->len && lx->src[lx->pos] != '\n' && (lx->src[lx->pos] == ' ' || lx->src[lx->pos] == '\t')) {
        lx->pos++; lx->col++;
    }
    if (lx->pos >= lx->len) return tk_new(TK_EOF, "", lx->line, lx->col);

    char c = lx->src[lx->pos];
    if (c == '\n') { lx->pos++; lx->line++; int co = lx->col; lx->col = 1; return tk_new(TK_NL, "\n", lx->line-1, co); }

    if (c == '"') { lx->pos++; lx->col++; DStr* s = dstr_new();
        while (lx->pos < lx->len && lx->src[lx->pos] != '"' && lx->src[lx->pos] != '\n') {
            if (lx->src[lx->pos] == '\\' && lx->pos+1 < lx->len) { lx->pos++; lx->col++;
                switch (lx->src[lx->pos]) { case 'n': dstr_addc(s,'\n'); break; case 't': dstr_addc(s,'\t'); break;
                case 'r': dstr_addc(s,'\r'); break; case '\\': dstr_addc(s,'\\'); break; case '"': dstr_addc(s,'"'); break;
                default: dstr_addc(s,lx->src[lx->pos]); break; }
            } else { dstr_addc(s, lx->src[lx->pos]); }
            lx->pos++; lx->col++;
        }
        if (lx->pos >= lx->len || lx->src[lx->pos] == '\n') error_at(lx->line, lx->col, "unterminated string");
        lx->pos++; lx->col++;
        Token* tk = tk_new(TK_STR, s->data, lx->line, lx->col); dstr_free(s); return tk; }

    if (isdigit(c)) { int is_float = 0, ln = lx->line, co = lx->col; DStr* s = dstr_new();
        while (lx->pos < lx->len && isdigit(lx->src[lx->pos])) { dstr_addc(s, lx->src[lx->pos]); lx->pos++; lx->col++; }
        if (lx->pos < lx->len && lx->src[lx->pos] == '.' && lx->pos+1 < lx->len && isdigit(lx->src[lx->pos+1])) {
            is_float = 1; dstr_addc(s, '.'); lx->pos++; lx->col++;
            while (lx->pos < lx->len && isdigit(lx->src[lx->pos])) { dstr_addc(s, lx->src[lx->pos]); lx->pos++; lx->col++; }
        }
        Token* tk = tk_new(is_float ? TK_FLT : TK_INT, s->data, ln, co); dstr_free(s); return tk; }

    if (is_id0(c)) { int ln = lx->line, co = lx->col; DStr* s = dstr_new();
        while (lx->pos < lx->len && is_id1(lx->src[lx->pos])) { dstr_addc(s, lx->src[lx->pos]); lx->pos++; lx->col++; }
        int t = kw_lookup(s->data); Token* tk = tk_new(t, s->data, ln, co); dstr_free(s); return tk; }

    if (c == '/' && lx->pos+1 < lx->len) {
        if (lx->src[lx->pos+1] == '/') { while (lx->pos < lx->len && lx->src[lx->pos] != '\n') { lx->pos++; lx->col++; } return lex_one(lx); }
        if (lx->src[lx->pos+1] == '*') { lx->pos += 2; lx->col += 2;
            while (lx->pos+1 < lx->len && !(lx->src[lx->pos] == '*' && lx->src[lx->pos+1] == '/')) {
                if (lx->src[lx->pos] == '\n') { lx->line++; lx->col = 1; } else lx->col++;
                lx->pos++;
            }
            lx->pos += 2; lx->col += 2; return lex_one(lx); }
    }

    int ln = lx->line, co = lx->col; lx->pos++; lx->col++;
    switch (c) {
        case '+': return tk_new(TK_PLUS,"+",ln,co); case '-': return tk_new(TK_MINUS,"-",ln,co);
        case '*': return tk_new(TK_STAR,"*",ln,co); case '/': return tk_new(TK_SLASH,"/",ln,co);
        case '%': return tk_new(TK_PCT,"%",ln,co); case '(': return tk_new(TK_LP,"(",ln,co);
        case ')': return tk_new(TK_RP,")",ln,co); case '[': return tk_new(TK_LB,"[",ln,co);
        case ']': return tk_new(TK_RB,"]",ln,co); case '{': return tk_new(TK_LC,"{",ln,co);
        case '}': return tk_new(TK_RC,"}",ln,co); case ':': return tk_new(TK_COLON,":",ln,co);
        case ';': return tk_new(TK_SEMI,";",ln,co); case ',': return tk_new(TK_COMMA,",",ln,co);
        case '.': return tk_new(TK_DOT,".",ln,co); case '#': return tk_new(TK_HASH,"#",ln,co);
        case '&': return tk_new(TK_AMP,"&",ln,co); case '|': return tk_new(TK_PIPE,"|",ln,co);
        case '!': if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tk_new(TK_NEQ,"!=",ln,co); }
                  return tk_new(TK_BANG,"!",ln,co);
        case '=': if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tk_new(TK_EQEQ,"==",ln,co); }
                  return tk_new(TK_EQ,"=",ln,co);
        case '<': if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tk_new(TK_LTE,"<=",ln,co); }
                  return tk_new(TK_LT,"<",ln,co);
        case '>': if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->col++; return tk_new(TK_GTE,">=",ln,co); }
                  return tk_new(TK_GT,">",ln,co);
        default: error_at(ln, co, "unexpected char '%c' (0x%02x)", c, c);
    }
    return NULL;
}

/* Tokenize: produce flat token array with INDENT/DEDENT inserted */
static Token** tokenize(const char* src, int* count) {
    int cap = 256, len = 0;
    Token** toks = malloc(sizeof(Token*) * cap);
    Lexer lx = {src, 0, (int)strlen(src), 1, 1};

    int* istack = malloc(sizeof(int) * 64); int idepth = 0; istack[idepth++] = 0;
    int at_bol = 1;

    while (1) {
        if (at_bol) {
            int indent = 0;
            while (lx.pos < lx.len) { if (lx.src[lx.pos] == ' ') { indent++; lx.pos++; lx.col++; } else if (lx.src[lx.pos] == '\t') { indent += 4; lx.pos++; lx.col++; } else break; }
            /* skip blank lines & comment lines */
            if (lx.pos >= lx.len) break; /* EOF with no more content */
            if (lx.src[lx.pos] == '\n' || lx.src[lx.pos] == '\r') {
                if (lx.src[lx.pos] == '\r') lx.pos++;
                if (lx.pos < lx.len && lx.src[lx.pos] == '\n') lx.pos++;
                lx.line++; lx.col = 1; continue;
            }
            if (lx.src[lx.pos] == '/' && lx.pos+1 < lx.len && (lx.src[lx.pos+1] == '/' || lx.src[lx.pos+1] == '*')) {
                Token* t = lex_one(&lx); tk_free(t); at_bol = (lx.pos < lx.len && lx.src[lx.pos-1] == '\n'); continue;
            }
            /* handle indent/dedent */
            int cur = istack[idepth-1];
            if (indent > cur) { if (len >= cap) { cap *= 2; toks = realloc(toks, sizeof(Token*)*cap); } toks[len++] = tk_new(TK_INDENT,"",lx.line,lx.col); istack[idepth++] = indent; cur = indent; }
            while (indent < cur) { if (len >= cap) { cap *= 2; toks = realloc(toks, sizeof(Token*)*cap); } toks[len++] = tk_new(TK_DEDENT,"",lx.line,lx.col); idepth--; cur = istack[idepth-1]; }
            if (indent != cur) error_at(lx.line, lx.col, "inconsistent indentation");
            at_bol = 0;
        }
        Token* t = lex_one(&lx);
        if (t->type == TK_EOF) { tk_free(t); break; }
        if (t->type == TK_NL) at_bol = 1;
        if (len >= cap) { cap *= 2; toks = realloc(toks, sizeof(Token*)*cap); }
        toks[len++] = t;
    }
    /* trailing DEDENTs */
    while (idepth > 1) {
        if (len >= cap) { cap *= 2; toks = realloc(toks, sizeof(Token*)*cap); }
        toks[len++] = tk_new(TK_DEDENT,"",lx.line,lx.col); idepth--;
    }
    /* final EOF */
    if (len >= cap) { cap *= 2; toks = realloc(toks, sizeof(Token*)*cap); }
    toks[len++] = tk_new(TK_EOF,"",lx.line,lx.col);
    free(istack);
    *count = len;
    return toks;
}

/* ================================================================
 *  AST nodes
 * ================================================================ */
enum {
    N_PROG, N_VAR, N_ASSIGN, N_IF, N_WHL, N_FOR, N_FN, N_RET, N_BLOCK,
    N_PRT, N_INP, N_BINOP, N_UNOP, N_ID, N_INT, N_FLT, N_STR, N_BOOL,
    N_CALL, N_ARR, N_IDX, N_FLD, N_ARR_LEN, N_STRUCT, N_STRUCT_LIT,
};

/* type tags for data_type field */
enum { T_NONE=0, T_INT, T_FLT, T_STR, T_BOL, T_VOID, T_STRUCT };

typedef struct ASTNode {
    int kind, dtype;
    char* sval;       /* identifier name / string value / struct name */
    int64_t ival;     /* int literal */
    double fval;      /* float literal */
    struct ASTNode *a, *b, *c, *d, *next;
    int line, col;
} AST;

static AST* ast_new(int kind, int ln, int co) {
    AST* n = calloc(1, sizeof(AST)); n->kind = kind; n->dtype = T_NONE; n->line = ln; n->col = co; return n;
}
static AST* ast_id(const char* name, int ln, int co) { AST* n = ast_new(N_ID, ln, co); n->sval = strdup(name); n->dtype = T_NONE; return n; }
static AST* ast_int(int64_t v, int ln, int co) { AST* n = ast_new(N_INT, ln, co); n->ival = v; n->dtype = T_INT; return n; }
static AST* ast_flt(double v, int ln, int co) { AST* n = ast_new(N_FLT, ln, co); n->fval = v; n->dtype = T_FLT; return n; }
static AST* ast_str(const char* s, int ln, int co) { AST* n = ast_new(N_STR, ln, co); n->sval = strdup(s); n->dtype = T_STR; return n; }
static AST* ast_bool(int v, int ln, int co) { AST* n = ast_new(N_BOOL, ln, co); n->ival = v; n->dtype = T_BOL; return n; }
static AST* ast_binop(int opkind, AST* l, AST* r, int ln, int co) { AST* n = ast_new(N_BINOP, ln, co); n->ival = opkind; n->a = l; n->b = r; return n; }
static AST* ast_unop(int opkind, AST* x, int ln, int co) { AST* n = ast_new(N_UNOP, ln, co); n->ival = opkind; n->a = x; return n; }

/* ================================================================
 *  Symbol table (simple linear-scan per scope)
 * ================================================================ */
typedef struct Sym { char* name; int dtype; int is_fn; AST* fn_ast; } Sym;
typedef struct Scope { Sym* syms; int count, cap; struct Scope* parent; } Scope;

static Scope* scope_new(Scope* parent) { Scope* s = calloc(1,sizeof(Scope)); s->cap=32; s->syms=malloc(sizeof(Sym)*32); s->parent=parent; return s; }
static Sym* scope_lookup(Scope* s, const char* name) {
    for (int i = 0; i < s->count; i++) if (!strcmp(s->syms[i].name, name)) return &s->syms[i];
    return s->parent ? scope_lookup(s->parent, name) : NULL;
}
static Sym* scope_add(Scope* s, const char* name, int dtype, int is_fn) {
    Sym* existing = NULL;
    for (int i = 0; i < s->count; i++) if (!strcmp(s->syms[i].name, name)) { existing = &s->syms[i]; break; }
    if (existing) { existing->dtype = dtype; existing->is_fn = is_fn; return existing; }
    if (s->count >= s->cap) { s->cap *= 2; s->syms = realloc(s->syms, sizeof(Sym)*s->cap); }
    Sym* sym = &s->syms[s->count++]; sym->name = strdup(name); sym->dtype = dtype; sym->is_fn = is_fn; sym->fn_ast = NULL;
    return sym;
}

/* ================================================================
 *  Parser (recursive descent)
 * ================================================================ */
typedef struct {
    Token** toks; int pos, count;
    Scope *global, *local;
} Parser;

static Token* peek(Parser* p) { return p->pos < p->count ? p->toks[p->pos] : NULL; }
static Token* next(Parser* p) { return p->pos+1 < p->count ? p->toks[p->pos+1] : NULL; }
static Token* consume(Parser* p) { return p->pos < p->count ? p->toks[p->pos++] : NULL; }
static int check(Parser* p, int t) { Token* tk = peek(p); return tk && tk->type == t; }
static int check_kw_type(Parser* p) { Token* tk = peek(p); return tk && (tk->type == KW_INT||tk->type==KW_FLT||tk->type==KW_STR||tk->type==KW_BOL); }
static Token* expect(Parser* p, int t) { if (check(p, t)) return consume(p); Token* tk = peek(p); error_at(tk?tk->line:0, tk?tk->col:0, "expected %s, got %s", tk_names[t], tk?tk_names[tk->type]:"?"); return NULL; }

/* Forward declarations */
static AST* parse_expr(Parser* p);
static AST* parse_statement(Parser* p);
static AST* parse_block(Parser* p);

static int type_kw_to_dtype(int kw) {
    switch (kw) { case KW_INT: return T_INT; case KW_FLT: return T_FLT; case KW_STR: return T_STR; case KW_BOL: return T_BOL; default: return T_INT; }
}

static const char* dtype_to_cstr(int dt) {
    switch (dt) { case T_INT: return "int64_t"; case T_FLT: return "double"; case T_STR: return "const char*"; case T_BOL: return "int"; case T_VOID: return "void"; default: return "int64_t"; }
}

/* Determine result type of binary operation */
static int binop_dtype(int a, int b, int op) {
    if (op == N_BINOP && 0) {} /* placeholder */
    switch (op) {
        case TK_PLUS: if (a==T_STR||b==T_STR) return T_STR; if (a==T_FLT||b==T_FLT) return T_FLT; return T_INT;
        case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PCT:
            if (a==T_FLT||b==T_FLT) return T_FLT; return T_INT;
        case TK_EQEQ: case TK_NEQ: case TK_LT: case TK_GT: case TK_LTE: case TK_GTE: return T_BOL;
        case TK_AMP: case TK_PIPE: return T_BOL;
        default: return T_INT;
    }
}

/* ---- Expression parser ---- */
static AST* parse_primary(Parser* p) {
    Token* tk = peek(p);
    if (!tk) error_at(0,0,"unexpected EOF");

    if (tk->type == TK_INT) { consume(p); return ast_int(strtoll(tk->text,NULL,10), tk->line, tk->col); }
    if (tk->type == TK_FLT) { consume(p); return ast_flt(strtod(tk->text,NULL), tk->line, tk->col); }
    if (tk->type == TK_STR) { consume(p); return ast_str(tk->text, tk->line, tk->col); }
    if (tk->type == KW_TRU) { consume(p); return ast_bool(1, tk->line, tk->col); }
    if (tk->type == KW_FAL) { consume(p); return ast_bool(0, tk->line, tk->col); }

    if (tk->type == TK_HASH) { consume(p); Token* id = expect(p, TK_IDENT); AST* n = ast_new(N_ARR_LEN, id->line, id->col); n->sval = strdup(id->text);
        Sym* s = scope_lookup(p->local, id->text); if (!s) error_at(id->line, id->col, "undefined '%s'", id->text); n->dtype = T_INT; return n; }

    if (tk->type == TK_IDENT || tk->type == KW_PRT || tk->type == KW_INP) { char* name = strdup(tk->text); consume(p);
        if (check(p, TK_LP)) { /* function call */
            consume(p); AST* n = ast_new(N_CALL, tk->line, tk->col); n->sval = name;
            if (!check(p, TK_RP)) {
                AST** tail = &n->a;
                do { *tail = parse_expr(p); tail = &(*tail)->next; } while (check(p, TK_COMMA) && (consume(p),1));
            }
            expect(p, TK_RP);
            Sym* s = scope_lookup(p->global, name); n->dtype = s ? s->dtype : T_INT;
            return n;
        }
        /* check for struct literal: Name{ ... } */
        if (check(p, TK_LC)) {
            consume(p); AST* n = ast_new(N_STRUCT_LIT, tk->line, tk->col); n->sval = name;
            if (!check(p, TK_RC)) {
                AST** tail = &n->a;
                do {
                    Token* fn = expect(p, TK_IDENT); expect(p, TK_COLON);
                    AST* fld = ast_new(N_FLD, fn->line, fn->col); fld->sval = strdup(fn->text); fld->a = parse_expr(p);
                    *tail = fld; tail = &fld->next;
                } while (check(p, TK_COMMA) && (consume(p),1));
            }
            expect(p, TK_RC); n->dtype = T_STRUCT; return n;
        }
        /* plain identifier */
        AST* idn = ast_id(name, tk->line, tk->col);
        Sym* s = scope_lookup(p->local, name); if (!s) s = scope_lookup(p->global, name);
        if (s) idn->dtype = s->dtype; else error_at(tk->line, tk->col, "undefined variable '%s'", name);
        free(name); return idn;
    }

    if (tk->type == TK_LP) { consume(p); AST* e = parse_expr(p); expect(p, TK_RP); return e; }

    if (tk->type == TK_LB) { consume(p); AST* n = ast_new(N_ARR, tk->line, tk->col);
        if (!check(p, TK_RB)) { AST** tail = &n->a; do { *tail = parse_expr(p); tail = &(*tail)->next; } while (check(p, TK_COMMA) && (consume(p),1)); }
        expect(p, TK_RB); n->dtype = T_INT; return n; }

    if (tk->type == KW_INP) { consume(p); AST* n = ast_new(N_INP, tk->line, tk->col);
        expect(p, TK_LP); if (check(p, TK_STR)) n->a = ast_str(consume(p)->text, tk->line, tk->col);
        expect(p, TK_RP); n->dtype = T_INT; return n; }

    error_at(tk->line, tk->col, "unexpected token '%s'", tk_names[tk->type]);
    return NULL;
}

static AST* parse_postfix(Parser* p) {
    AST* e = parse_primary(p);
    while (1) {
        if (check(p, TK_LB)) { consume(p); AST* idx = parse_expr(p); expect(p, TK_RB); AST* n = ast_new(N_IDX, e->line, e->col); n->a = e; n->b = idx; n->dtype = T_INT; e = n; }
        else if (check(p, TK_DOT)) { consume(p); Token* fld = expect(p, TK_IDENT); AST* n = ast_new(N_FLD, e->line, e->col); n->sval = strdup(fld->text); n->a = e; n->dtype = T_INT; e = n; }
        else break;
    }
    return e;
}

static AST* parse_unary(Parser* p) {
    Token* tk = peek(p);
    if (tk && (tk->type == TK_MINUS || tk->type == TK_BANG)) {
        int op = consume(p)->type; AST* x = parse_unary(p);
        AST* n = ast_unop(op == TK_MINUS ? TK_MINUS : TK_BANG, x, tk->line, tk->col);
        n->dtype = (op == TK_MINUS) ? x->dtype : T_BOL; return n;
    }
    return parse_postfix(p);
}

static AST* parse_mul(Parser* p) { AST* e = parse_unary(p); while (check(p,TK_STAR)||check(p,TK_SLASH)||check(p,TK_PCT)) { int op = consume(p)->type; AST* r = parse_unary(p); AST* n = ast_binop(op,e,r,e->line,e->col); n->dtype = binop_dtype(e->dtype,r->dtype,op); e = n; } return e; }
static AST* parse_add(Parser* p) { AST* e = parse_mul(p); while (check(p,TK_PLUS)||check(p,TK_MINUS)) { int op = consume(p)->type; AST* r = parse_mul(p); AST* n = ast_binop(op,e,r,e->line,e->col); n->dtype = binop_dtype(e->dtype,r->dtype,op); e = n; } return e; }
static AST* parse_rel(Parser* p) { AST* e = parse_add(p); while (check(p,TK_LT)||check(p,TK_GT)||check(p,TK_LTE)||check(p,TK_GTE)) { int op = consume(p)->type; AST* r = parse_add(p); AST* n = ast_binop(op,e,r,e->line,e->col); n->dtype = T_BOL; e = n; } return e; }
static AST* parse_eq(Parser* p) { AST* e = parse_rel(p); while (check(p,TK_EQEQ)||check(p,TK_NEQ)) { int op = consume(p)->type; AST* r = parse_rel(p); AST* n = ast_binop(op,e,r,e->line,e->col); n->dtype = T_BOL; e = n; } return e; }
static AST* parse_and(Parser* p) { AST* e = parse_eq(p); while (check(p,TK_AMP)) { consume(p); AST* r = parse_eq(p); AST* n = ast_binop(TK_AMP,e,r,e->line,e->col); n->dtype = T_BOL; e = n; } return e; }
static AST* parse_or(Parser* p) { AST* e = parse_and(p); while (check(p,TK_PIPE)) { consume(p); AST* r = parse_and(p); AST* n = ast_binop(TK_PIPE,e,r,e->line,e->col); n->dtype = T_BOL; e = n; } return e; }
static AST* parse_expr(Parser* p) { return parse_or(p); }

/* ---- Statement parser ---- */
static AST* parse_type_annot(Parser* p) {
    if (check_kw_type(p)) return ast_int(type_kw_to_dtype(consume(p)->type), 0, 0);
    if (check(p, TK_IDENT)) { AST* n = ast_new(N_ID, peek(p)->line, peek(p)->col); n->sval = strdup(consume(p)->text); n->dtype = T_STRUCT; return n; }
    return NULL;
}

static AST* parse_var_decl(Parser* p) {
    Token* lt = expect(p, KW_LET); Token* nm = expect(p, TK_IDENT);
    AST* ta = NULL;
    if (check(p, TK_COLON)) { consume(p); ta = parse_type_annot(p); }
    expect(p, TK_EQ);
    AST* init = parse_expr(p);
    AST* n = ast_new(N_VAR, lt->line, lt->col); n->sval = strdup(nm->text); n->a = init; n->b = ta;
    int dt = ta ? (ta->kind == N_ID ? T_STRUCT : (int)ta->ival) : init->dtype;
    n->dtype = dt;
    scope_add(p->local, nm->text, dt, 0);
    return n;
}

static AST* parse_assignment(Parser* p, AST* lv) {
    AST* n = ast_new(N_ASSIGN, lv->line, lv->col); n->a = lv; n->b = parse_expr(p);
    n->dtype = lv->dtype; return n;
}

static AST* parse_if(Parser* p) {
    Token* tk = consume(p); /* if */
    AST* cond = parse_expr(p);
    AST* n = ast_new(N_IF, tk->line, tk->col); n->a = cond; n->b = parse_block(p);
    AST** tail = &n->c;
    while (check(p, KW_ELS)) {
        Token* ek = consume(p);
        if (check(p, KW_IF)) {
            consume(p); AST* eif = ast_new(N_IF, ek->line, ek->col); eif->a = parse_expr(p); eif->b = parse_block(p);
            *tail = eif; tail = &eif->c;
        } else {
            *tail = parse_block(p); break;
        }
    }
    return n;
}

static AST* parse_while(Parser* p) {
    Token* tk = consume(p); AST* cond = parse_expr(p);
    AST* n = ast_new(N_WHL, tk->line, tk->col); n->a = cond; n->b = parse_block(p); return n;
}

static AST* parse_for(Parser* p) {
    Token* tk = consume(p); /* for */
    /* Create a temporary scope for loop variable */
    AST* init = NULL;
    if (check(p, KW_LET)) init = parse_var_decl(p);
    else { Token* id = expect(p, TK_IDENT); expect(p, TK_EQ); AST* lv = ast_id(id->text, id->line, id->col); init = parse_assignment(p, lv); }
    expect(p, TK_SEMI);
    AST* cond = parse_expr(p);
    expect(p, TK_SEMI);
    Token* uid = expect(p, TK_IDENT); expect(p, TK_EQ);
    AST* lv2 = ast_id(uid->text, uid->line, uid->col); AST* upd = parse_assignment(p, lv2);
    AST* n = ast_new(N_FOR, tk->line, tk->col); n->a = init; n->b = cond; n->c = upd; n->d = parse_block(p); return n;
}

static AST* parse_fn_def(Parser* p) {
    Token* tk = consume(p); /* fn */
    Token* nm = expect(p, TK_IDENT);
    expect(p, TK_LP);
    AST* fn = ast_new(N_FN, tk->line, tk->col); fn->sval = strdup(nm->text);
    /* collect params into a linked list */
    AST** ptail = &fn->a;
    if (!check(p, TK_RP)) {
        do {
            Token* pn = expect(p, TK_IDENT);
            AST* pa = ast_new(N_VAR, pn->line, pn->col); pa->sval = strdup(pn->text); pa->dtype = T_INT;
            if (check(p, TK_COLON)) { consume(p); AST* ta = parse_type_annot(p); if (ta) pa->dtype = (ta->kind == N_ID ? T_STRUCT : (int)ta->ival); }
            scope_add(p->local, pn->text, pa->dtype, 0);
            *ptail = pa; ptail = &pa->next;
        } while (check(p, TK_COMMA) && (consume(p),1));
    }
    expect(p, TK_RP);
    /* return type annotation */
    fn->b = NULL;
    if (check(p, TK_COLON) && !check(p, TK_NL) && !(next(p) && next(p)->type == TK_NL)) {
        /* this colon is return type, not block start */
        if (peek(p)->type == TK_COLON) {
            Token* nx = next(p);
            if (nx && check_kw_type(p) == 0 && nx->type != TK_NL) { /* actually let me simplify */ }
        }
    }
    if (check(p, TK_COLON)) {
        Token* col = peek(p);
        Token* nx = next(p);
        /* If the colon is followed by a type keyword (not NL/INDENT), it's a return type annotation */
        if (nx && (nx->type == KW_INT || nx->type == KW_FLT || nx->type == KW_STR || nx->type == KW_BOL || nx->type == KW_VOID)) {
            consume(p); /* colon */
            AST* rta = parse_type_annot(p);
            if (rta) fn->b = rta;
        }
    }
    fn->dtype = fn->b ? (fn->b->kind == N_ID ? T_STRUCT : (int)fn->b->ival) : T_INT;
    /* register in global scope */
    scope_add(p->global, nm->text, fn->dtype, 1);
    /* parse body */
    fn->c = parse_block(p);
    return fn;
}

static AST* parse_return(Parser* p) {
    Token* tk = consume(p); AST* n = ast_new(N_RET, tk->line, tk->col);
    if (!check(p, TK_NL) && !check(p, TK_DEDENT) && !check(p, TK_EOF)) n->a = parse_expr(p);
    return n;
}

static AST* parse_struct(Parser* p) {
    Token* tk = consume(p); /* typ */
    Token* nm = expect(p, TK_IDENT);
    expect(p, TK_COLON);
    AST* n = ast_new(N_STRUCT, tk->line, tk->col); n->sval = strdup(nm->text);
    scope_add(p->global, nm->text, T_STRUCT, 0);
    /* parse fields */
    expect(p, TK_NL); expect(p, TK_INDENT);
    AST** tail = &n->a;
    while (!check(p, TK_DEDENT) && !check(p, TK_EOF)) {
        Token* fnm = expect(p, TK_IDENT); expect(p, TK_COLON);
        AST* fld = ast_new(N_VAR, fnm->line, fnm->col); fld->sval = strdup(fnm->text);
        AST* ta = parse_type_annot(p); fld->b = ta; fld->dtype = ta ? (ta->kind==N_ID?T_STRUCT:(int)ta->ival) : T_INT;
        *tail = fld; tail = &fld->next;
        if (check(p, TK_NL)) consume(p); else if (!check(p, TK_DEDENT)) error_at(fnm->line, fnm->col, "expected newline in struct body");
    }
    expect(p, TK_DEDENT);
    return n;
}

static AST* parse_block(Parser* p) {
    /* After a ':', we may have a single-line body or an indented block */
    if (!check(p, TK_COLON)) error_at(peek(p)->line, peek(p)->col, "expected ':' before block");

    /* Check for single-line block: colon followed by non-NEWLINE (expression on same line) */
    Token* col = consume(p);

    AST* block = ast_new(N_BLOCK, col->line, col->col);

    if (check(p, TK_NL)) {
        consume(p); /* newline */
        expect(p, TK_INDENT);
        AST** tail = &block->a;
        while (!check(p, TK_DEDENT) && !check(p, TK_EOF)) {
            *tail = parse_statement(p);
            /* move to next after the statement */
            while (*tail) { tail = &(*tail)->next; (void)0; } /* find end */
            /* actually we need to link properly */
            if (!block->a) block->a = *tail;
            else { AST* cur = block->a; while (cur->next) cur = cur->next; cur->next = *tail; }
            /* also consume optional trailing newline */
            if (check(p, TK_NL)) consume(p);
        }
        expect(p, TK_DEDENT);
    } else {
        /* single-line block */
        block->a = parse_statement(p);
    }
    return block;
}

static AST* parse_statement(Parser* p) {
    Token* tk = peek(p);
    if (!tk || tk->type == TK_DEDENT || tk->type == TK_EOF) return NULL;

    if (tk->type == KW_LET) return parse_var_decl(p);
    if (tk->type == KW_IF) return parse_if(p);
    if (tk->type == KW_WHL) return parse_while(p);
    if (tk->type == KW_FOR) return parse_for(p);
    if (tk->type == KW_FN) return parse_fn_def(p);
    if (tk->type == KW_RET) return parse_return(p);
    if (tk->type == KW_TYP) return parse_struct(p);

    /* must be expression statement or assignment */
    AST* e = parse_expr(p);
    if (check(p, TK_EQ)) { consume(p); return parse_assignment(p, e); }
    /* expression statement wraps expression */
    AST* st = ast_new(N_PRT, tk->line, tk->col); st->dtype = -1; st->a = e;
    /* If it's already a prt call, return e directly */
    if (e->kind == N_CALL && e->sval && !strcmp(e->sval, "prt")) { st->a = e; st->kind = N_PRT; st->dtype = e->dtype; return st; }
    /* wrap as expr stmt */
    return st;
}

static AST* parse_program(Parser* p) {
    AST* prog = ast_new(N_PROG, 0, 0);
    AST** tail = &prog->a;
    while (p->pos < p->count && p->toks[p->pos]->type != TK_EOF) {
        AST* st = parse_statement(p);
        if (st) { *tail = st; tail = &st->next; }
        if (check(p, TK_NL)) consume(p);
    }
    return prog;
}

static AST* parse(Token** toks, int count) {
    Parser p = {toks, 0, count, NULL, NULL};
    p.global = scope_new(NULL);
    p.local = p.global;

    /* First pass: collect function names and struct names so mutual recursion works */
    int saved = p.pos;
    while (p.pos < count) {
        Token* tk = p.toks[p.pos];
        if (tk->type == KW_FN) {
            p.pos++;
            Token* nm = expect(&p, TK_IDENT);
            scope_add(p.global, nm->text, T_INT, 1); /* placeholder */
            /* skip to next top-level statement */
            int depth = 0;
            while (p.pos < count) {
                if (p.toks[p.pos]->type == TK_COLON) { /* skip block */ break; }
                p.pos++;
            }
            if (p.pos < count) p.pos++; /* skip colon */
            /* skip indented block */
        } else if (tk->type == KW_TYP) {
            p.pos++;
            Token* nm = expect(&p, TK_IDENT);
            scope_add(p.global, nm->text, T_STRUCT, 0);
            while (p.pos < count && p.toks[p.pos]->type != TK_DEDENT && p.toks[p.pos]->type != TK_EOF) p.pos++;
        } else {
            p.pos++;
        }
    }
    p.pos = saved;
    p.local = scope_new(p.global);

    AST* prog = parse_program(&p);
    free(p.local->syms); free(p.local);
    return prog;
}

/* ================================================================
 *  C Code generator
 * ================================================================ */
static void gen_expr(DStr* out, AST* n);
static void gen_stmt(DStr* out, AST* n, int indent);

static void gen_indent(DStr* out, int n) { for (int i = 0; i < n; i++) dstr_add(out, "    "); }

static const char* op_to_c(int op) {
    switch (op) { case TK_PLUS: return "+"; case TK_MINUS: return "-"; case TK_STAR: return "*"; case TK_SLASH: return "/";
    case TK_PCT: return "%"; case TK_EQEQ: return "=="; case TK_NEQ: return "!="; case TK_LT: return "<"; case TK_GT: return ">";
    case TK_LTE: return "<="; case TK_GTE: return ">="; case TK_AMP: return "&&"; case TK_PIPE: return "||"; default: return "?"; }
}

static void gen_expr(DStr* out, AST* n) {
    if (!n) { dstr_add(out, "0"); return; }
    switch (n->kind) {
        case N_INT: dstr_addi(out, n->ival); break;
        case N_FLT: dstr_addd(out, n->fval); break;
        case N_STR: dstr_addc(out, '"'); dstr_add(out, n->sval); dstr_addc(out, '"'); break;
        case N_BOOL: dstr_add(out, n->ival ? "1" : "0"); break;
        case N_ID: dstr_add(out, n->sval); break;
        case N_BINOP:
            dstr_addc(out, '('); gen_expr(out, n->a);
            dstr_addc(out, ' '); dstr_add(out, op_to_c((int)n->ival)); dstr_addc(out, ' ');
            gen_expr(out, n->b); dstr_addc(out, ')'); break;
        case N_UNOP:
            if (n->ival == TK_BANG) { dstr_add(out, "!("); gen_expr(out, n->a); dstr_addc(out, ')'); }
            else { dstr_add(out, "-("); gen_expr(out, n->a); dstr_addc(out, ')'); }
            break;
        case N_CALL:
            dstr_add(out, n->sval); dstr_addc(out, '(');
            { AST* a = n->a; int first = 1; while (a) { if (!first) dstr_add(out, ", "); gen_expr(out, a); a = a->next; first = 0; } }
            dstr_addc(out, ')'); break;
        case N_IDX: gen_expr(out, n->a); dstr_addc(out, '['); gen_expr(out, n->b); dstr_addc(out, ']'); break;
        case N_FLD: gen_expr(out, n->a); dstr_addc(out, '.'); dstr_add(out, n->sval); break;
        case N_ARR_LEN:
            dstr_add(out, "(sizeof("); dstr_add(out, n->sval); dstr_add(out, ") / sizeof("); dstr_add(out, n->sval); dstr_add(out, "[0]))"); break;
        case N_INP:
            if (n->a) { dstr_add(out, "dsc_inp_int("); gen_expr(out, n->a); dstr_addc(out, ')'); }
            else dstr_add(out, "dsc_inp_int(NULL)"); break;
        case N_STRUCT_LIT:
            dstr_add(out, "("); dstr_add(out, n->sval); dstr_add(out, "){");
            { AST* f = n->a; int first = 1; while (f) { if (!first) dstr_add(out, ", "); gen_expr(out, f->a); f = f->next; first = 0; } }
            dstr_addc(out, '}'); break;
        case N_ARR:
            dstr_addc(out, '{');
            { AST* e = n->a; int first = 1; while (e) { if (!first) dstr_add(out, ", "); gen_expr(out, e); e = e->next; first = 0; } }
            dstr_addc(out, '}'); break;
        default: dstr_add(out, "/*expr*/"); break;
    }
}

static void gen_prt(DStr* out, AST* n, int indent) {
    AST* arg = n->kind == N_PRT ? n->a : n;
    /* If arg is N_CALL with name prt, unwrap */
    if (arg && arg->kind == N_CALL && arg->sval && !strcmp(arg->sval, "prt")) {
        arg = arg->a; /* first argument */
    }
    if (!arg) return;
    gen_indent(out, indent);
    switch (arg->dtype) {
        case T_INT: dstr_add(out, "dsc_prt_int("); gen_expr(out, arg); dstr_add(out, ");\n"); break;
        case T_FLT: dstr_add(out, "dsc_prt_flt("); gen_expr(out, arg); dstr_add(out, ");\n"); break;
        case T_STR: dstr_add(out, "dsc_prt_str("); gen_expr(out, arg); dstr_add(out, ");\n"); break;
        case T_BOL: dstr_add(out, "dsc_prt_bol("); gen_expr(out, arg); dstr_add(out, ");\n"); break;
        default: dstr_add(out, "dsc_prt_int("); gen_expr(out, arg); dstr_add(out, ");\n"); break;
    }
}

static void gen_stmt(DStr* out, AST* n, int indent) {
    if (!n) return;
    switch (n->kind) {
        case N_VAR:
            gen_indent(out, indent);
            if (n->b && n->b->kind == N_ID) dstr_add(out, n->b->sval);
            else if (n->b) dstr_add(out, dtype_to_cstr((int)n->b->ival));
            else if (n->dtype == T_STRUCT && n->a && n->a->kind == N_STRUCT_LIT) dstr_add(out, n->a->sval);
            else if (n->dtype == T_STR) dstr_add(out, "const char*");
            else dstr_add(out, dtype_to_cstr(n->dtype));
            dstr_addc(out, ' '); dstr_add(out, n->sval);
            if (n->a && n->a->kind == N_ARR) dstr_add(out, "[]");
            dstr_add(out, " = "); gen_expr(out, n->a); dstr_add(out, ";\n"); break;
        case N_ASSIGN:
            gen_indent(out, indent); gen_expr(out, n->a); dstr_add(out, " = "); gen_expr(out, n->b); dstr_add(out, ";\n"); break;
        case N_IF:
            gen_indent(out, indent); dstr_add(out, "if ("); gen_expr(out, n->a); dstr_add(out, ") {\n");
            if (n->b) { for (AST* s = (n->b->kind == N_BLOCK ? n->b->a : n->b); s; s = s->next) gen_stmt(out, s, indent+1); }
            gen_indent(out, indent); dstr_add(out, "}");
            if (n->c) {
                if (n->c->kind == N_IF) { dstr_add(out, " else "); gen_stmt(out, n->c, indent); }
                else { dstr_add(out, " else {\n"); for (AST* s = (n->c->kind == N_BLOCK ? n->c->a : n->c); s; s = s->next) gen_stmt(out, s, indent+1);
                    gen_indent(out, indent); dstr_add(out, "}"); }
            }
            dstr_add(out, "\n"); break;
        case N_WHL:
            gen_indent(out, indent); dstr_add(out, "while ("); gen_expr(out, n->a); dstr_add(out, ") {\n");
            if (n->b) { for (AST* s = (n->b->kind == N_BLOCK ? n->b->a : n->b); s; s = s->next) gen_stmt(out, s, indent+1); }
            gen_indent(out, indent); dstr_add(out, "}\n"); break;
        case N_FOR:
            gen_indent(out, indent); dstr_add(out, "for (");
            /* init */
            if (n->a) {
                AST* init = n->a;
                if (init->kind == N_VAR) {
                    if (init->b) dstr_add(out, dtype_to_cstr((int)init->b->ival)); else dstr_add(out, dtype_to_cstr(init->dtype));
                    dstr_addc(out, ' '); dstr_add(out, init->sval); dstr_add(out, " = "); gen_expr(out, init->a);
                } else if (init->kind == N_ASSIGN) {
                    gen_expr(out, init->a); dstr_add(out, " = "); gen_expr(out, init->b);
                }
            }
            dstr_add(out, "; "); gen_expr(out, n->b); /* condition */
            dstr_add(out, "; ");
            if (n->c) { AST* upd = n->c; gen_expr(out, upd->a); dstr_add(out, " = "); gen_expr(out, upd->b); }
            dstr_add(out, ") {\n");
            if (n->d) { for (AST* s = (n->d->kind == N_BLOCK ? n->d->a : n->d); s; s = s->next) gen_stmt(out, s, indent+1); }
            gen_indent(out, indent); dstr_add(out, "}\n"); break;
        case N_RET:
            gen_indent(out, indent); if (n->a) { dstr_add(out, "return "); gen_expr(out, n->a); dstr_add(out, ";\n"); } else dstr_add(out, "return 0;\n"); break;
        case N_PRT: gen_prt(out, n, indent); break;
        case N_STRUCT:
            gen_indent(out, indent); dstr_add(out, "typedef struct {\n");
            for (AST* f = n->a; f; f = f->next) {
                gen_indent(out, indent+1);
                if (f->dtype == T_STRUCT && f->b && f->b->kind == N_ID) dstr_add(out, f->b->sval);
                else dstr_add(out, dtype_to_cstr(f->dtype));
                dstr_addc(out, ' '); dstr_add(out, f->sval); dstr_add(out, ";\n");
            }
            gen_indent(out, indent); dstr_add(out, "} "); dstr_add(out, n->sval); dstr_add(out, ";\n"); break;
        default:
            /* expression statement (unwrapped) */
            if (n->kind == N_CALL) { gen_indent(out, indent); gen_expr(out, n); dstr_add(out, ";\n"); }
            else { gen_indent(out, indent); dstr_add(out, "/* expr */ "); gen_expr(out, n); dstr_add(out, ";\n"); }
            break;
    }
}

static void gen_fn(DStr* out, AST* n) {
    /* signature: ret_type name(params) */
    if (n->b && n->b->kind == N_ID) dstr_add(out, n->b->sval);
    else if (n->b) dstr_add(out, dtype_to_cstr((int)n->b->ival));
    else if (n->dtype == T_STRUCT) dstr_add(out, "int64_t"); /* fallback */
    else dstr_add(out, dtype_to_cstr(n->dtype));
    dstr_addc(out, ' '); dstr_add(out, n->sval); dstr_addc(out, '(');
    AST* pa = n->a; int first = 1;
    while (pa) { if (!first) dstr_add(out, ", "); dstr_add(out, dtype_to_cstr(pa->dtype)); dstr_addc(out, ' '); dstr_add(out, pa->sval); pa = pa->next; first = 0; }
    dstr_add(out, ") {\n");
    if (n->c) {
        AST* body = n->c->kind == N_BLOCK ? n->c->a : n->c;
        for (AST* s = body; s; s = s->next) gen_stmt(out, s, 1);
    }
    dstr_add(out, "    return 0;\n}\n\n");
}

static char* generate_c(AST* prog, const char* src_name) {
    DStr* out = dstr_new();
    dstr_add(out, "/* Generated by DSCBT compiler from "); dstr_add(out, src_name); dstr_add(out, " */\n");
    dstr_add(out, "#include <stdio.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n\n");

    /* Helper functions */
    dstr_add(out, "static void dsc_prt_int(int64_t v) { printf(\"%lld\\n\", (long long)v); }\n");
    dstr_add(out, "static void dsc_prt_flt(double v) { printf(\"%f\\n\", v); }\n");
    dstr_add(out, "static void dsc_prt_str(const char* v) { printf(\"%s\\n\", v); }\n");
    dstr_add(out, "static void dsc_prt_bol(int v) { printf(\"%s\\n\", v ? \"tru\" : \"fal\"); }\n");
    dstr_add(out, "static int64_t dsc_inp_int(const char* p) { if(p) printf(\"%s\",p); int64_t v; scanf(\"%lld\",(long long*)&v); return v; }\n");
    dstr_add(out, "static char* dsc_str_cat(const char* a, const char* b) { char* r=malloc(strlen(a)+strlen(b)+1); strcpy(r,a); strcat(r,b); return r; }\n\n");

    /* Forward declarations */
    int has_fwd = 0;
    for (AST* s = prog->a; s; s = s->next) {
        if (s->kind == N_FN) { dstr_add(out, dtype_to_cstr(s->dtype)); dstr_addc(out, ' '); dstr_add(out, s->sval); dstr_addc(out, '(');
            AST* pa = s->a; int f = 1;
            while (pa) { if (!f) dstr_add(out, ", "); dstr_add(out, dtype_to_cstr(pa->dtype)); dstr_addc(out, ' '); dstr_add(out, pa->sval); pa = pa->next; f = 0; }
            dstr_add(out, ");\n"); has_fwd = 1; }
    }
    if (has_fwd) dstr_add(out, "\n");

    /* Struct definitions */
    for (AST* s = prog->a; s; s = s->next) if (s->kind == N_STRUCT) gen_stmt(out, s, 0);
    if (has_fwd) dstr_add(out, "\n");

    /* Function implementations */
    for (AST* s = prog->a; s; s = s->next) if (s->kind == N_FN) gen_fn(out, s);

    /* Main function */
    dstr_add(out, "int main(int argc, char** argv) {\n    (void)argc; (void)argv;\n");
    for (AST* s = prog->a; s; s = s->next) { if (s->kind != N_FN && s->kind != N_STRUCT) gen_stmt(out, s, 1); }
    dstr_add(out, "    return 0;\n}\n");

    char* result = strdup(out->data); dstr_free(out); return result;
}

/* ================================================================
 *  Compiler driver
 * ================================================================ */
static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = malloc(sz + 1); fread(buf, 1, sz, f); buf[sz] = 0; fclose(f); return buf;
}

static int file_exists(const char* path) { struct stat st; return stat(path, &st) == 0; }

static void compile_dscbt(const char* src_file, const char* out_exe) {
    char* src = read_file(src_file);
    if (!src) { fprintf(stderr, "[错误] 无法读取文件: %s\n", src_file); exit(1); }

    printf("[信息] 词法分析...\n");
    int tc; Token** toks = tokenize(src, &tc);

    printf("[信息] 语法分析...\n");
    AST* prog = parse(toks, tc);

    printf("[信息] 生成C代码...\n");
    char* c_code = generate_c(prog, src_file);

    /* write temp C file */
    char c_file[1024], exe_file[1024];
    /* output name */
    if (out_exe) strcpy(exe_file, out_exe);
    else { strcpy(exe_file, src_file); char* dot = strrchr(exe_file, '.'); if (dot) *dot = 0; strcat(exe_file, ".exe"); }
    /* temp C file alongside source */
    strcpy(c_file, src_file); char* dot = strrchr(c_file, '.'); if (dot) *dot = 0; strcat(c_file, "_gen.c");

    FILE* cf = fopen(c_file, "w");
    if (!cf) { fprintf(stderr, "[错误] 无法写入文件: %s\n", c_file); exit(1); }
    fputs(c_code, cf); fclose(cf);

    printf("[信息] 编译C代码 -> %s\n", exe_file);
    /* try gcc first, then clang */
    char cmd[2048];
    int ret;
    snprintf(cmd, sizeof(cmd), "gcc -O2 -Wall -Wno-parentheses -o \"%s\" \"%s\" 2>&1", exe_file, c_file);
    printf("[信息] 执行: gcc -O2 -o \"%s\" \"%s\"\n", exe_file, c_file);
    ret = system(cmd);
    if (ret != 0) {
        snprintf(cmd, sizeof(cmd), "clang -O2 -Wall -Wno-parentheses -o \"%s\" \"%s\" 2>&1", exe_file, c_file);
        printf("[信息] 执行: clang -O2 -o \"%s\" \"%s\"\n", exe_file, c_file);
        ret = system(cmd);
    }
    if (ret != 0) {
        fprintf(stderr, "[错误] C编译失败。请确保已安装 MinGW (gcc) 或 Clang。\n");
        fprintf(stderr, "[提示] 生成的C代码保留在: %s\n", c_file);
        exit(1);
    }

    printf("[信息] 成功! 输出: %s\n", exe_file);
    printf("[信息] 中间C代码: %s\n", c_file);
    /* clean up temp C file */
    remove(c_file);

    /* cleanup */
    for (int i = 0; i < tc; i++) tk_free(toks[i]); free(toks);
    free(c_code); free(src);
    /* AST cleanup (simple) */
}

int main(int argc, char** argv) {
    fprintf(stderr, "DSCBT \347\274\226\350\257\221\345\231\250 v1.0 (DSC)\n");
    if (argc < 2) {
        fprintf(stderr, "\347\224\250\346\263\225: dsc <\346\272\220\346\226\207\344\273\266.dscbt> [\350\276\223\345\207\272.exe]\n");
        return 1;
    }
    compile_dscbt(argv[1], argc > 2 ? argv[2] : NULL);
    return 0;
}
