use std::fmt;

#[derive(Debug, Clone)]
pub struct CompileError {
    pub line: u32,
    pub col: u32,
    pub msg: String,
}

impl CompileError {
    pub fn new(line: u32, col: u32, msg: impl Into<String>) -> Self {
        CompileError {
            line,
            col,
            msg: msg.into(),
        }
    }

    pub fn at(span: Span, msg: impl Into<String>) -> Self {
        CompileError::new(span.line, span.col, msg)
    }

    pub fn plain(msg: impl Into<String>) -> Self {
        CompileError::new(0, 0, msg)
    }
}

impl fmt::Display for CompileError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.line > 0 {
            write!(f, "错误 [{}:{}]: {}", self.line, self.col, self.msg)
        } else {
            write!(f, "错误: {}", self.msg)
        }
    }
}

impl std::error::Error for CompileError {}

pub type CResult<T> = Result<T, CompileError>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Span {
    pub line: u32,
    pub col: u32,
}

impl Span {
    pub fn new(line: u32, col: u32) -> Self {
        Span { line, col }
    }

    pub fn none() -> Self {
        Span { line: 0, col: 0 }
    }
}
