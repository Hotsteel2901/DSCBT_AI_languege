use std::collections::{HashMap, HashSet};

use crate::ast::*;
use crate::diag::{CResult, CompileError, Span};
use crate::sem::ModuleInfo;
use crate::stats::Stats;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ty {
    Unknown,
    Int,
    Float,
    Str,
    Bool,
    Arr,
    Obj,
}

#[derive(Debug, Clone)]
enum K {
    Int(i64),
    Float(f64),
    Str(Vec<u8>),
    Bool(bool),
}

impl K {
    fn truthy(&self) -> bool {
        match self {
            K::Int(v) => *v != 0,
            K::Float(v) => *v != 0.0,
            K::Str(v) => !v.is_empty(),
            K::Bool(v) => *v,
        }
    }
}

fn expr_of(k: &K, span: Span) -> Expr {
    match k {
        K::Int(v) => Expr {
            kind: ExprKind::Int(*v),
            span,
        },
        K::Float(v) => Expr {
            kind: ExprKind::Float(*v),
            span,
        },
        K::Str(v) => Expr {
            kind: ExprKind::Str(v.clone()),
            span,
        },
        K::Bool(v) => Expr {
            kind: ExprKind::Bool(*v),
            span,
        },
    }
}

fn const_of(e: &Expr) -> Option<K> {
    match &e.kind {
        ExprKind::Int(v) => Some(K::Int(*v)),
        ExprKind::Float(v) => Some(K::Float(*v)),
        ExprKind::Str(v) => Some(K::Str(v.clone())),
        ExprKind::Bool(v) => Some(K::Bool(*v)),
        _ => None,
    }
}

fn is_intlike(k: &K) -> bool {
    matches!(k, K::Int(_) | K::Bool(_))
}

fn is_num(k: &K) -> bool {
    matches!(k, K::Int(_) | K::Bool(_) | K::Float(_))
}

fn kint(k: &K) -> i64 {
    match k {
        K::Int(v) => *v,
        K::Bool(v) => *v as i64,
        _ => 0,
    }
}

fn kdbl(k: &K) -> f64 {
    match k {
        K::Float(v) => *v,
        _ => kint(k) as f64,
    }
}

fn fold_bin(op: BinOp, a: &K, b: &K, span: Span) -> CResult<Option<K>> {
    use BinOp::*;
    match op {
        And => return Ok(Some(if a.truthy() { b.clone() } else { a.clone() })),
        Or => return Ok(Some(if a.truthy() { a.clone() } else { b.clone() })),
        Add if matches!(a, K::Str(_)) || matches!(b, K::Str(_)) => {
            if let (K::Str(x), K::Str(y)) = (a, b) {
                let mut v = x.clone();
                v.extend_from_slice(y);
                return Ok(Some(K::Str(v)));
            }
            return Ok(None);
        }
        Eq | Ne => {
            let eq = if is_num(a) && is_num(b) {
                if is_intlike(a) && is_intlike(b) {
                    kint(a) == kint(b)
                } else {
                    kdbl(a) == kdbl(b)
                }
            } else {
                match (a, b) {
                    (K::Str(x), K::Str(y)) => x == y,
                    _ => false,
                }
            };
            return Ok(Some(K::Bool(if op == Eq { eq } else { !eq })));
        }
        Lt | Gt | Le | Ge => {
            let r = if is_num(a) && is_num(b) {
                if is_intlike(a) && is_intlike(b) {
                    let (x, y) = (kint(a), kint(b));
                    match op {
                        Lt => x < y,
                        Gt => x > y,
                        Le => x <= y,
                        _ => x >= y,
                    }
                } else {
                    let (x, y) = (kdbl(a), kdbl(b));
                    match op {
                        Lt => x < y,
                        Gt => x > y,
                        Le => x <= y,
                        _ => x >= y,
                    }
                }
            } else if let (K::Str(x), K::Str(y)) = (a, b) {
                match op {
                    Lt => x < y,
                    Gt => x > y,
                    Le => x <= y,
                    _ => x >= y,
                }
            } else {
                return Ok(None);
            };
            return Ok(Some(K::Bool(r)));
        }
        _ => {}
    }

    if !(is_num(a) && is_num(b)) {
        return Ok(None);
    }
    match op {
        Add | Sub | Mul => {
            if is_intlike(a) && is_intlike(b) {
                let (x, y) = (kint(a), kint(b));
                let v = match op {
                    Add => x.wrapping_add(y),
                    Sub => x.wrapping_sub(y),
                    _ => x.wrapping_mul(y),
                };
                Ok(Some(K::Int(v)))
            } else {
                let (x, y) = (kdbl(a), kdbl(b));
                Ok(Some(K::Float(match op {
                    Add => x + y,
                    Sub => x - y,
                    _ => x * y,
                })))
            }
        }
        Div => {
            if is_intlike(a) && is_intlike(b) {
                let (x, y) = (kint(a), kint(b));
                if y == 0 {
                    return Err(CompileError::at(span, "常量表达式除以零"));
                }
                if x == i64::MIN && y == -1 {
                    return Err(CompileError::at(span, "常量表达式整数溢出"));
                }
                Ok(Some(K::Int(x / y)))
            } else {
                let (x, y) = (kdbl(a), kdbl(b));
                Ok(Some(K::Float(x / y)))
            }
        }
        Mod => {
            if !(is_intlike(a) && is_intlike(b)) {
                return Ok(None);
            }
            let (x, y) = (kint(a), kint(b));
            if y == 0 {
                return Err(CompileError::at(span, "常量表达式取模零"));
            }
            Ok(Some(K::Int(if x == i64::MIN && y == -1 {
                0
            } else {
                x % y
            })))
        }
        _ => Ok(None),
    }
}

fn fold_un(op: UnOp, a: &K) -> Option<K> {
    match op {
        UnOp::Not => Some(K::Bool(!a.truthy())),
        UnOp::Neg => match a {
            K::Int(v) => Some(K::Int(v.wrapping_neg())),
            K::Bool(v) => Some(K::Int(-(*v as i64))),
            K::Float(v) => Some(K::Float(-*v)),
            K::Str(_) => None,
        },
    }
}

fn is_safe_int_expr(e: &Expr, env: &Env) -> bool {
    match &e.kind {
        ExprKind::Int(_) | ExprKind::Bool(_) => true,
        ExprKind::Ident(name) => matches!(env.ty(name), Ty::Int | Ty::Bool),
        ExprKind::Un(UnOp::Neg, a) => is_safe_int_expr(a, env),
        ExprKind::Bin(op, a, b) => {
            matches!(op, BinOp::Add | BinOp::Sub | BinOp::Mul)
                && is_safe_int_expr(a, env)
                && is_safe_int_expr(b, env)
        }
        _ => false,
    }
}

fn is_pure(e: &Expr) -> bool {
    match &e.kind {
        ExprKind::Int(_)
        | ExprKind::Float(_)
        | ExprKind::Str(_)
        | ExprKind::Bool(_)
        | ExprKind::Ident(_) => true,
        ExprKind::Un(_, a) => is_pure(a),
        ExprKind::Bin(_, a, b) => is_pure(a) && is_pure(b),
        ExprKind::Index(a, b) => is_pure(a) && is_pure(b),
        ExprKind::Field(a, _) => is_pure(a),
        _ => false,
    }
}

#[derive(Clone)]
struct Env {
    vars: HashMap<String, Ty>,
    globals: HashSet<String>,
    locals: HashSet<String>,
    in_function: bool,
}

impl Env {
    fn new(globals: &HashSet<String>) -> Self {
        Env {
            vars: HashMap::new(),
            globals: globals.clone(),
            locals: HashSet::new(),
            in_function: false,
        }
    }

    fn ty(&self, name: &str) -> Ty {
        self.vars.get(name).copied().unwrap_or(Ty::Unknown)
    }

    fn set(&mut self, name: &str, ty: Ty) {
        if ty == Ty::Unknown {
            self.vars.remove(name);
        } else {
            self.vars.insert(name.to_string(), ty);
        }
    }

    fn clear_globals(&mut self) {
        let doomed: Vec<String> = self
            .vars
            .keys()
            .filter(|k| self.globals.contains(*k) && !self.locals.contains(*k))
            .cloned()
            .collect();
        for k in doomed {
            self.vars.remove(&k);
        }
    }

    fn merge(&self, other: &Env) -> Env {
        let mut out = self.clone();
        out.vars.clear();
        for (k, v) in &self.vars {
            if let Some(w) = other.vars.get(k) {
                if v == w {
                    out.vars.insert(k.clone(), *v);
                }
            }
        }
        out
    }
}

pub struct Optimizer<'a> {
    info: &'a ModuleInfo,
    stats: &'a mut Stats,
}

impl<'a> Optimizer<'a> {
    pub fn new(info: &'a ModuleInfo, stats: &'a mut Stats) -> Self {
        Optimizer { info, stats }
    }

    pub fn run(&mut self, program: &mut Program) -> CResult<()> {
        let mut env = Env::new(&self.info.global_names);
        let old = std::mem::take(&mut program.stmts);
        let mut out = Vec::new();
        for mut s in old {
            match &s.kind {
                StmtKind::Func(_) | StmtKind::Class(_) => out.push(s),
                _ => {
                    if self.opt_stmt(&mut s, &mut env)? {
                        let term = stmt_terminates(&s);
                        out.push(s);
                        if term {
                            self.stats.bump("return 后死代码删除");
                            break;
                        }
                    }
                }
            }
        }
        program.stmts = out;

        // Optimize every function body.  Uncalled functions are still checked,
        // matching the v3 compiler's eager diagnostics.
        for s in &mut program.stmts {
            let func = match &mut s.kind {
                StmtKind::Func(f) => f,
                _ => continue,
            };
            let Some(idx) = self.info.func_map.get(&func.name) else {
                continue;
            };
            let params = self.info.funcs[*idx as usize].params.clone();
            let mut env = Env::new(&self.info.global_names);
            env.in_function = true;
            for p in &params {
                env.locals.insert(p.clone());
                env.vars.insert(p.clone(), Ty::Unknown);
            }
            let mut body = std::mem::take(&mut func.body);
            self.opt_block(&mut body, &mut env)?;
            func.body = body;
        }
        Ok(())
    }

    fn opt_block(&mut self, stmts: &mut Vec<Stmt>, env: &mut Env) -> CResult<()> {
        let old = std::mem::take(stmts);
        for mut s in old {
            if self.opt_stmt(&mut s, env)? {
                let term = stmt_terminates(&s);
                stmts.push(s);
                if term {
                    self.stats.bump("return 后死代码删除");
                    break;
                }
            }
        }
        Ok(())
    }

    /// Returns false when the statement is removed entirely.
    fn opt_stmt(&mut self, stmt: &mut Stmt, env: &mut Env) -> CResult<bool> {
        match &mut stmt.kind {
            StmtKind::Assign(lv, rv) => {
                *rv = self.opt_expr(rv, env)?;
                match lv {
                    LValue::Name(name) => {
                        let ty = expr_ty(rv, env);
                        env.set(name, ty);
                    }
                    LValue::Index(a, b) => {
                        *a = self.opt_expr(a, env)?;
                        *b = self.opt_expr(b, env)?;
                    }
                    LValue::Field(a, _) => {
                        *a = self.opt_expr(a, env)?;
                    }
                }
                Ok(true)
            }
            StmtKind::Expr(e) => {
                *e = self.opt_expr(e, env)?;
                if is_pure(e) {
                    self.stats.bump("纯表达式语句删除");
                    Ok(false)
                } else {
                    Ok(true)
                }
            }
            StmtKind::If {
                cond,
                then_body,
                else_body,
            } => {
                *cond = self.opt_expr(cond, env)?;
                if let Some(k) = const_of(cond) {
                    self.stats.bump("常量条件 if 消除");
                    if k.truthy() {
                        let mut body = std::mem::take(then_body);
                        self.opt_block(&mut body, env)?;
                        *stmt = Stmt {
                            kind: StmtKind::Block(body),
                            span: stmt.span,
                        };
                        return Ok(true);
                    } else if let Some(e) = else_body.take() {
                        let mut v = vec![*e];
                        self.opt_block(&mut v, env)?;
                        match v.len() {
                            0 => return Ok(false),
                            1 => {
                                *stmt = v.pop().unwrap();
                                return Ok(true);
                            }
                            _ => {
                                *stmt = Stmt {
                                    kind: StmtKind::Block(v),
                                    span: stmt.span,
                                };
                                return Ok(true);
                            }
                        }
                    } else {
                        return Ok(false);
                    }
                }

                let mut env_then = env.clone();
                self.opt_block(then_body, &mut env_then)?;
                let mut env_else = env.clone();
                if let Some(e) = else_body {
                    let mut v = vec![(**e).clone()];
                    self.opt_block(&mut v, &mut env_else)?;
                    *else_body = if v.is_empty() {
                        None
                    } else {
                        Some(Box::new(v.pop().unwrap()))
                    };
                }
                if then_body.is_empty() && else_body.is_none() {
                    self.stats.bump("空 if 分支删除");
                    return Ok(false);
                }
                *env = env_then.merge(&env_else);
                Ok(true)
            }
            StmtKind::While { cond, body } => {
                *cond = self.opt_expr(cond, env)?;
                if let Some(k) = const_of(cond) {
                    if !k.truthy() {
                        self.stats.bump("常量条件 while 消除");
                        return Ok(false);
                    }
                }
                let mut env_body = env.clone();
                self.opt_block(body, &mut env_body)?;
                for n in assigned_names(body) {
                    env.set(&n, Ty::Unknown);
                }
                Ok(true)
            }
            StmtKind::For { var, limit, body } => {
                *limit = self.opt_expr(limit, env)?;
                if let Some(K::Int(n)) = const_of(limit) {
                    // Inside functions a `for` variable is always a fresh local,
                    // even when a module global has the same name.  Rewriting
                    // the loop into plain assignments would lose that rule, so
                    // only unroll/remove loops at module scope.
                    let can_rewrite = !env.in_function;
                    if can_rewrite && n <= 0 {
                        self.stats.bump("range(0) 空循环消除");
                        env.set(var, Ty::Int);
                        *stmt = Stmt {
                            kind: StmtKind::Assign(
                                LValue::Name(var.clone()),
                                Expr {
                                    kind: ExprKind::Int(0),
                                    span: limit.span,
                                },
                            ),
                            span: stmt.span,
                        };
                        return Ok(true);
                    }
                    if can_rewrite && n <= 4 && stmt_count(body) * (n as usize) <= 24 {
                        self.stats.bump("小循环完全展开（常量 trip count）");
                        let mut out = Vec::new();
                        let body_assigns_var = assigned_names(body).contains(var);
                        for i in 0..n {
                            out.push(Stmt {
                                kind: StmtKind::Assign(
                                    LValue::Name(var.clone()),
                                    Expr {
                                        kind: ExprKind::Int(i),
                                        span: stmt.span,
                                    },
                                ),
                                span: stmt.span,
                            });
                            let mut copy = body.clone();
                            let mut env_copy = env.clone();
                            self.opt_block(&mut copy, &mut env_copy)?;
                            out.extend(copy);
                        }
                        if !body_assigns_var {
                            out.push(Stmt {
                                kind: StmtKind::Assign(
                                    LValue::Name(var.clone()),
                                    Expr {
                                        kind: ExprKind::Int(n),
                                        span: stmt.span,
                                    },
                                ),
                                span: stmt.span,
                            });
                        }
                        env.set(var, Ty::Int);
                        for n in assigned_names(body) {
                            if &n != var {
                                env.set(&n, Ty::Unknown);
                            }
                        }
                        *stmt = Stmt {
                            kind: StmtKind::Block(out),
                            span: stmt.span,
                        };
                        return Ok(true);
                    }
                }
                let mut env_body = env.clone();
                env_body.set(var, Ty::Int);
                env_body.locals.insert(var.clone());
                self.opt_block(body, &mut env_body)?;
                env.set(var, Ty::Int);
                for n in assigned_names(body) {
                    if &n != var {
                        env.set(&n, Ty::Unknown);
                    }
                }
                Ok(true)
            }
            StmtKind::Block(body) => {
                self.opt_block(body, env)?;
                if body.is_empty() {
                    self.stats.bump("空块删除");
                    return Ok(false);
                }
                Ok(true)
            }
            StmtKind::Return(v) => {
                if let Some(e) = v {
                    *e = self.opt_expr(e, env)?;
                }
                Ok(true)
            }
            StmtKind::Func(_) | StmtKind::Class(_) => Ok(true),
        }
    }

    fn opt_expr(&mut self, e: &mut Expr, env: &mut Env) -> CResult<Expr> {
        match &mut e.kind {
            ExprKind::Bin(op, a, b) => {
                **a = self.opt_expr(a, env)?;
                **b = self.opt_expr(b, env)?;
                if let (Some(ka), Some(kb)) = (const_of(a), const_of(b)) {
                    if let Some(k) = fold_bin(*op, &ka, &kb, e.span)? {
                        self.stats.bump("常量折叠：二元算术/比较/逻辑");
                        return Ok(expr_of(&k, e.span));
                    }
                }
                let op = *op;
                let ta = expr_ty(a, env);
                let tb = expr_ty(b, env);
                match op {
                    BinOp::And | BinOp::Or => {
                        if same_expr(a, b) && is_pure(a) {
                            self.stats.bump("and/or 自吸收（x and x → x）");
                            return Ok((**a).clone());
                        }
                    }
                    BinOp::Add => {
                        if matches!(b.kind, ExprKind::Int(0)) && ta == Ty::Int {
                            self.stats.bump("代数恒等：x+0");
                            return Ok((**a).clone());
                        }
                    }
                    BinOp::Sub => {
                        if matches!(b.kind, ExprKind::Int(0)) && matches!(ta, Ty::Int | Ty::Float) {
                            self.stats.bump("代数恒等：x-0");
                            return Ok((**a).clone());
                        }
                        if matches!(a.kind, ExprKind::Int(0))
                            && matches!(tb, Ty::Int | Ty::Float)
                            && is_pure(b)
                        {
                            self.stats.bump("代数化简：0-x 取负");
                            return Ok(Expr {
                                kind: ExprKind::Un(UnOp::Neg, b.clone()),
                                span: e.span,
                            });
                        }
                    }
                    BinOp::Mul => {
                        if matches!(b.kind, ExprKind::Int(1)) && matches!(ta, Ty::Int | Ty::Float) {
                            self.stats.bump("代数恒等：x*1");
                            return Ok((**a).clone());
                        }
                        if matches!(a.kind, ExprKind::Int(1)) && matches!(tb, Ty::Int | Ty::Float) {
                            self.stats.bump("代数恒等：x*1");
                            return Ok((**b).clone());
                        }
                        if matches!(b.kind, ExprKind::Int(0))
                            && ta == Ty::Int
                            && is_safe_int_expr(a, env)
                        {
                            self.stats.bump("代数化简：x*0");
                            return Ok(Expr {
                                kind: ExprKind::Int(0),
                                span: e.span,
                            });
                        }
                        if matches!(b.kind, ExprKind::Int(2))
                            && ta == Ty::Int
                            && is_safe_int_expr(a, env)
                        {
                            self.stats.bump("强度削减：x*2 → x+x");
                            return Ok(Expr {
                                kind: ExprKind::Bin(BinOp::Add, a.clone(), a.clone()),
                                span: e.span,
                            });
                        }
                    }
                    BinOp::Div => {
                        if matches!(b.kind, ExprKind::Int(1)) && matches!(ta, Ty::Int | Ty::Float) {
                            self.stats.bump("代数恒等：x/1");
                            return Ok((**a).clone());
                        }
                    }
                    BinOp::Eq | BinOp::Ne => {
                        if same_expr(a, b)
                            && is_pure(a)
                            && matches!(ta, Ty::Int | Ty::Bool | Ty::Str | Ty::Arr | Ty::Obj)
                        {
                            self.stats.bump("自比较化简：x==x / x!=x");
                            return Ok(Expr {
                                kind: ExprKind::Bool(op == BinOp::Eq),
                                span: e.span,
                            });
                        }
                    }
                    _ => {}
                }
                Ok(e.clone())
            }
            ExprKind::Un(op, a) => {
                **a = self.opt_expr(a, env)?;
                if let Some(k) = const_of(a) {
                    if let Some(v) = fold_un(*op, &k) {
                        self.stats.bump("常量折叠：一元 not/neg");
                        return Ok(expr_of(&v, e.span));
                    }
                }
                let op = *op;
                if op == UnOp::Not {
                    if let ExprKind::Un(UnOp::Not, inner) = &a.kind {
                        if expr_ty(inner, env) == Ty::Bool {
                            self.stats.bump("双重否定消除");
                            return Ok((**inner).clone());
                        }
                    }
                }
                if op == UnOp::Neg {
                    if let ExprKind::Un(UnOp::Neg, inner) = &a.kind {
                        if matches!(expr_ty(inner, env), Ty::Int | Ty::Float) {
                            self.stats.bump("双重取负消除");
                            return Ok((**inner).clone());
                        }
                    }
                }
                Ok(e.clone())
            }
            ExprKind::Call(name, args) => {
                for a in args.iter_mut() {
                    *a = self.opt_expr(a, env)?;
                }
                if name == "len" && args.len() == 1 {
                    if let Some(K::Str(s)) = const_of(&args[0]) {
                        let n = s.len() as i64;
                        self.stats.bump("常量折叠：len(字符串/数组)");
                        return Ok(Expr {
                            kind: ExprKind::Int(n),
                            span: e.span,
                        });
                    }
                    if let ExprKind::Array(elems) = &args[0].kind {
                        if elems.iter().all(is_pure) {
                            self.stats.bump("常量折叠：len(字符串/数组)");
                            return Ok(Expr {
                                kind: ExprKind::Int(elems.len() as i64),
                                span: e.span,
                            });
                        }
                    }
                }
                if name == "type" && args.len() == 1 {
                    if let Some(t) = const_type(&args[0], env) {
                        self.stats.bump("常量折叠：type(常量)");
                        return Ok(Expr {
                            kind: ExprKind::Str(t.as_bytes().to_vec()),
                            span: e.span,
                        });
                    }
                }
                if self.info.func_map.contains_key(name) {
                    env.clear_globals();
                }
                Ok(e.clone())
            }
            ExprKind::Array(elems) => {
                for x in elems.iter_mut() {
                    *x = self.opt_expr(x, env)?;
                }
                Ok(e.clone())
            }
            ExprKind::Index(a, b) => {
                **a = self.opt_expr(a, env)?;
                **b = self.opt_expr(b, env)?;
                Ok(e.clone())
            }
            ExprKind::Field(a, _) => {
                **a = self.opt_expr(a, env)?;
                Ok(e.clone())
            }
            ExprKind::ClassLit(_, fields) => {
                for (_, v) in fields.iter_mut() {
                    *v = self.opt_expr(v, env)?;
                }
                Ok(e.clone())
            }
            _ => Ok(e.clone()),
        }
    }
}

fn const_type(e: &Expr, env: &Env) -> Option<&'static str> {
    match &e.kind {
        ExprKind::Int(_) => Some("int"),
        ExprKind::Float(_) => Some("float"),
        ExprKind::Str(_) => Some("str"),
        ExprKind::Bool(_) => Some("bool"),
        ExprKind::Array(_) => Some("array"),
        ExprKind::ClassLit(_, _) => Some("object"),
        ExprKind::Ident(name) => match env.ty(name) {
            Ty::Int => Some("int"),
            Ty::Float => Some("float"),
            Ty::Str => Some("str"),
            Ty::Bool => Some("bool"),
            Ty::Arr => Some("array"),
            Ty::Obj => Some("object"),
            Ty::Unknown => None,
        },
        _ => None,
    }
}

fn expr_ty(e: &Expr, env: &Env) -> Ty {
    match &e.kind {
        ExprKind::Int(_) => Ty::Int,
        ExprKind::Float(_) => Ty::Float,
        ExprKind::Str(_) => Ty::Str,
        ExprKind::Bool(_) => Ty::Bool,
        ExprKind::Ident(n) => env.ty(n),
        ExprKind::Array(_) => Ty::Arr,
        ExprKind::ClassLit(_, _) => Ty::Obj,
        ExprKind::Bin(op, a, b) => match op {
            BinOp::Add => {
                let (ta, tb) = (expr_ty(a, env), expr_ty(b, env));
                if ta == Ty::Str && tb == Ty::Str {
                    Ty::Str
                } else if ta == Ty::Str || tb == Ty::Str {
                    Ty::Unknown
                } else {
                    num_result(ta, tb)
                }
            }
            BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Mod => {
                num_result(expr_ty(a, env), expr_ty(b, env))
            }
            BinOp::Eq | BinOp::Ne | BinOp::Lt | BinOp::Gt | BinOp::Le | BinOp::Ge => Ty::Bool,
            BinOp::And | BinOp::Or => {
                let (ta, tb) = (expr_ty(a, env), expr_ty(b, env));
                if ta == tb {
                    ta
                } else {
                    Ty::Unknown
                }
            }
        },
        ExprKind::Un(UnOp::Not, _) => Ty::Bool,
        ExprKind::Un(UnOp::Neg, a) => match expr_ty(a, env) {
            Ty::Int | Ty::Bool => Ty::Int,
            Ty::Float => Ty::Float,
            _ => Ty::Unknown,
        },
        ExprKind::Call(name, _) => match name.as_str() {
            "input" | "len" => Ty::Int,
            "type" => Ty::Str,
            _ => Ty::Unknown,
        },
        _ => Ty::Unknown,
    }
}

fn num_result(a: Ty, b: Ty) -> Ty {
    if a == Ty::Float || b == Ty::Float {
        if matches!(a, Ty::Int | Ty::Bool | Ty::Float)
            && matches!(b, Ty::Int | Ty::Bool | Ty::Float)
        {
            return Ty::Float;
        }
        return Ty::Unknown;
    }
    if matches!(a, Ty::Int | Ty::Bool) && matches!(b, Ty::Int | Ty::Bool) {
        return Ty::Int;
    }
    Ty::Unknown
}

fn same_expr(a: &Expr, b: &Expr) -> bool {
    match (&a.kind, &b.kind) {
        (ExprKind::Int(x), ExprKind::Int(y)) => x == y,
        (ExprKind::Float(x), ExprKind::Float(y)) => x.to_bits() == y.to_bits(),
        (ExprKind::Str(x), ExprKind::Str(y)) => x == y,
        (ExprKind::Bool(x), ExprKind::Bool(y)) => x == y,
        (ExprKind::Ident(x), ExprKind::Ident(y)) => x == y,
        (ExprKind::Bin(op1, a1, b1), ExprKind::Bin(op2, a2, b2)) => {
            op1 == op2 && same_expr(a1, a2) && same_expr(b1, b2)
        }
        (ExprKind::Un(op1, a1), ExprKind::Un(op2, a2)) => op1 == op2 && same_expr(a1, a2),
        _ => false,
    }
}

fn assigned_names(stmts: &[Stmt]) -> HashSet<String> {
    let mut out = HashSet::new();
    fn walk(s: &Stmt, out: &mut HashSet<String>) {
        match &s.kind {
            StmtKind::Assign(LValue::Name(n), _) => {
                out.insert(n.clone());
            }
            StmtKind::Assign(_, _) => {}
            StmtKind::If {
                then_body,
                else_body,
                ..
            } => {
                for s in then_body {
                    walk(s, out);
                }
                if let Some(e) = else_body {
                    walk(e, out);
                }
            }
            StmtKind::While { body, .. } | StmtKind::For { body, .. } | StmtKind::Block(body) => {
                for s in body {
                    walk(s, out);
                }
            }
            _ => {}
        }
    }
    for s in stmts {
        walk(s, &mut out);
    }
    out
}

fn stmt_count(stmts: &[Stmt]) -> usize {
    let mut n = 0;
    for s in stmts {
        n += 1;
        match &s.kind {
            StmtKind::If {
                then_body,
                else_body,
                ..
            } => {
                n += stmt_count(then_body);
                if let Some(e) = else_body {
                    n += stmt_count(std::slice::from_ref(e));
                }
            }
            StmtKind::While { body, .. } | StmtKind::For { body, .. } | StmtKind::Block(body) => {
                n += stmt_count(body);
            }
            _ => {}
        }
    }
    n
}

fn block_terminates(stmts: &[Stmt]) -> bool {
    stmts.last().map(stmt_terminates).unwrap_or(false)
}

fn stmt_terminates(s: &Stmt) -> bool {
    match &s.kind {
        StmtKind::Return(_) => true,
        StmtKind::Block(b) => block_terminates(b),
        StmtKind::If {
            then_body,
            else_body: Some(e),
            ..
        } => block_terminates(then_body) && stmt_terminates(e),
        _ => false,
    }
}
