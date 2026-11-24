# AGENTS.md

## 交流与工作方式

- 全程用中文与用户交流；代码、标识符和源码注释沿用现有的英文风格。
- 解释代码时先梳理**入口 → 调用链 → 数据流 → 关键函数**，再给结论；引用真实文件和函数名，行号只作辅助（容易过期）。
- 本项目由 AI 生成（见 `AI_GENERATED.md`）。修改时保持“无第三方依赖”的约束：不要引入 ncurses、SQLite、LSP 或新的构建系统。

## 项目速览

- `si-c` 是终端里的 Source Insight 风格 C/C++ 编辑器与源码浏览器；C11 + POSIX + ANSI 转义，无第三方运行时依赖。Linux/macOS 是主要平台，Windows 控制台后端只用于开发和测试。
- 交互入口 `ui_run()` 声明在 `src/ui.h`，实现在 `src/app.c`（仓库里没有 `ui.c`）。
- 三个大文件：`app.c`（应用状态、按键、动作）、`render.c`（五面板绘制）、`index.c`（扫描、解析、关系图、缓存）。优先按文件内的 `/* --- 分节 --- */` 注释定位。
- 编译功能单独放在 `src/compile.c` / `src/compile.h`（工具链探测、构建计划、子进程和输出清理），UI 状态仍在 `App` / `app.c` / `render.c`。

## 构建与测试

- 构建：`make -j`；完整验证：`make test`；手动运行：`./si examples/demo`。
- 单测：
  - `make test_core && ./test_core tests/fixtures/demo`
  - `make test_keys && ./test_keys`
  - `make ui_smoke && ./ui_smoke tests/fixtures/demo > /dev/null`
- `ui_smoke` 把整帧 ANSI 写到 stdout，汇总信息写到 stderr；运行时必须重定向 stdout。
- `test_core` / `ui_smoke` 使用 `tests/fixtures/demo`；`examples/demo` 是手动运行示例，两者不要混用。
- Makefile 的 `%.o: %.c` 不跟踪头文件依赖：改过 `.h` 后执行 `make clean && make -j`，否则可能链接旧对象。
- 新增 `.c` 时必须同时加入 Makefile 的 `SRC`，以及测试所需的 `CORE` / `UI_CORE`，否则主程序或测试会漏符号。
- 编译选项是 `-std=c11 -Wall -Wextra -Wpedantic`；当前 GCC 下已有少量 `-Wformat-truncation` 警告（`util.c` / `render.c` / `app.c`），目标是不要新增警告。
- Linux 当前实测：test_core 364、test_keys 32、ui_smoke 365。`test_keys` 的 macOS Option 用例在 `#if defined(__APPLE__)` 内，数量会与本机不同。`ui_smoke` 会真实调用 C 编译器：一次编译 `tests/fixtures/demo`（该 fixture 故意混用 C/C++ 语法，预期失败），一次在临时目录编译最小程序（预期成功），并清理 `build/`。

## 运行时主流程

1. `src/main.c:main()` 解析参数；无路径且交互模式时不解析项目，交给 `ui_run()` 显示最近项目列表；显式路径才解析为项目根；批量模式无路径仍使用 `.`。
2. `ui_run()`（`src/app.c`）：`term_init()` → `state_load()` 读取 `recent=` 历史 → 有显式根目录才 `project_load_cached()` 并打开初始文件；否则空工作区 + `app_project_picker_open()`。
3. 主循环：`app_layout()` → 选择项修正 → 编辑器焦点时 `app_sync_callflow()` → `app_render()`（含补全浮层/编译窗口）→ `relations_pending` 时构建并保存关系表 → `term_read_key()` → `app_handle_key()`；编辑器按键后再刷新 outline/info/callflow。编辑字符/退格/删除后由 `app_completion_update()` 重新收集候选。
4. `Ctrl+F5`/Compile 菜单调用 `app_compile_begin()`：它会阻塞主循环，内部启动子进程、每 30ms 轮询输出并重绘，Esc/Ctrl+C 取消；结束后编译窗口保持打开，等待 Enter/Esc 关闭。
5. 退出时保存 `.si-index`（关系表就绪时）和 `~/.si-c-state`（含 `recent=` 历史），再 `term_shutdown()`，最后释放补全/编译运行时数据（含 `compile_log`）。

- 渲染是整帧重绘：`app_render()` 把内容写入 `Str`，最后一次性 `term_write()`，并用 `ESC[?2026h/l` 同步输出；不要在 `render.c` 里直接写 stdout。
- 输入：`term.c` 把字节流解码成 `KeyEvent`；`term_decode_sequence()` 是给测试用的纯解码入口。全局快捷键在 `app_handle_key()`，各面板按键在 `app_handle_editor_key()` / `app_handle_outline_key()` / `app_handle_file_key()` / `app_handle_symbol_key()`。
- 模态优先级：`app_handle_key()` 最先处理编译窗口，然后处理关系表提示、右键菜单、帮助、目录选择器、主菜单、符号选择器、输入提示、最近项目列表，再处理全局快捷键，最后按焦点分发；新增全局键要放在焦点 switch 之前。
- Ctrl+字母没有统一枚举；现有代码直接用 ASCII 控制码（如 `3`=Ctrl+C、`17`=Ctrl+Q、`19`=Ctrl+S）；新增按键请沿用同一模式。`F3`/`F4` 是当前文件查找下一个/上一个（复用 `last_find`），`Ctrl+F3`/`Ctrl+F4` 才是聚焦 CALL FLOW/编辑器。

## 数据模型与索引逻辑

- `Buffer`（`text.h`）：连续 UTF-8 字节；`cursor` 是字节偏移，列是显示列；Tab 宽 4。移动和删除必须用 `utf8_prev()` / `utf8_next()` / `buf_*` 辅助函数，不能假设一个字符占一个字节。
- 编辑后行索引会失效，`buf_line_*` 按需重建；撤销按 `group` 合并，`buf_undo_break()` 用于切断组。
- `Project`（`index.h`）：`files[]` 按 `rel` 排序，`syms[]` 按 file/line/col/kind/name 排序并去重；每个文件的符号在 `syms` 中必须连续，`files[i].sym_start/sym_count` 依赖这个不变量。改索引器时保持它。
- 索引流程（`index.c`）：`scan_dir()` 递归扫描（跳过隐藏目录、符号链接目录和固定构建目录）→ `lex_source()`（跳过注释和字符串）→ `parse_source()` 依次解析宏、聚合类型、typedef、函数、全局变量。解析器是启发式的，不展开宏、不建 AST、不处理模板特化/重载。`parse_variables()` 支持 `struct Point origin;`、带初始化器的全局变量和顶层多声明符；`using Alias = Type;` 会索引成 `SYM_TYPEDEF` 并解析右侧目标类型。`project_aggregate_members()` 按需读取源文件提取结构体/联合/类成员（数据成员、成员函数、静态成员、嵌套类型），记录成员函数返回类型，并合并基类成员；补全侧支持 `this->`、继承成员和 `Box<int>` 退化解析。`compute_braces()` / `parse_functions()` 在 `#if` / `#else` / `#endif` 分支边界重置 brace depth 和作用域，尽量把互斥分支里的函数都索引出来（不判断哪个分支真正生效）。
- 关系表是独立阶段：`project_load()` 只建符号；`project_build_relations()` 才建调用边和全局变量使用表。调用边有两类：函数体里的直接调用 `name(`，以及函数名作为值出现（函数指针、`signal(SIGINT, handler)`、`std::thread` 参数等）；后者标记为 `is_reference`，CALL FLOW 中显示为 `[ref]`。同名方法解析优先匹配调用点的 `Class::`/`ns::Class::` 限定，其次匹配调用者所在类，最后退回同文件/首个定义；CALL FLOW 方框用 `symbol_short_name()` 保留限定名。不跟踪先存入变量/结构体再注册的间接关系。CALL FLOW 在未构建时显示 `RELATION INDEX NOT BUILT`，按 `Alt/Option+R` 或在大项目提示中确认后才构建；写 UI 测试时要显式调用它。
- `project_find_refs()` / `project_search_text()` 从磁盘重新读文件，不包含未保存的缓冲区；`--refs` / `--search` 同理。
- 缓存 `.si-index` 位于项目根目录，magic 为 `SICACH2`、版本为 `5`。改动 `Project`/`ProjectFile`/`Symbol`/`CallEdge`/`VarUse` 的持久化字段或顺序时，必须提升 `SI_CACHE_VERSION`（必要时改 magic），否则旧缓存会被错误解读。缓存只校验文件大小 + mtime，不做增量扫描；新增或删除文件需按 F5 重建。
- `~/.si-c-state` 除 `root=`/`file=`/`cursor=` 外还保存 `recent=` 项目历史（最近在前、绝对路径、读取时去重）。空工作区退出也必须写入历史，不能因 `root` 为空跳过；旧版只有 `root=` 时会在读取阶段迁移为第一条 recent。
- `ui_run()` 里 `a.project = project` 是所有权转移；局部 `project` 不再单独释放，最后由 `project_free(&a.project)` 清理。
- 编译计划（`compile.c`）：项目根有 `Makefile` / `makefile` / `GNUmakefile` 时执行 `make -C <root>`；否则按扩展名探测 `cc`/`gcc`/`clang` 与 `c++`/`g++`/`clang++`，每个源文件 `-c` 到 `build/obj/<rel>.o`，再用 C/C++ 驱动链接成 `build/<项目名>`（目录由 `compile_plan_prepare()` 创建）。混合项目用各自的编译器编译、用 C++ 驱动链接。`compile_process_start()` 用 fork/pipe/setpgid，输出经 `compile_sanitize_append()` 去掉 ANSI/CR 并展开 Tab；取消时向进程组发 SIGTERM，再升级到 SIGKILL。Windows 后端只返回“不支持编译”错误。
- 编译窗口状态放在 `App`：`compile_active/running/ok/cancelled`、`compile_log`、`compile_scroll`、`compile_popup_*`；绘制在 `render.c:draw_compile()`，按键在 `app.c:app_compile_handle_key()`，编译前会 `save_all()`，Save 失败则不启动。

## 修改指引

- 加 UI 功能：`App` 字段放 `app.h` → 按键/动作放 `app.c` → 绘制放 `render.c` → 在 `tests/ui_smoke.c` 用 `app_handle_key_event()` / `app_handle_mouse_event()` 加覆盖。测试可用 `force_rows` / `force_cols` 固定布局。
- 改变量补全：`app.c` 的 `/* --- Completion --- */` 分节负责上下文、候选、接受和抑制；`render.c:draw_completion()` 画浮层；`index.c` 的 `project_type_aggregate_key()` / `project_aggregate_members()` 解析成员。`.` / `->` 走实例成员链，`::` 走类型/命名空间限定链（只列静态数据、成员函数、嵌套类型）；`this->`、继承成员和类方法内的裸标识符（由 `completion_collect_class_members()` 收集，局部变量同名时跳过）已支持。局部变量表由 `app_rebuild_locals()` 建立，`auto` 会从构造表达式、变量复制、取地址/引用、常见字面量和普通函数/成员函数返回类型推断；`constexpr` / `thread_local` 等修饰词按存储类跳过。`Box<int>` 这类模板实例化经 `project_type_aggregate_key()` 退化为按 `Box` 查成员，`using` 别名沿 typedef 目标类型解析。`app_rebuild_globals()` 重建按名字排序的符号指针索引并清空成员缓存；项目重载/F5 后缓存自动失效。
- 改索引/符号：同步 `index.c` + `index.h` + `test_core`；若改了缓存布局，提升版本号。补全新增的全局变量符号不改变 `.si-index` 格式，但旧缓存需要 F5 才能看到。
- 改终端按键/鼠标：`term.c` + `term.h` + `test_keys`；macOS 的 Option 符号映射在 `term.c:mac_option_base_key()`。`Ctrl+F5` 是 `KEY_CTRL_F5`，终端通常发送 `CSI 15;5~`。
- 改编译功能：`compile.c` 的 `compile_plan_*` / `compile_process_*` + `test_core`；UI 状态/按键在 `app.c` 的 `/* --- Compile --- */` 逻辑与 `render.c:draw_compile()`，并在 `ui_smoke` 覆盖菜单、居中窗口、滚动、缺编译器提示以及真实成功/失败编译。
- 改启动/最近项目：`util.c` 的 `state_*` + `app.c` 的 `app_project_picker_*` / `app_recent_touch()` + `render.c:draw_project_picker()`；`test_core` 覆盖状态历史，`ui_smoke` 覆盖列表交互。
- 常量与上限集中在 `si.h`（单文件 4 MiB、文件数 20k、符号数 200k、关系自动构建阈值 2k 文件 / 12 层）；改限制时同步 README。
- 用户文档以 `README.md` 为准；改键位时同步 `README.md`、`main.c` 的 `usage()` 和 `tests/ui_smoke.c`。
- 架构文档由 `docs/build_doc.py` 生成 `docs/si-c-architecture.html`，没有 Makefile 目标；改完运行 `python3 docs/build_doc.py`。脚本里的函数行号是手写的，可能过期，查代码以源码为准。
- 构建产物 `si`、`*.o`、`test_core`、`test_keys`、`ui_smoke` 和 `.si-index` 已被 `.gitignore` 忽略；不要提交。
- 无参数 `./si` 打开空工作区并显示最近项目列表，不自动打开项目；显式路径才直接打开。批量模式（`--index` / `--files` / `--symbols` / `--refs` / `--search`）不需要 TTY，适合脚本验证。批量模式不会写 `.si-index`，只有交互式 `ui_run()` 会写。
