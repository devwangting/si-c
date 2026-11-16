/*
 * Headless UI smoke test.  It drives App directly (no terminal raw mode) and
 * renders ANSI frames to stdout, so the rendering and key paths can be
 * exercised in CI.  Run:  ./ui_smoke tests/fixtures/demo > frame.ansi
 */
#include "si.h"
#include "app.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static int failures;
static int checks;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void key(App *a, int code, const char *text)
{
    app_handle_key_event(a, code, text);
    app_layout(a);
    app_ensure_visible(a);
    if (a->focus == FOCUS_EDITOR)
        app_update_info(a);
    app_render(a);
}

static void type_text(App *a, const char *s)
{
    while (*s) {
        char one[2];
        one[0] = *s++;
        one[1] = '\0';
        key(a, KEY_CHAR, one);
    }
}

static LocalVar *find_local(App *a, const char *name)
{
    size_t i;
    for (i = 0; i < a->nlocals; i++) {
        if (strcmp(a->locals[i].name, name) == 0)
            return &a->locals[i];
    }
    return NULL;
}

static bool completion_has(App *a, const char *name)
{
    size_t i;
    for (i = 0; i < a->comp_count; i++) {
        if (strcmp(a->comp_items[i].name, name) == 0)
            return true;
    }
    return false;
}

static void move_after_anchor(App *a, const char *anchor)
{
    Buffer *b = app_buffer(a);
    BufMatch m;
    bool found = buf_find(b, anchor, 0, true, false, &m);
    check(found, anchor);
    if (!found)
        return;
    b->cursor = (int)(m.pos + strlen(anchor));
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    buf_insert_at(b, (size_t)b->cursor, "\n", 1);
    b->cursor++;
    b->goal_col = 0;
    app_update_info(a);
    app_completion_reset(a);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "tests/fixtures/demo";
    Project p;
    App a;
    char *err = NULL;
    Buffer *b;
    size_t len_before;
    int fi;
    int i;

    if (!project_load(&p, root, NULL, NULL, &err)) {
        fprintf(stderr, "project load failed: %s\n", err ? err : "?");
        free(err);
        return 1;
    }
    memset(&a, 0, sizeof(a));
    strlist_init(&a.recent_projects);
    a.project = p;
    app_rebuild_globals(&a);
    a.active = -1;
    a.prompt = PROMPT_NONE;
    a.focus = FOCUS_EDITOR;
    app_use_theme(&a, "dark");
    app_layout(&a);
    check(a.rows >= 20 && a.cols >= 80, "fallback terminal size");

    fi = project_find_file_by_rel(&a.project, "main.c");
    check(fi >= 0, "main.c in project");
    if (fi < 0)
        return 1;
    app_open_file_index(&a, fi, 1, 0, false);
    b = app_buffer(&a);
    check(b != NULL, "buffer opened");
    check(a.outline.count > 0, "file outline populated");
    {
        BufMatch cm;
        check(buf_find(b, "int result = 0, other;", 0, true, false, &cm),
              "find multi declarator locals");
        b->cursor = (int)cm.pos;
        app_update_info(&a);
        check(find_local(&a, "result") != NULL, "local result");
        check(find_local(&a, "other") != NULL, "local other");
        check(find_local(&a, "local_p") != NULL, "local local_p");
        check(find_local(&a, "local_q") != NULL, "local local_q");
        check(buf_find(b, "node.pos.y", 0, true, false, &cm),
              "find completion return");
        b->cursor = (int)cm.pos;
        app_update_info(&a);
        check(a.info_valid && strstr(a.info_type, "CompletionNode") != NULL,
              "info resolves CompletionNode local");
    }
    /* Before relation tables exist, CALL FLOW must show Alt+R guidance. */
    {
        BufMatch hm;
        check(buf_find(b, "helper", 0, true, false, &hm),
              "find helper before relations");
        b->cursor = (int)hm.pos;
        app_show_context(&a);
        check(a.context_need_relations,
              "call flow asks for Alt+R without relations");
        app_render(&a);
    }
    {
        char *rerr = NULL;
        check(project_build_relations(&a.project, NULL, NULL, &rerr),
              "UI builds relation tables");
        free(rerr);
    }
    app_render(&a);

    /* Hovering a splitter arms the resize indicator before any drag. */
    app_handle_mouse_event(&a, a.lw, a.content_top + 4, 35, false);
    check(a.hover_mode == 1, "hover shows left/right splitter");
    app_handle_mouse_event(&a, a.lw + 3, a.split_row, 35, false);
    check(a.hover_mode == 3, "hover shows up/down splitter");
    app_handle_mouse_event(&a, a.lw + 3, a.split_row + 1, 35, false);
    check(a.hover_mode == 0, "hover clears off splitter");
    app_handle_mouse_event(&a, a.lw + a.cw + 3, a.right_split_row, 35,
                           false);
    check(a.hover_mode == 4, "hover shows call-flow splitter");
    app_render(&a);

    /* Drag the vertical/horizontal splitters. */
    {
        int old_lw = a.lw;
        int old_rw = a.rw;
        int old_top = a.top_h;
        int old_rtop = a.right_top_h;
        int rx;
        app_handle_mouse_event(&a, a.lw, a.content_top + 4, 0, false);
        app_handle_mouse_event(&a, a.lw + 4, a.content_top + 4, 32, false);
        app_handle_mouse_event(&a, a.lw + 4, a.content_top + 4, 0, true);
        check(a.lw != old_lw, "left splitter drag resizes columns");

        app_handle_mouse_event(&a, a.lw + 5, a.split_row, 0, false);
        app_handle_mouse_event(&a, a.lw + 5, a.split_row + 2, 32, false);
        app_handle_mouse_event(&a, a.lw + 5, a.split_row + 2, 0, true);
        check(a.top_h != old_top, "center splitter drag resizes rows");

        {
            int rsep = a.lw + 1 + a.cw;
            app_handle_mouse_event(&a, rsep, a.content_top + 4, 0, false);
            app_handle_mouse_event(&a, rsep - 4, a.content_top + 4, 32,
                                   false);
            app_handle_mouse_event(&a, rsep - 4, a.content_top + 4, 0, true);
            check(a.rw != old_rw, "right splitter drag resizes columns");
        }

        rx = a.lw + a.cw + 2;
        app_handle_mouse_event(&a, rx, a.right_split_row, 0, false);
        app_handle_mouse_event(&a, rx, a.right_split_row + 2, 32, false);
        app_handle_mouse_event(&a, rx, a.right_split_row + 2, 0, true);
        check(a.right_top_h != old_rtop, "right splitter drag resizes rows");
    }

    /* Clicking the left pane activates the outline. */
    app_handle_mouse_event(&a, 3, a.content_top, 0, false);
    check(a.focus == FOCUS_OUTLINE, "mouse activates outline");

    /* Right-top file list: first click activates, second click opens. */
    {
        int rx = a.lw + a.cw + 4;
        app_handle_mouse_event(&a, rx, a.content_top + 1, 0, false);
        check(a.focus == FOCUS_PROJECT, "mouse activates project files");
        app_handle_mouse_event(&a, rx, a.content_top + 1, 0, false);
        check(strcmp(app_buffer(&a)->name, "util.c") == 0,
              "second click opens second file");
        app_handle_mouse_event(&a, rx, a.content_top, 0, false);
        app_handle_mouse_event(&a, rx, a.content_top, 0, false);
        check(strcmp(app_buffer(&a)->name, "main.c") == 0,
              "second click opens first file");
    }

    /* F2 and F4 focus the outline/editor without function-key aliases. */
    key(&a, KEY_F2, NULL);
    check(a.focus == FOCUS_OUTLINE, "F2 focuses outline");
    key(&a, KEY_F4, NULL);
    check(a.focus == FOCUS_EDITOR, "F4 focuses editor");

    app_handle_mouse_event(&a, a.lw + 3,
                           a.content_top + a.top_h + 1, 0, false);
    check(a.focus == FOCUS_INFO, "mouse activates symbol info");
    key(&a, KEY_F4, NULL);

    /* Bottom info pane resolves globals and locals. */
    {
        BufMatch m;
        check(buf_find(b, "global_counter", 0, true, false, &m),
              "find global");
        b->cursor = (int)m.pos;
        app_update_info(&a);
        check(a.info_valid && strstr(a.info_type, "int") != NULL,
              "global type resolved");
        check(strstr(a.info_where, "main.c:23") != NULL,
              "global location resolved");

        check(buf_find(b, "util_global++", 0, true, false, &m),
              "find external global use");
        b->cursor = (int)m.pos;
        app_update_info(&a);
        check(strstr(a.info_kind, "GLOBAL VARIABLE") != NULL,
              "external global kind");
        check(strstr(a.info_where, "util.c:3") != NULL,
              "external global definition file");
        check(a.info_buffer != NULL,
              "external global code buffer opened");
        {
            int hi = project_find_file_by_rel(&a.project, "util.h");
            int mi = project_find_file_by_rel(&a.project, "main.c");
            check(hi >= 0 && mi >= 0, "global test files indexed");
            app_open_file_index(&a, hi, 1, 0, false);
            b = app_buffer(&a);
            check(b && buf_find(b, "util_global", 0, true, false, &m),
                  "find current-file extern declaration");
            b->cursor = (int)m.pos;
            app_update_info(&a);
            check(strstr(a.info_where, "util.h:") != NULL,
                  "current file declaration wins for globals");
            app_open_file_index(&a, mi, 1, 0, false);
            b = app_buffer(&a);
        }
        {
            int ui = project_find_file_by_rel(&a.project, "util.c");
            int mi = project_find_file_by_rel(&a.project, "main.c");
            app_open_file_index(&a, ui, 1, 0, false);
            b = app_buffer(&a);
            check(b && buf_find(b, "global_counter", 0, true, false, &m),
                  "find duplicate global in util.c");
            b->cursor = (int)m.pos;
            app_update_info(&a);
            check(strstr(a.info_where, "util.c:") != NULL,
                  "duplicate global uses current file definition");
            app_open_file_index(&a, mi, 1, 0, false);
            b = app_buffer(&a);
        }
        app_render(&a);

        check(buf_find(b, "Point p", 0, true, false, &m),
              "find local declaration");
        b->cursor = (int)m.pos + 6;
        app_update_info(&a);
        check(a.info_valid && strcmp(a.info_type, "Point") == 0,
              "local type resolved");
        app_render(&a);

        check(buf_find(b, "value + 1", 0, true, false, &m),
              "find parameter use");
        b->cursor = (int)m.pos;
        app_update_info(&a);
        check(a.info_valid && strstr(a.info_type, "int") != NULL,
              "parameter type resolved");

        check(buf_find(b, "int shadow_value = 2", 0, true, false, &m),
              "find shadowing local");
        b->cursor = (int)m.pos + 4;
        app_update_info(&a);
        {
            int ln = buf_line_of_offset(b, (size_t)b->cursor) + 1;
            check(app_local_in_scope(&a, ln, "shadow_value", 12),
                  "local shadows global");
            check(strstr(a.info_type, "int") != NULL,
                  "shadowing local type");
            check(strstr(a.info_where, "main.c:28") != NULL,
                  "shadowing local location");
            app_ensure_visible(&a);
            app_render(&a);
        }
        check(buf_find(b, "int shadow_value = 1", 0, true, false, &m),
              "find shadowed global");
        {
            int ln = buf_line_of_offset(b, (size_t)m.pos) + 1;
            check(!app_local_in_scope(&a, ln, "shadow_value", 12),
                  "file-scope name is not local");
        }
    }

    /* Select "    P" on line 18 by dragging in the editor. */
    {
        int text_x;
        int line_y;
        b->scroll_line = 0;
        b->scroll_col = 0;
        buf_set_cursor(b, 0);
        app_ensure_visible(&a);
        text_x = a.lw + 1 + a.gutter_w + 3;
        line_y = a.content_top + 17;
        app_handle_mouse_event(&a, text_x, line_y, 0, false);
        app_handle_mouse_event(&a, text_x + 5, line_y, 32, false);
        app_handle_mouse_event(&a, text_x + 5, line_y, 0, true);
    }
    check(a.has_selection, "mouse drag creates selection");
    {
        int cx = a.lw + 1 + a.gutter_w + 4;
        int cy = a.content_top + 17;
        app_handle_mouse_event(&a, cx, cy, 2, false);
        check(a.context_menu_active && a.context_menu_copy,
              "right-click shows Copy for selection");
        app_render(&a);
        app_handle_key_event(&a, KEY_ENTER, NULL);
        check(!a.context_menu_active && a.clipboard_len == 5,
              "context menu Copy works");
    }
    app_handle_key_event(&a, 3, NULL); /* Ctrl+C */
    check(a.clipboard_len == 5, "selection copied");
    check(a.clipboard && memcmp(a.clipboard, "    P", 5) == 0,
          "clipboard content");
    {
        size_t old_len = app_buffer(&a)->len;
        app_handle_key_event(&a, 22, NULL); /* Ctrl+V */
        check(app_buffer(&a)->len == old_len + 5, "selection pasted");
        app_handle_key_event(&a, 26, NULL); /* Ctrl+Z */
        check(!buf_is_dirty(app_buffer(&a)), "paste undone");
    }
    {
        int cx = a.lw + 1 + a.gutter_w + 4;
        int cy = a.content_top + 17;
        app_handle_mouse_event(&a, cx, cy, 2, false);
        check(a.context_menu_active && a.context_menu_paste,
              "right-click enables Paste from clipboard");
        app_render(&a);
        app_handle_key_event(&a, KEY_ESC, NULL);
        check(!a.context_menu_active, "context menu closes with Esc");
    }

    {
        BufMatch mm;
        check(buf_find(b, "p.x", 0, true, false, &mm),
              "find struct member use");
        b->cursor = (int)mm.pos + 2;
        app_update_info(&a);
        check(strcmp(a.info_kind, "STRUCT MEMBER") == 0,
              "struct member symbol info");
        check(strstr(a.info_where, "main.c:7") != NULL,
              "struct member definition location");
        check(strchr(a.info_where, '/') != NULL ||
                  strchr(a.info_where, '\\') != NULL,
              "symbol info uses a full path");
        check(a.file_sel == a.info_def_file,
              "project file list selects definition file");
        app_show_context(&a);
        check(a.view.kind == VIEW_CONTEXT &&
                  strstr(a.view.title, "CALL FLOW") != NULL,
              "struct member call flow");
        app_render(&a);
        {
            int cx = a.lw + 2;
            int cy = a.content_top + a.top_h + 1;
            app_handle_mouse_event(&a, cx, cy, 0, false);
            app_handle_mouse_event(&a, cx, cy, 0, false);
            check(a.focus == FOCUS_EDITOR,
                  "double-click SYMBOL INFO edits definition");
        }
    }
    {
        BufMatch lm;
        check(buf_find(b, "int value", 0, true, false, &lm),
              "find local variable for call flow");
        b->cursor = (int)lm.pos + 4;
        app_show_context(&a);
        check(a.view.kind == VIEW_CONTEXT &&
                  strstr(a.view.title, "value") != NULL,
              "local variable call flow built");
        app_render(&a);
    }
    {
        BufMatch m;
        check(buf_find(b, "helper", 0, true, false, &m),
              "find helper for context");
        b->cursor = (int)m.pos;
        app_update_info(&a);
        app_layout(&a);
        check(a.info_def_end - a.info_def_line >= 3,
              "function info keeps full definition range");
        {
            int keep_h = a.bottom_h;
            app_update_info(&a);
            app_layout(&a);
            check(a.bottom_h == keep_h,
                  "long definition respects user CODE pane height");
        }
        a.focus = FOCUS_EDITOR;
        app_sync_callflow(&a);
        check(a.view.kind == VIEW_CONTEXT, "context follows cursor");
        check(strstr(a.view.title, "helper") != NULL,
              "context title has symbol");
        check(a.view.count > 1, "context has caller route");
        app_render(&a); /* exercise the CALL FLOW graph renderer */
    }
    {
        BufMatch m2;
        check(buf_find(b, "add(", 0, true, false, &m2),
              "find add for multi-caller graph");
        b->cursor = (int)m2.pos;
        app_show_context(&a);
        check(a.view.count > 2, "context has multiple caller nodes");
        a.focus = FOCUS_CONTEXT;
        app_render(&a); /* expanded CALL FLOW graph */
        check(a.focus == FOCUS_CONTEXT, "expanded call flow stays focused");
        {
            size_t bi;
            int clicked = 0;
            for (bi = 0; bi < a.call_box_count; bi++) {
                const CallFlowBox *box = &a.call_boxes[bi];
                const char *label = a.view.items[box->item].label;
                if (strstr(label, "main") != NULL) {
                    int cx = a.call_box_x + box->col + 2;
                    int cy = a.call_box_top + box->row -
                             a.call_box_scroll + 1;
                    app_handle_mouse_event(&a, cx, cy, 0, false);
                    clicked = 1;
                    break;
                }
            }
            check(clicked && a.focus == FOCUS_EDITOR,
                  "clicking a call-flow box jumps to call site");
        }
    }

    len_before = b->len;
    key(&a, KEY_F1, NULL);
    check(a.help, "help opened");
    app_render(&a);
    key(&a, KEY_CHAR, "x");
    check(!a.help, "help closed");
    check(b->len == len_before, "help key not inserted");

    key(&a, KEY_F3, NULL);
    check(a.focus == FOCUS_CONTEXT, "F3 focuses call flow");
    key(&a, KEY_DOWN, NULL);
    key(&a, KEY_ENTER, NULL);
    check(a.focus == FOCUS_EDITOR, "call-flow enter returns to editor");

    key(&a, 16, NULL); /* Ctrl+P */
    check(a.prompt == PROMPT_SYMBOL, "symbol prompt");
    type_text(&a, "main");
    check(a.view.count >= 1, "symbol search result");
    key(&a, KEY_ENTER, NULL);
    check(strcmp(app_buffer(&a)->name, "main.c") == 0, "jumped to main.c");

    key(&a, KEY_F12, NULL);
    check(strcmp(app_buffer(&a)->name, "main.c") == 0, "go to definition");
    key(&a, KEY_SHIFT_F12, NULL);
    check(a.view.kind == VIEW_REFS, "references view");
    check(a.view.count >= 1, "references found");

    key(&a, KEY_ALT_CHAR, "g"); /* Alt+G: project text search */
    type_text(&a, "GREETING");
    key(&a, KEY_ENTER, NULL);
    check(a.view.kind == VIEW_TEXT, "text search view");
    check(a.view.count >= 2, "text search results");

    key(&a, 7, NULL); /* Ctrl+G: go to line */
    check(a.prompt == PROMPT_GOTO, "Ctrl+G opens line prompt");
    app_render(&a);
    check(a.show_cursor, "go-to-line prompt shows a cursor");
    type_text(&a, "x9");
    check(strcmp(a.prompt_text, "9") == 0,
          "go-to-line input accepts digits only");
    key(&a, KEY_ENTER, NULL);
    check(a.prompt == PROMPT_NONE &&
              buf_line_of_offset(app_buffer(&a),
                                 (size_t)app_buffer(&a)->cursor) + 1 == 9,
          "Ctrl+G jumps to requested line");

    key(&a, 6, NULL); /* Ctrl+F: find prompt / next */
    type_text(&a, "return");
    key(&a, KEY_ENTER, NULL);
    check(a.find_len == 6, "find highlighted");
    {
        BufMatch vm;
        int line;
        int col;
        int mx;
        int my;
        check(buf_find(app_buffer(&a), "value", 0, true, false, &vm),
              "find variable for highlight");
        app_buffer(&a)->cursor = (int)vm.pos;
        line = buf_line_of_offset(app_buffer(&a),
                                  (size_t)app_buffer(&a)->cursor);
        col = buf_visual_col(app_buffer(&a),
                             (size_t)app_buffer(&a)->cursor);
        mx = a.lw + 1 + a.gutter_w + 3 + col -
             app_buffer(&a)->scroll_col;
        my = a.content_top + line - app_buffer(&a)->scroll_line;
        app_handle_mouse_event(&a, mx, my, 35, false);
    }
    key(&a, KEY_F8, NULL);
    check(a.highlight_active &&
              strcmp(a.highlight_word, "value") == 0,
          "F8 highlights word under mouse");
    app_render(&a);
    key(&a, KEY_F8, NULL);
    check(!a.highlight_active, "F8 clears variable highlight");
    {
        int line = buf_line_of_offset(app_buffer(&a),
                                      (size_t)app_buffer(&a)->cursor);
        check(line >= 0, "find line valid");
    }

    type_text(&a, "// smoke");
    key(&a, KEY_ENTER, NULL);
    check(buf_is_dirty(app_buffer(&a)), "edit marks dirty");
    for (i = 0; i < 20 && buf_is_dirty(app_buffer(&a)); i++)
        key(&a, 26, NULL); /* Ctrl+Z */
    check(!buf_is_dirty(app_buffer(&a)), "undo returns to saved state");

    /* Duplicate function names: choose a definition, then jump to a call
     * site (not the caller's first line). */
    {
        Buffer *mb;
        BufMatch m;
        int chosen;
        int chosen_file;
        int chosen_line;
        int caller_line;
        int caller_file;
        size_t view_count;
        app_open_path(&a, "main.c", 1, false);
        mb = app_buffer(&a);
        check(mb && strcmp(mb->name, "main.c") == 0, "main reopened");
        check(buf_find(mb, "static int usage", 0, true, false, &m),
              "find duplicate usage definition");
        mb->cursor = (int)m.pos + 11;
        app_show_context(&a);
        check(!a.chooser_active,
              "current file definition wins without chooser");
        chosen = a.context_sym;
        chosen_file = a.project.syms[chosen].file_index;
        chosen_line = a.project.syms[chosen].line;
        check(a.view.count > 1 && a.view.items[0].line == chosen_line,
              "call flow root is current-file definition");
        a.focus = FOCUS_CONTEXT;
        {
            size_t vi;
            caller_file = -1;
            caller_line = 0;
            for (vi = 1; vi < a.view.count; vi++) {
                if (a.view.items[vi].file_index >= 0) {
                    caller_file = a.view.items[vi].file_index;
                    caller_line = a.view.items[vi].line;
                    break;
                }
            }
        }
        check(caller_file >= 0, "call flow has a caller node");
        view_count = a.view.count;
        key(&a, KEY_DOWN, NULL);
        key(&a, KEY_ENTER, NULL);
        check(strcmp(app_buffer(&a)->path,
                     a.project.files[caller_file].path) == 0,
              "call-flow jump opens caller file");
        check(buf_line_of_offset(app_buffer(&a),
                                 (size_t)app_buffer(&a)->cursor) + 1 ==
                  caller_line,
              "call-flow jump lands on call site");
        check(a.view.kind == VIEW_CONTEXT && a.view.count == view_count,
              "call-flow view is kept after jump");
        (void)chosen_file;
    }
    {
        const char *tmp = "tests/fixtures/demo/choice_tmp.c";
        const char *src = "int choice_tmp(void) { return usage(1); }\n";
        char *werr = NULL;
        BufMatch um;
        memset(&um, 0, sizeof(um));
        check(file_write_atomic(tmp, src, strlen(src), &werr),
              "write chooser fixture");
        free(werr);
        app_open_path(&a, "choice_tmp.c", 1, false);
        b = app_buffer(&a);
        check(b && buf_find(b, "usage", 0, true, false, &um),
              "find cross-file usage");
        b->cursor = (int)um.pos;
        check(app_active_file_index(&a) < 0,
              "external chooser file has no project index");
        app_show_context(&a);
        check(a.chooser_active && a.chooser_count >= 2,
              "multiple other-file definitions open chooser");
        key(&a, KEY_DOWN, NULL);
        key(&a, KEY_ENTER, NULL);
        check(!a.chooser_active &&
                  a.context_sym >= 0 &&
                  a.info_def_file >= 0,
              "chooser selects both info and call flow");
        remove(tmp);
    }

    /* Completion: identifier candidates, accept, Esc, literals. */
    {
        int cfi = project_find_file_by_rel(&a.project, "main.c");
        check(cfi >= 0, "completion main.c");
        if (cfi >= 0) {
            const char *anchor =
                "node.pos.x = result + other + local_p.x + local_q->x;";
            char *orig;
            size_t orig_len;
            int orig_cursor;
            int orig_scroll_line;
            int orig_scroll_col;
            app_open_file_index(&a, cfi, 1, 0, false);
            b = app_buffer(&a);
            orig = xstrndup(b->data, b->len);
            orig_len = b->len;
            orig_cursor = b->cursor;
            orig_scroll_line = b->scroll_line;
            orig_scroll_col = b->scroll_col;

            move_after_anchor(&a, anchor);
            type_text(&a, "res");
            check(a.comp_active, "completion opens for res");
            check(a.comp_count >= 1, "completion has candidates");
            check(a.comp_items[0].group == COMPLETE_GROUP_LOCAL &&
                  strcmp(a.comp_items[0].name, "result") == 0,
                  "local result comes first");
            key(&a, KEY_TAB, NULL);
            check(!a.comp_active, "tab closes completion");
            check(b->cursor >= 6 &&
                  memcmp(b->data + b->cursor - 6, "result", 6) == 0,
                  "tab inserts result");

            move_after_anchor(&a, anchor);
            type_text(&a, "oth");
            check(a.comp_active && a.comp_count >= 1 &&
                  strcmp(a.comp_items[0].name, "other") == 0,
                  "completion finds other");
            key(&a, KEY_ENTER, NULL);
            check(!a.comp_active, "enter closes completion");

            move_after_anchor(&a, anchor);
            type_text(&a, "hel");
            check(a.comp_active && a.comp_count == 1 &&
                  strcmp(a.comp_items[0].name, "helper") == 0,
                  "unique helper still listed");
            key(&a, KEY_ENTER, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "GRE");
            check(a.comp_active && completion_has(&a, "GREETING"),
                  "macro completion");
            key(&a, KEY_TAB, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "completion_");
            check(a.comp_active, "project symbol completion");
            check(completion_has(&a, "completion_demo"),
                  "function completion");
            check(completion_has(&a, "completion_origin"),
                  "global completion");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "po");
            check(!a.comp_active, "completion is case sensitive");

            move_after_anchor(&a, anchor);
            type_text(&a, "// re");
            check(!a.comp_active, "no completion in comment");

            move_after_anchor(&a, anchor);
            type_text(&a, "\"re\"");
            check(!a.comp_active, "no completion in string");

            move_after_anchor(&a, anchor);
            type_text(&a, "re");
            check(a.comp_active, "completion before Esc");
            key(&a, KEY_ESC, NULL);
            check(!a.comp_active, "Esc closes completion");
            type_text(&a, "s");
            check(!a.comp_active, "Esc suppresses same word");
            type_text(&a, " ");
            type_text(&a, "re");
            check(a.comp_active, "space resets suppression");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "helper");
            check(!a.comp_active, "full unique candidate hides list");

            {
                BufMatch m;
                check(buf_find(b, "result", 0, true, false, &m),
                      "find result for mid-word accept");
                b->cursor = (int)m.pos + 3;
                b->goal_col = buf_visual_col(b, (size_t)b->cursor);
                app_update_info(&a);
                app_completion_update(&a);
                check(a.comp_active, "mid-word completion opens");
                app_completion_accept(&a);
                check(b->cursor == (int)m.pos + 6,
                      "mid-word accept moves cursor to word end");
                check(!buf_find(b, "resultult", 0, true, false, &m),
                      "mid-word accept has no duplicate suffix");
                app_update_info(&a);
            }

            move_after_anchor(&a, anchor);
            type_text(&a, "co");
            check(a.comp_active && a.comp_count >= 2, "co has candidates");
            {
                int cur = b->cursor;
                char chosen[256];
                key(&a, KEY_DOWN, NULL);
                check(a.comp_sel == 1, "down moves completion selection");
                check(b->cursor == cur, "down does not move editor cursor");
                snprintf(chosen, sizeof(chosen), "%s",
                         a.comp_items[1].name);
                key(&a, KEY_TAB, NULL);
                check(!a.comp_active, "tab accepts selected item");
                check(b->cursor >= (int)strlen(chosen) &&
                      memcmp(b->data + b->cursor - strlen(chosen), chosen,
                             strlen(chosen)) == 0,
                      "selected item inserted");
            }

            move_after_anchor(&a, anchor);
            type_text(&a, "co");
            key(&a, KEY_END, NULL);
            check(a.comp_sel == (int)a.comp_count - 1, "End selects last");
            key(&a, KEY_HOME, NULL);
            check(a.comp_sel == 0, "Home selects first");
            key(&a, KEY_PGDN, NULL);
            check(a.comp_sel > 0, "PgDn moves selection");
            key(&a, KEY_ESC, NULL);
            check(!a.comp_active, "Esc closes after navigation");

            move_after_anchor(&a, "util_global++; return add(p.x, p.y);");
            type_text(&a, "Point");
            check(!a.comp_active, "full Point hides list");
            check(a.comp_suppress, "Point suppression armed");
            check(!a.chooser_active, "auto chooser defers to completion");
            key(&a, KEY_F12, NULL);
            check(a.chooser_active ||
                  (a.focus == FOCUS_CONTEXT && a.view.count >= 2),
                  "F12 still resolves definitions");
            if (a.chooser_active)
                key(&a, KEY_ESC, NULL);
            else
                key(&a, KEY_F4, NULL);
            check(!a.comp_active, "F12 does not leave completion open");

            move_after_anchor(&a, "util_global++; return add(p.x, p.y);");
            type_text(&a, "Poin");
            check(a.comp_active, "Poin completion active");
            key(&a, KEY_TAB, NULL);
            check(!a.comp_active, "tab accepts Poin");
            check(!a.chooser_active, "accepted duplicate stays non-modal");

            move_after_anchor(&a, anchor);
            type_text(&a, "re");
            check(a.comp_active, "completion before Ctrl+F");
            a.last_find[0] = '\0';
            key(&a, 6, NULL); /* Ctrl+F */
            check(!a.comp_active && a.prompt == PROMPT_FIND,
                  "Ctrl+F closes completion and opens find");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "re");
            check(a.comp_active, "completion before Alt+M");
            key(&a, KEY_ALT_CHAR, "m");
            check(!a.comp_active && a.menu_active,
                  "Alt+M closes completion and opens menu");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "node.p");
            check(a.comp_active && a.comp_count == 1 &&
                  strcmp(a.comp_items[0].name, "pos") == 0,
                  "member prefix filters");

            move_after_anchor(&a, anchor);
            type_text(&a, "node.");
            check(a.comp_active && a.comp_member && a.comp_count == 4 &&
                  completion_has(&a, "pos") &&
                  completion_has(&a, "next") &&
                  completion_has(&a, "alpha") &&
                  completion_has(&a, "beta"), "node has four members");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "node.pos.");
            check(a.comp_active && a.comp_member && a.comp_count == 2 &&
                  completion_has(&a, "x") && completion_has(&a, "y"),
                  "chained value member list");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "node.next->pos.");
            check(a.comp_active && a.comp_member && a.comp_count == 2 &&
                  completion_has(&a, "x") && completion_has(&a, "y"),
                  "chained pointer member list");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "local_q->");
            check(a.comp_active && a.comp_member && a.comp_count == 2 &&
                  completion_has(&a, "x") && completion_has(&a, "y"),
                  "local pointer member list");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "unknown_value.");
            check(!a.comp_active, "unknown receiver has no members");

            move_after_anchor(&a, anchor);
            type_text(&a, "completion_arr[0].");
            check(!a.comp_active, "indexed expression has no members");

            move_after_anchor(&a,
                              "util_global++; return add(p.x, p.y);");
            type_text(&a, "p.");
            check(a.comp_active && a.comp_member, "p. opens member list");
            check(a.comp_count == 2 && completion_has(&a, "x") &&
                  completion_has(&a, "y"), "Point has x/y members");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a,
                              "util_global++; return add(p.x, p.y);");
            type_text(&a, "p.");
            check(a.comp_active, "p. reopens member list");
            key(&a, KEY_DOWN, NULL);
            key(&a, KEY_TAB, NULL);
            check(b->cursor >= 1 && b->data[b->cursor - 1] == 'y',
                  "member accept inserts y");

            move_after_anchor(&a,
                              "util_global++; return add(p.x, p.y);");
            type_text(&a, "completion_origin.");
            check(a.comp_active && a.comp_member && a.comp_count == 2,
                  "global struct member list");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a,
                              "util_global++; return add(p.x, p.y);");
            type_text(&a, "completion_world.");
            check(a.comp_active && a.comp_member && a.comp_count == 2,
                  "global typedef value member list");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a,
                              "util_global++; return add(p.x, p.y);");
            type_text(&a, "completion_ptr->");
            check(a.comp_active && a.comp_member && a.comp_count == 2,
                  "global pointer member list");
            key(&a, KEY_ESC, NULL);

            move_after_anchor(&a, anchor);
            type_text(&a, "co");
            check(a.comp_active, "popup active for geometry");
            app_render(&a);
            check(a.comp_popup_w > 0 && a.comp_popup_h > 0,
                  "completion popup has geometry");
            check(a.comp_popup_y >= a.content_top,
                  "popup inside content");
            check(a.comp_popup_y + a.comp_popup_h <=
                      a.content_top + a.content_height,
                  "popup avoids hint/status");
            check(a.comp_popup_rows >= 1, "popup has visible rows");
            {
                int before = (int)a.comp_count;
                int old_rows = a.force_rows;
                int old_cols = a.force_cols;
                a.force_rows = 14;
                a.force_cols = 80;
                app_layout(&a);
                app_render(&a);
                check(a.comp_active && (int)a.comp_count == before,
                      "small screen keeps all candidates");
                check(a.comp_popup_w > 0 && a.comp_popup_rows < before,
                      "small screen scrolls instead of truncating");
                a.force_rows = old_rows;
                a.force_cols = old_cols;
                app_layout(&a);
                app_render(&a);
            }
            key(&a, KEY_ESC, NULL);

            app_completion_reset(&a);
            buf_delete_range(b, 0, b->len);
            buf_insert_at(b, 0, orig, orig_len);
            b->cursor = orig_cursor;
            b->goal_col = buf_visual_col(b, (size_t)b->cursor);
            b->scroll_line = orig_scroll_line;
            b->scroll_col = orig_scroll_col;
            b->saved_state = b->state_id;
            free(orig);
            a.locals_buf = -1;
            app_update_info(&a);
        }
    }

    /* Menu, project switching, and the ANSI formatter. */
    key(&a, KEY_ALT_CHAR, "m");
    check(a.menu_active, "Alt+M opens menu");
    app_render(&a);
    key(&a, KEY_ESC, NULL);
    check(!a.menu_active, "Esc closes menu");
    app_handle_mouse_event(&a, 1, 0, 0, false);
    check(a.menu_active, "top-left menu label opens dropdown");
    app_render(&a);
    key(&a, KEY_ESC, NULL);

    key(&a, 14, NULL); /* Ctrl+N */
    check(a.dir_active && a.dir_for_new, "Ctrl+N opens new-project folder picker");
    key(&a, KEY_ESC, NULL);
    key(&a, 15, NULL); /* Ctrl+O */
    check(a.dir_active && !a.dir_for_new, "Ctrl+O opens project folder picker");
    key(&a, KEY_ESC, NULL);
    key(&a, KEY_F6, NULL);
    check(a.prompt == PROMPT_FORMAT, "F6 format code");
    key(&a, KEY_ESC, NULL);
    key(&a, 8, NULL); /* Ctrl+H */
    check(a.help, "Ctrl+H help");
    key(&a, KEY_ESC, NULL);
    key(&a, 1, NULL); /* Ctrl+A */
    check(!a.prompt, "Ctrl+A save all");

    key(&a, KEY_ALT_CHAR, "m");
    key(&a, KEY_ENTER, NULL); /* Open Project Directory... */
    check(a.dir_active && !a.dir_for_new, "menu opens project folder picker");
    key(&a, KEY_ENTER, NULL); /* [ Use this folder ] */
    check(!a.dir_active && app_buffer(&a) != NULL,
          "project reloaded from menu");
    {
        Buffer *hb = app_buffer(&a);
        free(a.home.path);
        a.home.path = xstrdup(hb->path);
        a.home.cursor = hb->cursor;
        a.home.scroll_line = hb->scroll_line;
        a.home.scroll_col = hb->scroll_col;
        a.home_valid = true;
        app_open_path(&a, "util.c", 1, true);
        check(strcmp(app_buffer(&a)->name, "util.c") == 0,
              "navigated away from home");
        key(&a, KEY_F10, NULL);
        check(strcmp(app_buffer(&a)->name, "main.c") == 0,
              "F10 goes back");
        key(&a, KEY_CTRL_F10, NULL);
        check(strcmp(app_buffer(&a)->name, "util.c") == 0,
              "Ctrl+F10 goes forward");
        key(&a, KEY_CTRL_F10, NULL);
        check(strcmp(app_buffer(&a)->name, "main.c") == 0,
              "forward falls back to first location");
    }

    key(&a, KEY_ALT_CHAR, "m");
    key(&a, KEY_DOWN, NULL);
    key(&a, KEY_ENTER, NULL); /* New Project... */
    check(a.dir_active && a.dir_for_new,
          "menu opens new-project folder picker");
    key(&a, KEY_ESC, NULL);

    {
        char tmp[1024];
        static const char ugly[] =
            "#define M(x) do { \\\n"
            "    x; \\\n"
            "} while (0)\n"
            "int f()\n"
            "{\n"
            "if(1){\n"
            "return 1;\n"
            "}\n"
            "}\n";
        char *werr = NULL;
        Buffer *fb;
        snprintf(tmp, sizeof(tmp), "%s/fmt_tmp.c",
                 a.project.root ? a.project.root : ".");
        check(file_write_atomic(tmp, ugly, sizeof(ugly) - 1, &werr),
              "write format fixture");
        free(werr);
        app_open_path(&a, tmp, 1, false);
        fb = app_buffer(&a);
        check(fb != NULL, "open format fixture");
        key(&a, KEY_F6, NULL);
        check(a.prompt == PROMPT_FORMAT, "F6 opens format choice");
        key(&a, KEY_CHAR, "c");
        check(strstr(fb->data, "    if(1){") != NULL,
              "formatter indents nested block");
        check(strstr(fb->data, "#define M(x)") != NULL,
              "formatter preserves macro line");
        key(&a, 19, NULL); /* Ctrl+S: make the buffer clean */
        check(!buf_is_dirty(fb), "formatted buffer saved");
        remove(tmp);
    }

    /* Relation tables modal: Enter is the OK confirmation. */
    {
        char *rerr = NULL;
        check(project_build_relations(&a.project, NULL, NULL, &rerr),
              "UI relation tables build");
        a.relation_edges = project_call_edge_count(&a.project);
        a.relation_vars = project_var_use_count(&a.project);
        a.relation_ready_prompt = true;
        app_render(&a);
        key(&a, KEY_ENTER, NULL);
        check(!a.relation_ready_prompt, "relation OK confirmation");
        free(rerr);
    }

    /* New-project flow and format-all. */
    {
        const char *project_dir = "tests/fixtures/demo/newproj_tmp";
        char path[1024];
        remove("tests/fixtures/demo/newproj_tmp/main.c");
        key(&a, 14, NULL); /* Ctrl+N */
        check(a.dir_active && a.dir_for_new, "new project folder picker");
        key(&a, KEY_ENTER, NULL); /* use current folder as parent */
        check(a.prompt == PROMPT_PROJECT_NAME, "new project name prompt");
        app_render(&a);
        check(a.show_cursor && a.cursor_y > a.rows / 3 &&
                  a.cursor_y < a.rows * 2 / 3,
              "new project prompt is centered");
        type_text(&a, "newproj_tmp");
        key(&a, KEY_ENTER, NULL);
        snprintf(path, sizeof(path), "%s/main.c", project_dir);
        check(path_exists(path), "new project creates main.c");
        check(strstr(app_buffer(&a)->path, "newproj_tmp") != NULL,
              "new project opened");
        key(&a, KEY_F6, NULL);
        key(&a, KEY_CHAR, "a");
        check(!a.prompt, "format-all completed");
        remove("tests/fixtures/demo/.si-index");
        remove(path);
        {
            char cache[1024];
            snprintf(cache, sizeof(cache), "%s/.si-index", project_dir);
            remove(cache);
        }
#ifdef _WIN32
        _rmdir(project_dir);
#else
        rmdir(project_dir);
#endif
    }

    key(&a, KEY_CHAR, "x");
    check(buf_is_dirty(app_buffer(&a)), "dirty buffer for quit prompt");
    key(&a, 17, NULL); /* Ctrl+Q */
    check(a.prompt == PROMPT_QUIT, "Ctrl+Q asks before discarding");
    app_render(&a);
    key(&a, KEY_CHAR, "c"); /* cancel */
    check(a.prompt == PROMPT_NONE && !a.quit,
          "quit confirmation can be cancelled");
    key(&a, 26, NULL); /* undo inserted x */
    key(&a, 17, NULL); /* Ctrl+Q */
    check(a.quit, "quit after clean buffer");

    /* Recent-project picker: selection, Esc, empty state, outside click. */
    a.force_rows = 24;
    a.force_cols = 80;
    app_layout(&a);
    strlist_push(&a.recent_projects, root);
    app_project_picker_open(&a);
    check(a.project_picker_active, "recent project picker opens");
    check(a.project_sel == 0, "recent project picker selects first");
    app_render(&a);
    key(&a, KEY_ESC, NULL);
    check(!a.project_picker_active, "recent project picker closes");
    check(a.project.root != NULL, "project remains after picker closes");

    strlist_free(&a.recent_projects);
    strlist_init(&a.recent_projects);
    app_project_picker_open(&a);
    key(&a, KEY_ENTER, NULL);
    check(a.project_picker_active, "empty picker stays open");
    check(strstr(a.status, "No recent project selected") != NULL,
          "empty picker reports no selection");
    app_handle_mouse_event(&a, 0, 0, 0, false);
    check(!a.project_picker_active, "empty picker closes on outside click");

    {
        App empty;
        memset(&empty, 0, sizeof(empty));
        strlist_init(&empty.recent_projects);
        empty.active = -1;
        empty.prompt = PROMPT_NONE;
        empty.focus = FOCUS_EDITOR;
        app_use_theme(&empty, "dark");
        empty.force_rows = 24;
        empty.force_cols = 80;
        app_layout(&empty);
        check(empty.project.root == NULL, "empty app has no project");
        app_project_picker_open(&empty);
        app_render(&empty);
        check(empty.project_picker_active, "empty app picker opens");
        key(&empty, KEY_ESC, NULL);
        check(!empty.project_picker_active, "empty app picker closes");
        app_render(&empty);
        app_view_clear(&empty.view);
        free(empty.view.items);
        free(empty.call_boxes);
        app_view_clear(&empty.outline);
        free(empty.outline.items);
        buf_free(empty.info_buffer);
        strlist_free(&empty.recent_projects);
    }

    a.force_rows = 24;
    a.force_cols = 80;
    app_layout(&a);
    check(!a.too_small && a.rw > 0, "80x24 keeps all panes");
    app_render(&a);

    a.force_rows = 12;
    a.force_cols = 60;
    app_layout(&a);
    check(!a.too_small && a.rw == 0, "60x12 hides symbol pane");
    app_render(&a);

    a.force_rows = 10;
    a.force_cols = 50;
    app_layout(&a);
    check(a.too_small, "50x10 reports too small");
    app_render(&a);

    fprintf(stderr, "UI smoke: %d checks, %d failures\n", checks, failures);
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
    for (i = 0; i < a.nbuf; i++)
        buf_free(a.bufs[i]);
    free(a.bufs);
    for (i = 0; i < (int)a.nback; i++)
        free(a.back[i].path);
    for (i = 0; i < (int)a.nfwd; i++)
        free(a.fwd[i].path);
    free(a.back);
    free(a.fwd);
    free(a.home.path);
    strlist_free(&a.recent_projects);
    strlist_free(&a.globals);
    strlist_free(&a.types);
    strlist_free(&a.macros);
    for (i = 0; i < (int)a.nlocals; i++) {
        free(a.locals[i].name);
        free(a.locals[i].type);
    }
    free(a.locals);
    free(a.clipboard);
    project_free(&a.project);
    return failures ? 1 : 0;
}
