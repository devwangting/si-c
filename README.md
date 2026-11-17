# si-c

> 用终端模拟 **Source Insight** 工作流的 C/C++ 编辑器与源码浏览器

![C11](https://img.shields.io/badge/C-11-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)
![AI Generated](https://img.shields.io/badge/AI-generated-6f42c1)

> [!IMPORTANT]
> **AI 生成声明**：本项目的源代码、测试和文档由 AI 编程助手（OpenCode / DeepSeek）
> 生成，人类负责提出需求、验收结果和发布仓库。它是 Source Insight 风格的独立实现，
> **不是 Source Insight 官方产品，也与 Source Insight 没有关联**。
> 详细说明见 [AI_GENERATED.md](AI_GENERATED.md)。

**English:** `si-c` is a terminal-based C/C++ editor and source browser inspired by
Source Insight. It is written in C11 with POSIX and ANSI escape sequences, has no
third-party runtime dependencies, and is AI-generated. See
[AI_GENERATED.md](AI_GENERATED.md).

## 项目定位

`si-c` 不是 Source Insight 的完整替代品，而是一个**可编译、可运行、可编辑、可跳转**
的终端原型。它把 Source Insight 最常用的“读代码 / 找定义 / 看调用关系”工作流搬进
终端：

- 左边看当前文件的函数、宏、类型和全局变量；
- 中间编辑代码；
- 下方看符号真正定义在哪、长什么样；
- 右边看项目文件和调用链；
- 所有面板都能用鼠标、键盘或分隔条操作。

整个程序只依赖 **C11 + POSIX + ANSI 转义序列**，不使用 ncurses、SQLite、LSP
或任何第三方库。终端建议至少 `100x30`，优先使用 256 色；16 色终端会自动降级。

## 它到底做了什么

- **五面板 TUI**：FILE OUTLINE、编辑器、SYMBOL INFO / CODE、PROJECT FILES、CALL FLOW。
- **项目级索引**：递归扫描 C/C++ 文件，识别函数、原型、宏、结构体/联合/枚举/类/命名空间、
  typedef、全局变量、枚举常量和结构成员。
- **项目符号搜索**：`Ctrl+P` 实时过滤全项目符号。
- **项目文本搜索**：`Alt/Option+G` 在磁盘源码中搜索并列出位置。
- **跳转定义 / 查找引用**：`F12` / `Shift+F12`，跳到真实文件和行号。
- **调用链**：按函数体中的 `name(...)` 调用表达式建图，同时把函数名作为值出现的
  位置标成 `[ref]`（例如 `signal(SIGINT, handler)`、`&handler`、`std::thread`
  参数），因此信号注册、回调和函数指针赋值也能在 CALL FLOW 里看到；支持多级
  调用者、循环检测和同名函数选择。
- **全局变量使用表**：查看一个全局变量被哪些函数、在哪些位置使用。
- **符号源码预览**：CODE 面板显示定义位置的原始代码，带行号和语法高亮。
- **编辑能力**：UTF-8、撤销/重做、自动缩进、Tab 转空格、查找、鼠标拖选、
  `Ctrl+C` / `Ctrl+V`、OSC52 终端剪贴板。
- **变量补全**：输入标识符时自动列出当前函数局部变量/参数、全局变量、函数、
  宏、类型和关键字；输入 `.` / `->` 列出结构体/类成员并支持链式访问，
  输入 `::` 列出类的静态成员、成员函数和嵌套类型，支持
  `Thing::Inner::` 和 `demo_ns::NsThing::` 这类限定名。C++ 类成员包括
  数据成员、成员函数、静态成员和嵌套类型，继承成员和 `this->` 也能列出；
  Tab/Enter 接受，Esc 放弃当前词；鼠标单击高亮、双击接受、滚轮滚动。
- **代码整理**：`F6`，内置 `astyle --style=ansi` 风格整理，保护预处理行。
- **变量高亮**：`F8` 高亮当前变量在可见区域和 CODE 面板中的所有同名出现。
- **主题与鼠标**：深色/浅色主题，SGR 鼠标点击、拖选、滚轮、四条可拖动分隔条。
- **项目缓存**：`.si-index` 保存文件、符号、调用边和变量使用关系，按文件大小和
  mtime 校验，减少重复索引。
- **最近项目**：无参数启动时显示最近打开/创建的项目列表，不自动打开上次项目。
- **命令行批处理**：`--index`、`--files`、`--symbols`、`--refs`、`--search`。
- **macOS Option 键支持**：Option 等同于 Alt，并识别默认键盘布局产生的符号。

## 界面

```text
 SI-C  demo  [main.c]
 FILE OUTLINE              │ main.c                      │ PROJECT FILES (3)
 macro  GREETING        L4 │  1 │ #include <stdio.h>      │> main.c
 struct Point           L6 │  2 │ #include "util.h"       │  util.c
 typedef Point          L9 │ ...│                         │  util.h
 fn     helper         L11 │ 18 │     Point p = {1, 2};   │
 fn     main           L16 │ 19 │     printf(...);        │
 var    global_counter L23 │ 20 │     return add(...);    │
                           │ CODE  p  main.c:18 ─────────│ CALL FLOW  helper ─────
                           │ 17 │ {                      │ static int helper  main.c:11
                           │ 18 │     Point p = {1, 2};   │ CALLERS (1)
                           │ 19 │     printf(...);        │ ↳ int main(void)   main.c:16
                           │ 20 │     return add(...);    │
 F2 outline  ^F3 flow  ^F4 editor  F3 next  F4 prev  F7 files  ^R context  ^F find  ^S save  F1 help
 Ln 18, Col 11  saved  fn:main
```

四条分隔条都可以用鼠标拖动：左栏/编辑器、编辑器/右栏、编辑器/CODE、
文件列表/调用链。悬停到分隔条时显示 `↔` / `↕` 手柄和当前比例。

## 与 Source Insight 的对应关系

| Source Insight | si-c |
| --- | --- |
| Project Files | 右上角 PROJECT FILES |
| Symbol Window | 左侧 FILE OUTLINE |
| Context Window | 中下 SYMBOL INFO / CODE |
| Relation Window | 右下 CALL FLOW |
| Jump to Definition | `F12` |
| Find References | `Shift+F12` |
| Project Symbol Lookup | `Ctrl+P` |
| Project-wide Search | `Alt/Option+G` |
| Syntax Formatting | `F6` |
| Browsing History | `Alt/Option+←/→` |
| Multiple Files | 打开文件标签 + `F9` / `Shift+F9` |

## 构建与运行

只需要 C 编译器和 make。

### Linux

```sh
sudo apt install build-essential      # Debian / Ubuntu
make -j
make test
./si examples/demo
```

其他发行版安装 `gcc`（或 clang）和 `make` 即可。程序没有链接 ncurses，
也没有运行时依赖。

### macOS

```sh
make -j
make test
./si examples/demo
```

Makefile 会在 macOS 上自动追加 `-D_DARWIN_C_SOURCE`。如果手动指定
`CPPFLAGS`，请保留 `-D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Isrc`。
支持 Intel 与 Apple Silicon；仓库中的安装包构建使用 Universal 二进制。

### 安装到系统

```sh
./install.sh                          # 构建并安装到 /usr/local/bin/si
PREFIX=$HOME/.local ./install.sh      # 安装到用户目录
```

脚本会先构建，再按需调用 `sudo` 写入目标目录；非交互环境可以设置
`SUDO_PASSWORD`。安装后可以在任意目录直接运行 `si`。

### 打开项目

```sh
./si /path/to/project        # 打开目录，递归索引
./si /path/to/project/a.c    # 打开文件，并索引它所在目录
./si                         # 显示最近项目列表，不自动打开
```

无参数运行 `./si` 时不会自动打开上次项目，而是在空工作区弹出最近项目列表；
从未打开或创建过项目时列表为空，按 `Ctrl+O` 打开目录或 `Ctrl+N` 新建项目。
显式传入路径时直接打开该路径，并把它加入最近项目。

最近项目列表支持 `Up/Down` 选择、`Enter` 打开、`Ctrl+O` 浏览目录、
`Ctrl+N` 新建项目、`Esc` 关闭列表。

## 命令行批处理

不进入 TUI 时可以直接查询索引，适合脚本或快速检查：

```sh
./si --index .
./si --files .
./si --symbols=main .
./si --refs=vec_push .
./si --search="VEC_OK" .
./si --theme=light .
```

| 选项 | 作用 |
| --- | --- |
| `--index` | 索引项目并输出文件/符号统计，然后退出 |
| `--files` | 列出项目中的源文件 |
| `--symbols[=PATTERN]` | 列出符号，支持大小写不敏感子串过滤 |
| `--refs=NAME` | 列出对 `NAME` 的引用 |
| `--search=TEXT` | 在项目文本中搜索 |
| `--theme=dark\|light` | 选择深色或浅色主题 |
| `--version` | 打印版本 |
| `--help` | 显示命令行帮助 |

`--refs` / `--search` 读取磁盘上已保存的内容，不包含未保存的缓冲区。

## 快捷键

### 面板与导航

| 按键 | 功能 |
| --- | --- |
| `F1` / `Ctrl+H` | 帮助 |
| `F2` / `Ctrl+B` | 聚焦左侧文件大纲 |
| `Ctrl+F3` | 聚焦右下调用链 |
| `Ctrl+F4` / `Esc` | 聚焦编辑器 |
| `F7` | 聚焦右上项目文件列表 |
| `Tab` / `Shift+Tab` | 循环切换五个面板 |
| `Ctrl+T` | 循环切换焦点 |
| `Alt/Option+M` | 打开顶部下拉菜单 |
| `Alt/Option+R` | 重建函数/变量/调用关系表 |
| `F5` | 重新扫描项目并重建索引 |
| `F10` / `Ctrl+F10` | 导航后退 / 前进 |
| `Alt/Option+←/→` | 导航后退 / 前进 |
| `F9` / `Shift+F9` | 下一个 / 上一个已打开文件 |
| `Ctrl+Q` | 退出（有未保存文件时先询问） |

### 搜索与跳转

| 按键 | 功能 |
| --- | --- |
| `F12` | 跳转到光标下标识符的定义 |
| `Shift+F12` | 查找光标下标识符的引用 |
| `Ctrl+P` | 全项目符号搜索（实时过滤） |
| `Alt/Option+G` | 全项目文本搜索 |
| `Ctrl+G` / `Ctrl+L` | 跳转到指定行 |
| `Ctrl+R` | 在右栏显示当前符号的上下文/调用关系 |
| `Ctrl+O` | 打开项目目录 |
| `Ctrl+N` | 新建项目 |
| `Enter` | 打开文件 / 跳转到选中的符号 |

### 编辑与显示

| 按键 | 功能 |
| --- | --- |
| `Ctrl+S` | 保存当前文件 |
| `Ctrl+A` | 保存所有文件 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Ctrl+F` | 文件内查找 / 查找下一个 |
| `F3` | 按上次搜索内容向下查找（当前文件） |
| `F4` | 按上次搜索内容向上查找（当前文件） |
| `F8` | 变量高亮 / 取消高亮 |
| `Shift+F8` | 查找上一个 |
| `F6` | 代码整理（ANSI 风格） |
| `Ctrl+K` / `Ctrl+U` | 删除到行尾 / 删除到行首 |
| `Ctrl+←/→` | 按单词移动 |
| `Ctrl+Home/End` | 文件首 / 文件尾 |
| 鼠标拖选 + `Ctrl+C/V` | 选中文本、复制、粘贴 |
| 鼠标滚轮 / `PgUp` / `PgDn` | 滚动 CODE 面板或列表 |

### 补全列表

输入标识符时自动出现候选，不自动接受唯一候选。列表活动时：

| 按键/鼠标 | 功能 |
| --- | --- |
| `Up` / `Down` | 上/下移动选中项 |
| `PgUp` / `PgDn` / `Home` / `End` | 翻页 / 首项 / 末项 |
| `Tab` / `Enter` | 接受选中项 |
| `Esc` | 关闭列表，并停止当前这个词的自动匹配 |
| 鼠标单击 | 只高亮选中 |
| 鼠标双击同一项 | 接受 |
| 鼠标滚轮 | 滚动候选列表 |

候选来自当前函数可见的局部变量/参数、全局变量、函数、宏、类型、C/C++ 关键字
和内置类型；输入 `.` 或 `->` 后只列结构体/联合/类成员，支持 `a.b.c`、
`a.b->c` 这类链式访问。注释、字符串和预处理行里不触发。

### macOS 的 Option 键

Mac 键盘没有 Alt 键，键盘上的 **Option（⌥）** 就是 Alt 的等价键。程序会把
Option 与字母/数字组合产生的符号（例如 Option+R = `®`、Option+G = `©`、
Option+M = `µ`、Option+1 = `¡`）映射到对应快捷键，Option+←/→ 也会后退/前进。

ABC/美式键盘下 Option+E 和 Option+I 是死键（要再按一个字符才输出重音），
此时可用 Option+Shift+E / Option+Shift+I，或改用 Option+3 / Option+5；
也可以在 Terminal 设置中勾选 **Use Option as Meta Key**，之后所有 Option
组合都会直接作为 Alt 组合发送。

## 主要功能说明

### 项目索引

启动或打开项目时，`si-c` 会递归扫描 `.c/.h/.cc/.cpp/.cxx/.hpp/.hh/.hxx/.inl`
等文件，跳过 `.git`、`node_modules`、`build`、`out`、`target`、`dist` 等目录。
索引器使用不依赖第三方库的轻量 lexer，跳过注释和字符串，识别：

- 函数定义 / 函数原型
- `struct` / `union` / `enum` / `class` / `namespace`
- `typedef`（包括函数指针 typedef）
- `#define` 宏
- 文件作用域全局变量
- 枚举常量
- 结构体/类成员

索引结果按文件和行号排序。解析器是启发式的，不展开宏，也不构建完整 AST；
这是“快速浏览”与“完整语义分析”之间的取舍。

### 项目缓存与恢复

- 首次索引后在项目根目录写入隐藏文件 `.si-index`，保存文件、符号、调用边和
  变量使用表。
- 再次打开项目时优先加载缓存；源码变化后通过文件大小和修改时间校验并自动重建。
- 启动和读取缓存时显示 `LOADING` 进度窗口。
- 无参数运行 `./si` 时，从 `~/.si-c-state` 读取最近打开/创建的项目并显示列表，
  不会自动打开项目。
- `~/.si-c-state` 的 `recent=` 行保存项目历史，按最近使用排序；旧版的 `root=`
  会自动作为第一条历史迁移。
- 新增/删除文件不会自动增量扫描；按 `F5` 强制重建。

### 大项目

- 项目文件和符号有上限：20,000 个文件、200,000 个符号。
- 文件数超过 2,000 或目录深度超过 12 时，启动不会自动构建调用图/变量关系表，
  而是显示 `LARGE PROJECT` 提示：
  - `Enter` / `Esc`：先浏览代码，不构建关系表；
  - `Alt/Option+R`：立即构建，完成后显示确认窗口。
- 索引期间显示扫描/解析进度，`Esc` / `Ctrl+C` 可取消。

### 调用链

在编辑器中把光标移到函数名或变量上，右下角 CALL FLOW 会显示：

- 目标函数/变量在左侧；
- 调用者向右生长，同一函数的多个调用者纵向排列；
- 每个方框对应真实调用点，`Enter` 跳到 `caller(...)` 那一行；
- 最多 8 层、200 个节点，带循环检测；
- 同名函数会弹出 `SELECT DEFINITION` 选择框；
- 直接按 `Ctrl+R` 也可以刷新当前符号的上下文。

关系表未建立时，CALL FLOW 会显示 `RELATION INDEX NOT BUILT` 和
`Press Alt/Option+R to build call/variable tables`，不会偷偷重建。

### 符号信息 / CODE 面板

- 显示当前符号的类型、定义文件、定义行和源码片段。
- 文件位置和定义代码中的标识符使用高亮背景。
- 长函数可用 `PgUp` / `PgDn` / 滚轮滚动；顶部两行身份信息保持可见。
- 在 CODE 面板双击，编辑器跳到定义位置。
- 全局变量、局部变量、函数参数、结构成员都会定位到声明/定义位置。

### 语法高亮与颜色

- 关键字、用户类型、函数、字符串、数字、宏/预处理行分别着色。
- **全局变量和枚举常量用红色，函数内局部变量/参数用浅蓝色**。
- 局部变量按函数级作用域近似处理；同名时局部变量优先。
- 支持 `SI_THEME=light` / `--theme=light` 浅色主题。
- 256 色终端显示完整颜色；16 色终端自动降级。

### 代码整理

`F6` 或菜单 **Format Code...** 打开选择：

```text
Format: [c] current file  [a] all files  [Esc] cancel
```

- `c` / `Enter`：整理当前缓冲区，可撤销。
- `a`：整理项目中所有 C/C++ 文件并写回磁盘，然后重新索引。
- 规则参考 `astyle --style=ansi`：4 空格缩进、函数左大括号独占一行、
  控制语句 `{` 同行、清理行尾空白。
- 预处理行（`#define`、`#if` 及反斜杠续行）不参与缩进整理。
- 全项目整理前会拒绝有未保存修改的缓冲区。

### 鼠标与编辑

- 点击任意面板激活；再次点击打开文件/跳转符号。
- 补全列表：单击只选中，双击接受，滚轮滚动；点击列表外关闭。
- 编辑器内右键打开 Copy/Paste 菜单；其他区域打开主菜单。
- 拖选文本，`Ctrl+C` 复制，`Ctrl+V` 粘贴；复制同时写 OSC52 系统剪贴板。
- 四条分隔条可拖动；悬停显示手柄和比例。
- 终端自己的文本选择可按住 `Shift` 拖选（GNOME Terminal 等通用行为）。

## 源码结构

```text
src/
  si.h         公共类型、容器、限制常量
  util.c       字符串/路径/目录遍历/UTF-8/原子写文件
  text.c       可编辑缓冲区、行索引、撤销重做、查找、词边界
  syntax.c     按行状态机的 C/C++ 语法高亮
  index.c      项目扫描、词法分析、符号/引用/文本搜索、索引缓存
  term.c       POSIX termios + ANSI；Windows 控制台开发后端；按键解码
  app.c        应用状态、焦点、编辑动作、搜索、导航、主循环
  render.c     五面板 TUI、颜色、状态栏、帮助覆盖层
  main.c       命令行参数与批处理模式
tests/
  test_core.c  缓冲区/UTF-8/语法/索引器/调用关系
  test_keys.c  终端按键/鼠标序列解码
  ui_smoke.c   无终端 UI 驱动测试
  fixtures/    小型 C 工程
examples/demo  可编译的示例项目
docs/          架构说明与生成脚本
```

更多实现细节见 [docs/si-c-architecture.html](docs/si-c-architecture.html)。

## 测试

当前 Linux 工作区实测：

```sh
make clean && make -j && make test
```

结果：

```text
test_core: 276 checks, 0 failures
test_keys:  30 checks, 0 failures
ui_smoke:  305 checks, 0 failures
```

macOS 的 `test_keys` 还包含 Option 键用例，检查数量会与本机不同。
测试覆盖缓冲区编辑、UTF-8、语法高亮、项目索引、调用关系、状态历史、终端
按键/鼠标解码、五面板渲染和主要 UI 动作。

Linux 使用同一套 POSIX 代码路径；建议首次在 Linux 上执行
`make clean && make -j && make test`。Windows 控制台后端主要用于开发和测试，
不是主要目标平台。

## 已知限制

- 解析器是启发式的，不展开宏，不处理模板实例化、完整继承图和重载解析；
  类成员补全只合并能从 `class X : public Base` 直接读到的基类成员。
- 引用搜索不会进入 `#define` 宏体；`#if`/`#else` 的所有分支都会被扫描。
- “查找引用”、调用关系和全项目文本搜索读取**磁盘文件**，不包含未保存修改。
- 文件大纲不会在每次按键后重解析；保存、重开或 `F5` 后更新。
- 最近项目列表只记录项目根；选中后打开 `main.c` 或第一个文件，不恢复每个项目
  上次打开的文件和光标。
- 调用者树最多 8 层、200 个节点；不做重载/虚函数解析，也不构建类继承图。
- CALL FLOW 的 `[ref]` 只识别函数体里的直接函数值引用；像 `auto h = handler;`
  之后再注册 `h` 的间接关系不会继续跟踪。
- 预处理器条件分支不展开；索引器会尽量扫描 `#if` / `#else` 的所有分支，可能同时
  列出未参与编译分支里的符号。
- 局部变量使用函数级符号表，块级 `{}` 作用域按函数级近似；在函数前插入/删除
  行会暂时偏移索引行号，必要时按 `F5` 重建。
- 补全只列数据成员，不列成员函数、静态数据成员、嵌套类型或 `this->`；数组下标、
  `(*p).x`、函数返回值链式访问在类型无法解析时不弹列表。
- CODE 预览对超长函数只显示窗口，可用滚轮 / `PgUp` / `PgDn` 滚动。
- 有鼠标拖选和普通行编辑，但没有列选择、多光标、分屏、LSP、CTags 导入、
  持久化符号数据库。
- 单文件上限 4 MiB，项目文件上限 20,000，符号上限 200,000。
- 主要为 Linux 编写；macOS 已支持并验证；Windows 控制台后端仅用于开发/测试。

## AI 生成声明

- **代码、测试、文档**：主要由 AI 编程助手（OpenCode / DeepSeek）生成。
- **人类角色**：提出需求、选择方向、验收结果、发布仓库。
- **项目性质**：Source Insight 风格的独立终端实现，不是 Source Insight 官方产品，
  也不隶属于 Source Insight 的开发商。
- **风险提示**：AI 生成的代码可能存在错误、遗漏或安全风险。用于生产环境前请自行
  阅读、测试和审查。
- **许可证**：MIT，见 [LICENSE](LICENSE)。

## License

MIT License. Copyright (c) 2026 si-c contributors.
