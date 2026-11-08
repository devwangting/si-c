#ifndef SI_TEXT_H
#define SI_TEXT_H

#include "si.h"
#include "syntax.h"

typedef struct {
    size_t pos;
    char *old_text;
    size_t old_len;
    char *new_text;
    size_t new_len;
    int group;
    uint64_t before_state;
    uint64_t after_state;
} UndoRec;

typedef struct Buffer {
    char *path;
    char *name;
    char *data;
    size_t len;
    size_t cap;

    int *lines;
    size_t nlines;
    size_t cap_lines;
    bool lines_valid;

    int cursor;
    int goal_col;
    int scroll_line;
    int scroll_col;

    SyntaxCache syn;

    UndoRec *undo;
    size_t nundo;
    size_t cundo;
    UndoRec *redo;
    size_t nredo;
    size_t credo;
    int next_group;
    int cur_group;

    uint64_t state_id;
    uint64_t next_state_id;
    uint64_t saved_state;
    uint64_t rev;
} Buffer;

typedef struct {
    size_t pos;
    size_t len;
    int line;
    int col;
} BufMatch;

Buffer *buf_create(const char *path, const char *text, size_t len);
Buffer *buf_open(const char *path, char **err);
void buf_free(Buffer *b);
bool buf_save(Buffer *b, char **err);
bool buf_is_dirty(const Buffer *b);

bool buf_insert_at(Buffer *b, size_t pos, const char *text, size_t len);
bool buf_delete_range(Buffer *b, size_t pos, size_t len);
void buf_undo_break(Buffer *b);
bool buf_undo(Buffer *b);
bool buf_redo(Buffer *b);
void buf_set_cursor(Buffer *b, size_t pos);

void buf_ensure_lines(Buffer *b);
size_t buf_line_count(Buffer *b);
size_t buf_line_start(Buffer *b, int line);
size_t buf_line_content_end(Buffer *b, int line);
size_t buf_line_break(Buffer *b, int line);
size_t buf_line_len(Buffer *b, int line);
int buf_line_of_offset(Buffer *b, size_t pos);
int buf_visual_col(Buffer *b, size_t pos);
size_t buf_offset_at_visual(Buffer *b, int line, int col);
size_t buf_leading_indent(Buffer *b, int line);

bool buf_find(Buffer *b, const char *needle, size_t start, bool forward,
              bool wrap, BufMatch *out);
bool buf_word_at(Buffer *b, size_t pos, size_t *start, size_t *len);

#endif /* SI_TEXT_H */
