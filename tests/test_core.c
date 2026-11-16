#include "si.h"
#include "index.h"
#include "syntax.h"
#include "text.h"

#include <math.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;
static int checks;
static int cache_ticks;

static bool cache_progress(void *ud, size_t files, size_t symbols,
                           const char *path)
{
    (void)ud;
    (void)files;
    (void)symbols;
    if (path && strncmp(path, "reading .si-index", 17) == 0)
        cache_ticks++;
    return true;
}

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void test_utf8(void)
{
    const char *s = "a\xe4\xb8\xad" "b";
    uint32_t cp = 0;
    int n;
    n = utf8_decode(s + 1, 3, &cp);
    check(n == 3, "utf8 decode length");
    check(cp == 0x4E2D, "utf8 decode codepoint");
    check(utf8_text_width(s, strlen(s)) == 4, "CJK display width");
    check(utf8_cp_width(0x0301) == 0, "combining width");
}

static void test_buffer(void)
{
    const char *text = "int main() {\n\treturn 0;\n}\n";
    Buffer *b = buf_create("sample.c", text, strlen(text));
    size_t start1;
    BufMatch m;

    check(buf_line_count(b) == 4, "line count including trailing blank");
    check(buf_line_len(b, 0) == 12, "first line length");
    check(buf_line_len(b, 1) == 10, "tab line length");
    start1 = buf_line_start(b, 1);
    check(buf_visual_col(b, start1 + 1) == 4, "tab visual column");
    check(buf_offset_at_visual(b, 1, 0) == start1, "visual offset at tab");
    check(buf_offset_at_visual(b, 1, 4) == start1 + 1,
          "visual offset after tab");

    check(!buf_is_dirty(b), "new buffer clean");
    buf_set_cursor(b, 0);
    buf_insert_at(b, 0, "// x\n", 5);
    check(buf_line_count(b) == 5, "insert newline increases line count");
    check(buf_is_dirty(b), "insert marks dirty");
    check(buf_undo(b), "undo insert");
    check(buf_line_count(b) == 4, "undo restores line count");
    check(!buf_is_dirty(b), "undo back to saved state clears dirty");
    check(buf_redo(b), "redo insert");
    check(buf_is_dirty(b), "redo marks dirty");
    check(buf_undo(b), "undo redo insert");

    buf_undo_break(b);
    buf_insert_at(b, 0, "a", 1);
    buf_insert_at(b, 1, "b", 1);
    check(b->len == strlen(text) + 2, "two inserts");
    check(buf_undo(b), "undo grouped inserts");
    check(b->len == strlen(text), "group undo removes both");
    check(buf_redo(b), "redo grouped inserts");
    check(memcmp(b->data, "ab", 2) == 0, "redo grouped text");
    check(buf_undo(b), "undo grouped again");

    buf_set_cursor(b, 0);
    check(buf_find(b, "return", 0, true, true, &m), "case-insensitive find");
    check(m.line == 1, "find line");
    check(buf_find(b, "int", 1, true, true, &m), "wrapped find");
    check(m.pos == 0, "wrapped find position");
    check(!buf_find(b, "not-there", 0, true, false, &m), "missing find");

    check(buf_word_at(b, 1, &start1, &m.len), "word at offset");
    check(start1 == 0 && m.len == 3, "word boundary");

    buf_free(b);
}

static void test_names(void)
{
    StrList names;
    strlist_init(&names);
    strlist_push(&names, "beta");
    strlist_push(&names, "alpha");
    strlist_push(&names, "gamma");
    strlist_sort(&names);
    check(strlist_contains_n(&names, "alpha", 5), "name set contains exact");
    check(!strlist_contains_n(&names, "alph", 4), "name set rejects prefix");
    check(!strlist_contains_n(&names, "delta", 5), "name set rejects miss");
    strlist_free(&names);
}

static void test_syntax(void)
{
    const char *text = "int main(void)\n{\n\treturn 0;\n}\n";
    Buffer *b = buf_create("sample.c", text, strlen(text));
    HlSpan spans[64];
    int n;
    int i;
    bool type_int = false;
    bool func_main = false;

    n = syntax_line_spans(&b->syn, b, 0, spans, (int)SI_ARRAY_LEN(spans));
    check(n >= 3, "syntax spans emitted");
    for (i = 0; i < n; i++) {
        if (spans[i].start == 0 && spans[i].end == 3 &&
            spans[i].kind == HL_TYPE)
            type_int = true;
        if (spans[i].start == 4 && spans[i].end == 8 &&
            spans[i].kind == HL_FUNC)
            func_main = true;
    }
    check(type_int, "int highlighted as type");
    check(func_main, "main highlighted as function");

    n = syntax_line_spans(&b->syn, b, 2, spans, (int)SI_ARRAY_LEN(spans));
    check(n >= 1, "body line spans emitted");
    for (i = 0; i < n; i++)
        check(spans[i].start >= 0, "spans are line-relative");
    buf_free(b);
}

static void test_project(const char *root)
{
    Project p;
    char *err = NULL;
    IntList ids;
    LocationList locs;
    size_t i;

    if (!project_load(&p, root, NULL, NULL, &err)) {
        fprintf(stderr, "FAIL: project load: %s\n", err ? err : "?");
        failures++;
        free(err);
        return;
    }
    check(p.file_count == 3, "fixture file count");
    check(p.sym_count > 5, "fixture symbol count");
    check(project_find_file_by_rel(&p, "main.c") >= 0, "find main.c by name");

    intlist_init(&ids);
    project_find_symbols(&p, "main", &ids);
    check(ids.count == 1, "exact main symbol");
    if (ids.count == 1) {
        Symbol *s = &p.syms[ids.items[0]];
        check(s->kind == SYM_FUNCTION, "main is a function");
        check(strcmp(p.files[s->file_index].rel, "main.c") == 0,
              "main file");
        check(s->line == 16, "main line");
    }
    intlist_free(&ids);

    intlist_init(&ids);
    project_find_symbols(&p, "add", &ids);
    check(ids.count == 2, "add has prototype and definition");
    intlist_free(&ids);

    {
        SymbolContext ctx;
        int main_idx = -1;
        int helper_idx = -1;
        size_t k;
        for (k = 0; k < p.sym_count; k++) {
            if (p.syms[k].kind == SYM_FUNCTION &&
                strcmp(p.syms[k].name, "main") == 0)
                main_idx = (int)k;
            if (p.syms[k].kind == SYM_FUNCTION &&
                strcmp(p.syms[k].name, "helper") == 0)
                helper_idx = (int)k;
        }
        check(main_idx >= 0 && helper_idx >= 0, "function symbols found");
        if (main_idx >= 0) {
            check(project_context(&p, main_idx, &ctx), "main context");
            check(ctx.callees.count >= 2, "main calls helper and add");
            check(ctx.callers.count == 0, "main has no callers");
            symbol_context_free(&ctx);
        }
        if (helper_idx >= 0) {
            LocationList tree;
            check(project_context(&p, helper_idx, &ctx), "helper context");
            check(ctx.callers.count == 1, "helper called by main");
            check(ctx.callees.count == 0, "helper calls nothing");
            symbol_context_free(&ctx);
            check(project_caller_tree(&p, helper_idx, &tree, 8),
                  "helper caller tree");
            check(tree.count == 1, "caller route has one node");
            check(tree.count > 0 && tree.items[0].depth == 1,
                  "caller route depth");
            check(tree.count > 0 && tree.items[0].line == 19,
                  "caller route points at call site");
            loclist_free(&tree);
        }
        {
            int add_idx = -1;
            LocationList tree;
            size_t j;
            int max_depth = 0;
            for (j = 0; j < p.sym_count; j++) {
                if (p.syms[j].kind == SYM_FUNCTION &&
                    strcmp(p.syms[j].name, "add") == 0)
                    add_idx = (int)j;
            }
            check(add_idx >= 0, "add definition found");
            if (add_idx >= 0) {
                check(project_caller_tree(&p, add_idx, &tree, 8),
                      "multi-level caller tree");
                check(tree.count >= 3, "call chain has multiple nodes");
                for (j = 0; j < tree.count; j++) {
                    if (tree.items[j].depth > max_depth)
                        max_depth = tree.items[j].depth;
                }
                check(max_depth >= 2, "call chain recurses past one level");
                {
                    size_t a;
                    size_t b;
                    bool duplicate = false;
                    for (a = 0; a < tree.count; a++) {
                        for (b = a + 1; b < tree.count; b++) {
                            if (tree.items[a].file_index ==
                                    tree.items[b].file_index &&
                                tree.items[a].line == tree.items[b].line)
                                duplicate = true;
                        }
                    }
                    check(!duplicate, "call route has no duplicate nodes");
                }
                loclist_free(&tree);
            }
        }
    }

    intlist_init(&ids);
    project_search_symbols(&p, "poin", &ids);
    check(ids.count >= 1, "symbol substring search");
    intlist_free(&ids);

    loclist_init(&locs);
    project_find_refs(&p, "add", &locs);
    check(locs.count >= 3, "add references");
    loclist_free(&locs);

    loclist_init(&locs);
    project_search_text(&p, "GREETING", &locs);
    check(locs.count >= 2, "text search macro uses");
    loclist_free(&locs);

    for (i = 0; i < p.sym_count; i++)
        check(p.syms[i].name != NULL && p.syms[i].signature != NULL,
              "symbol strings allocated");
    {
        char *rerr = NULL;
        check(project_build_relations(&p, NULL, NULL, &rerr),
              "relation tables build");
        check(project_relations_ready(&p), "relation tables marked ready");
        check(project_call_edge_count(&p) > 0, "relation call edges present");
        check(project_var_use_count(&p) > 0,
              "relation variable uses present");
        free(rerr);
    }
    {
        char *rerr = NULL;
        char *cache = path_join(root, ".si-index");
        Project cached;
        bool from_cache = false;
        check(project_cache_save(&p, &rerr), "project cache save");
        project_free(&p);
        cache_ticks = 0;
        check(project_load_cached(&cached, root, cache_progress, NULL,
                                  &rerr, &from_cache),
              "project cache load");
        check(from_cache, "project cache was used");
        check(cache_ticks > 0, "cache load reports progress");
        check(cached.relations_ready, "cached relations flag");
        check(cached.file_count > 0, "cached files");
        check(cached.sym_count > 0, "cached symbols");
        check(project_call_edge_count(&cached) > 0,
              "cached call edges");
        project_free(&cached);
        remove(cache);
        free(cache);
        free(rerr);
    }
}

#ifndef _WIN32
static void test_state(void)
{
    const char *tmp = getenv("TMPDIR");
    char tmpl[1024];
    char *home;
    char *old_home = getenv("HOME") ? xstrdup(getenv("HOME")) : NULL;
    char *path;
    StrList recent;

    snprintf(tmpl, sizeof(tmpl), "%s/si-state-XXXXXX",
             tmp && *tmp ? tmp : "/tmp");
    home = mkdtemp(tmpl);
    check(home != NULL, "state temp home");
    if (!home) {
        free(old_home);
        return;
    }
    setenv("HOME", home, 1);
    path = state_path();
    remove(path);

    strlist_init(&recent);
    strlist_push(&recent, "/tmp/si-project-a");
    strlist_push(&recent, "/tmp/si-project-b");
    check(state_save("/tmp/si-project-a", "/tmp/si-project-a/main.c",
                     12, 3, 4, &recent),
          "state save with history");
    {
        char *saved_root = NULL;
        char *saved_file = NULL;
        int cursor = -1;
        StrList loaded;
        strlist_init(&loaded);
        check(state_load(&saved_root, &saved_file, &cursor, &loaded),
              "state load with history");
        check(saved_root &&
                  strcmp(saved_root, "/tmp/si-project-a") == 0,
              "state root");
        check(saved_file &&
                  strcmp(saved_file, "/tmp/si-project-a/main.c") == 0,
              "state file");
        check(cursor == 12, "state cursor");
        check(loaded.count == 2, "state recent count");
        check(loaded.count > 0 &&
                  strcmp(loaded.items[0], "/tmp/si-project-a") == 0,
              "state recent order");
        check(loaded.count > 1 &&
                  strcmp(loaded.items[1], "/tmp/si-project-b") == 0,
              "state recent second");
        free(saved_root);
        free(saved_file);
        strlist_free(&loaded);
    }
    check(state_save(NULL, NULL, 0, 0, 0, &recent),
          "state save without project");
    {
        char *saved_root = NULL;
        StrList loaded;
        strlist_init(&loaded);
        check(state_load(&saved_root, NULL, NULL, &loaded),
              "state load without project");
        check(saved_root == NULL, "empty state root");
        check(loaded.count == 2, "empty state keeps history");
        free(saved_root);
        strlist_free(&loaded);
    }
    {
        StrList dup;
        StrList loaded;
        strlist_init(&dup);
        strlist_push(&dup, "/tmp/si-project-a");
        strlist_push(&dup, "/tmp/si-project-a");
        check(state_save(NULL, NULL, 0, 0, 0, &dup),
              "state save duplicate history");
        strlist_init(&loaded);
        check(state_load(NULL, NULL, NULL, &loaded),
              "state load duplicate history");
        check(loaded.count == 1, "state history deduplicated");
        strlist_free(&dup);
        strlist_free(&loaded);
    }
    {
        FILE *f = fopen(path, "wb");
        StrList loaded;
        check(f != NULL, "legacy state writable");
        if (f) {
            fprintf(f, "root=/tmp/si-legacy\nfile=\ncursor=0\n");
            fclose(f);
        }
        strlist_init(&loaded);
        check(state_load(NULL, NULL, NULL, &loaded),
              "legacy state load");
        check(loaded.count == 1 &&
                  strcmp(loaded.items[0], "/tmp/si-legacy") == 0,
              "legacy root becomes recent");
        strlist_free(&loaded);
    }

    remove(path);
    strlist_free(&recent);
    free(path);
    if (old_home)
        setenv("HOME", old_home, 1);
    else
        unsetenv("HOME");
    free(old_home);
    rmdir(home);
}
#endif

static void test_syntax_lists(void)
{
    bool has_return = false;
    bool has_int = false;
    size_t i;

    for (i = 0; i < syntax_keyword_count(); i++) {
        if (strcmp(syntax_keyword_at(i), "return") == 0)
            has_return = true;
    }
    for (i = 0; i < syntax_type_count(); i++) {
        if (strcmp(syntax_type_at(i), "int") == 0)
            has_int = true;
    }
    check(has_return, "syntax keyword list has return");
    check(has_int, "syntax type list has int");
    check(syntax_keyword_at(syntax_keyword_count()) == NULL,
          "keyword index out of range");
    check(syntax_type_at(syntax_type_count()) == NULL,
          "type index out of range");
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "tests/fixtures/demo";

    test_utf8();
    test_buffer();
    test_names();
    test_syntax();
    test_syntax_lists();
#ifndef _WIN32
    test_state();
#endif
    test_project(root);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
