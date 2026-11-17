#include "si.h"
#include "app.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define si_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define si_mkdir(path) mkdir(path, 0755)
#endif

static void app_clear_selection(App *a);
static void symbol_selection_fix(App *a);
static void file_selection_fix(App *a);
static void app_menu_open_group(App *a, int group);
static void app_clear_highlight(App *a);
static void app_toggle_highlight(App *a);
static void app_chooser_open(App *a, const IntList *defs, const char *word);
static void app_menu_open(App *a);
static void app_prompt_begin(App *a, PromptKind kind, const char *initial);
static void app_project_picker_close(App *a);
static void app_request_quit(App *a);

typedef struct {
    const char *root;
} ProgressCtx;

static bool index_progress(void *ud, size_t files, size_t symbols,
                           const char *path);

static void app_schedule_relations(App *a)
{
    a->relations_pending = false;
    a->relations_large_prompt = false;
    a->relation_ready_prompt = false;

    if (!a->project.root || a->project.file_count == 0) {
        a->relations_large_skipped = false;
        return;
    }
    if (a->project.relations_ready) {
        a->relations_large_skipped = false;
        a->relation_edges = (size_t)project_call_edge_count(&a->project);
        a->relation_vars = (size_t)project_var_use_count(&a->project);
        a->relation_files = a->project.file_count;
        a->relation_symbols = a->project.sym_count;
        return;
    }
    if (a->project.file_count > SI_RELATION_AUTO_MAX_FILES ||
        a->project.scan_depth > SI_RELATION_AUTO_MAX_DEPTH) {
        /*
         * Large project: parsing symbols is already done, but building
         * the full call/variable graph would make startup feel frozen.
         * Ask the user instead of starting it silently.
         */
        a->relations_large_skipped = true;
        a->relations_large_prompt = true;
        return;
    }
    a->relations_large_skipped = false;
    a->relations_pending = true;
}

/* ------------------------------------------------------------------ */
/* Theme / layout                                                      */
/* ------------------------------------------------------------------ */

static Theme make_theme(bool light)
{
    Theme t;
    memset(&t, 0, sizeof(t));
    t.colors = 256;
    if (light) {
        t.name = "light";
        t.bg = 255;
        t.panel = 254;
        t.panel_alt = 253;
        t.current = 252;
        t.status_bg = 153;
        t.accent_bg = 25;
        t.border = 250;
        t.fg = 235;
        t.dim = 243;
        t.selection_fg = 16;
        t.syn[HL_NORMAL] = 235;
        t.syn[HL_COMMENT] = 245;
        t.syn[HL_STRING] = 130;
        t.syn[HL_NUMBER] = 166;
        t.syn[HL_KEYWORD] = 25;
        t.syn[HL_TYPE] = 28;
        t.syn[HL_PREPROC] = 91;
        t.syn[HL_FUNC] = 20;
        t.syn[HL_OP] = 240;
        t.syn[HL_GLOBAL] = 160;
        t.syn[HL_LOCAL] = 24;
    } else {
        t.name = "dark";
        t.bg = 234;
        t.panel = 235;
        t.panel_alt = 236;
        t.current = 237;
        t.status_bg = 24;
        t.accent_bg = 24;
        t.border = 240;
        t.fg = 252;
        t.dim = 244;
        t.selection_fg = 255;
        t.syn[HL_NORMAL] = 252;
        t.syn[HL_COMMENT] = 244;
        t.syn[HL_STRING] = 221;
        t.syn[HL_NUMBER] = 215;
        t.syn[HL_KEYWORD] = 81;
        t.syn[HL_TYPE] = 114;
        t.syn[HL_PREPROC] = 176;
        t.syn[HL_FUNC] = 75;
        t.syn[HL_OP] = 250;
        t.syn[HL_GLOBAL] = 196;
        t.syn[HL_LOCAL] = 117;
    }
    return t;
}

static int digits_for(int n)
{
    int d = 1;
    if (n < 0)
        n = 0;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

void app_layout(App *a)
{
    int available;
    int avail_w;
    int usable_h;

    if (a->force_rows > 0 && a->force_cols > 0) {
        a->rows = a->force_rows;
        a->cols = a->force_cols;
    } else {
        term_get_size(&a->rows, &a->cols);
    }
    if (a->left_pct <= 0)
        a->left_pct = 30;
    if (a->left_pct > 60)
        a->left_pct = 60;
    if (a->right_pct <= 0)
        a->right_pct = 30;
    if (a->right_pct > 60)
        a->right_pct = 60;
    if (a->center_top_pct <= 0)
        a->center_top_pct = 70;
    if (a->center_top_pct > 95)
        a->center_top_pct = 95;
    if (a->right_top_pct <= 0)
        a->right_top_pct = 70;
    if (a->right_top_pct > 95)
        a->right_top_pct = 95;

    a->too_small = a->rows < 12 || a->cols < 60;
    if (a->too_small) {
        a->content_top = 2;
        a->content_height = SI_MAX(1, a->rows - 4);
        a->lw = a->cols / 3;
        a->rw = 0;
        a->cw = SI_MAX(1, a->cols - a->lw - 1);
        a->top_h = a->content_height;
        a->bottom_h = 0;
        a->right_top_h = a->content_height;
        a->right_bottom_h = 0;
        a->split_row = a->content_top + a->top_h;
        a->right_split_row = a->split_row;
        a->ed_h = a->content_height;
        a->ed_w = SI_MAX(1, a->cw - 6);
        a->gutter_w = 3;
        return;
    }

    avail_w = a->cols - 2; /* two vertical separators */
    if (a->cols < 72) {
        a->rw = 0;
        a->lw = avail_w * a->left_pct / 100;
        if (a->lw < 18)
            a->lw = 18;
        if (a->lw > a->cols - 20)
            a->lw = a->cols - 20;
        a->cw = a->cols - a->lw - 1;
    } else {
        a->lw = avail_w * a->left_pct / 100;
        a->rw = avail_w * a->right_pct / 100;
        if (a->lw < 18)
            a->lw = 18;
        if (a->rw < 18)
            a->rw = 18;
        if (a->lw > avail_w - 38)
            a->lw = avail_w - 38;
        if (a->rw > avail_w - a->lw - 20)
            a->rw = avail_w - a->lw - 20;
        if (a->rw < 18)
            a->rw = 18;
        a->cw = avail_w - a->lw - a->rw;
        if (a->cw < 20) {
            a->lw = 18;
            a->rw = 18;
            a->cw = avail_w - 36;
        }
        if (a->cw < 18) {
            a->rw = 0;
            a->lw = SI_MIN(24, a->cols / 3);
            a->cw = a->cols - a->lw - 1;
        }
    }
    available = 0;
    {
        Buffer *b = app_buffer(a);
        if (b)
            available = (int)buf_line_count(b);
    }
    a->gutter_w = SI_MAX(3, digits_for(available));
    if (a->gutter_w > 6)
        a->gutter_w = 6;
    a->content_top = 2;
    a->content_height = SI_MAX(1, a->rows - 4);
    usable_h = a->content_height - 1; /* one divider row per column */
    if (usable_h < 4)
        usable_h = SI_MAX(1, a->content_height);
    a->top_h = (usable_h * a->center_top_pct + 50) / 100;
    if (a->top_h < 3)
        a->top_h = 3;
    if (a->top_h > usable_h - 2)
        a->top_h = SI_MAX(1, usable_h - 2);
    a->bottom_h = a->content_height - a->top_h - 1;
    if (a->bottom_h < 1) {
        a->bottom_h = SI_MAX(1, a->content_height - a->top_h);
        a->top_h = a->content_height - a->bottom_h;
    }
    a->right_top_h = (usable_h * a->right_top_pct + 50) / 100;
    if (a->right_top_h < 3)
        a->right_top_h = 3;
    if (a->right_top_h > usable_h - 2)
        a->right_top_h = SI_MAX(1, usable_h - 2);
    a->right_bottom_h = a->content_height - a->right_top_h - 1;
    if (a->right_bottom_h < 1) {
        a->right_bottom_h = SI_MAX(1, a->content_height - a->right_top_h);
        a->right_top_h = a->content_height - a->right_bottom_h;
    }
    a->split_row = a->content_top + a->top_h;
    a->right_split_row = a->content_top + a->right_top_h;
    a->ed_h = a->top_h;
    a->ed_w = SI_MAX(1, a->cw - a->gutter_w - 3);
}

void app_set_status(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Views                                                               */
/* ------------------------------------------------------------------ */

void app_view_clear(View *v)
{
    size_t i;
    for (i = 0; i < v->count; i++) {
        free(v->items[i].label);
        free(v->items[i].detail);
    }
    v->count = 0;
    v->kind = VIEW_NONE;
    v->title[0] = '\0';
}

static int cmp_symbol_ptr_name(const void *pa, const void *pb)
{
    Symbol *const *sa = pa;
    Symbol *const *sb = pb;
    int c = strcmp((*sa)->name, (*sb)->name);
    if (c)
        return c;
    return (int)(*sa)->kind - (int)(*sb)->kind;
}

void app_rebuild_globals(App *a)
{
    size_t i;
    strlist_free(&a->globals);
    strlist_free(&a->types);
    strlist_free(&a->macros);
    strlist_init(&a->globals);
    strlist_init(&a->types);
    strlist_init(&a->macros);
    for (i = 0; i < a->project.sym_count; i++) {
        Symbol *s = &a->project.syms[i];
        switch (s->kind) {
        case SYM_VARIABLE:
        case SYM_ENUMVAL:
            strlist_push(&a->globals, s->name);
            break;
        case SYM_STRUCT:
        case SYM_UNION:
        case SYM_ENUM:
        case SYM_CLASS:
        case SYM_TYPEDEF:
            strlist_push(&a->types, s->name);
            break;
        case SYM_MACRO:
            strlist_push(&a->macros, s->name);
            break;
        default:
            break;
        }
    }
    strlist_sort(&a->globals);
    strlist_sort(&a->types);
    strlist_sort(&a->macros);

    free(a->sym_name_index);
    a->sym_name_index = NULL;
    a->sym_name_count = 0;
    if (a->project.sym_count > 0) {
        a->sym_name_index =
            xmalloc(a->project.sym_count * sizeof(*a->sym_name_index));
        for (i = 0; i < a->project.sym_count; i++)
            a->sym_name_index[i] = &a->project.syms[i];
        a->sym_name_count = a->project.sym_count;
        qsort(a->sym_name_index, a->sym_name_count,
              sizeof(*a->sym_name_index), cmp_symbol_ptr_name);
    }
    app_completion_reset(a);
}

static ViewItem *view_add(View *v, const char *label, const char *detail,
                          int file_index, int line, int col, int sym_index)
{
    ViewItem *it;
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->items = xrealloc(v->items, v->cap * sizeof(*v->items));
    }
    it = &v->items[v->count++];
    it->label = xstrdup(label ? label : "");
    it->detail = xstrdup(detail ? detail : "");
    it->file_index = file_index;
    it->line = line;
    it->col = col;
    it->sym_index = sym_index;
    it->depth = 0;
    it->is_header = false;
    return it;
}

static void view_add_header(View *v, const char *text)
{
    ViewItem *it = view_add(v, text, "", -1, 0, 0, -1);
    it->is_header = true;
}

void app_refresh_outline(App *a)
{
    Buffer *b = app_buffer(a);
    IntList ids;
    size_t i;
    int cursor_line;
    int best = 0;

    app_view_clear(&a->outline);
    a->view_stale = false;
    if (!b)
        return;
    a->outline.kind = VIEW_OUTLINE;
    snprintf(a->outline.title, sizeof(a->outline.title), "FILE OUTLINE  %s",
             b->name);
    intlist_init(&ids);
    {
        int fi = app_active_file_index(a);
        if (fi >= 0)
            project_file_symbols(&a->project, fi, &ids);
    }
    for (i = 0; i < ids.count; i++) {
        Symbol *s = &a->project.syms[ids.items[i]];
        char label[512];
        char detail[32];
        snprintf(label, sizeof(label), "%-6s %s",
                 symbol_kind_short(s->kind), s->name);
        snprintf(detail, sizeof(detail), "L%d", s->line);
        view_add(&a->outline, label, detail, s->file_index, s->line, s->col,
                 ids.items[i]);
    }
    intlist_free(&ids);

    cursor_line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
    for (i = 0; i < a->outline.count; i++) {
        if (a->outline.items[i].line <= cursor_line)
            best = (int)i;
    }
    a->outline_sel = best;
    if (a->outline_sel < a->outline_scroll)
        a->outline_scroll = a->outline_sel;
    if (a->outline_sel >= a->outline_scroll + a->content_height)
        a->outline_scroll = a->outline_sel - a->content_height + 1;
}

static void view_symbols(App *a, const IntList *ids, ViewKind kind,
                         const char *title)
{
    size_t i;
    app_view_clear(&a->view);
    a->view.kind = kind;
    snprintf(a->view.title, sizeof(a->view.title), "%s", title);
    for (i = 0; i < ids->count; i++) {
        Symbol *s = &a->project.syms[ids->items[i]];
        char detail[256];
        int fi = s->file_index;
        if (fi >= 0 && (size_t)fi < a->project.file_count &&
            fi != app_active_file_index(a)) {
            snprintf(detail, sizeof(detail), "%s:%d",
                     a->project.files[fi].rel, s->line);
        } else {
            snprintf(detail, sizeof(detail), "L%d", s->line);
        }
        view_add(&a->view, s->signature, detail, fi, s->line, s->col,
                 ids->items[i]);
    }
    a->sym_sel = 0;
    a->sym_scroll = 0;
}

static void view_locations(App *a, const LocationList *locs, ViewKind kind,
                           const char *title)
{
    size_t i;
    app_view_clear(&a->view);
    a->view.kind = kind;
    snprintf(a->view.title, sizeof(a->view.title), "%s", title);
    for (i = 0; i < locs->count; i++) {
        const Location *l = &locs->items[i];
        char detail[256];
        snprintf(detail, sizeof(detail), "%s:%d",
                 a->project.files[l->file_index].rel, l->line);
        view_add(&a->view, l->preview, detail, l->file_index, l->line, l->col,
                 -1);
    }
    a->sym_sel = 0;
    a->sym_scroll = 0;
}

/* ------------------------------------------------------------------ */
/* Buffers / navigation                                                */
/* ------------------------------------------------------------------ */

Buffer *app_buffer(App *a)
{
    if (a->active < 0 || a->active >= a->nbuf)
        return NULL;
    return a->bufs[a->active];
}

static int find_buffer(App *a, const char *path)
{
    int i;
    for (i = 0; i < a->nbuf; i++) {
        if (a->bufs[i]->path && strcmp(a->bufs[i]->path, path) == 0)
            return i;
    }
    return -1;
}

int app_active_file_index(App *a)
{
    Buffer *b = app_buffer(a);
    if (!b || !b->path)
        return -1;
    /*
     * The active file is usually opened through app_open_file_index(),
     * which already knows the index.  Avoid realpath()+linear scan on
     * every cursor movement in projects with tens of thousands of files.
     */
    if (a->active_project_file >= 0 &&
        (size_t)a->active_project_file < a->project.file_count)
        return a->active_project_file;
    return project_file_index_of_path(&a->project, b->path);
}

static int open_buffer_path(App *a, const char *path)
{
    int idx = find_buffer(a, path);
    Buffer *b;
    char *err = NULL;
    if (idx >= 0)
        return idx;
    b = buf_open(path, &err);
    if (!b) {
        app_set_status(a, "Open failed: %s", err ? err : "unknown error");
        free(err);
        return -1;
    }
    if (a->nbuf == a->capbuf) {
        a->capbuf = a->capbuf ? a->capbuf * 2 : 8;
        a->bufs = xrealloc(a->bufs, (size_t)a->capbuf * sizeof(*a->bufs));
    }
    a->bufs[a->nbuf] = b;
    return a->nbuf++;
}

static void set_cursor_line_col(App *a, Buffer *b, int line, int col)
{
    int li = line > 0 ? line - 1 : 0;
    size_t start;
    size_t end;
    size_t off;
    int count = (int)buf_line_count(b);
    if (li >= count)
        li = count - 1;
    if (li < 0)
        li = 0;
    start = buf_line_start(b, li);
    end = buf_line_content_end(b, li);
    if (col < 0)
        col = 0;
    off = start + (size_t)col;
    if (off > end)
        off = end;
    buf_set_cursor(b, off);
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    b->scroll_line = SI_MAX(0, li - a->ed_h / 3);
    b->scroll_col = 0;
}

void app_open_file_index(App *a, int fi, int line, int col, bool record)
{
    app_completion_close(a);
    Buffer *b;
    int idx;
    const char *path;

    if (fi < 0 || (size_t)fi >= a->project.file_count) {
        app_set_status(a, "No such project file");
        return;
    }
    app_clear_selection(a);
    path = a->project.files[fi].path;
    idx = open_buffer_path(a, path);
    if (idx < 0)
        return;
    if (record)
        app_push_location(a);
    a->active = idx;
    a->active_project_file = fi;
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->pending_info_sym = -1;
    a->pending_info_word[0] = '\0';
    app_clear_highlight(a);
    b = a->bufs[idx];
    set_cursor_line_col(a, b, line, col);
    a->file_sel = fi;
    a->find_pos = 0;
    a->find_len = 0;
    app_layout(a);
    app_refresh_outline(a);
    if (!a->pin_context)
        app_view_clear(&a->view);
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->locals_buf = -1;
    a->info_word[0] = '\0';
    a->info_line = -1;
    app_update_info(a);
    a->focus = FOCUS_EDITOR;
    {
        const char *base = path_basename(b->path);
        app_set_status(a, "%s:%d", base, line > 0 ? line : 1);
    }
    app_layout(a);
    app_ensure_visible(a);
}

int app_open_path(App *a, const char *path, int line, bool record)
{
    app_completion_close(a);
    char *abs;
    int fi;
    int idx;
    if (!path || !*path)
        return -1;
    fi = project_find_file_by_rel(&a->project, path);
    if (fi >= 0) {
        app_open_file_index(a, fi, line, 0, record);
        return fi;
    }
    if (path[0] == '/'
#if defined(_WIN32)
        || (isalpha((unsigned char)path[0]) && path[1] == ':')
#endif
    ) {
        abs = xstrdup(path);
    } else {
        abs = path_join(a->project.root ? a->project.root : ".", path);
    }
    if (!path_is_file(abs)) {
        app_set_status(a, "File not found: %s", path);
        free(abs);
        return -1;
    }
    idx = open_buffer_path(a, abs);
    free(abs);
    if (idx < 0)
        return -1;
    if (record)
        app_push_location(a);
    app_clear_selection(a);
    a->active = idx;
    a->active_project_file = -1;
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->pending_info_sym = -1;
    a->pending_info_word[0] = '\0';
    app_clear_highlight(a);
    set_cursor_line_col(a, a->bufs[idx], line, 0);
    app_layout(a);
    app_refresh_outline(a);
    app_view_clear(&a->view);
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->locals_buf = -1;
    a->info_word[0] = '\0';
    a->info_line = -1;
    app_update_info(a);
    a->focus = FOCUS_EDITOR;
    app_ensure_visible(a);
    return -1;
}

void app_goto_location(App *a, int fi, int line, int col, bool record)
{
    bool keep_context = a->view.kind == VIEW_CONTEXT;
    app_open_file_index(a, fi, line, col, record);
    if (keep_context)
        app_show_context(a);
}

static void app_jump_call_site(App *a, int fi, int line, int col)
{
    a->pin_context = true;
    app_open_file_index(a, fi, line, col, true);
    a->pin_context = false;
}

static void nav_free(NavLoc *loc)
{
    free(loc->path);
    loc->path = NULL;
}

void app_push_location(App *a)
{
    Buffer *b = app_buffer(a);
    NavLoc loc;
    size_t i;
    if (!b || !b->path)
        return;
    loc.path = xstrdup(b->path);
    loc.cursor = b->cursor;
    loc.scroll_line = b->scroll_line;
    loc.scroll_col = b->scroll_col;
    if (a->nback == a->cback) {
        a->cback = a->cback ? a->cback * 2 : 16;
        a->back = xrealloc(a->back, a->cback * sizeof(*a->back));
    }
    a->back[a->nback++] = loc;
    for (i = 0; i < a->nfwd; i++)
        nav_free(&a->fwd[i]);
    a->nfwd = 0;
    if (a->nback > 100) {
        nav_free(&a->back[0]);
        memmove(a->back, a->back + 1,
                (a->nback - 1) * sizeof(*a->back));
        a->nback--;
    }
}

static void goto_nav(App *a, NavLoc *loc)
{
    int idx = open_buffer_path(a, loc->path);
    Buffer *b;
    if (idx < 0)
        return;
    a->active = idx;
    b = a->bufs[idx];
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->pending_info_sym = -1;
    a->pending_info_word[0] = '\0';
    a->active_project_file =
        project_file_index_of_path(&a->project, b->path);
    buf_set_cursor(b, (size_t)loc->cursor);
    b->scroll_line = loc->scroll_line;
    b->scroll_col = loc->scroll_col;
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    a->focus = FOCUS_EDITOR;
    app_layout(a);
    app_ensure_visible(a);
}

void app_go_back(App *a)
{
    NavLoc loc;
    Buffer *b = app_buffer(a);
    if (a->nback == 0) {
        app_set_status(a, "No previous location");
        return;
    }
    if (b && b->path) {
        NavLoc cur;
        cur.path = xstrdup(b->path);
        cur.cursor = b->cursor;
        cur.scroll_line = b->scroll_line;
        cur.scroll_col = b->scroll_col;
        if (a->nfwd == a->cfwd) {
            a->cfwd = a->cfwd ? a->cfwd * 2 : 16;
            a->fwd = xrealloc(a->fwd, a->cfwd * sizeof(*a->fwd));
        }
        a->fwd[a->nfwd++] = cur;
    }
    loc = a->back[--a->nback];
    goto_nav(a, &loc);
    app_set_status(a, "Back: %s", path_basename(loc.path));
    nav_free(&loc);
}

void app_go_forward(App *a)
{
    NavLoc loc;
    Buffer *b = app_buffer(a);
    if (a->nfwd == 0) {
        if (a->home_valid &&
            (!b || !b->path || strcmp(b->path, a->home.path) != 0 ||
             b->cursor != a->home.cursor)) {
            if (b && b->path) {
                NavLoc cur;
                cur.path = xstrdup(b->path);
                cur.cursor = b->cursor;
                cur.scroll_line = b->scroll_line;
                cur.scroll_col = b->scroll_col;
                if (a->nback == a->cback) {
                    a->cback = a->cback ? a->cback * 2 : 16;
                    a->back = xrealloc(a->back,
                                       a->cback * sizeof(*a->back));
                }
                a->back[a->nback++] = cur;
            }
            goto_nav(a, &a->home);
            app_set_status(a, "Forward: first location (%s)",
                           path_basename(a->home.path));
            return;
        }
        app_set_status(a, "No next location");
        return;
    }
    if (b && b->path) {
        NavLoc cur;
        cur.path = xstrdup(b->path);
        cur.cursor = b->cursor;
        cur.scroll_line = b->scroll_line;
        cur.scroll_col = b->scroll_col;
        if (a->nback == a->cback) {
            a->cback = a->cback ? a->cback * 2 : 16;
            a->back = xrealloc(a->back, a->cback * sizeof(*a->back));
        }
        a->back[a->nback++] = cur;
    }
    loc = a->fwd[--a->nfwd];
    goto_nav(a, &loc);
    app_set_status(a, "Forward: %s", path_basename(loc.path));
    nav_free(&loc);
}

void app_ensure_visible(App *a)
{
    Buffer *b = app_buffer(a);
    int line;
    int col;
    int h;
    if (!b)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor);
    col = buf_visual_col(b, (size_t)b->cursor);
    h = SI_MAX(1, a->ed_h);
    if (line < b->scroll_line)
        b->scroll_line = line;
    if (line >= b->scroll_line + h)
        b->scroll_line = line - h + 1;
    if (b->scroll_line < 0)
        b->scroll_line = 0;
    if (a->ed_w > 4) {
        if (col < b->scroll_col)
            b->scroll_col = SI_MAX(0, col - 4);
        if (col >= b->scroll_col + a->ed_w)
            b->scroll_col = col - a->ed_w + 5;
    } else {
        b->scroll_col = 0;
    }
    if (b->scroll_col < 0)
        b->scroll_col = 0;
}

/* ------------------------------------------------------------------ */
/* Editing helpers                                                     */
/* ------------------------------------------------------------------ */

static void clear_find(App *a)
{
    a->find_pos = 0;
    a->find_len = 0;
}

static void app_clear_selection(App *a)
{
    a->selecting = false;
    a->has_selection = false;
    a->sel_buf = -1;
}

static void insert_at_cursor(App *a, const char *text, size_t len)
{
    Buffer *b = app_buffer(a);
    size_t pos;
    if (!b || len == 0)
        return;
    pos = (size_t)b->cursor;
    buf_insert_at(b, pos, text, len);
    b->cursor = (int)(pos + len);
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    a->find_pos = 0;
    a->find_len = 0;
    a->view_stale = true;
    app_ensure_visible(a);
}

static void delete_at_cursor(App *a, size_t pos, size_t len)
{
    Buffer *b = app_buffer(a);
    if (!b || len == 0)
        return;
    buf_delete_range(b, pos, len);
    b->cursor = (int)pos;
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    a->find_pos = 0;
    a->find_len = 0;
    a->view_stale = true;
    app_ensure_visible(a);
}

static size_t next_utf8(Buffer *b, size_t pos)
{
    uint32_t cp;
    int n;
    if (pos >= b->len)
        return b->len;
    n = utf8_decode(b->data + pos, b->len - pos, &cp);
    return pos + (n > 0 ? (size_t)n : 1);
}

static bool word_char(unsigned char c)
{
    return isalnum(c) || c == '_';
}

static void move_horizontal(App *a, int dir)
{
    Buffer *b = app_buffer(a);
    int line;
    size_t start;
    size_t end;
    if (!b)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor);
    start = buf_line_start(b, line);
    end = buf_line_content_end(b, line);
    if (dir < 0) {
        if ((size_t)b->cursor > start)
            b->cursor = (int)utf8_prev(b->data, (size_t)b->cursor);
        else if (line > 0)
            b->cursor = (int)buf_line_content_end(b, line - 1);
    } else {
        if ((size_t)b->cursor < end)
            b->cursor = (int)next_utf8(b, (size_t)b->cursor);
        else if ((size_t)(line + 1) < buf_line_count(b))
            b->cursor = (int)buf_line_start(b, line + 1);
        else
            b->cursor = (int)b->len;
    }
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    clear_find(a);
    app_ensure_visible(a);
}

static void move_vertical(App *a, int delta)
{
    Buffer *b = app_buffer(a);
    int line;
    int target;
    int count;
    int col;
    if (!b)
        return;
    count = (int)buf_line_count(b);
    line = buf_line_of_offset(b, (size_t)b->cursor);
    target = line + delta;
    if (target < 0)
        target = 0;
    if (target >= count)
        target = count - 1;
    col = b->goal_col;
    if (col < 0)
        col = buf_visual_col(b, (size_t)b->cursor);
    b->cursor = (int)buf_offset_at_visual(b, target, col);
    clear_find(a);
    app_ensure_visible(a);
}

static void word_forward(App *a)
{
    Buffer *b = app_buffer(a);
    if (!b)
        return;
    while ((size_t)b->cursor < b->len &&
           !word_char((unsigned char)b->data[b->cursor]))
        b->cursor = (int)next_utf8(b, (size_t)b->cursor);
    while ((size_t)b->cursor < b->len &&
           word_char((unsigned char)b->data[b->cursor]))
        b->cursor = (int)next_utf8(b, (size_t)b->cursor);
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    clear_find(a);
    app_ensure_visible(a);
}

static void word_backward(App *a)
{
    Buffer *b = app_buffer(a);
    size_t p;
    if (!b)
        return;
    p = (size_t)b->cursor;
    while (p > 0 && !word_char((unsigned char)b->data[p - 1]))
        p = utf8_prev(b->data, p);
    while (p > 0 && word_char((unsigned char)b->data[p - 1]))
        p = utf8_prev(b->data, p);
    b->cursor = (int)p;
    b->goal_col = buf_visual_col(b, p);
    clear_find(a);
    app_ensure_visible(a);
}

static void editor_home(App *a)
{
    Buffer *b = app_buffer(a);
    int line;
    size_t start;
    size_t end;
    size_t first;
    if (!b)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor);
    start = buf_line_start(b, line);
    end = buf_line_content_end(b, line);
    first = start;
    while (first < end && (b->data[first] == ' ' || b->data[first] == '\t'))
        first++;
    if ((size_t)b->cursor == first)
        b->cursor = (int)start;
    else
        b->cursor = (int)first;
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    clear_find(a);
    app_ensure_visible(a);
}

static void editor_end(App *a)
{
    Buffer *b = app_buffer(a);
    int line;
    if (!b)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor);
    b->cursor = (int)buf_line_content_end(b, line);
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    clear_find(a);
    app_ensure_visible(a);
}

static void editor_enter(App *a)
{
    Buffer *b = app_buffer(a);
    Str ins;
    int line;
    size_t start;
    size_t indent;
    size_t i;
    bool after_brace = false;
    if (!b)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor);
    start = buf_line_start(b, line);
    indent = buf_leading_indent(b, line);
    if (indent > (size_t)b->cursor - start)
        indent = (size_t)b->cursor - start;
    i = (size_t)b->cursor;
    while (i > start) {
        char c = b->data[i - 1];
        if (c != ' ' && c != '\t') {
            after_brace = c == '{';
            break;
        }
        i--;
    }
    str_init(&ins);
    str_putc(&ins, '\n');
    if (indent)
        str_append(&ins, b->data + start, indent);
    if (after_brace) {
        for (i = 0; i < SI_TAB_WIDTH; i++)
            str_putc(&ins, ' ');
    }
    buf_undo_break(b);
    insert_at_cursor(a, ins.data, ins.len);
    buf_undo_break(b);
    str_free(&ins);
}

static void editor_tab(App *a)
{
    Buffer *b = app_buffer(a);
    int col;
    int n;
    char spaces[SI_TAB_WIDTH + 1];
    int i;
    if (!b)
        return;
    col = buf_visual_col(b, (size_t)b->cursor);
    n = SI_TAB_WIDTH - (col % SI_TAB_WIDTH);
    for (i = 0; i < n; i++)
        spaces[i] = ' ';
    insert_at_cursor(a, spaces, (size_t)n);
}

static void editor_backspace(App *a)
{
    Buffer *b = app_buffer(a);
    size_t start;
    if (!b || b->cursor <= 0)
        return;
    start = utf8_prev(b->data, (size_t)b->cursor);
    if (b->data[start] == '\n' && start > 0 && b->data[start - 1] == '\r')
        start--;
    buf_undo_break(b);
    delete_at_cursor(a, start, (size_t)b->cursor - start);
    buf_undo_break(b);
}

static void editor_delete(App *a)
{
    Buffer *b = app_buffer(a);
    size_t end;
    if (!b || (size_t)b->cursor >= b->len)
        return;
    end = next_utf8(b, (size_t)b->cursor);
    if (b->data[b->cursor] == '\r' && end < b->len &&
        b->data[end] == '\n')
        end++;
    buf_undo_break(b);
    delete_at_cursor(a, (size_t)b->cursor, end - (size_t)b->cursor);
    buf_undo_break(b);
}

static void editor_kill_line(App *a, bool to_end)
{
    Buffer *b = app_buffer(a);
    int line;
    size_t start;
    size_t end;
    if (!b)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor);
    start = buf_line_start(b, line);
    end = buf_line_content_end(b, line);
    if (to_end) {
        if ((size_t)b->cursor < end)
            delete_at_cursor(a, (size_t)b->cursor,
                             end - (size_t)b->cursor);
    } else {
        if ((size_t)b->cursor > start)
            delete_at_cursor(a, start, (size_t)b->cursor - start);
    }
}

static bool save_active(App *a)
{
    Buffer *b = app_buffer(a);
    char *err = NULL;
    if (!b)
        return false;
    if (!buf_is_dirty(b)) {
        app_set_status(a, "%s is already saved", b->name);
        return true;
    }
    if (!buf_save(b, &err)) {
        app_set_status(a, "Save failed: %s", err ? err : "unknown error");
        free(err);
        return false;
    }
    a->view_stale = true;
    app_set_status(a, "Saved %s (%lu bytes)", b->name,
                   (unsigned long)b->len);
    return true;
}

static bool save_all(App *a)
{
    int i;
    bool ok = true;
    for (i = 0; i < a->nbuf; i++) {
        if (buf_is_dirty(a->bufs[i])) {
            char *err = NULL;
            if (!buf_save(a->bufs[i], &err)) {
                app_set_status(a, "Save failed for %s: %s",
                               a->bufs[i]->name,
                               err ? err : "unknown error");
                free(err);
                ok = false;
            }
        }
    }
    return ok;
}

static bool any_dirty(App *a)
{
    int i;
    for (i = 0; i < a->nbuf; i++) {
        if (buf_is_dirty(a->bufs[i]))
            return true;
    }
    return false;
}

static void selection_bounds(App *a, int *lo, int *hi)
{
    int x = a->sel_anchor;
    int y = a->sel_cursor;
    if (x <= y) {
        *lo = x;
        *hi = y;
    } else {
        *lo = y;
        *hi = x;
    }
}

static char *base64_encode(const unsigned char *data, size_t len,
                           size_t *out_len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 4 * ((len + 2) / 3);
    char *out = xmalloc(n + 1);
    size_t i;
    size_t j = 0;

    for (i = 0; i + 2 < len; i += 3) {
        unsigned int v = ((unsigned int)data[i] << 16) |
                         ((unsigned int)data[i + 1] << 8) |
                         (unsigned int)data[i + 2];
        out[j++] = table[(v >> 18) & 63];
        out[j++] = table[(v >> 12) & 63];
        out[j++] = table[(v >> 6) & 63];
        out[j++] = table[v & 63];
    }
    if (i < len) {
        unsigned int v = (unsigned int)data[i] << 16;
        if (i + 1 < len)
            v |= (unsigned int)data[i + 1] << 8;
        out[j++] = table[(v >> 18) & 63];
        out[j++] = table[(v >> 12) & 63];
        out[j++] = i + 1 < len ? table[(v >> 6) & 63] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

static void copy_selection(App *a)
{
    Buffer *b = app_buffer(a);
    int lo;
    int hi;
    size_t len;

    if (!b || !a->has_selection || a->sel_buf != a->active) {
        app_set_status(a, "No selection - drag with the mouse first");
        return;
    }
    selection_bounds(a, &lo, &hi);
    if (lo < 0)
        lo = 0;
    if (hi > (int)b->len)
        hi = (int)b->len;
    len = (size_t)(hi - lo);
    if (len == 0) {
        app_set_status(a, "Empty selection");
        return;
    }
    free(a->clipboard);
    a->clipboard = xmalloc(len + 1);
    memcpy(a->clipboard, b->data + lo, len);
    a->clipboard[len] = '\0';
    a->clipboard_len = len;

    if (len <= 100000 && term_is_tty()) {
        size_t enc_len = 0;
        char *enc = base64_encode((const unsigned char *)a->clipboard, len,
                                  &enc_len);
        Str seq;
        str_init(&seq);
        str_puts(&seq, "\x1b]52;c;");
        str_append(&seq, enc, enc_len);
        str_putc(&seq, '\a');
        term_write(seq.data, seq.len);
        str_free(&seq);
        free(enc);
    }
    app_set_status(a, "Copied %lu bytes (Ctrl+V pastes; OSC52 sent)",
                   (unsigned long)len);
}

static void paste_clipboard(App *a)
{
    Buffer *b = app_buffer(a);
    if (!b) {
        app_set_status(a, "No active file");
        return;
    }
    if (!a->clipboard || a->clipboard_len == 0) {
        app_set_status(a, "Clipboard is empty (select text, Ctrl+C)");
        return;
    }
    app_clear_selection(a);
    buf_undo_break(b);
    insert_at_cursor(a, a->clipboard, a->clipboard_len);
    buf_undo_break(b);
    app_set_status(a, "Pasted %lu bytes", (unsigned long)a->clipboard_len);
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static char *word_at_offset(App *a, size_t offset)
{
    Buffer *b = app_buffer(a);
    size_t start;
    size_t len;
    if (!b || offset > b->len)
        return NULL;
    if (!buf_word_at(b, offset, &start, &len))
        return NULL;
    return xstrndup(b->data + start, len);
}

static char *word_at_cursor(App *a)
{
    Buffer *b = app_buffer(a);
    if (!b)
        return NULL;
    return word_at_offset(a, (size_t)b->cursor);
}

static bool cursor_is_member_access(Buffer *b)
{
    size_t start;
    size_t len;
    size_t p;
    if (!b || !buf_word_at(b, (size_t)b->cursor, &start, &len))
        return false;
    p = start;
    while (p > 0 && (b->data[p - 1] == ' ' || b->data[p - 1] == '\t'))
        p--;
    if (p > 0 && b->data[p - 1] == '.')
        return true;
    if (p >= 2 && b->data[p - 1] == '>' && b->data[p - 2] == '-')
        return true;
    return false;
}

static int enclosing_function_for_line(App *a, int line)
{
    int fi = app_active_file_index(a);
    int i;
    int best = -1;
    int best_span = 0;
    if (fi < 0 || (size_t)fi >= a->project.file_count)
        return -1;
    for (i = 0; i < a->project.files[fi].sym_count; i++) {
        int si = a->project.files[fi].sym_start + i;
        Symbol *s = &a->project.syms[si];
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

static bool control_word(const char *s)
{
    static const char *const words[] = {
        "if", "for", "while", "switch", "return", "case", "do",
        "else", "sizeof", "catch", "throw", "new", "delete"
    };
    size_t i;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        if (strcmp(s, words[i]) == 0)
            return true;
    }
    return false;
}

static bool type_prefix_has_word(const char *s)
{
    while (*s) {
        if (isalpha((unsigned char)*s) || *s == '_')
            return true;
        s++;
    }
    return false;
}

static bool type_suffix_word(const char *s, size_t len)
{
    static const char *const words[] = {
        "const", "volatile", "restrict", "_Atomic", "__restrict",
        "__restrict__"
    };
    size_t i;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        if (strlen(words[i]) == len && memcmp(words[i], s, len) == 0)
            return true;
    }
    return false;
}

/*
 * Comma-continued declarations such as `int a, b;` put no type directly
 * before `b`.  Recover the base type from the first declarator, then add
 * only the pointer/qualifier text that belongs to this declarator.
 */
static bool type_prefix_from_base(const char *line, size_t len,
                                  size_t word_pos, size_t seg_start,
                                  char *out, size_t out_size)
{
    size_t stmt_start = 0;
    size_t i;
    size_t cut;
    size_t first_decl_end = word_pos;
    size_t name_end;
    size_t name_start;
    size_t base_end;
    size_t suffix_start = seg_start;
    size_t suffix_end = word_pos;
    size_t used;
    int paren = 0;
    int brack = 0;
    int brace = 0;
    char quote = '\0';

    if (word_pos > len)
        return false;
    for (i = 0; i < word_pos; i++) {
        if (line[i] == ';' || line[i] == '{' || line[i] == '}' ||
            line[i] == '(')
            stmt_start = i + 1;
    }

    for (i = stmt_start; i < word_pos; i++) {
        char c = line[i];
        if (quote) {
            if (c == '\\' && i + 1 < word_pos) {
                i++;
                continue;
            }
            if (c == quote)
                quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '/' && i + 1 < word_pos && line[i + 1] == '/')
            return false;
        if (c == '(') {
            paren++;
        } else if (c == ')' && paren > 0) {
            paren--;
        } else if (c == '[') {
            brack++;
        } else if (c == ']' && brack > 0) {
            brack--;
        } else if (c == '{') {
            brace++;
        } else if (c == '}' && brace > 0) {
            brace--;
        } else if (c == ',' && paren == 0 && brack == 0 && brace == 0) {
            first_decl_end = i;
            break;
        }
    }
    if (first_decl_end >= word_pos)
        return false;

    cut = first_decl_end;
    paren = brack = brace = 0;
    quote = '\0';
    for (i = stmt_start; i < first_decl_end; i++) {
        char c = line[i];
        if (quote) {
            if (c == '\\' && i + 1 < first_decl_end) {
                i++;
                continue;
            }
            if (c == quote)
                quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '(') {
            paren++;
        } else if (c == ')' && paren > 0) {
            paren--;
        } else if (c == '[') {
            brack++;
        } else if (c == ']' && brack > 0) {
            brack--;
        } else if (c == '{') {
            brace++;
        } else if (c == '}' && brace > 0) {
            brace--;
        } else if (paren == 0 && brack == 0 && brace == 0 &&
                   (c == '=' || c == '[' || c == ':')) {
            cut = i;
            break;
        }
    }

    name_end = cut;
    while (name_end > stmt_start &&
           (line[name_end - 1] == ' ' || line[name_end - 1] == '\t'))
        name_end--;
    name_start = name_end;
    while (name_start > stmt_start &&
           (isalnum((unsigned char)line[name_start - 1]) ||
            line[name_start - 1] == '_'))
        name_start--;
    if (name_start == name_end)
        return false;

    base_end = name_start;
    for (;;) {
        while (base_end > stmt_start &&
               (line[base_end - 1] == ' ' || line[base_end - 1] == '\t'))
            base_end--;
        if (base_end > stmt_start &&
            (line[base_end - 1] == '*' || line[base_end - 1] == '&')) {
            base_end--;
            continue;
        }
        {
            size_t word_start = base_end;
            while (word_start > stmt_start &&
                   (isalnum((unsigned char)line[word_start - 1]) ||
                    line[word_start - 1] == '_'))
                word_start--;
            if (word_start < base_end &&
                type_suffix_word(line + word_start, base_end - word_start)) {
                base_end = word_start;
                continue;
            }
        }
        break;
    }
    while (base_end > stmt_start &&
           (line[base_end - 1] == ' ' || line[base_end - 1] == '\t'))
        base_end--;
    if (base_end <= stmt_start)
        return false;

    while (suffix_start < suffix_end &&
           (line[suffix_start] == ' ' || line[suffix_start] == '\t'))
        suffix_start++;
    while (suffix_end > suffix_start &&
           (line[suffix_end - 1] == ' ' || line[suffix_end - 1] == '\t'))
        suffix_end--;

    used = base_end - stmt_start;
    if (used >= out_size)
        used = out_size - 1;
    memcpy(out, line + stmt_start, used);
    out[used] = '\0';
    if (suffix_start < suffix_end) {
        size_t n = suffix_end - suffix_start;
        if (used + 1 < out_size) {
            out[used++] = ' ';
            if (n > out_size - 1 - used)
                n = out_size - 1 - used;
            memcpy(out + used, line + suffix_start, n);
            used += n;
            out[used] = '\0';
        }
    }
    return true;
}

static bool type_prefix(const char *line, size_t len, size_t word_pos,
                        char *out, size_t out_size)
{
    size_t start = 0;
    size_t i;
    size_t end;
    if (word_pos > len)
        return false;
    for (i = 0; i < word_pos; i++) {
        if (line[i] == ';' || line[i] == '{' || line[i] == '}' ||
            line[i] == '(' || line[i] == ',')
            start = i + 1;
    }
    while (start < word_pos &&
           (line[start] == ' ' || line[start] == '\t'))
        start++;
    end = word_pos;
    while (end > start &&
           (line[end - 1] == ' ' || line[end - 1] == '\t'))
        end--;
    out[0] = '\0';
    if (end > start) {
        if (end - start >= out_size)
            end = start + out_size - 1;
        memcpy(out, line + start, end - start);
        out[end - start] = '\0';
    }
    if (!type_prefix_has_word(out)) {
        char base[256];
        if (!type_prefix_from_base(line, len, word_pos, start, base,
                                   sizeof(base)))
            return false;
        snprintf(out, out_size, "%s", base);
    }
    if (strchr(out, '=') || strchr(out, '.') || strchr(out, '[') ||
        strstr(out, "->") || strpbrk(out, "+/%!?|\""))
        return false;
    return !control_word(out);
}

static bool find_local_decl(App *a, Buffer *b, const char *word, int line,
                            char *type, size_t type_size, int *decl_line,
                            int *decl_col)
{
    int fn = enclosing_function_for_line(a, line);
    int start_line;
    int ln;
    size_t wlen = strlen(word);

    if (fn < 0)
        return false;
    start_line = a->project.syms[fn].line;
    for (ln = line - 1; ln >= start_line - 1; ln--) {
        size_t s;
        size_t e;
        size_t i;
        if (ln < 0)
            break;
        s = buf_line_start(b, ln);
        e = buf_line_content_end(b, ln);
        for (i = s; i + wlen <= e; i++) {
            bool left_ok;
            bool right_ok;
            size_t p;
            if (memcmp(b->data + i, word, wlen) != 0)
                continue;
            left_ok = i == s ||
                      !(isalnum((unsigned char)b->data[i - 1]) ||
                        b->data[i - 1] == '_');
            p = i + wlen;
            right_ok = p == e ||
                       !(isalnum((unsigned char)b->data[p]) ||
                         b->data[p] == '_');
            if (!left_ok || !right_ok)
                continue;
            while (p < e && (b->data[p] == ' ' || b->data[p] == '\t'))
                p++;
            if (p < e && b->data[p] == '(')
                continue; /* function call */
            if (type_prefix(b->data + s, e - s, i - s, type, type_size)) {
                *decl_line = ln + 1;
                *decl_col = (int)(i - s);
                return true;
            }
        }
    }
    return false;
}

static bool local_type_word(App *a, const char *prefix, size_t len)
{
    static const char *const storage[] = {
        "const", "volatile", "static", "extern", "register", "auto",
        "restrict", "inline", "typedef"
    };
    static const char *const base[] = {
        "void", "char", "int", "float", "double", "_Bool", "bool",
        "size_t", "ssize_t", "FILE", "struct", "union", "enum", "class",
        "unsigned", "signed", "short", "long"
    };
    const char *p = prefix;
    const char *end = prefix + len;
    size_t i;

    while (p < end) {
        const char *start;
        size_t n;
        bool skip = false;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '*' ||
                           *p == '&'))
            p++;
        if (p >= end)
            break;
        start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
            p++;
        n = (size_t)(p - start);
        if (n == 0)
            return false;
        for (i = 0; i < SI_ARRAY_LEN(storage); i++) {
            if (strlen(storage[i]) == n &&
                memcmp(storage[i], start, n) == 0) {
                skip = true;
                break;
            }
        }
        if (skip)
            continue;
        for (i = 0; i < SI_ARRAY_LEN(base); i++) {
            if (strlen(base[i]) == n &&
                memcmp(base[i], start, n) == 0)
                return true;
        }
        return strlist_contains_n(&a->types, start, n);
    }
    return false;
}

static bool local_word_is_type(App *a, const char *word, size_t len)
{
    static const char *const words[] = {
        "const", "volatile", "static", "extern", "register", "auto",
        "restrict", "inline", "typedef", "unsigned", "signed", "short",
        "long", "void", "char", "int", "float", "double", "_Bool", "bool",
        "size_t", "ssize_t", "FILE", "struct", "union", "enum", "class"
    };
    size_t i;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        if (strlen(words[i]) == len &&
            memcmp(words[i], word, len) == 0)
            return true;
    }
    return strlist_contains_n(&a->types, word, len);
}

static int buffer_function_end(Buffer *b, int start_line)
{
    int line;
    int depth = 0;
    bool opened = false;
    bool comment = false;
    char quote = '\0';

    for (line = start_line - 1; line < (int)buf_line_count(b); line++) {
        size_t start = buf_line_start(b, line);
        size_t end = buf_line_content_end(b, line);
        size_t i;
        for (i = start; i < end; i++) {
            char c = b->data[i];
            if (comment) {
                if (c == '*' && i + 1 < end && b->data[i + 1] == '/') {
                    comment = false;
                    i++;
                }
                continue;
            }
            if (quote) {
                if (c == '\\' && i + 1 < end) {
                    i++;
                    continue;
                }
                if (c == quote)
                    quote = '\0';
                continue;
            }
            if (c == '/' && i + 1 < end && b->data[i + 1] == '/')
                break;
            if (c == '/' && i + 1 < end && b->data[i + 1] == '*') {
                comment = true;
                i++;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                continue;
            }
            if (c == '{') {
                depth++;
                opened = true;
            } else if (c == '}') {
                if (depth > 0)
                    depth--;
                if (opened && depth == 0)
                    return line + 1;
            }
        }
    }
    return (int)buf_line_count(b);
}

void app_rebuild_locals(App *a)
{
    Buffer *b = app_buffer(a);
    int fi;
    int i;
    size_t li;

    for (li = 0; li < a->nlocals; li++) {
        free(a->locals[li].name);
        free(a->locals[li].type);
    }
    a->nlocals = 0;
    a->locals_buf = a->active;
    if (!b)
        return;
    a->locals_rev = b->rev;
    fi = app_active_file_index(a);
    if (fi < 0 || (size_t)fi >= a->project.file_count)
        return;

    for (i = 0; i < a->project.files[fi].sym_count; i++) {
        Symbol *fn = &a->project.syms[a->project.files[fi].sym_start + i];
        int fn_end;
        int ln;
        if (fn->kind != SYM_FUNCTION)
            continue;
        fn_end = buffer_function_end(b, fn->line);
        if (fn_end < fn->line)
            fn_end = fn->end_line;
        {
            bool in_comment = false;
            for (ln = fn->line - 1;
                 ln < fn_end && ln < (int)buf_line_count(b); ln++) {
            size_t s = buf_line_start(b, ln);
            size_t e = buf_line_content_end(b, ln);
            size_t p = s;
            while (p < e) {
                size_t word_start;
                size_t word_end;
                size_t next;
                char type[256];
                size_t wi;
                bool duplicate = false;
                bool is_array = false;
                if (in_comment) {
                    if (p + 1 < e && b->data[p] == '*' &&
                        b->data[p + 1] == '/') {
                        in_comment = false;
                        p += 2;
                    } else {
                        p++;
                    }
                    continue;
                }
                if (p + 1 < e && b->data[p] == '/' &&
                    b->data[p + 1] == '/')
                    break;
                if (p + 1 < e && b->data[p] == '/' &&
                    b->data[p + 1] == '*') {
                    in_comment = true;
                    p += 2;
                    continue;
                }
                if (b->data[p] == '"' || b->data[p] == '\'') {
                    char quote = b->data[p++];
                    while (p < e) {
                        if (b->data[p] == '\\' && p + 1 < e) {
                            p += 2;
                            continue;
                        }
                        if (b->data[p] == quote) {
                            p++;
                            break;
                        }
                        p++;
                    }
                    continue;
                }
                if (!(isalpha((unsigned char)b->data[p]) ||
                      b->data[p] == '_')) {
                    p++;
                    continue;
                }
                word_start = p;
                p++;
                while (p < e && (isalnum((unsigned char)b->data[p]) ||
                                 b->data[p] == '_'))
                    p++;
                word_end = p;
                if (local_word_is_type(a, b->data + word_start,
                                       word_end - word_start))
                    continue;
                next = word_end;
                while (next < e &&
                       (b->data[next] == ' ' || b->data[next] == '\t'))
                    next++;
                if (next < e && b->data[next] == '(')
                    continue;
                if (next < e && b->data[next] == '[')
                    is_array = true;
                if (!type_prefix(b->data + s, e - s, word_start - s, type,
                                 sizeof(type)))
                    continue;
                if (!local_type_word(a, type, strlen(type)))
                    continue;
                for (wi = 0; wi < a->nlocals; wi++) {
                    size_t nlen = word_end - word_start;
                    if (a->locals[wi].line == (int)ln + 1 &&
                        strlen(a->locals[wi].name) == nlen &&
                        memcmp(a->locals[wi].name, b->data + word_start,
                               nlen) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;
                if (a->nlocals == a->clocals) {
                    a->clocals = a->clocals ? a->clocals * 2 : 32;
                    a->locals = xrealloc(a->locals,
                                         a->clocals * sizeof(*a->locals));
                }
                a->locals[a->nlocals].name =
                    xstrndup(b->data + word_start, word_end - word_start);
                a->locals[a->nlocals].type = xstrdup(type);
                a->locals[a->nlocals].line = (int)ln + 1;
                a->locals[a->nlocals].end_line = fn_end;
                a->locals[a->nlocals].is_array = is_array;
                a->nlocals++;
            }
            }
        }
    }
}

static LocalVar *find_local_var(App *a, int line, const char *name,
                                size_t len)
{
    LocalVar *best = NULL;
    size_t i;
    for (i = 0; i < a->nlocals; i++) {
        LocalVar *v = &a->locals[i];
        if (line < v->line || line > v->end_line)
            continue;
        if (strlen(v->name) == len && memcmp(v->name, name, len) == 0) {
            if (!best || v->line > best->line)
                best = v;
        }
    }
    return best;
}

bool app_local_in_scope(App *a, int line, const char *name, size_t len)
{
    return find_local_var(a, line, name, len) != NULL;
}

/* ------------------------------------------------------------------ */
/* Completion                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    bool member;
    int line;
    int word_start;
    int word_end;
    int prefix_len;
    char prefix[256];
} CompletionContext;

static bool completion_ident(unsigned char c)
{
    return isalnum(c) || c == '_';
}

static void completion_word_bounds(Buffer *b, int pos, int *start, int *end)
{
    int line_start;
    int line_end;
    int s;
    int e;

    if (!b)
        pos = 0;
    if (pos < 0)
        pos = 0;
    if ((size_t)pos > b->len)
        pos = (int)b->len;
    line_start = (int)buf_line_start(b, buf_line_of_offset(b, (size_t)pos));
    line_end = (int)buf_line_content_end(b, buf_line_of_offset(b, (size_t)pos));
    s = pos;
    while (s > line_start && completion_ident((unsigned char)b->data[s - 1]))
        s--;
    e = pos;
    while (e < line_end && completion_ident((unsigned char)b->data[e]))
        e++;
    *start = s;
    *end = e;
}

static void completion_suppression_clear(App *a)
{
    a->comp_suppress = false;
    a->comp_suppress_first = 0;
    a->comp_suppress_word[0] = '\0';
    a->comp_suppress_member = false;
}

static bool completion_suppression_valid(App *a)
{
    Buffer *b;
    int line;
    int start;
    int end;
    char word[256];
    size_t n;
    size_t sn;

    if (!a->comp_suppress)
        return false;
    b = app_buffer(a);
    if (!b || a->active != a->comp_suppress_buf) {
        completion_suppression_clear(a);
        return false;
    }
    line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
    if (line != a->comp_suppress_line) {
        completion_suppression_clear(a);
        return false;
    }
    completion_word_bounds(b, b->cursor, &start, &end);
    (void)end;
    if (start != a->comp_suppress_start) {
        completion_suppression_clear(a);
        return false;
    }
    if (a->comp_suppress_first != 0 &&
        ((size_t)start >= b->len ||
         (unsigned char)b->data[start] != a->comp_suppress_first)) {
        completion_suppression_clear(a);
        return false;
    }
    n = (size_t)(b->cursor - start);
    if (n >= sizeof(word))
        n = sizeof(word) - 1;
    memcpy(word, b->data + start, n);
    word[n] = '\0';
    if (a->comp_suppress_member && a->comp_suppress_word[0] == '\0')
        return true;
    if (n == 0)
        return true;
    sn = strlen(a->comp_suppress_word);
    if (n <= sn && memcmp(word, a->comp_suppress_word, n) == 0)
        return true;
    if (sn <= n && memcmp(word, a->comp_suppress_word, sn) == 0)
        return true;
    completion_suppression_clear(a);
    return false;
}

static void completion_suppress_current(App *a, int word_start,
                                        const char *word, bool member)
{
    a->comp_suppress = true;
    a->comp_suppress_buf = a->active;
    a->comp_suppress_line = a->comp_line;
    a->comp_suppress_start = word_start;
    a->comp_suppress_first =
        word && *word ? (unsigned char)word[0] : 0;
    a->comp_suppress_member = member;
    snprintf(a->comp_suppress_word, sizeof(a->comp_suppress_word), "%s",
             word ? word : "");
}

static void completion_suppress_at_cursor(App *a)
{
    Buffer *b = app_buffer(a);
    int start;
    int end;
    int line;
    int line_start;
    int p;
    size_t n;
    char word[256];
    bool member = false;

    if (!b)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
    line_start = (int)buf_line_start(b, line - 1);
    completion_word_bounds(b, b->cursor, &start, &end);
    (void)end;
    n = (size_t)(b->cursor - start);
    if (n >= sizeof(word))
        n = sizeof(word) - 1;
    memcpy(word, b->data + start, n);
    word[n] = '\0';
    p = start;
    while (p > line_start &&
           (b->data[p - 1] == ' ' || b->data[p - 1] == '\t'))
        p--;
    if (b->cursor == start) {
        if (p > line_start && b->data[p - 1] == '.')
            member = true;
        else if (p >= line_start + 2 && b->data[p - 1] == '>' &&
                 b->data[p - 2] == '-')
            member = true;
    }
    a->comp_line = line;
    completion_suppress_current(a, start, word, member);
}

bool app_completion_suppresses_word(App *a, const char *word)
{
    size_t n;
    size_t sn;

    if (!word)
        return false;
    if (a->comp_active && strcmp(a->comp_prefix, word) == 0)
        return true;
    if (!completion_suppression_valid(a))
        return false;
    if (a->comp_suppress_member && a->comp_suppress_word[0] == '\0')
        return true;
    n = strlen(word);
    sn = strlen(a->comp_suppress_word);
    if (n <= sn && memcmp(word, a->comp_suppress_word, n) == 0)
        return true;
    return sn <= n && memcmp(word, a->comp_suppress_word, sn) == 0;
}

void app_completion_close(App *a)
{
    if (!a)
        return;
    free(a->comp_items);
    a->comp_items = NULL;
    a->comp_count = 0;
    a->comp_cap = 0;
    a->comp_active = false;
    a->comp_sel = 0;
    a->comp_scroll = 0;
    a->comp_popup_rows = 0;
    a->comp_popup_first = 0;
}

static void completion_clear_member_cache(App *a)
{
    size_t i;
    for (i = 0; i < a->member_cache_count; i++) {
        free(a->member_cache[i].key);
        project_member_list_free(&a->member_cache[i].members);
    }
    free(a->member_cache);
    a->member_cache = NULL;
    a->member_cache_count = 0;
    a->member_cache_cap = 0;
}

void app_completion_reset(App *a)
{
    app_completion_close(a);
    completion_suppression_clear(a);
    completion_clear_member_cache(a);
}

void app_completion_fix(App *a)
{
    int rows = a->comp_popup_rows > 0 ? a->comp_popup_rows : 1;
    if (a->comp_count == 0) {
        app_completion_close(a);
        return;
    }
    if (a->comp_sel < 0)
        a->comp_sel = 0;
    if ((size_t)a->comp_sel >= a->comp_count)
        a->comp_sel = (int)a->comp_count - 1;
    if (a->comp_sel < a->comp_scroll)
        a->comp_scroll = a->comp_sel;
    if (a->comp_sel >= a->comp_scroll + rows)
        a->comp_scroll = a->comp_sel - rows + 1;
    if (a->comp_scroll < 0)
        a->comp_scroll = 0;
}

void app_completion_move(App *a, int delta)
{
    if (!a->comp_active)
        return;
    a->comp_sel += delta;
    app_completion_fix(a);
}

static bool line_is_preproc_continuation(Buffer *b, int line)
{
    int ln = line - 1;

    while (ln >= 0) {
        size_t start = buf_line_start(b, ln);
        size_t end = buf_line_content_end(b, ln);
        size_t p = start;
        while (end > start &&
               (b->data[end - 1] == ' ' || b->data[end - 1] == '\t' ||
                b->data[end - 1] == '\r'))
            end--;
        if (end == start)
            return false;
        if (b->data[end - 1] != '\\')
            return false;
        while (p < end && (b->data[p] == ' ' || b->data[p] == '\t'))
            p++;
        if (p < end && b->data[p] == '#')
            return true;
        ln--;
    }
    return false;
}

static bool completion_context(App *a, CompletionContext *ctx)
{
    Buffer *b;
    HlSpan spans[512];
    int ns;
    int i;
    int pos;
    int line;
    int line_start;
    int line_end;
    int start;
    int end;
    int p;

    if (a->focus != FOCUS_EDITOR || a->prompt != PROMPT_NONE ||
        a->help || a->menu_active || a->chooser_active || a->dir_active ||
        a->project_picker_active || a->context_menu_active ||
        a->relation_building || a->relation_ready_prompt ||
        a->relations_large_prompt || a->has_selection)
        return false;
    b = app_buffer(a);
    if (!b)
        return false;
    pos = b->cursor;
    line = buf_line_of_offset(b, (size_t)pos);
    line_start = (int)buf_line_start(b, line);
    line_end = (int)buf_line_content_end(b, line);
    ns = syntax_line_spans(&b->syn, b, line, spans,
                           (int)SI_ARRAY_LEN(spans));
    for (i = 0; i < ns; i++) {
        int rel = pos - line_start;
        if (rel < spans[i].start || rel > spans[i].end)
            continue;
        if (spans[i].kind == HL_COMMENT || spans[i].kind == HL_STRING ||
            spans[i].kind == HL_PREPROC)
            return false;
    }
    if (line_is_preproc_continuation(b, line))
        return false;
    completion_word_bounds(b, pos, &start, &end);
    if (end > line_end)
        end = line_end;
    memset(ctx, 0, sizeof(*ctx));
    ctx->line = line + 1;
    ctx->word_start = start;
    ctx->word_end = end;
    ctx->prefix_len = pos - start;
    if (ctx->prefix_len > 0) {
        unsigned char first = (unsigned char)b->data[start];
        if (!(isalpha(first) || first == '_'))
            return false;
    }
    p = start;
    while (p > line_start &&
           (b->data[p - 1] == ' ' || b->data[p - 1] == '\t'))
        p--;
    if (p > line_start && b->data[p - 1] == '.')
        ctx->member = true;
    else if (p >= line_start + 2 && b->data[p - 1] == '>' &&
             b->data[p - 2] == '-')
        ctx->member = true;
    if (ctx->prefix_len == 0 && !ctx->member)
        return false;
    if (ctx->prefix_len > (int)sizeof(ctx->prefix) - 1)
        ctx->prefix_len = (int)sizeof(ctx->prefix) - 1;
    memcpy(ctx->prefix, b->data + start, (size_t)ctx->prefix_len);
    ctx->prefix[ctx->prefix_len] = '\0';
    return true;
}

static void completion_item_add(App *a, const char *name, const char *detail,
                                unsigned char group)
{
    CompleteItem *it;
    if (!name || !*name)
        return;
    if (a->comp_count == a->comp_cap) {
        a->comp_cap = a->comp_cap ? a->comp_cap * 2 : 32;
        a->comp_items = xrealloc(a->comp_items,
                                 a->comp_cap * sizeof(*a->comp_items));
    }
    it = &a->comp_items[a->comp_count++];
    it->name = name;
    it->detail = detail ? detail : "";
    it->group = group;
}

static int cmp_complete_name(const void *pa, const void *pb)
{
    const CompleteItem *a = pa;
    const CompleteItem *b = pb;
    int c = strcmp(a->name, b->name);
    if (c)
        return c;
    return (int)a->group - (int)b->group;
}

static int cmp_complete_group(const void *pa, const void *pb)
{
    const CompleteItem *a = pa;
    const CompleteItem *b = pb;
    if (a->group != b->group)
        return (int)a->group - (int)b->group;
    return strcmp(a->name, b->name);
}

static void completion_measure(App *a)
{
    size_t i;
    a->comp_name_w = 0;
    a->comp_detail_w = 0;
    for (i = 0; i < a->comp_count; i++) {
        int nw = (int)utf8_text_width(a->comp_items[i].name,
                                      strlen(a->comp_items[i].name));
        int dw = (int)utf8_text_width(a->comp_items[i].detail,
                                      strlen(a->comp_items[i].detail));
        if (nw > a->comp_name_w)
            a->comp_name_w = nw;
        if (dw > a->comp_detail_w)
            a->comp_detail_w = dw;
    }
}

static void completion_sort_items(App *a)
{
    size_t i;
    size_t out;

    if (a->comp_count > 1)
        qsort(a->comp_items, a->comp_count, sizeof(*a->comp_items),
              cmp_complete_name);
    out = 0;
    for (i = 0; i < a->comp_count; i++) {
        if (out > 0 &&
            strcmp(a->comp_items[out - 1].name, a->comp_items[i].name) == 0)
            continue;
        if (out != i)
            a->comp_items[out] = a->comp_items[i];
        out++;
    }
    a->comp_count = out;
    if (a->comp_count > 1)
        qsort(a->comp_items, a->comp_count, sizeof(*a->comp_items),
              cmp_complete_group);
}

static void completion_collect_locals(App *a, const CompletionContext *ctx)
{
    size_t i;
    size_t j;
    for (i = 0; i < a->nlocals; i++) {
        LocalVar *v = &a->locals[i];
        bool shadowed = false;
        if (ctx->line < v->line || ctx->line > v->end_line)
            continue;
        if (ctx->prefix_len > 0 &&
            (strlen(v->name) < (size_t)ctx->prefix_len ||
             memcmp(v->name, ctx->prefix, (size_t)ctx->prefix_len) != 0))
            continue;
        for (j = 0; j < a->nlocals; j++) {
            LocalVar *w = &a->locals[j];
            if (j == i || strcmp(v->name, w->name) != 0)
                continue;
            if (ctx->line < w->line || ctx->line > w->end_line)
                continue;
            if (w->line > v->line) {
                shadowed = true;
                break;
            }
        }
        if (shadowed)
            continue;
        completion_item_add(a, v->name, v->type, COMPLETE_GROUP_LOCAL);
    }
}

static int completion_symbol_group(SymbolKind kind)
{
    switch (kind) {
    case SYM_VARIABLE:
    case SYM_ENUMVAL:
        return COMPLETE_GROUP_GLOBAL;
    case SYM_FUNCTION:
    case SYM_PROTOTYPE:
        return COMPLETE_GROUP_FUNCTION;
    case SYM_MACRO:
        return COMPLETE_GROUP_MACRO;
    case SYM_STRUCT:
    case SYM_UNION:
    case SYM_ENUM:
    case SYM_CLASS:
    case SYM_TYPEDEF:
        return COMPLETE_GROUP_TYPE;
    default:
        return -1;
    }
}

static const char *completion_symbol_detail(Symbol *s)
{
    switch (s->kind) {
    case SYM_STRUCT:
    case SYM_UNION:
    case SYM_ENUM:
    case SYM_CLASS:
    case SYM_TYPEDEF:
        return symbol_kind_short(s->kind);
    default:
        return s->signature;
    }
}

static size_t completion_lower_bound(App *a, const char *prefix, size_t len)
{
    size_t lo = 0;
    size_t hi = a->sym_name_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strncmp(a->sym_name_index[mid]->name, prefix, len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void completion_collect_symbols(App *a, const CompletionContext *ctx)
{
    size_t i;
    size_t lo;
    if (!a->sym_name_index || a->sym_name_count == 0)
        return;
    lo = completion_lower_bound(a, ctx->prefix, (size_t)ctx->prefix_len);
    for (i = lo; i < a->sym_name_count; i++) {
        Symbol *s = a->sym_name_index[i];
        int group;
        if (strncmp(s->name, ctx->prefix, (size_t)ctx->prefix_len) != 0)
            break;
        group = completion_symbol_group(s->kind);
        if (group < 0)
            continue;
        completion_item_add(a, s->name, completion_symbol_detail(s),
                            (unsigned char)group);
    }
}

static void completion_collect_keywords(App *a, const CompletionContext *ctx)
{
    size_t i;
    for (i = 0; i < syntax_keyword_count(); i++) {
        const char *word = syntax_keyword_at(i);
        if (ctx->prefix_len > 0 &&
            (strlen(word) < (size_t)ctx->prefix_len ||
             memcmp(word, ctx->prefix, (size_t)ctx->prefix_len) != 0))
            continue;
        completion_item_add(a, word, "keyword", COMPLETE_GROUP_KEYWORD);
    }
}

static void completion_collect_types(App *a, const CompletionContext *ctx)
{
    size_t i;
    for (i = 0; i < syntax_type_count(); i++) {
        const char *word = syntax_type_at(i);
        if (ctx->prefix_len > 0 &&
            (strlen(word) < (size_t)ctx->prefix_len ||
             memcmp(word, ctx->prefix, (size_t)ctx->prefix_len) != 0))
            continue;
        completion_item_add(a, word, "type", COMPLETE_GROUP_TYPE);
    }
}

static bool completion_symbol_type(Symbol *s, char *out, size_t out_size)
{
    const char *sig = s->signature;
    size_t nlen = strlen(s->name);
    const char *p = sig;
    const char *last = NULL;
    size_t n;

    while ((p = strstr(p, s->name)) != NULL) {
        bool left = p == sig ||
                    !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        bool right = !(isalnum((unsigned char)p[nlen]) || p[nlen] == '_');
        if (left && right)
            last = p;
        p += nlen;
    }
    if (!last)
        return false;
    {
        const char *after = last + nlen;
        while (*after == ' ' || *after == '\t')
            after++;
        if (*after == '[')
            return false;
    }
    n = (size_t)(last - sig);
    while (n > 0 && (sig[n - 1] == ' ' || sig[n - 1] == '\t'))
        n--;
    if (n == 0)
        return false;
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, sig, n);
    out[n] = '\0';
    return true;
}

static ProjectMemberList *completion_members(App *a, const char *type_text)
{
    char key[256];
    size_t i;
    CompleteMemberCache *c;

    if (!project_type_aggregate_key(type_text, key, sizeof(key)))
        return NULL;
    for (i = 0; i < a->member_cache_count; i++) {
        if (strcmp(a->member_cache[i].key, key) == 0)
            return a->member_cache[i].found
                       ? &a->member_cache[i].members
                       : NULL;
    }
    if (a->member_cache_count == a->member_cache_cap) {
        a->member_cache_cap = a->member_cache_cap
                                  ? a->member_cache_cap * 2
                                  : 16;
        a->member_cache =
            xrealloc(a->member_cache,
                     a->member_cache_cap * sizeof(*a->member_cache));
    }
    c = &a->member_cache[a->member_cache_count++];
    c->key = xstrdup(key);
    project_member_list_init(&c->members);
    c->found = project_aggregate_members(&a->project, key, &c->members);
    if (!c->found) {
        project_member_list_free(&c->members);
        return NULL;
    }
    return &c->members;
}

static bool completion_global_type(App *a, const char *name, size_t len,
                                   char *out, size_t out_size)
{
    size_t lo;
    size_t i;
    int fi = app_active_file_index(a);
    Symbol *fallback = NULL;

    if (!a->sym_name_index || a->sym_name_count == 0)
        return false;
    lo = completion_lower_bound(a, name, len);
    for (i = lo; i < a->sym_name_count; i++) {
        Symbol *s = a->sym_name_index[i];
        if (strncmp(s->name, name, len) != 0)
            break;
        if (strlen(s->name) != len)
            continue;
        if (s->kind != SYM_VARIABLE)
            continue;
        if (s->file_index == fi)
            return completion_symbol_type(s, out, out_size);
        if (!fallback && !strstr(s->signature, "static"))
            fallback = s;
    }
    if (fallback)
        return completion_symbol_type(fallback, out, out_size);
    return false;
}

static bool completion_enclosing_class(App *a, int line, char *out,
                                       size_t out_size)
{
    int fi = app_active_file_index(a);
    int best = -1;
    int best_span = INT_MAX;
    int i;

    if (fi < 0 || (size_t)fi >= a->project.file_count)
        return false;
    for (i = 0; i < a->project.files[fi].sym_count; i++) {
        int si = a->project.files[fi].sym_start + i;
        Symbol *s = &a->project.syms[si];
        int span;
        if (s->kind != SYM_CLASS && s->kind != SYM_STRUCT &&
            s->kind != SYM_UNION)
            continue;
        if (line < s->line || line > s->end_line)
            continue;
        span = s->end_line - s->line;
        if (best < 0 || span < best_span) {
            best = si;
            best_span = span;
        }
    }
    if (best < 0)
        return false;
    snprintf(out, out_size, "%s", a->project.syms[best].name);
    return true;
}

static bool completion_member_type(App *a, const CompletionContext *ctx,
                                   char *out, size_t out_size)
{
    Buffer *b = app_buffer(a);
    struct {
        int start;
        int len;
    } segs[16];
    int n = 0;
    int line_start;
    int p;
    int i;

    if (!b)
        return false;
    line_start = (int)buf_line_start(b, ctx->line - 1);
    p = ctx->word_start;
    while (p > line_start &&
           (b->data[p - 1] == ' ' || b->data[p - 1] == '\t'))
        p--;
    if (p > line_start && b->data[p - 1] == '.') {
        p--;
    } else if (p >= line_start + 2 && b->data[p - 1] == '>' &&
               b->data[p - 2] == '-') {
        p -= 2;
    } else {
        return false;
    }
    for (;;) {
        int end;
        while (p > line_start &&
               (b->data[p - 1] == ' ' || b->data[p - 1] == '\t'))
            p--;
        if (p <= line_start || !completion_ident((unsigned char)b->data[p - 1]))
            return false;
        end = p;
        while (p > line_start &&
               completion_ident((unsigned char)b->data[p - 1]))
            p--;
        if (n >= (int)SI_ARRAY_LEN(segs))
            return false;
        segs[n].start = p;
        segs[n].len = end - p;
        n++;
        while (p > line_start &&
               (b->data[p - 1] == ' ' || b->data[p - 1] == '\t'))
            p--;
        if (p > line_start && b->data[p - 1] == '.') {
            p--;
            continue;
        }
        if (p >= line_start + 2 && b->data[p - 1] == '>' &&
            b->data[p - 2] == '-') {
            p -= 2;
            continue;
        }
        break;
    }
    if (n == 0)
        return false;
    for (i = n - 1; i >= 0; i--) {
        const char *name = b->data + segs[i].start;
        size_t len = (size_t)segs[i].len;
        if (i == n - 1) {
            LocalVar *lv;
            if (len == 4 && memcmp(name, "this", 4) == 0) {
                if (!completion_enclosing_class(a, ctx->line, out,
                                                out_size))
                    return false;
                continue;
            }
            lv = find_local_var(a, ctx->line, name, len);
            if (lv) {
                if (lv->is_array)
                    return false;
                snprintf(out, out_size, "%s", lv->type);
            } else if (!completion_global_type(a, name, len, out,
                                               out_size)) {
                return false;
            }
        } else {
            ProjectMemberList *members = completion_members(a, out);
            ProjectMember *m = NULL;
            size_t j;
            if (!members)
                return false;
            for (j = 0; j < members->count; j++) {
                ProjectMember *cand = &members->items[j];
                if (strlen(cand->name) == len &&
                    memcmp(cand->name, name, len) == 0) {
                    m = cand;
                    break;
                }
            }
            if (!m || !m->type[0] || m->is_array)
                return false;
            snprintf(out, out_size, "%s", m->type);
        }
    }
    return out[0] != '\0';
}

void app_completion_update(App *a)
{
    CompletionContext ctx;
    Buffer *b;

    a->comp_popup_rows = 0;
    if (!completion_context(a, &ctx)) {
        app_completion_close(a);
        return;
    }
    if (completion_suppression_valid(a)) {
        app_completion_close(a);
        return;
    }
    b = app_buffer(a);
    if (a->locals_buf != a->active || a->locals_rev != b->rev)
        app_rebuild_locals(a);
    free(a->comp_items);
    a->comp_items = NULL;
    a->comp_count = 0;
    a->comp_cap = 0;
    a->comp_buf = a->active;
    a->comp_line = ctx.line;
    a->comp_word_start = ctx.word_start;
    a->comp_word_end = ctx.word_end;
    a->comp_prefix_len = ctx.prefix_len;
    snprintf(a->comp_prefix, sizeof(a->comp_prefix), "%s", ctx.prefix);
    a->comp_member = ctx.member;
    if (ctx.member) {
        char type[512];
        ProjectMemberList *members;
        size_t i;
        if (!completion_member_type(a, &ctx, type, sizeof(type))) {
            app_completion_close(a);
            return;
        }
        members = completion_members(a, type);
        if (!members) {
            app_completion_close(a);
            return;
        }
        for (i = 0; i < members->count; i++) {
            ProjectMember *m = &members->items[i];
            const char *detail;
            if (ctx.prefix_len > 0 &&
                (strlen(m->name) < (size_t)ctx.prefix_len ||
                 memcmp(m->name, ctx.prefix, (size_t)ctx.prefix_len) != 0))
                continue;
            if (m->is_type)
                detail = "type";
            else if (m->is_method || m->is_static)
                detail = m->signature;
            else
                detail = m->type[0] ? m->type : m->signature;
            completion_item_add(a, m->name, detail, COMPLETE_GROUP_MEMBER);
        }
    } else {
        completion_collect_locals(a, &ctx);
        completion_collect_symbols(a, &ctx);
        completion_collect_keywords(a, &ctx);
        completion_collect_types(a, &ctx);
    }
    if (a->comp_count == 0) {
        app_completion_close(a);
        return;
    }
    completion_sort_items(a);
    completion_measure(a);
    if (a->comp_count == 1 && ctx.word_end == b->cursor &&
        strcmp(a->comp_items[0].name, ctx.prefix) == 0) {
        completion_suppress_current(a, ctx.word_start, ctx.prefix,
                                    ctx.member);
        app_completion_close(a);
        return;
    }
    a->comp_active = true;
    a->comp_sel = 0;
    a->comp_scroll = 0;
    app_completion_fix(a);
}

void app_completion_accept(App *a)
{
    char *name;
    size_t name_len;
    int prefix_len;
    int word_start;
    int word_end;
    int cursor;
    Buffer *b;

    if (!a->comp_active || a->comp_count == 0)
        return;
    b = app_buffer(a);
    if (!b)
        return;
    name = xstrdup(a->comp_items[a->comp_sel].name);
    name_len = strlen(name);
    prefix_len = a->comp_prefix_len;
    word_start = a->comp_word_start;
    word_end = a->comp_word_end;
    cursor = b->cursor;
    if (word_start < 0 || word_end < word_start ||
        (size_t)word_end > b->len || cursor < word_start ||
        cursor > word_end) {
        free(name);
        app_completion_close(a);
        return;
    }
    if (prefix_len <= (int)name_len &&
        strncmp(name, a->comp_prefix, (size_t)prefix_len) == 0) {
        if (word_end > cursor)
            delete_at_cursor(a, (size_t)cursor, (size_t)(word_end - cursor));
        if (name_len > (size_t)prefix_len)
            insert_at_cursor(a, name + prefix_len, name_len -
                             (size_t)prefix_len);
    } else {
        delete_at_cursor(a, (size_t)word_start,
                         (size_t)(word_end - word_start));
        insert_at_cursor(a, name, name_len);
    }
    completion_suppress_current(a, word_start, name, a->comp_member);
    free(name);
    app_completion_close(a);
}

bool app_completion_handle_key(App *a, KeyEvent *ev)
{
    if (!a->comp_active || a->comp_count == 0)
        return false;
    switch (ev->code) {
    case KEY_TAB:
    case KEY_ENTER:
        app_completion_accept(a);
        return true;
    case KEY_ESC:
        completion_suppress_current(a, a->comp_word_start, a->comp_prefix,
                                    a->comp_member);
        app_completion_close(a);
        return true;
    case KEY_UP:
        app_completion_move(a, -1);
        return true;
    case KEY_DOWN:
        app_completion_move(a, 1);
        return true;
    case KEY_PGUP:
        app_completion_move(a, -SI_MAX(1, a->comp_popup_rows));
        return true;
    case KEY_PGDN:
        app_completion_move(a, SI_MAX(1, a->comp_popup_rows));
        return true;
    case KEY_HOME:
        a->comp_sel = 0;
        app_completion_fix(a);
        return true;
    case KEY_END:
        a->comp_sel = a->comp_count ? (int)a->comp_count - 1 : 0;
        app_completion_fix(a);
        return true;
    case KEY_CHAR:
    case KEY_BACKSPACE:
    case KEY_DELETE:
        return false;
    case KEY_MOUSE:
        return app_completion_handle_mouse(a, ev);
    default:
        app_completion_close(a);
        return false;
    }
}

bool app_completion_handle_mouse(App *a, KeyEvent *ev)
{
    int x;
    int y;
    int w;
    int h;
    int first;
    int rows;
    int mx;
    int my;

    if (!a->comp_active || a->comp_count == 0)
        return false;
    if (!app_completion_geometry(a, &x, &y, &w, &h, &first, &rows))
        return false;
    mx = ev->mouse_x;
    my = ev->mouse_y;
    if (ev->mouse_button & 32)
        return false; /* SGR motion: let the normal hover path run */
    if (ev->mouse_button == 64 || ev->mouse_button == 65) {
        if (mx > x && mx < x + w - 1 && my > y && my < y + h - 1) {
            app_completion_move(a, ev->mouse_button == 64 ? -3 : 3);
            return true;
        }
        app_completion_close(a);
        return false;
    }
    if (ev->mouse_release)
        return false;
    if (ev->mouse_button != 0) {
        app_completion_close(a);
        return false;
    }
    if (mx > x && mx < x + w - 1 && my > y && my < y + h - 1) {
        int idx = first + (my - y - 1);
        long now = (long)time(NULL);
        if (idx >= 0 && (size_t)idx < a->comp_count) {
            if (idx == a->comp_sel && a->comp_click_time > 0 &&
                now - a->comp_click_time <= 1 &&
                abs(mx - a->comp_click_x) <= 2 &&
                abs(my - a->comp_click_y) <= 2) {
                app_completion_accept(a);
                return true;
            }
            a->comp_sel = idx;
            app_completion_fix(a);
            a->comp_click_time = now;
            a->comp_click_index = idx;
            a->comp_click_x = mx;
            a->comp_click_y = my;
            return true;
        }
        return false;
    }
    app_completion_close(a);
    return false;
}

bool app_completion_geometry(App *a, int *x, int *y, int *w, int *h,
                             int *first, int *rows)
{
    int top;
    int bottom;
    int maxw;
    int name_w;
    int detail_w;
    int boxw;
    int below_y;
    int below_avail;
    int above_avail;
    int avail;
    int boxh;
    int boxy;
    int boxx;
    int visible;

    if (!a->comp_active || a->comp_count == 0 || !a->show_cursor)
        return false;
    top = a->content_top;
    bottom = a->content_top + a->content_height - 1;
    if (bottom < top)
        return false;
    maxw = a->cols - 2;
    name_w = a->comp_name_w;
    detail_w = a->comp_detail_w;
    boxw = 4 + name_w;
    if (detail_w > 0)
        boxw += 2 + detail_w;
    if (boxw > maxw)
        boxw = maxw;
    if (boxw < 10)
        return false;
    below_y = a->cursor_y + 1;
    below_avail = bottom - below_y + 1;
    above_avail = a->cursor_y - top;
    if (below_avail >= 3 || below_avail >= above_avail)
        avail = below_avail;
    else
        avail = above_avail;
    if (avail < 3)
        return false;
    visible = (int)a->comp_count;
    if (visible > avail - 2)
        visible = avail - 2;
    if (visible < 1)
        return false;
    boxh = visible + 2;
    boxy = (avail == below_avail) ? below_y : a->cursor_y - boxh;
    if (boxy < top)
        boxy = top;
    if (boxy + boxh - 1 > bottom)
        boxy = bottom - boxh + 1;
    boxx = a->cursor_x;
    if (boxx + boxw > a->cols - 1)
        boxx = a->cols - 1 - boxw;
    if (boxx < 1)
        boxx = 1;
    a->comp_popup_rows = visible;
    if (a->comp_sel < a->comp_scroll)
        a->comp_scroll = a->comp_sel;
    if (a->comp_sel >= a->comp_scroll + visible)
        a->comp_scroll = a->comp_sel - visible + 1;
    if (a->comp_scroll > (int)a->comp_count - visible)
        a->comp_scroll = (int)a->comp_count - visible;
    if (a->comp_scroll < 0)
        a->comp_scroll = 0;
    a->comp_popup_x = boxx;
    a->comp_popup_y = boxy;
    a->comp_popup_w = boxw;
    a->comp_popup_h = boxh;
    a->comp_popup_first = a->comp_scroll;
    a->comp_popup_rows = visible;
    if (x)
        *x = boxx;
    if (y)
        *y = boxy;
    if (w)
        *w = boxw;
    if (h)
        *h = boxh;
    if (first)
        *first = a->comp_scroll;
    if (rows)
        *rows = visible;
    return true;
}

static bool symbol_is_extern_var(const Symbol *s)
{
    return s->kind == SYM_VARIABLE && s->signature &&
           strstr(s->signature, "extern") != NULL;
}

static bool symbol_is_definition(const Symbol *s)
{
    return s->kind != SYM_PROTOTYPE && !symbol_is_extern_var(s);
}

/*
 * Resolve a name in strict priority order:
 *   current file definitions -> other-file definitions -> declarations.
 * If the chosen priority level has multiple candidates, return -1 and
 * fill `choice`; the caller opens the definition chooser.
 */
static int app_pick_symbol(App *a, const char *word, bool member_access,
                           IntList *choice)
{
    IntList ids;
    IntList same;
    IntList other;
    IntList same_member;
    IntList other_member;
    IntList fallback_same;
    IntList fallback_other;
    int fi = app_active_file_index(a);
    int result = -1;
    size_t i;
    Buffer *b = app_buffer(a);
    if (b && b->path &&
        (fi < 0 || (size_t)fi >= a->project.file_count ||
         strcmp(a->project.files[fi].path, b->path) != 0))
        fi = project_file_index_of_path(&a->project, b->path);

    intlist_init(&ids);
    intlist_init(&same);
    intlist_init(&other);
    intlist_init(&same_member);
    intlist_init(&other_member);
    intlist_init(&fallback_same);
    intlist_init(&fallback_other);
    project_find_symbols(&a->project, word, &ids);
    for (i = 0; i < ids.count; i++) {
        int id = ids.items[i];
        Symbol *s = &a->project.syms[id];
        if (symbol_is_definition(s)) {
            if (member_access && s->kind == SYM_MEMBER) {
                if (s->file_index == fi)
                    intlist_push(&same_member, id);
                else
                    intlist_push(&other_member, id);
            }
            if (s->file_index == fi)
                intlist_push(&same, id);
            else
                intlist_push(&other, id);
        } else {
            if (s->file_index == fi)
                intlist_push(&fallback_same, id);
            else
                intlist_push(&fallback_other, id);
        }
    }

    if (member_access && (same_member.count > 0 || other_member.count > 0)) {
        intlist_free(&same);
        intlist_free(&other);
        same = same_member;
        other = other_member;
        same_member.items = NULL;
        other_member.items = NULL;
        same_member.count = same_member.cap = 0;
        other_member.count = other_member.cap = 0;
    }

    if (same.count == 1) {
        result = same.items[0];
    } else if (same.count > 1) {
        for (i = 0; i < same.count; i++)
            intlist_push(choice, same.items[i]);
    } else if (fallback_same.count == 1) {
        /* Current file has only a declaration (extern/prototype): still
         * prefer it over definitions that live in other files. */
        result = fallback_same.items[0];
    } else if (fallback_same.count > 1) {
        for (i = 0; i < fallback_same.count; i++)
            intlist_push(choice, fallback_same.items[i]);
    } else if (other.count == 1) {
        result = other.items[0];
    } else if (other.count > 1) {
        for (i = 0; i < other.count; i++)
            intlist_push(choice, other.items[i]);
    } else if (fallback_other.count == 1) {
        result = fallback_other.items[0];
    } else if (fallback_other.count > 1) {
        for (i = 0; i < fallback_other.count; i++)
            intlist_push(choice, fallback_other.items[i]);
    }

    intlist_free(&ids);
    intlist_free(&same);
    intlist_free(&other);
    intlist_free(&same_member);
    intlist_free(&other_member);
    intlist_free(&fallback_same);
    intlist_free(&fallback_other);
    return result;
}

static int member_parent_symbol(Project *p, const Symbol *member)
{
    int best = -1;
    int best_span = INT_MAX;
    size_t i;
    for (i = 0; i < p->sym_count; i++) {
        Symbol *s = &p->syms[i];
        int span;
        if (s->file_index != member->file_index)
            continue;
        if (s->kind != SYM_STRUCT && s->kind != SYM_UNION &&
            s->kind != SYM_CLASS)
            continue;
        if (s->line > member->line || s->end_line < member->line)
            continue;
        span = s->end_line - s->line;
        if (span < best_span) {
            best_span = span;
            best = (int)i;
        }
    }
    return best;
}

void app_update_info(App *a)
{
    Buffer *b = app_buffer(a);
    char *word;
    int line;
    int fn;
    int best = -1;
    int fi;
    bool member_access = false;

    if (b && (a->locals_buf != a->active || a->locals_rev != b->rev))
        app_rebuild_locals(a);
    if (!b) {
        a->info_valid = false;
        a->info_word[0] = '\0';
        buf_free(a->info_buffer);
        a->info_buffer = NULL;
        return;
    }
    word = word_at_cursor(a);
    line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
    if (!word) {
        a->info_valid = false;
        a->info_word[0] = '\0';
        a->info_line = line;
        buf_free(a->info_buffer);
        a->info_buffer = NULL;
        return;
    }
    if (a->pending_info_sym >= 0 &&
        strcmp(a->pending_info_word, word) != 0) {
        a->pending_info_sym = -1;
        a->pending_info_word[0] = '\0';
    }
    if (a->info_line == line && strcmp(a->info_word, word) == 0) {
        free(word);
        return;
    }
    snprintf(a->info_word, sizeof(a->info_word), "%s", word);
    a->info_line = line;
    a->info_click_time = 0;
    a->info_type[0] = '\0';
    a->info_signature[0] = '\0';
    a->info_kind[0] = '\0';
    a->info_where[0] = '\0';
    a->info_scope[0] = '\0';
    a->info_refs[0] = '\0';
    a->info_def_line = line;
    a->info_def_end = line;
    a->info_def_file = -1;
    a->info_code_start = 0;
    a->info_scroll = 0;
    buf_free(a->info_buffer);
    a->info_buffer = NULL;
    fi = app_active_file_index(a);
    member_access = cursor_is_member_access(b);

    if (!member_access) {
        LocalVar *lv = find_local_var(a, line, word, strlen(word));
        if (lv) {
            a->info_def_file = fi;
            a->info_def_line = lv->line;
            a->info_def_end = lv->line + 2;
            a->info_code_start = lv->line > 1 ? lv->line - 1 : 1;
            snprintf(a->info_type, sizeof(a->info_type), "%s", lv->type);
            snprintf(a->info_signature, sizeof(a->info_signature),
                     "%s %s", lv->type, word);
            snprintf(a->info_kind, sizeof(a->info_kind), "LOCAL VARIABLE");
            snprintf(a->info_where, sizeof(a->info_where), "%s:%d",
                     b->path, lv->line);
            a->file_sel = fi;
            fn = enclosing_function_for_line(a, line);
            if (fn >= 0)
                snprintf(a->info_scope, sizeof(a->info_scope), "%s",
                         a->project.syms[fn].name);
            snprintf(a->info_refs, sizeof(a->info_refs),
                     "local declaration");
            a->info_valid = true;
            free(word);
            return;
        }
    }

    if (!member_access) {
        int decl_line = 0;
        int decl_col = 0;
        char decl_type[256];
        if (find_local_decl(a, b, word, line, decl_type,
                            sizeof(decl_type), &decl_line, &decl_col)) {
            a->info_def_file = fi;
            a->info_def_line = decl_line;
            a->info_def_end = decl_line + 2;
            a->info_code_start = decl_line > 1 ? decl_line - 1 : 1;
            snprintf(a->info_type, sizeof(a->info_type), "%s", decl_type);
            snprintf(a->info_signature, sizeof(a->info_signature), "%s %s",
                     decl_type, word);
            snprintf(a->info_kind, sizeof(a->info_kind), "LOCAL VARIABLE");
            snprintf(a->info_where, sizeof(a->info_where), "%s:%d",
                     b->path, decl_line);
            a->file_sel = fi;
            fn = enclosing_function_for_line(a, line);
            if (fn >= 0)
                snprintf(a->info_scope, sizeof(a->info_scope), "%s",
                         a->project.syms[fn].name);
            snprintf(a->info_refs, sizeof(a->info_refs),
                     "local declaration at column %d", decl_col + 1);
            a->info_valid = true;
            free(word);
            return;
        }
    }

    fi = app_active_file_index(a);
    if (a->pending_info_sym >= 0 &&
        strcmp(a->pending_info_word, word) == 0) {
        best = a->pending_info_sym;
    } else {
        IntList choice;
        intlist_init(&choice);
        best = app_pick_symbol(a, word, member_access, &choice);
        if (choice.count > 1) {
            if (app_completion_suppresses_word(a, word)) {
                best = choice.items[0];
            } else {
                app_chooser_open(a, &choice, word);
                intlist_free(&choice);
                free(word);
                return;
            }
        }
        intlist_free(&choice);
    }

    if (best >= 0) {
        Symbol *s = &a->project.syms[best];
        a->info_def_file = s->file_index;
        a->info_def_line = s->line;
        a->info_def_end = s->kind == SYM_FUNCTION ? s->end_line
                                                  : s->line + 2;
        a->info_code_start = s->line;
        if (s->kind != SYM_FUNCTION && a->info_code_start > 1)
            a->info_code_start--;
        if (s->file_index != fi) {
            char *err = NULL;
            a->info_buffer = buf_open(a->project.files[s->file_index].path,
                                      &err);
            free(err);
        }
        snprintf(a->info_type, sizeof(a->info_type), "%s", s->signature);
        snprintf(a->info_signature, sizeof(a->info_signature), "%s",
                 s->signature);
        if (s->kind == SYM_FUNCTION)
            snprintf(a->info_kind, sizeof(a->info_kind), "FUNCTION");
        else if (s->kind == SYM_VARIABLE)
            snprintf(a->info_kind, sizeof(a->info_kind), "GLOBAL VARIABLE");
        else if (s->kind == SYM_MEMBER)
            snprintf(a->info_kind, sizeof(a->info_kind), "STRUCT MEMBER");
        else if (s->kind == SYM_MACRO)
            snprintf(a->info_kind, sizeof(a->info_kind), "MACRO");
        else if (s->kind == SYM_STRUCT || s->kind == SYM_UNION ||
                 s->kind == SYM_ENUM || s->kind == SYM_TYPEDEF ||
                 s->kind == SYM_CLASS)
            snprintf(a->info_kind, sizeof(a->info_kind), "TYPE");
        else
            snprintf(a->info_kind, sizeof(a->info_kind), "SYMBOL");
        snprintf(a->info_where, sizeof(a->info_where), "%s:%d",
                 a->project.files[s->file_index].path, s->line);
        a->file_sel = s->file_index;
        if (s->kind == SYM_MEMBER) {
            int parent = member_parent_symbol(&a->project, s);
            if (parent >= 0)
                snprintf(a->info_scope, sizeof(a->info_scope), "%s",
                         a->project.syms[parent].name);
            else
                snprintf(a->info_scope, sizeof(a->info_scope), "aggregate");
        } else {
            fn = enclosing_function_for_line(a, line);
            if (fn >= 0)
                snprintf(a->info_scope, sizeof(a->info_scope), "%s",
                         a->project.syms[fn].name);
            else
                snprintf(a->info_scope, sizeof(a->info_scope), "file scope");
        }
        snprintf(a->info_refs, sizeof(a->info_refs),
                 "Shift+F12 shows references");
        a->info_valid = true;
    } else {
        int decl_line = 0;
        int decl_col = 0;
        char type[256];
        if (find_local_decl(a, b, word, line, type, sizeof(type),
                            &decl_line, &decl_col)) {
            a->info_def_file = fi;
            a->info_def_line = decl_line;
            a->info_def_end = decl_line + 2;
            a->info_code_start = decl_line > 1 ? decl_line - 1 : 1;
            snprintf(a->info_type, sizeof(a->info_type), "%s", type);
            snprintf(a->info_where, sizeof(a->info_where), "%s:%d",
                     b->path, decl_line);
            a->file_sel = fi;
            fn = enclosing_function_for_line(a, line);
            if (fn >= 0)
                snprintf(a->info_scope, sizeof(a->info_scope), "%s",
                         a->project.syms[fn].name);
            snprintf(a->info_refs, sizeof(a->info_refs),
                     "local declaration at column %d", decl_col + 1);
            a->info_valid = true;
        }
    }
    if (!a->info_valid) {
        a->info_def_file = -1;
        a->info_code_start = 0;
        snprintf(a->info_type, sizeof(a->info_type), "local / not indexed");
        snprintf(a->info_signature, sizeof(a->info_signature), "%s", word);
        snprintf(a->info_kind, sizeof(a->info_kind), "SYMBOL");
        snprintf(a->info_where, sizeof(a->info_where), "%s:%d", b->path, line);
        a->file_sel = fi;
        fn = enclosing_function_for_line(a, line);
        if (fn >= 0)
            snprintf(a->info_scope, sizeof(a->info_scope), "%s",
                     a->project.syms[fn].name);
        else
            snprintf(a->info_scope, sizeof(a->info_scope), "file scope");
    }
    free(word);
}

static void action_goto_line(App *a, int line)
{
    Buffer *b = app_buffer(a);
    int count;
    if (!b)
        return;
    count = (int)buf_line_count(b);
    if (line < 1)
        line = 1;
    if (line > count)
        line = count;
    set_cursor_line_col(a, b, line, 0);
    b->scroll_line = SI_MAX(0, line - 1 - a->ed_h / 3);
    app_set_status(a, "Line %d", line);
    app_ensure_visible(a);
}

static void action_find(App *a, const char *needle, bool forward)
{
    Buffer *b = app_buffer(a);
    BufMatch m;
    size_t start;
    if (!b || !needle || !*needle)
        return;
    if (forward)
        start = (size_t)b->cursor + 1;
    else
        start = (size_t)b->cursor > 0 ? (size_t)b->cursor - 1 : 0;
    if (!buf_find(b, needle, start, forward, true, &m)) {
        app_set_status(a, "Pattern not found: %s", needle);
        a->find_pos = 0;
        a->find_len = 0;
        return;
    }
    b->cursor = (int)m.pos;
    b->goal_col = buf_visual_col(b, m.pos);
    a->find_pos = (int)m.pos;
    a->find_len = (int)m.len;
    if (needle != a->last_find)
        snprintf(a->last_find, sizeof(a->last_find), "%s", needle);
    app_set_status(a, "Found at %d:%d", m.line + 1, m.col + 1);
    app_ensure_visible(a);
}

static void action_goto_definition(App *a)
{
    char *word = word_at_cursor(a);
    IntList ids;
    IntList ordered;
    size_t i;
    int cat;
    int active_fi;

    if (!word) {
        app_set_status(a, "No identifier under cursor");
        return;
    }
    intlist_init(&ids);
    intlist_init(&ordered);
    project_find_symbols(&a->project, word, &ids);
    if (ids.count == 0) {
        app_set_status(a, "No definition for '%s'", word);
        intlist_free(&ids);
        free(word);
        return;
    }
    if (ids.count == 1) {
        Symbol *s = &a->project.syms[ids.items[0]];
        app_goto_location(a, s->file_index, s->line, s->col, true);
        if (a->view.kind != VIEW_CONTEXT)
            app_show_context(a);
        intlist_free(&ids);
        free(word);
        return;
    }
    active_fi = app_active_file_index(a);
    for (cat = 0; cat < 4; cat++) {
        for (i = 0; i < ids.count; i++) {
            Symbol *s = &a->project.syms[ids.items[i]];
            bool def = s->kind == SYM_FUNCTION;
            bool local = s->file_index == active_fi;
            int c = def ? (local ? 0 : 1) : (local ? 2 : 3);
            if (c == cat)
                intlist_push(&ordered, ids.items[i]);
        }
    }
    {
        char title[128];
        snprintf(title, sizeof(title), "DEFINITIONS  %s", word);
        view_symbols(a, &ordered, VIEW_DEFS, title);
    }
    a->focus = FOCUS_CONTEXT;
    app_set_status(a, "%lu definitions of '%s' - Enter to jump",
                   (unsigned long)ordered.count, word);
    intlist_free(&ids);
    intlist_free(&ordered);
    free(word);
}

static void action_find_refs(App *a)
{
    char *word = word_at_cursor(a);
    LocationList locs;
    char title[128];
    if (!word) {
        app_set_status(a, "No identifier under cursor");
        return;
    }
    loclist_init(&locs);
    project_find_refs(&a->project, word, &locs);
    snprintf(title, sizeof(title), "REFERENCES  %s", word);
    view_locations(a, &locs, VIEW_REFS, title);
    a->focus = FOCUS_CONTEXT;
    app_set_status(a, "%lu references to '%s' (saved files)",
                   (unsigned long)locs.count, word);
    loclist_free(&locs);
    free(word);
}

static void action_project_search(App *a, const char *query)
{
    LocationList locs;
    char title[128];
    if (!query || !*query)
        return;
    loclist_init(&locs);
    project_search_text(&a->project, query, &locs);
    snprintf(title, sizeof(title), "TEXT SEARCH  %s", query);
    view_locations(a, &locs, VIEW_TEXT, title);
    a->focus = FOCUS_CONTEXT;
    snprintf(a->last_grep, sizeof(a->last_grep), "%s", query);
    app_set_status(a, "%lu matching lines for '%s'",
                   (unsigned long)locs.count, query);
    loclist_free(&locs);
}

static void action_symbol_search(App *a, const char *query)
{
    IntList ids;
    char title[128];
    if (!query || !*query) {
        app_view_clear(&a->view);
        return;
    }
    intlist_init(&ids);
    project_search_symbols(&a->project, query, &ids);
    snprintf(title, sizeof(title), "SYMBOLS  %s", query);
    view_symbols(a, &ids, VIEW_SYMBOLS, title);
    app_set_status(a, "%lu matching symbols", (unsigned long)ids.count);
    intlist_free(&ids);
}

static char *symbol_code_snippet(Project *p, Symbol *s)
{
    char *src = NULL;
    size_t len = 0;
    char *err = NULL;
    char *out = NULL;
    size_t p0;
    size_t p1;
    int target = s->line;
    if (s->file_index < 0 || (size_t)s->file_index >= p->file_count)
        return xstrdup(s->signature);
    if (!file_read_all(p->files[s->file_index].path, SI_MAX_FILE_SIZE, &src,
                       &len, &err)) {
        free(err);
        return xstrdup(s->signature);
    }
    if (s->kind == SYM_FUNCTION && s->end_line > s->line)
        target = s->line + 1;
    for (;;) {
        p0 = 0;
        {
            int line = 1;
            while (p0 < len && line < target) {
                if (src[p0] == '\n')
                    line++;
                p0++;
            }
        }
        p1 = p0;
        while (p1 < len && src[p1] != '\n')
            p1++;
        while (p0 < p1 && (src[p0] == ' ' || src[p0] == '\t'))
            p0++;
        while (p1 > p0 && (src[p1 - 1] == ' ' || src[p1 - 1] == '\t' ||
                           src[p1 - 1] == '\r'))
            p1--;
        if (target < s->end_line && p1 == p0) {
            target++;
            continue;
        }
        if (target < s->end_line && p1 == p0 + 1 && src[p0] == '{' &&
            s->kind == SYM_FUNCTION) {
            target++;
            continue;
        }
        break;
    }
    if (p1 - p0 > 240)
        p1 = p0 + 240;
    out = xstrndup(src + p0, p1 - p0);
    free(src);
    return out;
}

static void app_chooser_close(App *a)
{
    size_t i;
    for (i = 0; i < a->chooser_count; i++)
        free(a->chooser_items[i].code);
    free(a->chooser_items);
    a->chooser_items = NULL;
    a->chooser_count = 0;
    a->chooser_active = false;
    a->chooser_sel = 0;
    a->chooser_scroll = 0;
}

static int chooser_visible(App *a)
{
    int rows = (a->rows - 8) / 2;
    return rows > 0 ? rows : 1;
}

static void app_chooser_fix(App *a)
{
    int visible = chooser_visible(a);
    if (a->chooser_sel < 0)
        a->chooser_sel = 0;
    if ((size_t)a->chooser_sel >= a->chooser_count)
        a->chooser_sel = a->chooser_count ? (int)a->chooser_count - 1 : 0;
    if (a->chooser_sel < a->chooser_scroll)
        a->chooser_scroll = a->chooser_sel;
    if (a->chooser_sel >= a->chooser_scroll + visible)
        a->chooser_scroll = a->chooser_sel - visible + 1;
    if (a->chooser_scroll < 0)
        a->chooser_scroll = 0;
}

static void app_chooser_open(App *a, const IntList *defs, const char *word)
{
    size_t i;
    app_completion_close(a);
    app_chooser_close(a);
    a->chooser_items = xcalloc(defs->count, sizeof(*a->chooser_items));
    a->chooser_count = defs->count;
    for (i = 0; i < defs->count; i++) {
        Symbol *s = &a->project.syms[defs->items[i]];
        a->chooser_items[i].sym_index = defs->items[i];
        a->chooser_items[i].code = symbol_code_snippet(&a->project, s);
    }
    snprintf(a->chooser_word, sizeof(a->chooser_word), "%s", word);
    a->chooser_active = true;
    a->chooser_sel = 0;
    a->chooser_scroll = 0;
    app_set_status(a, "Choose a definition of '%s' (%lu candidates)", word,
                   (unsigned long)defs->count);
}

static void app_show_context_for(App *a, int best, const char *word);

static void app_chooser_select(App *a)
{
    int best;
    char word[128];
    if (!a->chooser_active || a->chooser_count == 0)
        return;
    best = a->chooser_items[a->chooser_sel].sym_index;
    snprintf(word, sizeof(word), "%s", a->chooser_word);
    app_chooser_close(a);
    a->pending_info_sym = best;
    snprintf(a->pending_info_word, sizeof(a->pending_info_word), "%s", word);
    a->info_line = -1;
    a->info_word[0] = '\0';
    app_update_info(a);
    app_show_context_for(a, best, word);
    a->context_sym = best;
    a->focus = FOCUS_CONTEXT;
    symbol_selection_fix(a);
    app_set_status(a, "Pinned definition to %s:%d",
                   a->project.files[a->project.syms[best].file_index].path,
                   a->project.syms[best].line);
}

static void app_chooser_handle(App *a, KeyEvent *ev)
{
    int visible = chooser_visible(a);
    if (ev->code == KEY_MOUSE) {
        int boxw = SI_MIN(100, a->cols - 2);
        int boxh = SI_MIN(24, a->rows - 2);
        int x = (a->cols - boxw) / 2;
        int y = (a->rows - boxh) / 2;
        int mx = ev->mouse_x;
        int my = ev->mouse_y;
        if (ev->mouse_button == 64 || ev->mouse_button == 65) {
            a->chooser_sel += ev->mouse_button == 64 ? -1 : 1;
            app_chooser_fix(a);
            return;
        }
        if (ev->mouse_release || ev->mouse_button != 0)
            return;
        if (mx <= x || mx >= x + boxw || my <= y || my >= y + boxh) {
            app_chooser_close(a);
            app_set_status(a, "Definition selection cancelled");
            return;
        }
        if (my >= y + 2 && my < y + boxh - 2) {
            int slot = (my - (y + 2)) / 2;
            int idx = a->chooser_scroll + slot;
            if (idx >= 0 && (size_t)idx < a->chooser_count) {
                a->chooser_sel = idx;
                app_chooser_select(a);
            }
        }
        return;
    }
    if (ev->code == KEY_ESC) {
        app_chooser_close(a);
        app_set_status(a, "Definition selection cancelled");
        return;
    }
    if (ev->code == KEY_UP) {
        a->chooser_sel--;
    } else if (ev->code == KEY_DOWN) {
        a->chooser_sel++;
    } else if (ev->code == KEY_PGUP) {
        a->chooser_sel -= visible;
    } else if (ev->code == KEY_PGDN) {
        a->chooser_sel += visible;
    } else if (ev->code == KEY_HOME) {
        a->chooser_sel = 0;
    } else if (ev->code == KEY_END) {
        a->chooser_sel = (int)a->chooser_count - 1;
    } else if (ev->code == KEY_ENTER || ev->code == KEY_TAB) {
        app_chooser_select(a);
        return;
    } else {
        return;
    }
    app_chooser_fix(a);
}

static void app_show_context_for(App *a, int best, const char *word)
{
    LocationList tree;
    Symbol *sym;
    size_t i;
    char title[128];

    if (!project_relations_ready(&a->project)) {
        a->context_need_relations = true;
        a->context_sym = best;
        a->context_scroll = 0;
        app_view_clear(&a->view);
        a->view.kind = VIEW_CONTEXT;
        snprintf(a->view.title, sizeof(a->view.title), "CALL FLOW  %s", word);
        view_add_header(&a->view,
                        "  RELATION INDEX NOT BUILT - press Alt/Opt+R");
        a->sym_sel = 0;
        a->sym_scroll = 0;
        app_set_status(a,
            "Relation index not built: press Alt/Opt+R to build call/variable tables");
        return;
    }
    a->context_need_relations = false;
    if (!project_caller_tree(&a->project, best, &tree, 8)) {
        app_set_status(a, "Cannot build call flow for '%s'", word);
        return;
    }
    sym = &a->project.syms[best];
    a->context_sym = best;
    a->context_scroll = 0;
    app_view_clear(&a->view);
    a->view.kind = VIEW_CONTEXT;
    snprintf(a->view.title, sizeof(a->view.title), "CALL FLOW  %s", word);
    {
        char detail[256];
        ViewItem *root;
        snprintf(detail, sizeof(detail), "%s:%d",
                 a->project.files[sym->file_index].rel, sym->line);
        root = view_add(&a->view, sym->signature, detail, sym->file_index,
                        sym->line, sym->col, best);
        root->depth = 0;
    }
    if (tree.count == 0) {
        view_add_header(&a->view, "  (no callers; entry point or unused)");
    } else {
        snprintf(title, sizeof(title), "CALLERS (%lu)",
                 (unsigned long)tree.count);
        view_add_header(&a->view, title);
        for (i = 0; i < tree.count; i++) {
            Location *loc = &tree.items[i];
            char detail[256];
            ViewItem *item;
            snprintf(detail, sizeof(detail), "%s:%d",
                     a->project.files[loc->file_index].rel, loc->line);
            item = view_add(&a->view, loc->preview, detail, loc->file_index,
                            loc->line, loc->col, -1);
            item->depth = loc->depth;
        }
    }
    a->sym_sel = 0;
    a->sym_scroll = 0;
    symbol_selection_fix(a);
    app_set_status(a, "Call flow %s: %lu caller nodes",
                   word, (unsigned long)tree.count);
    loclist_free(&tree);
}

static void app_show_local_context(App *a, const char *word, int line)
{
    Buffer *b = app_buffer(a);
    LocalVar *lv;
    BufMatch m;
    size_t at = 0;
    size_t wlen = strlen(word);
    size_t uses = 0;
    int fi = app_active_file_index(a);

    if (!b)
        return;
    lv = find_local_var(a, line, word, wlen);
    a->context_need_relations = false;
    a->context_sym = -1;
    a->context_scroll = 0;
    app_view_clear(&a->view);
    a->view.kind = VIEW_CONTEXT;
    snprintf(a->view.title, sizeof(a->view.title), "CALL FLOW  %s", word);
    {
        char detail[256];
        char sig[256];
        ViewItem *root;
        snprintf(sig, sizeof(sig), "%s %s",
                 lv ? lv->type : "local", word);
        snprintf(detail, sizeof(detail), "%s:%d",
                 b->name, lv ? lv->line : line);
        root = view_add(&a->view, sig, detail, fi,
                        lv ? lv->line : line, 0, -1);
        root->depth = 0;
    }
    view_add_header(&a->view, "  LOCAL USES");
    while (buf_find(b, word, at, true, false, &m)) {
        size_t ws;
        size_t wl;
        bool whole = buf_word_at(b, m.pos, &ws, &wl) &&
                     ws == m.pos && wl == wlen;
        if (whole &&
            !(lv && m.line == lv->line)) {
            char detail[256];
            ViewItem *it;
            snprintf(detail, sizeof(detail), "%s:%d", b->name, m.line);
            it = view_add(&a->view, word, detail, fi, m.line, m.col, -1);
            it->depth = 1;
            uses++;
        }
        at = m.pos + (m.len ? m.len : 1);
    }
    if (uses == 0)
        view_add_header(&a->view, "  (no uses in current file)");
    a->sym_sel = 0;
    a->sym_scroll = 0;
    symbol_selection_fix(a);
    app_set_status(a, "Call flow %s: local variable, %lu uses",
                   word, (unsigned long)uses);
}

void app_show_context(App *a)
{
    char *word;
    IntList choice;
    int best = -1;
    bool member_access = false;

    if (a->rw == 0) {
        app_set_status(a, "Terminal too narrow for the context pane");
        return;
    }
    word = word_at_cursor(a);
    if (!word) {
        app_set_status(a, "No identifier under cursor");
        return;
    }
    a->live_context_file = app_active_file_index(a);
    {
        Buffer *b = app_buffer(a);
        a->live_context_line =
            b ? buf_line_of_offset(b, (size_t)b->cursor) + 1 : 0;
    }
    snprintf(a->live_context_word, sizeof(a->live_context_word), "%s", word);
    member_access = cursor_is_member_access(app_buffer(a));
    {
        Buffer *b = app_buffer(a);
        int ln = b ? buf_line_of_offset(b, (size_t)b->cursor) + 1 : 0;
        bool local = b && !member_access &&
                     app_local_in_scope(a, ln, word, strlen(word));
        if (b && !member_access && !local) {
            int dl = 0;
            int dc = 0;
            char lt[256];
            local = find_local_decl(a, b, word, ln, lt, sizeof(lt),
                                    &dl, &dc);
        }
        if (local) {
            app_show_local_context(a, word, ln);
            free(word);
            return;
        }
    }
    intlist_init(&choice);
    best = app_pick_symbol(a, word, member_access, &choice);
    if (choice.count > 1) {
        if (app_completion_suppresses_word(a, word)) {
            best = choice.items[0];
        } else {
            app_chooser_open(a, &choice, word);
            intlist_free(&choice);
            free(word);
            return;
        }
    }
    intlist_free(&choice);
    if (best < 0) {
        a->context_sym = -1;
        app_view_clear(&a->view);
        a->view.kind = VIEW_CONTEXT;
        snprintf(a->view.title, sizeof(a->view.title),
                 "CALL FLOW  %s", word);
        view_add_header(&a->view,
                        "  local/unknown symbol (no project call graph)");
        app_set_status(a, "'%s' is local/unknown; see CODE pane", word);
        free(word);
        return;
    }
    app_show_context_for(a, best, word);
    free(word);
}

void app_sync_callflow(App *a)
{
    Buffer *b;
    char *word;
    int fi;
    int line;

    if (a->focus != FOCUS_EDITOR || a->prompt != PROMPT_NONE ||
        a->help || a->menu_active || a->chooser_active ||
        a->dir_active || a->relation_building ||
        a->relation_ready_prompt || a->relations_large_prompt)
        return;
    b = app_buffer(a);
    if (!b)
        return;
    fi = app_active_file_index(a);
    line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
    word = word_at_cursor(a);
    if (!word || !*word) {
        a->live_context_word[0] = '\0';
        free(word);
        return;
    }
    if (a->live_context_file == fi &&
        a->live_context_line == line &&
        strcmp(a->live_context_word, word) == 0) {
        free(word);
        return;
    }
    a->live_context_file = fi;
    a->live_context_line = line;
    snprintf(a->live_context_word, sizeof(a->live_context_word),
             "%s", word);
    free(word);
    app_show_context(a);
}

static void app_reload_project(App *a)
{
    app_completion_close(a);
    char *root = a->project.root ? xstrdup(a->project.root) : NULL;
    Project np;
    ProgressCtx pctx;
    char *err = NULL;
    if (!root) {
        app_set_status(a, "No project root to reload");
        return;
    }
    app_set_status(a, "Reindexing %s ...", root);
    pctx.root = root;
    if (!project_load(&np, root, index_progress, &pctx, &err)) {
        app_set_status(a, "Reindex failed: %s", err ? err : "unknown error");
        free(err);
        free(root);
        return;
    }
    project_free(&a->project);
    a->project = np;
    project_cache_save(&a->project, NULL);
    a->active_project_file =
        app_buffer(a) ? project_file_index_of_path(&a->project,
                                                   app_buffer(a)->path)
                      : -1;
    app_rebuild_globals(a);
    if (a->file_sel >= (int)a->project.file_count)
        a->file_sel = (int)a->project.file_count - 1;
    if (a->file_sel < 0)
        a->file_sel = 0;
    app_layout(a);
    app_refresh_outline(a);
    app_view_clear(&a->view);
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->locals_buf = -1;
    a->info_word[0] = '\0';
    a->info_line = -1;
    app_update_info(a);
    app_schedule_relations(a);
    app_set_status(a, "Indexed %lu files, %lu symbols",
                   (unsigned long)a->project.file_count,
                   (unsigned long)a->project.sym_count);
    free(root);
}

static void app_switch_buffer(App *a, int delta)
{
    app_completion_close(a);
    completion_suppression_clear(a);
    int i;
    int next;
    if (a->nbuf <= 1) {
        app_set_status(a, "Only one open file");
        return;
    }
    next = a->active;
    for (i = 0; i < a->nbuf; i++) {
        next = (next + delta + a->nbuf) % a->nbuf;
        if (next != a->active)
            break;
    }
    app_clear_selection(a);
    a->active = next;
    app_clear_highlight(a);
    a->active_project_file =
        project_file_index_of_path(&a->project, a->bufs[next]->path);
    a->file_sel = a->active_project_file;
    if (a->file_sel < 0)
        a->file_sel = 0;
    app_layout(a);
    app_refresh_outline(a);
    app_view_clear(&a->view);
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->locals_buf = -1;
    a->info_word[0] = '\0';
    a->info_line = -1;
    app_update_info(a);
    a->focus = FOCUS_EDITOR;
    app_set_status(a, "%s", a->bufs[a->active]->name);
    app_ensure_visible(a);
}

static void outline_selection_fix(App *a)
{
    int count = (int)a->outline.count;
    int h = SI_MAX(1, a->content_height);
    if (count <= 0) {
        a->outline_sel = 0;
        a->outline_scroll = 0;
        return;
    }
    if (a->outline_sel < 0)
        a->outline_sel = 0;
    if (a->outline_sel >= count)
        a->outline_sel = count - 1;
    if (a->outline_sel < a->outline_scroll)
        a->outline_scroll = a->outline_sel;
    if (a->outline_sel >= a->outline_scroll + h)
        a->outline_scroll = a->outline_sel - h + 1;
}

static void sync_outline_to_cursor(App *a)
{
    Buffer *b = app_buffer(a);
    int line;
    int best = 0;
    size_t i;
    if (!b || a->outline.count == 0)
        return;
    line = buf_line_of_offset(b, (size_t)b->cursor) + 1;
    for (i = 0; i < a->outline.count; i++) {
        if (a->outline.items[i].line <= line)
            best = (int)i;
    }
    a->outline_sel = best;
    outline_selection_fix(a);
}

static void focus_outline(App *a)
{
    a->focus = FOCUS_OUTLINE;
    sync_outline_to_cursor(a);
    outline_selection_fix(a);
    app_set_status(a, "File outline: Up/Down select, Enter jumps");
}

static void focus_project(App *a)
{
    int fi;
    if (a->rw == 0) {
        app_set_status(a, "Terminal too narrow for the project pane");
        return;
    }
    fi = app_active_file_index(a);
    a->focus = FOCUS_PROJECT;
    if (fi >= 0)
        a->file_sel = fi;
    file_selection_fix(a);
    app_set_status(a, "Project files: Up/Down select, Enter opens");
}

static void focus_context(App *a)
{
    if (a->rw == 0) {
        app_set_status(a, "Terminal too narrow for the call-flow pane");
        return;
    }
    a->focus = FOCUS_CONTEXT;
    if (a->view.count == 0)
        app_show_context(a);
    symbol_selection_fix(a);
    app_set_status(a, "Call flow: Up/Down select, Enter jumps");
}

static void focus_info(App *a)
{
    a->focus = FOCUS_INFO;
    app_set_status(a, "Symbol info: click a variable or function to inspect");
}

static void app_focus_next(App *a)
{
    switch (a->focus) {
    case FOCUS_OUTLINE:
        a->focus = FOCUS_EDITOR;
        break;
    case FOCUS_EDITOR:
        a->focus = FOCUS_INFO;
        break;
    case FOCUS_INFO:
        a->focus = a->rw > 0 ? FOCUS_PROJECT : FOCUS_OUTLINE;
        break;
    case FOCUS_PROJECT:
        a->focus = a->rw > 0 ? FOCUS_CONTEXT : FOCUS_OUTLINE;
        break;
    case FOCUS_CONTEXT:
    default:
        a->focus = FOCUS_OUTLINE;
        break;
    }
}

static void app_focus_prev(App *a)
{
    switch (a->focus) {
    case FOCUS_OUTLINE:
        a->focus = a->rw > 0 ? FOCUS_CONTEXT : FOCUS_INFO;
        break;
    case FOCUS_EDITOR:
        a->focus = FOCUS_OUTLINE;
        break;
    case FOCUS_INFO:
        a->focus = FOCUS_EDITOR;
        break;
    case FOCUS_PROJECT:
        a->focus = FOCUS_INFO;
        break;
    case FOCUS_CONTEXT:
    default:
        a->focus = FOCUS_PROJECT;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ANSI-style formatter                                                */
/* ------------------------------------------------------------------ */

static bool fmt_control_prefix(const char *s)
{
    static const char *const words[] = {
        "if", "for", "while", "switch", "else", "do", "try", "catch",
        "struct", "union", "enum", "class", "namespace", "extern"
    };
    size_t i;
    while (*s == ' ' || *s == '\t')
        s++;
    for (i = 0; i < SI_ARRAY_LEN(words); i++) {
        size_t n = strlen(words[i]);
        if (strncmp(s, words[i], n) == 0 &&
            (s[n] == '\0' || s[n] == '(' || s[n] == ' ' || s[n] == '\t'))
            return true;
    }
    return false;
}

static void fmt_scan_line(const char *s, size_t n, bool *in_block,
                          int *opens, int *closes, bool *starts_close)
{
    size_t i = 0;
    bool first = true;
    *opens = 0;
    *closes = 0;
    *starts_close = false;
    while (i < n) {
        if (*in_block) {
            if (i + 1 < n && s[i] == '*' && s[i + 1] == '/') {
                *in_block = false;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/')
            break;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            *in_block = true;
            i += 2;
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i++];
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (s[i] == quote) {
                    i++;
                    break;
                }
                i++;
            }
            first = false;
            continue;
        }
        if (first && s[i] != ' ' && s[i] != '\t') {
            *starts_close = s[i] == '}';
            first = false;
        }
        if (s[i] == '{')
            (*opens)++;
        else if (s[i] == '}')
            (*closes)++;
        i++;
    }
}

static bool fmt_split_function_brace(const char *s, size_t n, size_t *prefix)
{
    size_t end;
    if (n == 0 || s[n - 1] != '{')
        return false;
    end = n - 1;
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        end--;
    if (end == 0 || fmt_control_prefix(s))
        return false;
    if (!memchr(s, ')', end))
        return false;
    *prefix = end;
    return true;
}

static char *format_ansi_source(const char *src, size_t len)
{
    Str out;
    Str line;
    size_t pos = 0;
    int depth = 0;
    bool in_block = false;
    bool in_macro = false;
    bool last_blank = false;

    str_init(&out);
    str_init(&line);
    while (pos < len) {
        size_t end = pos;
        size_t a;
        size_t b;
        int opens = 0;
        int closes = 0;
        bool starts_close = false;
        int out_indent;
        size_t prefix = 0;
        int col = 0;

        while (end < len && src[end] != '\n')
            end++;
        str_clear(&line);
        a = pos;
        while (a < end) {
            if (src[a] == '\t') {
                int spaces = SI_TAB_WIDTH - (col % SI_TAB_WIDTH);
                while (spaces-- > 0) {
                    str_putc(&line, ' ');
                    col++;
                }
                a++;
            } else if (src[a] == '\r') {
                a++;
            } else {
                str_putc(&line, src[a++]);
                col++;
            }
        }
        a = 0;
        b = line.len;
        while (a < b && (line.data[a] == ' ' || line.data[a] == '\t'))
            a++;
        while (b > a && (line.data[b - 1] == ' ' || line.data[b - 1] == '\t'))
            b--;

        if (in_macro) {
            str_append(&out, line.data + a, b - a);
            str_putc(&out, '\n');
            in_macro = b > a && line.data[b - 1] == '\\';
            pos = end < len ? end + 1 : end;
            last_blank = false;
            continue;
        }
        if (b > a && line.data[a] == '#') {
            str_append(&out, line.data + a, b - a);
            str_putc(&out, '\n');
            in_macro = line.data[b - 1] == '\\';
            pos = end < len ? end + 1 : end;
            last_blank = false;
            continue;
        }
        if (b == a) {
            if (out.len > 0 && !last_blank) {
                str_putc(&out, '\n');
                last_blank = true;
            }
            pos = end < len ? end + 1 : end;
            continue;
        }

        fmt_scan_line(line.data + a, b - a, &in_block, &opens, &closes,
                      &starts_close);
        out_indent = depth;
        if (starts_close && out_indent > 0)
            out_indent--;
        if (fmt_split_function_brace(line.data + a, b - a, &prefix)) {
            size_t k;
            for (k = 0; k < (size_t)out_indent * SI_TAB_WIDTH; k++)
                str_putc(&out, ' ');
            str_append(&out, line.data + a, prefix);
            str_putc(&out, '\n');
            for (k = 0; k < (size_t)out_indent * SI_TAB_WIDTH; k++)
                str_putc(&out, ' ');
            str_putc(&out, '{');
            str_putc(&out, '\n');
            depth = out_indent + 1;
            pos = end < len ? end + 1 : end;
            last_blank = false;
            continue;
        }
        {
            int k;
            for (k = 0; k < out_indent * SI_TAB_WIDTH; k++)
                str_putc(&out, ' ');
        }
        str_append(&out, line.data + a, b - a);
        str_putc(&out, '\n');
        depth += opens - closes;
        if (depth < 0)
            depth = 0;
        last_blank = false;
        pos = end < len ? end + 1 : end;
    }
    if (out.len == 0)
        str_putc(&out, '\n');
    str_free(&line);
    return out.data;
}

static bool format_current_buffer(App *a)
{
    Buffer *b = app_buffer(a);
    char *formatted;
    int line;
    int col;
    if (!b) {
        app_set_status(a, "No file to format");
        return false;
    }
    line = buf_line_of_offset(b, (size_t)b->cursor);
    col = b->cursor - (int)buf_line_start(b, line);
    formatted = format_ansi_source(b->data, b->len);
    if (formatted && strcmp(formatted, b->data) != 0) {
        size_t n = strlen(formatted);
        if (!buf_delete_range(b, 0, b->len) ||
            !buf_insert_at(b, 0, formatted, n)) {
            free(formatted);
            app_set_status(a, "Format failed");
            return false;
        }
        b->cursor = 0;
        set_cursor_line_col(a, b, line + 1, col);
        app_set_status(a, "Formatted %s (ANSI style)", b->name);
        free(formatted);
        return true;
    }
    free(formatted);
    app_set_status(a, "%s: already formatted", b->name);
    return true;
}

static bool ensure_dir(const char *path)
{
    char tmp[1024];
    size_t n;
    size_t i;
    if (!path || !*path)
        return false;
    n = strlen(path);
    if (n >= sizeof(tmp))
        return false;
    memcpy(tmp, path, n + 1);
    path_to_slashes(tmp);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' && tmp[i - 1] != ':') {
            char save = tmp[i];
            tmp[i] = '\0';
            if (si_mkdir(tmp) != 0 && errno != EEXIST)
                return false;
            tmp[i] = save;
        }
    }
    if (si_mkdir(tmp) != 0 && errno != EEXIST)
        return false;
    return true;
}

static bool create_project_file(const char *root)
{
    char *path = path_join(root, "main.c");
    static const char skeleton[] =
        "#include <stdio.h>\n\n"
        "int main(void)\n"
        "{\n"
        "    printf(\"Hello, world!\\n\");\n"
        "    return 0;\n"
        "}\n";
    char *err = NULL;
    bool ok = true;
    if (!path_exists(path))
        ok = file_write_atomic(path, skeleton, strlen(skeleton), &err);
    free(err);
    free(path);
    return ok;
}

static void app_set_home(App *a)
{
    Buffer *b = app_buffer(a);
    nav_free(&a->home);
    a->home_valid = false;
    if (!b || !b->path)
        return;
    a->home.path = xstrdup(b->path);
    a->home.cursor = b->cursor;
    a->home.scroll_line = b->scroll_line;
    a->home.scroll_col = b->scroll_col;
    a->home_valid = true;
}

static void app_recent_touch(App *a, const char *root)
{
    char *abs;
    size_t i;

    if (!root || !*root)
        return;
    abs = path_abs(root);
    if (!abs || !*abs) {
        free(abs);
        return;
    }
    for (i = 0; i < a->recent_projects.count; i++) {
        if (strcmp(a->recent_projects.items[i], abs) == 0) {
            free(a->recent_projects.items[i]);
            memmove(&a->recent_projects.items[i],
                    &a->recent_projects.items[i + 1],
                    (a->recent_projects.count - i - 1) * sizeof(char *));
            a->recent_projects.count--;
            break;
        }
    }
    if (a->recent_projects.count == a->recent_projects.cap) {
        a->recent_projects.cap = a->recent_projects.cap
                                     ? a->recent_projects.cap * 2
                                     : 8;
        a->recent_projects.items =
            xrealloc(a->recent_projects.items,
                     a->recent_projects.cap * sizeof(char *));
    }
    memmove(&a->recent_projects.items[1], &a->recent_projects.items[0],
            a->recent_projects.count * sizeof(char *));
    a->recent_projects.items[0] = abs;
    a->recent_projects.count++;
}

static void app_save_state(App *a)
{
    Buffer *b = app_buffer(a);
    state_save(a->project.root, b ? b->path : NULL, b ? b->cursor : 0,
               b ? b->scroll_line : 0, b ? b->scroll_col : 0,
               &a->recent_projects);
}

static bool app_change_project(App *a, const char *root, bool create)
{
    Project np;
    ProgressCtx pctx;
    char *err = NULL;
    bool from_cache = false;
    int i;
    int open_fi = -1;
    if (!root || !*root) {
        app_set_status(a, "Empty project path");
        return false;
    }
    if (any_dirty(a)) {
        app_set_status(a, "Save all files before switching project");
        return false;
    }
    if (create) {
        if (!ensure_dir(root)) {
            app_set_status(a, "Cannot create directory: %s", root);
            return false;
        }
        if (!create_project_file(root)) {
            app_set_status(a, "Cannot create main.c in %s", root);
            return false;
        }
    }
    pctx.root = root;
    if (!project_load_cached(&np, root, index_progress, &pctx, &err,
                             &from_cache)) {
        app_set_status(a, "Open project failed: %s",
                       err ? err : "unknown error");
        free(err);
        return false;
    }
    {
        KeyEvent junk;
        while (term_poll_key(&junk) > 0)
            ;
    }
    app_chooser_close(a);
    app_clear_selection(a);
    a->context_sym = -1;
    a->live_context_file = -1;
    a->live_context_line = -1;
    a->live_context_word[0] = '\0';
    a->pending_info_sym = -1;
    a->pending_info_word[0] = '\0';
    a->pin_context = false;
    app_clear_highlight(a);
    for (i = 0; i < a->nbuf; i++)
        buf_free(a->bufs[i]);
    free(a->bufs);
    a->bufs = NULL;
    a->nbuf = 0;
    a->capbuf = 0;
    a->active = -1;
    a->active_project_file = -1;
    buf_free(a->info_buffer);
    a->info_buffer = NULL;
    for (i = 0; i < (int)a->nlocals; i++) {
        free(a->locals[i].name);
        free(a->locals[i].type);
    }
    a->nlocals = 0;
    free(a->locals);
    a->locals = NULL;
    a->clocals = 0;
    app_view_clear(&a->view);
    free(a->view.items);
    a->view.items = NULL;
    a->view.cap = 0;
    free(a->call_boxes);
    a->call_boxes = NULL;
    a->call_box_count = 0;
    a->call_box_cap = 0;
    app_view_clear(&a->outline);
    free(a->outline.items);
    a->outline.items = NULL;
    a->outline.cap = 0;
    for (i = 0; i < (int)a->nback; i++)
        nav_free(&a->back[i]);
    free(a->back);
    a->back = NULL;
    a->nback = a->cback = 0;
    for (i = 0; i < (int)a->nfwd; i++)
        nav_free(&a->fwd[i]);
    free(a->fwd);
    a->fwd = NULL;
    a->nfwd = a->cfwd = 0;
    nav_free(&a->home);
    a->home_valid = false;
    project_free(&a->project);
    a->project = np;
    if (!from_cache)
        project_cache_save(&a->project, NULL);
    app_rebuild_globals(a);
    a->file_sel = 0;
    a->file_scroll = 0;
    a->sym_sel = 0;
    app_clear_highlight(a);
    a->sym_scroll = 0;
    a->outline_sel = 0;
    a->outline_scroll = 0;
    a->info_word[0] = '\0';
    a->info_line = -1;
    a->locals_buf = -1;
    a->focus = FOCUS_EDITOR;
    app_layout(a);
    for (i = 0; i < (int)a->project.file_count; i++) {
        if (strcmp(a->project.files[i].rel, "main.c") == 0) {
            open_fi = i;
            break;
        }
    }
    if (open_fi < 0 && a->project.file_count > 0)
        open_fi = 0;
    if (open_fi >= 0)
        app_open_file_index(a, open_fi, 1, 0, false);
    else {
        app_view_clear(&a->view);
        app_refresh_outline(a);
    }
    app_set_home(a);
    app_schedule_relations(a);
    app_recent_touch(a, a->project.root ? a->project.root : root);
    app_project_picker_close(a);
    app_save_state(a);
    app_set_status(a, "Project %s: %lu files, %lu symbols",
                   a->project.root ? a->project.root : root,
                   (unsigned long)a->project.file_count,
                   (unsigned long)a->project.sym_count);
    return true;
}

static void format_all_project_files(App *a)
{
    char *root = a->project.root ? xstrdup(a->project.root) : NULL;
    size_t i;
    int changed = 0;
    int failed = 0;
    if (!root) {
        app_set_status(a, "No project root");
        return;
    }
    if (any_dirty(a)) {
        app_set_status(a, "Save all files before formatting all");
        free(root);
        return;
    }
    for (i = 0; i < a->project.file_count; i++) {
        char *src = NULL;
        size_t len = 0;
        char *err = NULL;
        char *formatted;
        if (!file_read_all(a->project.files[i].path, SI_MAX_FILE_SIZE,
                           &src, &len, &err)) {
            free(err);
            failed++;
            continue;
        }
        formatted = format_ansi_source(src, len);
        if (formatted && (strlen(formatted) != len ||
                          memcmp(formatted, src, len) != 0)) {
            char *werr = NULL;
            if (!file_write_atomic(a->project.files[i].path, formatted,
                                   strlen(formatted), &werr)) {
                free(werr);
                failed++;
            } else {
                changed++;
            }
        }
        free(formatted);
        free(src);
    }
    free(root);
    if (failed) {
        app_set_status(a, "Formatted %d files, %d failed", changed, failed);
    } else {
        app_set_status(a, "Formatted %d project files (ANSI style)", changed);
    }
    if (changed) {
        char *again = a->project.root ? xstrdup(a->project.root) : NULL;
        app_change_project(a, again, false);
        free(again);
    }
}

/* ------------------------------------------------------------------ */
/* Directory picker                                                    */
/* ------------------------------------------------------------------ */

static int dir_picker_visible(App *a)
{
    int boxh = SI_MIN(24, a->rows - 4);
    int rows = boxh - 6;
    return rows > 1 ? rows : 1;
}

static void dir_picker_fix(App *a)
{
    int count = (int)a->dir_count + 2;
    int visible = dir_picker_visible(a);
    if (a->dir_sel < 0)
        a->dir_sel = 0;
    if (a->dir_sel >= count)
        a->dir_sel = count > 0 ? count - 1 : 0;
    if (a->dir_sel < a->dir_scroll)
        a->dir_scroll = a->dir_sel;
    if (a->dir_sel >= a->dir_scroll + visible)
        a->dir_scroll = a->dir_sel - visible + 1;
    if (a->dir_scroll < 0)
        a->dir_scroll = 0;
}

static void dir_picker_close(App *a)
{
    size_t i;
    for (i = 0; i < a->dir_count; i++)
        free(a->dir_items[i]);
    free(a->dir_items);
    a->dir_items = NULL;
    a->dir_count = 0;
    a->dir_active = false;
    a->dir_sel = 0;
    a->dir_scroll = 0;
    term_mouse_hover(true);
}

static void dir_picker_load(App *a, const char *path)
{
    DirIter *it;
    DirEntry ent;
    StrList names;
    char resolved[1024];
    size_t i;

    if (!path || !*path)
        path = a->project.root ? a->project.root : ".";
    snprintf(resolved, sizeof(resolved), "%s", path);
    path_to_slashes(resolved);
    strlist_init(&names);
    it = dir_open(resolved);
    if (!it) {
        app_set_status(a, "Cannot open directory: %s", resolved);
        return;
    }
    while (dir_next(it, &ent)) {
        if (!ent.is_dir)
            continue;
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
            continue;
        strlist_push(&names, ent.name);
    }
    dir_close(it);
    strlist_sort(&names);
    for (i = 0; i < a->dir_count; i++)
        free(a->dir_items[i]);
    free(a->dir_items);
    a->dir_items = names.items;
    a->dir_count = names.count;
    a->dir_sel = 0;
    a->dir_scroll = 0;
    snprintf(a->dir_path, sizeof(a->dir_path), "%s", resolved);
    a->dir_active = true;
    term_mouse_hover(false);
    dir_picker_fix(a);
    app_set_status(a, "%s: choose a folder",
                   a->dir_for_new ? "New project parent" : "Open project");
}

static void dir_picker_open(App *a, bool for_new)
{
    a->dir_for_new = for_new;
    dir_picker_load(a, a->project.root ? a->project.root : ".");
}

static void dir_picker_choose(App *a)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s", a->dir_path);
    if (a->dir_for_new) {
        snprintf(a->pending_parent, sizeof(a->pending_parent), "%s", path);
        dir_picker_close(a);
        app_prompt_begin(a, PROMPT_PROJECT_NAME, "");
    } else {
        dir_picker_close(a);
        app_change_project(a, path, false);
    }
}

static void dir_picker_parent(App *a)
{
    char *parent = path_dirname(a->dir_path);
    if (!parent) {
        return;
    }
    if (strcmp(parent, a->dir_path) == 0) {
        free(parent);
        return;
    }
    dir_picker_load(a, parent);
    free(parent);
}

static void dir_picker_enter(App *a)
{
    if (a->dir_sel == 0) {
        dir_picker_choose(a);
    } else if (a->dir_sel == 1) {
        dir_picker_parent(a);
    } else {
        size_t idx = (size_t)(a->dir_sel - 2);
        char *path;
        if (idx >= a->dir_count)
            return;
        path = path_join(a->dir_path, a->dir_items[idx]);
        dir_picker_load(a, path);
        free(path);
    }
}

static void dir_picker_handle(App *a, KeyEvent *ev)
{
    int visible = dir_picker_visible(a);
    int count = (int)a->dir_count + 2;
    if (ev->code == KEY_MOUSE) {
        int boxw = SI_MIN(76, a->cols - 4);
        int boxh = SI_MIN(24, a->rows - 4);
        int x = (a->cols - boxw) / 2;
        int y = (a->rows - boxh) / 2;
        int mx = ev->mouse_x;
        int my = ev->mouse_y;
        if (ev->mouse_button == 64 || ev->mouse_button == 65) {
            a->dir_sel += ev->mouse_button == 64 ? -1 : 1;
            dir_picker_fix(a);
            return;
        }
        if (ev->mouse_release || ev->mouse_button != 0)
            return;
        if (mx <= x || mx >= x + boxw || my <= y || my >= y + boxh) {
            dir_picker_close(a);
            return;
        }
        if (my >= y + 4 && my < y + boxh - 2) {
            int slot = my - (y + 4);
            int idx = a->dir_scroll + slot;
            if (idx >= 0 && idx < count) {
                a->dir_sel = idx;
                dir_picker_enter(a);
            }
        }
        return;
    }
    if (ev->code == KEY_ESC) {
        dir_picker_close(a);
        app_set_status(a, "Project selection cancelled");
        return;
    }
    if (ev->code == KEY_UP) {
        a->dir_sel--;
    } else if (ev->code == KEY_DOWN) {
        a->dir_sel++;
    } else if (ev->code == KEY_PGUP) {
        a->dir_sel -= visible;
    } else if (ev->code == KEY_PGDN) {
        a->dir_sel += visible;
    } else if (ev->code == KEY_HOME) {
        a->dir_sel = 0;
    } else if (ev->code == KEY_END) {
        a->dir_sel = count - 1;
    } else if (ev->code == KEY_ENTER) {
        dir_picker_enter(a);
        return;
    } else if (ev->code == KEY_BACKSPACE || ev->code == KEY_LEFT) {
        dir_picker_parent(a);
        return;
    } else {
        return;
    }
    dir_picker_fix(a);
}

/* ------------------------------------------------------------------ */
/* Recent project picker                                               */
/* ------------------------------------------------------------------ */

static int app_project_picker_visible(App *a)
{
    int boxh = SI_MIN(APP_PROJECT_PICKER_MAX_H, a->rows - 4);
    int rows = boxh - 5;
    return rows > 1 ? rows : 1;
}

static void app_project_picker_fix(App *a)
{
    int count = (int)a->recent_projects.count;
    int visible = app_project_picker_visible(a);
    if (a->project_sel < 0)
        a->project_sel = 0;
    if (a->project_sel >= count)
        a->project_sel = count > 0 ? count - 1 : 0;
    if (a->project_sel < a->project_scroll)
        a->project_scroll = a->project_sel;
    if (a->project_sel >= a->project_scroll + visible)
        a->project_scroll = a->project_sel - visible + 1;
    if (a->project_scroll > count - visible)
        a->project_scroll = count - visible;
    if (a->project_scroll < 0 || count == 0)
        a->project_scroll = 0;
}

static void app_project_picker_close(App *a)
{
    if (!a->project_picker_active)
        return;
    a->project_picker_active = false;
    a->project_sel = 0;
    a->project_scroll = 0;
    term_mouse_hover(true);
}

void app_project_picker_open(App *a)
{
    a->project_picker_active = true;
    a->project_sel = 0;
    a->project_scroll = 0;
    app_project_picker_fix(a);
    term_mouse_hover(false);
    if (a->recent_projects.count == 0) {
        app_set_status(a,
            "No project open. Ctrl+O open folder, Ctrl+N new project");
    } else {
        app_set_status(a,
            "No project open. Up/Down select, Enter open, Ctrl+O / Ctrl+N");
    }
}

static void app_project_picker_enter(App *a)
{
    const char *path;

    if (a->project_sel < 0 ||
        (size_t)a->project_sel >= a->recent_projects.count) {
        app_set_status(a, "No recent project selected");
        return;
    }
    path = a->recent_projects.items[a->project_sel];
    if (!path_is_dir(path)) {
        app_set_status(a, "Project folder not found: %s", path);
        return;
    }
    app_change_project(a, path, false);
}

static void app_project_picker_handle(App *a, KeyEvent *ev)
{
    int visible = app_project_picker_visible(a);
    int count = (int)a->recent_projects.count;

    if (ev->code == KEY_MOUSE) {
        int boxw = SI_MIN(APP_PROJECT_PICKER_MAX_W, a->cols - 4);
        int boxh = SI_MIN(APP_PROJECT_PICKER_MAX_H, a->rows - 4);
        int x = (a->cols - boxw) / 2;
        int y = (a->rows - boxh) / 2;
        int mx = ev->mouse_x;
        int my = ev->mouse_y;

        if (boxw < 40 || boxh < 8)
            return;
        if (ev->mouse_button == 64 || ev->mouse_button == 65) {
            a->project_sel += ev->mouse_button == 64 ? -1 : 1;
            app_project_picker_fix(a);
            return;
        }
        if (ev->mouse_release || ev->mouse_button != 0)
            return;
        if (mx <= x || mx >= x + boxw || my <= y || my >= y + boxh) {
            app_project_picker_close(a);
            app_set_status(a,
                "No project open. Ctrl+O open folder, Ctrl+N new project");
            return;
        }
        if (my >= y + 3 && my < y + boxh - 2) {
            int idx = a->project_scroll + (my - (y + 3));
            if (idx >= 0 && idx < count) {
                a->project_sel = idx;
                app_project_picker_enter(a);
            }
        }
        return;
    }
    if (ev->code == KEY_ESC) {
        app_project_picker_close(a);
        app_set_status(a,
            "No project open. Ctrl+O open folder, Ctrl+N new project");
        return;
    }
    if (ev->code == 17) { /* Ctrl+Q */
        app_request_quit(a);
        return;
    }
    if (ev->code == 15) { /* Ctrl+O */
        dir_picker_open(a, false);
        return;
    }
    if (ev->code == 14) { /* Ctrl+N */
        dir_picker_open(a, true);
        return;
    }
    if (ev->code == KEY_F1 || ev->code == 8) { /* F1 / Ctrl+H */
        a->help = true;
        return;
    }
    if (ev->code == KEY_ALT_CHAR && ev->len > 0 &&
        (ev->text[0] == 'm' || ev->text[0] == 'M')) {
        app_menu_open(a);
        return;
    }
    if (ev->code == KEY_UP) {
        a->project_sel--;
    } else if (ev->code == KEY_DOWN) {
        a->project_sel++;
    } else if (ev->code == KEY_PGUP) {
        a->project_sel -= visible;
    } else if (ev->code == KEY_PGDN) {
        a->project_sel += visible;
    } else if (ev->code == KEY_HOME) {
        a->project_sel = 0;
    } else if (ev->code == KEY_END) {
        a->project_sel = count - 1;
    } else if (ev->code == KEY_ENTER) {
        app_project_picker_enter(a);
        return;
    } else {
        return;
    }
    app_project_picker_fix(a);
}

/* ------------------------------------------------------------------ */
/* Prompts                                                             */
/* ------------------------------------------------------------------ */

static void prompt_set(App *a, const char *text)
{
    size_t n = text ? strlen(text) : 0;
    if (n > sizeof(a->prompt_text) - 1)
        n = sizeof(a->prompt_text) - 1;
    memcpy(a->prompt_text, text ? text : "", n);
    a->prompt_text[n] = '\0';
    a->prompt_len = (int)n;
    a->prompt_cursor = (int)n;
}

static void app_prompt_begin(App *a, PromptKind kind, const char *initial)
{
    app_completion_close(a);
    a->prompt_prev_focus = a->focus;
    a->prompt = kind;
    prompt_set(a, initial);
    if (kind == PROMPT_SYMBOL) {
        a->focus = FOCUS_CONTEXT;
        app_view_clear(&a->view);
        a->view.kind = VIEW_SYMBOLS;
        snprintf(a->view.title, sizeof(a->view.title), "SYMBOLS");
    } else if (kind == PROMPT_FIND || kind == PROMPT_GREP) {
        a->focus = FOCUS_EDITOR;
    }
}

static void app_prompt_cancel(App *a)
{
    PromptKind kind = a->prompt;
    Focus prev = a->prompt_prev_focus;
    a->prompt = PROMPT_NONE;
    if (kind == PROMPT_SYMBOL || kind == PROMPT_GREP)
        app_refresh_outline(a);
    a->focus = prev;
    app_set_status(a, "Cancelled");
}

static void prompt_insert(App *a, const char *text, int len)
{
    char digits[64];
    if (len <= 0)
        return;
    if (a->prompt == PROMPT_GOTO) {
        int i;
        int n = 0;
        for (i = 0; i < len && n < (int)sizeof(digits) - 1; i++) {
            if (text[i] >= '0' && text[i] <= '9')
                digits[n++] = text[i];
        }
        if (n == 0)
            return;
        text = digits;
        len = n;
    }
    if (a->prompt_len + len >= (int)sizeof(a->prompt_text))
        return;
    memmove(a->prompt_text + a->prompt_cursor + len,
            a->prompt_text + a->prompt_cursor,
            (size_t)(a->prompt_len - a->prompt_cursor));
    memcpy(a->prompt_text + a->prompt_cursor, text, (size_t)len);
    a->prompt_len += len;
    a->prompt_cursor += len;
    a->prompt_text[a->prompt_len] = '\0';
}

static void prompt_backspace(App *a)
{
    size_t prev;
    if (a->prompt_cursor <= 0)
        return;
    prev = utf8_prev(a->prompt_text, (size_t)a->prompt_cursor);
    memmove(a->prompt_text + prev, a->prompt_text + a->prompt_cursor,
            (size_t)(a->prompt_len - a->prompt_cursor));
    a->prompt_len -= a->prompt_cursor - (int)prev;
    a->prompt_cursor = (int)prev;
    a->prompt_text[a->prompt_len] = '\0';
}

static void prompt_delete(App *a)
{
    size_t next;
    if (a->prompt_cursor >= a->prompt_len)
        return;
    next = (size_t)a->prompt_cursor + 1;
    while (next < (size_t)a->prompt_len &&
           (((unsigned char)a->prompt_text[next] & 0xC0) == 0x80))
        next++;
    memmove(a->prompt_text + a->prompt_cursor,
            a->prompt_text + next,
            (size_t)(a->prompt_len - (int)next));
    a->prompt_len -= (int)(next - (size_t)a->prompt_cursor);
    a->prompt_text[a->prompt_len] = '\0';
}

static void app_prompt_execute(App *a)
{
    char query[512];
    PromptKind kind = a->prompt;
    snprintf(query, sizeof(query), "%s", a->prompt_text);
    a->prompt = PROMPT_NONE;

    switch (kind) {
    case PROMPT_FIND:
        a->focus = FOCUS_EDITOR;
        if (query[0])
            action_find(a, query, true);
        else
            app_set_status(a, "Empty search");
        break;
    case PROMPT_GREP:
        action_project_search(a, query);
        if (a->view.count > 0) {
            a->focus = FOCUS_CONTEXT;
        } else {
            app_refresh_outline(a);
            a->focus = a->prompt_prev_focus;
        }
        break;
    case PROMPT_SYMBOL:
        if (a->view.count == 1) {
            ViewItem *it = &a->view.items[0];
            app_goto_location(a, it->file_index, it->line, it->col, true);
        } else if (a->view.count > 1) {
            a->focus = FOCUS_CONTEXT;
            app_set_status(a, "%lu symbols - Enter to jump",
                           (unsigned long)a->view.count);
        } else {
            a->focus = a->prompt_prev_focus;
            app_set_status(a, "No matching symbol");
        }
        break;
    case PROMPT_OPEN:
        if (query[0])
            app_open_path(a, query, 1, true);
        else
            a->focus = a->prompt_prev_focus;
        break;
    case PROMPT_GOTO: {
        char *end = NULL;
        long line;
        if (!query[0]) {
            a->focus = FOCUS_EDITOR;
            break;
        }
        errno = 0;
        line = strtol(query, &end, 10);
        if (errno || end == query || *end != '\0' || line < 1)
            app_set_status(a, "Invalid line number (enter digits only)");
        else
            action_goto_line(a, (int)line);
        a->focus = FOCUS_EDITOR;
        break;
    }
    case PROMPT_PROJECT:
        if (query[0]) {
            app_change_project(a, query, false);
        } else {
            a->focus = FOCUS_EDITOR;
            app_set_status(a, "Empty project path");
        }
        break;
    case PROMPT_NEW_PROJECT:
        if (query[0]) {
            app_change_project(a, query, true);
        } else {
            a->focus = FOCUS_EDITOR;
            app_set_status(a, "Empty project path");
        }
        break;
    case PROMPT_PROJECT_NAME:
        if (query[0] && strchr(query, '/') == NULL &&
            strchr(query, '\\') == NULL &&
            strcmp(query, ".") != 0 && strcmp(query, "..") != 0) {
            char *path = path_join(a->pending_parent, query);
            app_change_project(a, path, true);
            free(path);
        } else if (query[0]) {
            app_set_status(a, "Invalid project folder name");
            a->focus = FOCUS_EDITOR;
        } else {
            app_set_status(a, "Empty project name");
            a->focus = FOCUS_EDITOR;
        }
        break;
    default:
        break;
    }
}

static void app_prompt_handle(App *a, KeyEvent *ev)
{
    bool changed = false;

    if (a->prompt == PROMPT_QUIT) {
        int ch = ev->code == KEY_CHAR && ev->len > 0 ? ev->text[0] : ev->code;
        if (ch == 'y' || ch == 'Y') {
            if (save_all(a)) {
                a->prompt = PROMPT_NONE;
                a->quit = true;
            } else {
                a->prompt = PROMPT_NONE;
            }
        } else if (ch == 'n' || ch == 'N') {
            a->prompt = PROMPT_NONE;
            a->quit = true;
        } else if (ch == 'c' || ch == 'C' || ev->code == KEY_ESC) {
            a->prompt = PROMPT_NONE;
            app_set_status(a, "Quit cancelled");
        }
        return;
    }
    if (a->prompt == PROMPT_FORMAT) {
        int ch = ev->code == KEY_CHAR && ev->len > 0 ? ev->text[0]
                                                      : ev->code;
        a->prompt = PROMPT_NONE;
        if (ev->code == KEY_ESC) {
            a->focus = FOCUS_EDITOR;
            app_set_status(a, "Format cancelled");
        } else if (ch == 'a' || ch == 'A') {
            a->focus = FOCUS_EDITOR;
            format_all_project_files(a);
        } else if (ch == 'c' || ch == 'C' || ch == 'f' || ch == 'F' ||
                   ev->code == KEY_ENTER) {
            a->focus = FOCUS_EDITOR;
            format_current_buffer(a);
        } else {
            a->focus = a->prompt_prev_focus;
            app_set_status(a, "Format cancelled");
        }
        return;
    }
    if (ev->code == KEY_ESC) {
        app_prompt_cancel(a);
        return;
    }
    if (ev->code == KEY_ENTER) {
        app_prompt_execute(a);
        return;
    }
    if (ev->code == KEY_BACKSPACE) {
        prompt_backspace(a);
        changed = true;
    } else if (ev->code == KEY_DELETE) {
        prompt_delete(a);
        changed = true;
    } else if (ev->code == KEY_LEFT) {
        if (a->prompt_cursor > 0)
            a->prompt_cursor = (int)utf8_prev(a->prompt_text,
                                              (size_t)a->prompt_cursor);
    } else if (ev->code == KEY_RIGHT) {
        if (a->prompt_cursor < a->prompt_len) {
            size_t next = (size_t)a->prompt_cursor + 1;
            while (next < (size_t)a->prompt_len &&
                   (((unsigned char)a->prompt_text[next] & 0xC0) == 0x80))
                next++;
            a->prompt_cursor = (int)next;
        }
    } else if (ev->code == KEY_HOME) {
        a->prompt_cursor = 0;
    } else if (ev->code == KEY_END) {
        a->prompt_cursor = a->prompt_len;
    } else if (ev->code == KEY_UP || ev->code == KEY_DOWN) {
        if (a->prompt == PROMPT_SYMBOL && a->view.count > 0) {
            if (ev->code == KEY_UP && a->sym_sel > 0)
                a->sym_sel--;
            if (ev->code == KEY_DOWN &&
                (size_t)(a->sym_sel + 1) < a->view.count)
                a->sym_sel++;
        }
    } else if (ev->code == KEY_CHAR && ev->len > 0) {
        prompt_insert(a, ev->text, ev->len);
        changed = true;
    } else if (ev->code == 21) { /* Ctrl+U */
        a->prompt_len = 0;
        a->prompt_cursor = 0;
        a->prompt_text[0] = '\0';
        changed = true;
    }
    if (a->prompt == PROMPT_SYMBOL && changed)
        action_symbol_search(a, a->prompt_text);
}

/* ------------------------------------------------------------------ */
/* Key handling                                                        */
/* ------------------------------------------------------------------ */

static void move_page(App *a, int dir)
{
    move_vertical(a, dir * SI_MAX(1, a->ed_h - 1));
}

static void app_request_quit(App *a)
{
    app_completion_close(a);
    if (any_dirty(a)) {
        a->prompt = PROMPT_QUIT;
        app_set_status(a, "Unsaved changes: y = save all, n = discard, c = cancel");
    } else {
        a->quit = true;
    }
}

enum {
    MENU_OPEN_PROJECT = 0,
    MENU_NEW_PROJECT,
    MENU_BACK,
    MENU_FORWARD,
    MENU_FORMAT,
    MENU_SAVE,
    MENU_SAVE_ALL,
    MENU_HELP,
    MENU_QUIT,
    MENU_COUNT
};

static const int menu_group_actions[APP_MENU_GROUPS][4] = {
    { MENU_OPEN_PROJECT, MENU_NEW_PROJECT, MENU_SAVE, MENU_SAVE_ALL },
    { MENU_BACK, MENU_FORWARD, -1, -1 },
    { MENU_FORMAT, -1, -1, -1 },
    { MENU_HELP, MENU_QUIT, -1, -1 }
};
static const int menu_group_sizes[APP_MENU_GROUPS] = { 4, 2, 1, 2 };
static const char *const menu_group_names[APP_MENU_GROUPS] = {
    "Project", "Navigate", "Format", "Help"
};
static const char *const menu_labels[MENU_COUNT] = {
    "Open Project Directory...",
    "New Project...",
    "Back",
    "Forward",
    "Format Code...",
    "Save Current File",
    "Save All Files",
    "Help",
    "Quit"
};
static const char *const menu_keys[MENU_COUNT] = {
    "Ctrl+O", "Ctrl+N", "F10", "Ctrl+F10", "F6", "Ctrl+S", "Ctrl+A",
    "Ctrl+H", "Ctrl+Q"
};

void app_menu_tab_layout(App *a, int xs[APP_MENU_GROUPS],
                         int ws[APP_MENU_GROUPS])
{
    int x = 0;
    int i;
    (void)a;
    for (i = 0; i < APP_MENU_GROUPS; i++) {
        ws[i] = (int)strlen(menu_group_names[i]) + 2;
        xs[i] = x;
        x += ws[i];
    }
}

const char *app_menu_group_name(int group)
{
    if (group < 0 || group >= APP_MENU_GROUPS)
        return "";
    return menu_group_names[group];
}

int app_menu_group_size(int group)
{
    if (group < 0 || group >= APP_MENU_GROUPS)
        return 0;
    return menu_group_sizes[group];
}

const char *app_menu_item_label(int group, int index)
{
    int action;
    if (group < 0 || group >= APP_MENU_GROUPS ||
        index < 0 || index >= menu_group_sizes[group])
        return "";
    action = menu_group_actions[group][index];
    return action >= 0 ? menu_labels[action] : "";
}

const char *app_menu_item_key(int group, int index)
{
    int action;
    if (group < 0 || group >= APP_MENU_GROUPS ||
        index < 0 || index >= menu_group_sizes[group])
        return "";
    action = menu_group_actions[group][index];
    return action >= 0 ? menu_keys[action] : "";
}

int app_menu_group_width(int group)
{
    int w = 26;
    int i;
    for (i = 0; i < app_menu_group_size(group); i++) {
        int need = (int)strlen(app_menu_item_label(group, i)) + 14;
        if (need > w)
            w = need;
    }
    return w;
}

int app_menu_group_at(App *a, int x)
{
    int xs[APP_MENU_GROUPS];
    int ws[APP_MENU_GROUPS];
    int i;
    app_menu_tab_layout(a, xs, ws);
    for (i = 0; i < APP_MENU_GROUPS; i++) {
        if (x >= xs[i] && x < xs[i] + ws[i])
            return i;
    }
    return -1;
}

static void app_menu_open_group(App *a, int group)
{
    if (group < 0 || group >= APP_MENU_GROUPS)
        group = 0;
    a->menu_active = true;
    a->menu_group = group;
    a->menu_sel = 0;
    term_mouse_hover(false);
    app_set_status(a, "Menu: Left/Right switch, Up/Down select, Enter, Esc");
}

static void app_menu_open(App *a)
{
    app_completion_close(a);
    app_menu_open_group(a, 0);
}

static void app_menu_close(App *a)
{
    bool was = a->menu_active;
    a->menu_active = false;
    a->menu_sel = 0;
    if (was)
        term_mouse_hover(true);
}

static void app_menu_execute(App *a, int action)
{
    app_menu_close(a);
    switch (action) {
    case MENU_OPEN_PROJECT:
        dir_picker_open(a, false);
        break;
    case MENU_NEW_PROJECT:
        dir_picker_open(a, true);
        break;
    case MENU_BACK:
        app_go_back(a);
        break;
    case MENU_FORWARD:
        app_go_forward(a);
        break;
    case MENU_FORMAT:
        app_prompt_begin(a, PROMPT_FORMAT, "");
        break;
    case MENU_SAVE:
        save_active(a);
        break;
    case MENU_SAVE_ALL:
        save_all(a);
        break;
    case MENU_HELP:
        a->help = true;
        break;
    case MENU_QUIT:
        app_request_quit(a);
        break;
    default:
        break;
    }
}

static int app_menu_action(int group, int index)
{
    if (group < 0 || group >= APP_MENU_GROUPS ||
        index < 0 || index >= menu_group_sizes[group])
        return -1;
    return menu_group_actions[group][index];
}

static void app_menu_handle(App *a, KeyEvent *ev)
{
    if (ev->code == KEY_MOUSE) {
        int mx = ev->mouse_x;
        int my = ev->mouse_y;
        int group;
        int xs[APP_MENU_GROUPS];
        int ws[APP_MENU_GROUPS];
        int x;
        int w;
        if (ev->mouse_button != 0 || ev->mouse_release)
            return;
        group = app_menu_group_at(a, mx);
        if (my == 0 && group >= 0) {
            app_menu_open_group(a, group);
            return;
        }
        app_menu_tab_layout(a, xs, ws);
        w = SI_MIN(app_menu_group_width(a->menu_group), a->cols - 2);
        x = xs[a->menu_group];
        if (x + w > a->cols - 1)
            x = a->cols - 1 - w;
        if (x < 0)
            x = 0;
        if (mx >= x && mx < x + w &&
            my >= 2 && my < 2 + app_menu_group_size(a->menu_group)) {
            a->menu_sel = my - 2;
            app_menu_execute(a,
                             app_menu_action(a->menu_group, a->menu_sel));
        } else {
            app_menu_close(a);
        }
        return;
    }
    if (ev->code == KEY_ESC) {
        app_menu_close(a);
        return;
    }
    if (ev->code == KEY_F10) {
        app_menu_close(a);
        app_go_back(a);
        return;
    }
    if (ev->code == KEY_CTRL_F10) {
        app_menu_close(a);
        app_go_forward(a);
        return;
    }
    if (ev->code == KEY_LEFT) {
        a->menu_group = (a->menu_group + APP_MENU_GROUPS - 1) %
                        APP_MENU_GROUPS;
        a->menu_sel = 0;
    } else if (ev->code == KEY_RIGHT) {
        a->menu_group = (a->menu_group + 1) % APP_MENU_GROUPS;
        a->menu_sel = 0;
    } else if (ev->code == KEY_UP) {
        a->menu_sel = (a->menu_sel + app_menu_group_size(a->menu_group) - 1) %
                      app_menu_group_size(a->menu_group);
    } else if (ev->code == KEY_DOWN) {
        a->menu_sel = (a->menu_sel + 1) %
                      app_menu_group_size(a->menu_group);
    } else if (ev->code == KEY_HOME) {
        a->menu_sel = 0;
    } else if (ev->code == KEY_END) {
        a->menu_sel = app_menu_group_size(a->menu_group) - 1;
    } else if (ev->code == KEY_ENTER) {
        app_menu_execute(a,
                         app_menu_action(a->menu_group, a->menu_sel));
    }
}

static void app_handle_editor_key(App *a, KeyEvent *ev)
{
    Buffer *b = app_buffer(a);
    if (!b)
        return;
    switch (ev->code) {
    case KEY_CHAR:
        if (ev->len > 0) {
            insert_at_cursor(a, ev->text, (size_t)ev->len);
            app_completion_update(a);
        }
        break;
    case KEY_ENTER:
        editor_enter(a);
        break;
    case KEY_TAB:
        editor_tab(a);
        break;
    case KEY_BACKSPACE:
        editor_backspace(a);
        app_completion_update(a);
        break;
    case KEY_DELETE:
        editor_delete(a);
        app_completion_update(a);
        break;
    case KEY_LEFT:
        move_horizontal(a, -1);
        break;
    case KEY_RIGHT:
        move_horizontal(a, 1);
        break;
    case KEY_UP:
        move_vertical(a, -1);
        break;
    case KEY_DOWN:
        move_vertical(a, 1);
        break;
    case KEY_PGUP:
        move_page(a, -1);
        break;
    case KEY_PGDN:
        move_page(a, 1);
        break;
    case KEY_HOME:
        editor_home(a);
        break;
    case KEY_END:
        editor_end(a);
        break;
    case KEY_CTRL_HOME:
        buf_set_cursor(b, 0);
        b->goal_col = 0;
        b->scroll_line = 0;
        b->scroll_col = 0;
        clear_find(a);
        break;
    case KEY_CTRL_END:
        buf_set_cursor(b, b->len);
        b->goal_col = buf_visual_col(b, b->len);
        clear_find(a);
        app_ensure_visible(a);
        break;
    case KEY_CTRL_LEFT:
        word_backward(a);
        break;
    case KEY_CTRL_RIGHT:
        word_forward(a);
        break;
    case 1: /* Ctrl+A */
        editor_home(a);
        break;
    case 5: /* Ctrl+E */
        editor_end(a);
        break;
    case 4: /* Ctrl+D */
        editor_delete(a);
        break;
    case 11: /* Ctrl+K */
        editor_kill_line(a, true);
        break;
    case 21: /* Ctrl+U */
        editor_kill_line(a, false);
        break;
    case 26: /* Ctrl+Z */
        buf_undo_break(b);
        if (buf_undo(b)) {
            b->goal_col = buf_visual_col(b, (size_t)b->cursor);
            clear_find(a);
            a->view_stale = true;
            app_set_status(a, "Undo");
            app_ensure_visible(a);
        } else {
            app_set_status(a, "Nothing to undo");
        }
        break;
    case 25: /* Ctrl+Y */
        buf_undo_break(b);
        if (buf_redo(b)) {
            b->goal_col = buf_visual_col(b, (size_t)b->cursor);
            clear_find(a);
            a->view_stale = true;
            app_set_status(a, "Redo");
            app_ensure_visible(a);
        } else {
            app_set_status(a, "Nothing to redo");
        }
        break;
    case 19: /* Ctrl+S */
        save_active(a);
        break;
    case 6: /* Ctrl+F */
        app_prompt_begin(a, PROMPT_FIND, a->last_find);
        break;
    case 7: /* Ctrl+G */
        app_prompt_begin(a, PROMPT_GOTO, "");
        break;
    case 16: /* Ctrl+P */
        app_prompt_begin(a, PROMPT_SYMBOL, "");
        break;
    case 15: /* Ctrl+O */
        app_prompt_begin(a, PROMPT_OPEN, "");
        break;
    case 12: /* Ctrl+L */
        app_prompt_begin(a, PROMPT_GOTO, "");
        break;
    case KEY_F8:
        app_toggle_highlight(a);
        break;
    case KEY_SHIFT_F8:
        if (a->last_find[0])
            action_find(a, a->last_find, false);
        else
            app_set_status(a, "No previous search");
        break;
    case KEY_F12:
        action_goto_definition(a);
        break;
    case KEY_SHIFT_F12:
        action_find_refs(a);
        break;
    case KEY_ESC:
        completion_suppress_at_cursor(a);
        a->focus = FOCUS_EDITOR;
        break;
    default:
        break;
    }
}

static void file_selection_fix(App *a)
{
    int count = (int)a->project.file_count;
    if (count <= 0) {
        a->file_sel = 0;
        a->file_scroll = 0;
        return;
    }
    if (a->file_sel < 0)
        a->file_sel = 0;
    if (a->file_sel >= count)
        a->file_sel = count - 1;
    if (a->file_sel < a->file_scroll)
        a->file_scroll = a->file_sel;
    if (a->file_sel >= a->file_scroll + SI_MAX(1, a->right_top_h))
        a->file_scroll = a->file_sel - SI_MAX(1, a->right_top_h) + 1;
}

static void app_handle_outline_key(App *a, KeyEvent *ev)
{
    int count = (int)a->outline.count;
    switch (ev->code) {
    case KEY_UP:
        if (a->outline_sel > 0)
            a->outline_sel--;
        break;
    case KEY_DOWN:
        if (a->outline_sel + 1 < count)
            a->outline_sel++;
        break;
    case KEY_PGUP:
        a->outline_sel -= SI_MAX(1, a->content_height - 1);
        if (a->outline_sel < 0)
            a->outline_sel = 0;
        break;
    case KEY_PGDN:
        a->outline_sel += SI_MAX(1, a->content_height - 1);
        if (a->outline_sel >= count)
            a->outline_sel = count > 0 ? count - 1 : 0;
        break;
    case KEY_HOME:
        a->outline_sel = 0;
        break;
    case KEY_END:
        a->outline_sel = count > 0 ? count - 1 : 0;
        break;
    case KEY_ENTER:
        if (a->outline_sel >= 0 && a->outline_sel < count) {
            ViewItem *it = &a->outline.items[a->outline_sel];
            app_goto_location(a, it->file_index, it->line, it->col, true);
        } else {
            app_set_status(a, "No outline item selected");
        }
        break;
    case KEY_ESC:
        a->focus = FOCUS_EDITOR;
        break;
    default:
        break;
    }
    outline_selection_fix(a);
}

static void app_handle_file_key(App *a, KeyEvent *ev)
{
    int count = (int)a->project.file_count;
    switch (ev->code) {
    case KEY_UP:
        if (a->file_sel > 0)
            a->file_sel--;
        break;
    case KEY_DOWN:
        if (a->file_sel + 1 < count)
            a->file_sel++;
        break;
    case KEY_PGUP:
        a->file_sel -= SI_MAX(1, a->right_top_h - 1);
        if (a->file_sel < 0)
            a->file_sel = 0;
        break;
    case KEY_PGDN:
        a->file_sel += SI_MAX(1, a->right_top_h - 1);
        if (a->file_sel >= count)
            a->file_sel = count > 0 ? count - 1 : 0;
        break;
    case KEY_HOME:
        a->file_sel = 0;
        break;
    case KEY_END:
        a->file_sel = count > 0 ? count - 1 : 0;
        break;
    case KEY_ENTER:
        if (a->file_sel >= 0 && a->file_sel < count)
            app_open_file_index(a, a->file_sel, 1, 0, true);
        else
            app_set_status(a, "No source file selected");
        break;
    case KEY_ESC:
        a->focus = FOCUS_EDITOR;
        break;
    default:
        break;
    }
    file_selection_fix(a);
}

static bool view_item_jumpable(const ViewItem *it)
{
    return !it->is_header && it->file_index >= 0;
}

static void symbol_selection_fix(App *a)
{
    int count = (int)a->view.count;
    int i;
    if (count <= 0) {
        a->sym_sel = 0;
        a->sym_scroll = 0;
        return;
    }
    if (a->sym_sel < 0)
        a->sym_sel = 0;
    if (a->sym_sel >= count)
        a->sym_sel = count - 1;
    if (!view_item_jumpable(&a->view.items[a->sym_sel])) {
        for (i = a->sym_sel + 1; i < count; i++) {
            if (view_item_jumpable(&a->view.items[i])) {
                a->sym_sel = i;
                break;
            }
        }
        if (i == count) {
            for (i = a->sym_sel - 1; i >= 0; i--) {
                if (view_item_jumpable(&a->view.items[i])) {
                    a->sym_sel = i;
                    break;
                }
            }
        }
    }
    if (a->view.kind == VIEW_CONTEXT) {
        /* The graph renderer scrolls by row, not by list item. */
        a->sym_scroll = 0;
        return;
    }
    if (a->sym_sel < a->sym_scroll)
        a->sym_scroll = a->sym_sel;
    if (a->sym_sel >= a->sym_scroll + SI_MAX(1, a->right_bottom_h))
        a->sym_scroll = a->sym_sel - SI_MAX(1, a->right_bottom_h) + 1;
}

static void symbol_move(App *a, int delta)
{
    int count = (int)a->view.count;
    int i = a->sym_sel + delta;
    while (i >= 0 && i < count) {
        if (view_item_jumpable(&a->view.items[i])) {
            a->sym_sel = i;
            break;
        }
        i += delta;
    }
    symbol_selection_fix(a);
}

static void app_handle_symbol_key(App *a, KeyEvent *ev)
{
    int count = (int)a->view.count;
    switch (ev->code) {
    case KEY_UP:
        symbol_move(a, -1);
        return;
    case KEY_DOWN:
        symbol_move(a, 1);
        return;
    case KEY_PGUP:
        a->sym_sel -= SI_MAX(1, a->right_bottom_h - 1);
        if (a->sym_sel < 0)
            a->sym_sel = 0;
        break;
    case KEY_PGDN:
        a->sym_sel += SI_MAX(1, a->right_bottom_h - 1);
        if (a->sym_sel >= count)
            a->sym_sel = count > 0 ? count - 1 : 0;
        break;
    case KEY_HOME:
        a->sym_sel = 0;
        break;
    case KEY_END:
        a->sym_sel = count > 0 ? count - 1 : 0;
        break;
    case KEY_ENTER:
        if (a->sym_sel >= 0 && a->sym_sel < count &&
            view_item_jumpable(&a->view.items[a->sym_sel])) {
            ViewItem *it = &a->view.items[a->sym_sel];
            app_jump_call_site(a, it->file_index, it->line, it->col);
        } else {
            app_set_status(a, "No jump target on this line");
        }
        break;
    case KEY_ESC:
        a->focus = FOCUS_EDITOR;
        break;
    default:
        break;
    }
    symbol_selection_fix(a);
}

static bool editor_offset_at(App *a, int x, int y, int *offset)
{
    Buffer *b = app_buffer(a);
    int row;
    int line;
    int text_x;
    int vcol;

    if (!b || a->too_small)
        return false;
    if (y < a->content_top)
        y = a->content_top;
    if (y >= a->content_top + a->top_h)
        y = a->content_top + a->top_h - 1;
    if (x < a->lw + 1)
        x = a->lw + 1;
    if (x >= a->lw + 1 + a->cw)
        x = a->lw + 1 + a->cw - 1;
    row = y - a->content_top;
    line = b->scroll_line + row;
    if (line < 0)
        line = 0;
    if ((size_t)line >= buf_line_count(b))
        line = (int)buf_line_count(b) - 1;
    text_x = a->lw + 1 + a->gutter_w + 3;
    vcol = b->scroll_col + (x >= text_x ? x - text_x : 0);
    *offset = (int)buf_offset_at_visual(b, line, vcol);
    return true;
}

static void app_clear_highlight(App *a)
{
    a->highlight_active = false;
    a->highlight_word[0] = '\0';
}

static void app_toggle_highlight(App *a)
{
    Buffer *b = app_buffer(a);
    char *word = NULL;
    int off = 0;
    int text_x;
    bool over_editor;

    if (!b) {
        app_set_status(a, "No file to highlight");
        return;
    }
    text_x = a->lw + 1 + a->gutter_w + 3;
    over_editor = a->hover_mode == 0 &&
                  a->hover_y >= a->content_top &&
                  a->hover_y < a->content_top + a->top_h &&
                  a->hover_x >= text_x &&
                  a->hover_x < a->lw + 1 + a->cw;
    if (over_editor && editor_offset_at(a, a->hover_x, a->hover_y, &off))
        word = word_at_offset(a, (size_t)off);
    if (!word)
        word = word_at_cursor(a);
    if (!word || !*word) {
        free(word);
        app_set_status(a, "No identifier under cursor/mouse");
        return;
    }
    if (a->highlight_active &&
        strcmp(a->highlight_word, word) == 0) {
        app_clear_highlight(a);
        app_set_status(a, "Variable highlight cleared");
        free(word);
        return;
    }
    snprintf(a->highlight_word, sizeof(a->highlight_word), "%s", word);
    a->highlight_active = true;
    app_set_status(a, "Highlighting '%s' (F8 clears)", word);
    free(word);
}

static bool app_callflow_hit(App *a, int x, int y)
{
    size_t i;
    if (a->view.kind != VIEW_CONTEXT || a->rw < 16 ||
        a->call_box_count == 0)
        return false;
    for (i = 0; i < a->call_box_count; i++) {
        const CallFlowBox *box = &a->call_boxes[i];
        int top = a->call_box_top + box->row - a->call_box_scroll;
        int left = a->call_box_x + box->col;
        if (y < top || y >= top + 3 ||
            x < left || x >= left + box->w)
            continue;
        if (box->item >= 0 && (size_t)box->item < a->view.count) {
            ViewItem *it = &a->view.items[box->item];
            a->sym_sel = box->item;
            a->focus = FOCUS_CONTEXT;
            if (view_item_jumpable(it)) {
                app_jump_call_site(a, it->file_index, it->line, it->col);
            }
        }
        return true;
    }
    return false;
}

static void app_jump_info_definition(App *a)
{
    if (a->info_def_file < 0 || a->info_def_line <= 0) {
        app_set_status(a, "No definition shown in SYMBOL INFO");
        return;
    }
    if (a->info_def_file >= (int)a->project.file_count) {
        app_set_status(a, "Definition file is no longer in the project");
        return;
    }
    app_open_file_index(a, a->info_def_file, a->info_def_line, 0, true);
    a->focus = FOCUS_EDITOR;
    a->info_click_time = 0;
    app_set_status(a, "Jumped to definition in editor");
}

static void app_context_menu_close(App *a)
{
    a->context_menu_active = false;
}

static void app_context_menu_open(App *a, int x, int y)
{
    a->context_menu_active = true;
    a->context_menu_x = x;
    a->context_menu_y = y;
    a->context_menu_copy = a->has_selection &&
                           a->sel_buf == a->active;
    a->context_menu_paste = a->clipboard != NULL &&
                            a->clipboard_len > 0;
    a->context_menu_sel = a->context_menu_copy ? 0 : 1;
    if (!a->context_menu_copy && !a->context_menu_paste)
        app_set_status(a, "Nothing to copy; clipboard is empty");
    else if (!a->context_menu_copy)
        app_set_status(a, "Clipboard available: Paste enabled");
}

static void app_context_menu_execute(App *a)
{
    if (a->context_menu_sel == 0 && a->context_menu_copy) {
        copy_selection(a);
        app_context_menu_close(a);
        return;
    }
    if (a->context_menu_sel == 1 && a->context_menu_paste) {
        paste_clipboard(a);
        app_context_menu_close(a);
        return;
    }
    app_set_status(a, "That context-menu item is disabled");
}

static int app_splitter_at(App *a, int x, int y)
{
    int row = y - a->content_top;
    int center_x = a->lw + 1;
    int right_x = a->lw + 1 + a->cw;
    if (row < 0 || row >= a->content_height)
        return 0;
    if (x >= center_x && x < right_x &&
        y >= a->split_row - 1 && y <= a->split_row)
        return 3;
    if (a->rw > 0 && x > right_x &&
        y >= a->right_split_row - 1 && y <= a->right_split_row)
        return 4;
    if (x >= a->lw - 1 && x <= a->lw + 1)
        return 1;
    if (a->rw > 0 && x >= right_x - 1 && x <= right_x + 1)
        return 2;
    return 0;
}

static void app_set_hover(App *a, int mode, int x, int y)
{
    int old = a->hover_mode;
    a->hover_mode = mode;
    a->hover_x = x;
    a->hover_y = y;
    if (mode == old)
        return;
    if (mode == 0)
        return;
    {
        int avail = SI_MAX(1, a->cols - 2);
        int lpw = a->lw * 100 / avail;
        int rpw = a->rw * 100 / avail;
        int cpw = a->top_h * 100 /
                  SI_MAX(1, a->content_height - 1);
        int rpw_h = a->right_top_h * 100 /
                    SI_MAX(1, a->content_height - 1);
        if (mode == 1)
            app_set_status(a, "\xe2\x86\x94 drag left divider  (left %d%% / center %d%% / files %d%%)",
                           lpw, SI_MAX(0, 100 - lpw - rpw), rpw);
        else if (mode == 2)
            app_set_status(a, "\xe2\x86\x94 drag right divider  (left %d%% / center %d%% / files %d%%)",
                           lpw, SI_MAX(0, 100 - lpw - rpw), rpw);
        else if (mode == 3)
            app_set_status(a, "\xe2\x86\x95 drag editor/CODE split  (%d%% / %d%%)",
                           cpw, 100 - cpw);
        else
            app_set_status(a, "\xe2\x86\x95 drag files/CALL FLOW split  (%d%% / %d%%)",
                           rpw_h, 100 - rpw_h);
    }
}

static void app_handle_mouse(App *a, KeyEvent *ev)
{
    int row;
    int x;
    int y;
    int button;
    int center_x;
    int right_x;

    if (a->help) {
        a->help = false;
        return;
    }
    if (a->prompt != PROMPT_NONE || a->too_small)
        return;
    x = ev->mouse_x;
    y = ev->mouse_y;
    if (a->context_menu_active) {
        int mx = a->context_menu_x;
        int my = a->context_menu_y;
        int rows = 2;
        int menu_row;
        if (mx + 24 > a->cols - 1)
            mx = a->cols - 25;
        if (mx < 0)
            mx = 0;
        if (my + 4 > a->rows - 1)
            my = a->rows - 5;
        if (my < 0)
            my = 0;
        if (ev->mouse_button == 0 && !ev->mouse_release) {
            menu_row = y - my - 1;
            if (x >= mx && x < mx + 24 && menu_row >= 0 &&
                menu_row < rows) {
                a->context_menu_sel = menu_row;
                app_context_menu_execute(a);
            } else {
                app_context_menu_close(a);
            }
        } else if (ev->mouse_button == 2 && !ev->mouse_release) {
            app_context_menu_close(a);
        }
        return;
    }

    if (ev->mouse_button == 0 && !ev->mouse_release && y == 0) {
        int group = app_menu_group_at(a, x);
        if (group >= 0)
            app_menu_open_group(a, group);
        return;
    }
    if (ev->mouse_release) {
        if (a->drag_mode) {
            a->drag_mode = 0;
            return;
        }
        if (a->selecting) {
            a->selecting = false;
            if (a->sel_anchor == a->sel_cursor)
                a->has_selection = false;
        }
        return;
    }
    if (ev->mouse_button == 2 && !ev->mouse_release) {
        if (y >= a->content_top && y < a->content_top + a->top_h &&
            x >= a->lw + 1 && x < a->lw + 1 + a->cw) {
            app_context_menu_open(a, x, y);
        } else {
            app_menu_open(a);
        }
        return;
    }
    if (y < a->content_top || y >= a->content_top + a->content_height) {
        if ((ev->mouse_button & 32) && !a->selecting)
            app_set_hover(a, 0, x, y);
        return;
    }
    row = y - a->content_top;
    button = ev->mouse_button;
    center_x = a->lw + 1;
    right_x = a->lw + 1 + a->cw;

    if (a->drag_mode && (button & 32)) {
        if (a->drag_mode == 1) {
            int n = x;
            if (n < 18)
                n = 18;
            if (n > a->cols - 40)
                n = a->cols - 40;
            if (n < 1)
                n = 1;
            a->left_pct = n * 100 / (a->cols - 2);
        } else if (a->drag_mode == 2) {
            int n = a->cols - 1 - x;
            if (n < 18)
                n = 18;
            if (n > a->cols - 40)
                n = a->cols - 40;
            if (n < 1)
                n = 1;
            a->right_pct = n * 100 / (a->cols - 2);
        } else if (a->drag_mode == 3) {
            int h = y - a->content_top;
            if (h < 3)
                h = 3;
            if (h > a->content_height - 3)
                h = a->content_height - 3;
            a->center_top_pct = h * 100 / SI_MAX(1, a->content_height - 1);
        } else if (a->drag_mode == 4) {
            int h = y - a->content_top;
            if (h < 3)
                h = 3;
            if (h > a->content_height - 3)
                h = a->content_height - 3;
            a->right_top_pct = h * 100 / SI_MAX(1, a->content_height - 1);
        }
        app_layout(a);
        a->hover_mode = a->drag_mode;
        a->hover_x = x;
        a->hover_y = y;
        {
            int avail = SI_MAX(1, a->cols - 2);
            if (a->drag_mode == 1 || a->drag_mode == 2) {
                int lpw = a->lw * 100 / avail;
                int rpw = a->rw * 100 / avail;
                app_set_status(a, "\xe2\x86\x94 resizing: left %d%% / center %d%% / files %d%%",
                               lpw, SI_MAX(0, 100 - lpw - rpw), rpw);
            } else if (a->drag_mode == 3) {
                int top = a->top_h * 100 /
                          SI_MAX(1, a->content_height - 1);
                app_set_status(a, "\xe2\x86\x95 resizing: editor %d%% / CODE %d%%",
                               top, 100 - top);
            } else {
                int top = a->right_top_h * 100 /
                          SI_MAX(1, a->content_height - 1);
                app_set_status(a, "\xe2\x86\x95 resizing: files %d%% / CALL FLOW %d%%",
                               top, 100 - top);
            }
        }
        return;
    }

    if ((button & 32) && !a->selecting) {
        app_set_hover(a, app_splitter_at(a, x, y), x, y);
        return;
    }

    /* SGR mouse motion with the left button held has bit 32 set. */
    if ((button & 32) && a->selecting && a->sel_buf == a->active) {
        int off;
        if (row < a->top_h && editor_offset_at(a, x, y, &off)) {
            Buffer *b = app_buffer(a);
            a->sel_cursor = off;
            a->has_selection = a->sel_cursor != a->sel_anchor;
            if (b) {
                b->cursor = off;
                b->goal_col = buf_visual_col(b, (size_t)off);
            }
            a->focus = FOCUS_EDITOR;
            clear_find(a);
            app_ensure_visible(a);
        }
        return;
    }

    if (button == 64 || button == 65) { /* wheel up / down */
        int delta = button == 64 ? -3 : 3;
        if (x < a->lw) {
            a->outline_sel += delta;
            a->focus = FOCUS_OUTLINE;
            outline_selection_fix(a);
        } else if (x >= center_x && x < right_x) {
            if (row < a->top_h) {
                a->focus = FOCUS_EDITOR;
                move_vertical(a, delta);
                sync_outline_to_cursor(a);
                app_update_info(a);
            } else if (row > a->top_h) {
                int max_scroll;
                a->focus = FOCUS_INFO;
                app_update_info(a);
                max_scroll = a->info_def_end - a->info_code_start + 1 -
                             SI_MAX(1, a->bottom_h - 2);
                if (max_scroll < 0)
                    max_scroll = 0;
                a->info_scroll += delta;
                if (a->info_scroll < 0)
                    a->info_scroll = 0;
                if (a->info_scroll > max_scroll)
                    a->info_scroll = max_scroll;
            }
        } else if (a->rw > 0 && x > right_x) {
            if (row < a->right_top_h) {
                a->file_sel += delta;
                a->focus = FOCUS_PROJECT;
                file_selection_fix(a);
            } else if (row > a->right_top_h) {
                a->sym_sel += delta;
                a->focus = FOCUS_CONTEXT;
                symbol_selection_fix(a);
            }
        }
        return;
    }
    if (button != 0)
        return;

    a->selecting = false;
    {
        int mode = app_splitter_at(a, x, y);
        if (mode) {
            a->drag_mode = mode;
            app_set_hover(a, mode, x, y);
            return;
        }
    }

    app_clear_selection(a);
    if (x < a->lw) {
        int idx = a->outline_scroll + row;
        if (idx >= 0 && (size_t)idx < a->outline.count) {
            bool same = a->focus == FOCUS_OUTLINE &&
                        a->outline_sel == idx;
            a->outline_sel = idx;
            a->focus = FOCUS_OUTLINE;
            outline_selection_fix(a);
            if (same) {
                ViewItem *it = &a->outline.items[idx];
                app_goto_location(a, it->file_index, it->line, it->col, true);
            } else {
                app_set_status(a, "Outline selected: Enter jumps");
            }
        }
        return;
    }
    if (x >= center_x && x < right_x) {
        if (row < a->top_h) {
            Buffer *b = app_buffer(a);
            int off;
            if (!b || !editor_offset_at(a, x, y, &off))
                return;
            buf_set_cursor(b, (size_t)off);
            b->goal_col = buf_visual_col(b, (size_t)b->cursor);
            a->sel_buf = a->active;
            a->sel_anchor = off;
            a->sel_cursor = off;
            a->selecting = true;
            a->has_selection = false;
            clear_find(a);
            a->focus = FOCUS_EDITOR;
            app_ensure_visible(a);
            sync_outline_to_cursor(a);
            app_update_info(a);
            app_show_context(a);
        } else if (row > a->top_h) {
            long now = (long)time(NULL);
            bool dbl = a->focus == FOCUS_INFO &&
                       a->info_click_time > 0 &&
                       now - a->info_click_time <= 1 &&
                       abs(x - a->info_click_x) <= 2 &&
                       abs(y - a->info_click_y) <= 2;
            a->focus = FOCUS_INFO;
            a->info_click_time = now;
            a->info_click_x = x;
            a->info_click_y = y;
            if (dbl)
                app_jump_info_definition(a);
            else
                app_set_status(a,
                    "Symbol info selected: double-click to edit definition");
        }
        return;
    }
    if (a->rw > 0 && x > right_x) {
        if (app_callflow_hit(a, x, y))
            return;
        if (a->view.kind == VIEW_CONTEXT &&
            a->focus == FOCUS_CONTEXT) {
            /* Expanded graph: use Up/Down to select a caller node. */
            return;
        }
        if (row < a->right_top_h) {
            int idx = a->file_scroll + row;
            if (idx >= 0 && (size_t)idx < a->project.file_count) {
                bool same = a->focus == FOCUS_PROJECT &&
                            a->file_sel == idx;
                a->file_sel = idx;
                a->focus = FOCUS_PROJECT;
                file_selection_fix(a);
                if (same)
                    app_open_file_index(a, idx, 1, 0, true);
                else
                    app_set_status(a, "File selected: Enter opens");
            }
        } else if (row > a->right_top_h) {
            int idx;
            if (a->view.kind == VIEW_CONTEXT) {
                /* Graph nodes are selected with Up/Down; a click focuses
                 * and expands the flow instead of guessing a list row. */
                a->focus = FOCUS_CONTEXT;
                app_set_status(a, "Call flow focused: Up/Down select, Enter jumps");
                return;
            }
            idx = a->sym_scroll + (row - a->right_top_h - 1);
            if (idx >= 0 && (size_t)idx < a->view.count) {
                ViewItem *it = &a->view.items[idx];
                bool same = a->focus == FOCUS_CONTEXT &&
                            a->sym_sel == idx;
                a->sym_sel = idx;
                a->focus = FOCUS_CONTEXT;
                symbol_selection_fix(a);
                if (same && view_item_jumpable(it))
                    app_jump_call_site(a, it->file_index, it->line, it->col);
                else
                    app_set_status(a, "Call-flow item selected: Enter jumps");
            }
        }
    }
}

static void app_handle_key(App *a, KeyEvent *ev)
{
    if (a->relation_building)
        return;
    if (a->relations_large_prompt) {
        if (ev->code == KEY_ALT_CHAR &&
            (ev->text[0] == 'r' || ev->text[0] == 'R')) {
            a->relations_large_prompt = false;
            a->relations_pending = true;
            app_set_status(a, "Building relation tables...");
        } else if (ev->code == KEY_ENTER || ev->code == KEY_ESC ||
                   (ev->code == KEY_MOUSE && ev->mouse_button <= 2)) {
            a->relations_large_prompt = false;
            app_set_status(a,
                "Large project: relations skipped (Alt/Opt+R builds them)");
        } else if (ev->code == 17) { /* Ctrl+Q */
            a->quit = true;
        }
        return;
    }
    if (a->relation_ready_prompt) {
        if (ev->code == KEY_ENTER || ev->code == KEY_ESC ||
            (ev->code == KEY_MOUSE && ev->mouse_button <= 2))
            a->relation_ready_prompt = false;
        return;
    }
    if (a->context_menu_active && ev->code != KEY_MOUSE) {
        if (ev->code == KEY_ESC) {
            app_context_menu_close(a);
        } else if (ev->code == KEY_UP || ev->code == KEY_DOWN) {
            a->context_menu_sel = a->context_menu_sel ? 0 : 1;
        } else if (ev->code == KEY_ENTER) {
            app_context_menu_execute(a);
        } else if (ev->code == KEY_CHAR && ev->text[0] == 'c') {
            a->context_menu_sel = 0;
            app_context_menu_execute(a);
        } else if (ev->code == KEY_CHAR && ev->text[0] == 'p') {
            a->context_menu_sel = 1;
            app_context_menu_execute(a);
        }
        return;
    }
    if (a->help) {
        a->help = false;
        return;
    }
    if (a->dir_active) {
        dir_picker_handle(a, ev);
        return;
    }
    if (a->menu_active) {
        app_menu_handle(a, ev);
        return;
    }
    if (a->chooser_active) {
        app_chooser_handle(a, ev);
        return;
    }
    if (a->prompt != PROMPT_NONE) {
        app_prompt_handle(a, ev);
        return;
    }
    if (a->project_picker_active) {
        app_project_picker_handle(a, ev);
        return;
    }
    if (a->comp_active) {
        if (app_completion_handle_key(a, ev))
            return;
    }
    if (ev->code == KEY_MOUSE) {
        app_handle_mouse(a, ev);
        return;
    }
    if (ev->code == KEY_NONE)
        return;
    if (ev->code == 3) { /* Ctrl+C */
        copy_selection(a);
        return;
    }
    if (ev->code == 22) { /* Ctrl+V */
        paste_clipboard(a);
        return;
    }
    app_clear_selection(a);

    if (ev->code == KEY_F1) {
        a->help = true;
        return;
    }
    if (ev->code == 17) { /* Ctrl+Q */
        app_request_quit(a);
        return;
    }
    if (ev->code == KEY_F5) {
        app_reload_project(a);
        return;
    }
    if (ev->code == KEY_F10) {
        app_go_back(a);
        return;
    }
    if (ev->code == KEY_CTRL_F10) {
        app_go_forward(a);
        return;
    }
    if (ev->code == KEY_F6) {
        app_prompt_begin(a, PROMPT_FORMAT, "");
        return;
    }
    if (ev->code == KEY_F9) {
        app_switch_buffer(a, 1);
        return;
    }
    if (ev->code == KEY_SHIFT_F9) {
        app_switch_buffer(a, -1);
        return;
    }
    if (ev->code == KEY_ALT_LEFT) {
        app_go_back(a);
        return;
    }
    if (ev->code == KEY_ALT_RIGHT) {
        app_go_forward(a);
        return;
    }
    if (ev->code == KEY_F2 || ev->code == 2) { /* F2 or Ctrl+B */
        focus_outline(a);
        return;
    }
    if (ev->code == KEY_CTRL_F3) {
        focus_context(a);
        return;
    }
    if (ev->code == KEY_CTRL_F4) {
        a->focus = FOCUS_EDITOR;
        return;
    }
    if (ev->code == KEY_F3) {
        if (a->last_find[0])
            action_find(a, a->last_find, true);
        else
            app_set_status(a, "No previous search");
        return;
    }
    if (ev->code == KEY_F4) {
        if (a->last_find[0])
            action_find(a, a->last_find, false);
        else
            app_set_status(a, "No previous search");
        return;
    }
    if (ev->code == KEY_F7) {
        focus_project(a);
        return;
    }
    if (ev->code == 20) { /* Ctrl+T */
        app_focus_next(a);
        return;
    }
    if (ev->code == KEY_SHIFT_TAB) {
        app_focus_prev(a);
        return;
    }
    if (ev->code == KEY_ALT_CHAR && ev->len > 0) {
        char c = ev->text[0];
        if (c == 'm' || c == 'M') {
            app_menu_open(a);
            return;
        }
        if (c == 'r' || c == 'R') {
            a->relations_pending = true;
            app_set_status(a, "Rebuilding relation tables...");
            return;
        }
        if (c == 'g' || c == 'G') {
            app_prompt_begin(a, PROMPT_GREP, a->last_grep);
            return;
        }
        if (c == '1' || c == 'f' || c == 'F')
            focus_outline(a);
        else if (c == '2' || c == 's' || c == 'S')
            focus_context(a);
        else if (c == '3' || c == 'e' || c == 'E')
            a->focus = FOCUS_EDITOR;
        else if (c == '4' || c == 'p' || c == 'P')
            focus_project(a);
        else if (c == '5' || c == 'i' || c == 'I')
            focus_info(a);
        else if (c == '0' || c == 't' || c == 'T')
            app_focus_next(a);
        return;
    }
    if (ev->code == 19) { /* Ctrl+S */
        save_active(a);
        return;
    }
    if (ev->code == 1) { /* Ctrl+A */
        save_all(a);
        return;
    }
    if (ev->code == 14) { /* Ctrl+N */
        dir_picker_open(a, true);
        return;
    }
    if (ev->code == 8) { /* Ctrl+H */
        a->help = true;
        return;
    }
    if (ev->code == 6) { /* Ctrl+F: find / find next */
        a->focus = FOCUS_EDITOR;
        if (a->last_find[0])
            action_find(a, a->last_find, true);
        else
            app_prompt_begin(a, PROMPT_FIND, "");
        return;
    }
    if (ev->code == KEY_F8) {
        a->focus = FOCUS_EDITOR;
        app_toggle_highlight(a);
        return;
    }
    if (ev->code == 7) { /* Ctrl+G */
        app_prompt_begin(a, PROMPT_GOTO, "");
        return;
    }
    if (ev->code == 16) { /* Ctrl+P */
        app_prompt_begin(a, PROMPT_SYMBOL, "");
        return;
    }
    if (ev->code == 15) { /* Ctrl+O */
        dir_picker_open(a, false);
        return;
    }
    if (ev->code == 12) { /* Ctrl+L */
        app_prompt_begin(a, PROMPT_GOTO, "");
        return;
    }
    if (ev->code == 18) { /* Ctrl+R */
        a->focus = FOCUS_EDITOR;
        app_show_context(a);
        return;
    }
    if (ev->code == KEY_TAB) {
        if (a->focus == FOCUS_EDITOR)
            editor_tab(a);
        else
            app_focus_next(a);
        return;
    }
    if (ev->code == KEY_ESC) {
        if (a->focus == FOCUS_EDITOR)
            completion_suppress_at_cursor(a);
        a->focus = FOCUS_EDITOR;
        return;
    }

    switch (a->focus) {
    case FOCUS_OUTLINE:
        app_handle_outline_key(a, ev);
        break;
    case FOCUS_PROJECT:
        app_handle_file_key(a, ev);
        break;
    case FOCUS_CONTEXT:
        app_handle_symbol_key(a, ev);
        break;
    case FOCUS_INFO:
        if (ev->code == KEY_ESC || ev->code == KEY_CTRL_F4) {
            a->focus = FOCUS_EDITOR;
        } else if (ev->code == KEY_UP || ev->code == KEY_PGUP ||
                   ev->code == KEY_DOWN || ev->code == KEY_PGDN ||
                   ev->code == KEY_HOME || ev->code == KEY_END) {
            int max_scroll = a->info_def_end - a->info_code_start + 1 -
                             SI_MAX(1, a->bottom_h - 2);
            int step = ev->code == KEY_PGUP || ev->code == KEY_PGDN
                           ? SI_MAX(1, a->bottom_h - 1)
                           : 1;
            if (max_scroll < 0)
                max_scroll = 0;
            if (ev->code == KEY_HOME)
                a->info_scroll = 0;
            else if (ev->code == KEY_END)
                a->info_scroll = max_scroll;
            else if (ev->code == KEY_UP || ev->code == KEY_PGUP)
                a->info_scroll -= step;
            else
                a->info_scroll += step;
            if (a->info_scroll < 0)
                a->info_scroll = 0;
            if (a->info_scroll > max_scroll)
                a->info_scroll = max_scroll;
        }
        break;
    case FOCUS_EDITOR:
    default:
        app_handle_editor_key(a, ev);
        break;
    }
}

void app_use_theme(App *a, const char *name)
{
    a->theme = make_theme(name && str_ieq(name, "light"));
    a->theme.colors = term_color_mode();
}

void app_handle_key_event(App *a, int code, const char *text)
{
    KeyEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.code = code;
    if (text && *text) {
        size_t n = strlen(text);
        if (n > sizeof(ev.text) - 1)
            n = sizeof(ev.text) - 1;
        memcpy(ev.text, text, n);
        ev.len = (int)n;
    }
    app_handle_key(a, &ev);
}

void app_handle_mouse_event(App *a, int x, int y, int button, bool release)
{
    KeyEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.code = KEY_MOUSE;
    ev.mouse_x = x;
    ev.mouse_y = y;
    ev.mouse_button = button;
    ev.mouse_release = release;
    app_handle_key(a, &ev);
}

/* ------------------------------------------------------------------ */
/* Startup / main loop                                                 */
/* ------------------------------------------------------------------ */

static void progress_line(Str *s, int x, int y, int content,
                          const char *text, bool center, bool strong)
{
    int len = (int)strlen(text);
    int left = 0;
    int right;

    if (len > content)
        len = content;
    if (center)
        left = (content - len) / 2;
    right = content - left - len;

    str_printf(s, "\x1b[%d;%dH|", y + 1, x + 1);
    if (strong)
        str_puts(s, "\x1b[1;36m");
    str_putc(s, ' ');
    while (left-- > 0)
        str_putc(s, ' ');
    str_append(s, text, (size_t)len);
    while (right-- > 0)
        str_putc(s, ' ');
    if (strong)
        str_puts(s, "\x1b[0m");
    str_putc(s, ' ');
    str_putc(s, '|');
}

static int g_load_spin;

static void draw_build_progress(const char *root, const char *phase,
                                size_t files, size_t symbols,
                                const char *path, bool cancellable)
{
    int rows;
    int cols;
    int boxw;
    int boxh = 11;
    int x;
    int y;
    int content;
    int i;
    char line[1024];
    char shortroot[512];
    char shortpath[512];
    Str s;

    term_get_size(&rows, &cols);
    boxw = SI_MIN(88, cols - 4);
    if (boxw < 46 || rows < boxh + 2) {
        /* Very small terminal: keep a compact readable fallback. */
        str_init(&s);
        str_puts(&s, "\x1b[2J\x1b[H");
        str_printf(&s, "SI-C [%c] %s\n\nfiles: %lu  symbols: %lu\n%s\n",
                   "|/\\-"[g_load_spin++ & 3], phase,
                   (unsigned long)files, (unsigned long)symbols,
                   path ? path : "");
        term_write(s.data, s.len);
        str_free(&s);
        return;
    }
    x = (cols - boxw) / 2;
    y = (rows - boxh) / 2;
    content = boxw - 4;

    path_shorten(root ? root : "", content - 8, shortroot,
                 sizeof(shortroot));
    path_shorten(path ? path : "", content - 9, shortpath,
                 sizeof(shortpath));

    str_init(&s);
    /* Full clear + synchronized update: no leftover fragments of old UI. */
    str_puts(&s, "\x1b[2J\x1b[H\x1b[?2026h\x1b[?25l");
    str_printf(&s, "\x1b[%d;%dH+", y + 1, x + 1);
    for (i = 0; i < boxw - 2; i++)
        str_putc(&s, '-');
    str_putc(&s, '+');

    {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "SI-C  LOADING  %c",
                 "|/\\-"[g_load_spin++ & 3]);
        progress_line(&s, x, y + 1, content, hdr, true, true);
    }
    snprintf(line, sizeof(line), "Phase: %s", phase);
    progress_line(&s, x, y + 2, content, line, false, false);
    snprintf(line, sizeof(line), "Root:  %s", shortroot);
    progress_line(&s, x, y + 4, content, line, false, false);
    snprintf(line, sizeof(line), "Files: %lu    Symbols: %lu",
             (unsigned long)files, (unsigned long)symbols);
    progress_line(&s, x, y + 5, content, line, false, false);
    snprintf(line, sizeof(line), "Now:   %s", shortpath);
    progress_line(&s, x, y + 6, content, line, false, false);
    progress_line(&s, x, y + 8, content,
                  cancellable ? "Esc / Ctrl+C: cancel"
                              : "Building all relations, please wait...",
                  false, false);
    progress_line(&s, x, y + 9, content,
                  "Do not close the terminal during this step", false, false);

    str_printf(&s, "\x1b[%d;%dH+", y + boxh - 1, x + 1);
    for (i = 0; i < boxw - 2; i++)
        str_putc(&s, '-');
    str_putc(&s, '+');
    str_puts(&s, "\x1b[0m\x1b[?2026l\x1b[?25l");
    term_write(s.data, s.len);
    str_free(&s);
}

static bool index_progress(void *ud, size_t files, size_t symbols,
                           const char *path)
{
    ProgressCtx *ctx = ud;
    KeyEvent ev;
    const char *phase;
    bool cache_load = path &&
                      strncmp(path, "reading .si-index", 17) == 0;

    /*
     * Poll input even while indexing.  The cache-read phase is not
     * cancellable because we are already loading a completed index.
     */
    if (!cache_load && term_poll_key(&ev) > 0 &&
        (ev.code == KEY_ESC || ev.code == 3)) {
        return false;
    }
    if (!cache_load && files != 0 && files != 1 && files % 20 != 0)
        return true;

    if (cache_load)
        phase = "LOADING CACHED INDEX";
    else if (files == 0)
        phase = "SCANNING DIRECTORIES";
    else
        phase = "PARSING C/C++ FILES";
    draw_build_progress(ctx->root, phase, files, symbols, path,
                        !cache_load);
    return true;
}

static bool relation_progress(void *ud, size_t files, size_t symbols,
                              const char *path)
{
    App *a = ud;
    a->relation_files = files;
    a->relation_symbols = symbols;
    if (files == 1 || files % 50 == 0)
        draw_build_progress(a->project.root,
                            "BUILDING CALL + VARIABLE RELATIONS",
                            files, symbols, path, false);
    return true;
}

int ui_run(const char *root, const char *initial_file, const char *theme,
           int initial_cursor)
{
    App a;
    Project project;
    ProgressCtx ctx;
    char *err = NULL;
    bool from_cache = false;
    int exit_code = 0;

    memset(&a, 0, sizeof(a));
    memset(&project, 0, sizeof(project));
    strlist_init(&a.recent_projects);
    a.active = -1;
    a.active_project_file = -1;
    a.pending_info_sym = -1;
    a.prompt = PROMPT_NONE;
    a.focus = FOCUS_EDITOR;
    app_use_theme(&a, theme);
    state_load(NULL, NULL, NULL, &a.recent_projects);

    if (!term_init(&err)) {
        fprintf(stderr, "si: %s\n", err ? err : "cannot initialize terminal");
        strlist_free(&a.recent_projects);
        free(err);
        return 2;
    }

    if (root && *root) {
        ctx.root = root;
        draw_build_progress(root, "LOADING PROJECT", 0, 0,
                            "restoring project and index...", false);
        if (!project_load_cached(&project, root, index_progress, &ctx, &err,
                                 &from_cache)) {
            term_shutdown();
            fprintf(stderr, "si: %s\n", err ? err : "cannot index project");
            free(err);
            strlist_free(&a.recent_projects);
            project_free(&project);
            return 1;
        }
        {
            KeyEvent junk;
            while (term_poll_key(&junk) > 0)
                ;
        }
        a.project = project;
        /* Persist symbols immediately; relations are added after they build. */
        if (!from_cache)
            project_cache_save(&a.project, NULL);
        app_rebuild_globals(&a);
        app_layout(&a);

        if (initial_file && *initial_file) {
            int fi = project_file_index_of_path(&a.project, initial_file);
            if (fi < 0)
                fi = project_find_file_by_rel(&a.project, initial_file);
            if (fi >= 0)
                app_open_file_index(&a, fi, 1, 0, false);
            else
                app_open_path(&a, initial_file, 1, false);
        }
        if (!app_buffer(&a)) {
            if (a.project.file_count > 0)
                app_open_file_index(&a, 0, 1, 0, false);
            else
                app_refresh_outline(&a);
        }
        if (a.view.kind == VIEW_NONE)
            app_refresh_outline(&a);

        if (initial_cursor > 0) {
            Buffer *rb = app_buffer(&a);
            if (rb) {
                if (initial_cursor > (int)rb->len)
                    initial_cursor = (int)rb->len;
                buf_set_cursor(rb, (size_t)initial_cursor);
                rb->goal_col = buf_visual_col(rb, (size_t)rb->cursor);
                app_ensure_visible(&a);
            }
        }

        a.file_sel = app_active_file_index(&a);
        if (a.file_sel < 0)
            a.file_sel = 0;
        file_selection_fix(&a);
        a.focus = FOCUS_EDITOR;
        app_set_home(&a);
        app_schedule_relations(&a);
        app_recent_touch(&a, a.project.root ? a.project.root : root);
        app_save_state(&a);

        if (from_cache)
            app_set_status(&a,
                "Loaded cached index: %lu files, %lu symbols (F5 reindex)",
                (unsigned long)a.project.file_count,
                (unsigned long)a.project.sym_count);
        else if (a.project.warnings > 0)
            app_set_status(&a, "Indexed %lu files, %lu symbols, %d warnings (F5 reindex)",
                           (unsigned long)a.project.file_count,
                           (unsigned long)a.project.sym_count,
                           a.project.warnings);
        else
            app_set_status(&a, "Indexed %lu files, %lu symbols. F1 help",
                           (unsigned long)a.project.file_count,
                           (unsigned long)a.project.sym_count);
    } else {
        app_layout(&a);
        app_refresh_outline(&a);
        a.focus = FOCUS_EDITOR;
        app_project_picker_open(&a);
    }

    while (!a.quit) {
        KeyEvent ev;
        int rc;
        app_layout(&a);
        app_ensure_visible(&a);
        file_selection_fix(&a);
        symbol_selection_fix(&a);
        outline_selection_fix(&a);
        if (a.focus == FOCUS_EDITOR)
            app_sync_callflow(&a);
        app_render(&a);

        /*
         * Relation tables are built *after* the project is loaded and
         * the normal UI exists.  This lets the modal show progress and
         * lets the user confirm the finished table with Enter/OK.
         */
        if (a.relations_pending) {
            char *rerr = NULL;
            a.relations_pending = false;
            a.relation_building = true;
            a.relation_files = 0;
            a.relation_symbols = 0;
            app_render(&a);
            term_mouse_hover(false);
            if (project_build_relations(&a.project, relation_progress,
                                        &a, &rerr)) {
                a.relation_edges =
                    (size_t)project_call_edge_count(&a.project);
                a.relation_vars =
                    (size_t)project_var_use_count(&a.project);
                a.relations_large_skipped = false;
                a.live_context_file = -1;
                a.live_context_line = -1;
                a.live_context_word[0] = '\0';
                a.relation_ready_prompt = true;
                {
                    char *cerr = NULL;
                    char *cpath = a.project.root
                                      ? path_join(a.project.root,
                                                  ".si-index")
                                      : NULL;
                    if (project_cache_save(&a.project, &cerr)) {
                        app_set_status(&a,
                            "Relations ready (%lu edges); index saved to %s",
                            (unsigned long)a.relation_edges,
                            cpath ? cpath : ".si-index");
                    } else {
                        app_set_status(&a,
                            "Relations ready, but index save failed: %s",
                            cerr ? cerr : "unknown error");
                    }
                    free(cerr);
                    free(cpath);
                }
            } else {
                app_set_status(&a, "Relation build failed: %s",
                               rerr ? rerr : "unknown error");
            }
            free(rerr);
            {
                /*
                 * Drop mouse/key events queued while the full-screen build
                 * progress was displayed.  Otherwise a click during the
                 * build could dismiss the READY dialog immediately.
                 */
                KeyEvent junk;
                while (term_poll_key(&junk) > 0)
                    ;
            }
            term_mouse_hover(true);
            a.relation_building = false;
            continue;
        }

        rc = term_read_key(&ev);
        if (rc < 0)
            break;
        if (rc == 0)
            continue;
        app_handle_key(&a, &ev);
        if (a.focus == FOCUS_EDITOR) {
            sync_outline_to_cursor(&a);
            app_update_info(&a);
            app_sync_callflow(&a);
        }
    }

    if (a.project.root && a.project.relations_ready)
        project_cache_save(&a.project, NULL);
    app_save_state(&a);
    term_shutdown();
    app_completion_reset(&a);
    free(a.sym_name_index);
    {
        int i;
        for (i = 0; i < a.nbuf; i++)
            buf_free(a.bufs[i]);
        free(a.bufs);
    }
    app_view_clear(&a.view);
    free(a.view.items);
    free(a.call_boxes);
    app_view_clear(&a.outline);
    free(a.outline.items);
    buf_free(a.info_buffer);
    {
        size_t ci;
        for (ci = 0; ci < a.chooser_count; ci++)
            free(a.chooser_items[ci].code);
        free(a.chooser_items);
    }
    {
        size_t di;
        for (di = 0; di < a.dir_count; di++)
            free(a.dir_items[di]);
        free(a.dir_items);
    }
    {
        size_t li;
        for (li = 0; li < a.nlocals; li++) {
            free(a.locals[li].name);
            free(a.locals[li].type);
        }
        free(a.locals);
    }
    strlist_free(&a.recent_projects);
    strlist_free(&a.globals);
    strlist_free(&a.types);
    strlist_free(&a.macros);
    free(a.clipboard);
    {
        size_t i;
        for (i = 0; i < a.nback; i++)
            nav_free(&a.back[i]);
        for (i = 0; i < a.nfwd; i++)
            nav_free(&a.fwd[i]);
        free(a.back);
        free(a.fwd);
        nav_free(&a.home);
    }
    project_free(&a.project);
    return exit_code;
}

