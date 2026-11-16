#ifndef SI_SYNTAX_H
#define SI_SYNTAX_H

#include "si.h"

typedef struct Buffer Buffer;

typedef enum {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_STRING,
    HL_NUMBER,
    HL_KEYWORD,
    HL_TYPE,
    HL_PREPROC,
    HL_FUNC,
    HL_OP,
    HL_GLOBAL,
    HL_LOCAL
} HlKind;

typedef struct {
    int start;
    int end;
    HlKind kind;
} HlSpan;

typedef struct {
    unsigned char *states;
    size_t count;
    size_t cap;
    uint64_t rev;
} SyntaxCache;

void syntax_init(SyntaxCache *syn);
void syntax_free(SyntaxCache *syn);
void syntax_ensure(SyntaxCache *syn, Buffer *b);
int syntax_line_spans(SyntaxCache *syn, Buffer *b, int line,
                      HlSpan *spans, int max_spans);
size_t syntax_keyword_count(void);
const char *syntax_keyword_at(size_t index);
size_t syntax_type_count(void);
const char *syntax_type_at(size_t index);

#endif /* SI_SYNTAX_H */
