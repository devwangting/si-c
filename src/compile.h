/*
 * compile.h -- project build detection and child-process output capture.
 *
 * The module is deliberately independent of the UI: it discovers a usable
 * toolchain, builds the command plan, and runs one command at a time while
 * the caller consumes sanitized output.
 */
#ifndef SI_COMPILE_H
#define SI_COMPILE_H

#include "si.h"

typedef struct {
    char **argv;
    size_t argc;
} CompileCommand;

typedef struct {
    CompileCommand *items;
    size_t count;
    size_t cap;
    char *output;  /* build/<name> for direct builds, NULL for make */
    bool uses_make;
} CompilePlan;

typedef struct {
    long pid;
    int fd;
    bool running;
    bool cancelled;
    int exit_code;
} CompileProcess;

void compile_plan_init(CompilePlan *plan);
void compile_plan_free(CompilePlan *plan);

bool compile_is_source(const char *path);
bool compile_is_cxx_source(const char *path);
char *compile_find_program(const char *name);
bool compile_has_makefile(const char *root);

bool compile_plan_make(const char *root, const char *make_program,
                       CompilePlan *plan, char **err);
bool compile_plan_direct(const char *root, const char *const *rels,
                         size_t count, const char *cc, const char *cxx,
                         CompilePlan *plan, char **err);
bool compile_plan_build(const char *root, const char *const *rels,
                        size_t count, CompilePlan *plan, char **err);
bool compile_plan_prepare(const char *root, const CompilePlan *plan,
                          char **err);
char *compile_command_display(const CompileCommand *cmd);

bool compile_process_start(CompileProcess *p, const char *cwd,
                           char *const argv[], char **err);
/* Returns 1 while running, 0 when reaped, -1 on wait error. */
int compile_process_poll(CompileProcess *p, Str *out);
void compile_process_cancel(CompileProcess *p);
void compile_process_close(CompileProcess *p);

/* Append compiler output after removing ANSI escapes, carriage-return
 * rewrites, and control bytes; tabs expand to 4-column stops. */
void compile_sanitize_append(Str *out, const char *data, size_t len);

#endif /* SI_COMPILE_H */
