@echo off
echo === DSCBT 编译器构建脚本 ===
echo.

:: Try GCC first
where gcc >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [信息] 使用 GCC 构建...
    gcc -O2 -Wall -Wno-parentheses -Wno-misleading-indentation -o dsc.exe dsc.c
    if %ERRORLEVEL% EQU 0 (
        echo [成功] dsc.exe 编译完成!
        goto :done
    )
)

:: Try Clang
where clang >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [信息] 使用 Clang 构建...
    clang -O2 -Wall -Wno-parentheses -Wno-misleading-indentation -o dsc.exe dsc.c
    if %ERRORLEVEL% EQU 0 (
        echo [成功] dsc.exe 编译完成!
        goto :done
    )
)

:: Try MSVC
where cl >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [信息] 使用 MSVC 构建...
    cl /O2 /Fe:dsc.exe dsc.c
    if %ERRORLEVEL% EQU 0 (
        echo [成功] dsc.exe 编译完成!
        goto :done
    )
)

echo [错误] 未找到可用的 C 编译器 (gcc/clang/cl)
echo 请安装 MinGW-w64: https://www.mingw-w64.org/
pause
exit /b 1

:done
echo.
echo 运行方式: dsc.exe 源文件.dscbt [输出.exe]
exit /b 0
