#include "si.h"
#include "syntax.h"
#include "text.h"

#include <ctype.h>

static const char *const keywords[] = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
    "bitor", "break", "case", "catch", "char8_t", "char16_t",
    "char32_t", "class", "co_await", "co_return", "co_yield", "compl",
    "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "decltype", "default", "delete", "do",
    "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "false", "for", "friend", "goto", "if", "inline", "mutable",
    "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "sizeof",
    "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true", "try",
    "typedef", "typeid", "typename", "union", "using", "virtual",
    "volatile", "while", "xor", "xor_eq",
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
    "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"
};

static const char *const types[] = {
    "bool", "char", "double", "float", "int", "long", "short",
    "signed", "unsigned", "void", "wchar_t", "size_t", "ssize_t",
    "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "off_t", "time_t", "clock_t", "FILE",
    "DIR", "pid_t", "mode_t", "uid_t", "gid_t", "socklen_t",
    "va_list", "stdint", "uint", "ulong", "uchar", "ushort"
};

static bool name_in(const char *s, size_t len, const char *const *list,
                    size_t count)
{
    size_t i;
    size_t j;
    for (i = 0; i < count; i++) {
        const char *w = list[i];
        for (j = 0; j < len; j++) {
            if (!w[j] || s[j] != w[j])
                break;
        }
        if (j == len && w[j] == '\0')
            return true;
    }
    return false;
}

static bool is_keyword(const char *s, size_t len)
{
    return name_in(s, len, keywords, SI_ARRAY_LEN(keywords));
}

static bool is_type(const char *s, size_t len)
{
    return name_in(s, len, types, SI_ARRAY_LEN(types));
}

size_t syntax_keyword_count(void)
{
    return SI_ARRAY_LEN(keywords);
}

const char *syntax_keyword_at(size_t index)
{
    return index < SI_ARRAY_LEN(keywords) ? keywords[index] : NULL;
}

size_t syntax_type_count(void)
{
    return SI_ARRAY_LEN(types);
}

const char *syntax_type_at(size_t index)
{
    return index < SI_ARRAY_LEN(types) ? types[index] : NULL;
}

void syntax_init(SyntaxCache *syn)
{
    syn->states = NULL;
    syn->count = 0;
    syn->cap = 0;
    syn->rev = 0;
}

void syntax_free(SyntaxCache *syn)
{
    free(syn->states);
    syn->states = NULL;
    syn->count = 0;
    syn->cap = 0;
    syn->rev = 0;
}

static int scan_string(const char *s, int len, int i, char quote)
{
    i++;
    while (i < len) {
        if (s[i] == '\\') {
            i += 2;
            continue;
        }
        if (s[i] == quote)
            return i + 1;
        i++;
    }
    return len;
}

void syntax_ensure(SyntaxCache *syn, Buffer *b)
{
    size_t n = buf_line_count(b);
    size_t line;
    unsigned char state = 0;

    if (syn->rev == b->rev && syn->count == n)
        return;
    if (syn->cap < n) {
        syn->cap = n + 16;
        syn->states = xrealloc(syn->states, syn->cap);
    }
    syn->count = n;

    for (line = 0; line < n; line++) {
        size_t start = buf_line_start(b, (int)line);
        size_t end = buf_line_content_end(b, (int)line);
        int i = (int)start;
        int lim = (int)end;
        syn->states[line] = state;

        while (i < lim) {
            char c = b->data[i];
            if (state) {
                if (c == '*' && i + 1 < lim && b->data[i + 1] == '/') {
                    state = 0;
                    i += 2;
                } else {
                    i++;
                }
            } else if (c == '/' && i + 1 < lim && b->data[i + 1] == '/') {
                break;
            } else if (c == '/' && i + 1 < lim && b->data[i + 1] == '*') {
                state = 1;
                i += 2;
            } else if (c == '"' || c == '\'') {
                i = scan_string(b->data, lim, i, c);
            } else {
                i++;
            }
        }
    }
    syn->rev = b->rev;
}

static void emit_span(HlSpan *spans, int max, int *count,
                      int start, int end, HlKind kind)
{
    if (start >= end || *count >= max)
        return;
    /* Keep normal spans token-sized: the renderer later refines normal
     * identifiers into globals/locals, so it must see each identifier
     * separately rather than one merged run of whitespace + words. */
    if (kind != HL_NORMAL && *count > 0 &&
        spans[*count - 1].end == start &&
        spans[*count - 1].kind == kind) {
        spans[*count - 1].end = end;
        return;
    }
    spans[*count].start = start;
    spans[*count].end = end;
    spans[*count].kind = kind;
    (*count)++;
}

static bool operator_char(char c)
{
    return c && strchr("+-*/%=<>!&|^~?:", c) != NULL;
}

int syntax_line_spans(SyntaxCache *syn, Buffer *b, int line,
                      HlSpan *spans, int max_spans)
{
    size_t start;
    size_t end;
    int i;
    int lim;
    int count = 0;
    unsigned char state;
    bool preproc = false;

    syntax_ensure(syn, b);
    if (line < 0 || (size_t)line >= buf_line_count(b))
        return 0;
    start = buf_line_start(b, line);
    end = buf_line_content_end(b, line);
    i = (int)start;
    lim = (int)end;
    state = (syn->states && (size_t)line < syn->count) ? syn->states[line] : 0;

    if (!state) {
        int p = i;
        while (p < lim && (b->data[p] == ' ' || b->data[p] == '\t'))
            p++;
        if (p < lim && b->data[p] == '#')
            preproc = true;
    }

    while (i < lim) {
        char c = b->data[i];
        int begin = i;

        if (state) {
            while (i < lim) {
                if (b->data[i] == '*' && i + 1 < lim &&
                    b->data[i + 1] == '/') {
                    i += 2;
                    state = 0;
                    break;
                }
                i++;
            }
            emit_span(spans, max_spans, &count, begin, i, HL_COMMENT);
            continue;
        }
        if (c == '/' && i + 1 < lim && b->data[i + 1] == '/') {
            emit_span(spans, max_spans, &count, i, lim, HL_COMMENT);
            break;
        }
        if (c == '/' && i + 1 < lim && b->data[i + 1] == '*') {
            state = 1;
            i += 2;
            while (i < lim) {
                if (b->data[i] == '*' && i + 1 < lim &&
                    b->data[i + 1] == '/') {
                    i += 2;
                    state = 0;
                    break;
                }
                i++;
            }
            emit_span(spans, max_spans, &count, begin, i, HL_COMMENT);
            continue;
        }
        if (c == '"' || c == '\'') {
            i = scan_string(b->data, lim, i, c);
            emit_span(spans, max_spans, &count, begin, i, HL_STRING);
            continue;
        }
        if (isspace((unsigned char)c)) {
            while (i < lim && isspace((unsigned char)b->data[i]))
                i++;
            emit_span(spans, max_spans, &count, begin, i,
                      preproc ? HL_PREPROC : HL_NORMAL);
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            bool func = false;
            int p;
            i++;
            while (i < lim && (isalnum((unsigned char)b->data[i]) ||
                               b->data[i] == '_'))
                i++;
            p = i;
            while (p < lim && (b->data[p] == ' ' || b->data[p] == '\t'))
                p++;
            func = p < lim && b->data[p] == '(';
            if (preproc) {
                emit_span(spans, max_spans, &count, begin, i, HL_PREPROC);
            } else if (is_keyword(b->data + begin, (size_t)(i - begin))) {
                emit_span(spans, max_spans, &count, begin, i, HL_KEYWORD);
            } else if (is_type(b->data + begin, (size_t)(i - begin))) {
                emit_span(spans, max_spans, &count, begin, i, HL_TYPE);
            } else if (func) {
                emit_span(spans, max_spans, &count, begin, i, HL_FUNC);
            } else {
                emit_span(spans, max_spans, &count, begin, i, HL_NORMAL);
            }
            continue;
        }
        if (isdigit((unsigned char)c)) {
            i++;
            while (i < lim && (isalnum((unsigned char)b->data[i]) ||
                               b->data[i] == '.' || b->data[i] == '_' ||
                               b->data[i] == '\''))
                i++;
            emit_span(spans, max_spans, &count, begin, i, HL_NUMBER);
            continue;
        }
        if (c == '#' && preproc) {
            i++;
            emit_span(spans, max_spans, &count, begin, i, HL_PREPROC);
            continue;
        }
        i++;
        emit_span(spans, max_spans, &count, begin, i,
                  preproc ? HL_PREPROC
                          : (operator_char(c) ? HL_OP : HL_NORMAL));
    }
    /* The renderer works in line-relative cells; callers should not need to
     * know the absolute file offset of the line. */
    {
        int k;
        for (k = 0; k < count; k++) {
            spans[k].start -= (int)start;
            spans[k].end -= (int)start;
        }
    }
    return count;
}
