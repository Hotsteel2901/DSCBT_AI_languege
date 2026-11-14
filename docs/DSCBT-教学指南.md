# 《DSCBT 语言教程》

> 从零开始学习 DSC（DSCBT）  
> 结构参考《Rust 程序设计语言》（The Rust Programming Language）  
> 对应解释器版本：v3.0

---

## 前言

### 这本教程写给谁

这本教程面向三类读者：

1. **第一次接触 DSCBT 的初学者**：你会写一点 Python、JavaScript 或 C，想快速掌握一门语法简洁的脚本语言。
2. **正在学习编译原理的学生**：DSCBT 的解释器只有一个 C 文件，包含词法分析、语法分析、字节码编译和虚拟机，非常适合配合阅读。
3. **想给 DSCBT 写工具或扩展的人**：教程会解释值模型、作用域、调用栈和字节码设计，而不只是罗列语法。

你不需要 Rust 基础。教程借用了《Rust 程序设计语言》的章节节奏和教学方式，但 DSCBT 本身是一门动态类型、解释执行的小语言，很多 Rust 概念在这里并不存在（例如所有权、借用、Trait、泛型）。遇到这些差异时，我们会明确说明。

### 怎样使用这本教程

- **按顺序阅读**。第 1 章到第 3 章是基础，第 4 章解释 DSCBT 最容易被误解的“值/引用”行为，第 5 章之后开始写真正的程序。
- **把代码亲手敲一遍**。教程中的完整程序都可以直接保存为 `.dscbt` 文件运行；输出块展示的是运行结果。
- **不要跳过练习**。每章末尾的练习都针对本章最容易出错的地方设计，附录 D 提供部分参考答案。
- **对照 Rust 章节阅读**。每个主要章节都有“对照 Rust”小节，说明同一个主题在 Rust 中如何呈现，以及 DSCBT 为什么不同。

### 约定

教程使用以下标记：

- `dsc` 代码块：可以直接运行的完整程序。
- `dsc error` 代码块：故意写错、用于演示报错的程序。
- `text` 代码块：程序输出、终端记录或语法示意，不是可执行代码。
- **提示**：经验性建议。
- **注意**：容易踩坑的地方。
- **对照 Rust**：与《Rust 程序设计语言》的对应关系。

### 章节结构与 Rust 教程的对应关系

| 本教程 | 《Rust 程序设计语言》对应内容 | 说明 |
|---|---|---|
| 第 1 章 入门 | 第 1 章 入门指南 | 安装、构建、Hello World |
| 第 2 章 猜数字游戏 | 第 2 章 写一个猜数字游戏 | 用一个小项目串联语法 |
| 第 3 章 常见编程概念 | 第 3 章 常见编程概念 | 变量、类型、函数、控制流 |
| 第 4 章 值、引用与内存 | 第 4 章 认识所有权 | DSCBT 没有所有权，用“值/引用”替代 |
| 第 5 章 用类组织数据 | 第 5 章 使用结构体组织相关联的数据 | DSCBT 的 `class` 是结构体而非 OOP 类 |
| 第 6 章 数组与字符串 | 第 8 章 常见集合 | 固定长度数组、不可变字符串 |
| 第 7 章 函数、递归与程序结构 | 第 3、7 章相关部分 | 分解问题、递归、无模块系统 |
| 第 8 章 错误处理与调试 | 第 9 章 错误处理 | 无异常/Result，错误即终止 |
| 第 9 章 项目实战 | 第 12 章 一个 I/O 项目 | 成绩管理 + 排序 |
| 第 10 章 性能与字节码原理 | 第 10 章 泛型、Trait 与生命周期（性能视角） | DSCBT 的分层与优化思路 |
| 第 11 章 与 Rust、Python 对照 | 附录与跨语言对照 | 帮助迁移已有经验 |
| 第 12 章 限制与下一步 | 第 19、20 章进阶与总结 | 语言边界与扩展方向 |

### 版本说明

本教程描述 **v3.0** 的行为。v2 与 v3 有一处重要差异：v3 修复了“函数内无法读取顶层变量”的缺陷，调整了 `input()` 的提示符处理，新增了多参数 `print`、通用数组元素、字符串比较等。完整变化见附录 E。

---

## 目录

1. [第 1 章 入门](#第-1-章-入门)
2. [第 2 章 猜数字游戏](#第-2-章-猜数字游戏)
3. [第 3 章 常见编程概念](#第-3-章-常见编程概念)
4. [第 4 章 值、引用与内存](#第-4-章-值引用与内存)
5. [第 5 章 用类组织数据](#第-5-章-用类组织数据)
6. [第 6 章 数组与字符串](#第-6-章-数组与字符串)
7. [第 7 章 函数、递归与程序结构](#第-7-章-函数递归与程序结构)
8. [第 8 章 错误处理与调试](#第-8-章-错误处理与调试)
9. [第 9 章 项目实战：成绩管理与排序](#第-9-章-项目实战成绩管理与排序)
10. [第 10 章 性能与字节码原理](#第-10-章-性能与字节码原理)
11. [第 11 章 与 Rust、Python 对照](#第-11-章-与-rustpython-对照)
12. [第 12 章 限制、下一步与学习路线](#第-12-章-限制下一步与学习路线)
- [附录 A 语法速查](#附录-a-语法速查)
- [附录 B 关键字、运算符与内置函数](#附录-b-关键字运算符与内置函数)
- [附录 C 错误码与诊断](#附录-c-错误码与诊断)
- [附录 D 练习参考答案（精选）](#附录-d-练习参考答案精选)
- [附录 E v2 → v3 行为变化](#附录-e-v2--v3-行为变化)

---

# 第 1 章 入门

本章目标：把 DSCBT 跑起来，理解一个源文件从磁盘到输出的完整路径，并学会看懂最基本的报错。

## 1.1 安装与构建

DSCBT 的源文件以 `.dscbt` 结尾。解释器本身是一个 C 程序（`dsc.c`），运行时不需要 C 编译器；但**第一次构建解释器**需要一个 C 编译器。

### 在 Linux / macOS 上构建

打开终端，进入项目目录：

```text
cd dsc
gcc -O2 -o dsc dsc.c
```

如果系统没有 GCC，可以使用 Clang：

```text
clang -O2 -o dsc dsc.c
```

构建完成后，检查版本：

```text
./dsc --version
```

输出：

```text
dsc 3.0
```

查看帮助：

```text
./dsc --help
```

### 在 Windows 上构建

项目根目录提供了 `build.bat`。双击运行，或在命令行执行：

```text
build.bat
```

脚本会依次尝试 GCC、Clang、MSVC，找到可用的编译器后生成 `dsc.exe`。也可以手动执行：

```text
gcc -O2 -o dsc.exe dsc.c
```

### 运行第一个程序

新建 `hello.dscbt`：

```dsc
# hello.dscbt
print("Hello, DSC!")
```

运行：

```text
./dsc hello.dscbt
```

输出：

```text
Hello, DSC!
```

Windows 下把 `./dsc` 换成 `dsc.exe` 即可。

> **提示**：命令行参数只有源文件路径；不支持一次运行多个文件，也不支持从标准输入读取源码。

## 1.2 Hello, World! 的细节

### `print` 是什么

`print` 是内置函数，不是关键字。它接受任意数量的参数，用空格分隔，最后输出一个换行：

```dsc
print("Hello")
print("DSC", "v3", 2026)
print()
```

输出：

```text
Hello
DSC v3 2026

```

最后一行是 `print()` 输出的空行。`print` 可以打印所有类型：

```dsc
print(42)
print(3.5)
print(True)
print("文本")
print([1, 2, 3])
```

输出：

```text
42
3.5
True
文本
[1, 2, 3]
```

数组和对象也能打印。数组会展开成 `[元素, 元素]`，对象统一显示为 `{object}`（字段不展开）。

### 字符串与转义

字符串用双引号包裹。支持以下转义：

| 写法 | 含义 |
|---|---|
| `\n` | 换行 |
| `\t` | 制表符 |
| `\r` | 回车 |
| `\\` | 反斜杠 |
| `\"` | 双引号 |

示例：

```dsc
print("第一行\n第二行")
print("列1\t列2")
print("他说：\"你好\"")
```

输出：

```text
第一行
第二行
列1	列2
他说："你好"
```

DSCBT 的字符串是 UTF-8 字节序列。中文可以直接写：

```dsc
print("你好，DSCBT！")
```

> **注意**：字符串必须写在一行内。DSCBT 不支持多行字符串，也不支持用单引号表示字符串。

### 注释

支持两种单行注释：

```dsc
# 井号注释
// 双斜杠注释
print("注释不会被执行")  # 行尾注释
```

没有块注释（`/* ... */`）。注释只到行尾。

## 1.3 程序如何运行

执行 `dsc hello.dscbt` 时，解释器内部依次做四件事：

```text
源文件 .dscbt
   │
   ▼
[1] 词法分析 lexer     把字符流切成 Token（标识符、数字、字符串、缩进）
   │
   ▼
[2] 语法分析 parser    把 Token 组织成 AST（抽象语法树）
   │
   ▼
[3] 字节码编译 compiler 把 AST 编译成 8 字节定长指令 + 常量池 + 函数表
   │
   ▼
[4] 虚拟机 VM          逐条执行指令，维护操作数栈、局部变量帧、全局槽
   │
   ▼
程序输出 / 运行时错误
```

这个流程带来两个直接后果：

1. **语法错误在执行前就会报出**。程序一行都不会运行。
2. **运行时错误带行号**。因为每条指令都内嵌了源代码行号。

### 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 正常结束 |
| 1 | 命令行用法错误或文件读取失败 |
| 2 | 词法/语法/编译错误 |
| 3 | 运行时错误 |
| 4 | 内存不足 |

脚本或 CI 可以据此判断失败类型。

### 一个简单但完整的程序

```dsc
# greet.dscbt
name = "DSC"
year = 2026
print("欢迎使用", name)
print("年份:", year)
print("1 + 1 =", 1 + 1)
```

输出：

```text
欢迎使用 DSC
年份: 2026
1 + 1 = 2
```

注意 `print("年份:", year)` 的两个参数之间会自动插入一个空格。

## 1.4 新手常见问题

### 找不到命令

```text
bash: ./dsc: No such file or directory
```

说明还没有构建，或者当前目录不对。先执行 `gcc -O2 -o dsc dsc.c`，并确认 `dsc` 与源文件在同一目录（或使用绝对路径）。

Linux/macOS 上如果提示权限不足：

```text
chmod +x dsc
```

### 缺少冒号

```dsc error
x = 1
if x > 0
    print(x)
```

```text
错误 [2:9]: 期望 :，实际为 NL
```

`if`、`while`、`for`、`def`、`class` 的头部都必须以冒号 `:` 结束。

### 缩进不一致

```dsc error
x = 1
    y = 2
```

```text
错误 [2:5]: 意外的记号 INDENT
```

DSCBT 用缩进表示代码块。同一层的语句必须从同一列开始；增加缩进意味着进入子块，减少缩进意味着离开子块。不要在同一份文件里混用空格和制表符。

### 把 `els` 写成 `else`

DSCBT 的关键字是 `els`，不是 `else`：

```dsc error
x = 1
if x > 0:
    print(1)
else:
    print(2)
```

```text
错误 [4:5]: 意外的记号 :
```

`else` 会被当成普通标识符，因此后面单独一个 `:` 就成了非法记号。类似地，`elif` 要写成 `els if`。

### 字符串没有闭合

```dsc error
print("hello)
```

```text
错误 [1:7]: 字符串未闭合
```

### 程序没有输出

检查：

1. 是否真的调用了 `print`；
2. 是否把代码写在了不会执行的分支里；
3. 是否 `while` 条件一开始就为假；
4. 是否在 `return` 之后写了代码（`return` 后的同块代码不会执行）。

## 1.5 小结与练习

本章你学会了：

- 构建和运行 DSCBT；
- `print`、字符串、转义、注释；
- 四阶段执行流程与退出码；
- 四类最常见的新手错误。

练习：

1. 输出一行包含双引号的中文句子。
2. 用 `print` 输出一个三行的小票，要求使用 `\n` 和 `\t`。
3. 故意制造“缺少冒号”“缩进错误”“`else` 拼写错误”三种错误，记录报错信息与退出码。
4. 用 `--version` 和 `--help` 观察命令行行为。

---

# 第 2 章 猜数字游戏

本章目标：通过一个完整的小游戏，把变量、输入、循环、条件分支和布尔运算串起来。这是《Rust 程序设计语言》第 2 章的教学方式：先做出能跑的东西，再解释细节。

## 2.1 游戏规则

程序随机（或伪随机）生成一个 1 到 100 之间的整数，玩家反复输入猜测：

- 猜小了，提示“太小了”；
- 猜大了，提示“太大了”；
- 猜对了，提示成功并结束。

DSCBT 没有随机数库，所以第一版先使用固定数字 `42`，保证每次运行结果可复现；稍后我们用一个简单的线性同余公式生成“看起来随机”的数字。

## 2.2 第一版：固定答案

创建 `guess.dscbt`：

```dsc
# 猜数字 v1：固定答案
secret = 42
guess = -1
tries = 0

while guess != secret:
    guess = input("请输入 1-100 的数字: ")
    tries = tries + 1

    if guess < secret:
        print("太小了")
    els if guess > secret:
        print("太大了")
    els:
        print("猜对了！")

print("你用了")
print(tries)
print("次")
```

一次运行记录（带下划线的部分是玩家输入）：

```text
请输入 1-100 的数字: 50
太小了
请输入 1-100 的数字: 75
太大了
请输入 1-100 的数字: 42
猜对了！
你用了
3
次
```

### 逐行解释

- `secret = 42`：赋值即声明。DSCBT 没有 `let` 或 `var`。
- `guess = -1`：先给一个不可能等于答案的初值，保证循环至少执行一次。
- `tries = 0`：计数器。
- `while guess != secret:`：条件为真时重复执行缩进块。
- `input("...")`：打印提示，读取一行，并把输入解析为整数返回。输入不是整数时返回 `0`。
- `tries = tries + 1`：没有 `+=` 运算符，必须写完整形式。
- `if / els if / els`：多分支。注意关键字拼写。
- 最后三行 `print` 分三次输出，是为了避免多参数格式化带来的干扰；也可以写成 `print("你用了", tries, "次")`。

### 为什么先判断“猜对”不放在最前面

代码里先判断 `<` 和 `>`，最后用 `els` 处理相等。这保证只有三种情况之一会执行。如果先写 `if guess == secret`，那么还需要处理另外两种情况，代码会更长。

## 2.3 第二版：伪随机答案

没有随机库，但可以用线性同余生成器（LCG）制造伪随机序列：

```dsc
# 猜数字 v2：伪随机答案
seed = 2026
secret = (seed * 73 + 41) % 100 + 1

print("我已经想好了一个 1-100 的数字。")
guess = -1
tries = 0

while guess != secret:
    guess = input("请输入你的猜测: ")
    tries = tries + 1

    if guess < secret:
        print("太小了")
    els if guess > secret:
        print("太大了")
    els:
        print("猜对了！")

print("答案:", secret)
print("次数:", tries)
```

把 `seed` 改成不同的整数，就能得到不同的答案。`%` 是取模，`(seed * 73 + 41) % 100` 的范围是 0 到 99，再加 1 得到 1 到 100。

> **注意**：这不是安全的随机数，也不适合模拟和游戏正式发布；它只是教学演示。

## 2.4 第三版：限制次数

增加“最多 7 次机会”，并且处理“机会用完”的情况：

```dsc
# 猜数字 v3：限制次数
seed = 2026
secret = (seed * 73 + 41) % 100 + 1
max_tries = 7

guess = -1
tries = 0
win = False

while guess != secret and tries < max_tries:
    guess = input("请输入你的猜测: ")
    tries = tries + 1

    if guess < secret:
        print("太小了")
    els if guess > secret:
        print("太大了")
    els:
        win = True

if win:
    print("恭喜你，猜对了！")
els:
    print("机会用完了。答案是:", secret)

print("总次数:", tries)
```

这里出现了两个新要点：

1. `and` 是短路求值：如果 `guess != secret` 已经为假，右边的 `tries < max_tries` 不会影响结果；两个条件都为真才继续循环。
2. 没有 `break`。我们用 `win` 标记是否猜中，循环结束后再统一判断。

> **对照 Rust**：Rust 的猜数字示例使用 `std::io`、`rand`、`match` 和 `Ordering`。DSCBT 没有标准库、枚举和模式匹配，因此用 `if / els if / els` 和整数比较完成同样的逻辑。

## 2.5 常见错误

### 输入不是整数

`input` 返回整数。输入 `abc` 会得到 `0`，于是程序会继续把它当成一次猜测。教学版可以接受；真实程序应先验证，但 DSCBT 没有“读取字符串”的函数，所以无法区分“输入了 0”和“输入了非数字”。

### 忘记更新计数器

```dsc
while guess != secret:
    guess = input("请输入: ")
    # 忘记 tries = tries + 1
```

程序仍能结束（只要猜中），但次数统计永远是 0。

### 把 `and` 写成 `&&`

DSCBT 支持 `&` 作为 `and` 的别名，但不支持 `&&`。同理，`||` 要写成 `|` 或 `or`。

## 2.6 练习

1. 改成“最多 5 次”，并在每次猜测后显示剩余次数。
2. 增加评分：1 次猜中 100 分，2 次 90 分，之后每次减 10 分，最低 0 分。
3. 增加“提示范围”：每次猜错后，告诉玩家当前已知的区间（例如“答案在 51 到 100 之间”）。
4. 把答案改成由两个输入种子共同决定，观察可复现性。
5. 思考：如果没有 `break`，如何实现“猜中后立即退出循环，不再读取输入”？提示：`while` 条件 + 标记变量。

---

# 第 3 章 常见编程概念

本章目标：系统掌握变量、类型、函数、注释和控制流。这一章对应《Rust 程序设计语言》第 3 章，但 DSCBT 是动态类型语言，很多规则更宽松，也更需要自律。

## 3.1 变量与重新赋值

### 赋值即声明

```dsc
count = 0
name = "DSC"
price = 3.5
enabled = True
```

变量没有类型标注，类型由当前值决定。变量可以随时改成另一种类型：

```dsc
x = 10
print(x)
x = "现在是字符串"
print(x)
x = 2.5
print(x)
```

输出：

```text
10
现在是字符串
2.5
```

### 命名规则

- 以字母或下划线开头；
- 后续字符可以是字母、数字、下划线；
- 区分大小写；
- 目前只支持 ASCII 字母，不支持中文变量名。

合法示例：`x`、`total`、`_temp`、`item2`、`maxValue`。  
非法示例：`2fast`、`a-b`、`中文名`。

### 没有 `let`、`const`、`mut`

DSCBT 的变量天然“可变”。习惯上，全大写名字表示常量：

```dsc
MAX_TRIES = 7
PI = 3.14159
```

这只是约定，解释器不会阻止你重新赋值。

### 顶层变量与函数局部变量

这是 DSCBT 最重要的作用域规则之一：

- 顶层（不在任何 `def` 里）的变量是**模块全局变量**；
- 函数参数和函数内部新建的变量是**局部变量**；
- 函数可以读取全局变量；
- 函数对“已存在的全局变量”赋值时，会修改全局变量，而不是创建同名局部变量。

示例：

```dsc
total = 0

def add_to_total(n):
    total = total + n

add_to_total(5)
add_to_total(7)
print(total)
```

输出：

```text
12
```

这与 Python 不同（Python 会报 `UnboundLocalError`），但与 DSCBT v2 的行为一致。若想使用局部变量，换一个名字，或把它作为参数传入：

```dsc
def add(a, b):
    result = a + b
    return result

print(add(1, 2))
```

`result` 是函数局部变量，函数返回后不可见。

> **注意**：DSCBT 没有块级作用域。`if`、`while`、`for` 里第一次赋值的变量，在块外仍然可见（在顶层就是全局变量）。

### 变量必须先赋值再参与运算

读取一个从未赋值过的全局变量不会报错，它的值是 `nil`；但把 `nil` 用于算术或比较会触发运行时错误：

```dsc error
print(missing + 1)
```

```text
错误 [1:0]: 不支持的操作数类型: nil + int
```

因此，养成“先初始化，再使用”的习惯。

## 3.2 数据类型

DSCBT 有七种运行时类型：

| 类型名（`type()`） | 字面量示例 | 说明 |
|---|---|---|
| `int` | `42`、`-7` | 64 位有符号整数 |
| `float` | `3.14`、`-0.5` | IEEE 754 双精度浮点数 |
| `str` | `"hello"` | UTF-8 字符串，不可变 |
| `bool` | `True`、`False` | 布尔值 |
| `array` | `[1, 2, 3]` | 固定长度数组，元素可为任意类型 |
| `object` | `Point { x: 1, y: 2 }` | 类实例，字段可读写 |
| `nil` | 无字面量 | 未初始化变量、缺失字段的值 |

用 `type()` 查看：

```dsc
print(type(42))
print(type(3.14))
print(type("hi"))
print(type(True))
print(type([1, 2]))
```

输出：

```text
int
float
str
bool
array
```

`type()` 返回的是字符串，所以可以继续参与字符串操作，例如 `print("类型是 " + type(42))`。

> **注意**：类型标注使用 `int`、`flt`、`str`、`bol`、`void`。`type()` 返回的 `float`、`bool` 是类型名字符串，不是标注关键字。

### 整数

- 十进制写法，支持负号（作为一元运算符）；
- 不支持十六进制、八进制、二进制、科学计数法、下划线分隔；
- 64 位有符号，溢出会按二补码回绕（不要依赖这一点做数值计算）。

```dsc
print(0)
print(123456789)
print(-42)
```

整数除法会向零截断：

```dsc
print(7 / 2)
print(-7 / 2)
```

输出：

```text
3
-3
```

取模 `%` 只接受整数，结果的符号跟随被除数（与 C 一致）：

```dsc
print(7 % 3)
print(-7 % 3)
print(7 % -3)
```

输出：

```text
1
-1
1
```

### 浮点数

浮点数字面量必须在小数点两侧都有数字：`1.5` 合法，`.5` 和 `1.` 不合法（后者会被切成整数 `1` 和点号）。不支持科学计数法。

```dsc
print(3.14)
print(1.0 / 3.0)
print(0.1 + 0.2)
```

输出：

```text
3.14
0.3333333333333333
0.30000000000000004
```

最后一行不是 bug，而是二进制浮点数的正常表现。DSCBT 采用“最短可往返”的格式化方式，所以 `1.0` 会打印成 `1`，`3.1400000000000001` 会打印成 `3.14`。

整数与浮点混合运算时，结果是浮点数：

```dsc
print(1 + 2.5)
print(4 / 2.0)
```

输出：

```text
3.5
2
```

> **注意**：`%` 不接受浮点操作数。`5.5 % 2` 会触发运行时错误。

### 布尔值与真值判断

布尔字面量是 `True` 和 `False`，首字母大写。比较运算产生布尔值：

```dsc
print(3 < 5)
print(3 == 3.0)
print("abc" == "abc")
```

输出：

```text
True
True
True
```

在条件中，以下值视为假：

- `False`、`0`、`0.0`、空字符串 `""`、`nil`；
- 其他值视为真，包括非空字符串、数组和对象（即使数组为空 `[]`，作为对象引用也为真）。

```dsc
if 0:
    print("不会输出")
els:
    print("0 是假")

if "":
    print("不会输出")
els:
    print("空字符串是假")

if []:
    print("空数组仍然是真")
```

输出：

```text
0 是假
空字符串是假
空数组仍然是真
```

### 字符串

字符串是不可变的。所有“修改”都会产生新字符串：

```dsc
s = "hello"
t = s + " world"
print(s)
print(t)
```

输出：

```text
hello
hello world
```

`+` 只要有一侧是字符串，就会把另一侧格式化成文本再拼接：

```dsc
print("年龄: " + 18)
print(1 + " + 1 = 2")
print(True + "!")
```

输出：

```text
年龄: 18
1 + 1 = 2
True!
```

支持字符串比较：

```dsc
print("abc" == "abc")
print("abc" != "abd")
print("abc" < "abd")
```

输出：

```text
True
True
True
```

`len()` 返回字符串的字节数，不是字符数：

```dsc
print(len("abc"))
print(len("中文"))
```

输出：

```text
3
6
```

`"中文"` 每个汉字在 UTF-8 中占 3 字节，所以长度是 6。

> **注意**：DSCBT 不支持字符串索引（`s[0]`）、切片、大小写转换、查找等操作。字符串只能拼接、比较、求长度和打印。

### 数组

数组用方括号创建，元素可以是任意类型：

```dsc
a = [1, 2, 3]
b = ["a", 2.5, True, [4, 5]]
print(a)
print(b)
print(len(b))
```

输出：

```text
[1, 2, 3]
[a, 2.5, True, [4, 5]]
4
```

数组是固定长度的：不能追加、删除或扩容。越界访问会终止程序。详见第 6 章。

### 对象

对象由 `class` 定义，用 `ClassName { 字段: 值 }` 创建：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 1, y: 2 }
print(p.x)
print(p.y)
print(type(p))
```

输出：

```text
1
2
object
```

对象字段可以修改：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 1, y: 2 }
p.x = 100
print(p.x)
```

输出：

```text
100
```

### `nil`

`nil` 没有字面量。它出现在：

- 未赋值或只声明未初始化的全局变量；
- 类字面量中缺失且无法推断默认值的字段；
- 某些操作的默认结果。

`nil` 可以通过 `type()` 观察到：

```dsc
print(type(nope))
print(nope)
```

输出：

```text
nil
nil
```

不要把 `nil` 当成 0 使用。

## 3.3 函数

### 定义与调用

```dsc
def add(a, b):
    return a + b

print(add(3, 5))
```

输出：

```text
8
```

语法要点：

- `def` 开头，函数名后跟参数列表，冒号结束；
- 函数体必须缩进；
- `return` 返回值；没有 `return` 时函数返回 `0`；
- 调用时参数个数必须匹配，否则编译时报错。

```dsc
def greet(name):
    print("你好，" + name)

greet("DSC")
```

输出：

```text
你好，DSC
```

没有显式 `return`：

```dsc
def nothing():
    x = 1
    x = x + 1

print(nothing())
```

输出：

```text
0
```

### 参数类型标注是“注释”

可以写：

```dsc
def add(a: int, b: int):
    return a + b
```

但标注不会被执行器检查。下面同样可以运行：

```dsc
def add(a: int, b: int):
    return a + b

print(add("x", "y"))
```

输出：

```text
xy
```

类型标注只用于阅读和教学。类字段的标注稍微不同：它决定缺省值（int/bol 为 0，flt 为 0，str/类为 nil），但同样不做运行时检查。

### 前向引用

函数可以先调用、后定义：

```dsc
print(double(21))

def double(x):
    return x * 2
```

输出：

```text
42
```

编译器会先注册所有顶层函数，再编译函数体与顶层语句，因此前向调用是安全的。

### 递归

```dsc
def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)

print(fact(5))
```

输出：

```text
120
```

斐波那契：

```dsc
def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(10))
```

输出：

```text
55
```

DSCBT 的调用栈是动态扩容的，十万层递归也可以运行；但每层都有开销，深递归程序会变慢并占用内存。

### 函数不能嵌套定义

```dsc error
def outer():
    def inner():
        return 1
    return inner()
```

```text
错误 [2:5]: 函数/类定义必须位于顶层
```

DSCBT 没有闭包，也没有函数值。函数只能定义在顶层。

## 3.4 注释

前面已经介绍过：

```dsc
# 井号注释
// 双斜杠注释
x = 1  # 行尾注释
```

注释只到行尾，没有块注释。注释行不参与缩进判断，因此可以在任意缩进层级写注释。

## 3.5 控制流

### `if` / `els if` / `els`

```dsc
score = 85

if score >= 90:
    print("优秀")
els if score >= 80:
    print("良好")
els if score >= 60:
    print("及格")
els:
    print("不及格")
```

输出：

```text
良好
```

规则：

- 关键字是 `els` 和 `els if`，不是 `else` / `elif`；
- 每个分支头以冒号结束；
- `els if` 可以连续写多个；
- 分支体必须缩进；
- 单行写法也支持：`if x > 0: print("正数")`，但不推荐在复杂程序中使用。

### `while`

```dsc
i = 0
while i < 5:
    print(i)
    i = i + 1
```

输出：

```text
0
1
2
3
4
```

`while` 没有 `break` 和 `continue`。需要提前退出时，用一个布尔标记：

```dsc
arr = [3, 7, 12, 19]
target = 12
found = False
i = 0

while i < len(arr) and not found:
    if arr[i] == target:
        found = True
    els:
        i = i + 1

if found:
    print("下标:", i)
els:
    print("未找到")
```

输出：

```text
下标: 2
```

### `for ... in range(n)`

`for` 只支持 `range(n)` 形式，循环变量从 `0` 到 `n-1`：

```dsc
for i in range(5):
    print(i)
```

输出：

```text
0
1
2
3
4
```

`range` 的参数只接受一个表达式，且在进入循环前只求值一次：

```dsc
limit = 3
for i in range(limit):
    print(i)
    limit = 0
```

输出：

```text
0
1
2
```

循环变量每轮会被更新为当前计数值。**不要在循环体内依赖修改循环变量来改变迭代次数**；v3 会使用隐藏计数器保证迭代次数稳定。需要“跳过”某些值时，用 `if` 控制语句：

```dsc
for i in range(10):
    if i % 2 == 0:
        print(i)
```

输出：

```text
0
2
4
6
8
```

### 嵌套循环

```dsc
for i in range(3):
    for j in range(3):
        print(i, j)
```

输出：

```text
0 0
0 1
0 2
1 0
1 1
1 2
2 0
2 1
2 2
```

### 循环选择建议

- 已知次数，用 `for`；
- 依赖条件，用 `while`；
- 没有 `break/continue`，用标记变量或调整条件。

## 3.6 小结与练习

本章覆盖了语言的核心语法。请确认你能独立写出：

- 变量交换；
- 判断奇偶；
- 求 1 到 n 的和；
- 用递归求阶乘；
- 用 `while` 和 `for` 分别遍历。

练习：

1. 写一个函数 `is_even(n)`，返回 `True` 或 `False`。
2. 打印 1 到 100 中所有能被 3 或 5 整除的数。
3. 用递归实现 `gcd(a, b)`。
4. 用 `while` 实现 `for` 的等价循环。
5. 写一个函数，接收三个整数，返回最大值；不要使用内置的 `max`（DSCBT 也没有）。
6. 解释为什么下面代码输出 `12`：

```dsc
total = 0
def add(n):
    total = total + n
add(5)
add(7)
print(total)
```

---

# 第 4 章 值、引用与内存

本章对应《Rust 程序设计语言》第 4 章“认识所有权”。DSCBT 没有所有权、借用和生命周期检查，但它有自己的“值类型 / 引用类型”二分法。理解这一章，能避免 90% 的“为什么我的数组被改了”问题。

## 4.1 两类值

DSCBT 的运行时值可以分为两类：

| 类别 | 类型 | 赋值/传参行为 |
|---|---|---|
| 值类型 | `int`、`float`、`bool`、`str` | 赋值后互不影响 |
| 引用类型 | `array`、`object` | 两个变量指向同一个对象，互相影响 |

`nil` 视为不可变的空值。

字符串虽然内部用指针指向字符数据，但它是**不可变**的，所以行为上可以当成值类型：你无法原地修改字符串，只能创建新字符串。

## 4.2 值类型：复制

```dsc
a = 10
b = a
b = b + 1

print(a)
print(b)
```

输出：

```text
10
11
```

`b = a` 复制了整数值。修改 `b` 不会影响 `a`。浮点、布尔同理。

字符串：

```dsc
s = "abc"
t = s
t = t + "d"

print(s)
print(t)
```

输出：

```text
abc
abcd
```

`s` 没有被修改。`t + "d"` 创建了一个新字符串。

## 4.3 引用类型：别名

数组：

```dsc
a = [1, 2, 3]
b = a
b[0] = 99

print(a[0])
print(b[0])
print(a == b)
```

输出：

```text
99
99
True
```

`b = a` 没有复制数组，而是让 `b` 和 `a` 指向同一个数组。因此 `b[0] = 99` 也会改变 `a`。

对象同理：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 1, y: 2 }
q = p
q.x = 100

print(p.x)
print(q.x)
print(p == q)
```

输出：

```text
100
100
True
```

如果想得到独立副本，需要手动复制每个字段：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 100, y: 2 }
r = Point { x: p.x, y: p.y }
r.x = 200
print(p.x)
print(r.x)
```

输出：

```text
100
200
```

数组没有内置复制函数，需要手动逐元素复制：

```dsc
src = [1, 2, 3]
dst = [0, 0, 0]
for i in range(len(src)):
    dst[i] = src[i]
dst[0] = 9
print(src[0])
print(dst[0])
```

输出：

```text
1
9
```

## 4.4 函数参数与别名

值类型传参是复制：

```dsc
def add_one(n):
    n = n + 1
    return n

x = 10
print(add_one(x))
print(x)
```

输出：

```text
11
10
```

引用类型传参是共享：

```dsc
def bump_first(arr):
    arr[0] = arr[0] + 1

a = [1, 2, 3]
bump_first(a)
print(a[0])
```

输出：

```text
2
```

函数拿到的是数组引用，所以能修改调用者的数组。这是设计工具（例如排序函数）的常用手法，但也意味着函数可能有副作用。

> **注意**：对象字段如果保存的是数组或对象，复制对象时只会复制引用。例如：

```dsc
class Box:
    data:

a = [1, 2]
b = Box { data: a }
b.data[0] = 99
print(a[0])
```

输出：

```text
99
```

`data:` 省略类型标注时会按 `int` 处理缺省值，但字段值不做运行时类型检查，所以仍可保存数组。

## 4.5 生命周期与内存

DSCBT 没有垃圾回收器（GC）。内存管理的实际规则是：

1. 局部变量的栈空间随函数调用分配、返回释放；
2. 数组、对象、拼接产生的新字符串都在堆上分配；
3. 堆对象在程序运行期间**不会**被回收；
4. 进程退出时统一释放所有堆对象。

这意味着：

- 小程序不需要关心内存；
- 长时间运行、循环中不断创建字符串或对象的程序，内存会持续增长；
- 没有 `free`、没有析构函数、没有弱引用。

例如，下面的循环会创建 10 万个临时字符串：

```dsc
i = 0
while i < 100000:
    s = "a" + "b"
    i = i + 1
print("done")
```

每个 `"a" + "b"` 都会分配新字符串，直到进程结束才释放。结果正确，但内存占用持续上升。

> **对照 Rust**：Rust 用所有权和 `Drop` 在编译期决定释放时机；DSCBT 用“进程结束时统一释放”换取实现简单。写 DSCBT 程序时，应避免在热循环里无意义地创建大量临时对象。

## 4.6 相等性

`==` 的规则取决于类型：

- `int` / `float` / `bool`：按数值比较，`1 == 1.0` 为 `True`；
- `str`：按内容比较，`"abc" == "abc"` 为 `True`；
- `array` / `object`：按**同一性**比较，只有两个变量指向同一个对象时才为 `True`；
- `nil == nil` 为 `True`；
- 不同非数值类型之间为 `False`。

```dsc
a = [1, 2]
b = [1, 2]
c = a
print(a == b)
print(a == c)
print("1" == 1)
```

输出：

```text
False
True
False
```

## 4.7 小结与练习

本章要点：

- 值类型复制，引用类型共享；
- 函数可以修改传入的数组/对象；
- 字符串不可变，拼接产生新值；
- 堆对象没有 GC；
- `==` 对数组/对象是比较同一性。

练习：

1. 预测并验证：`a = [1]; b = a; b[0] = 2; print(a[0])`。
2. 写一个函数 `swap_first(arr)`，交换数组前两个元素，观察原数组。
3. 写一个函数 `clone_point(p)`，返回字段独立的新对象。
4. 解释下面的输出，并说明原因：

```dsc
class Box:
    data:

x = [1]
b = Box { data: x }
c = Box { data: x }
b.data[0] = 7
print(c.data[0])
```

5. 把第 2 章的猜数字程序改成“把每次猜测记录到数组”，观察数组在函数之间如何共享。

---

# 第 5 章 用类组织数据

本章对应《Rust 程序设计语言》第 5 章“使用结构体组织相关联的数据”。DSCBT 的 `class` 实际上更接近 Rust 的 `struct`：它只包含字段，没有方法、构造函数、继承和多态。把它当成“带字段的轻量对象”即可。

## 5.1 为什么需要类

当数据开始成组出现时，散落的变量会变得难以维护。例如表示一个点：

```dsc
x = 10
y = 20
```

如果程序里有十个点，就要维护二十个变量。用类可以把它们绑定成一个整体：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 10, y: 20 }
print(p.x)
print(p.y)
```

输出：

```text
10
20
```

## 5.2 定义类

语法：

```text
class 类名:
    字段名: 类型
    字段名: 类型
    ...
```

完整示例：

```dsc
class Rectangle:
    width: int
    height: int
    name: str
    area_ratio: flt
```

字段类型可以写：

- `int`、`flt`、`str`、`bol`（布尔）、`void`；
- 另一个类名；
- 或者省略（默认为 `int`）。

类型标注有三个作用：

1. 提高可读性；
2. 决定类字面量缺失字段时的默认值；
3. 为未来的类型检查预留信息。

它**不会**在运行时强制检查。下面代码完全合法：

```dsc
class Rectangle:
    width: int
    height: int

r = Rectangle { width: "很宽", height: [1, 2, 3] }
print(r.width)
print(r.height)
```

输出：

```text
很宽
[1, 2, 3]
```

## 5.3 创建对象

使用类名 + 花括号：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 1, y: 2 }
print(p.x)
print(p.y)
```

字段顺序可以任意：

```dsc
class Point:
    x: int
    y: int

p = Point { y: 20, x: 10 }
print(p.x)
print(p.y)
```

输出：

```text
10
20
```

缺失字段使用默认值：

| 字段类型 | 默认值 |
|---|---|
| `int` / `bol` | `0` |
| `flt` | `0` |
| `str` / 类类型 / `void` | `nil` |

```dsc
class Point:
    x: int
    y: int
    label: str

p = Point { x: 7 }
print(p.x)
print(p.y)
print(p.label)
```

输出：

```text
7
0
nil
```

出现未声明字段、重复字段时会编译失败：

```dsc error
class Point:
    x: int
    y: int

p = Point { x: 1, z: 2 }
```

```text
错误 [5:19]: 类型 Point 没有字段 z
```

```dsc error
class Point:
    x: int
    y: int

p = Point { x: 1, x: 2 }
```

```text
错误 [5:19]: 字段 x 重复初始化
```

> **注意**：类字面量必须写在一行内。花括号中的换行会被词法器当成语句结束，导致语法错误。

## 5.4 访问与修改字段

```dsc
class Point:
    x: int
    y: int

p = Point { x: 1, y: 2 }
print(p.x)

p.x = 100
p.y = p.y + 1

print(p.x)
print(p.y)
```

输出：

```text
1
100
3
```

字段可以保存任意类型，包括数组和对象：

```dsc
class Canvas:
    pixels: int
    title: str

class Scene:
    name: str
    canvas: Canvas

c = Canvas { pixels: 1024, title: "main" }
s = Scene { name: "demo", canvas: c }

print(s.name)
print(s.canvas.pixels)
print(s.canvas.title)
```

输出：

```text
demo
1024
main
```

## 5.5 嵌套对象

定义嵌套结构：

```dsc
class Point:
    x: int
    y: int

class Rect:
    tl: Point
    br: Point

r = Rect { tl: Point { x: 0, y: 0 }, br: Point { x: 10, y: 20 } }

print(r.tl.x)
print(r.br.y)
```

输出：

```text
0
20
```

> **注意**：类字面量不能跨行。长表达式可以先用变量组装：

```dsc
class Point:
    x: int
    y: int

class Rect:
    tl: Point
    br: Point

tl = Point { x: 0, y: 0 }
br = Point { x: 10, y: 20 }
r = Rect { tl: tl, br: br }
```

## 5.6 用函数操作对象

DSCBT 的类没有方法。习惯上把对象作为函数第一个参数：

```dsc
class Rect:
    w: int
    h: int

def area(r):
    return r.w * r.h

def scale(r, k):
    return Rect { w: r.w * k, h: r.h * k }

r = Rect { w: 3, h: 4 }
print(area(r))

big = scale(r, 2)
print(big.w)
print(big.h)
```

输出：

```text
12
6
8
```

可以写“构造函数”辅助函数：

```dsc
class Point:
    x: int
    y: int

def make_point(x, y):
    return Point { x: x, y: y }

p = make_point(5, 8)
print(p.x + p.y)
```

输出：

```text
13
```

DSCBT 不支持 `p.area()` 这样的方法调用，也不支持继承、接口、`this`/`self`。

## 5.7 对象数组

对象可以放进数组：

```dsc
class Point:
    x: int
    y: int

points = [Point { x: 0, y: 0 }, Point { x: 3, y: 4 }, Point { x: -2, y: 1 }]

for i in range(len(points)):
    print(points[i].x, points[i].y)
```

输出：

```text
0 0
3 4
-2 1
```

数组持有对象引用，所以修改 `points[0].x` 会影响原对象。

## 5.8 常见错误

### 忘记声明字段

```dsc error
class Point:
    x: int

p = Point { x: 1, y: 2 }
```

```text
错误 [4:15]: 类型 Point 没有字段 y
```

### 对非对象做字段访问

```dsc error
x = 10
print(x.value)
```

```text
错误 [2:0]: 字段访问要求对象，实际为 int
```

### 使用未定义的类型

```dsc error
p = Point { x: 1 }
```

```text
错误 [1:5]: 未定义的类型 Point
```

### 在类字面量中换行

```dsc error
class Point:
    x: int
    y: int

p = Point {
    x: 1,
    y: 2
}
```

这会报 `意外的记号` 一类的语法错误。所有字段写在同一个花括号行内。

## 5.9 小结与练习

本章要点：

- `class` 定义字段，字段标注决定默认值；
- 类字面量按字段名初始化，顺序无关；
- 对象是引用类型；
- 没有方法、构造函数、继承；
- 用普通函数操作对象。

练习：

1. 定义 `Book` 类（`title: str`、`price: flt`、`pages: int`），创建两本书并打印总价。
2. 定义 `Point` 和 `Segment`（两个端点），写函数 `length_sq(seg)` 返回两点距离的平方。
3. 定义 `Student`（`name: str`、`score: int`），写函数 `is_pass(s)`。
4. 写一个函数 `clone_point(p)`，返回字段独立的新点。
5. 用对象数组表示三个矩形，写函数计算它们的总面积。

---

# 第 6 章 数组与字符串

本章对应《Rust 程序设计语言》第 8 章“常见集合”，但 DSCBT 的集合能力要弱得多：数组长度固定，字符串不可变且没有索引。掌握这些边界，是写出可靠程序的前提。

## 6.1 创建与访问数组

```dsc
arr = [10, 20, 30, 40, 50]

print(arr[0])
print(arr[2])
print(arr[4])
print(len(arr))
```

输出：

```text
10
30
50
5
```

下标从 `0` 开始，必须是整数。读取和修改都使用方括号：

```dsc
arr = [10, 20, 30, 40, 50]
arr[1] = 99
print(arr[1])
```

输出：

```text
99
```

## 6.2 固定长度与越界

数组长度在字面量写出时就固定了。不能追加、删除、扩容：

```text
arr = [1, 2, 3]
arr[3] = 4        # 越界，运行时错误
```

```dsc error
arr = [1, 2, 3]
print(arr[3])
```

```text
错误 [2:0]: 数组下标越界: 3 (长度 3)
```

退出码为 3。负数下标也不支持：

```dsc error
arr = [1, 2, 3]
print(arr[-1])
```

```text
错误 [2:0]: 数组下标越界: -1 (长度 3)
```

> **注意**：DSCBT 没有“负索引表示倒数”的规则。

## 6.3 混合类型与嵌套数组

数组元素可以是任意类型：

```dsc
data = [1, "two", 3.5, True, [4, 5]]
print(data)
print(data[1])
print(data[4][0])
```

输出：

```text
[1, two, 3.5, True, [4, 5]]
two
4
```

二维数组：

```dsc
matrix = [[1, 2, 3], [4, 5, 6]]

print(matrix[0][1])
matrix[1][2] = 99
print(matrix[1][2])
```

输出：

```text
2
99
```

> **注意**：数组字面量也不能跨行。上面的 `matrix` 如果按常见语言格式换行写，会报语法错误；必须写成一行，或分成多个变量再组合。

## 6.4 遍历

按下标遍历：

```dsc
arr = [5, 6, 7]
for i in range(len(arr)):
    print(i, arr[i])
```

输出：

```text
0 5
1 6
2 7
```

`for ... in range(n)` 只遍历下标，不直接遍历元素。数组没有 `for x in arr` 形式。

`while` 遍历：

```dsc
arr = [5, 6, 7]
i = 0
while i < len(arr):
    print(arr[i])
    i = i + 1
```

输出：

```text
5
6
7
```

## 6.5 固定容量缓冲区模式

既然数组不能扩容，需要动态数量时，可以一次性创建足够大的数组，并维护一个“已用长度”：

```dsc
CAP = 8
data = [0, 0, 0, 0, 0, 0, 0, 0]
count = 0

# push 三个值
data[count] = 11
count = count + 1
data[count] = 22
count = count + 1
data[count] = 33
count = count + 1

for i in range(count):
    print(data[i])
```

输出：

```text
11
22
33
```

封装成函数：

```dsc
CAP = 8
data = [0, 0, 0, 0, 0, 0, 0, 0]
count = 0

def push(value):
    count = count + 1
    if count <= CAP:
        data[count - 1] = value
    els:
        count = CAP
        print("缓冲区已满")

push(11)
push(22)
print(count)
for i in range(count):
    print(data[i])
```

输出：

```text
2
11
22
```

`push` 可以修改全局的 `count` 和 `data`，因为顶层变量是模块全局变量，数组本身也是引用。

## 6.6 常见数组算法

### 求最大值

先看一个常见错误：以为 `range` 像 Python 一样支持起始值。

```dsc error
arr = [3, 9, 2, 7, 5]
best = arr[0]
for i in range(1, len(arr)):
    if arr[i] > best:
        best = arr[i]
print(best)
```

```text
错误 [3:17]: 期望 )，实际为 ,
```

`range` 只接受一个参数。正确写法是从 `0` 开始遍历：

```dsc
arr = [3, 9, 2, 7, 5]
best = arr[0]
for i in range(len(arr)):
    if arr[i] > best:
        best = arr[i]
print(best)
```

输出：

```text
9
```

也可以从 `1` 开始，但要用下标变量：

```dsc
arr = [3, 9, 2, 7, 5]
best = arr[0]
for k in range(len(arr) - 1):
    i = k + 1
    if arr[i] > best:
        best = arr[i]
print(best)
```

输出同样是 `9`。

### 原地反转

```dsc
arr = [1, 2, 3, 4, 5]
i = 0
j = len(arr) - 1

while i < j:
    temp = arr[i]
    arr[i] = arr[j]
    arr[j] = temp
    i = i + 1
    j = j - 1

print(arr)
```

输出：

```text
[5, 4, 3, 2, 1]
```

### 冒泡排序

```dsc
arr = [64, 34, 25, 12, 22, 11, 90]
n = len(arr)

for i in range(n):
    for j in range(n - i - 1):
        if arr[j] > arr[j + 1]:
            temp = arr[j]
            arr[j] = arr[j + 1]
            arr[j + 1] = temp

print(arr)
```

输出：

```text
[11, 12, 22, 25, 34, 64, 90]
```

### 二分查找

要求数组升序：

```dsc
arr = [11, 12, 22, 25, 34, 64, 90]
target = 25

lo = 0
hi = len(arr) - 1
pos = -1

while lo <= hi:
    mid = (lo + hi) / 2
    if arr[mid] == target:
        pos = mid
        lo = hi + 1
    els if arr[mid] < target:
        lo = mid + 1
    els:
        hi = mid - 1

print(pos)
```

输出：

```text
3
```

循环中没有 `break`，所以用 `lo = hi + 1` 强制结束。

## 6.7 字符串

字符串的规则比数组更少：

- 可以拼接：`"a" + "b"`；
- 可以比较：`==`、`!=`、`<`、`>`、`<=`、`>=`；
- 可以求字节长度：`len(s)`；
- 可以打印；
- 不能索引、切片、修改、查找、替换、大小写转换。

```dsc
s = "Hello"
print(len(s))
print(s + ", DSC")
print(s == "Hello")
```

输出：

```text
5
Hello, DSC
True
```

字符串索引会报错：

```dsc error
s = "abc"
print(s[0])
```

```text
错误 [2:0]: 下标访问要求数组，实际为 str
```

`len` 返回字节数：

```dsc
print(len("a"))
print(len("中"))
```

输出：

```text
1
3
```

## 6.8 常见错误

### 数组字面量换行

```dsc error
arr = [
    1,
    2
]
```

DSCBT 的换行是语句分隔符，数组字面量必须在一行内写完。

### 忘记 len 减一

```dsc error
arr = [1, 2, 3]
for i in range(len(arr) + 1):
    print(arr[i])
```

最后一次循环会越界，退出码 3。

### 以为 `range(a, b)` 可用

```dsc error
for i in range(1, 5):
    print(i)
```

`range` 只接受一个表达式。需要从 1 开始时，用 `i = k + 1`。

### 字符串当数组用

`s[0]`、`s.length` 都不存在。只能 `len(s)`。

## 6.9 小结与练习

要点：

- 数组定长、可修改元素、下标从 0 开始；
- 越界是运行时错误；
- 数组字面量必须单行；
- 字符串不可变、按内容比较、按字节计长度；
- 没有动态数组和字符串方法。

练习：

1. 写一个函数求数组元素之和。
2. 写一个函数在数组中查找目标值，返回下标或 `-1`。
3. 实现选择排序。
4. 思考题：DSCBT 没有“取第 i 个字符”的函数，能否判断字符串是否为回文？如果不能，需要给语言增加什么内置函数？
5. 用固定容量数组实现一个最多存 10 个整数的栈（`push` / `pop` 用函数和全局 `count` 模拟）。

---

# 第 7 章 函数、递归与程序结构

本章把函数从“语法”提升到“工程”：如何分解问题、如何设计参数和返回值、如何用递归解决自相似问题，以及在没有模块系统的情况下组织单文件程序。

## 7.1 分解问题

一个函数只做一件事。比较下面两种写法：

```dsc
# 不推荐：所有逻辑堆在一起
nums = [3, 1, 4, 1, 5]
total = 0
for i in range(len(nums)):
    total = total + nums[i]
avg = total / len(nums)
print(avg)
```

```dsc
# 推荐：拆成可复用函数
def array_sum(a):
    total = 0
    for i in range(len(a)):
        total = total + a[i]
    return total

def array_avg(a):
    return array_sum(a) / len(a)

nums = [3, 1, 4, 1, 5]
print(array_avg(nums))
```

输出：

```text
2
```

拆分的收益：

- 每个函数可以单独测试；
- 名字表达意图；
- 修改实现时不影响调用者。

## 7.2 参数与返回值

DSCBT 只有“按值传递”一种语法，但引用类型会共享对象（见第 4 章）。

```dsc
def distance_sq(x1, y1, x2, y2):
    dx = x2 - x1
    dy = y2 - y1
    return dx * dx + dy * dy

print(distance_sq(0, 0, 3, 4))
```

输出：

```text
25
```

一次只能返回一个值。需要返回多个值时，用数组或对象：

```dsc
def min_max(a):
    lo = a[0]
    hi = a[0]
    for i in range(len(a)):
        if a[i] < lo:
            lo = a[i]
        if a[i] > hi:
            hi = a[i]
    return [lo, hi]

result = min_max([4, 1, 9, 3])
print(result[0])
print(result[1])
```

输出：

```text
1
9
```

或者定义类：

```dsc
class MinMax:
    lo: int
    hi: int

def min_max_obj(a):
    lo = a[0]
    hi = a[0]
    for i in range(len(a)):
        if a[i] < lo:
            lo = a[i]
        if a[i] > hi:
            hi = a[i]
    return MinMax { lo: lo, hi: hi }

r = min_max_obj([4, 1, 9, 3])
print(r.lo)
print(r.hi)
```

## 7.3 递归

递归函数必须满足：

1. 有一个“基本情况”直接返回；
2. 每次递归都向基本情况靠近。

### 阶乘

```dsc
def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)

print(fact(6))
```

输出：

```text
720
```

### 斐波那契

```dsc
def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(15))
```

输出：

```text
610
```

这个实现的时间复杂度是指数级。n 稍大就会明显变慢，教学时可用它演示“递归不一定高效”。

### 最大公约数

```dsc
def gcd(a, b):
    if b == 0:
        return a
    return gcd(b, a % b)

print(gcd(48, 18))
```

输出：

```text
6
```

### 汉诺塔

```dsc
def hanoi(n, from_rod, to_rod, aux_rod):
    if n == 0:
        return 0
    hanoi(n - 1, from_rod, aux_rod, to_rod)
    print(from_rod + " -> " + to_rod)
    hanoi(n - 1, aux_rod, to_rod, from_rod)
    return 0

hanoi(3, "A", "C", "B")
```

输出：

```text
A -> C
A -> B
C -> B
A -> C
B -> A
B -> C
A -> C
```

### 互递归

两个函数可以互相调用，因为所有顶层函数会先注册：

```dsc
def is_even(n):
    if n == 0:
        return True
    return is_odd(n - 1)

def is_odd(n):
    if n == 0:
        return False
    return is_even(n - 1)

print(is_even(10))
print(is_odd(7))
```

输出：

```text
True
True
```

> **注意**：互递归必须有终止条件，否则会耗尽内存。

## 7.4 递归的代价

每次函数调用都会创建栈帧，包含返回地址、局部变量槽和操作数栈空间。DSCBT 的栈帧是动态扩容的，所以十万层递归不会像旧版一样崩溃，但：

- 每层都有内存和时间开销；
- 递归深度受可用内存限制；
- 同样的逻辑常常可以用循环写成 O(1) 空间。

比较递归与迭代：

```dsc
def sum_rec(n):
    if n == 0:
        return 0
    return n + sum_rec(n - 1)

def sum_iter(n):
    total = 0
    for i in range(n):
        total = total + i + 1
    return total

print(sum_rec(100))
print(sum_iter(100))
```

输出：

```text
5050
5050
```

## 7.5 单文件程序的组织

DSCBT 没有模块和导入，所有代码在一个 `.dscbt` 文件里。推荐结构：

```dsc
# 1. 常量
MAX_SIZE = 100

# 2. 类定义
class Item:
    name: str
    price: int

# 3. 工具函数
def total_price(items):
    total = 0
    for i in range(len(items)):
        total = total + items[i].price
    return total

# 4. 主流程
items = [Item { name: "pen", price: 3 }, Item { name: "book", price: 20 }]
print(total_price(items))
```

输出：

```text
23
```

函数定义顺序不影响调用（前向引用），但建议“常量 → 类 → 工具函数 → 主流程”，可读性最好。

## 7.6 没有的东西

- 没有函数重载；
- 没有默认参数；
- 没有可变参数（`print` 是特例）；
- 没有匿名函数/闭包；
- 没有函数作为值传递；
- 没有模块、包、导入；
- 没有测试框架（可以用断言式 `print` 和脚本对比输出）。

## 7.7 小结与练习

练习：

1. 写 `power(base, exp)` 的递归和迭代两个版本。
2. 用递归求数组元素之和（提示：把下标作为参数）。
3. 写 `ackermann(m, n)` 的小规模版本，观察递归深度。
4. 把第 6 章的冒泡排序拆成 `swap`、`bubble_pass`、`sort` 三个函数。
5. 设计一个 `Student` 类和一组函数，计算平均分、最高分、及格人数。

---

# 第 8 章 错误处理与调试

本章对应《Rust 程序设计语言》第 9 章“错误处理”。区别很大：Rust 有 `Result`、`Option`、`panic!` 和 `?`；DSCBT 没有异常，也没有可恢复的错误类型。DSCBT 的错误策略是：

- 编译错误：在执行前发现，退出码 2；
- 运行时错误：立即终止程序，退出码 3；
- 没有 `try/catch`，没有 `throw`，没有 `Result`。

这听起来严格，但也让错误定位非常直接：错误信息会给出源代码行号，运行时错误还会指出具体操作。

## 8.1 两类错误

### 编译错误

在词法、语法或编译阶段发现。程序一行都不会执行。

```dsc error
x = 1
if x > 0
    print(x)
```

```text
错误 [2:9]: 期望 :，实际为 NL
```

### 运行时错误

程序已经启动，执行到某条指令时失败。

```dsc error
a = [1, 2]
print(a[5])
```

```text
错误 [2:0]: 数组下标越界: 5 (长度 2)
```

退出码为 3。

## 8.2 常见错误速查

| 错误现象 | 常见原因 | 修复方式 |
|---|---|---|
| `期望 :，实际为 NL` | `if/while/for/def/class` 头部缺少冒号 | 补 `:` |
| `意外的记号 INDENT` | 缩进不一致，或不该缩进的地方缩进 | 对齐到同列，统一空格/制表符 |
| `意外的记号 :` | 把 `els` 写成 `else` 或 `elif` | 改成 `els` / `els if` |
| `字符串未闭合` | 双引号没有配对 | 补全引号；字符串不能跨行 |
| `未定义的函数 xxx` | 函数名拼错，或函数定义在别的作用域 | 检查拼写；函数只能定义在顶层 |
| `函数 xxx 期望 N 个参数` | 调用参数数量不对 | 对齐定义与调用 |
| `数组下标越界` | 下标为负或 `>= len` | 访问前检查边界 |
| `整数除以零` | 除数为 0 | 先判断除数 |
| `不支持的操作数类型: nil + int` | 使用了未初始化变量 | 先赋值；用 `type()` 检查 |
| `下标访问要求数组` | 对字符串/整数用了 `[]` | 字符串不支持索引 |
| `字段访问要求对象` | 对非对象用了 `.` | 检查变量类型 |
| `类型 X 没有字段 y` | 字段名拼错，或类定义没包含该字段 | 修正字段名/类定义 |
| `函数/类定义必须位于顶层` | 在 `def` 或 `if` 内定义函数/类 | 移到顶层 |
| `return 不能出现在顶层` | 顶层直接写 `return` | 把逻辑放进函数 |

## 8.3 调试的基本方法

### 方法一：打印中间值

DSCBT 没有调试器，`print` 是最可靠的工具：

```dsc
def average(a):
    total = 0
    for i in range(len(a)):
        total = total + a[i]
        print("i =", i, "total =", total)
    return total / len(a)

print(average([2, 4, 6]))
```

输出：

```text
i = 0 total = 2
i = 1 total = 6
i = 2 total = 12
4
```

### 方法二：检查类型

```dsc
x = input("数字: ")
print(type(x))
```

`input` 总是返回 `int`。如果你期望字符串，需要重新设计程序——DSCBT 没有读取字符串的输入函数。

### 方法三：缩小问题

把大程序切成最小可复现片段。例如排序结果不对，先打印排序前的数组，再打印每一轮交换后的数组：

```dsc
arr = [3, 1, 2]
for i in range(len(arr)):
    for j in range(len(arr) - i - 1):
        if arr[j] > arr[j + 1]:
            temp = arr[j]
            arr[j] = arr[j + 1]
            arr[j + 1] = temp
    print("第", i, "轮:", arr)
```

输出：

```text
第 0 轮: [1, 2, 3]
第 1 轮: [1, 2, 3]
第 2 轮: [1, 2, 3]
```

### 方法四：从错误行号定位

错误信息 `[行:列]` 直接指向源代码。运行时错误的列号通常为 0，因为字节码只保留行号；编译错误会给出更精确的列。

## 8.4 防御式编程

### 安全访问数组

```dsc
def get_or(a, index, fallback):
    if index >= 0 and index < len(a):
        return a[index]
    return fallback

arr = [10, 20, 30]
print(get_or(arr, 1, -1))
print(get_or(arr, 9, -1))
```

输出：

```text
20
-1
```

### 安全除法

```dsc
def safe_div(a, b):
    if b == 0:
        print("错误：除数不能为 0")
        return 0
    return a / b

print(safe_div(10, 2))
print(safe_div(10, 0))
```

输出：

```text
5
错误：除数不能为 0
0
```

### 初始化所有变量

```dsc
count = 0
best = 0
name = ""
flag = False
```

不要依赖“默认是 0”。未初始化全局变量是 `nil`，参与算术会报错。

### 校验输入

由于 `input` 只返回整数，遇到非数字会得到 `0`。如果业务上 0 不合法，可以把它当错误：

```dsc
n = input("请输入正数: ")
if n <= 0:
    print("输入无效，按 1 处理")
    n = 1
```

## 8.5 错误处理的边界

- 运行时错误会终止整个程序，无法捕获；
- 没有异常栈，只有出错行号；
- 不会自动回滚已产生的输出；
- 数组/对象的部分修改可能已经发生。

因此，**在修改共享数据前做校验**：

```dsc
def set_score(scores, i, value):
    if i >= 0 and i < len(scores):
        scores[i] = value
        return True
    return False
```

## 8.6 小结与练习

练习：

1. 写一个函数 `parse_positive(n)`，当 `n <= 0` 时打印错误并返回 1。
2. 写一个函数 `find(arr, target)`，返回下标或 `-1`。
3. 故意让冒泡排序访问 `arr[j + 1]` 越界，观察错误行号并修复。
4. 用 `print` 追踪递归函数的参数和返回值。
5. 把一个会除零的程序改成安全版本。

---

# 第 9 章 项目实战：成绩管理与排序

本章把前面所有知识组合起来，完成一个可运行的小系统：管理学生成绩，计算统计量，并按分数排序。这是《Rust 程序设计语言》第 12 章“一个 I/O 项目”的对应实践，只是 DSCBT 没有文件 I/O 和命令行参数解析，我们直接使用内存数据。

## 9.1 需求

程序需要：

1. 表示若干学生（姓名、分数）；
2. 计算平均分；
3. 找出最高分学生；
4. 统计及格人数（`>= 60`）；
5. 按分数从高到低排序；
6. 打印所有学生。

## 9.2 数据模型

```dsc
class Student:
    name: str
    score: int
```

学生数组：

```dsc
class Student:
    name: str
    score: int

students = [Student { name: "Alice", score: 92 }, Student { name: "Bob", score: 58 }, Student { name: "Cara", score: 76 }, Student { name: "Dan", score: 45 }]
```

> **注意**：数组字面量必须写在一行。学生较多时，可以先逐个创建变量，再组装数组：`s1 = Student { name: "Alice", score: 92 }`、`s2 = Student { name: "Bob", score: 58 }`、`students = [s1, s2]`。

## 9.3 函数设计

- `average(students)`：返回平均分（整数除法）；
- `highest(students)`：返回分数最高的学生对象；
- `pass_count(students)`：返回及格人数；
- `sort_by_score(students)`：原地按分数降序排序；
- `print_all(students)`：逐行打印。

这些函数都不返回值以外的输出（除了 `print_all`），便于复用。

## 9.4 完整程序

```dsc
class Student:
    name: str
    score: int

def average(students):
    total = 0
    for i in range(len(students)):
        total = total + students[i].score
    return total / len(students)

def highest(students):
    best = students[0]
    for i in range(len(students)):
        if students[i].score > best.score:
            best = students[i]
    return best

def pass_count(students):
    count = 0
    for i in range(len(students)):
        if students[i].score >= 60:
            count = count + 1
    return count

def sort_by_score(students):
    n = len(students)
    for i in range(n):
        for j in range(n - i - 1):
            if students[j].score < students[j + 1].score:
                temp = students[j]
                students[j] = students[j + 1]
                students[j + 1] = temp

def print_all(students):
    for i in range(len(students)):
        print(students[i].name, students[i].score)

students = [Student { name: "Alice", score: 92 }, Student { name: "Bob", score: 58 }, Student { name: "Cara", score: 76 }, Student { name: "Dan", score: 45 }]

print("平均分:", average(students))
print("最高分:", highest(students).name, highest(students).score)
print("及格人数:", pass_count(students))

sort_by_score(students)
print("排序后:")
print_all(students)
```

输出：

```text
平均分: 67
最高分: Alice 92
及格人数: 2
排序后:
Alice 92
Cara 76
Bob 58
Dan 45
```

## 9.5 代码讲解

### 平均分为什么是 67

总分是 `92 + 58 + 76 + 45 = 271`，人数 4。`271 / 4` 是整数除法，结果是 `67`，不是 `67.75`。如果需要小数，把总分转成浮点——但 DSCBT 没有类型转换函数，可以写成 `total * 1.0 / len(students)`：

```dsc
print(271 * 1.0 / 4)
```

输出：

```text
67.75
```

### 排序交换的是对象引用

`temp = students[j]` 复制的是对象引用，不是对象本身。交换后数组持有相同的对象，只是顺序变化。这没有问题，因为我们只关心顺序，不关心复制。

### 最高分函数返回对象

`highest` 返回 `Student` 对象，调用者可以继续访问 `.name` 和 `.score`。这是“返回引用”的典型用法。

### 边界情况

如果学生数组为空，`highest` 中的 `students[0]` 会越界；平均分也会除以 0。真实程序应先判断：

```dsc
def average_safe(scores):
    if len(scores) == 0:
        return 0
    total = 0
    for i in range(len(scores)):
        total = total + scores[i]
    return total / len(scores)

print(average_safe([90, 100]))
print(average_safe([]))
```

输出：

```text
95
0
```

## 9.6 测试与验证

教学项目没有测试框架，可以用“固定输入 + 人工核对输出”的方式：

```dsc
def assert_equal(actual, expected, label):
    if actual == expected:
        print("PASS", label)
    els:
        print("FAIL", label, "actual =", actual, "expected =", expected)

assert_equal(2 + 3, 5, "add")
assert_equal(10 / 2, 5, "div")
```

输出：

```text
PASS add
PASS div
```

## 9.7 扩展练习

1. 增加 `rank` 字段，排序后填入名次（1、2、3……）。
2. 增加按姓名查找学生的函数（字符串 `==` 比较）。
3. 统计各分数段人数：`<60`、`60-69`、`70-79`、`80-89`、`90-100`。
4. 增加“最高分并列”处理：返回所有最高分学生。
5. 把排序改成升序，并思考如何只改一个比较符号。
6. 增加一个 `print_report` 函数，输出格式化的成绩单。

---

# 第 10 章 性能与字节码原理

本章解释 DSCBT 为什么这样设计，以及怎样写出更快的 DSCBT 程序。它对应《Rust 程序设计语言》中“零成本抽象”和性能相关讨论，但 DSCBT 是解释器，性能模型完全不同。

## 10.1 从源文件到指令

复习第 1 章的流程：

```text
源码 → Token → AST → 字节码 → VM 执行
```

真正运行的是字节码，而不是 AST。DSCBT v3 的指令是固定 8 字节：

```text
[ arg:32 ][ line:16 ][ op:8 ][ aux:8 ]
```

- `op`：操作码，例如 `OP_ADD`、`OP_LOAD`、`OP_CALL`；
- `arg`：操作数，例如局部变量槽号、常量池下标、跳转目标；
- `line`：源代码行号，用于运行时错误；
- `aux`：附加信息，例如参数个数、全局/局部标志。

固定长度让取指简单，8 字节让一个缓存行能放 8 条指令。

## 10.2 栈式虚拟机

DSCBT 是栈式 VM。表达式通过操作数栈求值：

```dsc
a = 1 + 2
```

概念上的指令序列：

```text
PUSH_1
PUSH_2
ADD
STORE a
```

函数调用使用栈帧：

- 局部变量位于当前帧的固定槽位；
- 参数从操作数栈直接成为被调函数的局部槽；
- 调用返回时把结果压回调用者的操作数栈；
- 帧数组动态扩容，所以深递归不会因为固定帧数而崩溃。

## 10.3 编译器会自动做的优化

写自然、清晰的代码即可，编译器会做：

1. **常量折叠**：`1 + 2 * 3` 在编译期算出 `7`；
2. **比较 + 跳转融合**：`if a < b` 直接编译成“比较并条件跳转”；
3. **and/or 短路融合**：去掉临时的布尔值；
4. **复合赋值超指令**：`x = x + 1` 编译成一条自增指令；
5. **循环自增**：`for` 的计数更新只占一条指令；
6. **字段复合赋值**：`p.x = p.x + 1` 用一条字段更新指令；
7. **数组累加**：`sum = sum + arr[i]` 用一条数组累加指令；
8. **死代码消除**：`return` 之后的同块代码不会生成；
9. **常量条件裁剪**：`if True:` 只保留对应分支。

因此，不要为了“优化”把代码写得更难读；先写清楚，再根据实际瓶颈调整。

## 10.4 性能建议

### 优先使用整数

整数运算走快路径；浮点运算需要类型提升和双精度计算。计数器、下标、分数等尽量用整数。

### 避免在热循环中拼接字符串

每次 `+` 都会分配新字符串，而且 DSCBT 没有 GC，旧字符串会保留到进程结束。

```dsc
# 不推荐：10 万次分配
i = 0
while i < 100000:
    s = "item" + i
    i = i + 1
```

如果只是打印，直接 `print("item", i)`，不要构造中间字符串。

### 避免在热循环中创建对象或数组

```dsc
class Point:
    x: int
    y: int

# 不推荐：循环中反复分配
for i in range(1000000):
    p = Point { x: i, y: i }
```

每次循环都会分配一个对象，内存持续增长。可以复用同一个对象并修改字段：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 0, y: 0 }
for i in range(1000000):
    p.x = i
    p.y = i
```

### 缓存重复计算

```dsc
# 每次循环都调用 len
arr = [1, 2, 3]
i = 0
while i < len(arr):
    print(arr[i])
    i = i + 1
```

`len(arr)` 很快，但如果循环体复杂，缓存到变量更清晰：

```dsc
arr = [1, 2, 3]
n = len(arr)
i = 0
while i < n:
    print(arr[i])
    i = i + 1
```

### 用函数局部变量

函数内部的局部变量通过槽位直接访问；顶层变量是模块全局槽，同样很快。真正的区别在于可读性和避免全局状态。热循环中的临时变量优先用局部变量。

### 递归要有终止条件

深递归不会崩溃，但会持续占用栈空间。能用循环解决的问题优先用循环。

## 10.5 测量性能

Linux / macOS：

```text
time ./dsc fib.dscbt
```

Windows PowerShell：

```text
Measure-Command { .\dsc.exe fib.dscbt }
```

基准程序示例：

```dsc
def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(30))
```

在开发 v3 时，同一台机器上的参考数据（仅供量级参考）：

| 基准 | v2 | v3 |
|---|---:|---:|
| `fib(30)` | 约 143 ms | 约 63 ms |
| 1000 万次 while 循环 | 约 351 ms | 约 155 ms |
| 1000 万次数组访问 | 约 433 ms | 约 219 ms |
| 20 万次字符串拼接 | 约 30 ms | 约 9 ms |

实际时间取决于 CPU、编译器和系统负载。

## 10.6 内存行为

- 数组和对象在堆上分配；
- 拼接字符串在堆上分配；
- 没有 GC，进程结束才统一释放；
- 局部变量本身在栈帧中，函数返回即消失，但它引用的堆对象不会消失。

所以“局部变量离开作用域”不等于“对象被释放”。长生命周期程序要控制对象创建数量。

## 10.7 小结与练习

练习：

1. 用 `time` 测量 `fib(20)`、`fib(25)`、`fib(30)`，观察增长趋势。
2. 把第 9 章的平均分函数改成直接调用 `len(students)`，比较可读性。
3. 找出一个在循环中重复创建数组的代码，并改成复用。
4. 估算：如果每次字符串拼接分配 20 字节，10 万次大约占用多少内存？
5. 阅读 `dsc.c` 中 `vm_run` 的 `OP_ADD` 和 `OP_CALL` 分支，理解快路径。

---

# 第 11 章 与 Rust、Python 对照

本章帮助有 Rust 或 Python 经验的读者快速迁移。DSCBT 是一门教学用的小语言，不试图替代任何生产语言。

## 11.1 与 Rust 的关键差异

| 主题 | Rust | DSCBT |
|---|---|---|
| 类型系统 | 静态、强类型、编译期检查 | 动态、运行时类型 |
| 变量声明 | `let` / `let mut` | 直接赋值 |
| 可变性 | 默认不可变，`mut` 显式 | 变量天然可变 |
| 内存管理 | 所有权、借用、生命周期、`Drop` | 无 GC，进程结束时统一释放 |
| 错误处理 | `Result`、`Option`、`panic!`、`?` | 编译错误/运行时错误直接终止 |
| 结构体 | `struct` + `impl` 方法 | `class` 仅字段，函数操作对象 |
| 枚举/模式匹配 | 支持 | 不支持 |
| 集合 | `Vec`、`HashMap`、`String` 等 | 固定长度数组、不可变字符串 |
| 模块系统 | `mod`、`use`、Cargo | 无，单文件 |
| 泛型/Trait | 支持 | 不支持 |
| 闭包/迭代器 | 支持 | 不支持 |
| 并发 | 线程、`Send`/`Sync` | 不支持 |
| 构建 | `cargo build` | `gcc -O2 -o dsc dsc.c` |

## 11.2 与 Python 的关键差异

| 主题 | Python | DSCBT |
|---|---|---|
| 代码块 | 缩进 | 缩进 |
| 条件分支 | `if` / `elif` / `else` | `if` / `els if` / `els` |
| 循环 | `for x in iterable` | 仅 `for i in range(n)` |
| 列表 | 动态、可追加 | 固定长度、元素可改 |
| 字符串 | 可索引、切片、方法 | 不可索引，仅拼接/比较/len |
| 函数 | 支持闭包、默认参数、`*args` | 无闭包、无默认参数 |
| 类 | 方法、继承、构造器 | 仅字段，无方法/继承 |
| 异常 | `try/except` | 无异常，错误终止 |
| 输入 | `input()` 返回字符串 | `input()` 返回整数 |
| 全局变量 | 函数内赋值默认局部 | 对已存在的全局名赋值会修改全局 |
| GC | 引用计数/垃圾回收 | 无 GC，进程结束释放 |

## 11.3 同一程序的三种写法

### 阶乘

Rust：

```text
fn fact(n: u64) -> u64 {
    if n <= 1 { 1 } else { n * fact(n - 1) }
}
```

Python：

```text
def fact(n):
    return 1 if n <= 1 else n * fact(n - 1)
```

DSCBT：

```dsc
def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)
```

### 结构体 / 类

Rust：

```text
struct Point { x: i64, y: i64 }
let p = Point { x: 1, y: 2 };
println!("{}", p.x);
```

DSCBT：

```dsc
class Point:
    x: int
    y: int

p = Point { x: 1, y: 2 }
print(p.x)
```

### 循环

Python：

```text
for i in range(5):
    print(i)
```

DSCBT：

```dsc
for i in range(5):
    print(i)
```

## 11.4 迁移建议

- 从 Python 过来：先记住 `els` / `els if`，不要用列表方法、异常和字符串索引；数组长度固定。
- 从 Rust 过来：不要寻找所有权和借用；`class` 只是结构体；错误不可恢复；没有泛型和 Trait。
- 把 DSCBT 当成“可执行的伪代码”或教学语言，而不是生产系统。

## 11.5 练习

1. 把一段 Python 的 `for x in list:` 改写成 DSCBT 的下标循环。
2. 把一段 Rust 的 `match` 改写成 `if / els if / els`。
3. 用 DSCBT 实现一个 Rust `struct` + 函数的等价程序。
4. 列出三个 Python 有、DSCBT 没有的功能，并说明如何绕开。

---

# 第 12 章 限制、下一步与学习路线

## 12.1 DSCBT 的能力边界

**支持：**

- 动态类型；
- 整数、浮点、布尔、字符串；
- 固定长度数组（元素可为任意类型）；
- 类（字段对象）与嵌套对象；
- 函数、递归、前向引用；
- `if / els if / els`、`while`、`for range`；
- 模块级全局变量与函数局部变量；
- 运行时错误定位；
- 编译期常量折叠、超指令等优化。

**不支持：**

- 模块、包、导入；
- 动态数组/列表方法；
- 字符串索引、切片和方法；
- 类方法、构造器、继承、多态；
- 枚举、模式匹配、泛型、Trait；
- 闭包、函数值、匿名函数；
- 异常、`try/catch`、可恢复错误；
- 线程、异步、I/O 文件读写；
- `break` / `continue`；
- 多行数组/类字面量、多行表达式；
- 字符串输入（`input` 只返回整数）；
- 类型转换函数；
- 垃圾回收；
- 标准库（数学、随机、时间等）。

## 12.2 常见变通方法

| 需求 | 变通 |
|---|---|
| 动态数组 | 预分配固定容量 + `count` 变量 |
| 读取字符串 | 当前无法实现；只能读整数 |
| 随机数 | 线性同余等伪随机公式 |
| `break` | 条件变量 + 循环条件 |
| 多返回值 | 返回数组或对象 |
| 字符串遍历 | 当前无法实现；需要扩展语言 |
| 数学函数 | 自己用算术实现（如平方、取模） |
| 模块化 | 单文件内按“常量→类→函数→主流程”组织 |

## 12.3 如果继续扩展解释器

阅读 `dsc.c` 可以按以下顺序入手：

1. 词法器：关键字、Token、缩进；
2. 解析器：AST 节点与递归下降；
3. 编译器：常量池、槽位分配、字节码发射；
4. 虚拟机：栈、帧、全局槽、操作码分派；
5. 运行时：值类型、数组、对象、输出缓冲。

可以尝试的扩展项目：

- 增加 `str_at(s, i)` 和字符串长度按字符计数；
- 增加动态数组（`push`/`pop`）；
- 增加 `break` / `continue`；
- 增加 `elif` / `else` 别名；
- 增加文件 I/O 和 `args()`；
- 增加 `sqrt`、`abs`、`min`、`max` 等内置函数；
- 增加模块/导入；
- 增加可选类型检查（利用现有字段标注）。

每次扩展都要同步更新教程、README 和测试。

## 12.4 学习路线（对照 Rust 教程）

如果你喜欢《Rust 程序设计语言》的结构，可以按以下路线继续：

1. **基础语法 → 写小游戏**：本教程第 1–3 章；
2. **数据建模 → 写结构体/类项目**：第 4–6 章；
3. **错误处理 → 写健壮程序**：第 8 章；
4. **项目实战 → 成绩管理**：第 9 章；
5. **性能与实现 → 阅读解释器源码**：第 10 章；
6. **跨语言对照 → 学习 Rust 所有权**：第 11 章；
7. **扩展语言 → 编译原理进阶**：第 12 章。

## 12.5 教学建议

- 第一次课：构建 + Hello World + 猜数字；
- 第二次课：变量、类型、函数、控制流；
- 第三次课：值与引用、数组、类；
- 第四次课：错误处理 + 小项目；
- 第五次课：性能与字节码，带学生读 `vm_run`；
- 作业：每个项目都要求“先写测试输出，再实现”。

## 12.6 结语

DSCBT 很小，但它包含了真实语言的核心环节：词法、语法、字节码、栈帧、作用域、错误定位。把本教程的练习做完，你不仅能使用 DSCBT，也能更清楚地理解 Python、Lua、Rust 等语言为什么这样设计。

---

# 附录 A 语法速查

## A.1 程序结构

```text
# 注释
x = 1                 # 赋值
print(x)              # 表达式语句

if x > 0:             # 条件
    print("正")
els if x == 0:
    print("零")
els:
    print("负")

while x > 0:          # 循环
    x = x - 1

for i in range(3):    # 计数循环 0..2
    print(i)

def add(a, b):        # 函数
    return a + b

class Point:          # 类
    x: int
    y: int

p = Point { x: 1, y: 2 }
```

## A.2 运算符优先级（从高到低）

| 优先级 | 运算符 | 说明 |
|---|---|---|
| 1 | `not`、一元 `-` | 逻辑非、取负 |
| 2 | `*`、`/`、`%` | 乘、除、取模 |
| 3 | `+`、`-` | 加、减、字符串拼接 |
| 4 | `<`、`>`、`<=`、`>=` | 比较 |
| 5 | `==`、`!=` | 相等 |
| 6 | `and` | 逻辑与（短路） |
| 7 | `or` | 逻辑或（短路） |

`and` 的别名是 `&`，`or` 的别名是 `|`，`not` 的别名是 `!`。

## A.3 字面量

| 类型 | 示例 | 说明 |
|---|---|---|
| 整数 | `0`、`42`、`-7` | 十进制 |
| 浮点 | `3.14`、`-0.5` | 小数点两侧都要有数字 |
| 字符串 | `"hello"` | 双引号；支持 `\n` `\t` `\r` `\\` `\"` |
| 布尔 | `True`、`False` | 首字母大写 |
| 数组 | `[1, 2, 3]` | 必须单行 |
| 类实例 | `Point { x: 1, y: 2 }` | 必须单行 |

## A.4 语句限制

- 每条语句一行，不能跨行；
- 代码块用缩进，通常 4 个空格；
- 不能在同一行写多条语句（没有分号）；
- 数组/类字面量不能跨行；
- 没有 `break`、`continue`、`switch`、`try`。

---

# 附录 B 关键字、运算符与内置函数

## B.1 关键字

| 关键字 | 用途 |
|---|---|
| `True` / `False` | 布尔字面量 |
| `if` / `els if` / `els` | 条件分支 |
| `while` | 条件循环 |
| `for` / `in` / `range` | 计数循环 |
| `def` | 函数定义 |
| `return` | 函数返回 |
| `class` | 类定义 |
| `and` / `or` / `not` | 逻辑运算 |
| `int` / `flt` / `str` / `bol` / `void` | 类型标注 |

## B.2 内置函数

| 函数 | 说明 | 示例 |
|---|---|---|
| `print(...)` | 打印任意数量参数，空格分隔，换行 | `print("x", 1)` |
| `input(prompt)` | 打印提示，读取一行并解析为整数 | `n = input("n: ")` |
| `len(x)` | 数组/字符串/对象的长度 | `len([1, 2])` → `2` |
| `type(x)` | 返回类型名字符串 | `type(1.5)` → `"float"` |

没有其他内置函数。

## B.3 转义序列

`\n` 换行、`\t` 制表符、`\r` 回车、`\\` 反斜杠、`\"` 双引号。

---

# 附录 C 错误码与诊断

## C.1 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 用法错误/文件读取失败 |
| 2 | 词法、语法或编译错误 |
| 3 | 运行时错误 |
| 4 | 内存不足 |

## C.2 诊断格式

编译错误：

```text
错误 [行:列]: 消息
```

运行时错误：

```text
错误 [行:0]: 消息
```

运行时错误只保留行号，所以列号为 0。编译错误会尽量给出精确列号。

## C.3 常见消息对照

| 消息 | 含义 |
|---|---|
| `表达式意外结束` | 表达式不完整 |
| `期望 X，实际为 Y` | 语法结构不符合预期 |
| `未定义的函数 xxx` | 调用了不存在的函数 |
| `函数 xxx 期望 N 个参数` | 实参与形参数量不匹配 |
| `数组下标越界` | 下标为负或超出长度 |
| `整数除以零` / `整数取模零` | 除数为 0 |
| `下标访问要求数组` | 对非数组使用 `[]` |
| `字段访问要求对象` | 对非对象使用 `.` |
| `类型 X 没有字段 y` | 字段未声明或拼写错误 |
| `未定义的类型 X` | 类字面量引用了不存在的类 |
| `函数/类定义必须位于顶层` | 嵌套定义 |
| `return 不能出现在顶层` | 顶层使用 `return` |
| `缩进不一致` | 空格/制表符混用或缩进层级错误 |

---

# 附录 D 练习参考答案（精选）

## D.1 第 1 章：小票

```dsc
print("=== 小票 ===")
print("苹果\t3\t9")
print("牛奶\t1\t12")
print("合计\t\t21")
```

输出：

```text
=== 小票 ===
苹果	3	9
牛奶	1	12
合计		21
```

## D.2 第 3 章：偶数与最大值

```dsc
def is_even(n):
    return n % 2 == 0

def max3(a, b, c):
    best = a
    if b > best:
        best = b
    if c > best:
        best = c
    return best

print(is_even(10))
print(is_even(7))
print(max3(3, 9, 2))
```

输出：

```text
True
False
9
```

## D.3 第 4 章：克隆对象

```dsc
class Point:
    x: int
    y: int

def clone_point(p):
    return Point { x: p.x, y: p.y }

a = Point { x: 1, y: 2 }
b = clone_point(a)
b.x = 100

print(a.x)
print(b.x)
```

输出：

```text
1
100
```

## D.4 第 5 章：图书总价

```dsc
class Book:
    title: str
    price: flt
    pages: int

def total_price(books):
    total = 0.0
    for i in range(len(books)):
        total = total + books[i].price
    return total

books = [Book { title: "A", price: 12.5, pages: 100 }, Book { title: "B", price: 30.0, pages: 200 }]
print(total_price(books))
```

输出：

```text
42.5
```

## D.5 第 6 章：求和、查找与选择排序

```dsc
def array_sum(a):
    total = 0
    for i in range(len(a)):
        total = total + a[i]
    return total

def find(a, target):
    for i in range(len(a)):
        if a[i] == target:
            return i
    return -1

def selection_sort(a):
    n = len(a)
    for i in range(n):
        m = i
        for k in range(n - i - 1):
            j = i + k + 1
            if a[j] < a[m]:
                m = j
        temp = a[i]
        a[i] = a[m]
        a[m] = temp

arr = [5, 2, 9, 1]
print(array_sum(arr))
print(find(arr, 9))
selection_sort(arr)
print(arr)
```

输出：

```text
17
2
[1, 2, 5, 9]
```

## D.6 第 7 章：递归与迭代求幂

```dsc
def power_rec(base, exp):
    if exp == 0:
        return 1
    return base * power_rec(base, exp - 1)

def power_iter(base, exp):
    result = 1
    for i in range(exp):
        result = result * base
    return result

print(power_rec(2, 10))
print(power_iter(2, 10))
```

输出：

```text
1024
1024
```

## D.7 第 8 章：安全查找

```dsc
def find(arr, target):
    for i in range(len(arr)):
        if arr[i] == target:
            return i
    return -1

def safe_div(a, b):
    if b == 0:
        print("除数不能为 0")
        return 0
    return a / b

print(find([4, 8, 15], 8))
print(find([4, 8, 15], 7))
print(safe_div(9, 3))
print(safe_div(9, 0))
```

输出：

```text
1
-1
3
除数不能为 0
0
```

---

# 附录 E v2 → v3 行为变化

如果你从旧版（v2）迁移，请特别注意以下变化。

## E.1 输出更干净

v2 会在标准输出打印：

```text
信息: parsing...
信息: compiling...
信息: running...
```

v3 不再打印这些内容。依赖完整输出做断言的脚本需要更新。

## E.2 函数可以读取顶层变量

v2 中，函数读取顶层变量会得到 `nil`。v3 在编译前预注册模块全局变量，因此：

```dsc
x = 10
def f():
    return x
print(f())
```

v3 输出 `10`。依赖旧行为（得到 `nil`）的程序需要调整。

## E.3 `input()` 提示符修复

v2 的 `input("提示")` 存在栈泄漏，实际显示的提示可能不是传入的字符串。v3 正确打印提示，并保持操作数栈平衡。

## E.4 `print` 支持多参数

```dsc
print("a", 1, True)
```

v2 只打印第一个参数，v3 打印全部参数，空格分隔。

## E.5 数组可以保存任意类型

v2 的数组内部只存 `int64`，浮点会被截断，字符串会变成 0。v3 的数组元素是完整的动态值，可以保存字符串、浮点、布尔、嵌套数组和对象。

## E.6 字符串比较与 `len`

- v3 支持字符串字典序比较；
- v3 的 `len` 支持字符串（字节数）和对象（字段数）；
- v2 的行为不完整或会崩溃。

## E.7 更安全的运行时

v3 不再因为以下情况直接崩溃：

- 除以零：给出错误并退出码 3；
- 数组越界：给出错误；
- 深递归：动态扩容，不再受 64 帧限制；
- 调用未定义函数：编译期报错；
- 对非对象/非数组使用 `.` 或 `[]`：明确报错。

## E.8 更严格的值语义

- `nil + 1` 现在报错，而不是用未定义的内存值；
- 数组/对象相等是比较同一性；
- `1 == 1.0` 为 `True`；
- 布尔参与算术按 0/1 处理。

## E.9 循环变量

`for` 循环的迭代次数由隐藏计数器保证。在循环体内修改循环变量不会改变迭代次数。这是 v3 修正后的可预测行为。

## E.10 性能

v3 重写了词法器、编译器与虚拟机，典型基准提升约 2 到 3.5 倍。具体数据见第 10 章。

## E.11 命令行

新增：

```text
dsc --help
dsc --version
```

## E.12 错误信息

v3 的错误格式统一为：

```text
错误 [行:列]: 消息
```

运行时错误列号为 0。退出码区分用法错误、编译错误、运行时错误和内存不足。

---

## 结语

到这里，你已经从“Hello, World!”走到了字节码和一个小项目。DSCBT 的设计目标不是取代成熟语言，而是把编程语言的核心机制压缩到一个可以完整阅读的实现里。

建议的下一步：

1. 把第 9 章的项目扩展成你自己的领域模型；
2. 阅读 `dsc.c`，对照第 10 章理解每条指令；
3. 尝试给语言增加一个内置函数或一种语法；
4. 再回到《Rust 程序设计语言》，对照所有权、Trait 和错误处理，理解不同语言的设计取舍。

祝学习顺利。


