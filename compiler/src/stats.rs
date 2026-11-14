use std::collections::BTreeMap;

#[derive(Default)]
pub struct Stats {
    pub counts: BTreeMap<&'static str, u64>,
}

impl Stats {
    pub fn bump(&mut self, name: &'static str) {
        *self.counts.entry(name).or_insert(0) += 1;
    }
}

/// Catalogue of the v4 optimization pipeline.  Entries in the first two
/// groups are implemented directly in this crate; the last group is provided
/// by Cranelift's `opt_level=speed` pipeline, which the code generator uses.
pub const OPTIMIZATION_CATALOG: &[&str] = &[
    // --- 前端 / AST ---
    "UTF-8 BOM 识别与跳过",
    "字符串字面量驻留（相同字面量只发射一次）",
    "类字段布局静态化（偏移预计算）",
    "全局变量槽静态分配",
    "编译期类字段存在性检查",
    "对象字面量字段顺序静态化",
    "常量折叠：二元算术/比较/逻辑",
    "常量折叠：一元 not/neg",
    "常量折叠：字符串拼接",
    "常量折叠：len(字符串/数组)",
    "常量折叠：type(常量)",
    "常量条件 if 消除",
    "常量条件 while 消除",
    "range(0) 空循环消除",
    "小循环完全展开（常量 trip count）",
    "return 后死代码删除",
    "空块删除",
    "空 if 分支删除",
    "代数恒等：x+0 / x-0 / x*1 / x/1",
    "代数化简：x*0",
    "代数化简：0-x 取负",
    "强度削减：x*2 → x+x",
    "自比较化简：x==x / x!=x",
    "双重否定消除",
    "双重取负消除",
    "and/or 自吸收（x and x → x）",
    "类型事实传播（局部）",
    "分支类型事实合并",
    "循环内变量类型事实失效",
    "纯表达式语句删除",
    // --- 代码生成 ---
    "局部变量提升为 SSA 变量",
    "全局变量直接寻址（基址+偏移）",
    "BSS 零初始化全局区",
    "只读字符串段",
    "静态类字段偏移访问",
    "字段访问快速路径（内联类检查）",
    "字段写入快速路径（内联类检查）",
    "字段复合赋值融合（rt_fop_at）",
    "数组字面量常数长度分配",
    "数组下标快速路径（内联边界检查）",
    "数组写入快速路径（内联边界检查）",
    "整数加法内联快速路径",
    "整数减法内联快速路径",
    "整数乘法内联快速路径",
    "整数除法内联快速路径（零/溢出保护）",
    "整数取模内联快速路径（零保护）",
    "整数取负内联快速路径",
    "整数相等比较快速路径",
    "整数大小比较快速路径",
    "比较结果直接进入分支（不物化布尔值）",
    "and/or 短路分支生成",
    "真值判断内联（int/bool 快路径）",
    "函数参数寄存器直传（≤8 参数）",
    "零参数函数调用特化",
    "返回值槽前传（return f(...)）",
    "调用参数与临时槽栈式复用",
    "常量整数立即数编码",
    "print 常量整数特化",
    "print 常量布尔特化",
    "print 常量浮点特化",
    "print 常量字符串特化（零格式化）",
    "print 单参数特化",
    "print 零参数特化",
    "print 多参数先求值后输出（语义保持）",
    "函数级 section（链接期死函数回收）",
    // --- 运行时 ---
    "字符串长度头部缓存（运行时）",
    "字符串指针快速相等检查（运行时）",
    "缓冲输出（64 KiB）",
    "整数格式化快速路径（运行时）",
    "最短 %g 浮点格式化（运行时）",
    "无 libc 运行时（直接系统调用/Win32）",
    // --- Cranelift 后端 ---
    "指令选择",
    "寄存器分配",
    "SSA 构造与 Phi 消除",
    "e-graph 重写优化",
    "全局值编号（GVN）",
    "循环不变量外提（LICM）",
    "死代码消除（DCE）",
    "常量折叠（IR）",
    "冗余分支消除",
    "基本块合并",
    "栈槽着色",
    "复制合并",
    "指令调度",
    "窥孔优化",
    "分支松弛/块布局",
    "调用约定降低",
    "栈帧布局优化",
    "地址模式折叠（LEA）",
    "乘法强度削减（Cranelift）",
    "寄存器压力感知溢出",
];

pub fn catalog_len() -> usize {
    OPTIMIZATION_CATALOG.len()
}
