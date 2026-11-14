use crate::diag::Span;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp {
    Not,
    Neg,
}

#[derive(Debug, Clone)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum ExprKind {
    Int(i64),
    Float(f64),
    Str(Vec<u8>),
    Bool(bool),
    Ident(String),
    Bin(BinOp, Box<Expr>, Box<Expr>),
    Un(UnOp, Box<Expr>),
    Call(String, Vec<Expr>),
    Array(Vec<Expr>),
    Index(Box<Expr>, Box<Expr>),
    Field(Box<Expr>, String),
    ClassLit(String, Vec<(String, Expr)>),
}

#[derive(Debug, Clone)]
pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum StmtKind {
    Assign(LValue, Expr),
    Expr(Expr),
    If {
        cond: Expr,
        then_body: Vec<Stmt>,
        else_body: Option<Box<Stmt>>,
    },
    While {
        cond: Expr,
        body: Vec<Stmt>,
    },
    For {
        var: String,
        limit: Expr,
        body: Vec<Stmt>,
    },
    Func(FuncDef),
    Class(ClassDef),
    Return(Option<Expr>),
    Block(Vec<Stmt>),
}

#[derive(Debug, Clone)]
pub enum LValue {
    Name(String),
    Index(Expr, Expr),
    Field(Expr, String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PrimTy {
    Int,
    Float,
    Str,
    Bool,
    Void,
}

#[derive(Debug, Clone)]
pub enum TypeAnnot {
    None,
    Prim(PrimTy),
    Class(String),
}

#[derive(Debug, Clone)]
pub struct Param {
    pub name: String,
    #[allow(dead_code)]
    pub ty: TypeAnnot,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct FuncDef {
    pub name: String,
    pub params: Vec<Param>,
    #[allow(dead_code)]
    pub ret_ty: Option<PrimTy>,
    pub body: Vec<Stmt>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct ClassField {
    pub name: String,
    pub ty: TypeAnnot,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct ClassDef {
    pub name: String,
    pub fields: Vec<ClassField>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Program {
    pub stmts: Vec<Stmt>,
}

pub fn expr(kind: ExprKind, span: Span) -> Expr {
    Expr { kind, span }
}

pub fn stmt(kind: StmtKind, span: Span) -> Stmt {
    Stmt { kind, span }
}
