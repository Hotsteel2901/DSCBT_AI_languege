#![no_std]
#![allow(static_mut_refs)]
#![allow(clippy::missing_safety_doc)]

//! DSCBT v4 native runtime.
//!
//! This crate is compiled as a freestanding static library for each target
//! (Linux x86-64 and Windows x86-64) and linked into every executable emitted
//! by the DSCBT compiler.  It contains no libc dependency.

use core::fmt::{self, Write as FmtWrite};
use core::panic::PanicInfo;

pub const TAG_NIL: u64 = 0;
pub const TAG_INT: u64 = 1;
pub const TAG_FLT: u64 = 2;
pub const TAG_STR: u64 = 3;
pub const TAG_BOL: u64 = 4;
pub const TAG_ARR: u64 = 5;
pub const TAG_OBJ: u64 = 6;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Value {
    pub tag: u64,
    pub payload: u64,
}

#[repr(C)]
pub struct Str {
    pub len: u64,
    // UTF-8 bytes follow the header.
}

#[repr(C)]
pub struct Arr {
    pub len: u64,
    pub data: *mut Value,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Field {
    pub name: *const Str,
    pub value: Value,
}

#[repr(C)]
pub struct Obj {
    pub cls: u64,
    pub nf: u64,
    pub fields: *mut Field,
    pub name: *const Str,
}

#[repr(C, align(8))]
struct StaticStr<const N: usize> {
    len: u64,
    data: [u8; N],
}

static S_NIL: StaticStr<3> = StaticStr {
    len: 3,
    data: *b"nil",
};
static S_INT: StaticStr<3> = StaticStr {
    len: 3,
    data: *b"int",
};
static S_FLT: StaticStr<5> = StaticStr {
    len: 5,
    data: *b"float",
};
static S_STR: StaticStr<3> = StaticStr {
    len: 3,
    data: *b"str",
};
static S_BOL: StaticStr<4> = StaticStr {
    len: 4,
    data: *b"bool",
};
static S_ARR: StaticStr<5> = StaticStr {
    len: 5,
    data: *b"array",
};
static S_OBJ: StaticStr<6> = StaticStr {
    len: 6,
    data: *b"object",
};

const HEAP_SIZE: usize = 64 * 1024 * 1024;
static mut HEAP: [u8; HEAP_SIZE] = [0; HEAP_SIZE];
static mut HEAP_USED: usize = 0;

static mut OUT_BUF: [u8; 65536] = [0; 65536];
static mut OUT_LEN: usize = 0;

static mut LINE_BUF: [u8; 256] = [0; 256];

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe {
        fatal(
            b"\xe9\x94\x99\xe8\xaf\xaf: \xe8\xbf\x90\xe8\xa1\x8c\xe6\x97\xb6 panic\n",
            4,
        )
    }
}

/* ------------------------------------------------------------------ */
/* Platform I/O                                                        */
/* ------------------------------------------------------------------ */

#[cfg(target_os = "linux")]
unsafe fn sys_write(fd: u64, buf: *const u8, len: u64) -> i64 {
    let r: i64;
    core::arch::asm!(
        "syscall",
        inlateout("rax") 1u64 => r,
        in("rdi") fd,
        in("rsi") buf,
        in("rdx") len,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    r
}

#[cfg(target_os = "linux")]
unsafe fn sys_read(fd: u64, buf: *mut u8, len: u64) -> i64 {
    let r: i64;
    core::arch::asm!(
        "syscall",
        inlateout("rax") 0u64 => r,
        in("rdi") fd,
        in("rsi") buf,
        in("rdx") len,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    r
}

#[cfg(target_os = "linux")]
unsafe fn sys_exit(code: u64) -> ! {
    core::arch::asm!(
        "syscall",
        in("rax") 60u64,
        in("rdi") code,
        options(noreturn)
    );
}

// The compiler emits `dsc_entry` as a normal Cranelift function.  The OS
// enters `_start` directly (not via `call`), so stack alignment must be fixed
// up in assembly before calling into generated code.  This also keeps the
// generated function's prologue assumptions intact on both targets.
#[cfg(all(target_arch = "x86_64", target_os = "linux"))]
core::arch::global_asm!(
    ".text",
    ".globl _start",
    ".type _start,@function",
    "_start:",
    "xor ebp, ebp",
    "and rsp, -16",
    "call dsc_entry",
    "mov edi, eax",
    "mov eax, 60",
    "syscall",
    ".size _start, .-_start",
);

#[cfg(all(target_arch = "x86_64", target_os = "windows"))]
core::arch::global_asm!(
    ".text",
    ".globl _start",
    "_start:",
    "and rsp, -16",
    "sub rsp, 32",
    "call dsc_entry",
    "int3",
);

#[cfg(target_os = "windows")]
extern "system" {
    fn GetStdHandle(which: u32) -> *mut core::ffi::c_void;
    fn WriteFile(
        h: *mut core::ffi::c_void,
        buf: *const u8,
        len: u32,
        written: *mut u32,
        ov: *mut core::ffi::c_void,
    ) -> i32;
    fn ReadFile(
        h: *mut core::ffi::c_void,
        buf: *mut u8,
        len: u32,
        read: *mut u32,
        ov: *mut core::ffi::c_void,
    ) -> i32;
    fn ExitProcess(code: u32) -> !;
}

#[cfg(target_os = "windows")]
const STD_OUTPUT: u32 = (-11i32) as u32;
#[cfg(target_os = "windows")]
const STD_ERROR: u32 = (-12i32) as u32;
#[cfg(target_os = "windows")]
const STD_INPUT: u32 = (-10i32) as u32;

unsafe fn write_fd(fd: u64, buf: *const u8, len: u64) {
    #[cfg(target_os = "linux")]
    {
        let _ = sys_write(fd, buf, len);
    }
    #[cfg(target_os = "windows")]
    {
        let which = if fd == 2 { STD_ERROR } else { STD_OUTPUT };
        let h = GetStdHandle(which);
        let mut written: u32 = 0;
        let _ = WriteFile(h, buf, len as u32, &mut written, core::ptr::null_mut());
    }
}

unsafe fn read_fd(fd: u64, buf: *mut u8, len: u64) -> i64 {
    #[cfg(target_os = "linux")]
    {
        sys_read(fd, buf, len)
    }
    #[cfg(target_os = "windows")]
    {
        let _ = fd;
        let h = GetStdHandle(STD_INPUT);
        let mut got: u32 = 0;
        if ReadFile(h, buf, len as u32, &mut got, core::ptr::null_mut()) == 0 {
            -1
        } else {
            got as i64
        }
    }
}

#[no_mangle]
pub extern "C" fn rt_exit(code: u64) -> ! {
    unsafe {
        out_flush();
        #[cfg(target_os = "linux")]
        sys_exit(code);
        #[cfg(target_os = "windows")]
        ExitProcess(code as u32);
    }
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

unsafe fn out_flush() {
    let len = OUT_LEN;
    if len != 0 {
        write_fd(1, (&raw const OUT_BUF) as *const u8, len as u64);
        OUT_LEN = 0;
    }
}

unsafe fn out_bytes(bytes: &[u8]) {
    let mut rest = bytes;
    while !rest.is_empty() {
        let room = 65536usize - OUT_LEN;
        if room == 0 {
            out_flush();
            continue;
        }
        let k = if rest.len() < room { rest.len() } else { room };
        core::ptr::copy_nonoverlapping(
            rest.as_ptr(),
            ((&raw mut OUT_BUF) as *mut u8).add(OUT_LEN),
            k,
        );
        OUT_LEN += k;
        rest = &rest[k..];
    }
}

unsafe fn out_byte(b: u8) {
    if OUT_LEN == 65536 {
        out_flush();
    }
    ((&raw mut OUT_BUF) as *mut u8).add(OUT_LEN).write(b);
    OUT_LEN += 1;
}

unsafe fn out_i64(mut v: i64) {
    let mut tmp = [0u8; 24];
    let mut n = 0usize;
    let neg = v < 0;
    let mut u = if neg {
        (v as i128).unsigned_abs() as u64
    } else {
        v as u64
    };
    if u == 0 {
        out_byte(b'0');
        return;
    }
    while u != 0 {
        tmp[n] = b'0' + (u % 10) as u8;
        n += 1;
        u /= 10;
    }
    if neg {
        out_byte(b'-');
    }
    while n != 0 {
        n -= 1;
        out_byte(tmp[n]);
    }
    let _ = &mut v;
}

struct FmtBuf {
    buf: [u8; 128],
    len: usize,
}

impl FmtBuf {
    fn new() -> Self {
        FmtBuf {
            buf: [0; 128],
            len: 0,
        }
    }
}

impl fmt::Write for FmtBuf {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let bytes = s.as_bytes();
        if self.len + bytes.len() > self.buf.len() {
            return Err(fmt::Error);
        }
        self.buf[self.len..self.len + bytes.len()].copy_from_slice(bytes);
        self.len += bytes.len();
        Ok(())
    }
}

/// Render `v` using C's shortest `%g` style (which is what the v3
/// interpreter printed): scientific notation when the decimal exponent is
/// below -4 or at least as large as the number of significant digits.
fn format_g(v: f64, out: &mut [u8; 64]) -> usize {
    let mut f = FmtBuf::new();
    let _ = write!(&mut f, "{:e}", v);
    let s = &f.buf[..f.len];
    let mut i = 0usize;
    let mut n = 0usize;
    if i < s.len() && s[i] == b'-' {
        out[n] = b'-';
        n += 1;
        i += 1;
    }
    let mut digits = [0u8; 32];
    let mut nd = 0usize;
    let mut exp: i32 = 0;
    let mut seen_e = false;
    while i < s.len() {
        let c = s[i];
        i += 1;
        if c == b'e' {
            seen_e = true;
            break;
        }
        if c == b'.' {
            continue;
        }
        if nd < digits.len() {
            digits[nd] = c;
            nd += 1;
        }
    }
    if seen_e {
        let mut neg = false;
        if i < s.len() && (s[i] == b'-' || s[i] == b'+') {
            neg = s[i] == b'-';
            i += 1;
        }
        let mut e: i32 = 0;
        while i < s.len() && s[i].is_ascii_digit() {
            e = e.saturating_mul(10).saturating_add((s[i] - b'0') as i32);
            i += 1;
        }
        exp = if neg { -e } else { e };
    }
    while nd > 1 && digits[nd - 1] == b'0' {
        nd -= 1;
    }
    if nd == 0 {
        digits[0] = b'0';
        nd = 1;
        exp = 0;
    }
    let p = nd as i32;
    if exp < -4 || exp >= p {
        out[n] = digits[0];
        n += 1;
        if nd > 1 {
            out[n] = b'.';
            n += 1;
            out[n..n + nd - 1].copy_from_slice(&digits[1..nd]);
            n += nd - 1;
        }
        out[n] = b'e';
        n += 1;
        out[n] = if exp < 0 { b'-' } else { b'+' };
        n += 1;
        let ae = exp.unsigned_abs();
        if ae < 10 {
            out[n] = b'0';
            n += 1;
        }
        let mut tmp = [0u8; 8];
        let mut k = 0usize;
        let mut u = ae;
        if u == 0 {
            tmp[k] = b'0';
            k += 1;
        }
        while u != 0 {
            tmp[k] = b'0' + (u % 10) as u8;
            k += 1;
            u /= 10;
        }
        while k != 0 {
            k -= 1;
            out[n] = tmp[k];
            n += 1;
        }
    } else if exp >= 0 {
        let int_len = (exp + 1) as usize;
        if int_len >= nd {
            out[n..n + nd].copy_from_slice(&digits[..nd]);
            n += nd;
            for _ in nd..int_len {
                out[n] = b'0';
                n += 1;
            }
        } else {
            out[n..n + int_len].copy_from_slice(&digits[..int_len]);
            n += int_len;
            out[n] = b'.';
            n += 1;
            out[n..n + nd - int_len].copy_from_slice(&digits[int_len..nd]);
            n += nd - int_len;
        }
    } else {
        out[n] = b'0';
        out[n + 1] = b'.';
        n += 2;
        for _ in 0..(-exp - 1) {
            out[n] = b'0';
            n += 1;
        }
        out[n..n + nd].copy_from_slice(&digits[..nd]);
        n += nd;
    }
    n
}

unsafe fn out_f64(v: f64) {
    if v != v {
        out_bytes(b"nan");
        return;
    }
    if v > f64::MAX {
        out_bytes(b"inf");
        return;
    }
    if v < -f64::MAX {
        out_bytes(b"-inf");
        return;
    }
    let mut tmp = [0u8; 64];
    let n = format_g(v, &mut tmp);
    out_bytes(&tmp[..n]);
}

fn tag_name(tag: u64) -> Value {
    let p: *const Str = match tag {
        TAG_INT => &S_INT as *const _ as *const Str,
        TAG_FLT => &S_FLT as *const _ as *const Str,
        TAG_STR => &S_STR as *const _ as *const Str,
        TAG_BOL => &S_BOL as *const _ as *const Str,
        TAG_ARR => &S_ARR as *const _ as *const Str,
        TAG_OBJ => &S_OBJ as *const _ as *const Str,
        _ => &S_NIL as *const _ as *const Str,
    };
    Value {
        tag: TAG_STR,
        payload: p as u64,
    }
}

unsafe fn str_bytes<'a>(s: *const Str) -> &'a [u8] {
    if s.is_null() {
        return &[];
    }
    let len = (*s).len as usize;
    core::slice::from_raw_parts((s as *const u8).add(8), len)
}

unsafe fn out_value(v: Value, depth: u64) {
    match v.tag {
        TAG_NIL => out_bytes(b"nil"),
        TAG_INT => out_i64(v.payload as i64),
        TAG_FLT => out_f64(f64::from_bits(v.payload)),
        TAG_STR => out_bytes(str_bytes(v.payload as *const Str)),
        TAG_BOL => {
            if v.payload != 0 {
                out_bytes(b"True")
            } else {
                out_bytes(b"False")
            }
        }
        TAG_ARR => {
            let a = v.payload as *const Arr;
            if depth >= 8 {
                out_bytes(b"[...]");
                return;
            }
            out_byte(b'[');
            let n = (*a).len;
            let data = (*a).data;
            let mut i = 0u64;
            while i < n {
                if i != 0 {
                    out_bytes(b", ");
                }
                out_value(*data.add(i as usize), depth + 1);
                i += 1;
            }
            out_byte(b']');
        }
        TAG_OBJ => out_bytes(b"{object}"),
        _ => out_bytes(b"nil"),
    }
}

#[no_mangle]
pub extern "C" fn rt_print_value(v: *const Value) {
    unsafe { out_value(*v, 0) }
}

#[no_mangle]
pub extern "C" fn rt_print_space() {
    unsafe { out_byte(b' ') }
}

#[no_mangle]
pub extern "C" fn rt_print_newline() {
    unsafe { out_byte(b'\n') }
}

#[no_mangle]
pub extern "C" fn rt_print_i64(v: i64) {
    unsafe { out_i64(v) }
}

#[no_mangle]
pub extern "C" fn rt_print_bool(v: u64) {
    unsafe {
        if v != 0 {
            out_bytes(b"True")
        } else {
            out_bytes(b"False")
        }
    }
}

#[no_mangle]
pub extern "C" fn rt_print_f64(bits: u64) {
    unsafe { out_f64(f64::from_bits(bits)) }
}

#[no_mangle]
pub unsafe extern "C" fn rt_print_bytes(p: *const u8, n: u64) {
    if !p.is_null() {
        out_bytes(core::slice::from_raw_parts(p, n as usize))
    }
}
/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

unsafe fn fatal(msg: &[u8], code: u64) -> ! {
    out_flush();
    write_fd(2, msg.as_ptr(), msg.len() as u64);
    #[cfg(target_os = "linux")]
    sys_exit(code);
    #[cfg(target_os = "windows")]
    ExitProcess(code as u32);
}

unsafe fn err_begin(line: u64) {
    out_flush();
    write_fd(2, b"\xe9\x94\x99\xe8\xaf\xaf [".as_ptr(), 8);
    let mut tmp = [0u8; 24];
    let mut n = 0usize;
    let mut u = line;
    if u == 0 {
        tmp[n] = b'0';
        n += 1;
    }
    while u != 0 {
        tmp[n] = b'0' + (u % 10) as u8;
        n += 1;
        u /= 10;
    }
    while n != 0 {
        n -= 1;
        write_fd(2, tmp.as_ptr().add(n), 1);
    }
    write_fd(2, b":0]: ".as_ptr(), 5);
}

unsafe fn err_text(s: &[u8]) {
    write_fd(2, s.as_ptr(), s.len() as u64);
}

unsafe fn err_type(v: Value) {
    match v.tag {
        TAG_INT => err_text(b"int"),
        TAG_FLT => err_text(b"float"),
        TAG_STR => err_text(b"str"),
        TAG_BOL => err_text(b"bool"),
        TAG_ARR => err_text(b"array"),
        TAG_OBJ => err_text(b"object"),
        _ => err_text(b"nil"),
    }
}

unsafe fn err_value(v: Value) {
    match v.tag {
        TAG_NIL => err_text(b"nil"),
        TAG_INT => {
            let mut tmp = [0u8; 24];
            let mut n = 0usize;
            let neg = (v.payload as i64) < 0;
            let mut u = if neg {
                (v.payload as i64).unsigned_abs()
            } else {
                v.payload
            };
            if u == 0 {
                tmp[n] = b'0';
                n += 1;
            }
            while u != 0 {
                tmp[n] = b'0' + (u % 10) as u8;
                n += 1;
                u /= 10;
            }
            if neg {
                write_fd(2, b"-".as_ptr(), 1);
            }
            while n != 0 {
                n -= 1;
                write_fd(2, tmp.as_ptr().add(n), 1);
            }
        }
        TAG_FLT => err_text(b"float"),
        TAG_BOL => err_text(b"bool"),
        TAG_ARR => err_text(b"array"),
        TAG_OBJ => err_text(b"object"),
        _ => err_text(b"nil"),
    }
}

unsafe fn err_end(line: u64) -> ! {
    write_fd(2, b"\n".as_ptr(), 1);
    let _ = line;
    #[cfg(target_os = "linux")]
    sys_exit(3);
    #[cfg(target_os = "windows")]
    ExitProcess(3);
}

unsafe fn error_str(line: u64, msg: &[u8]) -> ! {
    err_begin(line);
    err_text(msg);
    err_end(line)
}

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

unsafe fn alloc(n: usize) -> *mut u8 {
    let used = HEAP_USED;
    let start = (used + 15) & !15usize;
    if start > HEAP_SIZE || n > HEAP_SIZE - start {
        fatal(
            b"\xe9\x94\x99\xe8\xaf\xaf: \xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3\n",
            4,
        );
    }
    HEAP_USED = start + n;
    ((&raw mut HEAP) as *mut u8).add(start)
}

unsafe fn str_len(s: *const Str) -> u64 {
    if s.is_null() {
        0
    } else {
        (*s).len
    }
}

unsafe fn str_eq(a: *const Str, b: *const Str) -> bool {
    if a == b {
        return true;
    }
    let la = str_len(a) as usize;
    let lb = str_len(b) as usize;
    if la != lb {
        return false;
    }
    if la == 0 {
        return true;
    }
    core::slice::from_raw_parts((a as *const u8).add(8), la)
        == core::slice::from_raw_parts((b as *const u8).add(8), lb)
}

unsafe fn str_cmp(a: *const Str, b: *const Str) -> i32 {
    let ba = str_bytes(a);
    let bb = str_bytes(b);
    let n = if ba.len() < bb.len() {
        ba.len()
    } else {
        bb.len()
    };
    let mut i = 0usize;
    while i < n {
        if ba[i] != bb[i] {
            return ba[i] as i32 - bb[i] as i32;
        }
        i += 1;
    }
    if ba.len() < bb.len() {
        -1
    } else if ba.len() > bb.len() {
        1
    } else {
        0
    }
}

fn is_intlike(v: Value) -> bool {
    v.tag == TAG_INT || v.tag == TAG_BOL
}

fn is_num(v: Value) -> bool {
    v.tag == TAG_INT || v.tag == TAG_BOL || v.tag == TAG_FLT
}

fn as_d(v: Value) -> f64 {
    if v.tag == TAG_FLT {
        f64::from_bits(v.payload)
    } else {
        v.payload as i64 as f64
    }
}

unsafe fn truthy(v: Value) -> u64 {
    match v.tag {
        TAG_NIL => 0,
        TAG_INT => (v.payload as i64 != 0) as u64,
        TAG_FLT => (f64::from_bits(v.payload) != 0.0) as u64,
        TAG_STR => (!str_bytes(v.payload as *const Str).is_empty()) as u64,
        TAG_BOL => (v.payload != 0) as u64,
        TAG_ARR | TAG_OBJ => (v.payload != 0) as u64,
        _ => 0,
    }
}

unsafe fn value_eq(a: Value, b: Value) -> bool {
    if is_num(a) && is_num(b) {
        if is_intlike(a) && is_intlike(b) {
            return a.payload == b.payload;
        }
        return as_d(a) == as_d(b);
    }
    if a.tag != b.tag {
        return false;
    }
    match a.tag {
        TAG_NIL => true,
        TAG_STR => str_eq(a.payload as *const Str, b.payload as *const Str),
        TAG_ARR | TAG_OBJ => a.payload == b.payload,
        _ => false,
    }
}

unsafe fn concat(a: Value, b: Value) -> Value {
    let sa = if a.tag == TAG_STR {
        str_bytes(a.payload as *const Str)
    } else {
        &[]
    };
    let sb = if b.tag == TAG_STR {
        str_bytes(b.payload as *const Str)
    } else {
        &[]
    };
    let la = if a.tag == TAG_STR {
        sa.len()
    } else {
        fmt_len(a)
    };
    let lb = if b.tag == TAG_STR {
        sb.len()
    } else {
        fmt_len(b)
    };
    let p = alloc(8 + la + lb);
    (p as *mut u64).write((la + lb) as u64);
    let dst = p.add(8);
    if a.tag == TAG_STR {
        if !sa.is_empty() {
            core::ptr::copy_nonoverlapping(sa.as_ptr(), dst, la);
        }
    } else {
        fmt_write(a, dst);
    }
    if b.tag == TAG_STR {
        if !sb.is_empty() {
            core::ptr::copy_nonoverlapping(sb.as_ptr(), dst.add(la), lb);
        }
    } else {
        fmt_write(b, dst.add(la));
    }
    Value {
        tag: TAG_STR,
        payload: p as u64,
    }
}

unsafe fn fmt_len(v: Value) -> usize {
    match v.tag {
        TAG_INT => {
            let mut u = if (v.payload as i64) < 0 {
                (v.payload as i64).unsigned_abs()
            } else {
                v.payload
            };
            let mut n = 1usize;
            while u >= 10 {
                u /= 10;
                n += 1;
            }
            n + ((v.payload as i64) < 0) as usize
        }
        TAG_FLT => {
            let mut tmp = [0u8; 64];
            format_g(f64::from_bits(v.payload), &mut tmp)
        }
        TAG_BOL => {
            if v.payload != 0 {
                4
            } else {
                5
            }
        }
        _ => 3,
    }
}

unsafe fn fmt_write(v: Value, dst: *mut u8) {
    match v.tag {
        TAG_INT => {
            let s = v.payload as i64;
            let neg = s < 0;
            let mut u = if neg { s.unsigned_abs() } else { s as u64 };
            let mut tmp = [0u8; 24];
            let mut n = 0usize;
            if u == 0 {
                tmp[n] = b'0';
                n += 1;
            }
            while u != 0 {
                tmp[n] = b'0' + (u % 10) as u8;
                n += 1;
                u /= 10;
            }
            let mut w = 0usize;
            if neg {
                *dst.add(w) = b'-';
                w += 1;
            }
            while n != 0 {
                n -= 1;
                *dst.add(w) = tmp[n];
                w += 1;
            }
        }
        TAG_FLT => {
            let mut tmp = [0u8; 64];
            let n = format_g(f64::from_bits(v.payload), &mut tmp);
            core::ptr::copy_nonoverlapping(tmp.as_ptr(), dst, n);
        }
        TAG_BOL => {
            if v.payload != 0 {
                core::ptr::copy_nonoverlapping(b"True".as_ptr(), dst, 4);
            } else {
                core::ptr::copy_nonoverlapping(b"False".as_ptr(), dst, 5);
            }
        }
        _ => {
            core::ptr::copy_nonoverlapping(b"nil".as_ptr(), dst, 3);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Arithmetic                                                          */
/* ------------------------------------------------------------------ */

#[no_mangle]
pub unsafe extern "C" fn rt_truthy(v: *const Value) -> u64 {
    truthy(*v)
}

#[no_mangle]
pub unsafe extern "C" fn rt_eq(a: *const Value, b: *const Value) -> u64 {
    value_eq(*a, *b) as u64
}

#[no_mangle]
pub unsafe extern "C" fn rt_compare(a: *const Value, b: *const Value, op: u64, line: u64) -> u64 {
    let x = *a;
    let y = *b;
    if is_num(x) && is_num(y) {
        if is_intlike(x) && is_intlike(y) {
            let u = x.payload as i64;
            let v = y.payload as i64;
            return match op {
                0 => (u < v) as u64,
                1 => (u > v) as u64,
                2 => (u <= v) as u64,
                _ => (u >= v) as u64,
            };
        }
        let u = as_d(x);
        let v = as_d(y);
        return match op {
            0 => (u < v) as u64,
            1 => (u > v) as u64,
            2 => (u <= v) as u64,
            _ => (u >= v) as u64,
        };
    }
    if x.tag == TAG_STR && y.tag == TAG_STR {
        let c = str_cmp(x.payload as *const Str, y.payload as *const Str);
        return match op {
            0 => (c < 0) as u64,
            1 => (c > 0) as u64,
            2 => (c <= 0) as u64,
            _ => (c >= 0) as u64,
        };
    }
    err_begin(line);
    err_text(b"\xe6\x97\xa0\xe6\xb3\x95\xe6\xaf\x94\xe8\xbe\x83 ");
    err_type(x);
    err_text(b" \xe5\x92\x8c ");
    err_type(y);
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_add(out: *mut Value, a: *const Value, b: *const Value, line: u64) {
    let x = *a;
    let y = *b;
    if is_intlike(x) && is_intlike(y) {
        *out = Value {
            tag: TAG_INT,
            payload: (x.payload as i64).wrapping_add(y.payload as i64) as u64,
        };
        return;
    }
    if x.tag == TAG_STR || y.tag == TAG_STR {
        *out = concat(x, y);
        return;
    }
    if is_num(x) && is_num(y) {
        *out = Value {
            tag: TAG_FLT,
            payload: (as_d(x) + as_d(y)).to_bits(),
        };
        return;
    }
    err_begin(line);
    err_text("不支持的操作数类型: ".as_bytes());
    err_type(x);
    err_text(b" + ");
    err_type(y);
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_sub(out: *mut Value, a: *const Value, b: *const Value, line: u64) {
    let x = *a;
    let y = *b;
    if is_intlike(x) && is_intlike(y) {
        *out = Value {
            tag: TAG_INT,
            payload: (x.payload as i64).wrapping_sub(y.payload as i64) as u64,
        };
        return;
    }
    if is_num(x) && is_num(y) {
        *out = Value {
            tag: TAG_FLT,
            payload: (as_d(x) - as_d(y)).to_bits(),
        };
        return;
    }
    err_begin(line);
    err_text("不支持的操作数类型: ".as_bytes());
    err_type(x);
    err_text(b" - ");
    err_type(y);
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_mul(out: *mut Value, a: *const Value, b: *const Value, line: u64) {
    let x = *a;
    let y = *b;
    if is_intlike(x) && is_intlike(y) {
        *out = Value {
            tag: TAG_INT,
            payload: (x.payload as i64).wrapping_mul(y.payload as i64) as u64,
        };
        return;
    }
    if is_num(x) && is_num(y) {
        *out = Value {
            tag: TAG_FLT,
            payload: (as_d(x) * as_d(y)).to_bits(),
        };
        return;
    }
    err_begin(line);
    err_text("不支持的操作数类型: ".as_bytes());
    err_type(x);
    err_text(b" * ");
    err_type(y);
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_div(out: *mut Value, a: *const Value, b: *const Value, line: u64) {
    let x = *a;
    let y = *b;
    if is_intlike(x) && is_intlike(y) {
        let d = y.payload as i64;
        if d == 0 {
            error_str(
                line,
                b"\xe6\x95\xb4\xe6\x95\xb0\xe9\x99\xa4\xe4\xbb\xa5\xe9\x9b\xb6",
            );
        }
        let n = x.payload as i64;
        if n == i64::MIN && d == -1 {
            error_str(
                line,
                b"\xe6\x95\xb4\xe6\x95\xb0\xe9\x99\xa4\xe6\xb3\x95\xe6\xba\xa2\xe5\x87\xba",
            );
        }
        *out = Value {
            tag: TAG_INT,
            payload: (n / d) as u64,
        };
        return;
    }
    if is_num(x) && is_num(y) {
        *out = Value {
            tag: TAG_FLT,
            payload: (as_d(x) / as_d(y)).to_bits(),
        };
        return;
    }
    err_begin(line);
    err_text("不支持的操作数类型: ".as_bytes());
    err_type(x);
    err_text(b" / ");
    err_type(y);
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_mod(out: *mut Value, a: *const Value, b: *const Value, line: u64) {
    let x = *a;
    let y = *b;
    if is_intlike(x) && is_intlike(y) {
        let d = y.payload as i64;
        if d == 0 {
            error_str(
                line,
                b"\xe6\x95\xb4\xe6\x95\xb0\xe5\x8f\x96\xe6\xa8\xa1\xe9\x9b\xb6",
            );
        }
        let n = x.payload as i64;
        *out = Value {
            tag: TAG_INT,
            payload: if n == i64::MIN && d == -1 {
                0
            } else {
                (n % d) as u64
            },
        };
        return;
    }
    error_str(line, b"\xe5\x8f\x96\xe6\xa8\xa1\xe8\xbf\x90\xe7\xae\x97\xe8\xa6\x81\xe6\xb1\x82\xe6\x95\xb4\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c\xe6\x95\xb0")
}

#[no_mangle]
pub unsafe extern "C" fn rt_neg(out: *mut Value, a: *const Value, line: u64) {
    let x = *a;
    if is_intlike(x) {
        *out = Value {
            tag: TAG_INT,
            payload: (x.payload as i64).wrapping_neg() as u64,
        };
        return;
    }
    if x.tag == TAG_FLT {
        *out = Value {
            tag: TAG_FLT,
            payload: (-f64::from_bits(x.payload)).to_bits(),
        };
        return;
    }
    err_begin(line);
    err_text("不能对 ".as_bytes());
    err_type(x);
    err_text(" 取负".as_bytes());
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_not(out: *mut Value, a: *const Value) {
    *out = Value {
        tag: TAG_BOL,
        payload: (truthy(*a) == 0) as u64,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rt_inc(ptr: *mut Value, line: u64) {
    let v = *ptr;
    if is_intlike(v) || v.tag == TAG_FLT {
        let mut one = Value {
            tag: TAG_INT,
            payload: 1,
        };
        let mut out = Value {
            tag: TAG_NIL,
            payload: 0,
        };
        rt_add(&mut out, &v, &one, line);
        *ptr = out;
        let _ = &mut one;
        return;
    }
    let one = Value {
        tag: TAG_INT,
        payload: 1,
    };
    let mut out = Value {
        tag: TAG_NIL,
        payload: 0,
    };
    rt_add(&mut out, &v, &one, line);
    *ptr = out;
}

/* ------------------------------------------------------------------ */
/* Builtins                                                            */
/* ------------------------------------------------------------------ */

#[no_mangle]
pub unsafe extern "C" fn rt_type(v: *const Value, out: *mut Value) {
    *out = tag_name((*v).tag);
}

#[no_mangle]
pub unsafe extern "C" fn rt_len(v: *const Value, out: *mut Value, line: u64) {
    let x = *v;
    let n = match x.tag {
        TAG_STR => str_len(x.payload as *const Str),
        TAG_ARR => (*(x.payload as *const Arr)).len,
        TAG_OBJ => (*(x.payload as *const Obj)).nf,
        _ => {
            err_begin(line);
            err_text(b"len() \xe4\xb8\x8d\xe6\x94\xaf\xe6\x8c\x81 ");
            err_type(x);
            err_end(line)
        }
    };
    *out = Value {
        tag: TAG_INT,
        payload: n,
    };
}

#[no_mangle]
pub unsafe extern "C" fn rt_input(prompt: *const Value, out: *mut Value) {
    let p = *prompt;
    if p.tag == TAG_STR {
        out_value(p, 0);
    } else if p.tag != TAG_NIL {
        out_value(p, 0);
    }
    out_flush();
    let mut n = 0usize;
    let mut buf = &raw mut LINE_BUF as *mut u8;
    loop {
        let mut ch = 0u8;
        let got = read_fd(0, &mut ch, 1);
        if got <= 0 {
            break;
        }
        if ch == b'\n' {
            break;
        }
        if ch == b'\r' {
            continue;
        }
        if n < 255 {
            buf.add(n).write(ch);
            n += 1;
        }
    }
    let bytes = core::slice::from_raw_parts(buf, n);
    let mut i = 0usize;
    while i < bytes.len() && (bytes[i] == b' ' || bytes[i] == b'\t') {
        i += 1;
    }
    let mut neg = false;
    if i < bytes.len() && (bytes[i] == b'-' || bytes[i] == b'+') {
        neg = bytes[i] == b'-';
        i += 1;
    }
    let mut v: i64 = 0;
    let mut any = false;
    while i < bytes.len() && bytes[i].is_ascii_digit() {
        any = true;
        v = v.wrapping_mul(10).wrapping_add((bytes[i] - b'0') as i64);
        i += 1;
    }
    if !any {
        v = 0;
    }
    if neg {
        v = v.wrapping_neg();
    }
    *out = Value {
        tag: TAG_INT,
        payload: v as u64,
    };
    let _ = &mut buf;
}

/* ------------------------------------------------------------------ */
/* Arrays                                                              */
/* ------------------------------------------------------------------ */

#[no_mangle]
pub unsafe extern "C" fn rt_new_array(n: u64, vals: *const Value, out: *mut Value) {
    let a = alloc(core::mem::size_of::<Arr>()) as *mut Arr;
    let data = if n == 0 {
        core::ptr::null_mut()
    } else {
        let p = alloc((n as usize) * core::mem::size_of::<Value>()) as *mut Value;
        let mut i = 0u64;
        while i < n {
            p.add(i as usize).write(*vals.add(i as usize));
            i += 1;
        }
        p
    };
    (*a).len = n;
    (*a).data = data;
    *out = Value {
        tag: TAG_ARR,
        payload: a as u64,
    };
}

#[no_mangle]
pub unsafe extern "C" fn rt_get(arr: *const Value, idx: *const Value, out: *mut Value, line: u64) {
    let a = *arr;
    let i = *idx;
    if a.tag != TAG_ARR {
        err_begin(line);
        err_text(b"\xe4\xb8\x8b\xe6\xa0\x87\xe8\xae\xbf\xe9\x97\xae\xe8\xa6\x81\xe6\xb1\x82\xe6\x95\xb0\xe7\xbb\x84\xef\xbc\x8c\xe5\xae\x9e\xe9\x99\x85\xe4\xb8\xba ");
        err_type(a);
        err_end(line)
    }
    if i.tag != TAG_INT {
        error_str(line, b"\xe6\x95\xb0\xe7\xbb\x84\xe4\xb8\x8b\xe6\xa0\x87\xe5\xbf\x85\xe9\xa1\xbb\xe6\x98\xaf\xe6\x95\xb4\xe6\x95\xb0");
    }
    let arrp = a.payload as *const Arr;
    let n = (*arrp).len;
    let k = i.payload as i64;
    if k < 0 || k as u64 >= n {
        err_begin(line);
        err_text(b"\xe6\x95\xb0\xe7\xbb\x84\xe4\xb8\x8b\xe6\xa0\x87\xe8\xb6\x8a\xe7\x95\x8c: ");
        err_value(i);
        err_text(b" (\xe9\x95\xbf\xe5\xba\xa6 ");
        err_value(Value {
            tag: TAG_INT,
            payload: n,
        });
        err_text(b")");
        err_end(line)
    }
    *out = *(*arrp).data.add(k as usize);
}

#[no_mangle]
pub unsafe extern "C" fn rt_set(
    arr: *const Value,
    idx: *const Value,
    val: *const Value,
    line: u64,
) {
    let a = *arr;
    let i = *idx;
    if a.tag != TAG_ARR {
        err_begin(line);
        err_text(b"\xe4\xb8\x8b\xe6\xa0\x87\xe8\xb5\x8b\xe5\x80\xbc\xe8\xa6\x81\xe6\xb1\x82\xe6\x95\xb0\xe7\xbb\x84\xef\xbc\x8c\xe5\xae\x9e\xe9\x99\x85\xe4\xb8\xba ");
        err_type(a);
        err_end(line)
    }
    if i.tag != TAG_INT {
        error_str(line, b"\xe6\x95\xb0\xe7\xbb\x84\xe4\xb8\x8b\xe6\xa0\x87\xe5\xbf\x85\xe9\xa1\xbb\xe6\x98\xaf\xe6\x95\xb4\xe6\x95\xb0");
    }
    let arrp = a.payload as *const Arr;
    let n = (*arrp).len;
    let k = i.payload as i64;
    if k < 0 || k as u64 >= n {
        err_begin(line);
        err_text(b"\xe6\x95\xb0\xe7\xbb\x84\xe4\xb8\x8b\xe6\xa0\x87\xe8\xb6\x8a\xe7\x95\x8c: ");
        err_value(i);
        err_text(b" (\xe9\x95\xbf\xe5\xba\xa6 ");
        err_value(Value {
            tag: TAG_INT,
            payload: n,
        });
        err_text(b")");
        err_end(line)
    }
    *(*arrp).data.add(k as usize) = *val;
}

/* ------------------------------------------------------------------ */
/* Objects                                                             */
/* ------------------------------------------------------------------ */

#[no_mangle]
pub unsafe extern "C" fn rt_new_obj(
    cls: u64,
    nf: u64,
    names: *const *const Str,
    vals: *const Value,
    class_name: *const Str,
    out: *mut Value,
) {
    let o = alloc(core::mem::size_of::<Obj>()) as *mut Obj;
    let fields = if nf == 0 {
        core::ptr::null_mut()
    } else {
        alloc((nf as usize) * core::mem::size_of::<Field>()) as *mut Field
    };
    let mut i = 0u64;
    while i < nf {
        fields.add(i as usize).write(Field {
            name: *names.add(i as usize),
            value: *vals.add(i as usize),
        });
        i += 1;
    }
    (*o).cls = cls;
    (*o).nf = nf;
    (*o).fields = fields;
    (*o).name = class_name;
    *out = Value {
        tag: TAG_OBJ,
        payload: o as u64,
    };
}

#[no_mangle]
pub unsafe extern "C" fn rt_field_get(
    obj: *const Value,
    name: *const Str,
    out: *mut Value,
    line: u64,
) {
    let o = *obj;
    if o.tag != TAG_OBJ {
        err_begin(line);
        err_text(b"\xe5\xad\x97\xe6\xae\xb5\xe8\xae\xbf\xe9\x97\xae\xe8\xa6\x81\xe6\xb1\x82\xe5\xaf\xb9\xe8\xb1\xa1\xef\xbc\x8c\xe5\xae\x9e\xe9\x99\x85\xe4\xb8\xba ");
        err_type(o);
        err_end(line)
    }
    let op = o.payload as *const Obj;
    let nf = (*op).nf;
    let f = (*op).fields;
    let mut i = 0u64;
    while i < nf {
        if str_eq((*f.add(i as usize)).name, name) {
            *out = (*f.add(i as usize)).value;
            return;
        }
        i += 1;
    }
    err_begin(line);
    err_text("类型 ".as_bytes());
    err_text(str_bytes((*op).name));
    err_text(" 没有字段 ".as_bytes());
    err_text(str_bytes(name));
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_field_set(
    obj: *const Value,
    name: *const Str,
    val: *const Value,
    line: u64,
) {
    let o = *obj;
    if o.tag != TAG_OBJ {
        err_begin(line);
        err_text(b"\xe5\xad\x97\xe6\xae\xb5\xe8\xb5\x8b\xe5\x80\xbc\xe8\xa6\x81\xe6\xb1\x82\xe5\xaf\xb9\xe8\xb1\xa1\xef\xbc\x8c\xe5\xae\x9e\xe9\x99\x85\xe4\xb8\xba ");
        err_type(o);
        err_end(line)
    }
    let op = o.payload as *const Obj;
    let nf = (*op).nf;
    let f = (*op).fields;
    let mut i = 0u64;
    while i < nf {
        if str_eq((*f.add(i as usize)).name, name) {
            (*f.add(i as usize)).value = *val;
            return;
        }
        i += 1;
    }
    err_begin(line);
    err_text("类型 ".as_bytes());
    err_text(str_bytes((*op).name));
    err_text(" 没有字段 ".as_bytes());
    err_text(str_bytes(name));
    err_end(line)
}

#[no_mangle]
pub unsafe extern "C" fn rt_field_get_at(
    obj: *const Value,
    cls: u64,
    off: u64,
    out: *mut Value,
    line: u64,
) {
    let o = *obj;
    if o.tag != TAG_OBJ {
        err_begin(line);
        err_text(b"\xe5\xad\x97\xe6\xae\xb5\xe8\xae\xbf\xe9\x97\xae\xe8\xa6\x81\xe6\xb1\x82\xe5\xaf\xb9\xe8\xb1\xa1\xef\xbc\x8c\xe5\xae\x9e\xe9\x99\x85\xe4\xb8\xba ");
        err_type(o);
        err_end(line)
    }
    let op = o.payload as *const Obj;
    if (*op).cls != cls || off >= (*op).nf {
        error_str(line, b"\xe5\xad\x97\xe6\xae\xb5\xe8\xae\xbf\xe9\x97\xae\xe7\xb1\xbb\xe5\x9e\x8b\xe4\xb8\x8d\xe5\x8c\xb9\xe9\x85\x8d");
    }
    *out = (*(*op).fields.add(off as usize)).value;
}

#[no_mangle]
pub unsafe extern "C" fn rt_field_set_at(
    obj: *const Value,
    cls: u64,
    off: u64,
    val: *const Value,
    line: u64,
) {
    let o = *obj;
    if o.tag != TAG_OBJ {
        err_begin(line);
        err_text(b"\xe5\xad\x97\xe6\xae\xb5\xe8\xb5\x8b\xe5\x80\xbc\xe8\xa6\x81\xe6\xb1\x82\xe5\xaf\xb9\xe8\xb1\xa1\xef\xbc\x8c\xe5\xae\x9e\xe9\x99\x85\xe4\xb8\xba ");
        err_type(o);
        err_end(line)
    }
    let op = o.payload as *const Obj;
    if (*op).cls != cls || off >= (*op).nf {
        error_str(line, b"\xe5\xad\x97\xe6\xae\xb5\xe8\xb5\x8b\xe5\x80\xbc\xe7\xb1\xbb\xe5\x9e\x8b\xe4\xb8\x8d\xe5\x8c\xb9\xe9\x85\x8d");
    }
    (*(*op).fields.add(off as usize)).value = *val;
}

#[no_mangle]
pub unsafe extern "C" fn rt_fop_at(
    obj: *const Value,
    cls: u64,
    off: u64,
    rhs: *const Value,
    op: u64,
    line: u64,
) {
    let o = *obj;
    if o.tag != TAG_OBJ {
        err_begin(line);
        err_text("字段赋值要求对象，实际为 ".as_bytes());
        err_type(o);
        err_end(line)
    }
    let op_obj = o.payload as *const Obj;
    if (*op_obj).cls != cls || off >= (*op_obj).nf {
        error_str(line, b"\xe5\xad\x97\xe6\xae\xb5\xe8\xb5\x8b\xe5\x80\xbc\xe7\xb1\xbb\xe5\x9e\x8b\xe4\xb8\x8d\xe5\x8c\xb9\xe9\x85\x8d");
    }
    let slot = &mut (*(*op_obj).fields.add(off as usize)).value as *mut Value;
    let mut out = Value {
        tag: TAG_NIL,
        payload: 0,
    };
    match op {
        0 => rt_add(&mut out, slot, rhs, line),
        1 => rt_sub(&mut out, slot, rhs, line),
        2 => rt_mul(&mut out, slot, rhs, line),
        3 => rt_div(&mut out, slot, rhs, line),
        _ => rt_mod(&mut out, slot, rhs, line),
    }
    *slot = out;
}

/* ------------------------------------------------------------------ */
/* libc-ish helpers for Rust core / compiler builtins                  */
/* ------------------------------------------------------------------ */

/// Freestanding builds have no unwinder; the personality routine is only
/// referenced from debug tables, so an empty definition is sufficient.
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}

#[no_mangle]
pub unsafe extern "C" fn memcpy(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    let mut i = 0usize;
    while i < n {
        *dst.add(i) = *src.add(i);
        i += 1;
    }
    dst
}

#[no_mangle]
pub unsafe extern "C" fn memmove(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    if (dst as usize) < (src as usize) {
        memcpy(dst, src, n)
    } else {
        let mut i = n;
        while i != 0 {
            i -= 1;
            *dst.add(i) = *src.add(i);
        }
        dst
    }
}

#[no_mangle]
pub unsafe extern "C" fn memset(dst: *mut u8, c: i32, n: usize) -> *mut u8 {
    let mut i = 0usize;
    while i < n {
        *dst.add(i) = c as u8;
        i += 1;
    }
    dst
}

#[no_mangle]
pub unsafe extern "C" fn bcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    memcmp(a, b, n)
}

#[no_mangle]
pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    let mut i = 0usize;
    while i < n {
        let x = *a.add(i);
        let y = *b.add(i);
        if x != y {
            return x as i32 - y as i32;
        }
        i += 1;
    }
    0
}
