mod ast;
mod codegen;
mod diag;
mod lexer;
mod link;
mod opt;
mod parser;
mod sem;
mod stats;

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use crate::codegen::Target;
use crate::diag::CompileError;

const VERSION: &str = "4.0";

fn usage(out: &mut dyn std::io::Write) {
    let _ = writeln!(
        out,
        "DSC v{VERSION} - 原生编译器\n\
         用法: dsc <源文件.dscbt> [-o 输出] [--target linux|windows] [--stats]\n\
         选项:\n\
           -h, --help      显示帮助\n\
           -v, --version   显示版本\n\
           -o <文件>       指定输出文件\n\
           --target <os>   目标平台: linux (默认) 或 windows\n\
           --stats         打印优化清单与命中统计"
    );
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() {
        usage(&mut std::io::stderr());
        return ExitCode::from(1);
    }
    if args.iter().any(|a| a == "-h" || a == "--help") {
        usage(&mut std::io::stdout());
        return ExitCode::SUCCESS;
    }
    if args.iter().any(|a| a == "-v" || a == "--version") {
        println!("dsc {VERSION}");
        return ExitCode::SUCCESS;
    }

    let mut source: Option<PathBuf> = None;
    let mut output: Option<PathBuf> = None;
    let mut target = Target::Linux;
    let mut show_stats = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-o" => {
                i += 1;
                if i >= args.len() {
                    eprintln!("错误: -o 需要参数");
                    return ExitCode::from(1);
                }
                output = Some(PathBuf::from(&args[i]));
            }
            "--target" => {
                i += 1;
                if i >= args.len() {
                    eprintln!("错误: --target 需要参数");
                    return ExitCode::from(1);
                }
                target = match args[i].as_str() {
                    "linux" => Target::Linux,
                    "windows" | "win" => Target::Windows,
                    other => {
                        eprintln!("错误: 未知目标平台 {other}");
                        return ExitCode::from(1);
                    }
                };
            }
            "--stats" => show_stats = true,
            s if s.starts_with('-') => {
                eprintln!("错误: 未知选项 {s}");
                usage(&mut std::io::stderr());
                return ExitCode::from(1);
            }
            s => {
                if source.is_some() {
                    eprintln!("错误: 只能编译一个源文件");
                    return ExitCode::from(1);
                }
                source = Some(PathBuf::from(s));
            }
        }
        i += 1;
    }
    let Some(source) = source else {
        eprintln!("错误: 缺少源文件");
        usage(&mut std::io::stderr());
        return ExitCode::from(1);
    };

    match compile(&source, output, target, show_stats) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            let code = match e {
                CliError::Compile(_) => 2,
                _ => 1,
            };
            eprintln!("{e}");
            ExitCode::from(code)
        }
    }
}

fn compile(
    source: &Path,
    output: Option<PathBuf>,
    target: Target,
    show_stats: bool,
) -> Result<(), CliError> {
    let bytes = fs::read(source)
        .map_err(|e| CliError::Io(format!("无法打开文件 {}: {e}", source.display())))?;
    let toks = lexer::Lexer::new(&bytes).run().map_err(CliError::Compile)?;
    let mut parser = parser::Parser::new(toks);
    let mut program = parser.parse_program().map_err(CliError::Compile)?;
    let info = sem::build(&program).map_err(CliError::Compile)?;
    let mut stats = stats::Stats::default();
    {
        let mut optimizer = opt::Optimizer::new(&info, &mut stats);
        optimizer.run(&mut program).map_err(CliError::Compile)?;
    }
    let cg = codegen::Codegen::new(&info, target, &program)
        .map_err(|e| CliError::Internal(e.to_string()))?;
    let (object, cg_stats) = match cg.compile(&program) {
        Ok(v) => v,
        Err(e) => {
            if let Some(ce) = e.downcast_ref::<CompileError>() {
                return Err(CliError::Compile(ce.clone()));
            }
            return Err(CliError::Internal(format!("代码生成失败: {e}")));
        }
    };

    let out_path = match output {
        Some(p) => p,
        None => default_output(source, target),
    };
    link::link(&object, target, &out_path).map_err(|e| CliError::Internal(format!("{e}")))?;

    if show_stats {
        println!("DSC v{VERSION} 优化清单: {} 项", stats::catalog_len());
        let mut merged = stats;
        for (k, v) in cg_stats.counts {
            *merged.counts.entry(k).or_insert(0) += v;
        }
        for (name, count) in &merged.counts {
            if *count > 0 {
                println!("  {name}: {count}");
            }
        }
        println!("输出: {}", out_path.display());
    }
    Ok(())
}

fn default_output(source: &Path, target: Target) -> PathBuf {
    let stem = source
        .file_stem()
        .map(|s| s.to_os_string())
        .unwrap_or_else(|| "a.out".into());
    let mut p = PathBuf::from(stem);
    p.set_extension("");
    let mut name = p.as_os_str().to_os_string();
    name.push(target.exe_ext());
    PathBuf::from(name)
}

enum CliError {
    Compile(CompileError),
    Io(String),
    Internal(String),
}

impl std::fmt::Display for CliError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            CliError::Compile(e) => write!(f, "{e}"),
            CliError::Io(e) => write!(f, "错误: {e}"),
            CliError::Internal(e) => write!(f, "错误: {e}"),
        }
    }
}
