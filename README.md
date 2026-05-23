# DSC 编程语言教程

## 一、语言简介

DSC 是一门**解释型**编程语言，语法类似 Python，内置字节码虚拟机直接执行。不需要外部 C 编译器。

**设计理念：**
- 语法简洁，缩进定义代码块（Python 风格）
- 动态类型
- 内置字节码虚拟机，开箱即用
- 图灵完备（支持条件、循环、递归函数）

**语言得名：** DSCBT = DS Code Basic Test
---

## 二、构建与使用

### 构建

```bash
gcc -O2 -o dsc dsc.c
```

### 使用

```bash
dsc 源文件.dscbt
```

---

## 三、基础语法

### 3.1 注释

```
// 这是单行注释
# 这也是单行注释
```

### 3.2 变量

直接赋值，类型自动推断：

```
x = 10              # 整数
y = 3.14            # 浮点数
name = "你好"        # 字符串
flag = True         # 布尔值
```

变量可以被重新赋值：

```
x = 10
x = 20              # x 现在是 20
x = x + 5           # x 现在是 25
```

### 3.3 数据类型

| 关键字  | 说明              | 示例       |
|---------|-------------------|------------|
| True    | 布尔真            | `True`     |
| False   | 布尔假            | `False`    |

类型通过内置函数 `type()` 查看。

### 3.4 输出与输入

```
# 输出
print("你好，世界")
print(42)
print(3.14)
print(True)

# 输入整数
age = input("请输入年龄: ")
print(age)
```

### 3.5 运算符

**算术运算符：**

```
sum = a + b         # 加法
diff = a - b        # 减法
prod = a * b        # 乘法
quot = a / b        # 除法
rem = a % b         # 取模（求余）
```

字符串也可以使用 `+` 拼接：

```
greeting = "你好, " + "世界"
print(greeting)     # 输出: 你好, 世界
```

**比较运算符：**

```
a == b              # 等于
a != b              # 不等于
a < b               # 小于
a > b               # 大于
a <= b              # 小于等于
a >= b              # 大于等于
```

**逻辑运算符：**

```
a and b             # 逻辑与 (AND)（短路求值）
a or b              # 逻辑或 (OR)（短路求值）
not a               # 逻辑非 (NOT)
```

**优先级（从高到低）：**
1. `not` `-`（一元）
2. `*` `/` `%`
3. `+` `-`
4. `<` `>` `<=` `>=`
5. `==` `!=`
6. `and`
7. `or`

---

## 四、控制流

### 4.1 条件语句

```
if 条件:
    语句...
els if 条件:
    语句...
els:
    语句...
```

示例：

```
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

### 4.2 While 循环

```
while 条件:
    语句...
```

示例：

```
i = 0
while i < 10:
    print(i)
    i = i + 1
```

### 4.3 For 循环

`for ... in range(n):` 循环从 0 到 n-1：

```
for i in range(10):
    print(i)
```

等价于 Python 的 `for i in range(10):`。

---

## 五、函数

### 5.1 函数定义

```
def 函数名(参数1, 参数2, ...):
    语句...
    return 返回值
```

示例：

```
def add(a, b):
    return a + b

def max_val(a, b):
    if a > b:
        return a
    return b

print(add(3, 5))      # 输出: 8
print(max_val(10, 20))  # 输出: 20
```

### 5.2 递归

DSC 支持递归函数调用：

```
def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)

print(fact(5))         # 输出: 120
```

```
def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(10))        # 输出: 55
```

如果没有 `return` 语句，函数默认返回 0。

---

## 六、结构体

使用 `class` 关键字定义结构体类型：

```
class Point:
    x: int
    y: int

p = Point { x: 10, y: 20 }
print(p.x)            # 输出: 10
print(p.y)            # 输出: 20

# 修改字段
p.x = 100
print(p.x)            # 输出: 100
```

支持嵌套结构体：

```
class Rect:
    tl: Point
    br: Point

r = Rect { tl: Point { x: 0, y: 0 }, br: Point { x: 10, y: 10 } }
print(r.tl.x)         # 输出: 0
print(r.br.y)         # 输出: 10
```

---

## 七、数组

### 7.1 创建与访问

```
arr = [10, 20, 30, 40, 50]

print(arr[0])         # 输出: 10
print(arr[2])         # 输出: 30

arr[1] = 99
print(arr[1])         # 输出: 99
```

### 7.2 数组长度

使用 `len()` 内置函数：

```
arr = [1, 2, 3, 4, 5]
print(len(arr))       # 输出: 5
```

---

## 八、内置函数

| 函数                | 说明           | 示例                      |
|---------------------|----------------|---------------------------|
| `print(x)`          | 打印值并换行   | `print("hello")`          |
| `input("提示")`     | 读取整数输入   | `x = input("Enter: ")`    |
| `len(arr)`          | 获取数组长度   | `len([1, 2, 3])` → 3      |
| `type(x)`           | 获取类型名称   | `type(42)` → "int"        |

---

## 九、完整示例

### 示例 1：Hello World

```
print("你好，世界！")
print("欢迎使用 DSC 编程语言")
```

### 示例 2：计算 1 到 100 的和

```
sum = 0
i = 1
while i <= 100:
    sum = sum + i
    i = i + 1
print(sum)            # 输出: 5050
```

### 示例 3：判断质数

```
def is_prime(n):
    if n <= 1:
        return False
    i = 2
    while i * i <= n:
        if n % i == 0:
            return False
        i = i + 1
    return True

# 打印 1 到 50 的质数
num = 2
while num <= 50:
    if is_prime(num):
        print(num)
    num = num + 1
```

### 示例 4：冒泡排序

```
arr = [64, 34, 25, 12, 22, 11, 90]
n = len(arr)

for i in range(n):
    for j in range(n - i - 1):
        if arr[j] > arr[j + 1]:
            temp = arr[j]
            arr[j] = arr[j + 1]
            arr[j + 1] = temp

# 打印结果
k = 0
while k < n:
    print(arr[k])
    k = k + 1
```

### 示例 5：欧几里得算法求最大公约数

```
def gcd(a, b):
    while b != 0:
        temp = b
        b = a % b
        a = temp
    return a

print(gcd(48, 18))   # 输出: 6
print(gcd(100, 25))  # 输出: 25
```

---

## 十、语法参考

### 关键字一览

| 关键字  | 用途             | 示例                           |
|---------|------------------|--------------------------------|
| def     | 函数定义         | `def foo(a, b):`               |
| return  | 函数返回         | `return x + 1`                 |
| class   | 结构体定义       | `class Point:`                 |
| if      | 条件判断         | `if x > 0:`                    |
| els     | 条件分支         | `els:` 或 `els if x > 5:`     |
| while   | while 循环       | `while x > 0:`                 |
| for     | for 循环         | `for i in range(10):`          |
| in      | 成员遍历         | `for x in range(n):`           |
| range   | 范围生成         | `range(10)`                    |
| and     | 逻辑与           | `a and b`                      |
| or      | 逻辑或           | `a or b`                       |
| not     | 逻辑非           | `not a`                       |
| True    | 布尔真           | `flag = True`                  |
| False   | 布尔假           | `flag = False`                 |

### 内置函数

| 函数    | 用途             |
|---------|------------------|
| print   | 打印输出         |
| input   | 读取输入         |
| len     | 数组长度         |
| type    | 获取类型名称     |

### 特殊符号

| 符号 | 用途               |
|------|--------------------|
| `.`  | 字段访问（`p.x`）  |
| `[]` | 数组索引           |
| `{}` | 结构体字面量       |

---

## 十一、与旧版 v1 的语法对照

| 旧语法 (v1)                                | 新语法 (v2)                          |
|--------------------------------------------|--------------------------------------|
| `prt("hello")`                             | `print("hello")`                     |
| `inp("prompt")`                            | `input("prompt")`                    |
| `let x = 5`                                | `x = 5`                              |
| `fn add(a, b):`                            | `def add(a, b):`                     |
| `ret x`                                    | `return x`                           |
| `whl cond:`                                | `while cond:`                        |
| `tru` / `fal`                              | `True` / `False`                     |
| `a & b` / `a \| b`                         | `a and b` / `a or b`                |
| `typ Point:`                               | `class Point:`                       |
| `for let i=0; i<n; i=i+1:`                 | `for i in range(n):`                |
| `#arr`                                     | `len(arr)`                           |

---

## 十二、限制

- 没有垃圾回收（仅程序退出时清理堆内存）
- 没有字符串转义（部分支持）
- 没有模块/导入系统
- 数组是固定大小的（编译时确定）
- 没有泛型
- 没有浮点数组/结构体字段的完整支持
- 没有错误恢复机制

### 未来永远不可能添加

- 字符串类型改进
- 标准库函数
- 模块系统
- 更好的错误信息
- 调试信息支持
- 指针类型

---

感谢使用 DSCBT 编程语言！

*此处往上为AI言论，以下为人类言论：

本质上就是个c的套壳，搞了50min搞出这么一坨出来
