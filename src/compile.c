#include "compile.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define compile_mkdir(path) _mkdir(path)
#define read _read
#define close _close
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define compile_mkdir(path) mkdir(path, 0755)
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_err(char **err, const char *fmt, ...)
{
    va_list ap;

    if (!err)
        return;
    free(*err);
    *err = NULL;
    {
        Str s;
        str_init(&s);
        va_start(ap, fmt);
        str_vprintf(&s, fmt, ap);
        va_end(ap);
        *err = s.data ? s.data : xstrdup("");
    }
}

static void clear_err(char **err)
{
    if (!err)
        return;
    free(*err);
    *err = NULL;
}

static bool ext_is(const char *path, const char *ext, bool ci)
{
    size_t n;
    size_t m;
    const char *dot;

    if (!path)
        return false;
    dot = strrchr(path, '.');
    if (!dot)
        return false;
    n = strlen(dot);
    m = strlen(ext);
    if (n != m)
        return false;
    return ci ? str_ieq(dot, ext) : strcmp(dot, ext) == 0;
}

bool compile_is_source(const char *path)
{
    if (!path)
        return false;
    if (strcmp(path, "") == 0)
        return false;
    if (ext_is(path, ".C", false))
        return true;
    return ext_is(path, ".c", true) || ext_is(path, ".cc", true) ||
           ext_is(path, ".cpp", true) || ext_is(path, ".cxx", true) ||
           ext_is(path, ".c++", true);
}

bool compile_is_cxx_source(const char *path)
{
    if (!path)
        return false;
    if (ext_is(path, ".C", false))
        return true;
    return ext_is(path, ".cc", true) || ext_is(path, ".cpp", true) ||
           ext_is(path, ".cxx", true) || ext_is(path, ".c++", true);
}

char *compile_find_program(const char *name)
{
#ifdef _WIN32
    if (!name || !*name)
        return NULL;
    if (_access(name, 0) == 0)
        return xstrdup(name);
    return NULL;
#else
    const char *path;
    const char *p;
    char *found = NULL;

    if (!name || !*name)
        return NULL;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0 ? xstrdup(name) : NULL;

    path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    p = path;
    while (*p && !found) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *dir = len ? xstrndup(p, len) : xstrdup(".");
        char *cand = path_join(dir, name);
        if (access(cand, X_OK) == 0)
            found = cand;
        else
            free(cand);
        free(dir);
        if (!end)
            break;
        p = end + 1;
    }
    return found;
#endif
}

bool compile_has_makefile(const char *root)
{
    static const char *const names[] = { "Makefile", "makefile",
                                         "GNUmakefile" };
    size_t i;

    if (!root || !*root)
        return false;
    for (i = 0; i < SI_ARRAY_LEN(names); i++) {
        char *path = path_join(root, names[i]);
        bool ok = path_is_file(path);
        free(path);
        if (ok)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Command plans                                                       */
/* ------------------------------------------------------------------ */

void compile_plan_init(CompilePlan *plan)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
}

static void command_free(CompileCommand *cmd)
{
    size_t i;
    if (!cmd)
        return;
    for (i = 0; i < cmd->argc; i++)
        free(cmd->argv[i]);
    free(cmd->argv);
    memset(cmd, 0, sizeof(*cmd));
}

void compile_plan_free(CompilePlan *plan)
{
    size_t i;
    if (!plan)
        return;
    for (i = 0; i < plan->count; i++)
        command_free(&plan->items[i]);
    free(plan->items);
    free(plan->output);
    memset(plan, 0, sizeof(*plan));
}

static bool strlist_has(const StrList *l, const char *value)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        if (strcmp(l->items[i], value) == 0)
            return true;
    }
    return false;
}

static void argv_push(char ***argv, size_t *argc, const char *value)
{
    *argv = xrealloc(*argv, (*argc + 2) * sizeof(**argv));
    (*argv)[(*argc)++] = xstrdup(value);
    (*argv)[*argc] = NULL;
}

static void plan_push_argv(CompilePlan *plan, char **argv, size_t argc)
{
    if (plan->count == plan->cap) {
        plan->cap = plan->cap ? plan->cap * 2 : 8;
        plan->items = xrealloc(plan->items,
                               plan->cap * sizeof(*plan->items));
    }
    plan->items[plan->count].argv = argv;
    plan->items[plan->count].argc = argc;
    plan->count++;
}

static char *project_output_name(const char *root)
{
    char *abs = path_abs(root && *root ? root : ".");
    const char *base = path_basename(abs);
    char *name;

    if (!base || !*base || strcmp(base, ".") == 0 ||
        strcmp(base, "/") == 0) {
        name = xstrdup("a.out");
    } else {
        name = xstrdup(base);
    }
    free(abs);
    return name;
}

bool compile_plan_make(const char *root, const char *make_program,
                       CompilePlan *plan, char **err)
{
    char **argv = NULL;
    size_t argc = 0;

    if (!root || !*root) {
        set_err(err, "No project root");
        return false;
    }
    if (!make_program || !*make_program) {
        set_err(err, "make not found in PATH (required by Makefile)");
        return false;
    }
    argv_push(&argv, &argc, make_program);
    argv_push(&argv, &argc, "-C");
    argv_push(&argv, &argc, root);
    plan_push_argv(plan, argv, argc);
    plan->uses_make = true;
    clear_err(err);
    return true;
}

bool compile_plan_direct(const char *root, const char *const *rels,
                         size_t count, const char *cc, const char *cxx,
                         CompilePlan *plan, char **err)
{
    StrList dirs;
    StrList objects;
    size_t i;
    bool need_c = false;
    bool need_cxx = false;
    size_t sources = 0;
    const char *linker;
    char *output;
    char **argv;
    size_t argc;

    if (!root || !*root) {
        set_err(err, "No project root");
        return false;
    }
    for (i = 0; i < count; i++) {
        if (!compile_is_source(rels[i]))
            continue;
        sources++;
        if (compile_is_cxx_source(rels[i]))
            need_cxx = true;
        else
            need_c = true;
    }
    if (sources == 0) {
        set_err(err, "No C/C++ source files to compile");
        return false;
    }
    if (need_c && (!cc || !*cc)) {
        set_err(err, "C compiler not found in PATH (tried cc, gcc, clang)");
        return false;
    }
    if (need_cxx && (!cxx || !*cxx)) {
        set_err(err,
                "C++ compiler not found in PATH (tried c++, g++, clang++)");
        return false;
    }
    linker = need_cxx ? cxx : cc;

    strlist_init(&dirs);
    strlist_init(&objects);
    strlist_push(&dirs, ".");
    for (i = 0; i < count; i++) {
        char *dir;
        if (!compile_is_source(rels[i]))
            continue;
        dir = path_dirname(rels[i]);
        if (*dir && strcmp(dir, ".") != 0 && !strlist_has(&dirs, dir))
            strlist_push(&dirs, dir);
        free(dir);
    }
    strlist_sort(&dirs);

    for (i = 0; i < count; i++) {
        const char *rel = rels[i];
        const char *compiler;
        char *obj;
        size_t d;

        if (!compile_is_source(rel))
            continue;
        compiler = compile_is_cxx_source(rel) ? cxx : cc;
        obj = xasprintf("build/obj/%s.o", rel);
        strlist_push(&objects, obj);

        argv = NULL;
        argc = 0;
        argv_push(&argv, &argc, compiler);
        argv_push(&argv, &argc, "-c");
        argv_push(&argv, &argc, "-o");
        argv_push(&argv, &argc, obj);
        for (d = 0; d < dirs.count; d++) {
            char *flag = xasprintf("-I%s", dirs.items[d]);
            argv_push(&argv, &argc, flag);
            free(flag);
        }
        argv_push(&argv, &argc, rel);
        plan_push_argv(plan, argv, argc);
        free(obj);
    }

    output = xasprintf("build/%s", project_output_name(root));
    plan->output = output;

    argv = NULL;
    argc = 0;
    argv_push(&argv, &argc, linker);
    argv_push(&argv, &argc, "-o");
    argv_push(&argv, &argc, output);
    for (i = 0; i < objects.count; i++)
        argv_push(&argv, &argc, objects.items[i]);
    plan_push_argv(plan, argv, argc);

    strlist_free(&dirs);
    strlist_free(&objects);
    clear_err(err);
    return true;
}

bool compile_plan_build(const char *root, const char *const *rels,
                        size_t count, CompilePlan *plan, char **err)
{
    char *make = NULL;
    char *cc = NULL;
    char *cxx = NULL;
    bool need_c = false;
    bool need_cxx = false;
    size_t i;
    bool ok;

    if (compile_has_makefile(root)) {
        make = compile_find_program("make");
        if (!make) {
            set_err(err,
                    "make not found in PATH (required by the project Makefile)");
            return false;
        }
        ok = compile_plan_make(root, make, plan, err);
        free(make);
        return ok;
    }

    for (i = 0; i < count; i++) {
        if (!compile_is_source(rels[i]))
            continue;
        if (compile_is_cxx_source(rels[i]))
            need_cxx = true;
        else
            need_c = true;
    }
    if (!need_c && !need_cxx) {
        set_err(err, "No C/C++ source files to compile");
        return false;
    }
    if (need_c) {
        cc = compile_find_program("cc");
        if (!cc)
            cc = compile_find_program("gcc");
        if (!cc)
            cc = compile_find_program("clang");
    }
    if (need_cxx) {
        cxx = compile_find_program("c++");
        if (!cxx)
            cxx = compile_find_program("g++");
        if (!cxx)
            cxx = compile_find_program("clang++");
    }
    ok = compile_plan_direct(root, rels, count, cc, cxx, plan, err);
    free(cc);
    free(cxx);
    return ok;
}

static bool mkdir_p(const char *path, char **err)
{
    char *tmp;
    size_t i;
    size_t n;

    if (!path || !*path)
        return true;
    tmp = xstrdup(path);
    n = strlen(tmp);
    for (i = 1; i < n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\\')
            continue;
        tmp[i] = '\0';
        if (compile_mkdir(tmp) != 0 && errno != EEXIST) {
            set_err(err, "Cannot create directory %s: %s", tmp,
                    strerror(errno));
            free(tmp);
            return false;
        }
        tmp[i] = '/';
    }
    if (compile_mkdir(tmp) != 0 && errno != EEXIST) {
        set_err(err, "Cannot create directory %s: %s", tmp, strerror(errno));
        free(tmp);
        return false;
    }
    free(tmp);
    return true;
}

static bool mkdir_parent(const char *root, const char *rel, char **err)
{
    const char *slash = strrchr(rel, '/');
    char *dir;
    char *full;
    bool ok;

    if (!slash)
        return true;
    if (slash == rel)
        return true;
    dir = xstrndup(rel, (size_t)(slash - rel));
    full = path_join(root && *root ? root : ".", dir);
    ok = mkdir_p(full, err);
    free(full);
    free(dir);
    return ok;
}

bool compile_plan_prepare(const char *root, const CompilePlan *plan,
                          char **err)
{
    size_t i;
    if (!plan)
        return true;
    for (i = 0; i < plan->count; i++) {
        const CompileCommand *cmd = &plan->items[i];
        size_t a;
        for (a = 1; a + 1 < cmd->argc; a++) {
            if (strcmp(cmd->argv[a], "-o") == 0) {
                if (!mkdir_parent(root, cmd->argv[a + 1], err))
                    return false;
            }
        }
    }
    if (plan->output && !mkdir_parent(root, plan->output, err))
        return false;
    return true;
}

char *compile_command_display(const CompileCommand *cmd)
{
    Str s;
    size_t i;

    str_init(&s);
    if (!cmd)
        return xstrdup("");
    for (i = 0; i < cmd->argc; i++) {
        const char *arg = cmd->argv[i];
        bool quote = strchr(arg, ' ') || strchr(arg, '\t') ||
                     strchr(arg, '"');
        if (i)
            str_putc(&s, ' ');
        if (!quote) {
            str_puts(&s, arg);
            continue;
        }
        str_putc(&s, '"');
        while (*arg) {
            if (*arg == '"' || *arg == '\\')
                str_putc(&s, '\\');
            str_putc(&s, *arg++);
        }
        str_putc(&s, '"');
    }
    if (!s.data)
        s.data = xstrdup("");
    return s.data;
}

/* ------------------------------------------------------------------ */
/* Child process                                                       */
/* ------------------------------------------------------------------ */

bool compile_process_start(CompileProcess *p, const char *cwd,
                           char *const argv[], char **err)
{
#ifdef _WIN32
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    p->exit_code = -1;
    (void)cwd;
    (void)argv;
    set_err(err, "Compilation is not supported on the Windows console backend");
    return false;
#else
    int fds[2];
    pid_t pid;

    memset(p, 0, sizeof(*p));
    p->fd = -1;
    p->exit_code = -1;
    if (pipe(fds) != 0) {
        set_err(err, "Cannot create compiler pipe: %s", strerror(errno));
        return false;
    }
    pid = fork();
    if (pid < 0) {
        set_err(err, "Cannot start compiler: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0 ||
            dup2(fds[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(fds[1]);
        if (cwd && *cwd && chdir(cwd) != 0) {
            fprintf(stderr, "si: cannot enter %s: %s\n", cwd,
                    strerror(errno));
            _exit(126);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "si: cannot execute %s: %s\n", argv[0],
                strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    p->pid = (long)pid;
    p->fd = fds[0];
    p->running = true;
    p->exit_code = -1;
    clear_err(err);
    return true;
#endif
}

static void drain_fd(CompileProcess *p, Str *out)
{
    char buf[4096];

    if (!p || p->fd < 0)
        return;
    for (;;) {
        ssize_t n = read(p->fd, buf, sizeof(buf));
        if (n > 0) {
            if (out)
                compile_sanitize_append(out, buf, (size_t)n);
            continue;
        }
        if (n == 0) {
            close(p->fd);
            p->fd = -1;
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        close(p->fd);
        p->fd = -1;
        return;
    }
}

int compile_process_poll(CompileProcess *p, Str *out)
{
    int status = 0;
    pid_t r;

    if (!p)
        return -1;
    drain_fd(p, out);
    if (!p->running)
        return 0;
#ifndef _WIN32
    r = waitpid((pid_t)p->pid, &status, WNOHANG);
    if (r == 0)
        return 1;
    if (r == (pid_t)p->pid) {
        p->running = false;
        if (WIFEXITED(status))
            p->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            p->exit_code = 128 + WTERMSIG(status);
        else
            p->exit_code = -1;
        return 0;
    }
    if (r < 0 && errno == EINTR)
        return 1;
    p->running = false;
    p->exit_code = -1;
    return -1;
#else
    p->running = false;
    return 0;
#endif
}

void compile_process_cancel(CompileProcess *p)
{
#ifndef _WIN32
    int i;
    int status = 0;
    pid_t r = 0;

    if (!p || !p->running)
        return;
    p->cancelled = true;
    kill(-(pid_t)p->pid, SIGTERM);
    kill((pid_t)p->pid, SIGTERM);
    for (i = 0; i < 25; i++) {
        struct timespec ts;
        r = waitpid((pid_t)p->pid, &status, WNOHANG);
        if (r == (pid_t)p->pid)
            break;
        if (r < 0 && errno != EINTR)
            break;
        ts.tv_sec = 0;
        ts.tv_nsec = 20 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }
    if (r != (pid_t)p->pid) {
        kill(-(pid_t)p->pid, SIGKILL);
        kill((pid_t)p->pid, SIGKILL);
        while (waitpid((pid_t)p->pid, &status, 0) < 0 && errno == EINTR)
            ;
        r = (pid_t)p->pid;
    }
    p->running = false;
    if (WIFEXITED(status))
        p->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        p->exit_code = 128 + WTERMSIG(status);
    else
        p->exit_code = 130;
    (void)r;
#else
    (void)p;
#endif
}

void compile_process_close(CompileProcess *p)
{
    if (!p)
        return;
    if (p->running)
        compile_process_cancel(p);
#ifndef _WIN32
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
#else
    p->fd = -1;
#endif
    p->pid = 0;
}

/* ------------------------------------------------------------------ */
/* Output sanitizing                                                   */
/* ------------------------------------------------------------------ */

static int current_column(const Str *out)
{
    size_t start = 0;
    size_t i;

    if (!out || !out->data)
        return 0;
    for (i = out->len; i > 0; i--) {
        if (out->data[i - 1] == '\n') {
            start = i;
            break;
        }
    }
    return utf8_text_width(out->data + start, out->len - start);
}

static void strip_escape(const char *data, size_t len, size_t *pos)
{
    size_t i = *pos + 1;

    if (i >= len) {
        *pos = len;
        return;
    }
    if (data[i] == '[') {
        i++;
        while (i < len) {
            unsigned char c = (unsigned char)data[i++];
            if (c >= 0x40 && c <= 0x7E)
                break;
        }
    } else if (data[i] == ']') {
        i++;
        while (i < len) {
            if ((unsigned char)data[i] == 7) {
                i++;
                break;
            }
            if ((unsigned char)data[i] == 27 && i + 1 < len &&
                data[i + 1] == '\\') {
                i += 2;
                break;
            }
            i++;
        }
    } else {
        i++;
    }
    *pos = i;
}

void compile_sanitize_append(Str *out, const char *data, size_t len)
{
    size_t i = 0;
    int col;

    if (!out || !data)
        return;
    col = current_column(out);
    while (i < len) {
        unsigned char c = (unsigned char)data[i];

        if (c == 27) {
            strip_escape(data, len, &i);
            continue;
        }
        if (c == '\r') {
            if (i + 1 < len && data[i + 1] == '\n') {
                i++;
                continue;
            }
            {
                size_t pos = out->len;
                while (pos > 0 && out->data[pos - 1] != '\n')
                    pos--;
                out->len = pos;
                if (out->data)
                    out->data[pos] = '\0';
                col = 0;
            }
            i++;
            continue;
        }
        if (c == '\b') {
            i++;
            continue;
        }
        if (c == '\t') {
            int spaces = 4 - (col % 4);
            while (spaces-- > 0)
                str_putc(out, ' ');
            col += 4 - (col % 4);
            i++;
            continue;
        }
        if (c == '\n') {
            str_putc(out, '\n');
            col = 0;
            i++;
            continue;
        }
        if (c < 32) {
            i++;
            continue;
        }
        if (c < 0x80) {
            str_putc(out, (char)c);
            col++;
            i++;
            continue;
        }
        {
            uint32_t cp;
            int n = utf8_decode(data + i, len - i, &cp);
            if (n > 0) {
                int w = utf8_cp_width(cp);
                str_append(out, data + i, (size_t)n);
                col += w > 0 ? w : 0;
                i += (size_t)n;
            } else {
                str_putc(out, (char)c);
                col++;
                i++;
            }
        }
    }
}
