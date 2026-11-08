/*
 * si.h -- shared definitions for si-c, a small Source Insight style editor.
 *
 * The program deliberately depends only on C11 + POSIX (and the Win32
 * console API when built on Windows for development).  No ncurses, no
 * SQLite, no third party libraries.
 */
#ifndef SI_H
#define SI_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SI_VERSION "0.1.0"
#define SI_TAB_WIDTH 4
#define SI_MAX_FILE_SIZE (4u * 1024u * 1024u)
#define SI_MAX_PROJECT_FILES 20000
#define SI_MAX_SYMBOLS 200000
#define SI_MAX_REFS 5000
#define SI_MAX_VAR_USES 500000
#define SI_RELATION_AUTO_MAX_FILES 2000
#define SI_RELATION_AUTO_MAX_DEPTH 12
#define SI_MAX_UNDO_RECORDS 20000

#define SI_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ */
/* Growable string                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Str;

void str_init(Str *s);
void str_free(Str *s);
void str_clear(Str *s);
void str_reserve(Str *s, size_t need);
void str_append(Str *s, const char *data, size_t len);
void str_puts(Str *s, const char *text);
void str_putc(Str *s, char c);
void str_printf(Str *s, const char *fmt, ...);
void str_vprintf(Str *s, const char *fmt, va_list ap);

/* ------------------------------------------------------------------ */
/* String lists                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StrList;

void strlist_init(StrList *l);
void strlist_free(StrList *l);
void strlist_push(StrList *l, const char *s);
void strlist_sort(StrList *l);
bool strlist_contains_n(const StrList *l, const char *s, size_t len);

/* ------------------------------------------------------------------ */
/* Allocation / strings                                                */
/* ------------------------------------------------------------------ */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...);

bool str_ieq(const char *a, const char *b);
bool str_icontains(const char *haystack, const char *needle);
int str_icmp(const char *a, const char *b);
char *str_trim_dup(const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* Paths and files                                                     */
/* ------------------------------------------------------------------ */

char *path_join(const char *a, const char *b);
char *path_abs(const char *path);
char *path_dirname(const char *path);
const char *path_basename(const char *path);
bool path_is_dir(const char *path);
bool path_is_file(const char *path);
bool path_exists(const char *path);
bool has_source_ext(const char *path);
void path_to_slashes(char *s);
void path_shorten(const char *path, int width, char *out, size_t out_size);

bool file_read_all(const char *path, size_t max_size,
                   char **out, size_t *out_len, char **err);
bool file_write_atomic(const char *path, const char *data, size_t len,
                       char **err);
bool path_stat_info(const char *path, long long *size, long long *mtime);

/* Last project/editor position, stored in the user's home directory. */
char *state_path(void);
bool state_load(char **root, char **file, int *cursor);
bool state_save(const char *root, const char *file, int cursor,
                int scroll_line, int scroll_col);

/* ------------------------------------------------------------------ */
/* Directory iteration                                                 */
/* ------------------------------------------------------------------ */

typedef struct DirIter DirIter;

typedef struct {
    char name[1024];
    bool is_dir;
    bool is_symlink;
} DirEntry;

DirIter *dir_open(const char *path);
bool dir_next(DirIter *it, DirEntry *ent);
void dir_close(DirIter *it);

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

int utf8_decode(const char *s, size_t len, uint32_t *cp);
int utf8_encode(uint32_t cp, char out[4]);
int utf8_cp_width(uint32_t cp);
int utf8_text_width(const char *s, size_t len);
size_t utf8_prev(const char *s, size_t pos);

/* ------------------------------------------------------------------ */
/* Misc                                                                */
/* ------------------------------------------------------------------ */

#define SI_MIN(a, b) ((a) < (b) ? (a) : (b))
#define SI_MAX(a, b) ((a) > (b) ? (a) : (b))

#endif /* SI_H */
