#include "si.h"
#include "text.h"

#include <ctype.h>

static void undo_free_rec(UndoRec *r)
{
    free(r->old_text);
    free(r->new_text);
    r->old_text = NULL;
    r->new_text = NULL;
}

static void undo_clear_stack(Buffer *b, UndoRec **recs, size_t *count,
                             size_t *cap)
{
    size_t i;
    for (i = 0; i < *count; i++)
        undo_free_rec(&(*recs)[i]);
    free(*recs);
    *recs = NULL;
    *count = 0;
    *cap = 0;
    (void)b;
}

static void undo_push(Buffer *b, UndoRec **recs, size_t *count, size_t *cap,
                      UndoRec rec)
{
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *recs = xrealloc(*recs, *cap * sizeof(**recs));
    }
    (*recs)[(*count)++] = rec;
    (void)b;
}

Buffer *buf_create(const char *path, const char *text, size_t len)
{
    Buffer *b = xcalloc(1, sizeof(*b));
    b->path = path ? xstrdup(path) : NULL;
    b->name = xstrdup(path ? path_basename(path) : "<untitled>");
    b->cap = len + 1;
    b->data = xmalloc(b->cap);
    if (len)
        memcpy(b->data, text, len);
    b->data[len] = '\0';
    b->len = len;
    b->lines_valid = false;
    b->goal_col = 0;
    syntax_init(&b->syn);
    b->next_group = 1;
    b->cur_group = -1;
    b->state_id = 0;
    b->next_state_id = 1;
    b->saved_state = 0;
    b->rev = 0;
    return b;
}

Buffer *buf_open(const char *path, char **err)
{
    char *data = NULL;
    size_t len = 0;
    Buffer *b;
    if (!file_read_all(path, SI_MAX_FILE_SIZE, &data, &len, err))
        return NULL;
    if (len && memchr(data, '\0', len)) {
        if (err)
            *err = xasprintf("refusing binary file: %s", path);
        free(data);
        return NULL;
    }
    b = buf_create(path, data, len);
    free(data);
    return b;
}

void buf_free(Buffer *b)
{
    if (!b)
        return;
    undo_clear_stack(b, &b->undo, &b->nundo, &b->cundo);
    undo_clear_stack(b, &b->redo, &b->nredo, &b->credo);
    syntax_free(&b->syn);
    free(b->path);
    free(b->name);
    free(b->data);
    free(b->lines);
    free(b);
}

bool buf_is_dirty(const Buffer *b)
{
    return b->state_id != b->saved_state;
}

void buf_ensure_lines(Buffer *b)
{
    size_t i;
    size_t count = 1;

    if (b->lines_valid)
        return;
    for (i = 0; i < b->len; i++) {
        if (b->data[i] == '\n')
            count++;
    }
    if (b->cap_lines < count) {
        b->cap_lines = count + 8;
        b->lines = xrealloc(b->lines, b->cap_lines * sizeof(*b->lines));
    }
    b->nlines = 0;
    b->lines[b->nlines++] = 0;
    for (i = 0; i < b->len; i++) {
        if (b->data[i] == '\n') {
            b->lines[b->nlines++] = (int)(i + 1);
        }
    }
    b->lines_valid = true;
}

size_t buf_line_count(Buffer *b)
{
    buf_ensure_lines(b);
    return b->nlines;
}

size_t buf_line_start(Buffer *b, int line)
{
    buf_ensure_lines(b);
    if (line < 0)
        line = 0;
    if ((size_t)line >= b->nlines)
        return b->len;
    return (size_t)b->lines[line];
}

static size_t line_break_raw(Buffer *b, int line)
{
    size_t start = buf_line_start(b, line);
    size_t i;
    for (i = start; i < b->len; i++) {
        if (b->data[i] == '\n')
            return i;
    }
    return b->len;
}

size_t buf_line_break(Buffer *b, int line)
{
    return line_break_raw(b, line);
}

size_t buf_line_content_end(Buffer *b, int line)
{
    size_t end = line_break_raw(b, line);
    if (end > buf_line_start(b, line) && b->data[end - 1] == '\r')
        end--;
    return end;
}

size_t buf_line_len(Buffer *b, int line)
{
    size_t start = buf_line_start(b, line);
    size_t end = buf_line_content_end(b, line);
    return end >= start ? end - start : 0;
}

int buf_line_of_offset(Buffer *b, size_t pos)
{
    size_t lo = 0;
    size_t hi;
    buf_ensure_lines(b);
    if (pos > b->len)
        pos = b->len;
    hi = b->nlines;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((size_t)b->lines[mid] <= pos)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo ? (int)(lo - 1) : 0;
}

int buf_visual_col(Buffer *b, size_t pos)
{
    int line = buf_line_of_offset(b, pos);
    size_t start = buf_line_start(b, line);
    size_t i = start;
    int col = 0;
    if (pos > b->len)
        pos = b->len;
    while (i < pos) {
        unsigned char c = (unsigned char)b->data[i];
        if (c == '\t') {
            col += SI_TAB_WIDTH - (col % SI_TAB_WIDTH);
            i++;
        } else if (c < 0x80) {
            if (c != '\r' && c != '\n')
                col++;
            i++;
        } else {
            uint32_t cp;
            int n = utf8_decode(b->data + i, pos - i, &cp);
            if (n <= 0) {
                col++;
                i++;
            } else {
                col += utf8_cp_width(cp);
                i += (size_t)n;
            }
        }
    }
    return col;
}

size_t buf_offset_at_visual(Buffer *b, int line, int target)
{
    size_t start = buf_line_start(b, line);
    size_t end = buf_line_content_end(b, line);
    size_t i = start;
    int col = 0;

    if (target <= 0)
        return start;
    while (i < end) {
        unsigned char c = (unsigned char)b->data[i];
        int w;
        size_t n = 1;
        if (c == '\t') {
            w = SI_TAB_WIDTH - (col % SI_TAB_WIDTH);
        } else if (c < 0x80) {
            w = 1;
        } else {
            uint32_t cp;
            int d = utf8_decode(b->data + i, end - i, &cp);
            if (d <= 0) {
                w = 1;
            } else {
                n = (size_t)d;
                w = utf8_cp_width(cp);
            }
        }
        if (target < col + w)
            return i;
        col += w;
        i += n;
    }
    return end;
}

size_t buf_leading_indent(Buffer *b, int line)
{
    size_t start = buf_line_start(b, line);
    size_t end = buf_line_content_end(b, line);
    size_t i = start;
    while (i < end && (b->data[i] == ' ' || b->data[i] == '\t'))
        i++;
    return i - start;
}

void buf_set_cursor(Buffer *b, size_t pos)
{
    if (pos > b->len)
        pos = b->len;
    b->cursor = (int)pos;
}

static void buffer_apply(Buffer *b, size_t pos, size_t del_len,
                         const char *ins, size_t ins_len)
{
    size_t tail;
    size_t new_len;
    if (pos > b->len)
        pos = b->len;
    if (del_len > b->len - pos)
        del_len = b->len - pos;
    tail = b->len - pos - del_len;
    new_len = b->len - del_len + ins_len;

    if (new_len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < new_len + 1) {
            if (cap > SIZE_MAX / 2) {
                cap = new_len + 1;
                break;
            }
            cap *= 2;
        }
        b->data = xrealloc(b->data, cap);
        b->cap = cap;
    }
    if (tail)
        memmove(b->data + pos + ins_len, b->data + pos + del_len, tail);
    if (ins_len)
        memcpy(b->data + pos, ins, ins_len);
    b->len = new_len;
    b->data[b->len] = '\0';
    b->lines_valid = false;
    b->rev++;
}

static void redo_clear(Buffer *b)
{
    undo_clear_stack(b, &b->redo, &b->nredo, &b->credo);
}

static bool record_change(Buffer *b, size_t pos, const char *old_text,
                          size_t old_len, const char *new_text,
                          size_t new_len)
{
    UndoRec r;
    if (b->cur_group < 0)
        b->cur_group = b->next_group++;

    if (b->nundo >= SI_MAX_UNDO_RECORDS) {
        undo_free_rec(&b->undo[0]);
        memmove(b->undo, b->undo + 1,
                (b->nundo - 1) * sizeof(*b->undo));
        b->nundo--;
    }

    memset(&r, 0, sizeof(r));
    r.pos = pos;
    r.old_text = old_len ? xstrndup(old_text, old_len) : NULL;
    r.old_len = old_len;
    r.new_text = new_len ? xstrndup(new_text, new_len) : NULL;
    r.new_len = new_len;
    r.group = b->cur_group;
    r.before_state = b->state_id;
    r.after_state = b->next_state_id++;
    b->state_id = r.after_state;
    undo_push(b, &b->undo, &b->nundo, &b->cundo, r);
    return true;
}

bool buf_insert_at(Buffer *b, size_t pos, const char *text, size_t len)
{
    if (!len)
        return false;
    if (pos > b->len)
        pos = b->len;
    redo_clear(b);
    record_change(b, pos, NULL, 0, text, len);
    buffer_apply(b, pos, 0, text, len);
    return true;
}

bool buf_delete_range(Buffer *b, size_t pos, size_t len)
{
    if (!len || pos >= b->len)
        return false;
    if (len > b->len - pos)
        len = b->len - pos;
    redo_clear(b);
    record_change(b, pos, b->data + pos, len, NULL, 0);
    buffer_apply(b, pos, len, NULL, 0);
    return true;
}

void buf_undo_break(Buffer *b)
{
    b->cur_group = -1;
}

bool buf_undo(Buffer *b)
{
    int group;
    size_t cursor_pos = SIZE_MAX;

    if (b->nundo == 0) {
        b->cur_group = -1;
        return false;
    }
    b->cur_group = -1;
    group = b->undo[b->nundo - 1].group;
    while (b->nundo > 0 && b->undo[b->nundo - 1].group == group) {
        UndoRec r = b->undo[--b->nundo];
        buffer_apply(b, r.pos, r.new_len, r.old_text, r.old_len);
        b->state_id = r.before_state;
        cursor_pos = r.pos;
        undo_push(b, &b->redo, &b->nredo, &b->credo, r);
    }
    if (cursor_pos != SIZE_MAX)
        buf_set_cursor(b, cursor_pos);
    return true;
}

bool buf_redo(Buffer *b)
{
    int group;
    size_t cursor_pos = SIZE_MAX;

    if (b->nredo == 0) {
        b->cur_group = -1;
        return false;
    }
    b->cur_group = -1;
    group = b->redo[b->nredo - 1].group;
    while (b->nredo > 0 && b->redo[b->nredo - 1].group == group) {
        UndoRec r = b->redo[--b->nredo];
        buffer_apply(b, r.pos, r.old_len, r.new_text, r.new_len);
        b->state_id = r.after_state;
        cursor_pos = r.pos + r.new_len;
        undo_push(b, &b->undo, &b->nundo, &b->cundo, r);
    }
    if (cursor_pos != SIZE_MAX)
        buf_set_cursor(b, cursor_pos);
    return true;
}

bool buf_save(Buffer *b, char **err)
{
    if (!b->path) {
        if (err)
            *err = xstrdup("buffer has no file name");
        return false;
    }
    if (!file_write_atomic(b->path, b->data, b->len, err))
        return false;
    b->saved_state = b->state_id;
    return true;
}

static bool ident_char(unsigned char c)
{
    return isalnum(c) || c == '_';
}

bool buf_word_at(Buffer *b, size_t pos, size_t *start, size_t *len)
{
    size_t a;
    size_t z;

    if (b->len == 0)
        return false;
    if (pos > b->len)
        pos = b->len;
    if (pos == b->len || !ident_char((unsigned char)b->data[pos])) {
        if (pos == 0)
            return false;
        pos--;
    }
    if (!ident_char((unsigned char)b->data[pos]))
        return false;
    a = pos;
    z = pos;
    while (a > 0 && ident_char((unsigned char)b->data[a - 1]))
        a--;
    while (z < b->len && ident_char((unsigned char)b->data[z]))
        z++;
    *start = a;
    *len = z - a;
    return true;
}

static bool match_at(const char *s, size_t len, size_t at, const char *needle,
                     size_t nlen, bool case_sensitive)
{
    size_t i;
    if (nlen > len - at)
        return false;
    for (i = 0; i < nlen; i++) {
        unsigned char a = (unsigned char)s[at + i];
        unsigned char b = (unsigned char)needle[i];
        if (!case_sensitive) {
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b)
            return false;
    }
    return true;
}

bool buf_find(Buffer *b, const char *needle, size_t start, bool forward,
              bool wrap, BufMatch *out)
{
    size_t nlen;
    bool case_sensitive = false;
    const char *p;
    size_t i;

    if (!needle || !*needle)
        return false;
    nlen = strlen(needle);
    if (nlen > b->len)
        return false;
    for (p = needle; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            case_sensitive = true;
            break;
        }
    }
    if (start > b->len)
        start = b->len;

    if (forward) {
        for (i = start; i + nlen <= b->len; i++) {
            if (match_at(b->data, b->len, i, needle, nlen, case_sensitive)) {
                out->pos = i;
                out->len = nlen;
                out->line = buf_line_of_offset(b, i);
                out->col = buf_visual_col(b, i);
                return true;
            }
        }
        if (wrap) {
            for (i = 0; i + nlen <= b->len && i < start; i++) {
                if (match_at(b->data, b->len, i, needle, nlen,
                             case_sensitive)) {
                    out->pos = i;
                    out->len = nlen;
                    out->line = buf_line_of_offset(b, i);
                    out->col = buf_visual_col(b, i);
                    return true;
                }
            }
        }
    } else {
        size_t max = b->len - nlen;
        size_t at = start > max ? max : start;
        for (;;) {
            if (match_at(b->data, b->len, at, needle, nlen, case_sensitive)) {
                out->pos = at;
                out->len = nlen;
                out->line = buf_line_of_offset(b, at);
                out->col = buf_visual_col(b, at);
                return true;
            }
            if (at == 0)
                break;
            at--;
        }
        if (wrap) {
            at = max;
            while (at > start) {
                if (match_at(b->data, b->len, at, needle, nlen,
                             case_sensitive)) {
                    out->pos = at;
                    out->len = nlen;
                    out->line = buf_line_of_offset(b, at);
                    out->col = buf_visual_col(b, at);
                    return true;
                }
                at--;
            }
        }
    }
    return false;
}
