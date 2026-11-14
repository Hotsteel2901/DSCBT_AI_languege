use crate::ast::*;
use crate::diag::{CResult, CompileError, Span};
use crate::lexer::{Tok, Token};

pub struct Parser {
    toks: Vec<Token>,
    pos: usize,
}

impl Parser {
    pub fn new(toks: Vec<Token>) -> Self {
        Parser { toks, pos: 0 }
    }

    fn peek(&self) -> Option<&Token> {
        self.toks.get(self.pos)
    }

    fn peek_at(&self, n: usize) -> Option<&Token> {
        self.toks.get(self.pos + n)
    }

    fn at(&self, t: &Tok) -> bool {
        self.peek().map(|x| x.is(t)).unwrap_or(false)
    }

    fn take(&mut self) -> Token {
        let t = self.toks[self.pos].clone();
        self.pos += 1;
        t
    }

    fn accept(&mut self, t: &Tok) -> Option<Token> {
        if self.at(t) {
            Some(self.take())
        } else {
            None
        }
    }

    fn expect(&mut self, t: &Tok, what: &str) -> CResult<Token> {
        if self.at(t) {
            Ok(self.take())
        } else {
            let sp = self.peek().map(|x| x.span).unwrap_or_else(Span::none);
            Err(CompileError::at(sp, format!("期望{what}")))
        }
    }

    pub fn parse_program(&mut self) -> CResult<Program> {
        let mut stmts = Vec::new();
        while !self.at(&Tok::Eof) {
            if self.at(&Tok::Newline) {
                self.take();
                continue;
            }
            if self.at(&Tok::Dedent) || self.at(&Tok::Indent) {
                let sp = self.peek().unwrap().span;
                return Err(CompileError::at(sp, "意外的缩进"));
            }
            if let Some(s) = self.parse_stmt()? {
                stmts.push(s);
            }
            if self.at(&Tok::Newline) {
                self.take();
            }
        }
        Ok(Program { stmts })
    }

    fn parse_stmt(&mut self) -> CResult<Option<Stmt>> {
        let Some(tok) = self.peek().cloned() else {
            return Ok(None);
        };
        match tok.kind {
            Tok::Newline | Tok::Dedent | Tok::Eof => Ok(None),
            Tok::If => self.parse_if().map(Some),
            Tok::While => self.parse_while().map(Some),
            Tok::For => self.parse_for().map(Some),
            Tok::Def => self.parse_def().map(Some),
            Tok::Return => self.parse_return().map(Some),
            Tok::Class => self.parse_class().map(Some),
            _ => {
                let e = self.parse_expr()?;
                if self.at(&Tok::Eq) {
                    let eq = self.take();
                    let lv = match &e.kind {
                        ExprKind::Ident(name) => LValue::Name(name.clone()),
                        ExprKind::Index(a, b) => LValue::Index((**a).clone(), (**b).clone()),
                        ExprKind::Field(a, name) => LValue::Field((**a).clone(), name.clone()),
                        _ => return Err(CompileError::at(e.span, "非法的赋值目标")),
                    };
                    let rv = self.parse_expr()?;
                    return Ok(Some(stmt(
                        StmtKind::Assign(lv, rv),
                        Span::new(eq.span.line, eq.span.col),
                    )));
                }
                Ok(Some(stmt(StmtKind::Expr(e.clone()), e.span)))
            }
        }
    }

    fn parse_block(&mut self) -> CResult<Vec<Stmt>> {
        self.expect(&Tok::Colon, " ':'")?;
        let mut body = Vec::new();
        if self.at(&Tok::Newline) {
            self.take();
            self.expect(&Tok::Indent, "缩进")?;
            while !self.at(&Tok::Dedent) && !self.at(&Tok::Eof) {
                if self.at(&Tok::Newline) {
                    self.take();
                    continue;
                }
                if let Some(s) = self.parse_stmt()? {
                    body.push(s);
                }
                if self.at(&Tok::Newline) {
                    self.take();
                }
            }
            self.expect(&Tok::Dedent, "反缩进")?;
        } else {
            if let Some(s) = self.parse_stmt()? {
                body.push(s);
            }
        }
        Ok(body)
    }

    fn parse_if(&mut self) -> CResult<Stmt> {
        let tk = self.take();
        let cond = self.parse_expr()?;
        let then_body = self.parse_block()?;
        let mut else_body: Option<Box<Stmt>> = None;
        if self.at(&Tok::Els) {
            let ek = self.take();
            if self.at(&Tok::If) {
                let nested = self.parse_if()?;
                else_body = Some(Box::new(nested));
            } else {
                let body = self.parse_block()?;
                else_body = Some(Box::new(stmt(
                    StmtKind::Block(body),
                    Span::new(ek.span.line, ek.span.col),
                )));
            }
        }
        Ok(stmt(
            StmtKind::If {
                cond,
                then_body,
                else_body,
            },
            tk.span,
        ))
    }

    fn parse_while(&mut self) -> CResult<Stmt> {
        let tk = self.take();
        let cond = self.parse_expr()?;
        let body = self.parse_block()?;
        Ok(stmt(StmtKind::While { cond, body }, tk.span))
    }

    fn parse_for(&mut self) -> CResult<Stmt> {
        let tk = self.take();
        let var = self.expect(&Tok::Ident(String::new()), "循环变量")?;
        let var = match var.kind {
            Tok::Ident(s) => s,
            _ => unreachable!(),
        };
        self.expect(&Tok::In, " 'in'")?;
        self.expect(&Tok::Range, " 'range'")?;
        self.expect(&Tok::LParen, " '('")?;
        let limit = self.parse_expr()?;
        self.expect(&Tok::RParen, " ')'")?;
        let body = self.parse_block()?;
        Ok(stmt(StmtKind::For { var, limit, body }, tk.span))
    }

    fn parse_def(&mut self) -> CResult<Stmt> {
        let tk = self.take();
        let name = self.expect(&Tok::Ident(String::new()), "函数名")?;
        let name = match name.kind {
            Tok::Ident(s) => s,
            _ => unreachable!(),
        };
        let mut params = Vec::new();
        self.expect(&Tok::LParen, " '('")?;
        if !self.at(&Tok::RParen) {
            loop {
                let pn = self.expect(&Tok::Ident(String::new()), "参数名")?;
                let pname = match pn.kind {
                    Tok::Ident(s) => s,
                    _ => unreachable!(),
                };
                let mut ty = TypeAnnot::None;
                if self.accept(&Tok::Colon).is_some() {
                    ty = self.parse_type_annot()?;
                }
                params.push(Param {
                    name: pname,
                    ty,
                    span: pn.span,
                });
                if self.accept(&Tok::Comma).is_none() {
                    break;
                }
            }
        }
        self.expect(&Tok::RParen, " ')'")?;
        let mut ret_ty = None;
        if self.at(&Tok::Colon) {
            if let Some(nx) = self.peek_at(1) {
                ret_ty = match nx.kind {
                    Tok::IntTy => Some(PrimTy::Int),
                    Tok::FloatTy => Some(PrimTy::Float),
                    Tok::StrTy => Some(PrimTy::Str),
                    Tok::BoolTy => Some(PrimTy::Bool),
                    Tok::VoidTy => Some(PrimTy::Void),
                    _ => None,
                };
                if ret_ty.is_some() {
                    self.take();
                    self.take();
                }
            }
        }
        let body = self.parse_block()?;
        Ok(stmt(
            StmtKind::Func(FuncDef {
                name,
                params,
                ret_ty,
                body,
                span: tk.span,
            }),
            tk.span,
        ))
    }

    fn parse_return(&mut self) -> CResult<Stmt> {
        let tk = self.take();
        if self.at(&Tok::Newline) || self.at(&Tok::Dedent) || self.at(&Tok::Eof) {
            Ok(stmt(StmtKind::Return(None), tk.span))
        } else {
            let e = self.parse_expr()?;
            Ok(stmt(StmtKind::Return(Some(e)), tk.span))
        }
    }

    fn parse_class(&mut self) -> CResult<Stmt> {
        let tk = self.take();
        let name = self.expect(&Tok::Ident(String::new()), "类型名")?;
        let name = match name.kind {
            Tok::Ident(s) => s,
            _ => unreachable!(),
        };
        self.expect(&Tok::Colon, " ':'")?;
        self.expect(&Tok::Newline, "换行")?;
        self.expect(&Tok::Indent, "缩进")?;
        let mut fields = Vec::new();
        while !self.at(&Tok::Dedent) && !self.at(&Tok::Eof) {
            if self.at(&Tok::Newline) {
                self.take();
                continue;
            }
            let fname = self.expect(&Tok::Ident(String::new()), "字段名")?;
            let fspan = fname.span;
            let fname = match fname.kind {
                Tok::Ident(s) => s,
                _ => unreachable!(),
            };
            self.expect(&Tok::Colon, " ':'")?;
            let ty = self.parse_type_annot()?;
            fields.push(ClassField {
                name: fname,
                ty,
                span: fspan,
            });
            if self.at(&Tok::Newline) {
                self.take();
            } else if !self.at(&Tok::Dedent) && !self.at(&Tok::Eof) {
                let sp = self.peek().map(|x| x.span).unwrap_or_else(Span::none);
                return Err(CompileError::at(sp, "类定义中应为换行"));
            }
        }
        self.expect(&Tok::Dedent, "反缩进")?;
        Ok(stmt(
            StmtKind::Class(ClassDef {
                name,
                fields,
                span: tk.span,
            }),
            tk.span,
        ))
    }

    fn parse_type_annot(&mut self) -> CResult<TypeAnnot> {
        let Some(tok) = self.peek().cloned() else {
            return Err(CompileError::plain("类型标注意外结束"));
        };
        match tok.kind {
            Tok::IntTy => {
                self.take();
                Ok(TypeAnnot::Prim(PrimTy::Int))
            }
            Tok::FloatTy => {
                self.take();
                Ok(TypeAnnot::Prim(PrimTy::Float))
            }
            Tok::StrTy => {
                self.take();
                Ok(TypeAnnot::Prim(PrimTy::Str))
            }
            Tok::BoolTy => {
                self.take();
                Ok(TypeAnnot::Prim(PrimTy::Bool))
            }
            Tok::VoidTy => {
                self.take();
                Ok(TypeAnnot::Prim(PrimTy::Void))
            }
            Tok::Ident(name) => {
                self.take();
                Ok(TypeAnnot::Class(name))
            }
            _ => Err(CompileError::at(tok.span, "非法的类型标注")),
        }
    }

    fn parse_primary(&mut self) -> CResult<Expr> {
        let Some(tok) = self.peek().cloned() else {
            return Err(CompileError::plain("表达式意外结束"));
        };
        match tok.kind {
            Tok::Int(v) => {
                self.take();
                Ok(expr(ExprKind::Int(v), tok.span))
            }
            Tok::Float(v) => {
                self.take();
                Ok(expr(ExprKind::Float(v), tok.span))
            }
            Tok::Str(v) => {
                self.take();
                Ok(expr(ExprKind::Str(v), tok.span))
            }
            Tok::True => {
                self.take();
                Ok(expr(ExprKind::Bool(true), tok.span))
            }
            Tok::False => {
                self.take();
                Ok(expr(ExprKind::Bool(false), tok.span))
            }
            Tok::LParen => {
                self.take();
                let e = self.parse_expr()?;
                self.expect(&Tok::RParen, " ')'")?;
                Ok(e)
            }
            Tok::LBracket => {
                self.take();
                let mut elems = Vec::new();
                if !self.at(&Tok::RBracket) {
                    loop {
                        elems.push(self.parse_expr()?);
                        if self.accept(&Tok::Comma).is_none() {
                            break;
                        }
                    }
                }
                self.expect(&Tok::RBracket, " ']'")?;
                Ok(expr(ExprKind::Array(elems), tok.span))
            }
            Tok::Ident(name) => {
                self.take();
                if self.at(&Tok::LParen) {
                    self.take();
                    let mut args = Vec::new();
                    if !self.at(&Tok::RParen) {
                        loop {
                            args.push(self.parse_expr()?);
                            if self.accept(&Tok::Comma).is_none() {
                                break;
                            }
                        }
                    }
                    self.expect(&Tok::RParen, " ')'")?;
                    Ok(expr(ExprKind::Call(name, args), tok.span))
                } else if self.at(&Tok::LBrace) {
                    self.take();
                    let mut fields = Vec::new();
                    if !self.at(&Tok::RBrace) {
                        loop {
                            let fnm = self.expect(&Tok::Ident(String::new()), "字段名")?;
                            let fname = match fnm.kind {
                                Tok::Ident(s) => s,
                                _ => unreachable!(),
                            };
                            self.expect(&Tok::Colon, " ':'")?;
                            let value = self.parse_expr()?;
                            fields.push((fname, value));
                            if self.accept(&Tok::Comma).is_none() {
                                break;
                            }
                        }
                    }
                    self.expect(&Tok::RBrace, " '}'")?;
                    Ok(expr(ExprKind::ClassLit(name, fields), tok.span))
                } else {
                    Ok(expr(ExprKind::Ident(name), tok.span))
                }
            }
            _ => Err(CompileError::at(
                tok.span,
                format!("意外的记号 {}", describe(&tok.kind)),
            )),
        }
    }

    fn parse_unary(&mut self) -> CResult<Expr> {
        if let Some(tk) = self.peek().cloned() {
            if tk.is(&Tok::Not) {
                self.take();
                let x = self.parse_unary()?;
                return Ok(expr(ExprKind::Un(UnOp::Not, Box::new(x)), tk.span));
            }
            if tk.is(&Tok::Minus) {
                self.take();
                let x = self.parse_unary()?;
                return Ok(expr(ExprKind::Un(UnOp::Neg, Box::new(x)), tk.span));
            }
        }
        self.parse_primary()
    }

    fn parse_postfix(&mut self) -> CResult<Expr> {
        let mut e = self.parse_unary()?;
        loop {
            if self.at(&Tok::LBracket) {
                self.take();
                let idx = self.parse_expr()?;
                self.expect(&Tok::RBracket, " ']'")?;
                let sp = e.span;
                e = expr(ExprKind::Index(Box::new(e), Box::new(idx)), sp);
            } else if self.at(&Tok::Dot) {
                self.take();
                let fnm = self.expect(&Tok::Ident(String::new()), "字段名")?;
                let fname = match fnm.kind {
                    Tok::Ident(s) => s,
                    _ => unreachable!(),
                };
                let sp = e.span;
                e = expr(ExprKind::Field(Box::new(e), fname), sp);
            } else {
                break;
            }
        }
        Ok(e)
    }

    fn parse_mul(&mut self) -> CResult<Expr> {
        let mut e = self.parse_postfix()?;
        loop {
            let op = if self.at(&Tok::Star) {
                BinOp::Mul
            } else if self.at(&Tok::Slash) {
                BinOp::Div
            } else if self.at(&Tok::Percent) {
                BinOp::Mod
            } else {
                break;
            };
            self.take();
            let r = self.parse_postfix()?;
            let sp = e.span;
            e = expr(ExprKind::Bin(op, Box::new(e), Box::new(r)), sp);
        }
        Ok(e)
    }

    fn parse_add(&mut self) -> CResult<Expr> {
        let mut e = self.parse_mul()?;
        loop {
            let op = if self.at(&Tok::Plus) {
                BinOp::Add
            } else if self.at(&Tok::Minus) {
                BinOp::Sub
            } else {
                break;
            };
            self.take();
            let r = self.parse_mul()?;
            let sp = e.span;
            e = expr(ExprKind::Bin(op, Box::new(e), Box::new(r)), sp);
        }
        Ok(e)
    }

    fn parse_cmp(&mut self) -> CResult<Expr> {
        let mut e = self.parse_add()?;
        loop {
            let op = if self.at(&Tok::Lt) {
                BinOp::Lt
            } else if self.at(&Tok::Gt) {
                BinOp::Gt
            } else if self.at(&Tok::Le) {
                BinOp::Le
            } else if self.at(&Tok::Ge) {
                BinOp::Ge
            } else {
                break;
            };
            self.take();
            let r = self.parse_add()?;
            let sp = e.span;
            e = expr(ExprKind::Bin(op, Box::new(e), Box::new(r)), sp);
        }
        Ok(e)
    }

    fn parse_eq(&mut self) -> CResult<Expr> {
        let mut e = self.parse_cmp()?;
        loop {
            let op = if self.at(&Tok::EqEq) {
                BinOp::Eq
            } else if self.at(&Tok::Neq) {
                BinOp::Ne
            } else {
                break;
            };
            self.take();
            let r = self.parse_cmp()?;
            let sp = e.span;
            e = expr(ExprKind::Bin(op, Box::new(e), Box::new(r)), sp);
        }
        Ok(e)
    }

    fn parse_and(&mut self) -> CResult<Expr> {
        let mut e = self.parse_eq()?;
        while self.at(&Tok::And) {
            self.take();
            let r = self.parse_eq()?;
            let sp = e.span;
            e = expr(ExprKind::Bin(BinOp::And, Box::new(e), Box::new(r)), sp);
        }
        Ok(e)
    }

    fn parse_or(&mut self) -> CResult<Expr> {
        let mut e = self.parse_and()?;
        while self.at(&Tok::Or) {
            self.take();
            let r = self.parse_and()?;
            let sp = e.span;
            e = expr(ExprKind::Bin(BinOp::Or, Box::new(e), Box::new(r)), sp);
        }
        Ok(e)
    }

    pub fn parse_expr(&mut self) -> CResult<Expr> {
        self.parse_or()
    }
}

fn describe(t: &Tok) -> &'static str {
    match t {
        Tok::Eof => "EOF",
        Tok::Newline => "换行",
        Tok::Indent => "INDENT",
        Tok::Dedent => "DEDENT",
        Tok::Ident(_) => "标识符",
        Tok::Int(_) => "整数",
        Tok::Float(_) => "浮点数",
        Tok::Str(_) => "字符串",
        Tok::Plus => "+",
        Tok::Minus => "-",
        Tok::Star => "*",
        Tok::Slash => "/",
        Tok::Percent => "%",
        Tok::Eq => "=",
        Tok::EqEq => "==",
        Tok::Neq => "!=",
        Tok::Lt => "<",
        Tok::Gt => ">",
        Tok::Le => "<=",
        Tok::Ge => ">=",
        Tok::LParen => "(",
        Tok::RParen => ")",
        Tok::LBracket => "[",
        Tok::RBracket => "]",
        Tok::LBrace => "{",
        Tok::RBrace => "}",
        Tok::Colon => ":",
        Tok::Comma => ",",
        Tok::Dot => ".",
        Tok::True => "True",
        Tok::False => "False",
        Tok::If => "if",
        Tok::Els => "els",
        Tok::While => "while",
        Tok::For => "for",
        Tok::In => "in",
        Tok::Def => "def",
        Tok::Return => "return",
        Tok::Class => "class",
        Tok::And => "and",
        Tok::Or => "or",
        Tok::Not => "not",
        Tok::IntTy => "int",
        Tok::FloatTy => "flt",
        Tok::StrTy => "str",
        Tok::BoolTy => "bol",
        Tok::VoidTy => "void",
        Tok::Range => "range",
    }
}
