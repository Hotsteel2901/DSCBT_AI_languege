use crate::diag::{CResult, CompileError, Span};

#[derive(Debug, Clone, PartialEq)]
pub enum Tok {
    Eof,
    Newline,
    Indent,
    Dedent,
    Ident(String),
    Int(i64),
    Float(f64),
    Str(Vec<u8>),
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Eq,
    EqEq,
    Neq,
    Lt,
    Gt,
    Le,
    Ge,
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Colon,
    Comma,
    Dot,
    True,
    False,
    If,
    Els,
    While,
    For,
    In,
    Def,
    Return,
    Class,
    And,
    Or,
    Not,
    IntTy,
    FloatTy,
    StrTy,
    BoolTy,
    VoidTy,
    Range,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: Tok,
    pub span: Span,
}

impl Token {
    pub fn is(&self, k: &Tok) -> bool {
        std::mem::discriminant(&self.kind) == std::mem::discriminant(k)
    }
}

pub struct Lexer<'a> {
    src: &'a [u8],
    pos: usize,
    line: u32,
    col: u32,
    at_bol: bool,
    indents: Vec<i32>,
    out: Vec<Token>,
}

impl<'a> Lexer<'a> {
    pub fn new(src: &'a [u8]) -> Self {
        let mut pos = 0;
        if src.len() >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF {
            pos = 3;
        }
        Lexer {
            src,
            pos,
            line: 1,
            col: 1,
            at_bol: true,
            indents: vec![0],
            out: Vec::new(),
        }
    }

    fn peek(&self) -> Option<u8> {
        self.src.get(self.pos).copied()
    }

    fn peek2(&self) -> Option<u8> {
        self.src.get(self.pos + 1).copied()
    }

    fn bump(&mut self) {
        if let Some(c) = self.peek() {
            self.pos += 1;
            if c == b'\n' {
                self.line += 1;
                self.col = 1;
            } else {
                self.col += 1;
            }
        }
    }

    fn bump_newline(&mut self) {
        if self.peek() == Some(b'\r') {
            self.pos += 1;
            if self.peek() == Some(b'\n') {
                self.pos += 1;
            }
        } else {
            self.pos += 1;
        }
        self.line += 1;
        self.col = 1;
    }

    fn push(&mut self, kind: Tok, span: Span) {
        self.out.push(Token { kind, span });
    }

    fn skip_line(&mut self) {
        while let Some(c) = self.peek() {
            if c == b'\n' || c == b'\r' {
                break;
            }
            self.bump();
        }
    }

    fn scan_string(&mut self) -> CResult<()> {
        let span = Span::new(self.line, self.col);
        self.bump(); // opening quote
        let mut buf = Vec::new();
        loop {
            let Some(c) = self.peek() else {
                return Err(CompileError::at(span, "字符串未闭合"));
            };
            if c == b'"' {
                self.bump();
                break;
            }
            if c == b'\n' || c == b'\r' {
                return Err(CompileError::at(span, "字符串未闭合"));
            }
            if c == b'\\' {
                self.bump();
                let Some(e) = self.peek() else {
                    return Err(CompileError::at(span, "字符串未闭合"));
                };
                self.bump();
                buf.push(match e {
                    b'n' => b'\n',
                    b't' => b'\t',
                    b'r' => b'\r',
                    b'\\' => b'\\',
                    b'"' => b'"',
                    other => other,
                });
            } else {
                buf.push(c);
                self.bump();
            }
        }
        self.push(Tok::Str(buf), span);
        Ok(())
    }

    fn scan_number(&mut self) -> CResult<()> {
        let span = Span::new(self.line, self.col);
        let start = self.pos;
        while matches!(self.peek(), Some(c) if c.is_ascii_digit()) {
            self.bump();
        }
        let mut is_float = false;
        if self.peek() == Some(b'.') && matches!(self.peek2(), Some(c) if c.is_ascii_digit()) {
            is_float = true;
            self.bump();
            while matches!(self.peek(), Some(c) if c.is_ascii_digit()) {
                self.bump();
            }
        }
        let text = std::str::from_utf8(&self.src[start..self.pos]).unwrap();
        if is_float {
            match text.parse::<f64>() {
                Ok(v) => self.push(Tok::Float(v), span),
                Err(_) => return Err(CompileError::at(span, "浮点字面量超出范围")),
            }
        } else {
            match text.parse::<i64>() {
                Ok(v) => self.push(Tok::Int(v), span),
                Err(_) => return Err(CompileError::at(span, "整数字面量超出范围")),
            }
        }
        Ok(())
    }

    fn scan_ident(&mut self) {
        let span = Span::new(self.line, self.col);
        let start = self.pos;
        while matches!(self.peek(), Some(c) if c.is_ascii_alphanumeric() || c == b'_') {
            self.bump();
        }
        let text = std::str::from_utf8(&self.src[start..self.pos])
            .unwrap_or("")
            .to_string();
        let kind = match text.as_str() {
            "True" => Tok::True,
            "False" => Tok::False,
            "if" => Tok::If,
            "els" => Tok::Els,
            "while" => Tok::While,
            "for" => Tok::For,
            "in" => Tok::In,
            "def" => Tok::Def,
            "return" => Tok::Return,
            "class" => Tok::Class,
            "and" => Tok::And,
            "or" => Tok::Or,
            "not" => Tok::Not,
            "int" => Tok::IntTy,
            "flt" => Tok::FloatTy,
            "str" => Tok::StrTy,
            "bol" => Tok::BoolTy,
            "void" => Tok::VoidTy,
            "range" => Tok::Range,
            _ => Tok::Ident(text),
        };
        self.push(kind, span);
    }

    pub fn run(mut self) -> CResult<Vec<Token>> {
        loop {
            if self.at_bol {
                let mut indent = 0i32;
                while matches!(self.peek(), Some(b' ') | Some(b'\t')) {
                    indent += if self.peek() == Some(b'\t') { 4 } else { 1 };
                    self.pos += 1;
                    self.col += 1;
                }
                let Some(c) = self.peek() else { break };
                if c == b'\n' || c == b'\r' {
                    self.bump_newline();
                    continue;
                }
                if c == b'#' || (c == b'/' && self.peek2() == Some(b'/')) {
                    self.skip_line();
                    continue;
                }
                let cur = *self.indents.last().unwrap();
                if indent > cur {
                    self.indents.push(indent);
                    let sp = Span::new(self.line, self.col);
                    self.push(Tok::Indent, sp);
                } else if indent < cur {
                    while indent < *self.indents.last().unwrap() && self.indents.len() > 1 {
                        self.indents.pop();
                        let sp = Span::new(self.line, self.col);
                        self.push(Tok::Dedent, sp);
                    }
                    if indent != *self.indents.last().unwrap() {
                        return Err(CompileError::new(self.line, self.col, "缩进不一致"));
                    }
                }
                self.at_bol = false;
            }

            let Some(c) = self.peek() else { break };
            if c == b'\n' || c == b'\r' {
                let sp = Span::new(self.line, self.col);
                self.bump_newline();
                self.push(Tok::Newline, sp);
                self.at_bol = true;
                continue;
            }
            if c == b' ' || c == b'\t' {
                self.bump();
                continue;
            }
            if c == b'#' {
                self.skip_line();
                continue;
            }
            if c == b'/' && self.peek2() == Some(b'/') {
                self.skip_line();
                continue;
            }
            if c == b'"' {
                self.scan_string()?;
                continue;
            }
            if c.is_ascii_digit() {
                self.scan_number()?;
                continue;
            }
            if c.is_ascii_alphabetic() || c == b'_' {
                self.scan_ident();
                continue;
            }
            let sp = Span::new(self.line, self.col);
            self.bump();
            match c {
                b'+' => self.push(Tok::Plus, sp),
                b'-' => self.push(Tok::Minus, sp),
                b'*' => self.push(Tok::Star, sp),
                b'/' => self.push(Tok::Slash, sp),
                b'%' => self.push(Tok::Percent, sp),
                b'(' => self.push(Tok::LParen, sp),
                b')' => self.push(Tok::RParen, sp),
                b'[' => self.push(Tok::LBracket, sp),
                b']' => self.push(Tok::RBracket, sp),
                b'{' => self.push(Tok::LBrace, sp),
                b'}' => self.push(Tok::RBrace, sp),
                b':' => self.push(Tok::Colon, sp),
                b',' => self.push(Tok::Comma, sp),
                b'.' => self.push(Tok::Dot, sp),
                b'&' => self.push(Tok::And, sp),
                b'|' => self.push(Tok::Or, sp),
                b'!' => {
                    if self.peek() == Some(b'=') {
                        self.bump();
                        self.push(Tok::Neq, sp);
                    } else {
                        self.push(Tok::Not, sp);
                    }
                }
                b'=' => {
                    if self.peek() == Some(b'=') {
                        self.bump();
                        self.push(Tok::EqEq, sp);
                    } else {
                        self.push(Tok::Eq, sp);
                    }
                }
                b'<' => {
                    if self.peek() == Some(b'=') {
                        self.bump();
                        self.push(Tok::Le, sp);
                    } else {
                        self.push(Tok::Lt, sp);
                    }
                }
                b'>' => {
                    if self.peek() == Some(b'=') {
                        self.bump();
                        self.push(Tok::Ge, sp);
                    } else {
                        self.push(Tok::Gt, sp);
                    }
                }
                other => {
                    return Err(CompileError::new(
                        sp.line,
                        sp.col,
                        format!("非法字符 '{}'", other as char),
                    ))
                }
            }
        }

        while self.indents.len() > 1 {
            self.indents.pop();
            let sp = Span::new(self.line, self.col);
            self.push(Tok::Dedent, sp);
        }
        let sp = Span::new(self.line, self.col);
        self.push(Tok::Eof, sp);
        Ok(self.out)
    }
}
