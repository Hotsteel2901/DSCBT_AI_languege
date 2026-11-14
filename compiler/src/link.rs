use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{anyhow, Context, Result};

use crate::codegen::Target;

pub fn link(obj: &[u8], target: Target, output: &Path) -> Result<()> {
    let dir = env::temp_dir().join(format!(
        "dsc-link-{}-{}",
        std::process::id(),
        unique_suffix()
    ));
    fs::create_dir_all(&dir).with_context(|| format!("创建临时目录失败: {}", dir.display()))?;
    let result = do_link(&dir, obj, target, output);
    let _ = fs::remove_dir_all(&dir);
    result
}

fn unique_suffix() -> u128 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0)
}

fn find_tool(names: &[&str]) -> Option<PathBuf> {
    for name in names {
        if let Some(paths) = env::var_os("PATH") {
            for p in env::split_paths(&paths) {
                let candidate = p.join(name);
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
    }
    None
}

fn find_rust_lld() -> Option<PathBuf> {
    let rustc = env::var("RUSTC").unwrap_or_else(|_| "rustc".to_string());
    let out = Command::new(rustc)
        .arg("--print")
        .arg("sysroot")
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let sysroot = String::from_utf8(out.stdout).ok()?;
    let p = PathBuf::from(sysroot.trim()).join("lib/rustlib/x86_64-unknown-linux-gnu/bin/rust-lld");
    if p.is_file() {
        Some(p)
    } else {
        None
    }
}

fn do_link(dir: &Path, obj: &[u8], target: Target, output: &Path) -> Result<()> {
    let obj_path = dir.join(format!("dsc_user.{}", target.object_ext()));
    fs::write(&obj_path, obj).context("写入目标文件失败")?;
    let rt_bytes = target.runtime_bytes();
    if rt_bytes.is_empty() {
        return Err(anyhow!(
            "当前编译器构建时未包含 {} 运行时；请安装对应 rustup target 后重新构建 dsc",
            target.triple()
        ));
    }
    let rt_path = dir.join("libdsc_runtime.a");
    fs::write(&rt_path, rt_bytes).context("写入运行时库失败")?;

    if let Some(parent) = output.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).ok();
        }
    }

    match target {
        Target::Linux => {
            let custom = env::var_os("DSC_LLD").map(PathBuf::from);
            let (tool, flavor) = if let Some(t) = custom {
                (t, Vec::<String>::new())
            } else if let Some(t) = find_tool(&["ld.lld"]) {
                (t, Vec::new())
            } else if let Some(t) = find_rust_lld() {
                (t, vec!["-flavor".into(), "gnu".into()])
            } else {
                return Err(anyhow!("找不到链接器 ld.lld；请安装 LLD 或设置 DSC_LLD"));
            };
            let mut cmd = Command::new(&tool);
            cmd.args(flavor)
                .arg("-static")
                .arg("-e")
                .arg("_start")
                .arg("--gc-sections")
                .arg("-z")
                .arg("noexecstack")
                .arg("-o")
                .arg(output)
                .arg(&obj_path)
                .arg(&rt_path);
            run(&mut cmd, "链接可执行文件失败")?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let _ = fs::set_permissions(output, fs::Permissions::from_mode(0o755));
            }
        }
        Target::Windows => {
            let custom = env::var_os("DSC_LLD").map(PathBuf::from);
            let (tool, flavor) = if let Some(t) = custom {
                (t, Vec::<String>::new())
            } else if let Some(t) = find_tool(&["lld-link"]) {
                (t, Vec::new())
            } else if let Some(t) = find_rust_lld() {
                (t, vec!["-flavor".into(), "link".into()])
            } else {
                return Err(anyhow!("找不到链接器 lld-link；请安装 LLD 或设置 DSC_LLD"));
            };

            let def_path = dir.join("kernel32.def");
            fs::write(
                &def_path,
                "LIBRARY kernel32.dll\nEXPORTS\n  GetStdHandle\n  WriteFile\n  ReadFile\n  ExitProcess\n",
            )?;
            let lib_path = dir.join("kernel32.lib");
            let mut imp = Command::new(&tool);
            imp.args(&flavor)
                .arg("/dll")
                .arg("/noentry")
                .arg("/machine:x64")
                .arg(format!("/def:{}", def_path.display()))
                .arg(format!("/out:{}", lib_path.display()));
            run(&mut imp, "生成 kernel32 导入库失败")?;

            let mut cmd = Command::new(&tool);
            cmd.args(flavor)
                .arg("/entry:_start")
                .arg("/subsystem:console")
                .arg("/nodefaultlib")
                .arg("/machine:x64")
                .arg("/opt:ref")
                .arg("/opt:icf")
                .arg(format!("/out:{}", output.display()))
                .arg(&obj_path)
                .arg(&rt_path)
                .arg(&lib_path);
            run(&mut cmd, "链接 Windows 可执行文件失败")?;
        }
    }
    Ok(())
}

fn run(cmd: &mut Command, what: &str) -> Result<()> {
    let out = cmd
        .output()
        .with_context(|| format!("无法启动链接器: {cmd:?}"))?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let stdout = String::from_utf8_lossy(&out.stdout);
        return Err(anyhow!(
            "{}：{}\n{}\n{}",
            what,
            out.status,
            stdout.trim(),
            stderr.trim()
        ));
    }
    Ok(())
}
