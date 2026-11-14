@echo off
echo === DSCBT v4 原生编译器构建脚本 ===
echo.

where cargo >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 未找到 cargo。请先安装 Rust: https://rustup.rs/
    pause
    exit /b 1
)

echo [信息] 构建 dsc 编译器（release）...
cargo build --release -p dsc
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 构建失败。
    pause
    exit /b 1
)

copy /Y target\release\dsc.exe dsc.exe >nul
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 无法复制 dsc.exe。
    pause
    exit /b 1
)

echo [成功] dsc.exe 构建完成!
echo.
echo 用法: dsc.exe 源文件.dscbt [-o 输出.exe] [--target windows] [--stats]
echo 说明: 交叉编译 Linux 目标需要 rustup target add x86_64-unknown-linux-gnu
exit /b 0
