#include "si.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#endif

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static void oom(void)
{
    fputs("si: out of memory\n", stderr);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        oom();
    return q;
}

char *xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s);
    p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* Str                                                                 */
/* ------------------------------------------------------------------ */

void str_init(Str *s)
{
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_free(Str *s)
{
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_reserve(Str *s, size_t need)
{
    size_t cap;
    if (need <= s->cap)
        return;
    cap = s->cap ? s->cap : 128;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    s->data = xrealloc(s->data, cap);
    s->cap = cap;
}

void str_clear(Str *s)
{
    s->len = 0;
    if (s->data)
        s->data[0] = '\0';
}

void str_append(Str *s, const char *data, size_t len)
{
    if (!len)
        return;
    str_reserve(s, s->len + len + 1);
    memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
}

void str_puts(Str *s, const char *text)
{
    str_append(s, text, strlen(text));
}

void str_putc(Str *s, char c)
{
    str_append(s, &c, 1);
}

void str_vprintf(Str *s, const char *fmt, va_list ap)
{
    va_list cp;
    int n;
    char stackbuf[256];

    va_copy(cp, ap);
    n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, cp);
    va_end(cp);
    if (n < 0) {
        str_puts(s, "(format error)");
        return;
    }
    if ((size_t)n < sizeof(stackbuf)) {
        str_append(s, stackbuf, (size_t)n);
        return;
    }
    str_reserve(s, s->len + (size_t)n + 1);
    va_copy(cp, ap);
    vsnprintf(s->data + s->len, (size_t)n + 1, fmt, cp);
    va_end(cp);
    s->len += (size_t)n;
}

void str_printf(Str *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    str_vprintf(s, fmt, ap);
    va_end(ap);
}

char *xasprintf(const char *fmt, ...)
{
    Str s;
    va_list ap;
    str_init(&s);
    va_start(ap, fmt);
    str_vprintf(&s, fmt, ap);
    va_end(ap);
    if (!s.data) {
        s.data = xstrdup("");
    }
    return s.data;
}

/* ------------------------------------------------------------------ */
/* String lists                                                        */
/* ------------------------------------------------------------------ */

void strlist_init(StrList *l)
{
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void strlist_free(StrList *l)
{
    size_t i;
    for (i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void strlist_push(StrList *l, const char *s)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = xrealloc(l->items, l->cap * sizeof(*l->items));
    }
    l->items[l->count++] = xstrdup(s);
}

static int cmp_cstr(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

void strlist_sort(StrList *l)
{
    if (l->count > 1)
        qsort(l->items, l->count, sizeof(*l->items), cmp_cstr);
}

bool strlist_contains_n(const StrList *l, const char *s, size_t len)
{
    size_t lo = 0;
    size_t hi = l->count;
    if (!s || len == 0)
        return false;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *item = l->items[mid];
        int cmp = strncmp(item, s, len);
        if (cmp == 0) {
            if (item[len] == '\0')
                return true;
            cmp = 1; /* item is longer, so key sorts before item */
        }
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* ASCII helpers                                                       */
/* ------------------------------------------------------------------ */

static int lower_ascii(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

bool str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower_ascii((unsigned char)*a) != lower_ascii((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int str_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = lower_ascii((unsigned char)*a);
        int cb = lower_ascii((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return lower_ascii((unsigned char)*a) - lower_ascii((unsigned char)*b);
}

bool str_icontains(const char *haystack, const char *needle)
{
    size_t nl;
    if (!needle || !*needle)
        return true;
    if (!haystack)
        return false;
    nl = strlen(needle);
    for (; *haystack; haystack++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            if (!haystack[i])
                return false;
            if (lower_ascii((unsigned char)haystack[i]) !=
                lower_ascii((unsigned char)needle[i]))
                break;
        }
        if (i == nl)
            return true;
    }
    return false;
}

char *str_trim_dup(const char *s, size_t n)
{
    size_t a = 0;
    size_t b = n;
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    return xstrndup(s + a, b - a);
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

static bool is_sep(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

char *path_join(const char *a, const char *b)
{
    size_t alen;
    bool need_sep;
    char *out;
    if (!a || !*a)
        return xstrdup(b ? b : "");
    if (!b || !*b)
        return xstrdup(a);
#if defined(_WIN32)
    if (b[0] == '/' || b[0] == '\\' || (isalpha((unsigned char)b[0]) && b[1] == ':'))
        return xstrdup(b);
#else
    if (b[0] == '/')
        return xstrdup(b);
#endif
    alen = strlen(a);
    need_sep = !is_sep(a[alen - 1]);
    out = xmalloc(alen + (need_sep ? 1 : 0) + strlen(b) + 1);
    memcpy(out, a, alen);
    if (need_sep)
        out[alen++] = '/';
    strcpy(out + alen, b);
    return out;
}

char *path_abs(const char *path)
{
#if defined(_WIN32)
    char *p = _fullpath(NULL, path, 0);
    if (p)
        return p;
#else
    char *p = realpath(path, NULL);
    if (p)
        return p;
#endif
    if (path[0] == '/' || (isalpha((unsigned char)path[0]) && path[1] == ':'))
        return xstrdup(path);
    {
        char cwd[PATH_MAX];
#if defined(_WIN32)
        if (_getcwd(cwd, sizeof(cwd)))
            return path_join(cwd, path);
#else
        if (getcwd(cwd, sizeof(cwd)))
            return path_join(cwd, path);
#endif
    }
    return xstrdup(path);
}

char *path_dirname(const char *path)
{
    char *out = xstrdup(path);
    size_t n = strlen(out);
    while (n > 1 && is_sep(out[n - 1]))
        out[--n] = '\0';
    while (n > 0 && !is_sep(out[n - 1]))
        n--;
    if (n == 0) {
        free(out);
        return xstrdup(".");
    }
    while (n > 1 && is_sep(out[n - 1]))
        n--;
    out[n] = '\0';
    if (!*out) {
        free(out);
        return xstrdup("/");
    }
    return out;
}

const char *path_basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    if (!path)
        return "";
    for (; *p; p++) {
        if (is_sep(*p))
            last = p + 1;
    }
    return last;
}

bool path_exists(const char *path)
{
#if defined(_WIN32)
    struct _stat st;
    return _stat(path, &st) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

bool path_is_dir(const char *path)
{
#if defined(_WIN32)
    struct _stat st;
    return _stat(path, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool path_is_file(const char *path)
{
#if defined(_WIN32)
    struct _stat st;
    return _stat(path, &st) == 0 && (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static const char *ext_of(const char *path)
{
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot + 1 : "";
}

bool has_source_ext(const char *path)
{
    static const char *const exts[] = {
        "c", "h", "cc", "cpp", "cxx", "c++", "hh", "hpp", "hxx",
        "h++", "inl", "ipp", "tpp", "m", "mm"
    };
    const char *ext = ext_of(path);
    size_t i;
    if (!*ext)
        return false;
    for (i = 0; i < SI_ARRAY_LEN(exts); i++) {
        if (str_ieq(ext, exts[i]))
            return true;
    }
    return false;
}

void path_to_slashes(char *s)
{
    for (; *s; s++) {
        if (*s == '\\')
            *s = '/';
    }
}

void path_shorten(const char *path, int width, char *out, size_t out_size)
{
    size_t len;
    size_t start;
    int w = 0;
    int budget;

    if (out_size == 0)
        return;
    out[0] = '\0';
    if (!path || width <= 0)
        return;

    len = strlen(path);
    if (utf8_text_width(path, len) <= width) {
        if (len + 1 <= out_size) {
            memcpy(out, path, len + 1);
        } else {
            memcpy(out, path, out_size - 1);
            out[out_size - 1] = '\0';
        }
        return;
    }

    budget = width > 3 ? width - 3 : width;
    start = len;
    while (start > 0) {
        size_t prev = utf8_prev(path, start);
        uint32_t cp;
        int n = utf8_decode(path + prev, start - prev, &cp);
        int cw = n > 0 ? utf8_cp_width(cp) : 1;
        if (n <= 0)
            n = 1;
        if (w + cw > budget)
            break;
        w += cw;
        start = prev;
    }

    if (width > 3 && start > 0) {
        snprintf(out, out_size, "...%s", path + start);
    } else {
        snprintf(out, out_size, "%s", path + start);
    }
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

bool file_read_all(const char *path, size_t max_size,
                   char **out, size_t *out_len, char **err)
{
    FILE *f;
    long size;
    char *data;

    *out = NULL;
    *out_len = 0;
    if (err)
        *err = NULL;

    if (!path_is_file(path)) {
        if (err)
            *err = xasprintf("not a regular file: %s", path);
        return false;
    }

    f = fopen(path, "rb");
    if (!f) {
        if (err)
            *err = xasprintf("cannot open %s: %s", path, strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        if (err)
            *err = xasprintf("cannot seek %s: %s", path, strerror(errno));
        fclose(f);
        return false;
    }
    size = ftell(f);
    if (size < 0) {
        if (err)
            *err = xasprintf("cannot size %s: %s", path, strerror(errno));
        fclose(f);
        return false;
    }
    if ((unsigned long)size > max_size) {
        if (err)
            *err = xasprintf("%s is too large (%ld bytes, limit %lu)",
                             path, size, (unsigned long)max_size);
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        if (err)
            *err = xasprintf("cannot rewind %s: %s", path, strerror(errno));
        fclose(f);
        return false;
    }

    data = xmalloc((size_t)size + 1);
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        if (err)
            *err = xasprintf("cannot read %s: %s", path, strerror(errno));
        free(data);
        fclose(f);
        return false;
    }
    data[size] = '\0';
    fclose(f);

    *out = data;
    *out_len = (size_t)size;
    return true;
}

#if defined(_WIN32)
bool file_write_atomic(const char *path, const char *data, size_t len,
                       char **err)
{
    char tmp[PATH_MAX];
    FILE *f;
    int pid = (int)_getpid();
    if (err)
        *err = NULL;
    snprintf(tmp, sizeof(tmp), "%s.si-tmp-%d", path, pid);
    f = fopen(tmp, "wb");
    if (!f) {
        if (err)
            *err = xasprintf("cannot create %s: %s", tmp, strerror(errno));
        return false;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        if (err)
            *err = xasprintf("cannot write %s: %s", tmp, strerror(errno));
        fclose(f);
        remove(tmp);
        return false;
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        if (err)
            *err = xasprintf("cannot flush %s: %s", tmp, strerror(errno));
        remove(tmp);
        return false;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH)) {
        if (err)
            *err = xasprintf("cannot replace %s (error %lu)", path,
                             (unsigned long)GetLastError());
        remove(tmp);
        return false;
    }
    return true;
}
#else
bool file_write_atomic(const char *path, const char *data, size_t len,
                       char **err)
{
    char *dir = path_dirname(path);
    char *tmp;
    int fd;
    size_t off = 0;
    struct stat st;
    bool have_mode = stat(path, &st) == 0;

    if (err)
        *err = NULL;

    tmp = xasprintf("%s/.si-tmp-XXXXXX", dir);
    fd = mkstemp(tmp);
    if (fd < 0) {
        if (err)
            *err = xasprintf("cannot create temporary file in %s: %s",
                             dir, strerror(errno));
        free(tmp);
        free(dir);
        return false;
    }
    if (have_mode)
        (void)fchmod(fd, st.st_mode & 0777);

    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (err)
                *err = xasprintf("write %s: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            free(tmp);
            free(dir);
            return false;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) {
        if (err)
            *err = xasprintf("fsync %s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        free(tmp);
        free(dir);
        return false;
    }
    if (close(fd) != 0) {
        if (err)
            *err = xasprintf("close %s: %s", tmp, strerror(errno));
        unlink(tmp);
        free(tmp);
        free(dir);
        return false;
    }
    if (rename(tmp, path) != 0) {
        if (err)
            *err = xasprintf("rename %s -> %s: %s", tmp, path,
                             strerror(errno));
        unlink(tmp);
        free(tmp);
        free(dir);
        return false;
    }
    free(tmp);
    free(dir);
    return true;
}
#endif

bool path_stat_info(const char *path, long long *size, long long *mtime)
{
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(path, &st) != 0)
        return false;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
#endif
    if (size)
        *size = (long long)st.st_size;
    if (mtime)
        *mtime = (long long)st.st_mtime;
    return true;
}

char *state_path(void)
{
    const char *home = getenv(
#if defined(_WIN32)
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    if (!home || !*home)
        home = ".";
    return path_join(home, ".si-c-state");
}

bool state_load(char **root, char **file, int *cursor, StrList *recent)
{
    char *path = state_path();
    FILE *f = fopen(path, "rb");
    char line[8192];
    char *r = NULL;
    char *fn = NULL;
    int cur = 0;
    bool ok = false;

    if (root)
        *root = NULL;
    if (file)
        *file = NULL;
    if (cursor)
        *cursor = 0;
    if (!f) {
        free(path);
        return false;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (strncmp(line, "root=", 5) == 0) {
            free(r);
            r = xstrdup(line + 5);
        } else if (strncmp(line, "file=", 5) == 0) {
            free(fn);
            fn = xstrdup(line + 5);
        } else if (strncmp(line, "cursor=", 7) == 0) {
            cur = atoi(line + 7);
            if (cur < 0)
                cur = 0;
        } else if (recent && strncmp(line, "recent=", 7) == 0) {
            const char *value = line + 7;
            if (*value &&
                !strlist_contains_n(recent, value, strlen(value))) {
                strlist_push(recent, value);
            }
        }
    }
    fclose(f);
    /*
     * Older state files only have root=.  Keep that project visible in
     * the picker while the new recent= history is being populated.
     */
    if (recent && recent->count == 0 && r && *r)
        strlist_push(recent, r);
    if (r && *r) {
        if (root) {
            *root = r;
            r = NULL;
        }
        if (file) {
            *file = fn;
            fn = NULL;
        }
        if (cursor)
            *cursor = cur;
        ok = true;
    }
    if (recent && recent->count > 0)
        ok = true;
    free(path);
    free(r);
    free(fn);
    return ok;
}

bool state_save(const char *root, const char *file, int cursor,
                int scroll_line, int scroll_col, const StrList *recent)
{
    char *path;
    FILE *f;
    size_t i;
    bool ok;
    path = state_path();
    f = fopen(path, "wb");
    if (!f) {
        free(path);
        return false;
    }
    fprintf(f, "root=%s\n", root ? root : "");
    fprintf(f, "file=%s\n", file ? file : "");
    fprintf(f, "cursor=%d\n", cursor > 0 ? cursor : 0);
    fprintf(f, "scroll_line=%d\n", scroll_line > 0 ? scroll_line : 0);
    fprintf(f, "scroll_col=%d\n", scroll_col > 0 ? scroll_col : 0);
    if (recent) {
        for (i = 0; i < recent->count; i++) {
            if (recent->items[i] && *recent->items[i])
                fprintf(f, "recent=%s\n", recent->items[i]);
        }
    }
    ok = fflush(f) == 0;
    if (fclose(f) != 0)
        ok = false;
    free(path);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Directory iteration                                                 */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
struct DirIter {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    bool first;
    bool done;
    char base[PATH_MAX];
};

DirIter *dir_open(const char *path)
{
    DirIter *it = xcalloc(1, sizeof(*it));
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    it->handle = FindFirstFileA(pattern, &it->data);
    if (it->handle == INVALID_HANDLE_VALUE) {
        free(it);
        return NULL;
    }
    snprintf(it->base, sizeof(it->base), "%s", path);
    it->first = true;
    return it;
}

bool dir_next(DirIter *it, DirEntry *ent)
{
    if (it->done)
        return false;
    if (it->first) {
        it->first = false;
    } else if (!FindNextFileA(it->handle, &it->data)) {
        it->done = true;
        return false;
    }
    snprintf(ent->name, sizeof(ent->name), "%s", it->data.cFileName);
    ent->is_dir = (it->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ent->is_symlink =
        (it->data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

void dir_close(DirIter *it)
{
    if (!it)
        return;
    if (it->handle != INVALID_HANDLE_VALUE)
        FindClose(it->handle);
    free(it);
}
#else
struct DirIter {
    DIR *dir;
    char base[PATH_MAX];
};

DirIter *dir_open(const char *path)
{
    DirIter *it = xcalloc(1, sizeof(*it));
    it->dir = opendir(path);
    if (!it->dir) {
        free(it);
        return NULL;
    }
    snprintf(it->base, sizeof(it->base), "%s", path);
    return it;
}

bool dir_next(DirIter *it, DirEntry *ent)
{
    struct dirent *de;
    struct stat st;
    char full[PATH_MAX];

    for (;;) {
        de = readdir(it->dir);
        if (!de)
            return false;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(ent->name, sizeof(ent->name), "%s", de->d_name);
#if defined(DT_UNKNOWN)
        if (de->d_type != DT_UNKNOWN) {
            /*
             * ext4/xfs/btrfs usually fill d_type, so scanning a large
             * tree does not need one lstat() per directory entry.
             */
            ent->is_dir = de->d_type == DT_DIR;
            ent->is_symlink = de->d_type == DT_LNK;
        } else
#endif
        {
            snprintf(full, sizeof(full), "%s/%s", it->base, de->d_name);
            if (lstat(full, &st) == 0) {
                ent->is_dir = S_ISDIR(st.st_mode);
                ent->is_symlink = S_ISLNK(st.st_mode);
            } else {
                ent->is_dir = false;
                ent->is_symlink = false;
            }
        }
        return true;
    }
}

void dir_close(DirIter *it)
{
    if (!it)
        return;
    if (it->dir)
        closedir(it->dir);
    free(it);
}
#endif

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

int utf8_decode(const char *s, size_t len, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c;
    int n;
    int i;

    if (len == 0)
        return 0;
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0) {
        n = 2;
        c = p[0] & 0x1F;
    } else if ((p[0] & 0xF0) == 0xE0) {
        n = 3;
        c = p[0] & 0x0F;
    } else if ((p[0] & 0xF8) == 0xF0) {
        n = 4;
        c = p[0] & 0x07;
    } else {
        return 0;
    }
    if ((size_t)n > len)
        return 0;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        c = (c << 6) | (uint32_t)(p[i] & 0x3F);
    }
    if (n == 2 && c < 0x80)
        return 0;
    if (n == 3 && c < 0x800)
        return 0;
    if (n == 4 && c < 0x10000)
        return 0;
    if (c >= 0xD800 && c <= 0xDFFF)
        return 0;
    if (c > 0x10FFFF)
        return 0;
    *cp = c;
    return n;
}

int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_cp_width(uint32_t cp)
{
    if (cp == 0)
        return 1;
    if (cp == '\t')
        return SI_TAB_WIDTH;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0))
        return 1;
    /* Combining marks. */
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F) ||
        cp == 0x200B || cp == 0x200C || cp == 0x200D ||
        cp == 0xFEFF)
        return 0;
    /* East Asian wide / fullwidth and emoji. */
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

int utf8_text_width(const char *s, size_t len)
{
    size_t i = 0;
    int width = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (n <= 0) {
            width += 1;
            i += 1;
        } else {
            width += utf8_cp_width(cp);
            i += (size_t)n;
        }
    }
    return width;
}

size_t utf8_prev(const char *s, size_t pos)
{
    size_t p;
    if (pos == 0)
        return 0;
    p = pos - 1;
    while (p > 0 && (((unsigned char)s[p] & 0xC0) == 0x80) && pos - p < 4)
        p--;
    return p;
}
