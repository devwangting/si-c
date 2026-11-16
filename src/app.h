#ifndef SI_APP_H
#define SI_APP_H

#include "si.h"
#include "index.h"
#include "syntax.h"
#include "term.h"
#include "text.h"

typedef enum {
    FOCUS_OUTLINE = 0,
    FOCUS_EDITOR,
    FOCUS_INFO,
    FOCUS_PROJECT,
    FOCUS_CONTEXT
} Focus;

typedef enum {
    PROMPT_NONE = 0,
    PROMPT_FIND,
    PROMPT_GREP,
    PROMPT_SYMBOL,
    PROMPT_OPEN,
    PROMPT_GOTO,
    PROMPT_QUIT,
    PROMPT_PROJECT,
    PROMPT_NEW_PROJECT,
    PROMPT_PROJECT_NAME,
    PROMPT_FORMAT
} PromptKind;

typedef enum {
    VIEW_NONE = 0,
    VIEW_FILE,
    VIEW_OUTLINE,
    VIEW_DEFS,
    VIEW_REFS,
    VIEW_TEXT,
    VIEW_SYMBOLS,
    VIEW_CONTEXT
} ViewKind;

typedef struct {
    const char *name;
    int colors; /* 16 or 256 */
    int bg;
    int panel;
    int panel_alt;
    int current;
    int status_bg;
    int accent_bg;
    int border;
    int fg;
    int dim;
    int selection_fg;
    int syn[11];
} Theme;

typedef struct {
    char *label;
    char *detail;
    int file_index;
    int line;
    int col;
    int sym_index;
    int depth;
    bool is_header;
} ViewItem;

typedef struct {
    int item; /* index into App.view.items */
    int row;  /* graph-relative row */
    int col;  /* graph-relative column */
    int w;
} CallFlowBox;

typedef struct {
    ViewItem *items;
    size_t count;
    size_t cap;
    char title[96];
    ViewKind kind;
} View;

typedef struct {
    char *path;
    int cursor;
    int scroll_line;
    int scroll_col;
} NavLoc;

typedef struct {
    char *name;
    char *type;
    int line;
    int end_line;
} LocalVar;

typedef struct {
    int sym_index;
    char *code;
} ChooserItem;

typedef struct {
    const char *name;
    const char *detail;
    unsigned char group;
} CompleteItem;

enum {
    COMPLETE_GROUP_LOCAL = 0,
    COMPLETE_GROUP_GLOBAL,
    COMPLETE_GROUP_FUNCTION,
    COMPLETE_GROUP_MACRO,
    COMPLETE_GROUP_TYPE,
    COMPLETE_GROUP_KEYWORD,
    COMPLETE_GROUP_MEMBER
};

typedef struct {
    Project project;
    StrList recent_projects;
    StrList globals;
    StrList types;
    StrList macros;
    LocalVar *locals;
    size_t nlocals;
    size_t clocals;
    uint64_t locals_rev;
    int locals_buf;
    Buffer **bufs;
    int nbuf;
    int capbuf;
    int active;
    int active_project_file; /* cached project file index of active buffer */

    Focus focus;
    int outline_sel;
    int outline_scroll;
    int file_sel;
    int file_scroll;
    int sym_sel;
    int sym_scroll;
    int context_scroll; /* row offset of the CALL FLOW graph */
    bool highlight_active;
    char highlight_word[128];
    bool context_need_relations;
    int live_context_file;
    int live_context_line;
    char live_context_word[128];
    int pending_info_sym;
    char pending_info_word[128];
    long info_click_time;
    int info_click_x;
    int info_click_y;
    CallFlowBox *call_boxes;
    size_t call_box_count;
    size_t call_box_cap;
    int call_box_top;
    int call_box_x;
    int call_box_scroll;
    View outline;
    View view;

    bool chooser_active;
    ChooserItem *chooser_items;
    size_t chooser_count;
    char chooser_word[128];
    int chooser_sel;
    int chooser_scroll;
    int context_sym;
    bool pin_context;

    Symbol **sym_name_index;
    size_t sym_name_count;

    bool comp_active;
    bool comp_member;
    int comp_buf;
    int comp_line;
    int comp_word_start;
    int comp_word_end;
    int comp_prefix_len;
    char comp_prefix[256];
    CompleteItem *comp_items;
    size_t comp_count;
    size_t comp_cap;
    int comp_sel;
    int comp_scroll;
    int comp_popup_x;
    int comp_popup_y;
    int comp_popup_w;
    int comp_popup_h;
    int comp_popup_first;
    int comp_popup_rows;
    long comp_click_time;
    int comp_click_index;
    int comp_click_x;
    int comp_click_y;
    bool comp_suppress;
    int comp_suppress_buf;
    int comp_suppress_line;
    int comp_suppress_start;
    unsigned char comp_suppress_first;
    char comp_suppress_word[256];
    bool comp_suppress_member;

    bool relations_pending;
    bool relation_building;
    bool relation_ready_prompt;
    bool relations_large_prompt;
    bool relations_large_skipped;
    size_t relation_files;
    size_t relation_symbols;
    size_t relation_edges;
    size_t relation_vars;

    bool project_picker_active;
    int project_sel;
    int project_scroll;

    bool dir_active;
    bool dir_for_new;
    char dir_path[1024];
    char **dir_items;
    size_t dir_count;
    int dir_sel;
    int dir_scroll;
    char pending_parent[1024];

    bool menu_active;
    int menu_group;
    int menu_sel;
    bool context_menu_active;
    int context_menu_x;
    int context_menu_y;
    int context_menu_sel;
    bool context_menu_copy;
    bool context_menu_paste;

    NavLoc *back;
    size_t nback;
    size_t cback;
    NavLoc *fwd;
    size_t nfwd;
    size_t cfwd;
    NavLoc home;
    bool home_valid;

    char status[512];
    char last_find[256];
    char last_grep[256];

    PromptKind prompt;
    char prompt_text[512];
    int prompt_len;
    int prompt_cursor;
    Focus prompt_prev_focus;

    bool quit;
    bool help;
    bool too_small;
    bool view_stale;

    Theme theme;
    int rows;
    int cols;
    int force_rows;
    int force_cols;
    int lw;
    int rw;
    int cw;
    int left_pct;
    int right_pct;
    int center_top_pct;
    int right_top_pct;
    int drag_mode; /* 0 none, 1 left, 2 right, 3 center-h, 4 right-h */
    int hover_mode; /* same values as drag_mode, 0 = no splitter */
    int hover_x;
    int hover_y;
    int content_top;
    int content_height;
    int top_h;
    int bottom_h;
    int split_row;
    int right_top_h;
    int right_bottom_h;
    int right_split_row;
    int ed_h;
    int ed_w;
    int gutter_w;
    int cursor_y;
    int cursor_x;
    bool show_cursor;

    int find_pos;
    int find_len;

    bool info_valid;
    int info_line;
    int info_def_line;
    int info_def_end;
    int info_def_file;
    int info_code_start;
    int info_scroll;
    Buffer *info_buffer;
    char info_word[128];
    char info_type[256];
    char info_signature[512];
    char info_kind[16];
    char info_where[1024];
    char info_scope[128];
    char info_refs[64];

    int sel_buf;
    int sel_anchor;
    int sel_cursor;
    bool selecting;
    bool has_selection;
    char *clipboard;
    size_t clipboard_len;
} App;

void app_set_status(App *a, const char *fmt, ...);
void app_view_clear(View *v);
void app_refresh_outline(App *a);
void app_update_info(App *a);
void app_rebuild_locals(App *a);
bool app_local_in_scope(App *a, int line, const char *name, size_t len);
void app_show_context(App *a);
void app_sync_callflow(App *a);
void app_rebuild_globals(App *a);
void app_layout(App *a);
void app_ensure_visible(App *a);
Buffer *app_buffer(App *a);
int app_active_file_index(App *a);
void app_open_file_index(App *a, int fi, int line, int col, bool record);
int app_open_path(App *a, const char *path, int line, bool record);
void app_goto_location(App *a, int fi, int line, int col, bool record);
void app_push_location(App *a);
void app_go_back(App *a);
void app_go_forward(App *a);
void app_render(App *a);
void app_use_theme(App *a, const char *name);
void app_handle_key_event(App *a, int code, const char *text);
void app_handle_mouse_event(App *a, int x, int y, int button, bool release);

void app_completion_close(App *a);
void app_completion_update(App *a);
void app_completion_accept(App *a);
void app_completion_reset(App *a);
void app_completion_move(App *a, int delta);
void app_completion_fix(App *a);
bool app_completion_handle_key(App *a, KeyEvent *ev);
bool app_completion_handle_mouse(App *a, KeyEvent *ev);
bool app_completion_geometry(App *a, int *x, int *y, int *w, int *h,
                             int *first, int *rows);
bool app_completion_suppresses_word(App *a, const char *word);

#define APP_MENU_GROUPS 4
#define APP_PROJECT_PICKER_MAX_W 78
#define APP_PROJECT_PICKER_MAX_H 22

void app_project_picker_open(App *a);
void app_menu_tab_layout(App *a, int xs[APP_MENU_GROUPS],
                         int ws[APP_MENU_GROUPS]);
const char *app_menu_group_name(int group);
int app_menu_group_size(int group);
const char *app_menu_item_label(int group, int index);
const char *app_menu_item_key(int group, int index);
int app_menu_group_at(App *a, int x);
int app_menu_group_width(int group);

#endif /* SI_APP_H */
