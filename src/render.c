#include "si.h"
#include "app.h"

#include <ctype.h>
#include <limits.h>

/* ------------------------------------------------------------------ */
/* ANSI helpers                                                        */
/* ------------------------------------------------------------------ */

static int g_colors = 256;

static int ansi16_from_256(int idx, bool bg)
{
    int base = bg ? 40 : 30;
    int bright = bg ? 100 : 90;
    int r;
    int g;
    int b;
    int c;

    if (idx < 0)
        idx = 0;
    if (idx < 16) {
        int bright_bit = idx & 8;
        return (bright_bit ? bright : base) + (idx & 7);
    }
    if (idx >= 232) {
        int level = (idx - 232) * 10 / 24;
        if (level >= 7)
            return bright + 7;
        if (level >= 4)
            return base + 7;
        return base;
    }
    idx -= 16;
    r = idx / 36;
    g = (idx / 6) % 6;
    b = idx % 6;
    c = 0;
    if (r >= 2)
        c |= 1;
    if (g >= 2)
        c |= 2;
    if (b >= 2)
        c |= 4;
    if (c == 0)
        return base;
    return ((r >= 4 || g >= 4 || b >= 4) ? bright : base) + c;
}

static void sgr_fg(Str *s, int color)
{
    if (g_colors >= 256)
        str_printf(s, "\x1b[38;5;%dm", color);
    else
        str_printf(s, "\x1b[%dm", ansi16_from_256(color, false));
}

static void sgr_bg(Str *s, int color)
{
    if (g_colors >= 256)
        str_printf(s, "\x1b[48;5;%dm", color);
    else
        str_printf(s, "\x1b[%dm", ansi16_from_256(color, true));
}

static void sgr_reset(Str *s)
{
    str_puts(s, "\x1b[0m");
}

static void sgr_find_highlight_bg(Str *s)
{
    if (g_colors >= 256)
        sgr_bg(s, 226);
    else
        sgr_bg(s, 11);
}

static void sgr_find_highlight_fg(Str *s)
{
    if (g_colors >= 256)
        sgr_fg(s, 16);
    else
        sgr_fg(s, 0);
}

static void sgr_bold(Str *s)
{
    str_puts(s, "\x1b[1m");
}

static void sgr_move(Str *s, int row, int col)
{
    str_printf(s, "\x1b[%d;%dH", row + 1, col + 1);
}

static void draw_spaces(Str *out, int n);

/* Draw up to maxw cells from s, optionally padding to maxw. */
static int draw_text(Str *out, const char *s, size_t len, int maxw, bool pad)
{
    int w = 0;
    size_t i = 0;

    if (maxw <= 0)
        return 0;
    while (i < len && w < maxw) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        int cw;
        if (n <= 0) {
            char q = '?';
            str_append(out, &q, 1);
            w++;
            i++;
            continue;
        }
        if (cp == '\t') {
            int emit = SI_TAB_WIDTH - (w % SI_TAB_WIDTH);
            if (w + emit > maxw)
                emit = maxw - w;
            draw_spaces(out, emit);
            w += emit;
            i += (size_t)n;
            continue;
        }
        if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) {
            if (w + 1 > maxw)
                break;
            str_putc(out, '?');
            w++;
            i += (size_t)n;
            continue;
        }
        cw = utf8_cp_width(cp);
        if (cw == 0) {
            str_append(out, s + i, (size_t)n);
            i += (size_t)n;
            continue;
        }
        if (w + cw > maxw)
            break;
        str_append(out, s + i, (size_t)n);
        w += cw;
        i += (size_t)n;
    }
    if (pad) {
        while (w < maxw) {
            str_putc(out, ' ');
            w++;
        }
    }
    return w;
}

static void draw_spaces(Str *out, int n)
{
    while (n-- > 0)
        str_putc(out, ' ');
}

static void draw_center(Str *out, const char *text, int width)
{
    int tw = utf8_text_width(text, strlen(text));
    int left = (width - tw) / 2;
    if (left < 0)
        left = 0;
    draw_spaces(out, left);
    draw_text(out, text, strlen(text), width - left, true);
}

static void cell(Str *out, const char *text, int width, int bg, int fg,
                 bool bold)
{
    sgr_bg(out, bg);
    if (bold)
        sgr_bold(out);
    sgr_fg(out, fg);
    draw_text(out, text, strlen(text), width, true);
    sgr_reset(out);
}

static void sep_cell(Str *out, App *a)
{
    sgr_bg(out, a->theme.panel);
    sgr_fg(out, a->theme.border);
    str_puts(out, "\xe2\x94\x82");
    sgr_reset(out);
}

/* ------------------------------------------------------------------ */
/* Editor text                                                         */
/* ------------------------------------------------------------------ */

static bool line_in_function_file(App *a, int fi, int line)
{
    int i;
    int ln = line + 1;
    if (fi < 0 || (size_t)fi >= a->project.file_count)
        return false;
    for (i = 0; i < a->project.files[fi].sym_count; i++) {
        Symbol *s = &a->project.syms[a->project.files[fi].sym_start + i];
        if (s->kind == SYM_FUNCTION && ln >= s->line && ln <= s->end_line)
            return true;
    }
    return false;
}

static bool code_ident_char(unsigned char c)
{
    return isalnum(c) || c == '_';
}

static bool code_highlight_allowed(const HlSpan *spans, int ns,
                                   int pos, int len)
{
    int k;
    for (k = 0; k < ns; k++) {
        if (pos >= spans[k].start && pos + len <= spans[k].end)
            return spans[k].kind == HL_NORMAL ||
                   spans[k].kind == HL_LOCAL ||
                   spans[k].kind == HL_GLOBAL;
    }
    return true;
}

static void draw_code_range(App *a, Str *out, Buffer *b, int line, int width,
                            int scroll, bool current, bool semantic,
                            int semantic_file, bool definition_line)
{
    HlSpan spans[1024];
    int ns;
    size_t start = buf_line_start(b, line);
    size_t end = buf_line_content_end(b, line);
    int vcol = 0;
    int printed = 0;
    size_t i = start;
    int si = 0;
    HlKind cur_kind = HL_NORMAL;
    int bg_mode = 0; /* 0 normal, 1 find, 2 selection, 3 variable, 4 def */
    int hstart[256];
    int hend[256];
    int nh = 0;
    int hi = 0;
    bool sel_active = current && a->has_selection && a->sel_buf == a->active;
    int sel_lo = 0;
    int sel_hi = 0;

    if (sel_active) {
        if (a->sel_anchor <= a->sel_cursor) {
            sel_lo = a->sel_anchor;
            sel_hi = a->sel_cursor;
        } else {
            sel_lo = a->sel_cursor;
            sel_hi = a->sel_anchor;
        }
    }

    ns = syntax_line_spans(&b->syn, b, line, spans, (int)SI_ARRAY_LEN(spans));
    if (semantic && (a->globals.count > 0 || a->types.count > 0 ||
                     a->macros.count > 0 || a->nlocals > 0)) {
        int k;
        for (k = 0; k < ns; k++) {
            const char *word;
            size_t wlen;
            if (spans[k].end <= spans[k].start)
                continue;
            word = b->data + start + spans[k].start;
            wlen = (size_t)(spans[k].end - spans[k].start);
            if (!(isalpha((unsigned char)word[0]) || word[0] == '_'))
                continue;
            if (spans[k].kind == HL_FUNC) {
                if (strlist_contains_n(&a->macros, word, wlen))
                    spans[k].kind = HL_PREPROC;
                continue;
            }
            if (spans[k].kind != HL_NORMAL)
                continue;
            if (semantic_file == a->active &&
                app_local_in_scope(a, line + 1, word, wlen))
                spans[k].kind = HL_LOCAL;
            else if (strlist_contains_n(&a->globals, word, wlen))
                spans[k].kind = HL_GLOBAL;
            else if (strlist_contains_n(&a->types, word, wlen))
                spans[k].kind = HL_TYPE;
            else if (strlist_contains_n(&a->macros, word, wlen))
                spans[k].kind = HL_PREPROC;
            else if (line_in_function_file(a, semantic_file, line))
                spans[k].kind = HL_LOCAL;
        }
    }
    {
        const char *mark_word = NULL;
        const char *ld;
        size_t llen;
        size_t hlen;
        size_t p;
        if (definition_line && a->info_word[0])
            mark_word = a->info_word;
        else if (a->highlight_active && a->highlight_word[0])
            mark_word = a->highlight_word;
        if (mark_word)
            hlen = strlen(mark_word);
        else
            hlen = 0;
        ld = b->data + start;
        llen = end - start;
        for (p = 0; hlen > 0 && p + hlen <= llen && nh < 256; p++) {
            if (!code_ident_char((unsigned char)ld[p]))
                continue;
            if (p > 0 && code_ident_char((unsigned char)ld[p - 1]))
                continue;
            if (p + hlen < llen &&
                code_ident_char((unsigned char)ld[p + hlen]))
                continue;
            if (memcmp(ld + p, mark_word, hlen) == 0 &&
                (definition_line ||
                 code_highlight_allowed(spans, ns, (int)p, (int)hlen))) {
                hstart[nh] = (int)p;
                hend[nh] = (int)(p + hlen);
                nh++;
                p += hlen - 1;
            }
        }
    }
    sgr_bg(out, current ? a->theme.current : a->theme.bg);
    sgr_fg(out, a->theme.syn[HL_NORMAL]);

    while (i < end && printed < width) {
        uint32_t cp;
        int n = utf8_decode(b->data + i, end - i, &cp);
        int w;
        HlKind kind = HL_NORMAL;
        bool match;
        bool selected;
        bool highlighted;
        int want_mode;
        bool bad = false;

        while (si < ns && (int)(i - start) >= spans[si].end)
            si++;
        if (si < ns && (int)(i - start) >= spans[si].start)
            kind = spans[si].kind;
        if (n <= 0) {
            cp = '?';
            n = 1;
            w = 1;
            bad = true;
        } else if (cp == '\t') {
            w = SI_TAB_WIDTH - (vcol % SI_TAB_WIDTH);
        } else if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) {
            cp = '?';
            w = 1;
            bad = true;
        } else {
            w = utf8_cp_width(cp);
        }
        if (w < 0)
            w = 1;

        match = current && a->find_len > 0 && (int)i >= a->find_pos &&
                (int)i < a->find_pos + a->find_len;
        selected = sel_active && (int)i >= sel_lo && (int)i < sel_hi;
        while (hi < nh && (int)(i - start) >= hend[hi])
            hi++;
        highlighted = hi < nh && (int)(i - start) >= hstart[hi];
        want_mode = selected ? 2 : (highlighted ? 3 : (match ? 1 : 0));
        if (want_mode != bg_mode) {
            bg_mode = want_mode;
            if (bg_mode == 2) {
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
            } else if (bg_mode == 3) {
                /* Bright yellow background + black text: deliberately
                 * louder than syntax colors and the find-match color. */
                sgr_find_highlight_bg(out);
                sgr_find_highlight_fg(out);
            } else if (bg_mode == 1) {
                sgr_bg(out, 58);
                sgr_fg(out, 255);
            } else {
                sgr_bg(out, current ? a->theme.current : a->theme.bg);
                sgr_fg(out, a->theme.syn[cur_kind]);
            }
        }

        if (w > 0 && vcol + w <= scroll) {
            vcol += w;
            i += (size_t)n;
            continue;
        }
        if (w > 0 && vcol < scroll) {
            int show = vcol + w - scroll;
            if (show > width - printed)
                show = width - printed;
            draw_spaces(out, show);
            printed += show;
            vcol += w;
            i += (size_t)n;
            continue;
        }
        if (w > 0 && printed + w > width)
            break;
        if (kind != cur_kind) {
            cur_kind = kind;
            if (bg_mode == 0)
                sgr_fg(out, a->theme.syn[kind]);
        }
        if (w == 0) {
            if (!bad && vcol >= scroll && n > 0)
                str_append(out, b->data + i, (size_t)n);
            i += (size_t)n;
            continue;
        }
        if (cp == '\t') {
            draw_spaces(out, w);
        } else if (bad) {
            str_putc(out, '?');
        } else {
            str_append(out, b->data + i, (size_t)n);
        }
        printed += w;
        vcol += w;
        i += (size_t)n;
    }
    if (printed < width) {
        /* Do not let a match/highlight/selection background pad the rest
         * of the row (and, in the CODE pane, bleed into CALL FLOW). */
        sgr_bg(out, current ? a->theme.current : a->theme.bg);
        sgr_fg(out, a->theme.syn[cur_kind]);
        draw_spaces(out, width - printed);
    }
    sgr_reset(out);
}

static void draw_code(App *a, Str *out, Buffer *b, int line, int width)
{
    bool current = line == buf_line_of_offset(b, (size_t)b->cursor);
    draw_code_range(a, out, b, line, width, b->scroll_col, current, true,
                    a->active, false);
}

static void draw_editor_row(App *a, Str *out, int row, int line)
{
    Buffer *b = app_buffer(a);
    const Theme *t = &a->theme;
    bool current = b && line == buf_line_of_offset(b, (size_t)b->cursor);
    char num[32];

    (void)row;
    if (!b || line < 0 || (size_t)line >= buf_line_count(b)) {
        sgr_bg(out, t->bg);
        draw_spaces(out, a->cw);
        sgr_reset(out);
        return;
    }
    sgr_bg(out, current ? t->current : t->panel);
    sgr_fg(out, current ? t->syn[HL_NORMAL] : t->dim);
    if (current)
        sgr_bold(out);
    snprintf(num, sizeof(num), "%*d", a->gutter_w, line + 1);
    draw_text(out, num, strlen(num), a->gutter_w, true);
    sgr_reset(out);

    sgr_bg(out, current ? t->current : t->panel);
    sgr_fg(out, t->border);
    str_puts(out, " \xe2\x94\x82 ");
    sgr_reset(out);

    draw_code(a, out, b, line, a->ed_w);
}

/* ------------------------------------------------------------------ */
/* Side panels                                                         */
/* ------------------------------------------------------------------ */

static void draw_outline_row(App *a, Str *out, int row)
{
    const Theme *t = &a->theme;
    int idx = a->outline_scroll + row;
    bool selected = a->focus == FOCUS_OUTLINE && idx == a->outline_sel;
    bool active = idx == a->outline_sel;
    int bg = selected ? t->accent_bg : (active ? t->panel_alt : t->panel);
    int fg = selected ? t->selection_fg : (active ? t->fg : t->dim);
    int detail_fg = selected ? t->selection_fg : t->dim;
    ViewItem *it;
    int inner;
    int dw;
    int lw;

    if ((size_t)idx >= a->outline.count) {
        sgr_bg(out, t->panel);
        draw_spaces(out, a->lw);
        sgr_reset(out);
        return;
    }
    it = &a->outline.items[idx];
    sgr_bg(out, bg);
    sgr_fg(out, fg);
    inner = a->lw - 2;
    dw = utf8_text_width(it->detail, strlen(it->detail));
    if (dw > 6)
        dw = 6;
    lw = inner - dw - 1;
    if (lw < 4) {
        lw = inner;
        dw = 0;
    }
    str_putc(out, ' ');
    draw_text(out, it->label, strlen(it->label), lw, true);
    if (dw > 0) {
        str_putc(out, ' ');
        sgr_fg(out, detail_fg);
        draw_text(out, it->detail, strlen(it->detail), dw, true);
        sgr_fg(out, fg);
    }
    str_putc(out, ' ');
    sgr_reset(out);
}

static void draw_file_row(App *a, Str *out, int row)
{
    const Theme *t = &a->theme;
    int idx = a->file_scroll + row;
    bool selected = a->focus == FOCUS_PROJECT && idx == a->file_sel;
    bool info_selected = a->info_valid && a->info_def_file == idx;
    int active_fi = app_active_file_index(a);
    int bg = selected ? t->accent_bg : (idx == active_fi ? t->panel_alt : t->panel);
    int fg = selected ? t->selection_fg : (idx == active_fi ? t->fg : t->dim);
    char path[1024];
    char shortpath[1024];
    char label[1200];
    Buffer *b;

    if (info_selected) {
        sgr_find_highlight_bg(out);
        sgr_find_highlight_fg(out);
    } else {
        sgr_bg(out, bg);
        sgr_fg(out, fg);
    }
    if ((size_t)idx >= a->project.file_count) {
        if (info_selected) {
            sgr_bg(out, t->panel);
            sgr_fg(out, t->dim);
        }
        draw_spaces(out, a->rw);
        sgr_reset(out);
        return;
    }
    b = NULL;
    {
        int i;
        for (i = 0; i < a->nbuf; i++) {
            if (strcmp(a->bufs[i]->path, a->project.files[idx].path) == 0) {
                b = a->bufs[i];
                break;
            }
        }
    }
    snprintf(path, sizeof(path), "%s", a->project.files[idx].rel);
    path_shorten(path, a->rw - 3, shortpath, sizeof(shortpath));
    snprintf(label, sizeof(label), "%c %s%s", idx == active_fi ? '>' : ' ',
             shortpath, b && buf_is_dirty(b) ? "*" : "");
    draw_text(out, label, strlen(label), a->rw, true);
    sgr_reset(out);
}

static void draw_right_row(App *a, Str *out, int row)
{
    const Theme *t = &a->theme;
    int idx = a->sym_scroll + row;
    ViewItem *it;
    bool selected;
    int bg;
    int fg;
    int detail_fg;
    int inner;
    int dw;
    int lw;

    if ((size_t)idx >= a->view.count) {
        sgr_bg(out, t->panel);
        draw_spaces(out, a->rw);
        sgr_reset(out);
        return;
    }
    it = &a->view.items[idx];
    if (it->is_header) {
        sgr_bg(out, t->panel_alt);
        sgr_fg(out, t->fg);
        sgr_bold(out);
        str_putc(out, ' ');
        draw_text(out, it->label, strlen(it->label), a->rw - 1, true);
        sgr_reset(out);
        return;
    }
    selected = a->focus == FOCUS_CONTEXT && idx == a->sym_sel;
    bg = selected ? t->accent_bg : t->panel;
    fg = selected ? t->selection_fg : t->fg;
    detail_fg = selected ? t->selection_fg : t->dim;
    sgr_bg(out, bg);
    sgr_fg(out, fg);
    inner = a->rw - 2;
    dw = utf8_text_width(it->detail, strlen(it->detail));
    if (dw > 14)
        dw = 14;
    if (dw > inner / 2)
        dw = inner / 2;
    {
        int depth = it->depth;
        if (depth > 8)
            depth = 8;
        if (depth > 0) {
            draw_spaces(out, (depth - 1) * 2);
            str_puts(out, "\xe2\x86\xb3 "); /* ↳ */
            inner -= (depth - 1) * 2 + 2;
        }
    }
    lw = inner - dw - 1;
    if (lw < 4) {
        lw = inner;
        dw = 0;
    }
    str_putc(out, ' ');
    draw_text(out, it->label, strlen(it->label), lw, true);
    if (dw > 0) {
        str_putc(out, ' ');
        sgr_fg(out, detail_fg);
        draw_text(out, it->detail, strlen(it->detail), dw, true);
        sgr_fg(out, fg);
    }
    str_putc(out, ' ');
    sgr_reset(out);
}

/* ------------------------------------------------------------------ */
/* CALL FLOW graph                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *ch;
    int fg;
    int bg;
    bool bold;
} CfCell;

typedef struct {
    int item;
    int parent;
    int first;
    int next;
    int depth;
    int row;
    int col;
    int w;
    bool visible;
} CfNode;

#define CF_BOX_H 3
#define CF_GAP 3
#define CF_MIN_W 8

static void cf_put(CfCell *cv, int w, int h, int x, int y,
                   const char *ch, int fg, int bg)
{
    if (x < 0 || x >= w || y < 0 || y >= h)
        return;
    cv[y * w + x].ch = ch;
    cv[y * w + x].fg = fg;
    cv[y * w + x].bg = bg;
}


static int cf_layout_node(CfNode *n, int idx, int visible_depth,
                          int *cursor)
{
    int child = n[idx].first;
    int start = -1;
    int end = -1;
    if (child < 0 || n[idx].depth >= visible_depth) {
        n[idx].row = *cursor;
        *cursor += CF_BOX_H + 1;
        return n[idx].row;
    }
    while (child >= 0) {
        int r = cf_layout_node(n, child, visible_depth, cursor);
        if (start < 0 || r < start)
            start = r;
        if (r > end)
            end = r;
        child = n[child].next;
    }
    n[idx].row = start + (end - start) / 2;
    return n[idx].row;
}

static void cf_draw_edges(CfCell *cv, int w, int h, CfNode *n, int nn,
                          int scroll, int fg, int bg)
{
    int i;
    for (i = 0; i < nn; i++) {
        int child;
        int first_y = -1;
        int last_y = -1;
        int busx;
        int py;
        if (!n[i].visible || n[i].first < 0)
            continue;
        for (child = n[i].first; child >= 0; child = n[child].next) {
            int cy;
            if (!n[child].visible)
                continue;
            cy = n[child].row + 1 - scroll;
            if (first_y < 0)
                first_y = cy;
            last_y = cy;
        }
        if (first_y < 0)
            continue;
        busx = n[i].col + n[i].w + 1;
        py = n[i].row + 1 - scroll;
        if (first_y == last_y) {
            int x;
            for (x = n[i].col + n[i].w; x < busx + 1; x++)
                cf_put(cv, w, h, x, py, "\xe2\x94\x80", fg, bg);
            child = n[i].first;
            while (child >= 0 && !n[child].visible)
                child = n[child].next;
            if (child >= 0) {
                for (x = busx + 1; x < n[child].col; x++)
                    cf_put(cv, w, h, x, py, "\xe2\x94\x80", fg, bg);
            }
            cf_put(cv, w, h, busx, py, "\xe2\x97\x80", fg, bg);
        } else {
            int y;
            if (first_y > last_y) {
                int t = first_y;
                first_y = last_y;
                last_y = t;
            }
            for (y = first_y; y <= last_y; y++)
                cf_put(cv, w, h, busx, y, "\xe2\x94\x82", fg, bg);
            for (y = n[i].col + n[i].w; y <= busx; y++)
                cf_put(cv, w, h, y, py, "\xe2\x94\x80", fg, bg);
            cf_put(cv, w, h, n[i].col + n[i].w + 1, py,
                   "\xe2\x97\x80", fg, bg);
            for (child = n[i].first; child >= 0; child = n[child].next) {
                int cy;
                int x;
                if (!n[child].visible)
                    continue;
                cy = n[child].row + 1 - scroll;
                for (x = busx + 1; x < n[child].col; x++)
                    cf_put(cv, w, h, x, cy, "\xe2\x94\x80", fg, bg);
                cf_put(cv, w, h, busx, cy, "\xe2\x94\x9c", fg, bg);
            }
        }
    }
}

static void cf_draw_box(CfCell *cv, int w, int h, CfNode *n, int scroll,
                        bool selected, const Theme *t)
{
    int y = n->row - scroll;
    int x = n->col;
    int i;
    int bg = selected ? t->accent_bg : t->panel_alt;
    int border = selected ? t->selection_fg : t->border;
    if (y + CF_BOX_H <= 0 || y >= h)
        return;
    for (i = 0; i < n->w; i++) {
        const char *top = i == 0 ? "\xe2\x94\x8c"
                        : i == n->w - 1 ? "\xe2\x94\x90" : "\xe2\x94\x80";
        const char *bot = i == 0 ? "\xe2\x94\x94"
                        : i == n->w - 1 ? "\xe2\x94\x98" : "\xe2\x94\x80";
        cf_put(cv, w, h, x + i, y, top, border, bg);
        cf_put(cv, w, h, x + i, y + 2, bot, border, bg);
        cf_put(cv, w, h, x + i, y + 1, " ", border, bg);
    }
    cf_put(cv, w, h, x, y + 1, "\xe2\x94\x82", border, bg);
    cf_put(cv, w, h, x + n->w - 1, y + 1, "\xe2\x94\x82", border, bg);
}

static void cf_function_name(const char *sig, char *out, size_t outsz)
{
    const char *p = strchr(sig, '(');
    const char *end;
    const char *start;
    size_t n;

    if (!p)
        p = sig + strlen(sig);
    end = p;
    while (end > sig && isspace((unsigned char)end[-1]))
        end--;
    start = end;
    while (start > sig &&
           (isalnum((unsigned char)start[-1]) || start[-1] == '_'))
        start--;
    n = (size_t)(end - start);
    if (n == 0) {
        snprintf(out, outsz, "%s", sig);
        return;
    }
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

static void draw_relations_missing(App *a, Str *out, int top, int pane_h)
{
    int r;
    int x0 = a->lw + 1 + a->cw + 1;
    for (r = 0; r < pane_h; r++) {
        sgr_move(out, top + r, x0);
        sgr_bg(out, a->theme.panel);
        draw_spaces(out, a->rw);
        sgr_reset(out);
    }
    if (pane_h > 0) {
        sgr_move(out, top, x0);
        sgr_bg(out, a->theme.panel);
        sgr_fg(out, a->theme.fg);
        sgr_bold(out);
        draw_center(out, "RELATION INDEX NOT BUILT", a->rw);
        sgr_reset(out);
    }
    if (pane_h > 2) {
        sgr_move(out, top + 2, x0);
        sgr_bg(out, a->theme.panel);
        sgr_fg(out, a->theme.dim);
        draw_center(out, "Press Alt/Opt+R to build", a->rw);
        sgr_reset(out);
    }
    if (pane_h > 3) {
        sgr_move(out, top + 3, x0);
        sgr_bg(out, a->theme.panel);
        sgr_fg(out, a->theme.dim);
        draw_center(out, "call/variable tables", a->rw);
        sgr_reset(out);
    }
    a->show_cursor = false;
}

static bool draw_call_flow_pane(App *a, Str *out, int top, int pane_h)
{
    int pane_w = a->rw;
    int x0 = a->lw + 1 + a->cw + 1;
    CfNode *nodes;
    CfCell *cv;
    int nn = 0;
    int root = -1;
    int i;
    int maxdepth = 0;
    int max_levels;
    int levels;
    int bw;
    int gap = CF_GAP;
    int cursor = 0;
    int total;
    int selected = -1;
    int selrow = -1;
    int scroll;
    int visible_depth;

    a->call_box_count = 0;
    if (a->context_need_relations) {
        draw_relations_missing(a, out, top, pane_h);
        return true;
    }
    if (pane_h < 4 || pane_w < 16)
        return false;

    nodes = xcalloc(a->view.count ? a->view.count : 1, sizeof(*nodes));
    {
        int last[10];
        for (i = 0; i < 10; i++)
            last[i] = -1;
        for (i = 0; i < (int)a->view.count; i++) {
            ViewItem *it = &a->view.items[i];
            int d;
            int p;
            if (it->is_header || it->file_index < 0)
                continue;
            if (root < 0) {
                root = nn;
                nodes[nn].item = i;
                nodes[nn].parent = -1;
                nodes[nn].first = -1;
                nodes[nn].next = -1;
                nodes[nn].depth = 0;
                last[0] = nn;
                nn++;
                continue;
            }
            d = it->depth;
            if (d < 1)
                d = 1;
            if (d > 8)
                d = 8;
            p = last[d - 1];
            if (p < 0)
                p = root;
            nodes[nn].item = i;
            nodes[nn].parent = p;
            nodes[nn].first = -1;
            nodes[nn].next = -1;
            nodes[nn].depth = d;
            if (nodes[p].first < 0)
                nodes[p].first = nn;
            else {
                int s = nodes[p].first;
                while (nodes[s].next >= 0)
                    s = nodes[s].next;
                nodes[s].next = nn;
            }
            last[d] = nn;
            if (d > maxdepth)
                maxdepth = d;
            nn++;
        }
    }
    if (root < 0 || nn == 0) {
        free(nodes);
        return false;
    }
    max_levels = (pane_w - 2 + gap) / (CF_MIN_W + gap);
    if (max_levels < 1)
        max_levels = 1;
    levels = maxdepth + 1;
    if (levels > max_levels)
        levels = max_levels;
    bw = (pane_w - 2 - gap * (levels - 1)) / levels;
    if (bw > 22)
        bw = 22;
    if (bw < CF_MIN_W)
        bw = CF_MIN_W;
    visible_depth = levels - 1;

    for (i = 0; i < nn; i++) {
        nodes[i].visible = nodes[i].depth <= visible_depth;
        nodes[i].w = bw;
        nodes[i].col = 1 + nodes[i].depth * (bw + gap);
    }
    cf_layout_node(nodes, root, visible_depth, &cursor);
    total = cursor;

    for (i = 0; i < nn; i++) {
        if (nodes[i].item == a->sym_sel) {
            selected = i;
            break;
        }
    }
    if (selected < 0)
        selected = root;
    if (nodes[selected].visible)
        selrow = nodes[selected].row;
    scroll = a->context_scroll;
    if (selrow >= 0) {
        if (selrow < scroll)
            scroll = selrow;
        if (selrow + CF_BOX_H > scroll + pane_h)
            scroll = selrow + CF_BOX_H - pane_h;
    }
    if (scroll > total - pane_h)
        scroll = total - pane_h;
    if (scroll < 0)
        scroll = 0;
    a->context_scroll = scroll;

    if (a->call_box_cap < (size_t)nn) {
        a->call_box_cap = (size_t)nn;
        a->call_boxes = xrealloc(a->call_boxes,
                                 a->call_box_cap * sizeof(*a->call_boxes));
    }
    a->call_box_count = 0;
    a->call_box_top = top;
    a->call_box_x = x0;
    a->call_box_scroll = scroll;
    for (i = 0; i < nn; i++) {
        if (!nodes[i].visible)
            continue;
        a->call_boxes[a->call_box_count].item = nodes[i].item;
        a->call_boxes[a->call_box_count].row = nodes[i].row;
        a->call_boxes[a->call_box_count].col = nodes[i].col;
        a->call_boxes[a->call_box_count].w = nodes[i].w;
        a->call_box_count++;
    }

    cv = xcalloc((size_t)pane_w * (size_t)pane_h, sizeof(*cv));
    for (i = 0; i < pane_w * pane_h; i++) {
        cv[i].ch = " ";
        cv[i].fg = a->theme.panel;
        cv[i].bg = a->theme.panel;
    }
    cf_draw_edges(cv, pane_w, pane_h, nodes, nn, scroll,
                  a->theme.dim, a->theme.panel);
    for (i = 0; i < nn; i++) {
        if (nodes[i].visible)
            cf_draw_box(cv, pane_w, pane_h, &nodes[i], scroll,
                        i == selected, &a->theme);
    }

    for (i = 0; i < pane_h; i++) {
        int x;
        int lastfg = -1;
        int lastbg = -1;
        sgr_move(out, top + i, x0);
        for (x = 0; x < pane_w; x++) {
            CfCell *c = &cv[i * pane_w + x];
            if (c->fg != lastfg) {
                sgr_fg(out, c->fg);
                lastfg = c->fg;
            }
            if (c->bg != lastbg) {
                sgr_bg(out, c->bg);
                lastbg = c->bg;
            }
            str_puts(out, c->ch);
        }
        sgr_reset(out);
        /* Labels are overlaid after the cell row, so UTF-8 text is not
         * split into one-byte canvas cells. */
        for (int j = 0; j < nn; j++) {
            CfNode *n = &nodes[j];
            int by = n->row + 1 - scroll;
            if (!n->visible || by != i)
                continue;
            sgr_move(out, top + i, x0 + n->col + 1);
            char name[128];
            cf_function_name(a->view.items[n->item].label, name,
                             sizeof(name));
            sgr_bg(out, j == selected ? a->theme.accent_bg
                                      : a->theme.panel_alt);
            sgr_fg(out, j == selected ? a->theme.selection_fg
                                      : a->theme.fg);
            draw_text(out, name, strlen(name), n->w - 2, true);
            sgr_reset(out);
        }
    }
    free(cv);
    free(nodes);
    (void)total;
    return true;
}

/* ------------------------------------------------------------------ */
/* Header / status                                                     */
/* ------------------------------------------------------------------ */

static void draw_title(App *a, Str *out)
{
    Str text;
    int i;
    const char *rootname;

    str_init(&text);
    rootname = a->project.root ? path_basename(a->project.root) : "project";
    str_printf(&text, " SI-C  %s  ", rootname);
    if (a->nbuf == 0) {
        str_puts(&text, "[no file]");
    } else {
        for (i = 0; i < a->nbuf; i++) {
            Buffer *b = a->bufs[i];
            if (i > 0)
                str_putc(&text, ' ');
            if (i == a->active)
                str_putc(&text, '[');
            str_puts(&text, b->name);
            if (buf_is_dirty(b))
                str_putc(&text, '*');
            if (i == a->active)
                str_putc(&text, ']');
            if (utf8_text_width(text.data, text.len) > a->cols - 18)
                break;
        }
        if (i < a->nbuf - 1)
            str_printf(&text, " +%d", a->nbuf - i - 1);
    }
    {
        int xs[APP_MENU_GROUPS];
        int ws[APP_MENU_GROUPS];
        int mi;
        int menu_w;
        app_menu_tab_layout(a, xs, ws);
        menu_w = xs[APP_MENU_GROUPS - 1] + ws[APP_MENU_GROUPS - 1];
        for (mi = 0; mi < APP_MENU_GROUPS; mi++) {
            const char *name = app_menu_group_name(mi);
            bool active = a->menu_active && a->menu_group == mi;
            sgr_move(out, 0, xs[mi]);
            sgr_bg(out, active ? a->theme.accent_bg : a->theme.status_bg);
            sgr_fg(out, active ? a->theme.selection_fg : 255);
            str_putc(out, ' ');
            draw_text(out, name, strlen(name), ws[mi] - 2, false);
            str_putc(out, ' ');
        }
        sgr_move(out, 0, menu_w + 1);
        sgr_bg(out, a->theme.status_bg);
        sgr_bold(out);
        sgr_fg(out, 255);
        if (menu_w + 1 < a->cols)
            draw_text(out, text.data ? text.data : "", text.len,
                      a->cols - menu_w - 1, true);
    }
    sgr_reset(out);
    str_free(&text);
}

static void draw_header(App *a, Str *out)
{
    const Theme *t = &a->theme;
    char title[256];
    Buffer *b = app_buffer(a);

    sgr_move(out, 1, 0);
    sgr_bg(out, t->panel);
    cell(out, " FILE OUTLINE ", a->lw, t->panel,
         a->focus == FOCUS_OUTLINE ? t->selection_fg : t->dim, false);
    sep_cell(out, a);

    if (b) {
        snprintf(title, sizeof(title), " %s%s ", b->name,
                 buf_is_dirty(b) ? " *" : "");
    } else {
        snprintf(title, sizeof(title), " SI-C ");
    }
    cell(out, title, a->cw, t->panel,
         a->focus == FOCUS_EDITOR ? t->selection_fg : t->fg, true);
    if (a->rw > 0) {
        sep_cell(out, a);
        if (a->view.kind == VIEW_CONTEXT &&
            a->focus == FOCUS_CONTEXT) {
            size_t nodes = 0;
            size_t vi;
            for (vi = 0; vi < a->view.count; vi++) {
                if (!a->view.items[vi].is_header &&
                    a->view.items[vi].file_index >= 0)
                    nodes++;
            }
            snprintf(title, sizeof(title), " CALL FLOW (%lu) ",
                     (unsigned long)nodes);
        } else {
            snprintf(title, sizeof(title), " PROJECT FILES (%lu) ",
                     (unsigned long)a->project.file_count);
        }
        cell(out, title, a->rw, t->panel,
             a->focus == FOCUS_PROJECT ? t->selection_fg : t->dim, false);
    }
    sgr_reset(out);
}

static void current_function(App *a, char *out, size_t size)
{
    Buffer *b = app_buffer(a);
    int fi;
    int line;
    int i;
    int best = -1;
    int best_span = INT_MAX;

    out[0] = '\0';
    if (!b)
        return;
    fi = app_active_file_index(a);
    if (fi < 0 || (size_t)fi >= a->project.file_count)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
    for (i = 0; i < a->project.files[fi].sym_count; i++) {
        Symbol *s = &a->project.syms[a->project.files[fi].sym_start + i];
        if (s->kind != SYM_FUNCTION)
            continue;
        if (s->line <= line && line <= s->end_line) {
            int span = s->end_line - s->line;
            if (span < best_span) {
                best_span = span;
                best = a->project.files[fi].sym_start + i;
            }
        }
    }
    if (best >= 0)
        snprintf(out, size, "%s", a->project.syms[best].name);
}

static void draw_hint(App *a, Str *out)
{
    const char *hint;
    if (a->help)
        hint = " press any key to close help ";
    else if (a->menu_active)
        hint = " MENU  Up/Down select  Enter execute  Esc close ";
    else if (a->prompt == PROMPT_QUIT)
        hint = " y save all   n discard changes   c cancel ";
    else if (a->prompt == PROMPT_FORMAT)
        hint = " c/Enter current file   a all files   Esc cancel ";
    else if (a->prompt == PROMPT_PROJECT || a->prompt == PROMPT_NEW_PROJECT ||
             a->prompt == PROMPT_PROJECT_NAME)
        hint = " Enter confirm   Esc cancel   Left/Right edit ";
    else if (a->prompt != PROMPT_NONE)
        hint = " Enter execute   Esc cancel   Left/Right edit   Ctrl+U clear ";
    else if (a->focus == FOCUS_OUTLINE)
        hint = " OUTLINE  Up/Down select  Enter jumps  Tab focus  Esc editor  F1 help ";
    else if (a->focus == FOCUS_PROJECT)
        hint = " PROJECT  Up/Down select  Enter opens  Tab focus  Esc editor  F1 help ";
    else if (a->focus == FOCUS_CONTEXT)
        hint = " CALL FLOW  Up/Down select  Enter jumps  Tab focus  Esc editor  F1 help ";
    else if (a->focus == FOCUS_INFO)
        hint = " SYMBOL INFO  Tab focus  F4/Esc editor  F1 help ";
    else if (a->cols < 120)
        hint = " F2 outline  F3 flow  F4 editor  F7 files  F6 format  ^F find  F8 hi  F10 back  ^F10 fwd  F1 help ";
    else
        hint = " F2 outline  F3 call-flow  F4 editor  F7 files  F6 format  ^F find  F8 highlight  F10 back  ^F10 fwd  F12 def  Shift+F12 refs  F1 help ";

    sgr_move(out, a->rows - 2, 0);
    sgr_bg(out, a->theme.panel_alt);
    sgr_fg(out, a->theme.dim);
    draw_text(out, hint, strlen(hint), a->cols, true);
    sgr_reset(out);
}

static void draw_status(App *a, Str *out)
{
    const Theme *t = &a->theme;
    int row = a->rows - 1;

    sgr_move(out, row, 0);
    sgr_bg(out, t->status_bg);
    sgr_fg(out, 255);

    if (a->prompt != PROMPT_NONE) {
        const char *label = "";
        int lw;
        switch (a->prompt) {
        case PROMPT_FIND:
            label = "Find: ";
            break;
        case PROMPT_GREP:
            label = "Project search: ";
            break;
        case PROMPT_SYMBOL:
            label = "Symbol: ";
            break;
        case PROMPT_OPEN:
            label = "Open file: ";
            break;
        case PROMPT_GOTO:
            label = "Go to line: ";
            break;
        case PROMPT_PROJECT:
            label = "Open project directory: ";
            break;
        case PROMPT_NEW_PROJECT:
            label = "New project directory: ";
            break;
        case PROMPT_PROJECT_NAME:
            label = "New project folder name: ";
            break;
        case PROMPT_FORMAT:
            label = "Format: [c] current file  [a] all files  [Esc] cancel  ";
            break;
        case PROMPT_QUIT:
            label = "Unsaved changes: y=save all  n=discard  c=cancel  ";
            break;
        default:
            break;
        }
        lw = utf8_text_width(label, strlen(label));
        sgr_bold(out);
        draw_text(out, label, strlen(label), a->cols, false);
        if (a->prompt != PROMPT_QUIT) {
            int remaining = a->cols - lw;
            int used;
            if (remaining > 0)
                draw_text(out, a->prompt_text, (size_t)a->prompt_len,
                          remaining, false);
            used = lw + utf8_text_width(a->prompt_text,
                                        (size_t)a->prompt_len);
            if (used < a->cols)
                draw_spaces(out, a->cols - used);
            a->cursor_x = lw + utf8_text_width(a->prompt_text,
                                               (size_t)a->prompt_cursor);
            if (a->cursor_x >= a->cols)
                a->cursor_x = a->cols - 1;
            a->cursor_y = row;
            a->show_cursor = true;
        }
        sgr_reset(out);
        return;
    }

    {
        char info[256];
        char fn[128];
        Buffer *b = app_buffer(a);
        int info_w;
        int left_w;
        int line = 1;
        int col = 1;

        if (b) {
            line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
            col = buf_visual_col(b, (size_t)b->cursor) + 1;
        }
        current_function(a, fn, sizeof(fn));
        if (b && fn[0])
            snprintf(info, sizeof(info), "Ln %d, Col %d  %s  fn:%s ",
                     line, col, buf_is_dirty(b) ? "modified" : "saved", fn);
        else if (b)
            snprintf(info, sizeof(info), "Ln %d, Col %d  %s ",
                     line, col, buf_is_dirty(b) ? "modified" : "saved");
        else
            snprintf(info, sizeof(info), " no file ");

        info_w = utf8_text_width(info, strlen(info));
        if (info_w > a->cols / 2)
            info_w = a->cols / 2;
        left_w = a->cols - info_w;
        if (left_w < 0)
            left_w = 0;
        cell(out, a->status, left_w, t->status_bg, 255, true);
        cell(out, info, info_w, t->status_bg, 220, false);
    }

    if (!a->help && a->focus == FOCUS_EDITOR && app_buffer(a)) {
        Buffer *b = app_buffer(a);
        int line = buf_line_of_offset(b, (size_t)b->cursor);
        int vcol = buf_visual_col(b, (size_t)b->cursor);
        int y = a->content_top + line - b->scroll_line;
        int x = a->lw + 1 + a->gutter_w + 3 + vcol - b->scroll_col;
        if (line >= b->scroll_line && line < b->scroll_line + a->ed_h &&
            x >= a->lw + 1 + a->gutter_w + 3 &&
            x < a->lw + 1 + a->cw) {
            a->cursor_y = y;
            a->cursor_x = x;
            a->show_cursor = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Centered New Project name prompt                                    */
/* ------------------------------------------------------------------ */

static void draw_project_name_prompt(App *a, Str *out)
{
    int boxw = SI_MIN(66, a->cols - 4);
    int boxh = 9;
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int i;
    char parent[512];
    int parent_cols;
    int input_cols;
    int cursor_col;

    if (boxw < 48 || boxh >= a->rows)
        return;

    path_shorten(a->pending_parent[0] ? a->pending_parent : "",
                 SI_MAX(10, content - 9), parent, sizeof(parent));
    parent_cols = (int)utf8_text_width(parent, strlen(parent));
    input_cols = (int)utf8_text_width(a->prompt_text,
                                      (size_t)a->prompt_len);
    cursor_col = (int)utf8_text_width(a->prompt_text,
                                      (size_t)a->prompt_cursor);

    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 1) {
                sgr_fg(out, a->theme.fg);
                sgr_bold(out);
                draw_center(out, "NEW PROJECT", content);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else if (i == 3) {
                char line[600];
                snprintf(line, sizeof(line), "Parent: %s", parent);
                sgr_fg(out, a->theme.fg);
                draw_text(out, line, strlen(line), content, false);
                sgr_fg(out, a->theme.border);
                if ((int)strlen(line) < content)
                    draw_spaces(out, content - (int)strlen(line));
                str_putc(out, ' ');
            } else if (i == 4) {
                const char *label = "Project folder name:";
                sgr_fg(out, a->theme.dim);
                draw_text(out, label, strlen(label), content, false);
                if ((int)strlen(label) < content)
                    draw_spaces(out, content - (int)strlen(label));
                str_putc(out, ' ');
            } else if (i == 5) {
                int used;
                sgr_fg(out, a->theme.fg);
                sgr_bold(out);
                str_puts(out, "> ");
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.fg);
                sgr_fg(out, a->theme.fg);
                draw_text(out, a->prompt_text, (size_t)a->prompt_len,
                          content - 2, false);
                used = 2 + input_cols;
                if (used < content)
                    draw_spaces(out, content - used);
                str_putc(out, ' ');
                a->cursor_x = x + 2 + 2 + cursor_col;
                a->cursor_y = y + i;
                if (a->cursor_x > x + boxw - 2)
                    a->cursor_x = x + boxw - 2;
                a->show_cursor = true;
            } else if (i == 7) {
                const char *hint = "Enter: create    Esc: cancel";
                sgr_fg(out, a->theme.dim);
                draw_text(out, hint, strlen(hint), content, false);
                if ((int)strlen(hint) < content)
                    draw_spaces(out, content - (int)strlen(hint));
                str_putc(out, ' ');
            } else {
                draw_spaces(out, content);
                str_putc(out, ' ');
            }
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    (void)parent_cols;
}

/* ------------------------------------------------------------------ */
/* Prominent modal prompts                                             */
/* ------------------------------------------------------------------ */

static void draw_input_modal(App *a, Str *out, const char *title,
                             const char *message, const char *hint)
{
    int boxw = SI_MIN(64, a->cols - 4);
    int boxh = 7;
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int i;
    int cursor_col;

    if (boxw < 40 || boxh >= a->rows)
        return;
    cursor_col = (int)utf8_text_width(a->prompt_text,
                                      (size_t)a->prompt_cursor);

    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 1) {
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
                sgr_bold(out);
                draw_center(out, title, content);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else if (i == 2) {
                sgr_fg(out, a->theme.fg);
                draw_text(out, message, strlen(message), content, false);
                if ((int)strlen(message) < content)
                    draw_spaces(out, content - (int)strlen(message));
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else if (i == 3) {
                int used;
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
                sgr_bold(out);
                str_puts(out, "> ");
                used = 2 + draw_text(out, a->prompt_text,
                                     (size_t)a->prompt_len,
                                     content - 2, false);
                if (used < content)
                    draw_spaces(out, content - used);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
                a->cursor_x = x + 2 + 2 + cursor_col;
                a->cursor_y = y + i;
                if (a->cursor_x > x + boxw - 2)
                    a->cursor_x = x + boxw - 2;
                a->show_cursor = true;
            } else if (i == 4) {
                sgr_fg(out, a->theme.dim);
                draw_text(out, hint, strlen(hint), content, false);
                if ((int)strlen(hint) < content)
                    draw_spaces(out, content - (int)strlen(hint));
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else {
                draw_spaces(out, content);
                str_putc(out, ' ');
            }
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
}

static void draw_quit_modal(App *a, Str *out)
{
    int boxw = SI_MIN(68, a->cols - 4);
    int boxh = 9;
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int i;
    int dirty = 0;
    int shown = 0;
    char list[512];
    char msg[128];
    list[0] = '\0';
    for (i = 0; i < a->nbuf; i++) {
        Buffer *b = a->bufs[i];
        if (!b || !buf_is_dirty(b))
            continue;
        dirty++;
        if (shown < 2) {
            size_t used = strlen(list);
            snprintf(list + used, sizeof(list) - used, "%s%s",
                     used ? ", " : "", b->name ? b->name : "?");
            shown++;
        }
    }
    if (dirty > shown) {
        size_t used = strlen(list);
        snprintf(list + used, sizeof(list) - used, " ...");
    }
    snprintf(msg, sizeof(msg), "%d file%s %s unsaved changes:",
             dirty, dirty == 1 ? "" : "s",
             dirty == 1 ? "has" : "have");
    if (boxw < 44 || boxh >= a->rows)
        return;

    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else if (i == 1) {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            sgr_bg(out, a->theme.accent_bg);
            sgr_fg(out, a->theme.selection_fg);
            sgr_bold(out);
            draw_center(out, "UNSAVED CHANGES", content);
            sgr_reset(out);
            sgr_bg(out, a->theme.panel_alt);
            sgr_fg(out, a->theme.border);
            str_putc(out, ' ');
            str_puts(out, "\xe2\x94\x82");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 2) {
                sgr_fg(out, a->theme.fg);
                draw_text(out, msg, strlen(msg), content, false);
                if ((int)strlen(msg) < content)
                    draw_spaces(out, content - (int)strlen(msg));
            } else if (i == 3) {
                sgr_fg(out, a->theme.dim);
                draw_text(out, list, strlen(list), content, false);
                if ((int)strlen(list) < content)
                    draw_spaces(out, content - (int)strlen(list));
            } else if (i == 5) {
                sgr_fg(out, a->theme.fg);
                {
                    const char *s = "[Y]  Save all and quit";
                    sgr_fg(out, a->theme.fg);
                    draw_text(out, s, strlen(s), content, false);
                    if ((int)strlen(s) < content)
                        draw_spaces(out, content - (int)strlen(s));
                }
            } else if (i == 6) {
                {
                    const char *s = "[N]  Discard changes and quit";
                    sgr_fg(out, a->theme.fg);
                    draw_text(out, s, strlen(s), content, false);
                    if ((int)strlen(s) < content)
                        draw_spaces(out, content - (int)strlen(s));
                }
            } else if (i == 7) {
                {
                    const char *s = "[C] / Esc  Cancel";
                    sgr_fg(out, a->theme.dim);
                    draw_text(out, s, strlen(s), content, false);
                    if ((int)strlen(s) < content)
                        draw_spaces(out, content - (int)strlen(s));
                }
            } else {
                draw_spaces(out, content);
            }
            sgr_fg(out, a->theme.border);
            str_putc(out, ' ');
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

static void draw_format_modal(App *a, Str *out)
{
    int boxw = SI_MIN(52, a->cols - 4);
    int boxh = 7;
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int i;
    if (boxw < 40 || boxh >= a->rows)
        return;
    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 1) {
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
                sgr_bold(out);
                draw_center(out, "FORMAT CODE", content);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else if (i == 2) {
                const char *s = "Choose formatting scope:";
                sgr_fg(out, a->theme.fg);
                draw_text(out, s, strlen(s), content, false);
                if ((int)strlen(s) < content)
                    draw_spaces(out, content - (int)strlen(s));
            } else if (i == 3) {
                const char *s = "[C]  Current file";
                sgr_fg(out, a->theme.fg);
                draw_text(out, s, strlen(s), content, false);
                if ((int)strlen(s) < content)
                    draw_spaces(out, content - (int)strlen(s));
            } else if (i == 4) {
                const char *s = "[A]  All project files";
                sgr_fg(out, a->theme.fg);
                draw_text(out, s, strlen(s), content, false);
                if ((int)strlen(s) < content)
                    draw_spaces(out, content - (int)strlen(s));
            } else if (i == 5) {
                const char *s = "[Esc]  Cancel";
                sgr_fg(out, a->theme.dim);
                draw_text(out, s, strlen(s), content, false);
                if ((int)strlen(s) < content)
                    draw_spaces(out, content - (int)strlen(s));
            } else {
                draw_spaces(out, content);
            }
            sgr_fg(out, a->theme.border);
            str_putc(out, ' ');
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

static void draw_search_prompt(App *a, Str *out)
{
    const char *title;
    const char *hint;
    int boxw = SI_MIN(72, a->cols - 4);
    int boxh = 5;
    int x = (a->cols - boxw) / 2;
    int y = 1;
    int content = boxw - 4;
    int i;
    int cursor_col;

    switch (a->prompt) {
    case PROMPT_GREP:
        title = "PROJECT TEXT SEARCH";
        hint = "Enter: search     Esc: cancel";
        break;
    case PROMPT_SYMBOL:
        title = "PROJECT SYMBOL SEARCH";
        hint = "Up/Down: select   Enter: jump   Esc: cancel";
        break;
    default:
        title = "FIND IN FILE";
        hint = "Enter: find next     Esc: close";
        break;
    }
    if (boxw < 44 || y + boxh > a->rows)
        return;
    cursor_col = (int)utf8_text_width(a->prompt_text,
                                      (size_t)a->prompt_cursor);
    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 1) {
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
                sgr_bold(out);
                draw_center(out, title, content);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else if (i == 2) {
                int used;
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
                sgr_bold(out);
                str_puts(out, "> ");
                used = 2 + draw_text(out, a->prompt_text,
                                     (size_t)a->prompt_len,
                                     content - 2, false);
                if (used < content)
                    draw_spaces(out, content - used);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
                a->cursor_x = x + 4 + cursor_col;
                a->cursor_y = y + i;
                if (a->cursor_x > x + boxw - 2)
                    a->cursor_x = x + boxw - 2;
                a->show_cursor = true;
            } else {
                sgr_fg(out, a->theme.dim);
                draw_text(out, hint, strlen(hint), content, false);
                if ((int)strlen(hint) < content)
                    draw_spaces(out, content - (int)strlen(hint));
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            }
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
}

static void draw_prompt_modal(App *a, Str *out)
{
    char msg[160];
    if (a->prompt == PROMPT_GOTO) {
        Buffer *b = app_buffer(a);
        int count = b ? (int)buf_line_count(b) : 1;
        if (count < 1)
            count = 1;
        snprintf(msg, sizeof(msg), "Enter a line from 1 to %d (digits only)",
                 count);
        draw_input_modal(a, out, "GO TO LINE", msg,
                         "Enter: jump     Esc: cancel");
    } else if (a->prompt == PROMPT_QUIT) {
        draw_quit_modal(a, out);
    } else if (a->prompt == PROMPT_FORMAT) {
        draw_format_modal(a, out);
    } else if (a->prompt == PROMPT_OPEN) {
        draw_input_modal(a, out, "OPEN FILE", "Enter a file path",
                         "Enter: open     Esc: cancel");
    } else if (a->prompt == PROMPT_PROJECT) {
        draw_input_modal(a, out, "OPEN PROJECT", "Enter project directory",
                         "Enter: open     Esc: cancel");
    } else if (a->prompt == PROMPT_NEW_PROJECT) {
        draw_input_modal(a, out, "NEW PROJECT", "Enter parent directory",
                         "Enter: continue     Esc: cancel");
    }
}

/* ------------------------------------------------------------------ */
/* Relation-table build / confirmation modal                           */
/* ------------------------------------------------------------------ */

static void draw_relation_modal(App *a, Str *out)
{
    int boxw = SI_MIN(68, a->cols - 4);
    int boxh = 12;
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int i;
    const char *title = a->relation_building
                            ? "BUILDING RELATION TABLES"
                            : "RELATION TABLES READY";

    if (boxw < 40 || boxh >= a->rows)
        return;
    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 1) {
                sgr_fg(out, a->theme.fg);
                sgr_bold(out);
                draw_center(out, title, content);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else if (i == 3) {
                char line[256];
                snprintf(line, sizeof(line), "files: %lu    symbols: %lu",
                         (unsigned long)a->relation_files,
                         (unsigned long)a->relation_symbols);
                sgr_fg(out, a->theme.fg);
                draw_text(out, line, strlen(line), content, false);
                sgr_fg(out, a->theme.border);
                draw_spaces(out, content -
                            (int)utf8_text_width(line, strlen(line)));
                str_putc(out, ' ');
            } else if (i == 4 && !a->relation_building) {
                char line[256];
                snprintf(line, sizeof(line),
                         "call edges: %lu    variable uses: %lu",
                         (unsigned long)a->relation_edges,
                         (unsigned long)a->relation_vars);
                sgr_fg(out, a->theme.fg);
                draw_text(out, line, strlen(line), content, false);
                sgr_fg(out, a->theme.border);
                draw_spaces(out, content -
                            (int)utf8_text_width(line, strlen(line)));
                str_putc(out, ' ');
            } else if (i == 6 && a->relation_building) {
                const char *line = "Scanning all files and building edges...";
                sgr_fg(out, a->theme.dim);
                draw_text(out, line, strlen(line), content, false);
                draw_spaces(out, content -
                            (int)utf8_text_width(line, strlen(line)));
                str_putc(out, ' ');
            } else if (i == 7 && !a->relation_building) {
                const char *hint = "Enter / Esc / mouse click: OK";
                sgr_fg(out, a->theme.dim);
                draw_text(out, hint, strlen(hint), content, false);
                draw_spaces(out, content -
                            (int)utf8_text_width(hint, strlen(hint)));
                str_putc(out, ' ');
            } else if (i == 9 && !a->relation_building) {
                const char *ok = "[ OK ]";
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
                draw_center(out, ok, content);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else {
                draw_spaces(out, content);
            }
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

/* ------------------------------------------------------------------ */
/* Large-project relation notice                                       */
/* ------------------------------------------------------------------ */

static void draw_large_project_prompt(App *a, Str *out)
{
    int boxw = SI_MIN(72, a->cols - 4);
    int boxh = 10;
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int i;
    char line[256];

    if (boxw < 48 || boxh >= a->rows)
        return;
    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 1) {
                sgr_fg(out, a->theme.fg);
                sgr_bold(out);
                draw_center(out, "LARGE PROJECT", content);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                str_putc(out, ' ');
            } else if (i == 3) {
                snprintf(line, sizeof(line), "files: %lu    depth: %d",
                         (unsigned long)a->project.file_count,
                         a->project.scan_depth);
                sgr_fg(out, a->theme.fg);
                draw_text(out, line, strlen(line), content, false);
                if ((int)strlen(line) < content)
                    draw_spaces(out, content - (int)strlen(line));
                str_putc(out, ' ');
            } else if (i == 4) {
                const char *msg =
                    "Relation tables were not built at startup.";
                sgr_fg(out, a->theme.fg);
                draw_text(out, msg, strlen(msg), content, false);
                if ((int)strlen(msg) < content)
                    draw_spaces(out, content - (int)strlen(msg));
                str_putc(out, ' ');
            } else if (i == 5) {
                const char *msg =
                    "This keeps opening a very large tree responsive.";
                sgr_fg(out, a->theme.dim);
                draw_text(out, msg, strlen(msg), content, false);
                if ((int)strlen(msg) < content)
                    draw_spaces(out, content - (int)strlen(msg));
                str_putc(out, ' ');
            } else if (i == 7) {
                const char *msg =
                    "Alt/Opt+R: build now  Enter: browse without it";
                sgr_fg(out, a->theme.fg);
                sgr_bold(out);
                draw_text(out, msg, strlen(msg), content, false);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
                if ((int)strlen(msg) < content)
                    draw_spaces(out, content - (int)strlen(msg));
                str_putc(out, ' ');
            } else {
                draw_spaces(out, content);
                str_putc(out, ' ');
            }
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

/* ------------------------------------------------------------------ */
/* Directory picker overlay                                            */
/* ------------------------------------------------------------------ */

static void draw_dir_picker(App *a, Str *out)
{
    int boxw = SI_MIN(76, a->cols - 4);
    int boxh = SI_MIN(24, a->rows - 4);
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int i;

    if (boxw < 40 || boxh < 10)
        return;
    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (i == 1) {
                const char *title = a->dir_for_new
                                        ? "NEW PROJECT: SELECT PARENT FOLDER"
                                        : "OPEN PROJECT: SELECT FOLDER";
                sgr_fg(out, a->theme.fg);
                sgr_bold(out);
                draw_text(out, title, strlen(title), content, false);
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
            } else if (i == 2) {
                char shown[1024];
                path_shorten(a->dir_path, content, shown, sizeof(shown));
                sgr_fg(out, a->theme.dim);
                draw_text(out, shown, strlen(shown), content, false);
                sgr_fg(out, a->theme.border);
            } else if (i == 3) {
                sgr_fg(out, a->theme.dim);
                draw_text(out, "Folders", 7, content, false);
                sgr_fg(out, a->theme.border);
            } else if (i < boxh - 2) {
                int slot = i - 4;
                int idx = a->dir_scroll + slot;
                int count = (int)a->dir_count + 2;
                if (idx >= 0 && idx < count) {
                    bool selected = idx == a->dir_sel;
                    char label[1200];
                    if (idx == 0)
                        snprintf(label, sizeof(label), "[ Use this folder ]");
                    else if (idx == 1)
                        snprintf(label, sizeof(label), "..");
                    else
                        snprintf(label, sizeof(label), "%s/",
                                 a->dir_items[idx - 2]);
                    if (selected) {
                        sgr_bg(out, a->theme.accent_bg);
                        sgr_fg(out, a->theme.selection_fg);
                    } else {
                        sgr_fg(out, a->theme.fg);
                    }
                    draw_text(out, label, strlen(label), content, false);
                    if (selected) {
                        sgr_reset(out);
                        sgr_bg(out, a->theme.panel_alt);
                        sgr_fg(out, a->theme.border);
                    }
                }
            } else {
                const char *hint =
                    "Enter open/select   Backspace parent   Esc cancel";
                sgr_fg(out, a->theme.dim);
                draw_text(out, hint, strlen(hint), content, false);
                sgr_fg(out, a->theme.border);
            }
            {
                int used = 0;
                if (i >= 4 && i < boxh - 2) {
                    int idx = a->dir_scroll + i - 4;
                    int count = (int)a->dir_count + 2;
                    if (idx >= 0 && idx < count) {
                        char label[1200];
                        if (idx == 0)
                            snprintf(label, sizeof(label),
                                     "[ Use this folder ]");
                        else if (idx == 1)
                            snprintf(label, sizeof(label), "..");
                        else
                            snprintf(label, sizeof(label), "%s/",
                                     a->dir_items[idx - 2]);
                        used = utf8_text_width(label, strlen(label));
                    }
                }
                if (used < content)
                    draw_spaces(out, content - used);
            }
            str_putc(out, ' ');
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

/* ------------------------------------------------------------------ */
/* Right-click copy/paste menu                                        */
/* ------------------------------------------------------------------ */

static void draw_context_menu(App *a, Str *out)
{
    int w = 24;
    int h = 4;
    int x = a->context_menu_x;
    int y = a->context_menu_y;
    int content = w - 4;
    int i;
    const char *items[2] = {"Copy", "Paste"};
    bool enabled[2] = {a->context_menu_copy, a->context_menu_paste};

    if (!a->context_menu_active)
        return;
    if (x + w > a->cols - 1)
        x = a->cols - 1 - w;
    if (y + h > a->rows - 1)
        y = a->rows - 1 - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    for (i = 0; i < h; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == h - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < w - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            int item = i - 1;
            bool sel = item == a->context_menu_sel;
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            sgr_bg(out, sel ? a->theme.accent_bg : a->theme.panel_alt);
            if (enabled[item])
                sgr_fg(out, sel ? a->theme.selection_fg : a->theme.fg);
            else
                sgr_fg(out, a->theme.dim);
            if (sel)
                sgr_bold(out);
            draw_text(out, items[item], strlen(items[item]), content, true);
            sgr_reset(out);
            sgr_bg(out, a->theme.panel_alt);
            sgr_fg(out, a->theme.border);
            str_putc(out, ' ');
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

/* ------------------------------------------------------------------ */
/* Main menu overlay                                                   */
/* ------------------------------------------------------------------ */

static void draw_menu(App *a, Str *out)
{
    int xs[APP_MENU_GROUPS];
    int ws[APP_MENU_GROUPS];
    int group = a->menu_group;
    int count = app_menu_group_size(group);
    int x;
    int boxw;
    int boxh = count + 2;
    int content;
    int i;

    if (!a->menu_active)
        return;
    app_menu_tab_layout(a, xs, ws);
    boxw = SI_MIN(app_menu_group_width(group), a->cols - 2);
    x = xs[group];
    if (x + boxw > a->cols - 1)
        x = a->cols - 1 - boxw;
    if (x < 0)
        x = 0;
    if (boxw < 20)
        return;
    content = boxw - 4;
    for (i = 0; i < boxh; i++) {
        sgr_move(out, 1 + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            int idx = i - 1;
            const char *label = app_menu_item_label(group, idx);
            const char *key = app_menu_item_key(group, idx);
            bool selected = idx == a->menu_sel;
            str_puts(out, "\xe2\x94\x82");
            str_putc(out, ' ');
            if (selected) {
                sgr_bg(out, a->theme.accent_bg);
                sgr_fg(out, a->theme.selection_fg);
            } else {
                sgr_fg(out, a->theme.fg);
            }
            draw_text(out, label, strlen(label), content - 9, false);
            {
                int used = utf8_text_width(label, strlen(label));
                if (used < content - 9)
                    draw_spaces(out, content - 9 - used);
            }
            sgr_fg(out, selected ? a->theme.selection_fg : a->theme.dim);
            draw_text(out, key, strlen(key), 9, false);
            {
                int used = utf8_text_width(key, strlen(key));
                if (used < 9)
                    draw_spaces(out, 9 - used);
            }
            if (selected) {
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
            }
            str_putc(out, ' ');
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

/* ------------------------------------------------------------------ */
/* Definition chooser overlay                                          */
/* ------------------------------------------------------------------ */

static void draw_chooser(App *a, Str *out)
{
    int boxw = SI_MIN(100, a->cols - 2);
    int boxh = SI_MIN(24, a->rows - 2);
    int x = (a->cols - boxw) / 2;
    int y = (a->rows - boxh) / 2;
    int content = boxw - 4;
    int visible = SI_MAX(1, (boxh - 4) / 2);
    int i;

    if (boxw < 40 || boxh < 8)
        return;
    if (a->chooser_sel < a->chooser_scroll)
        a->chooser_scroll = a->chooser_sel;
    if (a->chooser_sel >= a->chooser_scroll + visible)
        a->chooser_scroll = a->chooser_sel - visible + 1;
    for (i = 0; i < boxh; i++) {
        sgr_move(out, y + i, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (i == 0 || i == boxh - 1) {
            int j;
            str_puts(out, i == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (j = 0; j < boxw - 2; j++)
                str_puts(out, "\xe2\x94\x80");
            str_puts(out, i == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else {
            str_puts(out, "\xe2\x94\x82");
            if (i == 1) {
                char title[256];
                snprintf(title, sizeof(title), "SELECT DEFINITION: %s",
                         a->chooser_word);
                str_putc(out, ' ');
                sgr_fg(out, a->theme.fg);
                sgr_bold(out);
                draw_text(out, title, strlen(title), content, false);
                {
                    int used = utf8_text_width(title, strlen(title));
                    if (used < content)
                        draw_spaces(out, content - used);
                }
                str_putc(out, ' ');
                sgr_reset(out);
                sgr_bg(out, a->theme.panel_alt);
                sgr_fg(out, a->theme.border);
            } else if (i == boxh - 2) {
                const char *hint = "Up/Down choose   Enter select   Esc cancel";
                str_putc(out, ' ');
                sgr_fg(out, a->theme.dim);
                draw_text(out, hint, strlen(hint), content, false);
                {
                    int used = utf8_text_width(hint, strlen(hint));
                    if (used < content)
                        draw_spaces(out, content - used);
                }
                str_putc(out, ' ');
                sgr_fg(out, a->theme.border);
            } else {
                int slot = (i - 2) / 2;
                int rowoff = (i - 2) % 2;
                int idx = a->chooser_scroll + slot;
                str_putc(out, ' ');
                if (idx >= 0 && (size_t)idx < a->chooser_count) {
                    ChooserItem *ci = &a->chooser_items[idx];
                    Symbol *s = &a->project.syms[ci->sym_index];
                    bool selected = idx == a->chooser_sel;
                    if (selected) {
                        sgr_bg(out, a->theme.accent_bg);
                        sgr_fg(out, a->theme.selection_fg);
                    } else {
                        sgr_fg(out, rowoff == 0 ? a->theme.fg
                                                : a->theme.dim);
                    }
                    if (rowoff == 0) {
                        char head[320];
                        snprintf(head, sizeof(head), "%c %s:%d  %s",
                                 selected ? '>' : ' ', 
                                 a->project.files[s->file_index].rel,
                                 s->line, s->signature);
                        draw_text(out, head, strlen(head), content, false);
                    } else {
                        draw_text(out, ci->code, strlen(ci->code), content,
                                  false);
                    }
                    if (selected) {
                        sgr_reset(out);
                        sgr_bg(out, a->theme.panel_alt);
                        sgr_fg(out, a->theme.border);
                    }
                }
                {
                    int used = 0;
                    if (idx >= 0 && (size_t)idx < a->chooser_count) {
                        ChooserItem *ci = &a->chooser_items[idx];
                        Symbol *s = &a->project.syms[ci->sym_index];
                        if (rowoff == 0) {
                            char head[320];
                            snprintf(head, sizeof(head), "%c %s:%d  %s",
                                     idx == a->chooser_sel ? '>' : ' ',
                                     a->project.files[s->file_index].rel,
                                     s->line, s->signature);
                            used = utf8_text_width(head, strlen(head));
                        } else {
                            used = utf8_text_width(ci->code,
                                                   strlen(ci->code));
                        }
                    }
                    if (used < content)
                        draw_spaces(out, content - used);
                }
                str_putc(out, ' ');
            }
            str_puts(out, "\xe2\x94\x82");
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

/* ------------------------------------------------------------------ */
/* Help overlay                                                        */
/* ------------------------------------------------------------------ */

static void draw_help(App *a, Str *out)
{
    enum { HELP_COLS = 4, HELP_ROWS = 9 };
    static const char *const lines[HELP_ROWS * HELP_COLS] = {
        "F1  Help",         "F2  Outline",       "F3  Call flow",
        "F4  Editor",       "F5  Reindex",       "F6  Format",
        "F7  Files",        "F8  Highlight",     "F9  Next file",
        "F10 Back",         "Alt/Opt+M Menu",    "F12 Definition",
        "Ctrl+F10 Fwd",     "Ctrl+A Save all",   "Ctrl+B Outline",
        "Ctrl+C Copy",      "Ctrl+D Delete",     "Ctrl+E End line",
        "Ctrl+F Find",      "Ctrl+G Goto line",  "Ctrl+H Help",
        "Ctrl+K Kill line", "Ctrl+L Goto line",  "Ctrl+N New proj",
        "Ctrl+O Open proj", "Ctrl+P Symbols",    "Ctrl+Q Quit",
        "Ctrl+R Call flow", "Ctrl+S Save",       "Ctrl+T Cycle focus",
        "Ctrl+U Kill start","Ctrl+V Paste",      "Ctrl+Y Redo",
        "Ctrl+Z Undo",      "Alt/Opt+R Rebuild", "Tab/Shift+Tab"
    };
    int max_col_w = (a->cols - 5) / HELP_COLS;
    int col_w = SI_MIN(22, max_col_w);
    int boxw;
    int boxh = HELP_ROWS + 4;
    int x;
    int y;
    int c;
    int r;

    if (a->cols < 60 || a->rows < boxh + 2)
        return;
    if (col_w < 12)
        col_w = 12;
    boxw = 1 + HELP_COLS * col_w + HELP_COLS;
    if (boxw > a->cols - 2)
        return;
    x = (a->cols - boxw) / 2;
    y = (a->rows - boxh) / 2;

    for (r = 0; r < boxh; r++) {
        sgr_move(out, y + r, x);
        sgr_bg(out, a->theme.panel_alt);
        sgr_fg(out, a->theme.border);
        if (r == 0 || r == boxh - 1) {
            str_puts(out, r == 0 ? "\xe2\x94\x8c" : "\xe2\x94\x94");
            for (c = 0; c < HELP_COLS; c++) {
                int j;
                for (j = 0; j < col_w; j++)
                    str_puts(out, "\xe2\x94\x80");
                if (c < HELP_COLS - 1)
                    str_puts(out, r == 0 ? "\xe2\x94\xac" : "\xe2\x94\xb4");
            }
            str_puts(out, r == 0 ? "\xe2\x94\x90" : "\xe2\x94\x98");
        } else if (r == 1) {
            str_puts(out, "\xe2\x94\x82");
            sgr_fg(out, a->theme.fg);
            sgr_bold(out);
            draw_center(out, "SI-C HELP", boxw - 2);
            sgr_reset(out);
            sgr_bg(out, a->theme.panel_alt);
            sgr_fg(out, a->theme.border);
            str_puts(out, "\xe2\x94\x82");
        } else if (r == 2) {
            str_puts(out, "\xe2\x94\x9c");
            for (c = 0; c < HELP_COLS; c++) {
                int j;
                for (j = 0; j < col_w; j++)
                    str_puts(out, "\xe2\x94\x80");
                if (c < HELP_COLS - 1)
                    str_puts(out, "\xe2\x94\xbc");
            }
            str_puts(out, "\xe2\x94\xa4");
        } else {
            int row = r - 3;
            str_puts(out, "\xe2\x94\x82");
            for (c = 0; c < HELP_COLS; c++) {
                int li = c * HELP_ROWS + row;
                int cell = col_w - 1;
                int used;
                str_putc(out, ' ');
                sgr_fg(out, a->theme.fg);
                used = draw_text(out, lines[li], strlen(lines[li]),
                                 cell, false);
                if (used < cell)
                    draw_spaces(out, cell - used);
                sgr_fg(out, a->theme.border);
                str_puts(out, "\xe2\x94\x82");
            }
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

static void draw_splash(App *a, Str *out, int row)
{
    static const char *const lines[] = {
        "SI-C  Source Insight style C/C++ browser",
        "",
        "F2 project files   F3 symbols   F12 go to definition",
        "Ctrl+P symbol search  Ctrl+G go to line  Alt/Opt+G project search",
        "Ctrl+O project   Ctrl+F find   F8 highlight   F1 help"
    };
    int start = a->content_height / 2 - 2;
    int li = row - start;
    sgr_bg(out, a->theme.bg);
    sgr_fg(out, li >= 0 && li < (int)SI_ARRAY_LEN(lines)
                    ? (li == 0 ? a->theme.fg : a->theme.dim)
                    : a->theme.dim);
    if (li >= 0 && li < (int)SI_ARRAY_LEN(lines)) {
        if (li == 0)
            sgr_bold(out);
        draw_center(out, lines[li], a->cw);
    } else {
        draw_spaces(out, a->cw);
    }
    sgr_reset(out);
}

static void draw_divider(App *a, Str *out, int width, const char *label,
                         bool focused)
{
    int used = utf8_text_width(label, strlen(label));
    int i;
    sgr_bg(out, focused ? a->theme.accent_bg : a->theme.panel_alt);
    sgr_fg(out, focused ? a->theme.selection_fg : a->theme.dim);
    if (focused)
        sgr_bold(out);
    draw_text(out, label, strlen(label), width, false);
    for (i = used; i < width; i++)
        str_puts(out, "\xe2\x94\x80");
    sgr_reset(out);
}

static void draw_info_header(App *a, Str *out, const char *label,
                             const char *value)
{
    const Theme *t = &a->theme;
    int label_w = 16;
    bool defined = strcmp(label, "DEFINED") == 0;
    int used;
    sgr_bg(out, t->panel);
    sgr_fg(out, a->focus == FOCUS_INFO ? t->selection_fg : t->dim);
    draw_spaces(out, 1);
    draw_text(out, label, strlen(label), label_w, true);
    if (defined) {
        sgr_find_highlight_bg(out);
        sgr_find_highlight_fg(out);
    } else {
        sgr_fg(out, t->fg);
    }
    {
        int val_w = (int)utf8_text_width(value, strlen(value));
        int max_w = a->cw - label_w - 2;
        if (val_w > max_w)
            val_w = max_w;
        draw_text(out, value, strlen(value), max_w, false);
        used = 1 + label_w + val_w;
    }
    if (used < a->cw) {
        sgr_bg(out, t->panel);
        draw_spaces(out, a->cw - used);
    }
    sgr_reset(out);
}

static void draw_info(App *a, Str *out, int row)
{
    const Theme *t = &a->theme;
    Buffer *ib = a->info_buffer ? a->info_buffer : app_buffer(a);
    int line_count = ib ? (int)buf_line_count(ib) : 0;
    int code_row = row - 2;
    int line = a->info_code_start + a->info_scroll + code_row;
    int digits = 1;
    int n = line_count;
    int gutter;
    int text_w;

    if (a->info_valid && row == 0) {
        draw_info_header(a, out, a->info_kind[0] ? a->info_kind : "SYMBOL",
                         a->info_word);
        return;
    }
    if (a->info_valid && row == 1) {
        draw_info_header(a, out, "DEFINED", a->info_where);
        return;
    }
    while (n >= 10) {
        n /= 10;
        digits++;
    }
    if (digits < 3)
        digits = 3;
    gutter = digits;
    text_w = a->cw - gutter - 3;
    if (!a->info_valid || !ib || a->info_code_start <= 0) {
        sgr_bg(out, t->panel);
        if (row == 0) {
            sgr_fg(out, t->dim);
            draw_spaces(out, 1);
            draw_text(out, "CLICK A FUNCTION OR VARIABLE TO SEE ITS CODE",
                      strlen("CLICK A FUNCTION OR VARIABLE TO SEE ITS CODE"),
                      a->cw - 1, true);
        } else {
            draw_spaces(out, a->cw);
        }
        sgr_reset(out);
        return;
    }
    if (code_row < 0 || line < 1 || line > line_count ||
        line > a->info_def_end) {
        sgr_bg(out, t->panel);
        draw_spaces(out, a->cw);
        sgr_reset(out);
        return;
    }
    sgr_bg(out, t->panel);
    sgr_fg(out, line == a->info_def_line ? t->accent_bg : t->dim);
    {
        char num[32];
        snprintf(num, sizeof(num), "%*d", gutter, line);
        draw_text(out, num, strlen(num), gutter, true);
    }
    sgr_reset(out);
    sgr_bg(out, t->panel);
    sgr_fg(out, t->border);
    str_puts(out, " \xe2\x94\x82 ");
    sgr_reset(out);
    if (line <= a->info_def_end) {
        draw_code_range(a, out, ib, line - 1, text_w, 0, false, true,
                        a->info_def_file, line == a->info_def_line);
    } else {
        sgr_bg(out, t->bg);
        draw_spaces(out, text_w);
        sgr_reset(out);
    }
}

static void draw_content(App *a, Str *out)
{
    int row;
    Buffer *b = app_buffer(a);
    char dlabel[160];

    for (row = 0; row < a->content_height; row++) {
        sgr_move(out, a->content_top + row, 0);
        draw_outline_row(a, out, row);
        sep_cell(out, a);

        if (row < a->top_h) {
            if (!b) {
                draw_splash(a, out, row);
            } else {
                int line = b->scroll_line + row;
                draw_editor_row(a, out, row, line);
            }
        } else if (row == a->top_h) {
            if (a->info_valid && a->info_word[0])
                snprintf(dlabel, sizeof(dlabel), " \xe2\x86\x95 CODE  %s  %s ",
                         a->info_word, a->info_where);
            else
                snprintf(dlabel, sizeof(dlabel), " \xe2\x86\x95 SYMBOL INFO ");
            draw_divider(a, out, a->cw, dlabel, a->focus == FOCUS_INFO);
        } else {
            draw_info(a, out, row - a->top_h - 1);
        }

        if (a->rw > 0) {
            sep_cell(out, a);
            if (a->view.kind == VIEW_CONTEXT &&
                a->focus == FOCUS_CONTEXT && a->rw >= 16) {
                /* Expanded graph paints this whole column below. */
                sgr_bg(out, a->theme.panel);
                draw_spaces(out, a->rw);
                sgr_reset(out);
            } else if (row < a->right_top_h) {
                draw_file_row(a, out, row);
            } else if (row == a->right_top_h) {
                if (a->view.title[0])
                    snprintf(dlabel, sizeof(dlabel), " \xe2\x86\x95 %s ",
                             a->view.title);
                else
                    snprintf(dlabel, sizeof(dlabel), " \xe2\x86\x95 CALL FLOW ");
                draw_divider(a, out, a->rw, dlabel,
                             a->focus == FOCUS_CONTEXT);
            } else if (a->view.kind == VIEW_CONTEXT && a->rw >= 16) {
                /* The graph renderer paints the whole pane below. */
                sgr_bg(out, a->theme.panel);
                draw_spaces(out, a->rw);
                sgr_reset(out);
            } else {
                draw_right_row(a, out, row - a->right_top_h - 1);
            }
        }
    }

    if (!project_relations_ready(&a->project) && a->rw >= 16 &&
        !(a->view.kind == VIEW_CONTEXT && a->context_need_relations)) {
        /* No call/var graph yet: keep the CALL FLOW pane explanatory even
         * before the user clicks a function. */
        draw_relations_missing(a, out, a->right_split_row + 1,
                              a->right_bottom_h);
    }

    if (a->view.kind == VIEW_CONTEXT && a->rw >= 16) {
        if (a->focus == FOCUS_CONTEXT) {
            /* Focused CALL FLOW uses the full right column, giving the
             * tree room for several caller levels. */
            char label[160];
            sgr_move(out, a->content_top,
                     a->lw + 1 + a->cw + 1);
            snprintf(label, sizeof(label), " \xe2\x86\x95 %s ",
                     a->view.title[0] ? a->view.title : "CALL FLOW");
            draw_divider(a, out, a->rw, label, true);
            draw_call_flow_pane(a, out, a->content_top + 1,
                                a->content_height - 1);
        } else {
            draw_call_flow_pane(a, out, a->right_split_row + 1,
                                a->right_bottom_h);
        }
    }

    /* A portable splitter handle: terminals do not allow us to change the
     * OS mouse cursor shape, so draw an arrow at the hovered splitter. */
    if (a->hover_mode >= 1 && a->hover_mode <= 4) {
        int hx = a->hover_x;
        int hy = a->hover_y;
        const char *arrow = "\xe2\x86\x95";
        if (a->hover_mode == 1)
            hx = a->lw;
        else if (a->hover_mode == 2)
            hx = a->lw + 1 + a->cw;
        else if (a->hover_mode == 3)
            hy = a->split_row;
        else
            hy = a->right_split_row;
        if (a->hover_mode <= 2)
            arrow = "\xe2\x86\x94";
        if (hy >= a->content_top &&
            hy < a->content_top + a->content_height && hx >= 0 &&
            hx < a->cols) {
            sgr_move(out, hy, hx);
            sgr_bg(out, a->theme.accent_bg);
            sgr_fg(out, a->theme.selection_fg);
            sgr_bold(out);
            str_puts(out, arrow);
            sgr_reset(out);
        }
    }
}

static void draw_too_small(App *a, Str *out)
{
    int row;
    char msg[128];
    for (row = 0; row < a->rows; row++) {
        sgr_move(out, row, 0);
        sgr_bg(out, a->theme.bg);
        sgr_fg(out, row == a->rows / 2 ? a->theme.fg : a->theme.dim);
        if (row == a->rows / 2) {
            snprintf(msg, sizeof(msg),
                     "Terminal too small: %dx%d (need at least 60x12)",
                     a->cols, a->rows);
            sgr_bold(out);
            draw_center(out, msg, a->cols);
        } else {
            draw_spaces(out, a->cols);
        }
        sgr_reset(out);
    }
    a->show_cursor = false;
}

void app_render(App *a)
{
    Str out;
    str_init(&out);
    g_colors = a->theme.colors > 0 ? a->theme.colors : 256;
    a->show_cursor = false;

    /* Begin a synchronized terminal update and hide the cursor while the
     * frame is being painted.  Without this, the real terminal cursor is
     * visible and jumps through every sgr_move() while the frame is written. */
    str_puts(&out, "\x1b[?2026h");
    str_puts(&out, "\x1b[?25l");

    if (a->too_small) {
        draw_too_small(a, &out);
    } else {
        sgr_move(&out, 0, 0);
        draw_title(a, &out);
        draw_header(a, &out);
        draw_content(a, &out);
        draw_hint(a, &out);
        draw_status(a, &out);
        if (a->prompt == PROMPT_FIND ||
            a->prompt == PROMPT_GREP ||
            a->prompt == PROMPT_SYMBOL) {
            draw_search_prompt(a, &out);
        } else if (a->prompt != PROMPT_NONE &&
                   a->prompt != PROMPT_PROJECT_NAME) {
            draw_prompt_modal(a, &out);
        }
        if (a->prompt == PROMPT_PROJECT_NAME)
            draw_project_name_prompt(a, &out);
        if (a->menu_active)
            draw_menu(a, &out);
        if (a->chooser_active)
            draw_chooser(a, &out);
        if (a->dir_active)
            draw_dir_picker(a, &out);
        if (a->context_menu_active)
            draw_context_menu(a, &out);
        if (a->relations_large_prompt)
            draw_large_project_prompt(a, &out);
        if (a->relation_building || a->relation_ready_prompt)
            draw_relation_modal(a, &out);
        if (a->help)
            draw_help(a, &out);
    }

    {
        bool cursor_visible = a->show_cursor && !a->help &&
                              !a->chooser_active && !a->menu_active &&
                              !a->dir_active && !a->relation_building &&
                              !a->relation_ready_prompt &&
                              !a->relations_large_prompt &&
                              !a->context_menu_active;
        if (cursor_visible)
            str_printf(&out, "\x1b[%d;%dH", a->cursor_y + 1,
                       a->cursor_x + 1);

        /* End synchronized update, then reveal the cursor at its cell. */
        str_puts(&out, "\x1b[?2026l");
        if (cursor_visible)
            str_puts(&out, "\x1b[?25h");
        else
            str_puts(&out, "\x1b[?25l");
    }

    term_write(out.data, out.len);
    str_free(&out);
}
