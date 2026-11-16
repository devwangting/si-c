# 变量补全 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `si-c` 编辑器里实现逐字符变量/函数/宏/类型/关键字补全，以及 `.` / `->` 的结构体成员补全（含链式访问），支持键盘和鼠标选择。

**Architecture:** 复用现有 `LocalVar`、`Project.syms`、`Syntax` 和 `Str` 渲染管线。`index.c` 负责补齐全局变量解析和按需提取聚合成员；`app.c` 负责候选收集、上下文判断、接受/抑制和按键/鼠标状态机；`render.c` 负责光标旁浮层。运行时新增按名字排序的符号指针索引和成员缓存，不改变 `.si-index` 磁盘格式。

**Tech Stack:** C11、POSIX、ANSI 转义序列、现有 Makefile；无第三方依赖。

**Spec:** `docs/superpowers/specs/2026-11-16-variable-completion-design.md`

## Global Constraints

- 全程使用中文交流；代码、标识符和源码注释沿用现有英文风格。
- 只使用 C11 + POSIX + ANSI；不得引入 ncurses、SQLite、LSP、第三方库或新构建系统。
- 编译选项固定为 `-std=c11 -Wall -Wextra -Wpedantic`，不得新增警告。
- 修改 `.h` 后必须 `make clean` 再构建；`Makefile` 的 `%.o: %.c` 不跟踪头文件依赖。
- 不新增 `.c` 文件；如果最终决定拆分，必须同步更新 `Makefile` 的 `SRC`、`CORE`、`UI_CORE`。
- 不修改 `Symbol` / `Project` / `ProjectFile` 的持久化字段或顺序，不提升 `SI_CACHE_VERSION`。
- `.si-index` 格式不变；旧缓存缺少新增全局变量符号，需要 F5 重建后才能看到。
- 渲染内容必须写入 `Str`，最后一次性 `term_write()`；不要在 `render.c` 里直接写 stdout。
- UTF-8 光标移动/删除必须使用 `utf8_prev()` / `utf8_next()` / `buf_*` 辅助函数。
- 测试使用 `tests/fixtures/demo`；`examples/demo` 只用于手动运行。
- 每个任务结束前运行对应测试；最终必须 `make clean && make test` 全绿。
- 每个任务单独提交，提交信息使用 `feat:` / `fix:` / `test:` / `docs:` 前缀。

## Review Focus

- 光标在标识符中间接受补全时，必须替换整个标识符，不能留下重复后缀或破坏后续字符。
- 注释、字符串、预处理行里输入时不能弹补全，也不能改动缓冲区。
- 同名符号（如 fixture 的 `Point` 同时有 struct 和 typedef）不能让自动“选择定义”框打断补全。
- 候选很多、屏幕很小时，候选总数不能被截断；只能限制可视行并滚动。
- 鼠标单击只能高亮，双击才接受；点击坐标必须与最近一次渲染的浮层几何一致。

---

### Task 1: 暴露语法关键字/类型列表

**Files:**
- Modify: `src/syntax.h`
- Modify: `src/syntax.c`
- Test: `tests/test_core.c`

**Interfaces:**
- Consumes: 现有 `src/syntax.c` 里的静态 `keywords[]` / `types[]`。
- Produces:
  - `size_t syntax_keyword_count(void)`
  - `const char *syntax_keyword_at(size_t index)`
  - `size_t syntax_type_count(void)`
  - `const char *syntax_type_at(size_t index)`
  - 越界返回 `NULL`。

- [ ] **Step 1: 写失败测试**

在 `tests/test_core.c` 增加 `test_syntax_lists()`，并在 `main()` 调用：

```c
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
```

同时在文件顶部 `#include "syntax.h"`。

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make test_core && ./test_core tests/fixtures/demo`

Expected: 编译失败，提示 `syntax_keyword_count` 等未定义。

- [ ] **Step 3: 实现访问器**

在 `src/syntax.h` 加声明；在 `src/syntax.c` 返回静态数组的长度和元素：

```c
size_t syntax_keyword_count(void);
const char *syntax_keyword_at(size_t index);
size_t syntax_type_count(void);
const char *syntax_type_at(size_t index);
```

实现时越界返回 `NULL`。

- [ ] **Step 4: 运行测试确认通过**

Run: `make clean && make test_core && ./test_core tests/fixtures/demo`

Expected: 全部通过，无新增警告。

- [ ] **Step 5: 提交**

```bash
git add src/syntax.h src/syntax.c tests/test_core.c
git commit -m "feat: expose syntax keyword and type lists"
```

---

### Task 2: 补齐全局变量解析并扩展 fixture

**Files:**
- Modify: `src/index.c`
- Modify: `tests/test_core.c`
- Modify: `tests/fixtures/demo/main.c`

**Interfaces:**
- Consumes: 现有 `parse_variables()`、`variable_segment()`、`add_symbol()`。
- Produces:
  - `SYM_VARIABLE` 覆盖 `struct Point origin;`、`Point world = {1, 2};`、
    `struct Point *ptr;`、多声明符和数组。
  - 每个变量 `Symbol.signature` 以完整类型开头，例如 `int b`、
    `struct Point * q`。
  - 不新增公开 API。

- [ ] **Step 1: 在 fixture 末尾追加测试代码**

保持现有 `main` 在第 16 行、`global_counter` 在第 23 行不变，只在文件末尾追加：

```c
typedef struct CompletionNode {
    Point pos;
    struct CompletionNode *next;
    int alpha, beta;
} CompletionNode;

typedef struct {
    int alpha, beta;
} CompletionAnon;

struct CompletionForward;

struct Point completion_origin;
Point completion_world = {1, 2};
struct Point *completion_ptr;
int completion_a, completion_b;
struct Point completion_p, *completion_q;
int completion_arr[4];

int completion_demo(void)
{
    int result = 0, other;
    Point local_p, *local_q;
    CompletionNode node;
    node.pos.x = result + other + local_p.x + local_q->x;
    return node.pos.y + completion_origin.x + completion_world.y +
           completion_ptr->x;
}
```

- [ ] **Step 2: 写失败测试**

在 `tests/test_core.c` 增加 `test_global_variables(Project *p)`：

```c
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
```

调用并检查：

```c
check_variable(&p, "completion_origin", "struct Point");
check_variable(&p, "completion_world", "Point");
check_variable(&p, "completion_ptr", "Point");
check_variable(&p, "completion_a", "int");
check_variable(&p, "completion_b", "int");
check_variable(&p, "completion_p", "struct Point");
check_variable(&p, "completion_q", "struct Point *");
check_variable(&p, "completion_arr", "int");
```

另外检查前置声明不产生变量：

```c
intlist_init(&ids);
project_find_symbols(&p, "CompletionForward", &ids);
check(ids.count == 0, "forward declaration is not a variable");
intlist_free(&ids);
```

原有 `add` 原型+定义仍为 2 条。

- [ ] **Step 3: 运行测试确认失败**

Run: `make clean && make test_core && ./test_core tests/fixtures/demo`

Expected: `completion_origin` 等找不到，或签名缺少类型。

- [ ] **Step 4: 修改 `parse_variables()`**

行为要求：

- 继续只处理顶层 `;` 结尾语句。
- 继续跳过 `typedef`、`using`、`template`、`namespace`、`static_assert`。
- 以 `struct`/`union`/`enum`/`class` 开头的声明：
  - 有 `{...}` 聚合体时跳过聚合体，只解析右花括号后的声明符；
  - 没有聚合体时，若关键字后只有一个标识符（`struct Foo;`），视为
    前置声明，不产生变量；
  - 若关键字后有两个或更多标识符（`struct Foo origin;`），最后一个
    是变量名。
- `=` 后的初始化器（包括 `{...}` 和 `foo()`）不参与声明符解析。
- `(` 在 `=` 之前出现时跳过该语句（函数声明/定义、函数指针变量）。
- 顶层逗号切分声明符；每个声明符的签名必须以完整基础类型开头：
  - 新增 `add_symbol_raw()` 或等价 helper，允许传入拼接好的签名；
  - `variable_segment()` 接收基础类型文本，非首个声明符时生成
    `基础类型 + 当前段指针/限定符 + 名字`；
  - `int a, b;` → `int a`、`int b`；
  - `struct Point p, *q;` → `struct Point p`、`struct Point * q`。
- 数组声明符（`int arr[4];`）仍记录名字，签名不要求包含 `[4]`。
- 不要为聚合 tag、函数名或 typedef 产生 `SYM_VARIABLE`。

- [ ] **Step 5: 运行测试确认通过**

Run: `make clean && make test_core && ./test_core tests/fixtures/demo`

Expected: `test_global_variables` 全部通过，原有测试不回归。

- [ ] **Step 6: 提交**

```bash
git add src/index.c tests/test_core.c tests/fixtures/demo/main.c
git commit -m "feat: index common global variable declarations"
```

---

### Task 3: 补齐局部多声明符

**Files:**
- Modify: `src/app.c`
- Test: `tests/ui_smoke.c`

**Interfaces:**
- Consumes: `type_prefix()`、`app_rebuild_locals()`、`LocalVar`。
- Produces: `LocalVar` 表包含 `int result = 0, other;` 的 `other`，
  以及 `Point local_p, *local_q;` 的 `local_q`。

- [ ] **Step 1: 写失败测试**

在 `tests/ui_smoke.c` 增加辅助函数：

```c
static LocalVar *find_local(App *a, const char *name)
{
    size_t i;
    for (i = 0; i < a->nlocals; i++) {
        if (strcmp(a->locals[i].name, name) == 0)
            return &a->locals[i];
    }
    return NULL;
}
```

在打开 `main.c` 后、进入其他测试前，定位 `completion_demo` 并把光标放到函数体内，调用 `app_rebuild_locals(&a)`：

```c
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
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: `local other` 或 `local local_q` 失败。

- [ ] **Step 3: 实现逗号声明回退**

修改 `type_prefix()`：

- 当前词前没有直接类型时，向前找到同一语句的第一个逗号；
- 取语句起点到首个逗号的文本，去掉首个声明符名字、初始化器、
  数组后缀，并去掉基础类型末尾的 `*` / `&`；
- 再把当前段逗号后、名字前的 `*` / `&` 补上；
- 结果仍需通过 `local_type_word()` 校验。

示例：

- `int a, b;` → `b` 类型 `int`；
- `struct Point p, *q;` → `q` 类型 `struct Point *`；
- `char *a, b;` → `b` 类型 `char`（首声明符的 `*` 不继承）。

`type_prefix()` 也被 `find_local_decl()` 用于 SYMBOL INFO；改动不能破坏
多行声明。测试里额外把光标放到 `completion_demo` 的 `return` 行，调用
`app_update_info()`，确认 `a.info_valid` 且 `a.info_type` 包含
`CompletionNode`。

- [ ] **Step 4: 运行测试确认通过**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: 局部变量测试全部通过。

- [ ] **Step 5: 提交**

```bash
git add src/app.c tests/ui_smoke.c
git commit -m "fix: include comma-separated local declarations"
```

---

### Task 4: 聚合类型键与成员提取

**Files:**
- Modify: `src/index.h`
- Modify: `src/index.c`
- Test: `tests/test_core.c`

**Interfaces:**
- Consumes: `Project`、`Symbol`、现有 `lex_source` / token 扫描。
- Produces:

```c
typedef struct {
    char *name;
    char *type;
    char *signature;
} ProjectMember;

typedef struct {
    ProjectMember *items;
    size_t count;
    size_t cap;
} ProjectMemberList;

void project_member_list_init(ProjectMemberList *l);
void project_member_list_free(ProjectMemberList *l);
bool project_type_aggregate_key(const char *type_text,
                                char *out, size_t out_size);
bool project_aggregate_members(Project *p, const char *key,
                               ProjectMemberList *out);
```

- `project_type_aggregate_key` 成功返回 true，失败返回 false。
- `project_aggregate_members` 向已初始化的 `out` 追加成员；返回是否找到
  聚合定义。调用方负责 `project_member_list_free()`。

- [ ] **Step 1: 写失败测试**

在 `tests/test_core.c` 增加 `test_type_keys()` 和 `test_aggregate_members(Project *p)`。

类型键断言：

```c
char key[256];
check(project_type_aggregate_key("const struct Point *", key, sizeof(key)) &&
      strcmp(key, "Point") == 0, "type key struct pointer");
check(project_type_aggregate_key("unsigned long", key, sizeof(key)) == false,
      "builtin type has no aggregate key");
check(project_type_aggregate_key("struct { int x; }", key, sizeof(key)) == false,
      "anonymous aggregate has no key");
check(project_type_aggregate_key("Point[4]", key, sizeof(key)) &&
      strcmp(key, "Point") == 0, "array type key");
```

成员断言：

```c
ProjectMemberList members;
project_member_list_init(&members);
check(project_aggregate_members(&p, "Point", &members),
      "Point members found");
check(members.count == 2, "Point has two members");
/* x/y type int, signature includes int */
project_member_list_free(&members);

project_member_list_init(&members);
check(project_aggregate_members(&p, "CompletionAnon", &members),
      "anonymous typedef members found");
check(members.count == 2, "anonymous typedef has alpha/beta");
project_member_list_free(&members);

project_member_list_init(&members);
check(project_aggregate_members(&p, "CompletionNode", &members),
      "CompletionNode members found");
check(members.count == 4, "CompletionNode has pos/next/alpha/beta");
/* pos type Point; next type struct CompletionNode *; alpha/beta int */
project_member_list_free(&members);

project_member_list_init(&members);
check(!project_aggregate_members(&p, "CompletionForward", &members),
      "forward declaration has no members");
project_member_list_free(&members);

project_member_list_init(&members);
check(!project_aggregate_members(&p, "NoSuchType", &members),
      "unknown aggregate has no members");
project_member_list_free(&members);
```

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make test_core && ./test_core tests/fixtures/demo`

Expected: 编译失败，新增符号未定义。

- [ ] **Step 3: 实现类型键归一化**

按 spec 6.1：

- 跳过限定符/存储符；
- 跳过 `struct`/`union`/`enum`/`class` 后取标签；
- `ns::Foo` 取 `Foo`；
- 忽略 `*`、`&`、`[`、`]`；
- 内置类型、匿名聚合、函数指针返回 false；
- 输出缓冲区不足时截断，不越界。

- [ ] **Step 4: 实现聚合成员提取**

按 spec 6.2：

- 在 `Project.syms` 中找同名 `SYM_STRUCT`/`SYM_UNION`/`SYM_CLASS`，
  或同名 `SYM_TYPEDEF`。
- 按 `Symbol.file_index` 读取源文件，用 `lex_source` 得到 token。
- 用 `line`/`col`/`name` 定位符号 token。
- 结构/联合/类：从名字 token 后找 `{`，匹配 `}`，收集第一层成员。
- typedef：
  - 找到包含 alias 的 `typedef` 声明；
  - 声明内有聚合体时收集该体；
  - 没有聚合体时把 `typedef struct Tag Alias;` / `typedef Other Alias;`
    递归解析，深度上限 8。
- 成员收集按 spec 6.2：
  - 顶层 `;` 切分；
  - 顶层逗号切分声明符；
  - 跳过嵌套聚合体；
  - 记录数据成员，跳过成员函数、静态数据成员、嵌套类型；
  - 多声明符继承基础类型；
  - 位域、数组、函数指针成员按 spec 处理。
- 多处同名定义合并，成员按名字去重，保留先出现的。
- 分配失败使用现有 `xmalloc` / `xrealloc` / `xstrdup` 风格。
- `project_member_list_free` 释放每个成员的 `name`、`type`、`signature`
  和数组。

- [ ] **Step 5: 运行测试确认通过**

Run: `make clean && make test_core && ./test_core tests/fixtures/demo`

Expected: 类型键和成员测试全部通过。

- [ ] **Step 6: 提交**

```bash
git add src/index.h src/index.c tests/test_core.c
git commit -m "feat: extract aggregate members for completion"
```

---

### Task 5: 补全状态与普通候选

**Files:**
- Modify: `src/app.h`
- Modify: `src/app.c`
- Test: `tests/ui_smoke.c`

**Interfaces:**
- Consumes: `syntax_*` 访问器、`sym_name_index`、`LocalVar`、`Project.syms`。
- Produces:

```c
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
```

`App` 增加 spec 7.3 中的补全字段，以及：

```c
Symbol **sym_name_index;
size_t sym_name_count;
```

对外函数（放在 `app.h`）：

```c
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
```

`app_completion_move()` 移动选中项并调用 `app_completion_fix()` 修正
滚动；Up/Down 用 ±1，PgUp/PgDn 用可视行数，Home/End 直接设置索引。
`app_completion_fix()` 负责把 `comp_sel` 夹到 `[0, comp_count)`，并让
`comp_scroll` 保证选中项在 `comp_popup_rows` 内。

`App` 的点击字段明确为：

```c
long comp_click_time;
int comp_click_index;
int comp_click_x;
int comp_click_y;
```

本任务先实现普通模式；成员模式返回空候选。

- [ ] **Step 1: 写失败测试**

在 `tests/ui_smoke.c` 增加补全测试函数，放在现有测试之后。辅助函数：

```c
static void move_after_anchor(App *a, const char *anchor)
{
    Buffer *b = app_buffer(a);
    BufMatch m;
    check(buf_find(b, anchor, 0, true, false, &m), anchor);
    if (!m.pos)
        return;
    b->cursor = (int)(m.pos + strlen(anchor));
    b->goal_col = buf_visual_col(b, (size_t)b->cursor);
    buf_insert_at(b, (size_t)b->cursor, "\n", 1);
    b->cursor++;
    b->goal_col = 0;
    app_update_info(a);
}
```

`move_to_completion_body(&a)` 使用锚点
`"node.pos.x = result + other + local_p.x + local_q->x;"`，在函数体内插入
一个新行并把光标放在新行行首；不要直接把光标放到声明中间再输入。

测试：

```c
move_to_completion_body(&a);
type_text(&a, "res");
check(a.comp_active, "completion opens for res");
check(a.comp_count >= 1, "completion has candidates");
check(a.comp_items[0].group == COMPLETE_GROUP_LOCAL &&
      strcmp(a.comp_items[0].name, "result") == 0,
      "local result comes first");
key(&a, KEY_TAB, NULL);
check(!a.comp_active, "tab closes completion");
check(strstr(app_buffer(&a)->data, "result") != NULL,
      "tab inserts result");
```

继续覆盖：

- `oth` → `other`（依赖 Task 3）；
- `hel` → 只有 `helper`，列表仍活动；Enter 接受；
- `GRE` → `GREETING`，Tab 接受；
- `completion_` → 包含 `completion_demo` 函数和全局变量；
- `po` → 不包含 `Point`（大小写敏感），列表不活动；
- 注释/字符串：在 `// re` 或 `"re"` 后输入，列表不活动；
- 输入完整 `helper` 后列表自动收起；
- 光标在词中间接受：把光标放到已有 `result` 的 `res|ult` 中间，
  调用 `app_completion_update(&a)`，选中 `result` 后 `app_completion_accept(&a)`，
  缓冲区仍只有一个 `result`，不能出现 `resultult`。

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: `a.comp_active` 等字段不存在或测试失败。

- [ ] **Step 3: 实现状态、名字索引和上下文**

- 在 `app.h` 增加 `CompleteItem`、组枚举、App 字段和函数声明。
- `app_rebuild_globals()` 中：
  - 释放旧 `sym_name_index`；
  - 分配 `project.sym_count` 个 `Symbol *`；
  - 填入 `project.syms`；
  - 用比较 `Symbol->name` 的 `qsort` 排序；
  - 清空补全状态和成员缓存。
- 实现 `app_completion_context()`：
  - 只允许编辑器焦点、有 buffer、无选择；
  - 拒绝注释/字符串/预处理行（`syntax_line_spans`）；
  - 读取光标前标识符前缀，记录 `word_start`、`word_end`、
    `prefix_len`、行号、buffer 索引；`comp_prefix` 最多存 255 字节，
    但 `word_start` / `word_end` 仍覆盖完整标识符；
  - 检测 `.` / `->` 成员模式（允许中间空格/Tab），本任务只置标志，
    候选仍为空；
  - 前缀为空且非成员模式时关闭。
- 实现普通候选收集：
  - 局部变量/参数按行号过滤、同名取最近声明；
  - 在 `sym_name_index` 上二分查找前缀下界并遍历匹配项；
  - 按 kind 映射到组；
  - 关键字/内置类型从 `syntax_*` 遍历；
  - 按“名字、组号”排序，相邻同名保留组号最小，再按“组号、名字”
    排序；
  - `detail`：局部用 `LocalVar.type`；全局/函数/宏用 `Symbol.signature`；
    类型用 kind 文本；关键字 `"keyword"`；内置类型 `"type"`。
- 实现 `app_completion_close()` / `app_completion_reset()` /
  `app_completion_update()` / `app_completion_accept()` / 抑制逻辑。
- 在 `app_handle_editor_key()` 的 `KEY_CHAR`、`KEY_BACKSPACE`、
  `KEY_DELETE` 分支后调用 `app_completion_update()`。
- 在以下入口调用 `app_completion_close()`：
  `app_prompt_begin()`、`app_menu_open()`、`app_chooser_open()`、
  `app_open_file_index()`、`app_open_path()`、`app_reload_project()`、
  `app_request_quit()`、打开帮助/目录选择器/项目选择器/右键菜单、
  撤销/重做/粘贴/格式化、开始文本选择、焦点切换。

- [ ] **Step 4: 实现接受和抑制**

- 接受时删除 `[cursor, word_end)` 旧后缀，插入 `candidate + prefix_len`；
  若候选不以 prefix 开头，替换 `[word_start, word_end)`。
- 更新 cursor、goal_col、脏状态、可视区域。
- 关闭列表，设置当前词抑制。
- 抑制判断：
  - 同 buffer、同 line、同 word_start；
  - 当前词与抑制词存在前缀关系；
  - 空成员前缀单独保留抑制。
- `app_completion_update()` 在抑制有效时不打开列表。

- [ ] **Step 5: 运行测试确认通过**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: 普通补全、接受、Esc、大小写、注释/字符串测试通过。

- [ ] **Step 6: 提交**

```bash
git add src/app.h src/app.c tests/ui_smoke.c
git commit -m "feat: add identifier completion engine"
```

---

### Task 6: 键盘拦截与自动选择框协调

**Files:**
- Modify: `src/app.c`
- Test: `tests/ui_smoke.c`

**Interfaces:**
- Consumes: Task 5 的 `app_completion_*` 和 `CompleteItem`。
- Produces: 弹出框活动时的 Up/Down/PgUp/PgDn/Home/End/Tab/Enter/Esc
  行为；自动定义选择框让路。

- [ ] **Step 1: 写失败测试**

在补全测试中增加：

- 输入 `co` 后 `comp_count >= 2`，Down 使 `a.comp_sel == 1` 且编辑器
  光标不移动；记录 `comp_items[1].name`，Tab 后缓冲区包含该名字；
- PgDn/Home/End 改变选中项；
- Esc 关闭并抑制；继续输入同一词不弹；空格后恢复；
- 输入完整 `Point` 后 `!a.chooser_active`；
- 活动状态输入 `Poin` 时 `!a.chooser_active`；
- Tab/Enter 接受后不打开选择框；
- 用户主动按 F12 时仍按原逻辑打开选择框或跳转；
- 弹出框活动时按 `Ctrl+F` / `Alt+M` 打开提示框或菜单，列表关闭，
  原功能正常。

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: 上下键移动了编辑器光标而不是 `comp_sel`，或 `Point` 弹出
`chooser_active`。

- [ ] **Step 3: 实现按键拦截**

在 `app_handle_key()` 的模态框检查之后、`KEY_MOUSE` 分发之前：

```c
if (a->comp_active) {
    if (app_completion_handle_key(a, ev))
        return;
}
```

`app_completion_handle_key()`：

- Up/Down/PgUp/PgDn/Home/End：移动选中项并返回 true；
- Tab/Enter：接受并返回 true；
- Esc：关闭并抑制，返回 true；
- Left/Right：关闭，返回 false；
- KEY_CHAR/BACKSPACE/DELETE：返回 false，让编辑器处理后更新；
- 其他键：关闭，返回 false。

- [ ] **Step 4: 协调自动选择框**

- 实现 `app_completion_suppresses_word(App *a, const char *word)`：
  - 列表活动且当前词匹配时返回 true；
  - 当前词处于抑制状态时返回 true。
- `app_update_info()` 中 `choice.count > 1` 时：
  - 如果 `app_completion_suppresses_word(a, word)`，取
    `choice.items[0]` 作为 `best`，然后 `intlist_free(&choice)`，
    不打开 chooser；
  - 否则保持原行为。
- `app_show_context()` 的多候选分支同样处理：让路时取第一个候选，
  释放 `choice`，继续走 `app_show_context_for()`。
- 输入已等于唯一候选、列表自动收起时，设置当前词抑制。
- 用户主动 F12/Ctrl+P/点击符号仍然打开选择框。

- [ ] **Step 5: 运行测试确认通过**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: 键盘和自动选择框测试通过，原有 chooser 测试不回归。

- [ ] **Step 6: 提交**

```bash
git add src/app.c tests/ui_smoke.c
git commit -m "feat: handle completion keys and defer auto chooser"
```

---

### Task 7: 浮层渲染、几何与提示行

**Files:**
- Modify: `src/render.c`
- Modify: `src/app.h`（如几何函数需要声明）
- Test: `tests/ui_smoke.c`

**Interfaces:**
- Consumes: `App.comp_*`、`a->cursor_x/y`、`CompleteItem`。
- Produces:
  - `bool app_completion_geometry(App *a, int *x, int *y, int *w, int *h,
    int *first, int *rows)`
  - `static void draw_completion(App *a, Str *out)`

- [ ] **Step 1: 写失败测试**

在浮层活动时调用 `app_render(&a)`，检查：

```c
check(a.comp_popup_w > 0 && a.comp_popup_h > 0,
      "completion popup has geometry");
check(a.comp_popup_y >= a.content_top, "popup inside content");
check(a.comp_popup_y + a.comp_popup_h <= a.content_top + a.content_height,
      "popup avoids hint/status");
check(a.comp_popup_rows >= 1, "popup has visible rows");
```

用 `a.force_rows = 12` 或更小的高度触发大量候选，检查
`a.comp_count` 不因可视行数而减少。

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: `comp_popup_w` 为 0。

- [ ] **Step 3: 实现几何计算**

`app_completion_geometry()`：

- 使用 `a->cursor_x` / `a->cursor_y`；
- 优先画在光标下一行；底部不足时翻到上方；
- 宽度 = 名字列 + 2 + 详情列 + 左右边框，夹在 `cols - 2`；
- 高度 = 候选数 + 2 边框，受内容区高度限制；
- 选中项滚动到可视区；
- 写回 `comp_popup_x/y/w/h/first/rows`；
- 两侧都放不下时返回 false。

- [ ] **Step 4: 实现绘制**

在 `render.c` 新增 `draw_completion()`：

- 只绘制可视行；
- 边框 `border`，背景 `panel_alt`，普通文字 `fg`，详情 `dim`，
  选中行 `accent_bg` / `selection_fg`；
- 名字列和详情列用 `utf8_text_width` / `draw_text`；
- 详情过长截断，不换行；
- 不覆盖光标行。

在 `app_render()` 中，`draw_content()` 之后、`draw_hint()` 之前调用。
`draw_hint()` 增加：

```
COMPLETION  Up/Down select  PgUp/PgDn  Tab/Enter accept  Esc close
```

- [ ] **Step 5: 运行测试确认通过**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: 几何测试通过，渲染无崩溃。

- [ ] **Step 6: 提交**

```bash
git add src/render.c src/app.h tests/ui_smoke.c
git commit -m "feat: render completion popup near cursor"
```

---

### Task 8: 成员补全与链式解析

**Files:**
- Modify: `src/app.h`
- Modify: `src/app.c`
- Test: `tests/ui_smoke.c`

**Interfaces:**
- Consumes: `project_type_aggregate_key()`、`project_aggregate_members()`、
  Task 5 的候选/状态机。
- Produces:
  - 成员模式候选；
  - `app_completion_chain_type()`；
  - 成员缓存 `CompleteMemberCache`。

- [ ] **Step 1: 写失败测试**

在 `main.c` 的 `main()` 里新起一行输入 `p.`（用 Task 5 的
`move_after_anchor()`，锚点用
`"util_global++; return add(p.x, p.y);"`）：

```c
move_after_anchor(&a, "util_global++; return add(p.x, p.y);");
type_text(&a, "p.");
check(a.comp_active && a.comp_member, "p. opens member list");
check(a.comp_count == 2, "Point has x and y");
```

继续：

- `completion_origin.`、`completion_world.`、`completion_ptr->` → x/y；
- `node.` → 包含 pos/next/alpha/beta；
- `node.pos.` → x/y；
- `node.next->pos.` → x/y；
- `local_q->` → x/y（局部多声明符指针）；
- `unknown_value.` → 不弹；
- `arr[0].` / `(*p).` → 不弹。

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: `p.` 没有候选。

- [ ] **Step 3: 实现链式类型解析**

- `app_completion_chain_type()`：
  - 从成员模式的 `.` / `->` 前向前读标识符；
  - 跳过空格/Tab；
  - 收集 `a.b->c` 片段序列；
  - 最左片段先查局部变量/参数，再查全局变量；
  - 全局类型从 `Symbol.signature` 提取变量名前文本；
  - 后续片段用 `project_type_aggregate_key()` +
    `project_aggregate_members()` 逐级解析；
  - 解析失败返回 false。
- `app_completion_members()`：
  - 用类型键查 `member_cache`；
  - 未命中则调用 `project_aggregate_members()` 并缓存；
  - 项目重载/F5 时清空缓存。
- 成员模式候选：
  - 只收集数据成员；
  - 按成员名排序；
  - `detail` 用 `ProjectMember.type` 或 `signature`；
  - 前缀过滤和接受复用 Task 5 逻辑。

- [ ] **Step 4: 运行测试确认通过**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: `p.`、全局结构体变量、链式成员测试通过。

- [ ] **Step 5: 提交**

```bash
git add src/app.h src/app.c tests/ui_smoke.c
git commit -m "feat: complete struct members and chained access"
```

---

### Task 9: 鼠标选择与双击接受

**Files:**
- Modify: `src/app.c`
- Modify: `src/app.h`（如需新增点击状态字段）
- Test: `tests/ui_smoke.c`

**Interfaces:**
- Consumes: `app_completion_geometry()`、`comp_popup_*`、`comp_sel`。
- Produces: 单击高亮、双击接受、滚轮滚动、点击外部关闭。

- [ ] **Step 1: 写失败测试**

浮层活动并 `app_render()` 后：

```c
int row = a.comp_popup_first;
int x = a.comp_popup_x + 2;
char first[256];
snprintf(first, sizeof(first), "%s", a.comp_items[0].name);
app_handle_mouse_event(&a, x, row, 0, false);
check(a.comp_sel == 0, "single click selects first item");
check(strstr(app_buffer(&a)->data, first) == NULL,
      "single click does not insert");
app_handle_mouse_event(&a, x, row, 0, false);
check(!a.comp_active, "second click closes popup");
check(strstr(app_buffer(&a)->data, first) != NULL,
      "second click inserts first item");
```

再覆盖：

- 点击另一行只改变选中；
- 滚轮 64/65 改变 `comp_scroll` / `comp_sel`，选中项仍可见；
- 点击浮层外关闭列表，随后正常鼠标逻辑仍执行；
- 双击不同项不插入。

- [ ] **Step 2: 运行测试确认失败**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: 点击没有改变 `comp_sel` 或直接插入。

- [ ] **Step 3: 实现鼠标命中与双击**

- 在 `app_handle_key()` 的 `KEY_MOUSE` 分支先调用
  `app_completion_handle_mouse()`。
- 命中判定使用最近一次渲染的 `comp_popup_x/y/w/h` 和
  `comp_popup_first/rows`。
- 单击命中：设置 `comp_sel`，记录 `comp_click_time` /
  `comp_click_index` / `comp_click_x` / `comp_click_y`，不插入。
- 同一索引 1 秒内、坐标相差不超过 2 列/行再次单击：接受。
- 滚轮：移动选中项 3 行并保持可见。
- 点击外部：关闭列表，返回 false 让原有鼠标逻辑继续。
- 鼠标事件处理期间不改变编辑器焦点。

- [ ] **Step 4: 运行测试确认通过**

Run: `make clean && make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`

Expected: 鼠标测试通过。

- [ ] **Step 5: 提交**

```bash
git add src/app.c src/app.h tests/ui_smoke.c
git commit -m "feat: support mouse selection in completion popup"
```

---

### Task 10: 文档、全量验证与收尾

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `docs/si-c-architecture.html`（由脚本生成）
- Test: 全量

**Interfaces:**
- Consumes: 前九个任务。
- Produces: 用户文档、测试计数、最终验证。

- [ ] **Step 1: 更新 README**

在“它到底做了什么”增加：

- 逐字符变量/函数/宏/类型/关键字补全；
- `变量.` / `变量->` 结构体成员补全，支持链式；
- Tab/Enter 接受，Esc 抑制，单击高亮、双击接受、滚轮滚动。

- [ ] **Step 2: 更新 AGENTS.md**

- 运行时主流程补充补全更新/渲染/鼠标分支；
- 测试计数改为实际值；
- 记录 `.si-index` 格式未变、旧缓存需 F5 才能看到新增全局变量。

- [ ] **Step 3: 生成架构文档**

Run: `python3 docs/build_doc.py`

Expected: `docs/si-c-architecture.html` 更新，无脚本错误。

- [ ] **Step 4: 全量构建和测试**

Run: `make clean && make -j && make test`

Expected: `test_core`、`test_keys`、`ui_smoke` 全部通过，无新增警告。

- [ ] **Step 5: 手动验证**

Run: `./si tests/fixtures/demo`

手动检查：

- `main()` 里新起一行输入 `p.`，出现 `x`、`y`；
- `completion_origin.` / `completion_world.` / `completion_ptr->` 出现
  `x`、`y`；
- `completion_demo()` 里输入 `res`，出现 `result`；
- Esc 后继续输入同一词不再弹；
- 鼠标单击只高亮，双击接受，滚轮滚动；
- 注释/字符串里不弹。

Run: `./si examples/demo`

手动检查 `v->` 出现 `data`、`len`、`cap`。

- [ ] **Step 6: 提交**

```bash
git add README.md AGENTS.md docs/si-c-architecture.html
git commit -m "docs: document variable completion"
```

---

## Self-Review

- **Spec coverage:** 逐项对照 spec 第 2–13 节；普通候选、成员/链式、键盘、
  鼠标、Esc、自动选择框协调、全局变量解析、渲染、测试和文档都有对应任务。
- **Step scan:** 每个步骤都是单一动作，测试步骤给出断言，代码步骤给出
  签名/行为，验证步骤给出命令和预期。
- **Type consistency:** `syntax_*`、`project_*`、`CompleteItem`、`comp_*`、
  `app_completion_*` 在各任务中保持同名同签名。
- **Review Focus:** 中间标识符接受（Task 5）、字面量抑制（Task 5）、
  同名自动选择框（Task 6）、小屏不截断（Task 7）、鼠标坐标/双击
  （Task 9）都有测试。
- **Proportion:** 计划长度与 spec 相当，代码块只保留接口、测试断言和
  fixture 源码，不转录实现。
