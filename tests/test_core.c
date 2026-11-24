#include "si.h"
#include "compile.h"
#include "index.h"
#include "syntax.h"
#include "text.h"

#include <math.h>

#if !defined(_WIN32)
#include <time.h>
#endif

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

static ProjectMember *find_member(ProjectMemberList *l, const char *name);

static void check_variable(Project *p, const char *name,
                           const char *type_fragment)
{
    IntList ids;
    intlist_init(&ids);
    project_find_symbols(p, name, &ids);
    check(ids.count == 1, name);
    if (ids.count == 1) {
        Symbol *s = &p->syms[ids.items[0]];
        check(s->kind == SYM_VARIABLE, name);
        check(strstr(s->signature, type_fragment) != NULL, name);
    }
    intlist_free(&ids);
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
            check(tree.count > 0 && tree.items[0].preview != NULL &&
                  strstr(tree.items[0].preview, "[ref]") == NULL,
                  "direct call is not marked as a reference");
            loclist_free(&tree);
        }
        {
            int cb_idx = -1;
            IntList cids;
            LocationList tree;
            size_t j;
            bool saw_register = false;
            bool saw_signal = false;
            intlist_init(&cids);
            project_find_symbols(&p, "callback_target", &cids);
            for (j = 0; j < cids.count; j++) {
                if (p.syms[cids.items[j]].kind == SYM_FUNCTION)
                    cb_idx = cids.items[j];
            }
            intlist_free(&cids);
            check(cb_idx >= 0, "callback target function found");
            if (cb_idx >= 0) {
                check(project_context(&p, cb_idx, &ctx),
                      "callback target context");
                check(ctx.callers.count >= 2,
                      "callback target has reference callers");
                for (j = 0; j < ctx.callers.count; j++) {
                    const char *p = ctx.callers.items[j].preview;
                    if (p && (strstr(p, "callback_register_demo") != NULL ||
                              strstr(p, "callback_signal_demo") != NULL)) {
                        check(strstr(p, "[ref]") != NULL,
                              "reference caller marked [ref]");
                        if (strstr(p, "callback_register_demo") != NULL)
                            saw_register = true;
                        if (strstr(p, "callback_signal_demo") != NULL)
                            saw_signal = true;
                    }
                }
                check(saw_register, "function pointer assignment is a reference");
                check(saw_signal, "signal registration is a reference");
                symbol_context_free(&ctx);
                check(project_caller_tree(&p, cb_idx, &tree, 8),
                      "callback caller tree");
                check(tree.count >= 2, "callback tree has nodes");
                for (j = 0; j < tree.count; j++) {
                    const char *p = tree.items[j].preview;
                    if (p && (strstr(p, "callback_register_demo") != NULL ||
                              strstr(p, "callback_signal_demo") != NULL)) {
                        check(strstr(p, "[ref]") != NULL,
                              "callback tree node marked [ref]");
                    }
                }
                loclist_free(&tree);
            }
        }
        {
            int alpha_ping = -1;
            int beta_ping = -1;
            IntList pids;
            size_t j;
            SymbolContext qctx;
            bool saw_alpha = false;
            bool saw_beta = false;
            intlist_init(&pids);
            project_find_symbols(&p, "ping", &pids);
            for (j = 0; j < pids.count; j++) {
                Symbol *ps = &p.syms[pids.items[j]];
                if (ps->kind != SYM_FUNCTION)
                    continue;
                if (strstr(ps->signature, "AlphaThing::ping") != NULL)
                    alpha_ping = (int)pids.items[j];
                if (strstr(ps->signature, "BetaThing::ping") != NULL)
                    beta_ping = (int)pids.items[j];
            }
            intlist_free(&pids);
            check(alpha_ping >= 0 && beta_ping >= 0,
                  "qualified methods are indexed");
            if (alpha_ping >= 0) {
                char label[128];
                check(symbol_short_name(p.syms[alpha_ping].signature, label,
                                        sizeof(label)) &&
                      strcmp(label, "AlphaThing::ping") == 0,
                      "call flow label keeps class qualifier");
                check(project_context(&p, alpha_ping, &qctx),
                      "alpha ping context");
                for (j = 0; j < qctx.callers.count; j++) {
                    const char *pv = qctx.callers.items[j].preview;
                    if (pv && strstr(pv, "AlphaThing::call") != NULL)
                        saw_alpha = true;
                    if (pv && strstr(pv, "BetaThing::call") != NULL)
                        saw_beta = true;
                }
                check(saw_alpha && !saw_beta,
                      "same-class call resolves to alpha ping");
                symbol_context_free(&qctx);
            }
            if (beta_ping >= 0) {
                saw_alpha = false;
                saw_beta = false;
                check(project_context(&p, beta_ping, &qctx),
                      "beta ping context");
                for (j = 0; j < qctx.callers.count; j++) {
                    const char *pv = qctx.callers.items[j].preview;
                    if (pv && strstr(pv, "AlphaThing::call") != NULL)
                        saw_alpha = true;
                    if (pv && strstr(pv, "BetaThing::call") != NULL)
                        saw_beta = true;
                }
                check(saw_beta && !saw_alpha,
                      "same-class call resolves to beta ping");
                symbol_context_free(&qctx);
            }
        }
        {
            int bm_idx = -1;
            int ba_idx = -1;
            IntList bids;
            size_t j;
            intlist_init(&bids);
            project_find_symbols(&p, "branch_main", &bids);
            for (j = 0; j < bids.count; j++) {
                if (p.syms[bids.items[j]].kind == SYM_FUNCTION)
                    bm_idx = bids.items[j];
            }
            intlist_free(&bids);
            intlist_init(&bids);
            project_find_symbols(&p, "branch_alt", &bids);
            for (j = 0; j < bids.count; j++) {
                if (p.syms[bids.items[j]].kind == SYM_FUNCTION)
                    ba_idx = bids.items[j];
            }
            intlist_free(&bids);
            check(bm_idx >= 0, "function after #else branch is indexed");
            check(ba_idx >= 0, "function in #ifdef branch is indexed");
            {
                IntList fids;
                size_t fcount = 0;
                intlist_init(&fids);
                project_find_symbols(&p, "callback_target", &fids);
                for (j = 0; j < fids.count; j++) {
                    if (p.syms[fids.items[j]].kind == SYM_FUNCTION)
                        fcount++;
                }
                intlist_free(&fids);
                check(fcount == 1,
                      "call inside if is not indexed as a function");
            }
            if (bm_idx >= 0) {
                check(project_context(&p, bm_idx, &ctx),
                      "branch main context");
                check(ctx.callees.count >= 1,
                      "shared body call attributed to #else function");
                symbol_context_free(&ctx);
            }
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

    check_variable(&p, "completion_origin", "struct Point");
    check_variable(&p, "completion_world", "Point");
    check_variable(&p, "completion_ptr", "Point");
    check_variable(&p, "completion_points", "[3]");
    check_variable(&p, "completion_a", "int");
    check_variable(&p, "completion_b", "int");
    check_variable(&p, "completion_p", "struct Point");
    check_variable(&p, "completion_q", "struct Point *");
    check_variable(&p, "completion_arr", "int");
    intlist_init(&ids);
    project_find_symbols(&p, "CompletionForward", &ids);
    check(ids.count == 0, "forward declaration is not a variable");
    intlist_free(&ids);
    intlist_init(&ids);
    project_find_symbols(&p, "CompletionMode", &ids);
    check(ids.count == 0, "opaque enum class is not a variable");
    intlist_free(&ids);
    intlist_init(&ids);
    project_find_symbols(&p, "CompletionMode2", &ids);
    check(ids.count == 0, "opaque enum class underlying type is not a variable");
    intlist_free(&ids);
    intlist_init(&ids);
    project_find_symbols(&p, "shared_point", &ids);
    check(ids.count == 2, "same-name static globals both indexed");
    intlist_free(&ids);

    {
        ProjectMemberList members;
        ProjectMember *m;
        project_member_list_init(&members);
        check(project_aggregate_members(&p, "Point", &members),
              "Point members found");
        check(members.count == 2, "Point has two members");
        m = find_member(&members, "x");
        check(m != NULL && strcmp(m->type, "int") == 0, "Point.x type");
        m = find_member(&members, "y");
        check(m != NULL && strcmp(m->type, "int") == 0, "Point.y type");
        project_member_list_free(&members);

        project_member_list_init(&members);
        check(project_aggregate_members(&p, "CompletionAnon", &members),
              "anonymous typedef members found");
        check(members.count == 2, "anonymous typedef has alpha/beta");
        project_member_list_free(&members);

        project_member_list_init(&members);
        check(project_aggregate_members(&p, "CompletionNode", &members),
              "CompletionNode members found");
        check(members.count == 5, "CompletionNode has five members");
        m = find_member(&members, "pos");
        check(m != NULL && strcmp(m->type, "Point") == 0,
              "CompletionNode.pos type");
        m = find_member(&members, "next");
        check(m != NULL && strstr(m->type, "CompletionNode") != NULL &&
              strchr(m->type, '*') != NULL, "CompletionNode.next type");
        m = find_member(&members, "alpha");
        check(m != NULL && strcmp(m->type, "int") == 0,
              "CompletionNode.alpha type");
        m = find_member(&members, "point_arr");
        check(m != NULL && m->is_array && strcmp(m->type, "Point") == 0,
              "CompletionNode.point_arr is an array");
        project_member_list_free(&members);

        project_member_list_init(&members);
        check(project_aggregate_members(&p, "Thing", &members),
              "C++ class members found");
        check(members.count == 11,
              "Thing has eleven members including base and methods");
        m = find_member(&members, "base_value");
        check(m != NULL && !m->is_method && !m->is_static && !m->is_type &&
              strcmp(m->type, "int") == 0, "inherited base_value is data");
        m = find_member(&members, "base_method");
        check(m != NULL && m->is_method, "inherited base_method is method");
        m = find_member(&members, "value");
        check(m != NULL && !m->is_method && strcmp(m->type, "int") == 0,
              "Thing.value is data");
        m = find_member(&members, "count");
        check(m != NULL && m->is_static && !m->is_method,
              "Thing.count is static data");
        m = find_member(&members, "static_method");
        check(m != NULL && m->is_static && m->is_method,
              "Thing.static_method is static method");
        m = find_member(&members, "method");
        check(m != NULL && m->is_method, "Thing.method is method");
        m = find_member(&members, "Inner");
        check(m != NULL && m->is_type, "Thing.Inner is nested type");
        m = find_member(&members, "use_this");
        check(m != NULL && m->is_method, "inline method is indexed");
        m = find_member(&members, "make_thing");
        check(m != NULL && m->is_method && strstr(m->type, "Thing") != NULL,
              "method return type is captured");
        m = find_member(&members, "make_ptr");
        check(m != NULL && m->is_method && strstr(m->type, "Thing") != NULL &&
              strchr(m->type, '*') != NULL,
              "pointer return type is captured");
        project_member_list_free(&members);

        project_member_list_init(&members);
        check(project_aggregate_members(&p, "Inner", &members),
              "nested type members found");
        m = find_member(&members, "z");
        check(m != NULL && !m->is_method, "Inner.z is data");
        m = find_member(&members, "inner_count");
        check(m != NULL && m->is_static && !m->is_method,
              "Inner.inner_count is static data");
        m = find_member(&members, "inner_method");
        check(m != NULL && m->is_method, "Inner.inner_method is method");
        project_member_list_free(&members);

        project_member_list_init(&members);
        check(project_aggregate_members(&p, "NsThing", &members),
              "namespace class members found");
        m = find_member(&members, "ns_value");
        check(m != NULL && !m->is_method, "NsThing.ns_value is data");
        m = find_member(&members, "ns_count");
        check(m != NULL && m->is_static && !m->is_method,
              "NsThing.ns_count is static data");
        m = find_member(&members, "ns_method");
        check(m != NULL && m->is_method, "NsThing.ns_method is method");
        project_member_list_free(&members);

        project_member_list_init(&members);
        check(project_aggregate_members(&p, "Box", &members),
              "template class members found");
        m = find_member(&members, "item");
        check(m != NULL && !m->is_method && strcmp(m->type, "T") == 0,
              "template data member keeps type parameter");
        m = find_member(&members, "put");
        check(m != NULL && m->is_method, "template method found");
        m = find_member(&members, "box_count");
        check(m != NULL && m->is_static && !m->is_method,
              "template static member found");
        project_member_list_free(&members);

        {
            IntList aids;
            int alias_idx = -1;
            intlist_init(&aids);
            project_find_symbols(&p, "AliasThing", &aids);
            if (aids.count > 0)
                alias_idx = aids.items[0];
            intlist_free(&aids);
            check(alias_idx >= 0 && p.syms[alias_idx].kind == SYM_TYPEDEF,
                  "using alias is indexed as typedef");
            if (alias_idx >= 0) {
                project_member_list_init(&members);
                check(project_aggregate_members(&p, "AliasThing", &members),
                      "using alias resolves target members");
                m = find_member(&members, "value");
                check(m != NULL && !m->is_method,
                      "using alias exposes Thing.value");
                project_member_list_free(&members);
            }
        }

        project_member_list_init(&members);
        check(!project_aggregate_members(&p, "CompletionForward", &members),
              "forward declaration has no members");
        project_member_list_free(&members);

        project_member_list_init(&members);
        check(!project_aggregate_members(&p, "NoSuchType", &members),
              "unknown aggregate has no members");
        project_member_list_free(&members);
    }

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
        {
            IntList cids;
            int cb_idx = -1;
            size_t j;
            intlist_init(&cids);
            project_find_symbols(&cached, "callback_target", &cids);
            for (j = 0; j < cids.count; j++) {
                if (cached.syms[cids.items[j]].kind == SYM_FUNCTION)
                    cb_idx = cids.items[j];
            }
            intlist_free(&cids);
            check(cb_idx >= 0, "cached callback target found");
            if (cb_idx >= 0) {
                SymbolContext cctx;
                check(project_context(&cached, cb_idx, &cctx),
                      "cached callback context");
                check(cctx.callers.count >= 2,
                      "cached reference callers preserved");
                {
                    bool cached_ref = false;
                    size_t ci;
                    for (ci = 0; ci < cctx.callers.count; ci++) {
                        if (cctx.callers.items[ci].preview != NULL &&
                            strstr(cctx.callers.items[ci].preview,
                                   "[ref]") != NULL)
                            cached_ref = true;
                    }
                    check(cached_ref, "cached reference marker preserved");
                }
                symbol_context_free(&cctx);
            }
        }
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

static ProjectMember *find_member(ProjectMemberList *l, const char *name)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0)
            return &l->items[i];
    }
    return NULL;
}

static void test_type_keys(void)
{
    char key[256];

    check(project_type_aggregate_key("const struct Point *", key,
                                     sizeof(key)) &&
          strcmp(key, "Point") == 0, "type key struct pointer");
    check(project_type_aggregate_key("struct CompletionNode *", key,
                                     sizeof(key)) &&
          strcmp(key, "CompletionNode") == 0, "type key typedef pointer");
    check(project_type_aggregate_key("Point[4]", key, sizeof(key)) &&
          strcmp(key, "Point") == 0, "type key array");
    check(!project_type_aggregate_key("unsigned long", key, sizeof(key)),
          "builtin type has no aggregate key");
    check(!project_type_aggregate_key("struct { int x; }", key, sizeof(key)),
          "anonymous aggregate has no key");
    check(!project_type_aggregate_key("void (*)(int)", key, sizeof(key)),
          "function pointer has no aggregate key");
}

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

static bool command_has_arg(const CompileCommand *cmd, const char *arg)
{
    size_t i;
    for (i = 0; i < cmd->argc; i++) {
        if (strcmp(cmd->argv[i], arg) == 0)
            return true;
    }
    return false;
}

static void test_compile_helpers(void)
{
    char *sh;

    check(compile_is_source("main.c"), "compile: c source");
    check(compile_is_source("src/a.cpp"), "compile: cpp source");
    check(compile_is_source("src/a.cc"), "compile: cc source");
    check(!compile_is_source("util.h"), "compile: header is not source");
    check(!compile_is_source("README.md"), "compile: other file is not source");
    check(compile_is_cxx_source("a.cpp"), "compile: cpp is cxx");
    check(compile_is_cxx_source("a.cxx"), "compile: cxx extension");
    check(!compile_is_cxx_source("a.c"), "compile: c is not cxx");

    sh = compile_find_program("sh");
    check(sh != NULL, "compile: find sh in PATH");
    free(sh);
    check(compile_find_program("si-no-such-program-xyz") == NULL,
          "compile: missing program stays missing");
}

static void test_compile_plan(void)
{
    static const char *mixed[] = { "main.c", "util.c", "src/extra.cpp" };
    static const char *conly[] = { "main.c" };
    static const char *empty[1];
    CompilePlan plan;
    CompileCommand *link;
    char *err = NULL;

    compile_plan_init(&plan);
    check(compile_plan_direct("demo", mixed, 3, "/usr/bin/cc", "/usr/bin/c++",
                              &plan, &err),
          "compile: mixed plan builds");
    check(err == NULL, "compile: mixed plan has no error");
    check(!plan.uses_make, "compile: direct plan uses no make");
    check(plan.count == 4, "compile: three objects plus link");
    check(plan.output && strcmp(plan.output, "build/demo") == 0,
          "compile: output path");
    if (plan.count == 4) {
        check(strcmp(plan.items[0].argv[0], "/usr/bin/cc") == 0,
              "compile: c file uses c compiler");
        check(command_has_arg(&plan.items[0], "-c"),
              "compile: object compile flag");
        check(command_has_arg(&plan.items[0], "build/obj/main.c.o"),
              "compile: object path");
        check(strcmp(plan.items[2].argv[0], "/usr/bin/c++") == 0,
              "compile: cpp file uses c++ compiler");
        link = &plan.items[3];
        check(strcmp(link->argv[0], "/usr/bin/c++") == 0,
              "compile: mixed link uses c++ driver");
        check(command_has_arg(link, "build/obj/src/extra.cpp.o"),
              "compile: link includes nested object");
    }
    compile_plan_free(&plan);

    compile_plan_init(&plan);
    check(compile_plan_direct("demo", conly, 1, "/usr/bin/cc", NULL,
                              &plan, &err),
          "compile: c-only plan builds");
    check(plan.count == 2, "compile: c-only compile plus link");
    if (plan.count == 2) {
        check(strcmp(plan.items[1].argv[0], "/usr/bin/cc") == 0,
              "compile: c-only link uses c compiler");
        check(command_has_arg(&plan.items[1], "-o"),
              "compile: link writes output");
    }
    compile_plan_free(&plan);

    compile_plan_init(&plan);
    check(!compile_plan_direct("demo", empty, 0, "/usr/bin/cc", NULL,
                               &plan, &err),
          "compile: empty project is rejected");
    free(err);
    err = NULL;
    compile_plan_free(&plan);

    compile_plan_init(&plan);
    check(!compile_plan_direct("demo", conly, 1, NULL, NULL, &plan, &err),
          "compile: missing compiler is rejected");
    check(err != NULL && strstr(err, "compiler") != NULL,
          "compile: missing compiler explains why");
    free(err);
    err = NULL;
    compile_plan_free(&plan);

    compile_plan_init(&plan);
    check(compile_plan_make("demo", "/usr/bin/make", &plan, &err),
          "compile: make plan builds");
    check(plan.uses_make && plan.count == 1,
          "compile: make plan has one command");
    if (plan.count == 1) {
        check(strcmp(plan.items[0].argv[0], "/usr/bin/make") == 0 &&
              command_has_arg(&plan.items[0], "-C") &&
              command_has_arg(&plan.items[0], "demo"),
              "compile: make runs in project root");
    }
    compile_plan_free(&plan);

#ifndef _WIN32
    {
        const char *old_path = getenv("PATH");
        char *saved = old_path ? xstrdup(old_path) : NULL;
        check(setenv("PATH", "/nonexistent-si-c", 1) == 0,
              "compile: temporarily clear PATH");
        compile_plan_init(&plan);
        check(!compile_plan_build("tests/fixtures/demo", conly, 1,
                                  &plan, &err),
              "compile: build rejects missing toolchain");
        check(err != NULL, "compile: toolchain error message");
        free(err);
        err = NULL;
        compile_plan_free(&plan);
        if (saved)
            setenv("PATH", saved, 1);
        else
            unsetenv("PATH");
        free(saved);
    }
#endif
}

static void test_compile_sanitize(void)
{
    Str out;
    const char *raw = "\x1b[31merror\x1b[0m\r\nx\ty\n";

    str_init(&out);
    compile_sanitize_append(&out, raw, strlen(raw));
    check(out.data && strstr(out.data, "error") != NULL,
          "compile: sanitize keeps text");
    check(out.data && strchr(out.data, 27) == NULL,
          "compile: sanitize strips escape");
    check(out.data && strstr(out.data, "x   y") != NULL,
          "compile: sanitize expands tabs");
    str_free(&out);
}

#ifndef _WIN32
static void test_compile_makefile_detection(void)
{
    char tmpl[] = "/tmp/si-compile-XXXXXX";
    char path[512];
    char *dir = mkdtemp(tmpl);
    CompilePlan plan;
    char *err = NULL;

    check(dir != NULL, "compile: temp dir created");
    if (!dir)
        return;
    snprintf(path, sizeof(path), "%s/Makefile", dir);
    check(file_write_atomic(path, "all:\n\t@true\n", 12, &err),
          "compile: write Makefile fixture");
    free(err);
    err = NULL;
    check(compile_has_makefile(dir), "compile: Makefile detected");
    compile_plan_init(&plan);
    check(compile_plan_build(dir, NULL, 0, &plan, &err),
          "compile: Makefile plan selected");
    check(plan.uses_make && plan.count == 1,
          "compile: Makefile uses make");
    compile_plan_free(&plan);
    remove(path);
    rmdir(dir);
}

static void test_compile_process(void)
{
    char *argv[] = { "/bin/sh", "-c",
                     "printf 'hello\\n'; printf 'err\\n' >&2; exit 7",
                     NULL };
    CompileProcess p;
    Str out;
    char *err = NULL;
    int i;

    memset(&p, 0, sizeof(p));
    p.fd = -1;
    str_init(&out);
    check(compile_process_start(&p, ".", argv, &err),
          "compile: process starts");
    for (i = 0; i < 300 && p.running; i++) {
        struct timespec ts;
        compile_process_poll(&p, &out);
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }
    compile_process_poll(&p, &out);
    check(!p.running, "compile: process finishes");
    check(p.exit_code == 7, "compile: process exit code");
    check(out.data && strstr(out.data, "hello") != NULL,
          "compile: process captures stdout");
    check(out.data && strstr(out.data, "err") != NULL,
          "compile: process captures stderr");
    compile_process_close(&p);
    str_free(&out);
    free(err);
}
#endif

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "tests/fixtures/demo";

    test_utf8();
    test_buffer();
    test_names();
    test_syntax();
    test_syntax_lists();
    test_type_keys();
    test_compile_helpers();
    test_compile_plan();
    test_compile_sanitize();
#ifndef _WIN32
    test_state();
    test_compile_makefile_detection();
    test_compile_process();
#endif
    test_project(root);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
