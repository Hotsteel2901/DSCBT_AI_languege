use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let runtime = manifest.join("../runtime/src/lib.rs");
    let rustc = env::var("RUSTC").unwrap_or_else(|_| "rustc".to_string());

    println!("cargo:rerun-if-changed={}", runtime.display());
    println!("cargo:rerun-if-changed=build.rs");

    let targets = [
        ("x86_64-unknown-linux-gnu", "libdsc_runtime_linux.a"),
        ("x86_64-pc-windows-gnu", "libdsc_runtime_windows.a"),
    ];

    for (triple, name) in targets {
        let dest = out.join(name);
        let result = Command::new(&rustc)
            .arg("--crate-name")
            .arg("dsc_runtime")
            .arg("--crate-type")
            .arg("staticlib")
            .arg("--edition")
            .arg("2021")
            .arg("-C")
            .arg("opt-level=2")
            .arg("-C")
            .arg("panic=abort")
            .arg("-C")
            .arg("relocation-model=static")
            .arg("-C")
            .arg("debuginfo=0")
            .arg("--target")
            .arg(triple)
            .arg(&runtime)
            .arg("-o")
            .arg(&dest)
            .output();
        let ok = matches!(&result, Ok(o) if o.status.success());
        if !ok {
            // Keep the compiler buildable even when a cross target's `std`
            // is not installed; linking that target will report a clear
            // error instead.  `rustup target add <triple>` enables it.
            let _ = std::fs::write(&dest, b"");
            println!(
                "cargo:warning=未能构建 {triple} 运行时；该目标需要先安装 rustup target（rustup target add {triple}）"
            );
        }
    }
}
