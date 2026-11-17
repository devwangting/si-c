#include "si.h"
#include "index.h"

#include <ctype.h>

typedef struct {
    int caller;
    int callee;
    int file_index;
    int line;
    int col;
    bool is_reference;
} CallEdge;

struct CallGraph {
    CallEdge *items;
    size_t count;
    size_t cap;
};

static void callgraph_free(CallGraph *g);
static void callgraph_add(CallGraph *g, int caller, int callee, int fi,
                          int line, int col, bool is_reference);

/* ------------------------------------------------------------------ */
/* Lists                                                               */
/* ------------------------------------------------------------------ */

void intlist_init(IntList *l)
{
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void intlist_free(IntList *l)
{
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void intlist_clear(IntList *l)
{
    l->count = 0;
}

void intlist_push(IntList *l, int value)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = xrealloc(l->items, l->cap * sizeof(*l->items));
    }
    l->items[l->count++] = value;
}

void loclist_init(LocationList *l)
{
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void loclist_free(LocationList *l)
{
    size_t i;
    for (i = 0; i < l->count; i++)
        free(l->items[i].preview);
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void loclist_clear(LocationList *l)
{
    size_t i;
    for (i = 0; i < l->count; i++)
        free(l->items[i].preview);
    l->count = 0;
}

void loclist_push_depth(LocationList *l, int file_index, int line, int col,
                        int depth, const char *preview)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = xrealloc(l->items, l->cap * sizeof(*l->items));
    }
    l->items[l->count].file_index = file_index;
    l->items[l->count].line = line;
    l->items[l->count].col = col;
    l->items[l->count].depth = depth;
    l->items[l->count].preview = xstrdup(preview ? preview : "");
    l->count++;
}

void loclist_push(LocationList *l, int file_index, int line, int col,
                  const char *preview)
{
    loclist_push_depth(l, file_index, line, col, 0, preview);
}

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    TK_CHAR,
    TK_PREPROC,
    TK_PUNCT
} TokKind;

typedef struct {
    TokKind kind;
    int start;
    int len;
    int line;
    int col;
    int line_start;
} Tok;

typedef struct {
    const char *src;
    int len;
    Tok *toks;
    int count;
    int cap;
    int line;
    int line_start;
} Lexer;

static bool ident_start(unsigned char c)
{
    return isalpha(c) || c == '_' || c == '$';
}

static bool ident_char(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '$';
}

static void lex_push(Lexer *lx, TokKind kind, int start, int end, int line,
                     int line_start)
{
    Tok *t;
    if (lx->count >= 2000000)
        return;
    if (lx->count == lx->cap) {
        lx->cap = lx->cap ? lx->cap * 2 : 1024;
        lx->toks = xrealloc(lx->toks, (size_t)lx->cap * sizeof(*lx->toks));
    }
    t = &lx->toks[lx->count++];
    t->kind = kind;
    t->start = start;
    t->len = end - start;
    t->line = line;
    t->col = start - line_start;
    t->line_start = line_start;
}

static void lex_skip_string(Lexer *lx, int *i, char quote)
{
    const char *s = lx->src;
    int n = lx->len;
    (*i)++;
    while (*i < n) {
        if (s[*i] == '\\') {
            if (*i + 1 < n && s[*i + 1] == '\n') {
                (*i) += 2;
                lx->line++;
                lx->line_start = *i;
            } else {
                (*i) += 2;
            }
            continue;
        }
        if (s[*i] == quote) {
            (*i)++;
            return;
        }
        if (s[*i] == '\n') {
            lx->line++;
            lx->line_start = *i + 1;
        }
        (*i)++;
    }
}

static void lex_source(const char *src, int len, Lexer *lx)
{
    int i = 0;
    bool bol = true;

    memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->len = len;
    lx->line = 1;
    lx->line_start = 0;

    while (i < len) {
        unsigned char c = (unsigned char)src[i];

        if (c == '\n') {
            i++;
            lx->line++;
            lx->line_start = i;
            bol = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n')
                i++;
            bol = true;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i < len) {
                if (src[i] == '*' && i + 1 < len && src[i + 1] == '/') {
                    i += 2;
                    break;
                }
                if (src[i] == '\n') {
                    lx->line++;
                    lx->line_start = i + 1;
                }
                i++;
            }
            continue;
        }
        if (c == '#' && bol) {
            int start = i;
            int line = lx->line;
            int ls = lx->line_start;
            while (i < len) {
                if (src[i] == '\\' && i + 1 < len && src[i + 1] == '\n') {
                    i += 2;
                    lx->line++;
                    lx->line_start = i;
                    continue;
                }
                if (src[i] == '\n')
                    break;
                i++;
            }
            lex_push(lx, TK_PREPROC, start, i, line, ls);
            bol = false;
            continue;
        }
        if (ident_start(c)) {
            int start = i;
            bol = false;
            i++;
            while (i < len && ident_char((unsigned char)src[i]))
                i++;
            lex_push(lx, TK_IDENT, start, i, lx->line, lx->line_start);
            continue;
        }
        if (isdigit(c)) {
            int start = i;
            bol = false;
            i++;
            while (i < len) {
                unsigned char d = (unsigned char)src[i];
                if (isalnum(d) || d == '.' || d == '_' || d == '\'')
                    i++;
                else
                    break;
            }
            lex_push(lx, TK_NUMBER, start, i, lx->line, lx->line_start);
            continue;
        }
        if (c == '"' || c == '\'') {
            int start = i;
            bol = false;
            lex_skip_string(lx, &i, (char)c);
            lex_push(lx, c == '"' ? TK_STRING : TK_CHAR, start, i, lx->line,
                     lx->line_start);
            continue;
        }
        bol = false;
        lex_push(lx, TK_PUNCT, i, i + 1, lx->line, lx->line_start);
        i++;
    }
}

static void lex_free(Lexer *lx)
{
    free(lx->toks);
    lx->toks = NULL;
    lx->count = 0;
    lx->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Parser helpers                                                      */
/* ------------------------------------------------------------------ */

#define SCOPE_BLOCK 100
#define SCOPE_EXTERN 101

typedef struct {
    Project *proj;
    int file_index;
    const char *src;
    int len;
    Tok *t;
    int n;
    int *match;       /* matching braces */
    int *depth;       /* brace depth before token */
    int *brace_scope; /* scope kind at '{' token, 0 if plain block */
} Parser;

static bool tok_is(const Parser *ps, int i, char c)
{
    if (i < 0 || i >= ps->n)
        return false;
    return ps->t[i].kind == TK_PUNCT && ps->src[ps->t[i].start] == c;
}

static bool tok_kind(const Parser *ps, int i, TokKind k)
{
    return i >= 0 && i < ps->n && ps->t[i].kind == k;
}

static bool tok_text(const Parser *ps, int i, const char *text)
{
    size_t n = strlen(text);
    if (i < 0 || i >= ps->n)
        return false;
    return (size_t)ps->t[i].len == n &&
           memcmp(ps->src + ps->t[i].start, text, n) == 0;
}

static bool tok_ident(const Parser *ps, int i)
{
    return tok_kind(ps, i, TK_IDENT);
}

static bool is_reserved_name(const Parser *ps, int i)
{
    static const char *const reserved[] = {
        "if", "else", "while", "for", "switch", "case", "do", "return",
        "sizeof", "alignof", "decltype", "static_assert", "noexcept",
        "throw", "catch", "typeof", "__typeof__", "__attribute__",
        "__declspec", "alignas", "requires", "operator", "new", "delete",
        "and", "or", "not"
    };
    size_t k;
    for (k = 0; k < SI_ARRAY_LEN(reserved); k++) {
        if (tok_text(ps, i, reserved[k]))
            return true;
    }
    return false;
}

static const char *kind_word(SymbolKind kind)
{
    switch (kind) {
    case SYM_FUNCTION:
        return "fn";
    case SYM_PROTOTYPE:
        return "proto";
    case SYM_STRUCT:
        return "struct";
    case SYM_UNION:
        return "union";
    case SYM_ENUM:
        return "enum";
    case SYM_CLASS:
        return "class";
    case SYM_NAMESPACE:
        return "namespace";
    case SYM_TYPEDEF:
        return "typedef";
    case SYM_MACRO:
        return "macro";
    case SYM_VARIABLE:
        return "var";
    case SYM_ENUMVAL:
        return "const";
    case SYM_MEMBER:
        return "member";
    }
    return "sym";
}

const char *symbol_kind_name(SymbolKind kind)
{
    switch (kind) {
    case SYM_FUNCTION:
        return "function";
    case SYM_PROTOTYPE:
        return "prototype";
    case SYM_STRUCT:
        return "struct";
    case SYM_UNION:
        return "union";
    case SYM_ENUM:
        return "enum";
    case SYM_CLASS:
        return "class";
    case SYM_NAMESPACE:
        return "namespace";
    case SYM_TYPEDEF:
        return "typedef";
    case SYM_MACRO:
        return "macro";
    case SYM_VARIABLE:
        return "variable";
    case SYM_ENUMVAL:
        return "enumerator";
    case SYM_MEMBER:
        return "struct member";
    }
    return "symbol";
}

const char *symbol_kind_short(SymbolKind kind)
{
    return kind_word(kind);
}

static int add_symbol_raw(Parser *ps, SymbolKind kind, int name_tok,
                          const char *signature, int end_line)
{
    Symbol *s;

    if (name_tok < 0 || name_tok >= ps->n)
        return -1;
    if (ps->proj->sym_count >= SI_MAX_SYMBOLS)
        return -1;
    if (ps->proj->sym_count == ps->proj->sym_cap) {
        ps->proj->sym_cap = ps->proj->sym_cap ? ps->proj->sym_cap * 2 : 256;
        ps->proj->syms = xrealloc(ps->proj->syms,
                                  ps->proj->sym_cap * sizeof(*ps->proj->syms));
    }
    s = &ps->proj->syms[ps->proj->sym_count++];
    memset(s, 0, sizeof(*s));
    s->name = xstrndup(ps->src + ps->t[name_tok].start,
                       (size_t)ps->t[name_tok].len);
    s->signature = xstrdup(signature && *signature ? signature : s->name);
    s->file_index = ps->file_index;
    s->line = ps->t[name_tok].line;
    s->col = ps->t[name_tok].col;
    s->end_line = end_line > 0 ? end_line : s->line;
    s->kind = kind;
    return (int)ps->proj->sym_count - 1;
}

static int add_symbol(Parser *ps, SymbolKind kind, int name_tok,
                      int sig_start, int sig_end, int end_line)
{
    Str sig;
    int i;
    char prev = '\0';
    int rc;

    if (name_tok < 0 || name_tok >= ps->n)
        return -1;

    str_init(&sig);
    if (sig_start < 0)
        sig_start = name_tok;
    if (sig_end < sig_start)
        sig_end = sig_start;
    for (i = sig_start; i <= sig_end && i < ps->n; i++) {
        const char *text = ps->src + ps->t[i].start;
        int len = ps->t[i].len;
        char first;
        bool need_space = false;
        if (len <= 0)
            continue;
        first = text[0];
        if (sig.len > 0) {
            bool prev_word = isalnum((unsigned char)prev) || prev == '_';
            bool cur_word = isalnum((unsigned char)first) || first == '_';
            if (prev == ',')
                need_space = true;
            else if (prev_word && cur_word)
                need_space = true;
            else if ((first == '*' || first == '&') && prev_word)
                need_space = true;
            else if (first == '{' || first == '}' || first == ';')
                need_space = true;
        }
        if (need_space && sig.len + 1 < 200)
            str_putc(&sig, ' ');
        if (sig.len + (size_t)len > 190)
            len = (int)(190 - sig.len);
        if (len > 0)
            str_append(&sig, text, (size_t)len);
        if (sig.len >= 190)
            break;
        prev = text[len - 1];
    }
    if (sig.len == 0)
        str_putc(&sig, '?');
    rc = add_symbol_raw(ps, kind, name_tok, sig.data, end_line);
    str_free(&sig);
    return rc;
}

static int matching_brace(const Parser *ps, int open)
{
    if (open < 0 || open >= ps->n)
        return -1;
    return ps->match[open];
}

static int find_open_paren(const Parser *ps, int close)
{
    int depth = 0;
    int i;
    for (i = close; i >= 0; i--) {
        if (tok_is(ps, i, ')'))
            depth++;
        else if (tok_is(ps, i, '(')) {
            depth--;
            if (depth == 0)
                return i;
        }
    }
    return -1;
}

static bool is_scope_aggregate(int scope)
{
    return scope == SYM_STRUCT || scope == SYM_UNION || scope == SYM_CLASS ||
           scope == SYM_NAMESPACE || scope == SCOPE_EXTERN;
}

static void parse_macro(Parser *ps, int idx)
{
    const char *s = ps->src;
    int i = ps->t[idx].start + 1;
    int end = ps->t[idx].start + ps->t[idx].len;
    int name_start;
    int name_end;
    Str sig;
    char prev = '\0';

    while (i < end && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i + 6 > end || memcmp(s + i, "define", 6) != 0)
        return;
    i += 6;
    if (i < end && ident_char((unsigned char)s[i]))
        return;
    while (i < end && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i >= end || !ident_start((unsigned char)s[i]))
        return;
    name_start = i;
    while (i < end && ident_char((unsigned char)s[i]))
        i++;
    name_end = i;

    str_init(&sig);
    for (i = ps->t[idx].start; i < end; i++) {
        if (s[i] == '\\' && i + 1 < end && s[i + 1] == '\n') {
            i++;
            continue;
        }
        if (isspace((unsigned char)s[i])) {
            if (sig.len > 0 && sig.data[sig.len - 1] != ' ')
                str_putc(&sig, ' ');
            continue;
        }
        str_putc(&sig, s[i]);
        if (sig.len >= 180)
            break;
        prev = s[i];
    }
    (void)prev;
    while (sig.len > 0 && sig.data[sig.len - 1] == ' ')
        sig.data[--sig.len] = '\0';

    if (ps->proj->sym_count >= SI_MAX_SYMBOLS) {
        str_free(&sig);
        return;
    }
    if (ps->proj->sym_count == ps->proj->sym_cap) {
        ps->proj->sym_cap = ps->proj->sym_cap ? ps->proj->sym_cap * 2 : 256;
        ps->proj->syms = xrealloc(ps->proj->syms,
                                  ps->proj->sym_cap * sizeof(*ps->proj->syms));
    }
    {
        Symbol *sym = &ps->proj->syms[ps->proj->sym_count++];
        int line = ps->t[idx].line;
        int col = ps->t[idx].col + (name_start - ps->t[idx].start);
        memset(sym, 0, sizeof(*sym));
        sym->name = xstrndup(s + name_start, (size_t)(name_end - name_start));
        sym->signature = sig.data ? sig.data : xstrdup(sym->name);
        sym->file_index = ps->file_index;
        sym->line = line;
        sym->col = col;
        sym->end_line = line;
        sym->kind = SYM_MACRO;
    }
}

static void parse_enum_values(Parser *ps, int open, int close)
{
    int i = open + 1;
    bool expect = true;
    int paren = 0;
    int brack = 0;

    while (i < close) {
        if (expect && tok_ident(ps, i)) {
            add_symbol(ps, SYM_ENUMVAL, i, i, i, ps->t[i].line);
            expect = false;
            i++;
            continue;
        }
        if (tok_is(ps, i, '('))
            paren++;
        else if (tok_is(ps, i, ')') && paren > 0)
            paren--;
        else if (tok_is(ps, i, '['))
            brack++;
        else if (tok_is(ps, i, ']') && brack > 0)
            brack--;
        else if (tok_is(ps, i, ',') && paren == 0 && brack == 0) {
            expect = true;
            i++;
            continue;
        } else if (tok_is(ps, i, '=') && paren == 0 && brack == 0) {
            /* Skip the initializer expression. */
            i++;
            while (i < close) {
                if (tok_is(ps, i, '('))
                    paren++;
                else if (tok_is(ps, i, ')') && paren > 0)
                    paren--;
                else if (tok_is(ps, i, '['))
                    brack++;
                else if (tok_is(ps, i, ']') && brack > 0)
                    brack--;
                else if (tok_is(ps, i, ',') && paren == 0 && brack == 0)
                    break;
                i++;
            }
            continue;
        }
        i++;
    }
}

static void parse_members(Parser *ps, int open, int close);

static void parse_aggregates(Parser *ps)
{
    int i;
    for (i = 0; i < ps->n; i++) {
        SymbolKind kind;
        int j;
        int name = -1;
        int open = -1;
        int close;

        if (!tok_ident(ps, i))
            continue;
        if (tok_text(ps, i, "struct"))
            kind = SYM_STRUCT;
        else if (tok_text(ps, i, "union"))
            kind = SYM_UNION;
        else if (tok_text(ps, i, "enum"))
            kind = SYM_ENUM;
        else if (tok_text(ps, i, "class"))
            kind = SYM_CLASS;
        else if (tok_text(ps, i, "namespace")) {
            kind = SYM_NAMESPACE;
        } else if (tok_text(ps, i, "extern") && i + 2 < ps->n &&
                   tok_kind(ps, i + 1, TK_STRING) &&
                   ps->t[i + 1].len == 3 &&
                   memcmp(ps->src + ps->t[i + 1].start, "\"C\"", 3) == 0 &&
                   tok_is(ps, i + 2, '{')) {
            ps->brace_scope[i + 2] = SCOPE_EXTERN;
            continue;
        } else {
            continue;
        }

        if (kind == SYM_NAMESPACE) {
            j = i + 1;
            if (tok_ident(ps, j))
                name = j++;
            while (j < ps->n && !tok_is(ps, j, '{') &&
                   !tok_is(ps, j, ';') && !tok_is(ps, j, '='))
                j++;
            if (j < ps->n && tok_is(ps, j, '{'))
                open = j;
        } else {
            j = i + 1;
            if (kind == SYM_ENUM && (tok_text(ps, j, "class") ||
                                     tok_text(ps, j, "struct")))
                j++;
            if (tok_ident(ps, j) && !tok_text(ps, j, "final"))
                name = j++;
            while (j < ps->n && !tok_is(ps, j, '{') &&
                   !tok_is(ps, j, ';') && !tok_is(ps, j, '=') &&
                   !tok_is(ps, j, ')') && !tok_is(ps, j, '}')) {
                j++;
            }
            if (j < ps->n && tok_is(ps, j, '{'))
                open = j;
        }
        if (open < 0)
            continue;
        close = matching_brace(ps, open);
        if (close < 0)
            continue;
        ps->brace_scope[open] = (int)kind;
        if (name >= 0) {
            int sig_start = i;
            int sig_end = name;
            if (kind == SYM_NAMESPACE)
                sig_end = name;
            add_symbol(ps, kind, name, sig_start, sig_end,
                       ps->t[close].line);
        }
        if (kind == SYM_ENUM)
            parse_enum_values(ps, open, close);
        else
            parse_members(ps, open, close);
    }
}

static void parse_typedefs(Parser *ps)
{
    int i;
    for (i = 0; i < ps->n; i++) {
        int j;
        int semi = -1;
        int depth;
        int alias = -1;
        int first = -1;

        if (!tok_text(ps, i, "typedef"))
            continue;
        depth = ps->depth[i];
        for (j = i + 1; j < ps->n; j++) {
            if (tok_is(ps, j, '{'))
                depth++;
            else if (tok_is(ps, j, '}'))
                depth--;
            else if (tok_is(ps, j, ';') && depth == ps->depth[i]) {
                semi = j;
                break;
            }
        }
        if (semi < 0)
            continue;

        /* Function pointer typedef: typedef int (*cb)(int); */
        for (j = i + 1; j + 3 < semi; j++) {
            if (tok_is(ps, j, '(') && tok_is(ps, j + 1, '*') &&
                tok_ident(ps, j + 2) && tok_is(ps, j + 3, ')')) {
                alias = j + 2;
                break;
            }
        }
        if (alias < 0) {
            for (j = semi - 1; j > i; j--) {
                if (tok_ident(ps, j)) {
                    alias = j;
                    break;
                }
            }
        }
        if (alias < 0)
            continue;
        first = i;
        add_symbol(ps, SYM_TYPEDEF, alias, first, semi - 1, ps->t[semi].line);
    }
}

static bool signature_prefix(Parser *ps, int name, bool in_aggregate,
                             int *start_out)
{
    int i = name - 1;
    bool has_prefix = false;
    bool seen_equal = false;
    int steps = 0;

    if (i >= 0 && (tok_is(ps, i, ')') || tok_is(ps, i, ']')))
        return false;
    while (i >= 0 && steps < 60) {
        if (tok_is(ps, i, ';') || tok_is(ps, i, '{') || tok_is(ps, i, '}') ||
            tok_kind(ps, i, TK_PREPROC))
            break;
        if (tok_is(ps, i, '='))
            seen_equal = true;
        if (tok_is(ps, i, ':')) {
            if (i > 0 && tok_is(ps, i - 1, ':')) {
                i -= 2;
                has_prefix = true;
                steps += 2;
                continue;
            }
            break;
        }
        if (tok_ident(ps, i) || tok_is(ps, i, '*') || tok_is(ps, i, '&') ||
            tok_is(ps, i, '~') || tok_is(ps, i, '<') || tok_is(ps, i, '>')) {
            has_prefix = true;
        }
        i--;
        steps++;
    }
    *start_out = i + 1;
    if (seen_equal)
        return false;
    if (!has_prefix && !in_aggregate)
        return false;
    return true;
}

static bool preproc_is(const Parser *ps, int i, const char *word);

static void parse_functions(Parser *ps)
{
    int *scope_stack = xmalloc((size_t)(ps->n + 1) * sizeof(*scope_stack));
    int cond_stack[64];
    int cond_top = -1;
    int top = -1;
    int i;

    for (i = 0; i < ps->n; i++) {
        int open;
        int close;
        int name;
        int k;
        int depth_paren = 0;
        int depth_brack = 0;
        bool def = false;
        bool proto = false;
        bool seen_colon = false;
        int body = -1;
        bool in_aggregate;
        int sig_start;
        int end_line;

        if (preproc_is(ps, i, "if") || preproc_is(ps, i, "ifdef") ||
            preproc_is(ps, i, "ifndef")) {
            if (cond_top + 1 < (int)SI_ARRAY_LEN(cond_stack))
                cond_stack[++cond_top] = top;
            continue;
        }
        if (preproc_is(ps, i, "else") || preproc_is(ps, i, "elif")) {
            if (cond_top >= 0)
                top = cond_stack[cond_top];
            continue;
        }
        if (preproc_is(ps, i, "endif")) {
            if (cond_top >= 0)
                top = cond_stack[cond_top--];
            continue;
        }
        if (tok_is(ps, i, '{')) {
            int scope = ps->brace_scope[i];
            scope_stack[++top] = scope ? scope : SCOPE_BLOCK;
            continue;
        }
        if (tok_is(ps, i, '}')) {
            if (top >= 0)
                top--;
            continue;
        }
        if (!tok_is(ps, i, ')'))
            continue;

        in_aggregate = top >= 0 && is_scope_aggregate(scope_stack[top]);
        if (top >= 0 && !in_aggregate)
            continue;
        if (top < 0 && ps->depth[i] != 0)
            continue;

        open = find_open_paren(ps, i);
        if (open <= 0)
            continue;
        name = open - 1;
        if (!tok_ident(ps, name) || is_reserved_name(ps, name))
            continue;

        for (k = i + 1; k < ps->n && k < i + 180; k++) {
            if (tok_is(ps, k, '(')) {
                depth_paren++;
            } else if (tok_is(ps, k, ')')) {
                if (depth_paren > 0)
                    depth_paren--;
            } else if (tok_is(ps, k, '[')) {
                depth_brack++;
            } else if (tok_is(ps, k, ']')) {
                if (depth_brack > 0)
                    depth_brack--;
            } else if (depth_paren == 0 && depth_brack == 0) {
                if (tok_is(ps, k, '{')) {
                    def = true;
                    body = k;
                    break;
                }
                if (tok_is(ps, k, ';') || tok_is(ps, k, '=')) {
                    proto = true;
                    break;
                }
                if (tok_is(ps, k, ':')) {
                    seen_colon = true;
                } else if (tok_is(ps, k, ',') && !seen_colon) {
                    break;
                } else if (tok_is(ps, k, '}')) {
                    break;
                }
            }
        }
        if (!def && !proto)
            continue;
        if (!signature_prefix(ps, name, in_aggregate, &sig_start))
            continue;

        close = i;
        if (def && body >= 0) {
            int body_close = matching_brace(ps, body);
            if (body_close >= 0)
                end_line = ps->t[body_close].line;
            else
                end_line = ps->t[body].line;
        } else {
            end_line = ps->t[close].line;
        }
        add_symbol(ps, def ? SYM_FUNCTION : SYM_PROTOTYPE, name, sig_start,
                   close, end_line);
    }
    free(scope_stack);
}

static int first_top_level_equal(const Parser *ps, int start, int end)
{
    int i;
    int paren = 0;
    int brack = 0;
    int brace = 0;

    for (i = start; i < end; i++) {
        if (tok_is(ps, i, '('))
            paren++;
        else if (tok_is(ps, i, ')') && paren > 0)
            paren--;
        else if (tok_is(ps, i, '['))
            brack++;
        else if (tok_is(ps, i, ']') && brack > 0)
            brack--;
        else if (tok_is(ps, i, '{'))
            brace++;
        else if (tok_is(ps, i, '}') && brace > 0)
            brace--;
        else if (tok_is(ps, i, '=') && paren == 0 && brack == 0 &&
                 brace == 0)
            return i;
    }
    return -1;
}

static int first_top_level_comma(const Parser *ps, int start, int end)
{
    int i;
    int paren = 0;
    int brack = 0;
    int brace = 0;

    for (i = start; i < end; i++) {
        if (tok_is(ps, i, '('))
            paren++;
        else if (tok_is(ps, i, ')') && paren > 0)
            paren--;
        else if (tok_is(ps, i, '['))
            brack++;
        else if (tok_is(ps, i, ']') && brack > 0)
            brack--;
        else if (tok_is(ps, i, '{'))
            brace++;
        else if (tok_is(ps, i, '}') && brace > 0)
            brace--;
        else if (tok_is(ps, i, ',') && paren == 0 && brack == 0 &&
                 brace == 0)
            return i;
    }
    return -1;
}

static bool has_call_before_equal(const Parser *ps, int start, int end)
{
    int eq = first_top_level_equal(ps, start, end);
    int stop = eq >= 0 ? eq : end;
    int i;
    int brace = 0;

    for (i = start; i < stop; i++) {
        if (tok_is(ps, i, '{')) {
            brace++;
        } else if (tok_is(ps, i, '}') && brace > 0) {
            brace--;
        } else if (brace == 0 && tok_is(ps, i, '(')) {
            return true;
        }
    }
    return false;
}

static int last_ident_before(const Parser *ps, int start, int end)
{
    int i = end - 1;

    while (i >= start && !tok_ident(ps, i))
        i--;
    return i >= start ? i : -1;
}

static bool type_suffix_token(const Parser *ps, int i)
{
    return tok_is(ps, i, '*') || tok_is(ps, i, '&') ||
           tok_text(ps, i, "const") || tok_text(ps, i, "volatile") ||
           tok_text(ps, i, "restrict") || tok_text(ps, i, "_Atomic") ||
           tok_text(ps, i, "__restrict") || tok_text(ps, i, "__restrict__");
}

static void sig_append_range(Parser *ps, Str *sig, int start, int end)
{
    int i;

    for (i = start; i <= end && i < ps->n; i++) {
        int len;
        if (i < 0 || ps->t[i].len <= 0 || sig->len >= 190)
            continue;
        if (sig->len > 0)
            str_putc(sig, ' ');
        len = ps->t[i].len;
        if (sig->len + (size_t)len > 190)
            len = (int)(190 - sig->len);
        if (len > 0)
            str_append(sig, ps->src + ps->t[i].start, (size_t)len);
    }
}

static int declarator_signature_end(Parser *ps, int name, int end)
{
    int q = name + 1;

    while (q < end && tok_is(ps, q, '[')) {
        int depth = 1;
        q++;
        while (q < end && depth > 0) {
            if (tok_is(ps, q, '['))
                depth++;
            else if (tok_is(ps, q, ']'))
                depth--;
            q++;
        }
    }
    return q - 1;
}

static void variable_segment(Parser *ps, int seg, int end,
                             int base_start, int base_end,
                             bool first_seg, bool aggregate_tail)
{
    int limit = first_top_level_equal(ps, seg, end);
    int name;
    int sig_end;
    Str sig;

    if (limit < 0)
        limit = end;
    if (limit <= seg)
        return;
    name = last_ident_before(ps, seg, limit);
    if (name < 0)
        return;
    sig_end = declarator_signature_end(ps, name, limit);
    if (first_seg && !aggregate_tail) {
        /* The first declarator keeps its original token range, including
         * pointer stars before the name. */
        if (name == seg)
            return;
        add_symbol(ps, SYM_VARIABLE, name, base_start, sig_end,
                   ps->t[name].line);
        return;
    }
    if (base_end < base_start)
        return;
    str_init(&sig);
    sig_append_range(ps, &sig, base_start, base_end);
    sig_append_range(ps, &sig, seg, sig_end);
    if (sig.len == 0)
        str_putc(&sig, '?');
    add_symbol_raw(ps, SYM_VARIABLE, name, sig.data, ps->t[name].line);
    str_free(&sig);
}

static void member_segment(Parser *ps, int seg, int end, bool first_seg)
{
    int limit = end;
    int q;
    int name;

    if (end <= seg)
        return;
    for (q = seg; q < end; q++) {
        if (tok_is(ps, q, '=')) {
            limit = q;
            break;
        }
    }
    if (limit <= seg)
        return;
    for (q = seg; q < limit; q++) {
        /* Function pointers and nested aggregate bodies are skipped:
         * choosing their "last identifier" would name the wrong token. */
        if (tok_is(ps, q, '(') || tok_is(ps, q, '{') ||
            tok_is(ps, q, '}'))
            return;
    }
    q = limit - 1;
    if (q >= seg && tok_is(ps, q, ']')) {
        while (q >= seg && !tok_is(ps, q, '['))
            q--;
        q--;
    }
    while (q >= seg && !tok_ident(ps, q))
        q--;
    if (q < seg || is_reserved_name(ps, q))
        return;
    if (first_seg && q == seg)
        return;
    name = q;
    add_symbol(ps, SYM_MEMBER, name, seg, limit - 1,
               ps->t[name].line);
}

static void parse_members(Parser *ps, int open, int close)
{
    int i = open + 1;
    int seg = open + 1;
    int depth = 0;

    while (i < close) {
        if (tok_is(ps, i, '{')) {
            depth++;
        } else if (tok_is(ps, i, '}')) {
            if (depth > 0)
                depth--;
        } else if (tok_is(ps, i, ';') && depth == 0) {
            int j;
            int start = seg;
            int paren = 0;
            int brack = 0;
            bool first = true;
            for (j = seg; j < i; j++) {
                if (tok_is(ps, j, '(')) {
                    paren++;
                } else if (tok_is(ps, j, ')') && paren > 0) {
                    paren--;
                } else if (tok_is(ps, j, '[')) {
                    brack++;
                } else if (tok_is(ps, j, ']') && brack > 0) {
                    brack--;
                } else if (tok_is(ps, j, ',') && paren == 0 &&
                           brack == 0) {
                    member_segment(ps, start, j, first);
                    first = false;
                    start = j + 1;
                }
            }
            member_segment(ps, start, i, first);
            seg = i + 1;
        }
        i++;
    }
}

static void parse_variables(Parser *ps)
{
    int i;
    int stmt = 0;

    for (i = 0; i < ps->n; i++) {
        int start;
        int end;
        int k;

        if (tok_kind(ps, i, TK_PREPROC)) {
            stmt = i + 1;
            continue;
        }
        if (tok_is(ps, i, '}') && ps->depth[i] == 1) {
            int open = ps->match[i];
            int scope = open >= 0 ? ps->brace_scope[open] : 0;
            bool aggregate = scope == SYM_STRUCT || scope == SYM_UNION ||
                             scope == SYM_ENUM || scope == SYM_CLASS;
            bool initializer = false;
            if (!aggregate && open >= 0) {
                int q;
                for (q = open - 1; q >= stmt; q--) {
                    if (tok_is(ps, q, '=')) {
                        initializer = true;
                        break;
                    }
                    if (tok_is(ps, q, ';') || tok_is(ps, q, '{') ||
                        tok_is(ps, q, '}'))
                        break;
                }
            }
            if (!aggregate && !initializer)
                stmt = i + 1;
            continue;
        }
        if (!tok_is(ps, i, ';') || ps->depth[i] != 0)
            continue;

        start = stmt;
        end = i;
        stmt = i + 1;
        if (end - start <= 1)
            continue;
        if (tok_text(ps, start, "typedef") || tok_text(ps, start, "using") ||
            tok_text(ps, start, "template") ||
            tok_text(ps, start, "namespace") ||
            tok_text(ps, start, "static_assert"))
            continue;
        if (has_call_before_equal(ps, start, end))
            continue;

        {
            bool aggregate_stmt = tok_text(ps, start, "struct") ||
                                  tok_text(ps, start, "union") ||
                                  tok_text(ps, start, "enum") ||
                                  tok_text(ps, start, "class");
            int body_open = -1;
            int body_close = -1;
            int decl_start = start;
            int base_end = -1;

            if (aggregate_stmt) {
                for (k = start; k < end; k++) {
                    int scope;
                    if (!tok_is(ps, k, '{'))
                        continue;
                    scope = ps->brace_scope[k];
                    if (scope == SYM_STRUCT || scope == SYM_UNION ||
                        scope == SYM_ENUM || scope == SYM_CLASS) {
                        body_open = k;
                        break;
                    }
                }
                if (body_open >= 0) {
                    body_close = ps->match[body_open];
                    if (body_close < 0 || body_close >= end)
                        continue;
                    base_end = body_open - 1;
                    decl_start = body_close + 1;
                    if (decl_start >= end)
                        continue;
                } else {
                    int first = start + 1;
                    int ident_count = 0;
                    bool opaque_enum = false;
                    if (tok_text(ps, start, "enum") && first < end &&
                        (tok_text(ps, first, "class") ||
                         tok_text(ps, first, "struct")))
                        first++;
                    for (k = first; k < end; k++) {
                        if (tok_ident(ps, k))
                            ident_count++;
                        if (tok_text(ps, start, "enum") &&
                            tok_is(ps, k, ':'))
                            opaque_enum = true;
                    }
                    /* struct Foo; / enum class Foo; are declarations. */
                    if (opaque_enum || ident_count < 2)
                        continue;
                }
            }

            if (base_end < 0) {
                int eq = first_top_level_equal(ps, decl_start, end);
                int limit = eq >= 0 ? eq : end;
                int comma = first_top_level_comma(ps, decl_start, limit);
                int first_name;
                if (comma >= 0)
                    limit = comma;
                first_name = last_ident_before(ps, decl_start, limit);
                if (first_name < 0)
                    continue;
                if (first_name == decl_start && !aggregate_stmt)
                    continue;
                base_end = first_name - 1;
                while (base_end >= start && type_suffix_token(ps, base_end))
                    base_end--;
                if (base_end < start)
                    continue;
            }

            {
                int seg = decl_start;
                int paren = 0;
                int brack = 0;
                int brace = 0;
                bool first = true;
                for (k = decl_start; k <= end; k++) {
                    bool split = k == end;
                    if (!split) {
                        if (tok_is(ps, k, '(')) {
                            paren++;
                        } else if (tok_is(ps, k, ')') && paren > 0) {
                            paren--;
                        } else if (tok_is(ps, k, '[')) {
                            brack++;
                        } else if (tok_is(ps, k, ']') && brack > 0) {
                            brack--;
                        } else if (tok_is(ps, k, '{')) {
                            brace++;
                        } else if (tok_is(ps, k, '}') && brace > 0) {
                            brace--;
                        } else if (tok_is(ps, k, ',') && paren == 0 &&
                                   brack == 0 && brace == 0) {
                            split = true;
                        }
                    }
                    if (split) {
                        variable_segment(ps, seg, k, start, base_end, first,
                                         body_open >= 0);
                        first = false;
                        seg = k + 1;
                    }
                }
            }
        }
    }
}

static bool preproc_is(const Parser *ps, int i, const char *word)
{
    const char *s;
    int len;
    int p;
    size_t n;

    if (i < 0 || i >= ps->n || ps->t[i].kind != TK_PREPROC)
        return false;
    s = ps->src + ps->t[i].start;
    len = ps->t[i].len;
    p = 0;
    while (p < len && (s[p] == ' ' || s[p] == '\t'))
        p++;
    if (p >= len || s[p] != '#')
        return false;
    p++;
    while (p < len && (s[p] == ' ' || s[p] == '\t'))
        p++;
    n = strlen(word);
    if ((size_t)(p + (int)n) > (size_t)len || memcmp(s + p, word, n) != 0)
        return false;
    if (p + (int)n < len &&
        (isalnum((unsigned char)s[p + n]) || s[p + n] == '_'))
        return false;
    return true;
}

static void compute_braces(Parser *ps)
{
    int *stack = xmalloc((size_t)(ps->n + 1) * sizeof(*stack));
    int cond_stack[64];
    int cond_top = -1;
    int top = -1;
    int d = 0;
    int i;

    for (i = 0; i < ps->n; i++) {
        ps->depth[i] = d;
        ps->match[i] = -1;
        if (preproc_is(ps, i, "if") || preproc_is(ps, i, "ifdef") ||
            preproc_is(ps, i, "ifndef")) {
            if (cond_top + 1 < (int)SI_ARRAY_LEN(cond_stack))
                cond_stack[++cond_top] = d;
        } else if (preproc_is(ps, i, "else") ||
                   preproc_is(ps, i, "elif")) {
            if (cond_top >= 0)
                d = cond_stack[cond_top];
        } else if (preproc_is(ps, i, "endif")) {
            if (cond_top >= 0)
                d = cond_stack[cond_top--];
        } else if (tok_is(ps, i, '{')) {
            stack[++top] = i;
            d++;
        } else if (tok_is(ps, i, '}')) {
            if (top >= 0) {
                int open = stack[top--];
                ps->match[open] = i;
                ps->match[i] = open;
            }
            if (d > 0)
                d--;
        }
    }
    free(stack);
}

static void parse_source(Project *p, int file_index, const char *src, int len)
{
    Parser ps;
    Lexer lx;
    int i;

    lex_source(src, len, &lx);
    if (lx.count == 0) {
        lex_free(&lx);
        return;
    }
    memset(&ps, 0, sizeof(ps));
    ps.proj = p;
    ps.file_index = file_index;
    ps.src = src;
    ps.len = len;
    ps.t = lx.toks;
    ps.n = lx.count;
    ps.match = xmalloc((size_t)ps.n * sizeof(*ps.match));
    ps.depth = xmalloc((size_t)ps.n * sizeof(*ps.depth));
    ps.brace_scope = xcalloc((size_t)ps.n, sizeof(*ps.brace_scope));

    compute_braces(&ps);
    for (i = 0; i < ps.n; i++) {
        if (tok_kind(&ps, i, TK_PREPROC))
            parse_macro(&ps, i);
    }
    parse_aggregates(&ps);
    parse_typedefs(&ps);
    parse_functions(&ps);
    parse_variables(&ps);

    free(ps.match);
    free(ps.depth);
    free(ps.brace_scope);
    lex_free(&lx);
}

/* ------------------------------------------------------------------ */
/* Aggregate members (for editor completion)                           */
/* ------------------------------------------------------------------ */

void project_member_list_init(ProjectMemberList *l)
{
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void project_member_list_free(ProjectMemberList *l)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        free(l->items[i].name);
        free(l->items[i].type);
        free(l->items[i].signature);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static void member_list_add(ProjectMemberList *l, const char *name,
                            const char *type, const char *signature,
                            bool is_array)
{
    size_t i;
    ProjectMember *m;

    if (!name || !*name)
        return;
    for (i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0)
            return;
    }
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = xrealloc(l->items, l->cap * sizeof(*l->items));
    }
    m = &l->items[l->count++];
    m->name = xstrdup(name);
    m->type = xstrdup(type ? type : "");
    m->signature = xstrdup(signature ? signature : name);
    m->is_array = is_array;
}

static bool type_word_qualifier(const char *s, size_t n)
{
    static const char *const words[] = {
        "const", "volatile", "static", "extern", "register", "auto",
        "restrict", "inline", "_Atomic", "mutable", "typename",
        "__restrict", "__restrict__"
    };
    size_t i;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        if (strlen(words[i]) == n && memcmp(words[i], s, n) == 0)
            return true;
    }
    return false;
}

static bool type_word_builtin(const char *s, size_t n)
{
    static const char *const words[] = {
        "void", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "_Bool", "bool", "size_t", "ssize_t",
        "wchar_t", "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t",
        "uintmax_t", "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t", "off_t",
        "time_t", "clock_t", "FILE", "DIR", "pid_t", "mode_t", "uid_t",
        "gid_t", "socklen_t", "va_list", "stdint", "uint", "ulong",
        "uchar", "ushort"
    };
    size_t i;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        if (strlen(words[i]) == n && memcmp(words[i], s, n) == 0)
            return true;
    }
    return false;
}

static bool type_word_aggregate_keyword(const char *s, size_t n)
{
    static const char *const words[] = {
        "struct", "union", "enum", "class"
    };
    size_t i;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        if (strlen(words[i]) == n && memcmp(words[i], s, n) == 0)
            return true;
    }
    return false;
}

bool project_type_aggregate_key(const char *type_text,
                                char *out, size_t out_size)
{
    const char *p;
    const char *key = NULL;
    size_t key_len = 0;

    if (!type_text || !out || out_size == 0)
        return false;
    for (p = type_text; *p; p++) {
        if (*p == '{' || *p == '(')
            return false;
    }
    p = type_text;
    while (*p) {
        const char *start;
        size_t n;
        if (!isalpha((unsigned char)*p) && *p != '_') {
            p++;
            continue;
        }
        start = p;
        p++;
        while (isalnum((unsigned char)*p) || *p == '_')
            p++;
        n = (size_t)(p - start);
        if (type_word_qualifier(start, n) ||
            type_word_builtin(start, n) ||
            type_word_aggregate_keyword(start, n))
            continue;
        key = start;
        key_len = n;
    }
    if (!key || key_len == 0)
        return false;
    if (key_len >= out_size)
        key_len = out_size - 1;
    memcpy(out, key, key_len);
    out[key_len] = '\0';
    return true;
}

static bool tok_is_punct(const char *src, const Tok *t, char c)
{
    return t->kind == TK_PUNCT && t->len == 1 && src[t->start] == c;
}

static bool tok_is_text(const char *src, const Tok *t, const char *text)
{
    size_t n = strlen(text);
    return (size_t)t->len == n && memcmp(src + t->start, text, n) == 0;
}

static bool tok_is_ident(const Tok *t)
{
    return t->kind == TK_IDENT;
}

static int tok_matching_brace(const char *src, const Tok *t, int n, int open)
{
    int i;
    int depth = 0;
    for (i = open; i < n; i++) {
        if (tok_is_punct(src, &t[i], '{'))
            depth++;
        else if (tok_is_punct(src, &t[i], '}')) {
            depth--;
            if (depth == 0)
                return i;
        }
    }
    return -1;
}

static int tok_find_symbol(const char *src, const Tok *t, int n,
                           const Symbol *s)
{
    int i;
    for (i = 0; i < n; i++) {
        if (t[i].line != s->line || t[i].col != s->col)
            continue;
        if (tok_is_text(src, &t[i], s->name))
            return i;
    }
    return -1;
}

static int *tok_depths(const char *src, const Tok *t, int n)
{
    int *depth = xmalloc((size_t)n * sizeof(*depth));
    int d = 0;
    int i;
    for (i = 0; i < n; i++) {
        depth[i] = d;
        if (tok_is_punct(src, &t[i], '{'))
            d++;
        else if (tok_is_punct(src, &t[i], '}') && d > 0)
            d--;
    }
    return depth;
}

static char *tok_range_text(const char *src, const Tok *t,
                            int start, int end)
{
    Str s;
    int i;

    str_init(&s);
    for (i = start; i <= end; i++) {
        if (i < 0 || t[i].len <= 0)
            continue;
        if (s.len > 0)
            str_putc(&s, ' ');
        str_append(&s, src + t[i].start, (size_t)t[i].len);
        if (s.len >= 190)
            break;
    }
    if (s.len == 0) {
        str_free(&s);
        return xstrdup("");
    }
    return s.data;
}

static bool member_type_suffix(const char *src, const Tok *t, int i)
{
    return tok_is_punct(src, &t[i], '*') || tok_is_punct(src, &t[i], '&') ||
           tok_is_text(src, &t[i], "const") ||
           tok_is_text(src, &t[i], "volatile") ||
           tok_is_text(src, &t[i], "restrict") ||
           tok_is_text(src, &t[i], "_Atomic") ||
           tok_is_text(src, &t[i], "__restrict") ||
           tok_is_text(src, &t[i], "__restrict__");
}

static int member_name_index(const char *src, const Tok *t, int start, int end)
{
    int i;
    int cut = end;
    int paren = 0;
    int brack = 0;
    int brace = 0;

    for (i = start; i < end; i++) {
        if (tok_is_punct(src, &t[i], '(')) {
            paren++;
        } else if (tok_is_punct(src, &t[i], ')') && paren > 0) {
            paren--;
        } else if (tok_is_punct(src, &t[i], '[')) {
            brack++;
        } else if (tok_is_punct(src, &t[i], ']') && brack > 0) {
            brack--;
        } else if (tok_is_punct(src, &t[i], '{')) {
            brace++;
        } else if (tok_is_punct(src, &t[i], '}') && brace > 0) {
            brace--;
        } else if (paren == 0 && brack == 0 && brace == 0 &&
                   (tok_is_punct(src, &t[i], '=') ||
                    tok_is_punct(src, &t[i], ':'))) {
            cut = i;
            break;
        }
    }
    i = cut - 1;
    while (i >= start && !tok_is_ident(&t[i]))
        i--;
    return i >= start ? i : -1;
}

static bool member_has_array(const char *src, const Tok *t, int name,
                             int end)
{
    int i;
    int paren = 0;
    int brack = 0;
    int brace = 0;

    for (i = name + 1; i < end; i++) {
        if (tok_is_punct(src, &t[i], '(')) {
            paren++;
        } else if (tok_is_punct(src, &t[i], ')') && paren > 0) {
            paren--;
        } else if (tok_is_punct(src, &t[i], '[')) {
            if (paren == 0 && brace == 0)
                return true;
            brack++;
        } else if (tok_is_punct(src, &t[i], ']') && brack > 0) {
            brack--;
        } else if (tok_is_punct(src, &t[i], '{')) {
            brace++;
        } else if (tok_is_punct(src, &t[i], '}') && brace > 0) {
            brace--;
        } else if (paren == 0 && brack == 0 && brace == 0 &&
                   (tok_is_punct(src, &t[i], '=') ||
                    tok_is_punct(src, &t[i], ':'))) {
            break;
        }
    }
    return false;
}

static void member_segment_add(const char *src, const Tok *t,
                               ProjectMemberList *out, int base_start,
                               int base_end, int seg, int end, bool first)
{
    int name = member_name_index(src, t, seg, end);
    bool is_array;
    char *type;
    char *signature;
    char *name_text;

    if (name < 0)
        return;
    is_array = member_has_array(src, t, name, end);
    if (first) {
        type = tok_range_text(src, t, base_start, name - 1);
    } else {
        Str s;
        char *base = tok_range_text(src, t, base_start, base_end);
        char *prefix = tok_range_text(src, t, seg, name - 1);
        str_init(&s);
        if (base[0])
            str_puts(&s, base);
        if (prefix[0]) {
            if (s.len > 0)
                str_putc(&s, ' ');
            str_puts(&s, prefix);
        }
        free(base);
        free(prefix);
        if (s.len > 0) {
            type = s.data;
        } else {
            str_free(&s);
            type = xstrdup("");
        }
    }
    signature = tok_range_text(src, t, seg, name);
    name_text = tok_range_text(src, t, name, name);
    member_list_add(out, name_text, type, signature, is_array);
    free(name_text);
    free(type);
    free(signature);
}

static void process_member_statement(const char *src, const Tok *t, int n,
                                     int start, int end,
                                     ProjectMemberList *out)
{
    int i;
    int paren = 0;
    int brack = 0;
    int brace = 0;
    int eq = -1;
    int body_open = -1;
    int lparen = -1;
    int base_end;
    int first_end;
    int first_name;
    int seg;

    if (start >= end)
        return;
    if (tok_is_ident(&t[start]) &&
        (tok_is_text(src, &t[start], "public") ||
         tok_is_text(src, &t[start], "private") ||
         tok_is_text(src, &t[start], "protected")) &&
        start + 1 < end && tok_is_punct(src, &t[start + 1], ':')) {
        start += 2;
    }
    if (start >= end)
        return;
    if (tok_is_text(src, &t[start], "typedef") ||
        tok_is_text(src, &t[start], "using") ||
        tok_is_text(src, &t[start], "friend") ||
        tok_is_text(src, &t[start], "template") ||
        tok_is_text(src, &t[start], "static"))
        return;

    for (i = start; i < end; i++) {
        if (tok_is_punct(src, &t[i], '(')) {
            paren++;
        } else if (tok_is_punct(src, &t[i], ')') && paren > 0) {
            paren--;
        } else if (tok_is_punct(src, &t[i], '[')) {
            brack++;
        } else if (tok_is_punct(src, &t[i], ']') && brack > 0) {
            brack--;
        } else if (tok_is_punct(src, &t[i], '{')) {
            if (paren == 0 && brack == 0 && brace == 0 && body_open < 0)
                body_open = i;
            brace++;
        } else if (tok_is_punct(src, &t[i], '}') && brace > 0) {
            brace--;
        } else if (paren == 0 && brack == 0 && brace == 0 &&
                   tok_is_punct(src, &t[i], '=') && eq < 0) {
            eq = i;
        }
    }

    if (body_open >= 0 && (eq < 0 || eq > body_open)) {
        int close = tok_matching_brace(src, t, n, body_open);
        int q;
        if (close < 0 || close >= end)
            return;
        seg = close + 1;
        for (q = close + 1; q <= end; q++) {
            if (q == end || tok_is_punct(src, &t[q], ',') ||
                tok_is_punct(src, &t[q], ';')) {
                int name = member_name_index(src, t, seg, q);
                if (name >= 0) {
                    char *name_text = tok_range_text(src, t, name, name);
                    char *signature =
                        tok_range_text(src, t, seg, name);
                    member_list_add(out, name_text, "", signature, false);
                    free(signature);
                    free(name_text);
                }
                seg = q + 1;
            }
        }
        return;
    }

    for (i = start; i < end; i++) {
        if (tok_is_punct(src, &t[i], '(')) {
            lparen = i;
            break;
        }
    }
    if (lparen >= 0 && (eq < 0 || lparen < eq)) {
        if (lparen + 2 < end && tok_is_punct(src, &t[lparen + 1], '*') &&
            tok_is_ident(&t[lparen + 2])) {
            char *name_text = tok_range_text(src, t, lparen + 2, lparen + 2);
            char *signature = tok_range_text(src, t, start, end - 1);
            member_list_add(out, name_text, "", signature, false);
            free(signature);
            free(name_text);
        }
        return;
    }

    first_end = end;
    for (i = start; i < end; i++) {
        if (tok_is_punct(src, &t[i], '(')) {
            paren++;
        } else if (tok_is_punct(src, &t[i], ')') && paren > 0) {
            paren--;
        } else if (tok_is_punct(src, &t[i], '[')) {
            brack++;
        } else if (tok_is_punct(src, &t[i], ']') && brack > 0) {
            brack--;
        } else if (tok_is_punct(src, &t[i], '{')) {
            brace++;
        } else if (tok_is_punct(src, &t[i], '}') && brace > 0) {
            brace--;
        } else if (paren == 0 && brack == 0 && brace == 0 &&
                   tok_is_punct(src, &t[i], ',')) {
            first_end = i;
            break;
        }
    }
    first_name = member_name_index(src, t, start, first_end);
    if (first_name < 0)
        return;
    base_end = first_name - 1;
    while (base_end >= start && member_type_suffix(src, t, base_end))
        base_end--;

    seg = start;
    paren = brack = brace = 0;
    for (i = start; i <= end; i++) {
        bool split = i == end;
        if (!split) {
            if (tok_is_punct(src, &t[i], '(')) {
                paren++;
            } else if (tok_is_punct(src, &t[i], ')') && paren > 0) {
                paren--;
            } else if (tok_is_punct(src, &t[i], '[')) {
                brack++;
            } else if (tok_is_punct(src, &t[i], ']') && brack > 0) {
                brack--;
            } else if (tok_is_punct(src, &t[i], '{')) {
                brace++;
            } else if (tok_is_punct(src, &t[i], '}') && brace > 0) {
                brace--;
            } else if (paren == 0 && brack == 0 && brace == 0 &&
                       tok_is_punct(src, &t[i], ',')) {
                split = true;
            }
        }
        if (split) {
            member_segment_add(src, t, out, start, base_end, seg, i,
                               seg == start);
            seg = i + 1;
        }
    }
}

static void collect_members(const char *src, const Tok *t, int n,
                            int open, int close, ProjectMemberList *out)
{
    int i = open + 1;
    int stmt_start = open + 1;
    int paren = 0;
    int brack = 0;
    int brace = 0;

    while (i < close) {
        if (tok_is_punct(src, &t[i], '{') && brace == 0) {
            int q;
            bool eq_before = false;
            bool paren_before = false;
            int end = tok_matching_brace(src, t, n, i);
            for (q = stmt_start; q < i; q++) {
                if (tok_is_punct(src, &t[q], '='))
                    eq_before = true;
                if (tok_is_punct(src, &t[q], '('))
                    paren_before = true;
            }
            if (paren_before && !eq_before) {
                i = end >= 0 ? end + 1 : close;
                stmt_start = i;
                continue;
            }
        }
        if (tok_is_punct(src, &t[i], '(')) {
            paren++;
        } else if (tok_is_punct(src, &t[i], ')') && paren > 0) {
            paren--;
        } else if (tok_is_punct(src, &t[i], '[')) {
            brack++;
        } else if (tok_is_punct(src, &t[i], ']') && brack > 0) {
            brack--;
        } else if (tok_is_punct(src, &t[i], '{')) {
            brace++;
        } else if (tok_is_punct(src, &t[i], '}') && brace > 0) {
            brace--;
        } else if (paren == 0 && brack == 0 && brace == 0 &&
                   tok_is_punct(src, &t[i], ';')) {
            process_member_statement(src, t, n, stmt_start, i, out);
            stmt_start = i + 1;
        }
        i++;
    }
}

static char *read_project_source(Project *p, int file_index, int *len_out)
{
    char *text = NULL;
    char *err = NULL;
    size_t len = 0;

    if (file_index < 0 || (size_t)file_index >= p->file_count)
        return NULL;
    if (!file_read_all(p->files[file_index].path, SI_MAX_FILE_SIZE,
                       &text, &len, &err)) {
        free(err);
        return NULL;
    }
    free(err);
    *len_out = (int)len;
    return text;
}

static bool collect_symbol_body(Project *p, const Symbol *s,
                                ProjectMemberList *out)
{
    char *src;
    int len = 0;
    Lexer lx;
    int sym_tok;
    int open = -1;
    int close;
    int i;

    src = read_project_source(p, s->file_index, &len);
    if (!src)
        return false;
    lex_source(src, len, &lx);
    sym_tok = tok_find_symbol(src, lx.toks, lx.count, s);
    if (sym_tok < 0) {
        lex_free(&lx);
        free(src);
        return false;
    }
    for (i = sym_tok + 1; i < lx.count; i++) {
        if (tok_is_punct(src, &lx.toks[i], ';'))
            break;
        if (tok_is_punct(src, &lx.toks[i], '}'))
            break;
        if (tok_is_punct(src, &lx.toks[i], '{')) {
            open = i;
            break;
        }
    }
    if (open < 0) {
        lex_free(&lx);
        free(src);
        return false;
    }
    close = tok_matching_brace(src, lx.toks, lx.count, open);
    if (close < 0) {
        lex_free(&lx);
        free(src);
        return false;
    }
    collect_members(src, lx.toks, lx.count, open, close, out);
    lex_free(&lx);
    free(src);
    return true;
}

static bool aggregate_members_internal(Project *p, const char *key,
                                       ProjectMemberList *out, int depth);

static bool collect_typedef_body(Project *p, const Symbol *s,
                                 ProjectMemberList *out, int depth)
{
    char *src;
    int len = 0;
    Lexer lx;
    int *tdepth;
    int alias;
    int adepth;
    int start = -1;
    int end = -1;
    int i;
    bool found = false;

    src = read_project_source(p, s->file_index, &len);
    if (!src)
        return false;
    lex_source(src, len, &lx);
    alias = tok_find_symbol(src, lx.toks, lx.count, s);
    if (alias < 0) {
        lex_free(&lx);
        free(src);
        return false;
    }
    tdepth = tok_depths(src, lx.toks, lx.count);
    adepth = tdepth[alias];
    for (i = alias - 1; i >= 0; i--) {
        if (tdepth[i] < adepth)
            break;
        if (tdepth[i] == adepth && tok_is_punct(src, &lx.toks[i], ';'))
            break;
        if (tdepth[i] == adepth &&
            tok_is_text(src, &lx.toks[i], "typedef")) {
            start = i;
            break;
        }
    }
    if (start >= 0) {
        for (i = alias + 1; i < lx.count; i++) {
            if (tdepth[i] < adepth)
                break;
            if (tdepth[i] == adepth && tok_is_punct(src, &lx.toks[i], ';')) {
                end = i;
                break;
            }
        }
    }
    if (start >= 0 && end > start) {
        for (i = start + 1; i < alias; i++) {
            int open;
            int close;
            int j;
            if (!(tok_is_text(src, &lx.toks[i], "struct") ||
                  tok_is_text(src, &lx.toks[i], "union") ||
                  tok_is_text(src, &lx.toks[i], "class")))
                continue;
            j = i + 1;
            if (j < alias && tok_is_ident(&lx.toks[j]))
                j++;
            if (j >= alias || !tok_is_punct(src, &lx.toks[j], '{'))
                continue;
            open = j;
            close = tok_matching_brace(src, lx.toks, lx.count, open);
            if (close < 0 || close > alias)
                continue;
            collect_members(src, lx.toks, lx.count, open, close, out);
            found = true;
            break;
        }
        if (!found) {
            for (i = alias - 1; i > start; i--) {
                const char *text;
                size_t n;
                char *type;
                if (!tok_is_ident(&lx.toks[i]))
                    continue;
                text = src + lx.toks[i].start;
                n = (size_t)lx.toks[i].len;
                if (type_word_qualifier(text, n) ||
                    type_word_aggregate_keyword(text, n) ||
                    type_word_builtin(text, n))
                    continue;
                type = xstrndup(text, n);
                found = aggregate_members_internal(p, type, out,
                                                   depth + 1);
                free(type);
                break;
            }
        }
    }
    free(tdepth);
    lex_free(&lx);
    free(src);
    return found;
}

static bool aggregate_members_internal(Project *p, const char *key,
                                       ProjectMemberList *out, int depth)
{
    IntList ids;
    size_t i;
    bool found = false;

    if (depth > 8 || !key || !*key)
        return false;
    intlist_init(&ids);
    project_find_symbols(p, key, &ids);
    for (i = 0; i < ids.count; i++) {
        Symbol *s = &p->syms[ids.items[i]];
        if (s->kind == SYM_STRUCT || s->kind == SYM_UNION ||
            s->kind == SYM_CLASS) {
            if (collect_symbol_body(p, s, out))
                found = true;
        } else if (s->kind == SYM_TYPEDEF) {
            if (collect_typedef_body(p, s, out, depth))
                found = true;
        }
    }
    intlist_free(&ids);
    return found;
}

bool project_aggregate_members(Project *p, const char *key,
                               ProjectMemberList *out)
{
    return aggregate_members_internal(p, key, out, 0);
}

/* ------------------------------------------------------------------ */
/* Project scan                                                        */
/* ------------------------------------------------------------------ */

static bool hidden_name(const char *name)
{
    return name[0] == '.';
}

static bool skip_dir_name(const char *name)
{
    static const char *const skip[] = {
        "node_modules", "__pycache__", "build", "out", "target", "dist",
        "cmake-build-debug", "cmake-build-release", "CMakeFiles"
    };
    size_t i;
    if (hidden_name(name))
        return true;
    for (i = 0; i < SI_ARRAY_LEN(skip); i++) {
        if (strcmp(name, skip[i]) == 0)
            return true;
    }
    return false;
}

static void project_add_file(Project *p, const char *abs, const char *rel)
{
    ProjectFile *f;
    if (p->file_count >= SI_MAX_PROJECT_FILES)
        return;
    if (p->file_count == p->file_cap) {
        p->file_cap = p->file_cap ? p->file_cap * 2 : 128;
        p->files = xrealloc(p->files,
                            p->file_cap * sizeof(*p->files));
    }
    f = &p->files[p->file_count++];
    memset(f, 0, sizeof(*f));
    /*
     * abs is already absolute: scan_dir started from path_abs(root).
     * Calling realpath/_fullpath for every file was a measurable startup
     * cost for trees with tens of thousands of entries.
     */
    f->path = xstrdup(abs);
    path_to_slashes(f->path);
    f->rel = xstrdup(rel);
    f->name = xstrdup(path_basename(abs));
    f->size = 0;
}

static void scan_dir(Project *p, const char *abs, const char *rel,
                     int depth)
{
    DirIter *it;
    DirEntry ent;

    if (depth > 64 || p->file_count >= SI_MAX_PROJECT_FILES)
        return;
    if (depth > p->scan_depth)
        p->scan_depth = depth;
    it = dir_open(abs);
    if (!it)
        return;
    while (dir_next(it, &ent)) {
        char *child_abs;
        char *child_rel;
        if (hidden_name(ent.name))
            continue;
        if (ent.is_symlink && ent.is_dir)
            continue;
        child_abs = path_join(abs, ent.name);
        child_rel = rel && *rel ? xasprintf("%s/%s", rel, ent.name)
                                : xstrdup(ent.name);
        if (ent.is_dir) {
            if (!skip_dir_name(ent.name))
                scan_dir(p, child_abs, child_rel, depth + 1);
        } else if (has_source_ext(ent.name)) {
            path_to_slashes(child_rel);
            project_add_file(p, child_abs, child_rel);
        }
        free(child_abs);
        free(child_rel);
        if (p->file_count >= SI_MAX_PROJECT_FILES)
            break;
    }
    dir_close(it);
}

static int cmp_project_file(const void *a, const void *b)
{
    const ProjectFile *fa = a;
    const ProjectFile *fb = b;
    return strcmp(fa->rel, fb->rel);
}

static int cmp_symbol(const void *a, const void *b)
{
    const Symbol *sa = a;
    const Symbol *sb = b;
    int c;
    if (sa->file_index != sb->file_index)
        return sa->file_index - sb->file_index;
    if (sa->line != sb->line)
        return sa->line - sb->line;
    if (sa->col != sb->col)
        return sa->col - sb->col;
    c = (int)sa->kind - (int)sb->kind;
    if (c)
        return c;
    return strcmp(sa->name, sb->name);
}

static void project_dedup_symbols(Project *p)
{
    size_t i;
    size_t out = 0;
    for (i = 0; i < p->sym_count; i++) {
        if (out > 0) {
            Symbol *prev = &p->syms[out - 1];
            Symbol *cur = &p->syms[i];
            if (prev->file_index == cur->file_index &&
                prev->line == cur->line && prev->col == cur->col &&
                prev->kind == cur->kind &&
                strcmp(prev->name, cur->name) == 0) {
                free(cur->name);
                free(cur->signature);
                continue;
            }
        }
        if (out != i)
            p->syms[out] = p->syms[i];
        out++;
    }
    p->sym_count = out;
}

static void project_assign_ranges(Project *p)
{
    size_t fi;
    size_t si = 0;
    for (fi = 0; fi < p->file_count; fi++) {
        size_t start = si;
        while (si < p->sym_count &&
               p->syms[si].file_index == (int)fi)
            si++;
        p->files[fi].sym_start = (int)start;
        p->files[fi].sym_count = (int)(si - start);
    }
}

static void project_warn(Project *p, const char *fmt, ...)
{
    va_list ap;
    Str s;
    str_init(&s);
    va_start(ap, fmt);
    str_vprintf(&s, fmt, ap);
    va_end(ap);
    free(p->last_warning);
    p->last_warning = s.data;
    p->warnings++;
}

bool project_load(Project *p, const char *root, ProjectProgress progress,
                  void *ud, char **err)
{
    size_t i;
    char *abs;

    if (err)
        *err = NULL;
    memset(p, 0, sizeof(*p));
    if (!root || !*root) {
        if (err)
            *err = xstrdup("no project directory given");
        return false;
    }
    abs = path_abs(root);
    if (!path_is_dir(abs)) {
        if (err)
            *err = xasprintf("not a directory: %s", root);
        free(abs);
        return false;
    }
    p->root = abs;
    if (progress && !progress(ud, 0, 0, "scanning directory...")) {
        project_free(p);
        if (err)
            *err = xstrdup("indexing cancelled");
        return false;
    }
    scan_dir(p, p->root, "", 0);
    if (p->file_count == 0)
        project_warn(p, "no C/C++ source files found under %s", root);
    if (p->file_count >= SI_MAX_PROJECT_FILES)
        project_warn(p, "file limit %d reached; some files were skipped",
                     SI_MAX_PROJECT_FILES);

    if (p->file_count > 1)
        qsort(p->files, p->file_count, sizeof(*p->files), cmp_project_file);
    for (i = 0; i < p->file_count; i++) {
        char *text = NULL;
        size_t len = 0;
        char *rerr = NULL;
        long long fsize = 0;
        long long fmtime = 0;
        if (path_stat_info(p->files[i].path, &fsize, &fmtime)) {
            p->files[i].size = (size_t)fsize;
            p->files[i].mtime = fmtime;
        }
        if (!file_read_all(p->files[i].path, SI_MAX_FILE_SIZE, &text, &len,
                           &rerr)) {
            project_warn(p, "%s", rerr ? rerr : "cannot read file");
            free(rerr);
            continue;
        }
        if (len && memchr(text, '\0', len)) {
            project_warn(p, "skipped binary file: %s", p->files[i].rel);
            free(text);
            continue;
        }
        p->files[i].size = len;
        parse_source(p, (int)i, text, (int)len);
        free(text);
        if (progress &&
            !progress(ud, i + 1, p->sym_count, p->files[i].rel)) {
            project_free(p);
            if (err)
                *err = xstrdup("indexing cancelled");
            return false;
        }
    }

    if (p->sym_count > 1)
        qsort(p->syms, p->sym_count, sizeof(*p->syms), cmp_symbol);
    project_dedup_symbols(p);
    project_assign_ranges(p);
    return true;
}

void project_free(Project *p)
{
    size_t i;
    if (!p)
        return;
    for (i = 0; i < p->file_count; i++) {
        free(p->files[i].path);
        free(p->files[i].rel);
        free(p->files[i].name);
    }
    free(p->files);
    for (i = 0; i < p->sym_count; i++) {
        free(p->syms[i].name);
        free(p->syms[i].signature);
    }
    free(p->syms);
    free(p->root);
    free(p->last_warning);
    if (p->call_graph) {
        callgraph_free(p->call_graph);
        free(p->call_graph);
    }
    free(p->var_uses);
    memset(p, 0, sizeof(*p));
}

/* ------------------------------------------------------------------ */
/* On-disk project cache                                               */
/* ------------------------------------------------------------------ */

#define SI_CACHE_MAGIC "SICACH2\0"
#define SI_CACHE_VERSION 3u

static bool cache_write(FILE *f, const void *data, size_t len)
{
    return len == 0 || fwrite(data, 1, len, f) == len;
}

static bool cache_u32(FILE *f, uint32_t v)
{
    return cache_write(f, &v, sizeof(v));
}

static bool cache_i32(FILE *f, int32_t v)
{
    return cache_write(f, &v, sizeof(v));
}

static bool cache_u64(FILE *f, uint64_t v)
{
    return cache_write(f, &v, sizeof(v));
}

static bool cache_i64(FILE *f, int64_t v)
{
    return cache_write(f, &v, sizeof(v));
}

static bool cache_str(FILE *f, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 0x00FFFFFFu)
        return false;
    return cache_u32(f, (uint32_t)n) &&
           cache_write(f, s ? s : "", n);
}

static bool cache_rd(FILE *f, void *data, size_t len)
{
    return len == 0 || fread(data, 1, len, f) == len;
}

static bool cache_rd_u32(FILE *f, uint32_t *v)
{
    return cache_rd(f, v, sizeof(*v));
}

static bool cache_rd_i32(FILE *f, int32_t *v)
{
    return cache_rd(f, v, sizeof(*v));
}

static bool cache_rd_u64(FILE *f, uint64_t *v)
{
    return cache_rd(f, v, sizeof(*v));
}

static bool cache_rd_i64(FILE *f, int64_t *v)
{
    return cache_rd(f, v, sizeof(*v));
}

static char *cache_rd_str(FILE *f)
{
    uint32_t n;
    char *s;
    if (!cache_rd_u32(f, &n) || n > 4u * 1024u * 1024u)
        return NULL;
    s = xmalloc((size_t)n + 1);
    if (!cache_rd(f, s, n)) {
        free(s);
        return NULL;
    }
    s[n] = '\0';
    return s;
}

bool project_cache_save(const Project *p, char **err)
{
    char *path;
    char *tmp;
    FILE *f;
    uint32_t flags = p->relations_ready ? 1u : 0u;
    size_t i;
    bool ok = true;

    if (err)
        *err = NULL;
    if (!p || !p->root)
        return false;
    path = path_join(p->root, ".si-index");
    tmp = xasprintf("%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        if (err)
            *err = xasprintf("cannot write %s", tmp);
        free(path);
        free(tmp);
        return false;
    }
    ok = cache_write(f, SI_CACHE_MAGIC, 8) &&
         cache_u32(f, SI_CACHE_VERSION) &&
         cache_u32(f, flags) &&
         cache_i32(f, (int32_t)p->scan_depth) &&
         cache_u32(f, (uint32_t)p->file_count) &&
         cache_u32(f, (uint32_t)p->sym_count) &&
         cache_u32(f, (uint32_t)p->var_use_count) &&
         cache_u32(f, (uint32_t)(p->call_graph ? p->call_graph->count : 0));

    for (i = 0; ok && i < p->file_count; i++) {
        const ProjectFile *pf = &p->files[i];
        ok = cache_str(f, pf->rel) &&
             cache_u64(f, (uint64_t)pf->size) &&
             cache_i64(f, (int64_t)pf->mtime) &&
             cache_i32(f, (int32_t)pf->sym_start) &&
             cache_i32(f, (int32_t)pf->sym_count);
    }
    for (i = 0; ok && i < p->sym_count; i++) {
        const Symbol *s = &p->syms[i];
        ok = cache_str(f, s->name) &&
             cache_str(f, s->signature) &&
             cache_i32(f, (int32_t)s->kind) &&
             cache_i32(f, (int32_t)s->file_index) &&
             cache_i32(f, (int32_t)s->line) &&
             cache_i32(f, (int32_t)s->col) &&
             cache_i32(f, (int32_t)s->end_line);
    }
    if (ok && p->call_graph) {
        for (i = 0; i < p->call_graph->count; i++) {
            const CallEdge *e = &p->call_graph->items[i];
            ok = cache_i32(f, e->caller) &&
                 cache_i32(f, e->callee) &&
                 cache_i32(f, e->file_index) &&
                 cache_i32(f, e->line) &&
                 cache_i32(f, e->col) &&
                 cache_u32(f, e->is_reference ? 1u : 0u);
        }
    }
    for (i = 0; ok && i < p->var_use_count; i++) {
        const VarUse *v = &p->var_uses[i];
        ok = cache_i32(f, v->variable) &&
             cache_i32(f, v->function) &&
             cache_i32(f, v->file_index) &&
             cache_i32(f, v->line) &&
             cache_i32(f, v->col);
    }
    if (ok && fflush(f) != 0)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (ok) {
#if defined(_WIN32)
        remove(path);
#endif
        if (rename(tmp, path) != 0)
            ok = false;
    }
    if (!ok) {
        remove(tmp);
        if (err && !*err)
            *err = xasprintf("cannot write project cache");
    }
    free(path);
    free(tmp);
    return ok;
}

static bool project_cache_load(Project *p, const char *root,
                               ProjectProgress progress, void *ud)
{
    char *path = path_join(root, ".si-index");
    FILE *f = fopen(path, "rb");
    char magic[8];
    uint32_t version;
    uint32_t flags;
    int32_t scan_depth;
    uint32_t nfiles;
    uint32_t nsyms;
    uint32_t nvars;
    uint32_t nedges;
    uint32_t i;
    bool ok = true;

    free(path);
    memset(p, 0, sizeof(*p));
    if (!f)
        return false;
    if (!cache_rd(f, magic, 8) ||
        memcmp(magic, SI_CACHE_MAGIC, 8) != 0 ||
        !cache_rd_u32(f, &version) || version != SI_CACHE_VERSION ||
        !cache_rd_u32(f, &flags) ||
        !cache_rd_i32(f, &scan_depth) ||
        !cache_rd_u32(f, &nfiles) ||
        !cache_rd_u32(f, &nsyms) ||
        !cache_rd_u32(f, &nvars) ||
        !cache_rd_u32(f, &nedges)) {
        fclose(f);
        return false;
    }
    if (nfiles > SI_MAX_PROJECT_FILES || nsyms > SI_MAX_SYMBOLS ||
        nvars > SI_MAX_VAR_USES)
        ok = false;

    if (progress)
        progress(ud, 0, 0, "reading .si-index");
    p->root = xstrdup(root);
    p->scan_depth = scan_depth;
    if (ok && nfiles) {
        p->files = xcalloc(nfiles, sizeof(*p->files));
        p->file_count = nfiles;
        p->file_cap = nfiles;
    }
    for (i = 0; ok && i < nfiles; i++) {
        uint64_t size;
        int64_t mtime;
        int32_t sym_start;
        int32_t sym_count;
        char *rel = cache_rd_str(f);
        long long actual_size = 0;
        long long actual_mtime = 0;
        if (!rel) {
            ok = false;
            break;
        }
        if (!cache_rd_u64(f, &size) || !cache_rd_i64(f, &mtime) ||
            !cache_rd_i32(f, &sym_start) || !cache_rd_i32(f, &sym_count)) {
            free(rel);
            ok = false;
            break;
        }
        p->files[i].rel = rel;
        p->files[i].path = path_join(root, rel);
        path_to_slashes(p->files[i].path);
        p->files[i].name = xstrdup(path_basename(p->files[i].path));
        p->files[i].size = (size_t)size;
        p->files[i].mtime = (long long)mtime;
        p->files[i].sym_start = sym_start;
        p->files[i].sym_count = sym_count;
        if (!path_stat_info(p->files[i].path, &actual_size,
                            &actual_mtime) ||
            actual_size != (long long)size ||
            actual_mtime != (long long)mtime) {
            ok = false;
            break;
        }
        if (progress && (i % 128) == 0)
            progress(ud, i, 0, "reading .si-index");
    }
    if (ok && nsyms) {
        p->syms = xcalloc(nsyms, sizeof(*p->syms));
        p->sym_count = nsyms;
        p->sym_cap = nsyms;
    }
    for (i = 0; ok && i < nsyms; i++) {
        int32_t kind;
        int32_t fi;
        int32_t line;
        int32_t col;
        int32_t end_line;
        char *name = cache_rd_str(f);
        char *sig = cache_rd_str(f);
        if (!name || !sig ||
            !cache_rd_i32(f, &kind) || !cache_rd_i32(f, &fi) ||
            !cache_rd_i32(f, &line) || !cache_rd_i32(f, &col) ||
            !cache_rd_i32(f, &end_line)) {
            free(name);
            free(sig);
            ok = false;
            break;
        }
        p->syms[i].name = name;
        p->syms[i].signature = sig;
        p->syms[i].kind = (SymbolKind)kind;
        p->syms[i].file_index = fi;
        p->syms[i].line = line;
        p->syms[i].col = col;
        p->syms[i].end_line = end_line;
        if (progress && (i % 2048) == 0)
            progress(ud, nfiles, i, "reading .si-index");
    }
    if (ok) {
        p->call_graph = xcalloc(1, sizeof(*p->call_graph));
        for (i = 0; i < nedges; i++) {
            int32_t caller, callee, fi, line, col;
            uint32_t is_reference;
            if (!cache_rd_i32(f, &caller) || !cache_rd_i32(f, &callee) ||
                !cache_rd_i32(f, &fi) || !cache_rd_i32(f, &line) ||
                !cache_rd_i32(f, &col) ||
                !cache_rd_u32(f, &is_reference)) {
                ok = false;
                break;
            }
            callgraph_add(p->call_graph, caller, callee, fi, line, col,
                          is_reference != 0);
            if (progress && (i % 4096) == 0)
                progress(ud, nfiles, nsyms, "reading .si-index");
        }
    }
    if (ok && nvars) {
        p->var_uses = xcalloc(nvars, sizeof(*p->var_uses));
        p->var_use_count = nvars;
        p->var_use_cap = nvars;
    }
    for (i = 0; ok && i < nvars; i++) {
        int32_t variable, function, fi, line, col;
        if (!cache_rd_i32(f, &variable) ||
            !cache_rd_i32(f, &function) ||
            !cache_rd_i32(f, &fi) ||
            !cache_rd_i32(f, &line) ||
            !cache_rd_i32(f, &col)) {
            ok = false;
            break;
        }
        p->var_uses[i].variable = variable;
        p->var_uses[i].function = function;
        p->var_uses[i].file_index = fi;
        p->var_uses[i].line = line;
        p->var_uses[i].col = col;
        if (progress && (i % 4096) == 0)
            progress(ud, nfiles, nsyms, "reading .si-index");
    }
    fclose(f);
    if (!ok) {
        project_free(p);
        return false;
    }
    p->relations_ready = (flags & 1u) != 0;
    return true;
}

bool project_load_cached(Project *p, const char *root,
                         ProjectProgress progress, void *ud, char **err,
                         bool *from_cache)
{
    if (from_cache)
        *from_cache = false;
    if (project_cache_load(p, root, progress, ud)) {
        if (from_cache)
            *from_cache = true;
        if (err)
            *err = NULL;
        return true;
    }
    return project_load(p, root, progress, ud, err);
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

void project_file_symbols(Project *p, int file_index, IntList *out)
{
    int i;
    if (file_index < 0 || (size_t)file_index >= p->file_count)
        return;
    for (i = 0; i < p->files[file_index].sym_count; i++)
        intlist_push(out, p->files[file_index].sym_start + i);
}

void project_find_symbols(Project *p, const char *name, IntList *out)
{
    size_t i;
    if (!name || !*name)
        return;
    for (i = 0; i < p->sym_count; i++) {
        if (strcmp(p->syms[i].name, name) == 0)
            intlist_push(out, (int)i);
    }
}

void project_search_symbols(Project *p, const char *query, IntList *out)
{
    size_t i;
    if (!query || !*query)
        return;
    for (i = 0; i < p->sym_count; i++) {
        if (str_icontains(p->syms[i].name, query)) {
            intlist_push(out, (int)i);
            if (out->count >= 1000)
                break;
        }
    }
}

static char *line_preview(const char *src, int len, int start, int end)
{
    char *raw;
    char *trim;
    size_t n;
    (void)len;
    if (end < start)
        end = start;
    n = (size_t)(end - start);
    if (n > 240)
        n = 240;
    raw = xstrndup(src + start, n);
    trim = str_trim_dup(raw, strlen(raw));
    free(raw);
    return trim;
}

int project_find_file_by_rel(Project *p, const char *rel)
{
    size_t i;
    const char *q = rel;
    if (!rel || !*rel)
        return -1;
    while (q[0] == '.' && (q[1] == '/' || q[1] == '\\'))
        q += 2;
    if (!*q)
        return -1;
    for (i = 0; i < p->file_count; i++) {
        if (strcmp(p->files[i].rel, q) == 0)
            return (int)i;
    }
    for (i = 0; i < p->file_count; i++) {
        size_t rl = strlen(p->files[i].rel);
        size_t ql = strlen(q);
        if (ql <= rl && strcmp(p->files[i].rel + (rl - ql), q) == 0 &&
            (rl == ql || p->files[i].rel[rl - ql - 1] == '/'))
            return (int)i;
        if (strcmp(p->files[i].name, q) == 0)
            return (int)i;
    }
    return -1;
}

int project_file_index_of_path(Project *p, const char *path)
{
    size_t i;
    char *abs;
    if (!path)
        return -1;
    abs = path_abs(path);
    path_to_slashes(abs);
    for (i = 0; i < p->file_count; i++) {
        if (strcmp(p->files[i].path, abs) == 0) {
            free(abs);
            return (int)i;
        }
    }
    free(abs);
    return -1;
}

void project_find_refs(Project *p, const char *name, LocationList *out)
{
    size_t fi;
    size_t nl = strlen(name);
    if (!name || !*name)
        return;

    for (fi = 0; fi < p->file_count && out->count < SI_MAX_REFS; fi++) {
        char *src = NULL;
        size_t len = 0;
        char *err = NULL;
        Lexer lx;
        int i;
        if (!file_read_all(p->files[fi].path, SI_MAX_FILE_SIZE, &src, &len,
                           &err)) {
            free(err);
            continue;
        }
        if (len && memchr(src, '\0', len)) {
            free(src);
            continue;
        }
        lex_source(src, (int)len, &lx);
        for (i = 0; i < lx.count && out->count < SI_MAX_REFS; i++) {
            Tok *t = &lx.toks[i];
            char *preview;
            if (t->kind != TK_IDENT || (size_t)t->len != nl ||
                memcmp(src + t->start, name, nl) != 0)
                continue;
            preview = line_preview(src, (int)len, t->line_start,
                                   t->line_start + (int)strcspn(src + t->line_start, "\n"));
            loclist_push(out, (int)fi, t->line, t->col, preview);
            free(preview);
        }
        lex_free(&lx);
        free(src);
    }
}

void symbol_context_init(SymbolContext *ctx)
{
    ctx->symbol_index = -1;
    loclist_init(&ctx->callers);
    intlist_init(&ctx->callees);
    ctx->ref_count = 0;
}

void symbol_context_free(SymbolContext *ctx)
{
    loclist_free(&ctx->callers);
    intlist_free(&ctx->callees);
    ctx->symbol_index = -1;
    ctx->ref_count = 0;
}

static int enclosing_function(Project *p, int file_index, int line)
{
    int i;
    int best = -1;
    int best_span = 0;
    if (file_index < 0 || (size_t)file_index >= p->file_count)
        return -1;
    for (i = 0; i < p->files[file_index].sym_count; i++) {
        int si = p->files[file_index].sym_start + i;
        Symbol *s = &p->syms[si];
        int span;
        if (s->kind != SYM_FUNCTION)
            continue;
        if (line < s->line || line > s->end_line)
            continue;
        span = s->end_line - s->line;
        if (best < 0 || span < best_span) {
            best = si;
            best_span = span;
        }
    }
    return best;
}

static int callable_symbol(Project *p, Symbol **funcs, size_t nfuncs,
                           const char *name, int file_index)
{
    size_t lo = 0;
    size_t hi = nfuncs;
    size_t i;
    int first_def = -1;
    int prototype = -1;
    int same_file_proto = -1;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(funcs[mid]->name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < nfuncs && strcmp(funcs[i]->name, name) == 0; i++) {
        Symbol *s = funcs[i];
        if (s->kind == SYM_FUNCTION) {
            if (s->file_index == file_index)
                return (int)(s - p->syms);
            if (first_def < 0)
                first_def = (int)(s - p->syms);
        } else if (s->kind == SYM_PROTOTYPE) {
            if (prototype < 0)
                prototype = (int)(s - p->syms);
            if (s->file_index == file_index && same_file_proto < 0)
                same_file_proto = (int)(s - p->syms);
        }
    }
    if (first_def >= 0)
        return first_def;
    if (same_file_proto >= 0)
        return same_file_proto;
    return prototype;
}

static bool call_reserved(const char *s, int len)
{
    static const char *const words[] = {
        "if", "for", "while", "switch", "return", "sizeof", "do",
        "case", "catch", "throw", "new", "delete", "typeof",
        "__typeof__", "alignof", "decltype", "static_assert", "assert"
    };
    size_t i;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        size_t n = strlen(words[i]);
        if ((int)n == len && memcmp(words[i], s, n) == 0)
            return true;
    }
    return false;
}

static bool int_contains(const IntList *l, int value)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        if (l->items[i] == value)
            return true;
    }
    return false;
}

static void callgraph_add(CallGraph *g, int caller, int callee, int fi,
                          int line, int col, bool is_reference)
{
    size_t i;
    if (caller == callee)
        return;
    for (i = 0; i < g->count; i++) {
        CallEdge *e = &g->items[i];
        if (e->caller == caller && e->callee == callee &&
            e->file_index == fi && e->line == line && e->col == col &&
            e->is_reference == is_reference)
            return;
    }
    if (g->count == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 128;
        g->items = xrealloc(g->items, g->cap * sizeof(*g->items));
    }
    g->items[g->count].caller = caller;
    g->items[g->count].callee = callee;
    g->items[g->count].file_index = fi;
    g->items[g->count].line = line;
    g->items[g->count].col = col;
    g->items[g->count].is_reference = is_reference;
    g->count++;
}

static void callgraph_free(CallGraph *g)
{
    free(g->items);
    g->items = NULL;
    g->count = 0;
    g->cap = 0;
}

static void varuse_add(Project *p, int variable, int function, int fi,
                       int line, int col)
{
    if (p->var_use_count >= SI_MAX_VAR_USES)
        return;
    if (p->var_use_count == p->var_use_cap) {
        p->var_use_cap = p->var_use_cap ? p->var_use_cap * 2 : 256;
        p->var_uses = xrealloc(p->var_uses,
                               p->var_use_cap * sizeof(*p->var_uses));
    }
    p->var_uses[p->var_use_count].variable = variable;
    p->var_uses[p->var_use_count].function = function;
    p->var_uses[p->var_use_count].file_index = fi;
    p->var_uses[p->var_use_count].line = line;
    p->var_uses[p->var_use_count].col = col;
    p->var_use_count++;
}

static int cmp_symbol_name_ptr(const void *a, const void *b)
{
    const Symbol *sa = *(Symbol *const *)a;
    const Symbol *sb = *(Symbol *const *)b;
    return strcmp(sa->name, sb->name);
}

static int lookup_global_variable(Project *p, Symbol **vars, size_t nvars,
                                  const char *name)
{
    size_t lo = 0;
    size_t hi = nvars;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(vars[mid]->name, name);
        if (c < 0)
            lo = mid + 1;
        else if (c > 0)
            hi = mid;
        else
            return (int)(vars[mid] - p->syms);
    }
    return -1;
}

/*
 * Build a real call graph from call expressions inside function bodies.
 * This deliberately does not use every identifier reference: a variable
 * named like a function must not become a call edge.
 *
 * It also records global-variable uses inside functions.  Local variables
 * are deliberately approximate here; the editor's LocalVar table refines
 * shadowing for the active file.
 */
static bool build_call_graph(Project *p, CallGraph *g,
                             ProjectProgress progress, void *ud)
{
    size_t fi;
    Symbol **vars = NULL;
    Symbol **funcs = NULL;
    size_t nvars = 0;
    size_t nfuncs = 0;
    size_t vi;

    memset(g, 0, sizeof(*g));

    /* Build name-sorted tables for O(log n) variable/function lookups. */
    if (p->sym_count) {
        vars = xmalloc(p->sym_count * sizeof(*vars));
        funcs = xmalloc(p->sym_count * sizeof(*funcs));
        for (vi = 0; vi < p->sym_count; vi++) {
            if (p->syms[vi].kind == SYM_VARIABLE)
                vars[nvars++] = &p->syms[vi];
            else if (p->syms[vi].kind == SYM_FUNCTION ||
                     p->syms[vi].kind == SYM_PROTOTYPE)
                funcs[nfuncs++] = &p->syms[vi];
        }
        if (nvars > 1)
            qsort(vars, nvars, sizeof(*vars), cmp_symbol_name_ptr);
        if (nfuncs > 1)
            qsort(funcs, nfuncs, sizeof(*funcs), cmp_symbol_name_ptr);
    }

    for (fi = 0; fi < p->file_count; fi++) {
        char *src = NULL;
        size_t len = 0;
        char *err = NULL;
        Lexer lx;
        int ti;
        int sym_begin;
        int sym_end;
        int cur_sym = -1;
        bool in_body = false;

        if (!file_read_all(p->files[fi].path, SI_MAX_FILE_SIZE, &src, &len,
                           &err)) {
            free(err);
            continue;
        }
        if (len && memchr(src, '\0', len)) {
            free(src);
            continue;
        }
        lex_source(src, (int)len, &lx);
        sym_begin = p->files[fi].sym_start;
        sym_end = sym_begin + p->files[fi].sym_count;

        for (ti = 0; ti < lx.count; ti++) {
            Tok *t = &lx.toks[ti];
            int ci;
            Symbol *caller;
            bool is_call;

            while (sym_begin < sym_end &&
                   (p->syms[sym_begin].kind != SYM_FUNCTION ||
                    p->syms[sym_begin].end_line < t->line))
                sym_begin++;
            if (sym_begin >= sym_end)
                break;
            if (p->syms[sym_begin].line > t->line)
                continue;
            if (cur_sym != sym_begin) {
                cur_sym = sym_begin;
                in_body = false;
            }
            if (t->kind == TK_PUNCT && src[t->start] == '{') {
                in_body = true;
                continue;
            }
            if (!in_body || t->kind != TK_IDENT)
                continue;

            is_call = ti + 1 < lx.count &&
                      lx.toks[ti + 1].kind == TK_PUNCT &&
                      src[lx.toks[ti + 1].start] == '(';

            {
                bool member_access = false;
                bool handled = false;
                if (ti > 0) {
                    if (tok_is_punct(src, &lx.toks[ti - 1], '.')) {
                        member_access = true;
                    } else if (tok_is_punct(src, &lx.toks[ti - 1], '>') &&
                               ti > 1 &&
                               tok_is_punct(src, &lx.toks[ti - 2], '-')) {
                        member_access = true;
                    }
                }
                if (is_call) {
                    if (call_reserved(src + t->start, t->len))
                        continue;
                    caller = &p->syms[cur_sym];
                    if ((size_t)t->len == strlen(caller->name) &&
                        memcmp(src + t->start, caller->name,
                               (size_t)t->len) == 0)
                        continue; /* self-recursion is not a useful edge */
                    {
                        char *name = xstrndup(src + t->start,
                                              (size_t)t->len);
                        ci = callable_symbol(p, funcs, nfuncs, name, (int)fi);
                        free(name);
                    }
                    if (ci >= 0)
                        callgraph_add(g, cur_sym, ci, (int)fi,
                                      t->line, t->col, false);
                } else {
                    if (nvars > 0) {
                        char *name = xstrndup(src + t->start,
                                              (size_t)t->len);
                        int var = lookup_global_variable(p, vars, nvars, name);
                        free(name);
                        if (var >= 0) {
                            varuse_add(p, var, cur_sym, (int)fi,
                                       t->line, t->col);
                            handled = true;
                        }
                    }
                    if (!handled && !member_access && nfuncs > 0 &&
                        !call_reserved(src + t->start, t->len)) {
                        char *name = xstrndup(src + t->start,
                                              (size_t)t->len);
                        ci = callable_symbol(p, funcs, nfuncs, name, (int)fi);
                        free(name);
                        if (ci >= 0)
                            callgraph_add(g, cur_sym, ci, (int)fi,
                                          t->line, t->col, true);
                    }
                }
            }
        }
        lex_free(&lx);
        free(src);
        if (progress &&
            !progress(ud, fi + 1, p->sym_count, p->files[fi].rel)) {
            free(vars);
            free(funcs);
            return false;
        }
    }
    free(vars);
    free(funcs);
    return true;
}

static CallGraph *project_call_graph(Project *p)
{
    if (!p->call_graph) {
        p->call_graph = xcalloc(1, sizeof(*p->call_graph));
        if (!build_call_graph(p, p->call_graph, NULL, NULL)) {
            callgraph_free(p->call_graph);
            free(p->call_graph);
            p->call_graph = NULL;
        }
    }
    return p->call_graph;
}

bool project_build_relations(Project *p, ProjectProgress progress, void *ud,
                             char **err)
{
    if (!p) {
        if (err)
            *err = xstrdup("no project");
        return false;
    }
    if (p->call_graph) {
        callgraph_free(p->call_graph);
        free(p->call_graph);
        p->call_graph = NULL;
    }
    free(p->var_uses);
    p->var_uses = NULL;
    p->var_use_count = 0;
    p->var_use_cap = 0;
    p->relations_ready = false;

    p->call_graph = xcalloc(1, sizeof(*p->call_graph));
    if (!build_call_graph(p, p->call_graph, progress, ud)) {
        callgraph_free(p->call_graph);
        free(p->call_graph);
        p->call_graph = NULL;
        free(p->var_uses);
        p->var_uses = NULL;
        p->var_use_count = 0;
        p->var_use_cap = 0;
        if (err)
            *err = xstrdup("relation table build cancelled");
        return false;
    }
    p->relations_ready = true;
    return true;
}

bool project_relations_ready(const Project *p)
{
    return p && p->relations_ready;
}

size_t project_call_edge_count(const Project *p)
{
    return p && p->call_graph ? p->call_graph->count : 0;
}

size_t project_var_use_count(const Project *p)
{
    return p ? p->var_use_count : 0;
}

static void graph_callees(const CallGraph *g, int caller, IntList *out)
{
    size_t i;
    for (i = 0; i < g->count; i++) {
        if (g->items[i].caller == caller &&
            !int_contains(out, g->items[i].callee))
            intlist_push(out, g->items[i].callee);
    }
}

static void caller_preview(Project *p, const CallEdge *e, char *buf,
                           size_t size)
{
    Symbol *s = &p->syms[e->caller];
    snprintf(buf, size, "%s%s", e->is_reference ? "[ref] " : "",
             s->signature);
}

static void caller_tree_walk(Project *p, const CallGraph *g, int symbol_index,
                             int depth, int max_depth, IntList *seen,
                             LocationList *out, int *nodes)
{
    size_t i;
    int pass;
    if (depth >= max_depth || *nodes >= 200)
        return;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < g->count && *nodes < 200; i++) {
            const CallEdge *e = &g->items[i];
            char preview[512];
            if (e->callee != symbol_index)
                continue;
            if (e->is_reference != (pass == 0))
                continue;
            if (int_contains(seen, e->caller))
                continue;
            intlist_push(seen, e->caller);
            caller_preview(p, e, preview, sizeof(preview));
            loclist_push_depth(out, e->file_index, e->line, e->col,
                               depth + 1, preview);
            (*nodes)++;
            caller_tree_walk(p, g, e->caller, depth + 1, max_depth, seen,
                             out, nodes);
        }
    }
}

bool project_caller_tree(Project *p, int symbol_index, LocationList *out,
                         int max_depth)
{
    CallGraph *graph;
    IntList seen;
    int nodes = 0;
    if (!p || !out || symbol_index < 0 ||
        (size_t)symbol_index >= p->sym_count)
        return false;
    if (max_depth < 1)
        max_depth = 1;
    if (max_depth > 10)
        max_depth = 10;
    loclist_init(out);
    graph = project_call_graph(p);
    if (!graph)
        return false;
    intlist_init(&seen);
    intlist_push(&seen, symbol_index);
    if (p->syms[symbol_index].kind == SYM_FUNCTION) {
        caller_tree_walk(p, graph, symbol_index, 0, max_depth, &seen, out,
                         &nodes);
    } else {
        LocationList refs;
        IntList callers;
        size_t i;
        loclist_init(&refs);
        project_find_refs(p, p->syms[symbol_index].name, &refs);
        intlist_init(&callers);
        for (i = 0; i < refs.count && nodes < 200; i++) {
            int caller = enclosing_function(p, refs.items[i].file_index,
                                            refs.items[i].line);
            Symbol *s;
            if (caller < 0 || int_contains(&seen, caller))
                continue;
            intlist_push(&seen, caller);
            s = &p->syms[caller];
            loclist_push_depth(out, refs.items[i].file_index,
                               refs.items[i].line, refs.items[i].col, 1,
                               s->signature);
            nodes++;
            caller_tree_walk(p, graph, caller, 1, max_depth, &seen, out,
                             &nodes);
        }
        intlist_free(&callers);
        loclist_free(&refs);
    }
    intlist_free(&seen);
    return true;
}

bool project_context(Project *p, int symbol_index, SymbolContext *ctx)
{
    Symbol *sym;
    LocationList refs;
    IntList caller_ids;
    IntList callee_ids;
    CallGraph *graph;
    size_t i;
    int pass;

    if (!p || !ctx || symbol_index < 0 ||
        (size_t)symbol_index >= p->sym_count)
        return false;
    symbol_context_init(ctx);
    ctx->symbol_index = symbol_index;
    sym = &p->syms[symbol_index];

    loclist_init(&refs);
    project_find_refs(p, sym->name, &refs);
    ctx->ref_count = (int)refs.count;

    intlist_init(&caller_ids);
    intlist_init(&callee_ids);
    graph = project_call_graph(p);
    if (!graph) {
        loclist_free(&refs);
        intlist_free(&caller_ids);
        intlist_free(&callee_ids);
        return false;
    }
    if (sym->kind == SYM_FUNCTION) {
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < graph->count; i++) {
                const CallEdge *e = &graph->items[i];
                char preview[512];
                if (e->callee != symbol_index)
                    continue;
                if (e->is_reference != (pass == 0))
                    continue;
                if (int_contains(&caller_ids, e->caller))
                    continue;
                intlist_push(&caller_ids, e->caller);
                caller_preview(p, e, preview, sizeof(preview));
                loclist_push(&ctx->callers, e->file_index, e->line, e->col,
                             preview);
            }
        }
        graph_callees(graph, symbol_index, &callee_ids);
    } else {
        /* For variables, the "callers" are the functions containing a
         * reference to the variable; use the reference itself as the site. */
        for (i = 0; i < refs.count; i++) {
            int caller = enclosing_function(p, refs.items[i].file_index,
                                            refs.items[i].line);
            Symbol *cs;
            if (caller < 0 || int_contains(&caller_ids, caller))
                continue;
            intlist_push(&caller_ids, caller);
            cs = &p->syms[caller];
            loclist_push(&ctx->callers, refs.items[i].file_index,
                         refs.items[i].line, refs.items[i].col,
                         cs->signature);
        }
    }
    for (i = 0; i < callee_ids.count; i++)
        intlist_push(&ctx->callees, callee_ids.items[i]);
    loclist_free(&refs);
    intlist_free(&caller_ids);
    intlist_free(&callee_ids);
    return true;
}

static bool line_has_ci(const char *s, size_t n, const char *query,
                        int *at)
{
    size_t qn = strlen(query);
    size_t i;
    if (qn == 0 || qn > n)
        return false;
    for (i = 0; i + qn <= n; i++) {
        size_t j;
        for (j = 0; j < qn; j++) {
            unsigned char a = (unsigned char)s[i + j];
            unsigned char b = (unsigned char)query[j];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + ('a' - 'A'));
            if (a != b)
                break;
        }
        if (j == qn) {
            *at = (int)i;
            return true;
        }
    }
    return false;
}

void project_search_text(Project *p, const char *query, LocationList *out)
{
    size_t fi;
    if (!query || !*query)
        return;
    for (fi = 0; fi < p->file_count && out->count < 1000; fi++) {
        char *src = NULL;
        size_t len = 0;
        char *err = NULL;
        size_t pos = 0;
        int line = 1;
        if (!file_read_all(p->files[fi].path, SI_MAX_FILE_SIZE, &src, &len,
                           &err)) {
            free(err);
            continue;
        }
        if (len && memchr(src, '\0', len)) {
            free(src);
            continue;
        }
        while (pos <= len && out->count < 1000) {
            size_t end = pos;
            int at = 0;
            char *preview;
            while (end < len && src[end] != '\n')
                end++;
            if (line_has_ci(src + pos, end - pos, query, &at)) {
                preview = line_preview(src, (int)len, (int)pos, (int)end);
                loclist_push(out, (int)fi, line, at, preview);
                free(preview);
            }
            if (end >= len)
                break;
            pos = end + 1;
            line++;
        }
        free(src);
    }
}
