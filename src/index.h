#ifndef SI_INDEX_H
#define SI_INDEX_H

#include "si.h"

typedef struct {
    int *items;
    size_t count;
    size_t cap;
} IntList;

void intlist_init(IntList *l);
void intlist_free(IntList *l);
void intlist_clear(IntList *l);
void intlist_push(IntList *l, int value);

typedef enum {
    SYM_FUNCTION = 0,
    SYM_PROTOTYPE,
    SYM_STRUCT,
    SYM_UNION,
    SYM_ENUM,
    SYM_CLASS,
    SYM_NAMESPACE,
    SYM_TYPEDEF,
    SYM_MACRO,
    SYM_VARIABLE,
    SYM_ENUMVAL,
    SYM_MEMBER
} SymbolKind;

typedef struct {
    char *name;
    char *signature;
    int file_index;
    int line;     /* 1-based */
    int col;      /* 0-based byte offset within the line */
    int end_line; /* 1-based, inclusive */
    SymbolKind kind;
} Symbol;

typedef struct {
    char *path; /* absolute */
    char *rel;  /* forward slashes, project relative */
    char *name;
    size_t size;
    long long mtime;
    int sym_start;
    int sym_count;
} ProjectFile;

typedef struct {
    char *name;
    char *type;
    char *signature;
    bool is_array;
    bool is_method;
    bool is_static;
    bool is_type;
} ProjectMember;

typedef struct {
    ProjectMember *items;
    size_t count;
    size_t cap;
} ProjectMemberList;

typedef struct CallGraph CallGraph;

typedef struct {
    int variable;  /* symbol index of the global variable */
    int function;  /* symbol index of the enclosing function */
    int file_index;
    int line;
    int col;
} VarUse;

typedef struct {
    char *root;
    ProjectFile *files;
    size_t file_count;
    size_t file_cap;
    Symbol *syms;
    size_t sym_count;
    size_t sym_cap;
    int warnings;
    char *last_warning;
    CallGraph *call_graph; /* lazily built, owned by Project */
    VarUse *var_uses;      /* global-variable uses inside functions */
    size_t var_use_count;
    size_t var_use_cap;
    bool relations_ready;
    int scan_depth;
} Project;

typedef struct {
    int file_index;
    int line;
    int col;
    int depth;
    char *preview;
} Location;

typedef struct {
    Location *items;
    size_t count;
    size_t cap;
} LocationList;

void loclist_init(LocationList *l);
void loclist_free(LocationList *l);
void loclist_clear(LocationList *l);
void loclist_push(LocationList *l, int file_index, int line, int col,
                  const char *preview);
void loclist_push_depth(LocationList *l, int file_index, int line, int col,
                        int depth, const char *preview);

typedef struct {
    int symbol_index;
    LocationList callers; /* functions that reference the symbol */
    IntList callees;      /* functions called by the symbol, one level */
    int ref_count;
} SymbolContext;

void symbol_context_init(SymbolContext *ctx);
void symbol_context_free(SymbolContext *ctx);
bool project_context(Project *p, int symbol_index, SymbolContext *ctx);
bool project_caller_tree(Project *p, int symbol_index, LocationList *out,
                         int max_depth);

/* Return false to cancel indexing (for example when the user presses Esc). */
typedef bool (*ProjectProgress)(void *ud, size_t files, size_t symbols,
                                const char *path);

void project_member_list_init(ProjectMemberList *l);
void project_member_list_free(ProjectMemberList *l);
bool project_type_aggregate_key(const char *type_text,
                                char *out, size_t out_size);
bool project_aggregate_members(Project *p, const char *key,
                               ProjectMemberList *out);

bool project_load(Project *p, const char *root, ProjectProgress progress,
                  void *ud, char **err);
bool project_load_cached(Project *p, const char *root,
                         ProjectProgress progress, void *ud, char **err,
                         bool *from_cache);
bool project_cache_save(const Project *p, char **err);
bool project_build_relations(Project *p, ProjectProgress progress, void *ud,
                             char **err);
bool project_relations_ready(const Project *p);
size_t project_call_edge_count(const Project *p);
size_t project_var_use_count(const Project *p);
void project_free(Project *p);

const char *symbol_kind_name(SymbolKind kind);
const char *symbol_kind_short(SymbolKind kind);
bool symbol_short_name(const char *signature, char *out, size_t out_size);

void project_file_symbols(Project *p, int file_index, IntList *out);
void project_find_symbols(Project *p, const char *name, IntList *out);
void project_search_symbols(Project *p, const char *query, IntList *out);
void project_find_refs(Project *p, const char *name, LocationList *out);
void project_search_text(Project *p, const char *query, LocationList *out);

int project_find_file_by_rel(Project *p, const char *rel);
int project_file_index_of_path(Project *p, const char *path);

#endif /* SI_INDEX_H */
