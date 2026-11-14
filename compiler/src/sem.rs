use std::collections::{HashMap, HashSet};

use crate::ast::*;
use crate::diag::{CResult, CompileError};

#[derive(Debug, Clone)]
pub struct FieldInfo {
    pub name: String,
    pub ty: TypeAnnot,
}

#[derive(Debug, Clone)]
pub struct ClassInfo {
    pub name: String,
    pub fields: Vec<FieldInfo>,
}

#[derive(Debug, Clone)]
pub struct FuncInfo {
    pub params: Vec<String>,
    pub def: FuncDef,
}

#[derive(Debug, Default)]
pub struct ModuleInfo {
    pub classes: Vec<ClassInfo>,
    pub class_map: HashMap<String, u32>,
    pub funcs: Vec<FuncInfo>,
    pub func_map: HashMap<String, u32>,
    /// Names assigned at module scope (the only names that functions treat as
    /// globals during `resolve_store`).
    pub global_names: HashSet<String>,
}

pub fn is_builtin(name: &str) -> bool {
    matches!(name, "print" | "input" | "len" | "type")
}

pub fn build(program: &Program) -> CResult<ModuleInfo> {
    let mut info = ModuleInfo::default();

    for stmt in &program.stmts {
        match &stmt.kind {
            StmtKind::Class(c) => {
                if info.class_map.contains_key(&c.name) {
                    return Err(CompileError::at(
                        c.span,
                        format!("类型 {} 重复定义", c.name),
                    ));
                }
                if info.classes.len() >= 65535 {
                    return Err(CompileError::at(c.span, "类型数量超出限制"));
                }
                let id = info.classes.len() as u32;
                info.class_map.insert(c.name.clone(), id);
                info.classes.push(ClassInfo {
                    name: c.name.clone(),
                    fields: Vec::new(),
                });
            }
            StmtKind::Func(f) => {
                if is_builtin(&f.name) {
                    return Err(CompileError::at(
                        f.span,
                        format!("函数 {} 重复定义", f.name),
                    ));
                }
                if info.func_map.contains_key(&f.name) {
                    return Err(CompileError::at(
                        f.span,
                        format!("函数 {} 重复定义", f.name),
                    ));
                }
                if f.params.len() > 255 {
                    return Err(CompileError::at(f.span, "函数参数过多"));
                }
                let mut seen = HashSet::new();
                for p in &f.params {
                    if !seen.insert(p.name.clone()) {
                        return Err(CompileError::at(p.span, format!("参数 {} 重复", p.name)));
                    }
                }
                let id = info.funcs.len() as u32;
                info.func_map.insert(f.name.clone(), id);
                info.funcs.push(FuncInfo {
                    params: f.params.iter().map(|p| p.name.clone()).collect(),
                    def: f.clone(),
                });
            }
            _ => {}
        }
    }

    // Validate class layouts after all class names are registered.
    for stmt in &program.stmts {
        if let StmtKind::Class(c) = &stmt.kind {
            let id = info.class_map[&c.name] as usize;
            let mut seen = HashSet::new();
            let mut fields = Vec::new();
            for f in &c.fields {
                if !seen.insert(f.name.clone()) {
                    return Err(CompileError::at(
                        f.span,
                        format!("字段 {} 重复定义", f.name),
                    ));
                }
                if let TypeAnnot::Class(name) = &f.ty {
                    if !info.class_map.contains_key(name) {
                        return Err(CompileError::at(f.span, format!("未定义的类型 {}", name)));
                    }
                }
                fields.push(FieldInfo {
                    name: f.name.clone(),
                    ty: f.ty.clone(),
                });
            }
            if fields.len() > 65535 {
                return Err(CompileError::at(c.span, "类型字段过多"));
            }
            info.classes[id].fields = fields;
        }
    }

    // Collect module-level names that `resolve_store` may target as globals.
    for stmt in &program.stmts {
        scan_globals(stmt, &mut info.global_names);
    }

    // Top-level returns are rejected by the v3 compiler.
    for stmt in &program.stmts {
        check_top_level(stmt, &info)?;
    }

    // Validate calls in every function (including uncalled ones).
    for f in &info.funcs {
        for stmt in &f.def.body {
            check_stmt_calls(stmt, &info)?;
        }
    }
    for stmt in &program.stmts {
        match &stmt.kind {
            StmtKind::Func(_) | StmtKind::Class(_) => {}
            _ => check_stmt_calls(stmt, &info)?,
        }
    }

    Ok(info)
}

fn scan_globals(stmt: &Stmt, out: &mut HashSet<String>) {
    match &stmt.kind {
        StmtKind::Assign(LValue::Name(n), _) => {
            out.insert(n.clone());
        }
        StmtKind::For { var, body, .. } => {
            out.insert(var.clone());
            for s in body {
                scan_globals(s, out);
            }
        }
        StmtKind::If {
            then_body,
            else_body,
            ..
        } => {
            for s in then_body {
                scan_globals(s, out);
            }
            if let Some(e) = else_body {
                scan_globals(e, out);
            }
        }
        StmtKind::While { body, .. } | StmtKind::Block(body) => {
            for s in body {
                scan_globals(s, out);
            }
        }
        _ => {}
    }
}

fn check_top_level(stmt: &Stmt, info: &ModuleInfo) -> CResult<()> {
    match &stmt.kind {
        StmtKind::Return(_) => Err(CompileError::at(stmt.span, "return 不能出现在顶层")),
        StmtKind::Func(f) => check_function_body(f, info, false),
        StmtKind::Class(_) => Ok(()),
        StmtKind::If {
            then_body,
            else_body,
            ..
        } => {
            for s in then_body {
                check_top_level(s, info)?;
            }
            if let Some(e) = else_body {
                check_top_level(e, info)?;
            }
            Ok(())
        }
        StmtKind::While { body, .. } | StmtKind::Block(body) => {
            for s in body {
                check_top_level(s, info)?;
            }
            Ok(())
        }
        StmtKind::For { body, .. } => {
            for s in body {
                check_top_level(s, info)?;
            }
            Ok(())
        }
        _ => Ok(()),
    }
}

fn check_function_body(f: &FuncDef, info: &ModuleInfo, _top: bool) -> CResult<()> {
    for s in &f.body {
        check_nested_defs(s, info)?;
    }
    Ok(())
}

fn check_nested_defs(stmt: &Stmt, info: &ModuleInfo) -> CResult<()> {
    match &stmt.kind {
        StmtKind::Func(_) | StmtKind::Class(_) => {
            Err(CompileError::at(stmt.span, "函数/类定义必须位于顶层"))
        }
        StmtKind::If {
            then_body,
            else_body,
            ..
        } => {
            for s in then_body {
                check_nested_defs(s, info)?;
            }
            if let Some(e) = else_body {
                check_nested_defs(e, info)?;
            }
            Ok(())
        }
        StmtKind::While { body, .. } | StmtKind::Block(body) | StmtKind::For { body, .. } => {
            for s in body {
                check_nested_defs(s, info)?;
            }
            Ok(())
        }
        _ => Ok(()),
    }
}

fn check_stmt_calls(stmt: &Stmt, info: &ModuleInfo) -> CResult<()> {
    let check_expr = |e: &Expr| -> CResult<()> { check_calls(e, info) };
    match &stmt.kind {
        StmtKind::Assign(lv, rv) => {
            match lv {
                LValue::Name(_) => {}
                LValue::Index(a, b) => {
                    check_expr(a)?;
                    check_expr(b)?;
                }
                LValue::Field(a, _) => check_expr(a)?,
            }
            check_expr(rv)
        }
        StmtKind::Expr(e) | StmtKind::Return(Some(e)) => check_expr(e),
        StmtKind::Return(None) => Ok(()),
        StmtKind::If {
            cond,
            then_body,
            else_body,
        } => {
            check_expr(cond)?;
            for s in then_body {
                check_stmt_calls(s, info)?;
            }
            if let Some(e) = else_body {
                check_stmt_calls(e, info)?;
            }
            Ok(())
        }
        StmtKind::While { cond, body } => {
            check_expr(cond)?;
            for s in body {
                check_stmt_calls(s, info)?;
            }
            Ok(())
        }
        StmtKind::For { limit, body, .. } => {
            check_expr(limit)?;
            for s in body {
                check_stmt_calls(s, info)?;
            }
            Ok(())
        }
        StmtKind::Block(body) => {
            for s in body {
                check_stmt_calls(s, info)?;
            }
            Ok(())
        }
        StmtKind::Func(_) | StmtKind::Class(_) => Ok(()),
    }
}

fn check_calls(e: &Expr, info: &ModuleInfo) -> CResult<()> {
    match &e.kind {
        ExprKind::Bin(_, a, b) => {
            check_calls(a, info)?;
            check_calls(b, info)
        }
        ExprKind::Un(_, a) => check_calls(a, info),
        ExprKind::Call(name, args) => {
            for a in args {
                check_calls(a, info)?;
            }
            if name == "print" {
                if args.len() > 255 {
                    return Err(CompileError::at(e.span, "print 参数过多"));
                }
                return Ok(());
            }
            if name == "input" {
                if args.len() > 1 {
                    return Err(CompileError::at(e.span, "input 只接受 0 或 1 个参数"));
                }
                return Ok(());
            }
            if name == "len" || name == "type" {
                if args.len() != 1 {
                    return Err(CompileError::at(
                        e.span,
                        format!("{} 需要且仅需要 1 个参数", name),
                    ));
                }
                return Ok(());
            }
            let Some(fi) = info.func_map.get(name) else {
                return Err(CompileError::at(e.span, format!("未定义的函数 {}", name)));
            };
            let f = &info.funcs[*fi as usize];
            if args.len() != f.params.len() {
                return Err(CompileError::at(
                    e.span,
                    format!(
                        "函数 {} 期望 {} 个参数，实际传入 {} 个",
                        name,
                        f.params.len(),
                        args.len()
                    ),
                ));
            }
            Ok(())
        }
        ExprKind::Array(elems) => {
            for x in elems {
                check_calls(x, info)?;
            }
            Ok(())
        }
        ExprKind::Index(a, b) => {
            check_calls(a, info)?;
            check_calls(b, info)
        }
        ExprKind::Field(a, _) => check_calls(a, info),
        ExprKind::ClassLit(_, fields) => {
            for (_, v) in fields {
                check_calls(v, info)?;
            }
            Ok(())
        }
        _ => Ok(()),
    }
}
