#include "si.h"
#include "index.h"
#include "term.h"
#include "ui.h"

#include <locale.h>

typedef enum {
    MODE_UI = 0,
    MODE_INDEX,
    MODE_FILES,
    MODE_SYMBOLS,
    MODE_REFS,
    MODE_SEARCH
} RunMode;

static void usage(FILE *out)
{
    fprintf(out,
            "si-c %s - a small Source Insight style C/C++ editor\n\n"
            "Usage:\n"
            "  si [options] [file-or-directory]\n\n"
            "Options:\n"
            "  --index              index project and print counts, then exit\n"
            "  --files              list indexed source files, then exit\n"
            "  --symbols[=PATTERN]  list symbols (case-insensitive substring)\n"
            "  --refs=NAME          list references to NAME, then exit\n"
            "  --search=TEXT        search project text, then exit\n"
            "  --theme=NAME         dark (default) or light\n"
            "  --version            print version\n"
            "  --help               show this help\n\n"
            "Interactive keys:\n"
            "  F1 help        F2 files       F3 symbols     F4 editor\n"
            "  F5 reindex     F6 focus       F12 definition Shift+F12 refs\n"
            "  Ctrl+P symbol  Ctrl+G grep    Ctrl+F find     F8 find next\n"
            "  Ctrl+O open    Ctrl+L line    Ctrl+S save     Ctrl+Q quit\n"
            "  Ctrl+Z/Y undo/redo        Alt/Opt+Left back   F9 next file\n\n"
            "If PATH is a file, its directory is indexed and the file is opened.\n"
            "No ncurses or other third-party libraries are required.\n",
            SI_VERSION);
}

static int parse_args(int argc, char **argv, RunMode *mode, const char **path,
                      const char **pattern, const char **theme)
{
    int i;
    *mode = MODE_UI;
    *path = NULL;
    *pattern = NULL;
    *theme = NULL;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 1;
        }
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            printf("si-c %s\n", SI_VERSION);
            return 1;
        }
        if (strcmp(arg, "--index") == 0) {
            *mode = MODE_INDEX;
        } else if (strcmp(arg, "--files") == 0) {
            *mode = MODE_FILES;
        } else if (strcmp(arg, "--symbols") == 0) {
            *mode = MODE_SYMBOLS;
            *pattern = "";
        } else if (strncmp(arg, "--symbols=", 10) == 0) {
            *mode = MODE_SYMBOLS;
            *pattern = arg + 10;
        } else if (strncmp(arg, "--refs=", 7) == 0) {
            *mode = MODE_REFS;
            *pattern = arg + 7;
        } else if (strncmp(arg, "--search=", 9) == 0) {
            *mode = MODE_SEARCH;
            *pattern = arg + 9;
        } else if (strncmp(arg, "--theme=", 8) == 0) {
            *theme = arg + 8;
        } else if (arg[0] == '-') {
            fprintf(stderr, "si: unknown option: %s\n", arg);
            return -1;
        } else if (!*path) {
            *path = arg;
        } else {
            fprintf(stderr, "si: unexpected argument: %s\n", arg);
            return -1;
        }
    }
    if (*theme && !str_ieq(*theme, "dark") && !str_ieq(*theme, "light")) {
        fprintf(stderr, "si: invalid theme '%s' (use dark or light)\n", *theme);
        return -1;
    }
    if (*mode == MODE_REFS && (!*pattern || !**pattern)) {
        fprintf(stderr, "si: --refs requires an identifier\n");
        return -1;
    }
    if (*mode == MODE_SEARCH && (!*pattern || !**pattern)) {
        fprintf(stderr, "si: --search requires text\n");
        return -1;
    }
    return 0;
}

static void print_index(Project *p)
{
    int counts[12];
    size_t i;
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < p->sym_count; i++) {
        int k = (int)p->syms[i].kind;
        if (k >= 0 && k < 12)
            counts[k]++;
    }
    printf("root: %s\n", p->root ? p->root : "");
    printf("files: %lu\n", (unsigned long)p->file_count);
    printf("symbols: %lu\n", (unsigned long)p->sym_count);
    printf("warnings: %d\n", p->warnings);
    for (i = 0; i < 12; i++)
        printf("  %-10s %d\n", symbol_kind_name((SymbolKind)i), counts[i]);
    if (p->last_warning)
        printf("last warning: %s\n", p->last_warning);
}

static void print_files(Project *p)
{
    size_t i;
    for (i = 0; i < p->file_count; i++)
        printf("%s\n", p->files[i].rel);
}

static void print_symbols(Project *p, const char *pattern)
{
    size_t i;
    for (i = 0; i < p->sym_count; i++) {
        Symbol *s = &p->syms[i];
        if (pattern && *pattern && !str_icontains(s->name, pattern))
            continue;
        printf("%s:%d:%d  %-10s %s\n",
               p->files[s->file_index].rel, s->line, s->col + 1,
               symbol_kind_short(s->kind), s->signature);
    }
}

static void print_locations(Project *p, LocationList *locs)
{
    size_t i;
    for (i = 0; i < locs->count; i++) {
        Location *l = &locs->items[i];
        printf("%s:%d:%d  %s\n", p->files[l->file_index].rel, l->line,
               l->col + 1, l->preview);
    }
}

int main(int argc, char **argv)
{
    RunMode mode;
    const char *arg_path;
    const char *pattern;
    const char *theme;
    char *root = NULL;
    char *initial = NULL;
    char *err = NULL;
    Project project;
    int initial_cursor = 0;
    int parsed;

    setlocale(LC_ALL, "");
    parsed = parse_args(argc, argv, &mode, &arg_path, &pattern, &theme);
    if (parsed > 0)
        return 0;
    if (parsed < 0) {
        usage(stderr);
        return 2;
    }
    if (!theme)
        theme = getenv("SI_THEME");

    /*
     * With no explicit path, restore the last project and cursor.  An
     * explicit path always wins, so `si /other/project` behaves normally.
     */
    if (!arg_path && mode == MODE_UI) {
        char *sroot = NULL;
        char *sfile = NULL;
        int scursor = 0;
        if (state_load(&sroot, &sfile, &scursor) &&
            sroot && path_is_dir(sroot)) {
            root = sroot;
            initial = sfile;
            initial_cursor = scursor;
        } else {
            free(sroot);
            free(sfile);
        }
    }
    if (!root) {
        if (!arg_path)
            arg_path = ".";
        {
            char *abs = path_abs(arg_path);
            if (!path_exists(abs)) {
                fprintf(stderr, "si: no such file or directory: %s\n",
                        arg_path);
                free(abs);
                return 1;
            }
            if (path_is_dir(abs)) {
                root = xstrdup(abs);
            } else {
                root = path_dirname(abs);
                initial = xstrdup(abs);
            }
            free(abs);
        }
    }

    if (mode == MODE_UI) {
        int rc;
        if (!term_is_tty()) {
            fprintf(stderr,
                    "si: stdout/stdin is not a TTY. Use --index, --files,\n"
                    "    --symbols, --refs=NAME or --search=TEXT for batch mode.\n");
            free(root);
            free(initial);
            return 2;
        }
        rc = ui_run(root, initial, theme, initial_cursor);
        free(root);
        free(initial);
        return rc;
    }

    if (!project_load(&project, root, NULL, NULL, &err)) {
        fprintf(stderr, "si: %s\n", err ? err : "cannot index project");
        free(err);
        free(root);
        free(initial);
        return 1;
    }
    if (mode == MODE_INDEX) {
        print_index(&project);
    } else if (mode == MODE_FILES) {
        print_files(&project);
    } else if (mode == MODE_SYMBOLS) {
        print_symbols(&project, pattern);
    } else if (mode == MODE_REFS) {
        LocationList locs;
        loclist_init(&locs);
        project_find_refs(&project, pattern, &locs);
        print_locations(&project, &locs);
        loclist_free(&locs);
    } else if (mode == MODE_SEARCH) {
        LocationList locs;
        loclist_init(&locs);
        project_search_text(&project, pattern, &locs);
        print_locations(&project, &locs);
        loclist_free(&locs);
    }
    project_free(&project);
    free(root);
    free(initial);
    return 0;
}
