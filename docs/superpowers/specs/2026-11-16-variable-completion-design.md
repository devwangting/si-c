# 变量补全功能设计

- 日期：2026-11-16
- 状态：已确认，待实现
- 影响范围：`src/app.c`、`src/app.h`、`src/render.c`、`src/index.c`、
  `src/index.h`、`src/syntax.c`、`src/syntax.h`、`tests/`、`README.md`
- 不改变 `.si-index` 的磁盘格式，不需要提升 `SI_CACHE_VERSION`

## 1. 为什么做

`si-c` 目前只有项目符号搜索（`Ctrl+P`），没有“边写边补全”。写代码时，
局部变量、参数、全局变量、函数名、宏名、类型名和结构体成员都要靠记忆或
来回跳转。这个功能把当前上下文里可见的名字直接放到光标下方，减少查找和
拼写错误。

## 2. 目标

1. 在编辑器里输入标识符的任意前缀时，自动弹出候选列表。
2. 候选包括：
   - 当前函数里已声明且在当前行可见的局部变量和参数；
   - 全局变量和枚举常量（包括 `struct Point origin;`、
     `Point world = {1, 2};`、`struct Point *ptr;` 这类常见形式）；
   - 函数和函数原型；
   - 宏；
   - 结构体/联合/枚举/类/typedef 名称；
   - C/C++ 关键字；
   - 内置基础类型名（`int`、`size_t` 等）。
3. 输入 `变量.` 或 `变量->` 后只列出该结构体的成员。
4. 支持链式成员访问：`a.b.c`、`a.b->c`、`a->b.c` 等。
5. 候选多时用键盘或鼠标选择；候选不截断，列表放不下就滚动。
6. 按 Esc 放弃当前这个词的自动匹配，直到离开当前标识符。
7. 补全不阻塞输入；在大型项目上逐字符输入仍然可用。

## 3. 非目标

- 不做编译器级语义分析，不展开宏，不做模板、继承、重载解析。
- 不补全代码片段，不自动插入 `()`、`{}` 或分号。
- 不在注释、字符串、预处理行、提示框、菜单里触发。
- 不自动接受唯一候选；即使只剩一个候选，也等 Tab/Enter 或双击。
- 成员补全只列数据成员；成员函数、静态数据成员和嵌套类型暂不进入
  `obj.` / `obj->` 的候选（用户要的是“所有变量”）。
- 不支持 `this->` 形式的 C++ 隐式当前对象解析。
- 不支持需要先计算表达式的成员访问：
  - `arr[i].x`
  - `(*p).x`
  - `get_user().name`
  - `a.b[0].c`
  这些形式如果无法解析出类型，就不弹列表；不为了猜测而弹错列表。
- 不改变 `.si-index` 的格式，不增加新的持久化缓存。
- 不引入第三方库、不引入新构建系统。

## 4. 用户已确认的行为

- 输入 `re` 时，`return`、`result` 等候选都要出现。
- 候选只剩一个时，也保留列表，等 Tab 或 Enter 确认。
- 函数名和宏名加入候选。
- 支持链式结构体成员补全。
- 候选数量不设上限；列表可视区域放不下时滚动，候选本身不丢弃。
- 鼠标：单击只高亮选中，双击同一项才接受；滚轮滚动列表。
- 键盘：上下键移动选择，Tab/Enter 接受。
- Esc：关闭列表，并让当前这次输入不再自动匹配；空格、标点、换行、
  光标移走或开始新词后恢复。
- 排序：局部变量/参数 → 全局变量 → 函数/宏 → 类型 → 关键字；同类按名字。
- 匹配区分大小写，使用前缀匹配。
- 输入已经等于唯一候选且后面没有可补字符时，自动收起列表。
- 类型解析不出来时不弹列表。

## 5. 交互规格

### 5.1 触发上下文

- 只在 `FOCUS_EDITOR`、有活动缓冲区、无选择、无其他模态框时触发。
- 每次 `KEY_CHAR`、`KEY_BACKSPACE`、`KEY_DELETE` 后重新计算。
- 当前光标前必须是标识符片段；标识符字符限定为 ASCII 字母、数字、下划线，
  且片段首字符必须是字母或下划线。
- 用 `Buffer.syn` 和 `syntax_line_spans()` 判断光标是否位于
  `HL_COMMENT`、`HL_STRING` 或 `HL_PREPROC` 片段内；是则不触发。
- 普通模式：片段为空时不弹。
- 成员模式：片段前的非空白字符是 `.`，或是 `->`，进入成员模式。
  允许 `.` / `->` 与标识符之间有空格或 Tab。
- 成员模式下片段可以为空；此时列出接收者的全部成员。
- 如果前缀已经等于唯一候选，且光标后没有同一标识符的后续字符，
  关闭列表；如果光标在词中间，仍可显示。

### 5.2 候选来源与排序

普通补全按以下组排序，组内按名字的字节序升序：

1. 当前函数局部变量和参数（只取 `LocalVar.line <= 当前行 <= end_line`，
   同名取声明行最接近当前行的一个）。
2. 全局变量、枚举常量（`SYM_VARIABLE`、`SYM_ENUMVAL`）。
3. 函数和原型（`SYM_FUNCTION`、`SYM_PROTOTYPE`）。
4. 宏（`SYM_MACRO`）。
5. 类型（`SYM_STRUCT`、`SYM_UNION`、`SYM_ENUM`、`SYM_CLASS`、
   `SYM_TYPEDEF`）以及 `syntax.c` 里的内置类型名。
6. C/C++ 关键字（复用 `syntax.c` 的列表，不在补全模块里另抄一份）。

成员补全只有一组：该结构体/联合/类的成员，按成员名升序。

- 同名候选合并显示，保留优先级最高的来源（局部变量最高）。
- 函数原型和函数定义同名时只显示一条；优先显示函数定义。
- `SYM_MEMBER` 和 `SYM_NAMESPACE` 不进入普通候选。
- 右侧详情列：
  - 局部变量/参数：类型文本；
  - 全局变量：签名（包含类型和名字）；
  - 函数/原型：签名；
  - 宏：签名；
  - 类型：`struct` / `union` / `enum` / `class` / `typedef`；
  - 关键字：`keyword`；
  - 内置类型：`type`；
  - 成员：成员类型文本。
- 详情文本过长时截断，不换行。

### 5.3 匹配

- 使用区分大小写的前缀匹配：`re` 匹配 `result`、`return`，
  不匹配 `Result` 或 `create`。
- 继续输入缩小范围；退格恢复范围。
- 前缀长度上限 255 字节；超过上限时只匹配前 255 字节，但接受补全时
  仍使用完整词范围。
- 不排序“精确匹配优先”；同一组内仍然按名字。`return` 会排在
  `return_value` 前，因为短名先比较结束。

### 5.4 弹出框

- 位置：光标下方一行；底部空间不足时翻到光标上方。
- 水平位置优先与光标对齐，超出屏幕边缘时向内收。
- 宽度：最长候选名 + 2 列间隔 + 最长详情，再加左右边框；
  最大不超过 `cols - 2`，详情按剩余宽度截断。
- 高度：候选数 + 上下边框；受可用屏幕高度限制。
  **候选总数不截断**，只限制可视行数，超出部分滚动。
- 列表没有标题行；只画边框和候选行，尽量少占代码空间。
- 选中行使用主题的 `accent_bg` / `selection_fg`；普通行使用 `fg`，
  详情使用 `dim`，边框使用 `border`，背景使用 `panel_alt`。
- 弹出框绘制在内容区之内，不能覆盖底部提示行和状态行。
- 光标保持可见；弹出框不覆盖当前光标所在行。
- 活动时帮助提示行显示：
  `COMPLETION  Up/Down select  PgUp/PgDn  Tab/Enter accept  Esc close`。
- 滚动时保证选中项始终在可视区域内。

### 5.5 键盘

弹出框活动时：

| 按键 | 行为 |
| --- | --- |
| Up / Down | 选中项上/下移动一行 |
| PgUp / PgDn | 上/下移动一页 |
| Home / End | 跳到第一/最后一项 |
| Tab / Enter | 接受当前选中项 |
| Esc | 关闭并抑制当前词 |
| Left / Right | 关闭列表，交给编辑器移动光标 |
| 字母、数字、下划线 | 交给编辑器插入，然后重新过滤 |
| Backspace / Delete | 交给编辑器删除，然后重新过滤 |
| 其他按键（Ctrl/F 键等） | 关闭列表，交给原有全局逻辑 |

弹出框不活动时，所有按键保持原有行为。

### 5.6 鼠标

- 单击候选项：只移动选中项，不插入。
- 在 1 秒内再次单击同一项（同一候选索引）：接受补全。
- 双击不同项：只选中新项，不插入。
- 滚轮在弹出框上：滚动列表（每次 3 行），选中项跟随移动并保持可见。
- 单击弹出框外：关闭列表，然后让原有鼠标逻辑继续处理这次点击。
- 鼠标命中区域使用最近一次渲染保存的弹出框几何信息。

### 5.7 接受补全

- 接受时替换整个当前标识符范围 `[word_start, word_end)`，而不只是光标前
  的前缀；这样光标在词中间时不会产生重复后缀。
- 如果候选以当前前缀开头（前缀匹配保证如此），只删除光标后的旧后缀并
  插入候选剩余部分，保留用户已经输入的前缀。
- 成员模式同样处理：替换 `.` / `->` 后的整个成员标识符。
- 函数名、宏名、类型名都只插入名字，不自动加括号。
- 接受后关闭列表，并把刚接受的词设为“已接受”，当前词不再重新弹出，
  直到离开这个词。
- 修改进入当前撤销组；用户按 Ctrl+Z 能撤销补全和这次输入。

### 5.8 Esc 抑制

- 按 Esc 时记录：
  - 缓冲区索引；
  - 行号；
  - 当前词起始字节偏移；
  - 当前词文本；
  - 是否处于“刚输入 `.` / `->`、前缀为空”的成员上下文。
- 满足以下条件时继续抑制：
  - 同一缓冲区、同一行、同一词起始位置；
  - 当前词是抑制词的“前缀扩展”或“退格缩减”，即两词存在前缀关系。
- 抑制期间继续输入或退格都不弹列表。
- 当前词被删空后：
  - 普通上下文清除抑制；
  - 成员空前缀上下文继续抑制，直到光标离开这个成员位置。
- 光标换行、换词起始位置、切换文件或项目时清除抑制。
- 接受补全也设置同样的抑制，避免刚补完又立刻弹出更长的同名前缀候选。
- 如果当前没有弹出框，Esc 仍对当前词建立抑制；不改变 Esc 原有的焦点行为。

### 5.9 与自动“选择定义”框的协调

现有 `app_update_info()` 和 `app_show_context()` 在同名符号有多个定义时会
自动打开定义选择框。补全活动期间、以及刚接受补全后的当前词上，这个自动
弹窗必须让路，否则输入 `Point` 或接受重名成员后会立刻被模态框打断。

规则：

- 补全弹出框活动时，`app_update_info()` / `app_show_context()` 不打开
  定义选择框；如果 `app_pick_symbol()` 返回多个候选，取第一个候选继续
  刷新信息/CALL FLOW，或保持原面板内容。
- 输入已经等于唯一候选、列表自动收起时，也把当前词设为抑制状态；
  这样自动选择框不会紧接着抢焦点。
- 接受补全后，当前词处于抑制状态；在抑制过期前同样不自动打开定义
  选择框。
- 用户主动按 `Ctrl+P`、`F12`、`Shift+F12` 或点击符号时，仍然按原逻辑
  打开选择框，不受此限制。
- 抑制失效（换词、换行、光标移走）后，自动选择框恢复正常。

### 5.10 关闭时机

以下情况立即关闭弹出框（不清除抑制，除非明确说明）：

- 焦点离开编辑器；
- 打开菜单、帮助、目录选择器、项目选择器、符号选择器、输入提示、
  右键菜单、关系表提示；
- 保存/重载项目（F5）、切换文件、打开新文件；
- 撤销、重做、粘贴、格式化；
- 建立文本选择或开始鼠标拖选；
- 终端尺寸变化（下一次输入会重新计算）；
- 当前上下文不再产生候选。

## 6. 类型与成员解析

### 6.1 类型文本归一化

新增 `project_type_aggregate_key(const char *type_text, char *out, size_t out_size)`：

- 去掉存储/限定符：`const`、`volatile`、`static`、`extern`、`register`、
  `auto`、`restrict`、`inline`、`_Atomic`、`mutable`、`typename`。
- 去掉 `struct`、`union`、`enum`、`class` 关键字，但记住后面的标签名。
- 忽略 `*`、`&`、`[`、`]`、`::` 的限定部分；`ns::Foo` 取 `Foo`。
- 内置类型（`int`、`char`、`unsigned long`、`void`、`size_t` 等）返回 false。
- 匿名聚合 `struct { ... }` 返回 false。
- 函数指针类型返回 false。
- 成功时输出成员查找键，例如：
  - `const struct Point *` → `Point`
  - `Point *` → `Point`
  - `struct ns::Node` → `Node`
  - `unsigned long` → 失败

### 6.2 聚合成员查询

新增 `project_aggregate_members(Project *p, const char *key, ProjectMemberList *out)`。

查找顺序：

1. 在项目符号中找同名 `SYM_STRUCT` / `SYM_UNION` / `SYM_CLASS`：
   - 从其名字 token 后找 `{`，匹配 `}`，收集大括号内第一层的成员声明。
2. 找同名 `SYM_TYPEDEF`：
   - 定位 `typedef` 声明；
   - 如果声明里有 `struct/union/class { ... }`，收集该大括号内的成员；
   - 如果是 `typedef struct Tag Alias;`，递归解析 `Tag`；
   - 如果是 `typedef Other Alias;`，递归解析 `Other`；
   - 递归深度上限 8，防止别名环。
3. 多处定义同名聚合时合并成员，按成员名去重，先出现的保留。
4. 只有前置声明、没有定义时返回 false。
5. 成员列表为空但聚合定义存在时返回 true（C++ 空类）。

成员收集规则：

- 只收集聚合体大括号内第一层的声明。
- `int x, y;` 两个成员都要收集，`y` 继承 `x` 的基础类型。
- `struct Point *next;` 记录名字 `next`，类型 `struct Point *`。
- `char name[32];` 记录名字 `name`，类型 `char`；数组下标成员访问
  （`name[i].`）不在本功能范围内。
- `void (*cb)(int);` 记录名字 `cb`，类型无法用于链式解析。
- 位域 `unsigned flags : 3;` 记录名字 `flags`，类型 `unsigned`。
- 嵌套匿名聚合：
  - 有名字的成员（`struct { ... } inner;`）收集 `inner`，类型留空；
  - 没有名字的匿名成员暂不展开到外层。
- C++ 访问修饰符 `public:` / `private:`、继承列表不产生成员。
- 只收集数据成员。成员函数（`void f();` / `void f() {}`）、静态数据成员
  （`static int count;`）和嵌套类型名（`struct Inner { ... };`）不进入
  `obj.` / `obj->` 的候选。
- 成员签名用于显示，成员类型用于继续链式解析。

### 6.3 链式解析

从光标前的 `.` / `->` 向前解析接收者：

1. 跳过空格/Tab。
2. 读取一个标识符片段。
3. 如果片段前还有 `.` 或 `->`，继续向前读取。
4. 得到从左到右的片段序列，例如 `a.b->c` → `a`、`b`、`c`。

然后：

1. 用最左片段查当前函数的局部变量/参数；找不到再查全局变量。
2. 得到基础类型文本。
3. 对后续每个片段：
   - 用当前类型查聚合成员；
   - 找到同名字段，取其类型作为新的当前类型；
   - 找不到则解析失败。
4. 最后一个片段的类型就是成员补全的接收者类型。
5. 解析失败不弹列表。

基础类型来源：

- 局部变量/参数：`LocalVar.type`。
- 全局变量：从 `Symbol.signature` 提取变量名前的类型文本。
- 多声明符全局变量（`int a, b;`）的每个符号签名都要以完整类型开头，
  例如 `int a`、`int b`；`struct Point p, *q;` 的 `q` 签名是
  `struct Point * q`。这样链式解析可以直接从签名提取类型，不再依赖
  同一行前一个变量。
- 多声明符局部变量（`int a, b;`）在 `app_rebuild_locals` 中补齐，
  让 `b` 也有正确的类型。
- `LocalVar.type` 当前最多 256 字节；类型文本超长时截断，截断后解析失败
  则不弹。

成员类型来源：

- `project_aggregate_members` 返回的 `ProjectMember.type`。
- 继续用 `project_type_aggregate_key` 归一化。

数组、下标、解引用括号、函数调用结果不参与链式解析；遇到这些形式直接
判定失败，不猜测。

### 6.4 全局变量解析补齐

现有 `parse_variables()` 会跳过以下顶层声明：

- 以 `struct` / `union` / `enum` / `class` 开头的声明；
- 语句中出现 `(` 或 `{` / `}` 的声明。

因此 `struct Point origin;`、`Point world = {1, 2};`、
`struct Point *ptr;` 这类常见全局变量根本不在 `Project.syms` 里，既不
会出现在补全候选，也不能作为 `origin.` 的接收者。本次一并修复：

- 继续只处理文件顶层（brace depth 0）的 `;` 结尾声明。
- 仍然跳过 `typedef`、`using`、`template`、`namespace`、`static_assert`。
- 以 `struct` / `union` / `enum` / `class` 开头的声明不再整体跳过：
  - 如果声明里带聚合体 `{ ... }`，跳过聚合体，只解析右花括号之后的
    声明符（例如 `struct Point { int x; } origin;`）；
  - 如果没有聚合体，按普通变量声明解析（例如 `struct Point origin;`）。
- `=` 之后的初始化器不参与声明符解析：
  - `Point world = {1, 2};` 解析出 `world`；
  - `int x = 0;`、`int y = foo();` 仍能解析出 `x` / `y`。
- 函数声明和函数定义继续交给 `parse_functions()`；函数指针变量
  （`int (*fp)(int);`）本次不新增支持。
- 顶层多声明符按逗号切分，并共用基础类型：
  - `int a, b;` 两个都是 `int`，签名分别为 `int a`、`int b`；
  - `struct Point p, *q;` 中 `p` 是 `struct Point`，`q` 是
    `struct Point *`，签名分别为 `struct Point p`、
    `struct Point * q`；
  - 数组 `int arr[10];` 记录 `arr`。
  每个符号的 `signature` 都必须以完整类型开头，供补全提取类型。
- 为支持“每个声明符都带完整类型签名”，`variable_segment()` 在非首个
  声明符时用基础类型拼接签名（新增 `add_symbol_raw()` 或等价 helper），
  不要只存 `b` 这种裸名字。
- 不改变 `Symbol` 布局和 `.si-index` 格式；旧缓存里没有新符号，
  需要 F5 重建后才能看到新增的全局变量。

`app_rebuild_locals()` 同步补齐局部多声明符，例如 `int a, b;` 里的 `b`
也要进入局部变量表和补全候选。

## 7. 实现结构

不新增 `.c` 文件，不修改 `Makefile`。

### 7.1 `src/syntax.h` / `src/syntax.c`

暴露现有静态列表给补全模块复用：

```c
size_t syntax_keyword_count(void);
const char *syntax_keyword_at(size_t index);
size_t syntax_type_count(void);
const char *syntax_type_at(size_t index);
```

### 7.2 `src/index.h` / `src/index.c`

新增：

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

实现要点：

- `project_aggregate_members` 按需读取聚合所在文件，用现有 `lex_source`
  词法分析，再用 token 层的小型扫描器收集成员。
- 扩展 `parse_variables()`：支持顶层 `struct/union/enum/class` 变量、
  带初始化器的变量和多声明符；详见 6.4。
- 不修改 `Symbol` 布局，不修改 `.si-index`。
- 读文件失败、找不到 token、括号不匹配时返回 false。
- 不把成员信息写回 `Project` 结构，运行时缓存放在 `App`。

### 7.3 `src/app.h` / `src/app.c`

`App` 增加：

```c
Symbol **sym_name_index;        /* 按名字排序的符号指针，运行时索引 */
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
int comp_popup_x, comp_popup_y, comp_popup_w, comp_popup_h;
int comp_popup_first, comp_popup_rows;
long comp_click_time;
int comp_click_index;
int comp_click_x;
int comp_click_y;
bool comp_suppress;
int comp_suppress_buf;
int comp_suppress_line;
int comp_suppress_start;
char comp_suppress_word[256];
bool comp_suppress_member;
CompleteMemberCache *member_cache;
size_t member_cache_count;
size_t member_cache_cap;
```

`CompleteItem`：

```c
typedef struct {
    const char *name;   /* 指向符号名、局部变量名、静态关键字等 */
    const char *detail; /* 指向类型/签名/静态文本 */
    unsigned char group;
} CompleteItem;
```

`CompleteMemberCache`：

```c
typedef struct {
    char *key;
    ProjectMemberList members;
    bool found;
} CompleteMemberCache;
```

`app.c` 新增 `/* --- Completion --- */` 分节，包含：

- `app_completion_close()`：清空活动状态，不释放缓存。
- `app_completion_reset()`：项目重载时清空缓存、名字索引相关状态、抑制状态。
- `app_completion_update()`：计算上下文、收集候选、过滤、排序、去重。
- `app_completion_handle_key()`：弹出框活动时的按键拦截。
- `app_completion_handle_mouse()`：弹出框活动时的鼠标拦截。
- `app_completion_accept()`：替换当前标识符。
- `app_completion_suppress()`：Esc/接受后的抑制。
- `app_completion_geometry()`：统一计算弹出框位置/尺寸，供 `render.c`
  和鼠标命中共用。
- `app_completion_members()`：带缓存的聚合成员查询。
- `app_completion_chain_type()`：链式接收者类型解析。
- `app_completion_context()`：前缀、词范围、是否成员模式、是否字面量。
- `app_completion_collect_*()`：局部变量、符号、关键字/类型、成员。

`app_rebuild_globals()` 中：

- 重新构建 `sym_name_index`：分配 `Symbol*` 数组，填入 `project.syms`，
  用 `strcmp(symbol->name)` 排序。
- 清空成员缓存和补全活动状态。
- 该函数已经在项目加载、F5、缓存加载后调用，名字索引生命周期与项目一致。

按键挂接：

- `app_handle_key()` 在模态框检查之后、`KEY_MOUSE` 分发之前调用
  `app_completion_handle_key()`。
- 返回 true 表示已消费；返回 false 时继续原有逻辑。
- `app_handle_mouse()` 或 `app_handle_key()` 的鼠标分支先调用
  `app_completion_handle_mouse()`；未命中则关闭列表并继续原有鼠标逻辑。
- `app_handle_editor_key()` 在处理完 `KEY_CHAR`、`KEY_BACKSPACE`、
  `KEY_DELETE` 后调用 `app_completion_update()`。
- 其他会导致编辑或模态切换的动作显式调用 `app_completion_close()`。
- 打开项目选择器、菜单、提示框、符号选择器、目录选择器、帮助、
  右键菜单时关闭列表。

候选收集：

- 普通模式：
  - 局部变量/参数：遍历 `a->locals`，按行号过滤，按名字排序去重。
  - 符号：在 `sym_name_index` 上二分查找前缀下界，向后遍历到不再匹配
    为止。按 kind 分到全局/函数/宏/类型组。
  - 关键字和内置类型：遍历 `syntax_keyword_at` / `syntax_type_at`。
- 成员模式：
  - 解析接收者类型；
  - 查成员缓存或 `project_aggregate_members`；
  - 按前缀过滤，按名字排序。
- 排序/去重：
  1. 先按名字、再按组号排序；
  2. 相邻同名只保留组号最小的一条；
  3. 再按组号、名字排序。
- `CompleteItem.name` / `.detail` 不拥有字符串；局部变量、符号、静态列表、
  成员缓存的生命周期都覆盖一次补全周期。

接受补全：

- 用当前的 `comp_word_start` / `comp_word_end` / `comp_prefix_len`。
- 如果候选以 `comp_prefix` 开头：
  - 删除 `[cursor, comp_word_end)` 的旧后缀；
  - 在光标处插入 `candidate + comp_prefix_len`。
- 否则删除整个 `[comp_word_start, comp_word_end)`，插入完整候选。
- 更新光标、目标列、可视区域、脏标记；撤销记录留在当前组。
- 关闭列表并设置抑制，抑制词为候选名。
- 不自动触发下一次补全。

名字索引内存：

- `app_rebuild_globals()` 先 `free(a->sym_name_index)`，再按
  `a->project.sym_count` 分配。
- 排序比较器只比较 `Symbol->name`，同名时比较 kind 保证稳定。
- 项目符号在加载后不再重排，指针数组安全。

### 7.4 `src/render.c`

新增 `/* --- Completion popup --- */` 分节：

- `draw_completion(App *a, Str *out)`：
  - 调用 `app_completion_geometry()` 获取位置/尺寸；
  - 画边框、候选行、详情列、选中高亮；
  - 只绘制可视行；
  - 把 `comp_popup_first`、`comp_popup_rows` 等几何信息写回 App，
    供下一次鼠标事件命中。
- `app_render()` 在 `draw_content()` 之后、`draw_hint()` / `draw_status()`
  之前调用 `draw_completion()`。
- `draw_hint()` 增加 `comp_active` 分支。
- 光标显示逻辑不需要隐藏光标；弹出框不覆盖光标行。
- 所有文本宽度用 `utf8_text_width()` / `draw_text()`，保证中文和宽字符
  不破坏边框。

### 7.5 初始化/清理

- `App` 零初始化后，`sym_name_index = NULL`、`comp_items = NULL`、
  `member_cache = NULL`，无需额外 init 调用。
- 退出时在 `ui_run()` 清理段释放：
  - `comp_items`；
  - `member_cache` 每项的 key 和 `ProjectMemberList`；
  - `sym_name_index`。
- `app_rebuild_globals()` 负责释放旧的运行时索引和缓存。

## 8. 性能

- 名字索引只在项目加载/F5 时构建一次，大小约为 `8 * sym_count` 字节；
  20 万符号约 1.6 MB。
- 每次输入用二分查找定位前缀范围，只遍历匹配项，不扫描整个项目。
- 每次输入最多排序/去重匹配项；候选人很多时仍只绘制可视行。
- 成员查询按类型键缓存，首次读取并词法分析源文件，之后只查缓存。
- 局部变量表已有 `locals_rev` 机制；补全更新前若发现过期，调用
  `app_rebuild_locals()`，主循环随后调用 `app_update_info()` 不会重复重建。
- 不在每次输入时读取磁盘，除非首次查询某个结构体成员。

## 9. 缓存与索引

- `.si-index` 格式不变，`SI_CACHE_VERSION` 不变。
- 名字索引、成员缓存、补全状态都是运行时数据，项目重载/F5 时重建。
- 结构体定义修改后，和现有符号索引一样需要 F5 才进入聚合成员查询；
  当前缓冲区里的局部变量类型仍然实时更新。
- 补全不写入 `.si-index`，批量模式不涉及补全。

## 10. 测试

### 10.1 `tests/test_core.c`

- `project_type_aggregate_key`：
  - `struct Point *` → `Point`
  - `const union Value` → `Value`
  - `unsigned long` → 失败
  - `struct { int x; }` → 失败
- `project_aggregate_members`：
  - fixture 里的 `Point` 返回 `x`、`y`，类型都是 `int`；
  - `typedef struct { ... } Alias;` 能返回成员；
  - 只有前置声明时返回 false；
  - 未知类型返回 false。
- 多声明符成员 `int x, y;` 两个都能查到。
- `parse_variables()` 回归：
  - `struct Point origin;`、`Point world = {1, 2};`、
    `struct Point *ptr;` 都产生 `SYM_VARIABLE`；
  - `int a, b;` 两个都能找到；
  - `struct Point p, *q;` 两个都能找到；
  - 函数声明/定义仍然只由 `parse_functions()` 产生，不重复计数；
  - fixture 原有的 `add` 原型+定义数量不变。

### 10.2 `tests/ui_smoke.c`

在 fixture 末尾追加测试用代码，保持现有 `main`（第 16 行）和
`global_counter`（第 23 行）行号不变：

```c
typedef struct CompletionNode {
    Point pos;
    struct CompletionNode *next;
    int alpha, beta;
    Point point_arr[2];
} CompletionNode;

struct Point completion_origin;
Point completion_world = {1, 2};
struct Point *completion_ptr;
struct Point completion_points[3];
static struct Point shared_point;

int completion_demo(void)
{
    int result = 0, other;
    Point local_p, *local_q;
    Point local_arr[2];
    CompletionNode node;
#define COMPLETION_LOCAL_MACRO(x) \
    x + result
    node.pos.x = result + other + local_p.x + local_q->x;
    return node.pos.y + completion_origin.x + completion_world.y +
           completion_ptr->x;
}

enum class CompletionMode;
enum class CompletionMode2 : unsigned;
```

另外在 `util.h` 增加 `typedef struct UtilBox { int w; } UtilBox;`，
在 `util.c` 末尾增加 `static UtilBox shared_point;`，用于验证当前文件
优先的同名静态变量类型解析。

测试用例：

- 普通补全：在函数内输入 `res`，列表活动且包含 `result`；Tab 接受后
  缓冲区出现 `result`，列表关闭。
- 全局结构体变量：输入 `completion_origin.`、`completion_world.`
  能列出 `x`、`y`；输入 `completion_ptr->` 能列出 `x`、`y`。
- 多声明符局部变量：输入 `oth` 能匹配到 `other`。
- 唯一候选不自动插入：输入 `hel`，列表只有 `helper`，仍处于活动状态；
  Enter 接受。
- 宏和函数：`GRE` 出现 `GREETING`，`completion_` 出现
  `completion_demo`。
- demo 局部结构体：在 `tests/fixtures/demo/main.c` 的 `main()` 里新起
  一行输入 `p.`，出现 `Point` 的 `x`、`y`；选择 `y` 后缓冲区为 `p.y`。
  这是用户实际试过的场景，必须覆盖。
- 成员补全：输入 `node.` 出现 `pos`、`next`、`alpha`、`beta`、
  `point_arr`；选择 `pos` 后输入 `.` 出现 `x`、`y`。
- 链式补全：输入 `node.pos.` 出现 `x`、`y`；输入 `node.next->pos.`
  同样出现 `x`、`y`。
- 数组接收者：`completion_points.`、`local_arr.`、`node.point_arr.`
  都不弹成员列表。
- Esc 抑制：输入 `re` 后 Esc，再输入 `s`，列表不活动；输入空格后
  重新输入 `re`，列表恢复。没有弹出框时按 Esc 也抑制下一个词。
- 注释/字符串/预处理续行不触发：在 `// re`、`"re"` 和宏反斜杠续行中
  不活动。
- 大小写：`po` 不匹配 `Point`。
- 同名静态变量：在 `util.c` 输入 `shared_point.` 只出现 `UtilBox` 的
  `w`，不会串到 `main.c` 的 `Point`。
- opaque enum：`enum class CompletionMode;` 和
  `enum class CompletionMode2 : unsigned;` 不产生变量符号。
- 鼠标：SGR 移动事件（button 35）不关闭浮层；浮层内单击只改变选中项，
  再次单击接受；滚轮滚动选中项；浮层外滚轮关闭浮层。
- 渲染：`app_render()` 后几何信息有效，弹出框不覆盖状态行/提示行。

### 10.3 回归

- `make test` 全部通过。
- `./si tests/fixtures/demo` 手动验证。
- 测试数量变化后更新 `AGENTS.md` 中的 test_core / test_keys / ui_smoke
  计数。

## 11. 文档

- `README.md` 的“它到底做了什么”增加变量补全条目；
  键盘说明补充 Tab/Enter 接受、Esc 抑制、鼠标双击接受、滚轮滚动。
- `AGENTS.md` 的运行时主流程/测试计数按实现结果更新。
- 运行 `python3 docs/build_doc.py` 更新架构文档。
- 不修改 `main.c` 的 `usage()`，因为没有新增命令行参数。

## 12. 已知限制

- 解析器是启发式的，不展开宏，不做模板/继承/重载。
- 聚合成员来自索引时的源文件；未保存的结构体定义不会立即进入补全，
  需要保存并 F5。
- 块作用域是近似的：同一函数里已经结束的内层块中的变量可能仍出现在
  候选里；同名时取最近声明。函数结束行会从当前缓冲区重新扫描，但在函数
  之前插入/删除行仍会暂时偏移索引行号，必要时按 F5 重建。
- `arr[i].x`、`(*p).x`、函数返回值链式访问不支持；解析失败不弹。
- C++ 继承成员、命名空间成员、模板成员、成员函数、静态数据成员和
  嵌套类型不支持；`this->` 不支持。
- 结构体成员里的匿名嵌套聚合只显示具名成员，不展开匿名成员。
- 多声明符局部变量在 `app_rebuild_locals` 中补齐；全局变量由 6.4 的
  `parse_variables()` 扩展补齐。
- 函数指针变量、成员函数、静态数据成员、嵌套类型暂不支持。

## 13. 验收标准

- 在 `tests/fixtures/demo` 的 `main()` 中新起一行输入 `p.`，能看到
  `x`、`y`。
- 输入 `completion_origin.`、`completion_world.`、`completion_ptr->`
  都能看到 `x`、`y`。
- 在 `tests/fixtures/demo` 中，输入 `re` 能看到 `result`、`return`。
- 输入 `node.` 能看到 `pos`、`next`、`alpha`、`beta`、`point_arr`，
  输入 `node.pos.` 能看到 `x`、`y`；输入数组接收者 `local_arr.` 或
  `completion_points.` 不弹列表。
- 只有一个候选时列表不自动补全，Tab/Enter 后才插入。
- Esc 后继续输入同一个词不再弹；换词后恢复。
- 注释、字符串、提示框、菜单里不弹。
- 候选很多时列表可滚动，候选不被截断。
- 单击只选中，双击接受；滚轮滚动；上下键移动。
- 输入大小写不匹配时不弹。
- 类型无法解析时不弹，不显示错误候选。
- `make test` 全部通过，无新增编译警告。

## 14. 留给实现者的决定

- 弹出框是否显示 `当前项/总数` 或滚动条；不得因此截断候选。
- 详情列的具体留白宽度。
- 成员缓存的具体哈希/线性查找方式（候选类型数量通常不大）。
- 多声明符局部变量补齐的具体实现方式，只要行为满足第 6.3 节。
- 是否把补全分节进一步拆到新文件；若拆，必须同步更新
  `Makefile` 的 `SRC` / `CORE` / `UI_CORE`。
