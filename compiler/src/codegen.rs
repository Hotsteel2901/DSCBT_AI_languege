use std::collections::{HashMap, HashSet};
use std::str::FromStr;

use anyhow::{anyhow, Context, Result};
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::entities::StackSlot;
use cranelift_codegen::ir::types;
use cranelift_codegen::ir::TrapCode;
use cranelift_codegen::ir::{AbiParam, GlobalValue, InstBuilder, MemFlags, Signature, Type, Value};
use cranelift_codegen::isa::lookup;
use cranelift_codegen::settings::{self, Configurable};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Variable};
use cranelift_module::{default_libcall_names, DataDescription, DataId, FuncId, Linkage, Module};
use cranelift_object::{ObjectBuilder, ObjectModule};
use target_lexicon::Triple;

use crate::ast::*;
use crate::diag::{CResult, CompileError, Span};
use crate::sem::ModuleInfo;
use crate::stats::Stats;

pub const TAG_NIL: i64 = 0;
pub const TAG_INT: i64 = 1;
pub const TAG_FLT: i64 = 2;
pub const TAG_STR: i64 = 3;
pub const TAG_BOL: i64 = 4;
pub const TAG_ARR: i64 = 5;
pub const TAG_OBJ: i64 = 6;

const VALUE_SIZE: i64 = 16;
const FIELD_SIZE: i64 = 24;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Target {
    Linux,
    Windows,
}

impl Target {
    pub fn triple(self) -> &'static str {
        match self {
            Target::Linux => "x86_64-unknown-linux-gnu",
            Target::Windows => "x86_64-pc-windows-gnu",
        }
    }

    pub fn object_ext(self) -> &'static str {
        match self {
            Target::Linux => "o",
            Target::Windows => "obj",
        }
    }

    pub fn exe_ext(self) -> &'static str {
        match self {
            Target::Linux => "",
            Target::Windows => ".exe",
        }
    }

    pub fn runtime_bytes(self) -> &'static [u8] {
        match self {
            Target::Linux => include_bytes!(concat!(env!("OUT_DIR"), "/libdsc_runtime_linux.a")),
            Target::Windows => {
                include_bytes!(concat!(env!("OUT_DIR"), "/libdsc_runtime_windows.a"))
            }
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Val {
    pub tag: Value,
    pub payload: Value,
}

struct LocalVar {
    tag: Variable,
    payload: Variable,
}

pub struct Codegen<'a> {
    module: ObjectModule,
    info: &'a ModuleInfo,
    ptr_ty: Type,
    rt: HashMap<&'static str, FuncId>,
    user_ids: Vec<FuncId>,
    main_id: FuncId,
    start_id: FuncId,
    globals_data: DataId,
    global_slots: HashMap<String, u32>,
    global_names: Vec<String>,
    strings: HashMap<Vec<u8>, DataId>,
    class_names_data: Vec<DataId>,
    next_data: usize,
    global_class: HashMap<String, Option<u32>>,
    pub stats: Stats,
}

struct FnState {
    ptr_ty: Type,
    out_ptr: Option<Value>,
    is_main: bool,
    locals: HashMap<String, LocalVar>,
    visible: HashSet<String>,
    local_class: HashMap<String, Option<u32>>,
    globals_gv: GlobalValue,
    data_cache: HashMap<DataId, GlobalValue>,
    scratch_pool: Vec<StackSlot>,
    scratch_depth: usize,
    next_var: u32,
}

impl FnState {
    fn mark(&self) -> usize {
        self.scratch_depth
    }

    fn release(&mut self, mark: usize) {
        self.scratch_depth = mark;
    }

    fn scratch(&mut self, b: &mut FunctionBuilder) -> StackSlot {
        if self.scratch_depth < self.scratch_pool.len() {
            let s = self.scratch_pool[self.scratch_depth];
            self.scratch_depth += 1;
            s
        } else {
            let s = b.create_sized_stack_slot(cranelift_codegen::ir::StackSlotData::new(
                cranelift_codegen::ir::StackSlotKind::ExplicitSlot,
                VALUE_SIZE as u32,
                3,
            ));
            self.scratch_pool.push(s);
            self.scratch_depth += 1;
            s
        }
    }

    fn new_var(&mut self, b: &mut FunctionBuilder) -> Variable {
        let v = Variable::from_u32(self.next_var);
        self.next_var += 1;
        b.declare_var(v, types::I64);
        v
    }
}

const RT_SIGS: &[(&str, usize, bool)] = &[
    ("rt_exit", 1, false),
    ("rt_add", 4, false),
    ("rt_sub", 4, false),
    ("rt_mul", 4, false),
    ("rt_div", 4, false),
    ("rt_mod", 4, false),
    ("rt_neg", 3, false),
    ("rt_not", 2, false),
    ("rt_truthy", 1, true),
    ("rt_eq", 2, true),
    ("rt_compare", 4, true),
    ("rt_print_value", 1, false),
    ("rt_print_space", 0, false),
    ("rt_print_newline", 0, false),
    ("rt_print_i64", 1, false),
    ("rt_print_bool", 1, false),
    ("rt_print_f64", 1, false),
    ("rt_print_bytes", 2, false),
    ("rt_input", 2, false),
    ("rt_type", 2, false),
    ("rt_len", 3, false),
    ("rt_new_array", 3, false),
    ("rt_get", 4, false),
    ("rt_set", 4, false),
    ("rt_new_obj", 6, false),
    ("rt_field_get", 4, false),
    ("rt_field_set", 4, false),
    ("rt_field_get_at", 5, false),
    ("rt_field_set_at", 5, false),
    ("rt_fop_at", 6, false),
    ("rt_inc", 2, false),
];

impl<'a> Codegen<'a> {
    pub fn new(info: &'a ModuleInfo, target: Target, program: &Program) -> Result<Self> {
        let mut flags = settings::builder();
        flags
            .set("opt_level", "speed")
            .map_err(|e| anyhow!("设置优化级别失败: {e}"))?;
        let _ = flags.set("is_pic", "false");
        let triple =
            Triple::from_str(target.triple()).map_err(|e| anyhow!("无法解析目标三元组: {e:?}"))?;
        let isa = lookup(triple)
            .map_err(|e| anyhow!("不支持的目标架构: {e}"))?
            .finish(settings::Flags::new(flags))?;
        let mut ob = ObjectBuilder::new(isa, "dsc_user", default_libcall_names())?;
        ob.per_function_section(true);
        let module = ObjectModule::new(ob);
        let ptr_ty = module.isa().pointer_type();

        let mut cg = Codegen {
            module,
            info,
            ptr_ty,
            rt: HashMap::new(),
            user_ids: Vec::new(),
            main_id: FuncId::from_u32(0),
            start_id: FuncId::from_u32(0),
            globals_data: DataId::from_u32(0),
            global_slots: HashMap::new(),
            global_names: Vec::new(),
            strings: HashMap::new(),
            class_names_data: Vec::new(),
            next_data: 0,
            global_class: HashMap::new(),
            stats: Stats::default(),
        };

        let main_sig = cg.make_sig(&[]);
        cg.main_id = cg
            .module
            .declare_function("dsc_main", Linkage::Local, &main_sig)?;
        cg.start_id = cg
            .module
            .declare_function("dsc_entry", Linkage::Export, &main_sig)?;

        for (name, nparams, returns) in RT_SIGS {
            let mut sig = cg.make_sig(&vec![ptr_ty; *nparams]);
            if *returns {
                sig.returns.push(AbiParam::new(types::I64));
            }
            let id = cg.module.declare_function(name, Linkage::Import, &sig)?;
            cg.rt.insert(name, id);
        }

        for (i, f) in info.funcs.iter().enumerate() {
            let sig = cg.make_user_sig(f.params.len());
            let id = cg
                .module
                .declare_function(&format!("dsc_fn_{i}"), Linkage::Local, &sig)?;
            cg.user_ids.push(id);
        }

        cg.globals_data = cg
            .module
            .declare_data("dsc_globals", Linkage::Local, true, false)?;

        let mut names: Vec<String> = info.global_names.iter().cloned().collect();
        names.sort();
        for n in names {
            cg.global_slot_internal(&n);
        }

        cg.register_strings(program);
        cg.register_class_names()?;
        Ok(cg)
    }

    fn make_sig(&self, params: &[Type]) -> Signature {
        let mut sig = Signature::new(self.module.isa().default_call_conv());
        for &p in params {
            sig.params.push(AbiParam::new(p));
        }
        sig
    }

    fn make_user_sig(&self, nparams: usize) -> Signature {
        if nparams <= 8 {
            self.make_sig(&vec![self.ptr_ty; nparams + 1])
        } else {
            self.make_sig(&vec![self.ptr_ty; 2])
        }
    }

    fn global_slot_internal(&mut self, name: &str) -> u32 {
        if let Some(s) = self.global_slots.get(name) {
            return *s;
        }
        let s = self.global_names.len() as u32;
        self.global_names.push(name.to_string());
        self.global_slots.insert(name.to_string(), s);
        s
    }

    fn global_slot(&mut self, name: &str) -> u32 {
        self.global_slot_internal(name)
    }

    fn register_strings(&mut self, program: &Program) {
        let mut all = Vec::new();
        for s in &self.info.classes {
            for f in &s.fields {
                all.push(f.name.as_bytes().to_vec());
            }
        }
        for f in &self.info.funcs {
            collect_strings_stmts(&f.def.body, &mut all);
        }
        collect_strings_stmts(&program.stmts, &mut all);
        // Deduplicate while keeping deterministic order.
        let mut seen = HashSet::new();
        for b in all {
            if seen.insert(b.clone()) {
                let _ = self.intern_str(&b);
            }
        }
    }

    fn register_class_names(&mut self) -> Result<()> {
        for (i, cls) in self.info.classes.iter().enumerate() {
            let name = format!("dsc_cls_{i}_names");
            let id = self
                .module
                .declare_data(&name, Linkage::Local, false, false)?;
            let mut desc = DataDescription::new();
            desc.set_align(8);
            if cls.fields.is_empty() {
                desc.define(Box::new([]));
            } else {
                // Pointers must live in an initialized section (relocations
                // cannot be applied to .bss), so start from explicit zero
                // bytes rather than `define_zeroinit`.
                desc.define(vec![0u8; cls.fields.len() * 8].into_boxed_slice());
                for (fi, f) in cls.fields.iter().enumerate() {
                    let sid = self.strings[&f.name.as_bytes().to_vec()];
                    let gv = self.module.declare_data_in_data(sid, &mut desc);
                    desc.write_data_addr((fi * 8) as u32, gv, 0);
                }
            }
            self.module.define_data(id, &desc)?;
            self.class_names_data.push(id);
        }
        Ok(())
    }

    fn intern_str(&mut self, bytes: &[u8]) -> DataId {
        if let Some(id) = self.strings.get(bytes) {
            return *id;
        }
        let name = format!("dsc_str_{}", self.next_data);
        self.next_data += 1;
        let id = self
            .module
            .declare_data(&name, Linkage::Local, false, false)
            .expect("declare string data");
        let mut data = Vec::with_capacity(8 + bytes.len());
        data.extend_from_slice(&(bytes.len() as u64).to_le_bytes());
        data.extend_from_slice(bytes);
        let mut desc = DataDescription::new();
        desc.set_align(8);
        desc.define(data.into_boxed_slice());
        self.module
            .define_data(id, &desc)
            .expect("define string data");
        self.strings.insert(bytes.to_vec(), id);
        id
    }

    fn data_gv(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, id: DataId) -> GlobalValue {
        if let Some(gv) = fs.data_cache.get(&id) {
            return *gv;
        }
        let gv = self.module.declare_data_in_func(id, &mut b.func);
        fs.data_cache.insert(id, gv);
        gv
    }

    fn str_addr(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, bytes: &[u8]) -> Value {
        let id = self.intern_str(bytes);
        let gv = self.data_gv(fs, b, id);
        b.ins().global_value(fs.ptr_ty, gv)
    }

    fn globals_addr(&mut self, fs: &mut FnState, b: &mut FunctionBuilder) -> Value {
        b.ins().global_value(fs.ptr_ty, fs.globals_gv)
    }

    fn load_global(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, slot: u32) -> Val {
        let base = self.globals_addr(fs, b);
        let addr = b.ins().iadd_imm(base, (slot as i64) * VALUE_SIZE);
        let mf = MemFlags::new();
        let tag = b.ins().load(types::I64, mf, addr, 0);
        let payload = b.ins().load(types::I64, mf, addr, 8);
        Val { tag, payload }
    }

    fn store_global(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, slot: u32, v: Val) {
        let base = self.globals_addr(fs, b);
        let addr = b.ins().iadd_imm(base, (slot as i64) * VALUE_SIZE);
        let mf = MemFlags::new();
        b.ins().store(mf, v.tag, addr, 0);
        b.ins().store(mf, v.payload, addr, 8);
    }

    fn store_local(&self, b: &mut FunctionBuilder, fs: &FnState, name: &str, v: Val) {
        if let Some(l) = fs.locals.get(name) {
            b.def_var(l.tag, v.tag);
            b.def_var(l.payload, v.payload);
        }
    }

    fn load_local(&self, b: &mut FunctionBuilder, fs: &FnState, name: &str) -> Val {
        let l = &fs.locals[name];
        Val {
            tag: b.use_var(l.tag),
            payload: b.use_var(l.payload),
        }
    }

    fn const_val(&self, b: &mut FunctionBuilder, ty: i64, payload: i64) -> Val {
        Val {
            tag: b.ins().iconst(types::I64, ty),
            payload: b.ins().iconst(types::I64, payload),
        }
    }

    fn nil_val(&self, b: &mut FunctionBuilder) -> Val {
        self.const_val(b, TAG_NIL, 0)
    }

    fn int_val(&self, b: &mut FunctionBuilder, v: i64) -> Val {
        self.const_val(b, TAG_INT, v)
    }

    fn bool_val(&self, b: &mut FunctionBuilder, v: bool) -> Val {
        self.const_val(b, TAG_BOL, v as i64)
    }

    fn str_val(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, bytes: &[u8]) -> Val {
        let p = self.str_addr(fs, b, bytes);
        Val {
            tag: b.ins().iconst(types::I64, TAG_STR),
            payload: p,
        }
    }

    fn store_to_slot(&self, b: &mut FunctionBuilder, fs: &FnState, slot: StackSlot, v: Val) {
        let addr = b.ins().stack_addr(fs.ptr_ty, slot, 0);
        let mf = MemFlags::new();
        b.ins().store(mf, v.tag, addr, 0);
        b.ins().store(mf, v.payload, addr, 8);
    }

    fn load_from_slot(&self, b: &mut FunctionBuilder, fs: &FnState, slot: StackSlot) -> Val {
        let addr = b.ins().stack_addr(fs.ptr_ty, slot, 0);
        let mf = MemFlags::new();
        Val {
            tag: b.ins().load(types::I64, mf, addr, 0),
            payload: b.ins().load(types::I64, mf, addr, 8),
        }
    }

    fn materialize(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, v: Val) -> Value {
        let slot = fs.scratch(b);
        self.store_to_slot(b, fs, slot, v);
        b.ins().stack_addr(fs.ptr_ty, slot, 0)
    }

    fn call_rt(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        name: &'static str,
        args: &[Value],
    ) -> Option<Value> {
        let id = self.rt[name];
        let fref = self.module.declare_func_in_func(id, &mut b.func);
        let call = b.ins().call(fref, args);
        let _ = fs;
        b.inst_results(call).first().copied()
    }

    pub fn compile(mut self, program: &Program) -> Result<(Vec<u8>, Stats)> {
        for i in 0..self.info.funcs.len() {
            self.define_user_function(i)?;
        }
        self.define_main(program)?;
        self.define_start()?;
        self.define_globals()?;
        let stats = std::mem::take(&mut self.stats);
        let product = self.module.finish();
        Ok((product.emit().context("生成目标文件失败")?, stats))
    }

    fn define_globals(&mut self) -> Result<()> {
        let size = self.global_names.len() * VALUE_SIZE as usize;
        let mut desc = DataDescription::new();
        desc.set_align(8);
        if size == 0 {
            desc.define(Box::new([]));
        } else {
            desc.define_zeroinit(size);
        }
        self.module.define_data(self.globals_data, &desc)?;
        Ok(())
    }

    fn init_fn_state(
        &mut self,
        b: &mut FunctionBuilder,
        is_main: bool,
        out_ptr: Option<Value>,
    ) -> FnState {
        let gv = self
            .module
            .declare_data_in_func(self.globals_data, &mut b.func);
        FnState {
            ptr_ty: self.ptr_ty,
            out_ptr,
            is_main,
            locals: HashMap::new(),
            visible: HashSet::new(),
            local_class: HashMap::new(),
            globals_gv: gv,
            data_cache: HashMap::new(),
            scratch_pool: Vec::new(),
            scratch_depth: 0,
            next_var: 0,
        }
    }

    fn define_user_function(&mut self, idx: usize) -> Result<()> {
        let def = self.info.funcs[idx].def.clone();
        let nparams = def.params.len();
        let fid = self.user_ids[idx];
        let mut ctx = self.module.make_context();
        ctx.func.signature = self.make_user_sig(nparams);
        let mut fctx = FunctionBuilderContext::new();
        let mut b = FunctionBuilder::new(&mut ctx.func, &mut fctx);
        let entry = b.create_block();
        b.append_block_params_for_function_params(entry);
        b.switch_to_block(entry);

        let params = b.block_params(entry).to_vec();
        let out_ptr = if nparams <= 8 {
            params[nparams]
        } else {
            params[1]
        };
        let mut fs = self.init_fn_state(&mut b, false, Some(out_ptr));

        // Collect all names that may become function locals.
        let mut potentials = HashSet::new();
        for p in &def.params {
            potentials.insert(p.name.clone());
        }
        collect_assigned_stmts(&def.body, &mut potentials);

        for name in &potentials {
            let tv = fs.new_var(&mut b);
            let pv = fs.new_var(&mut b);
            let nilt = b.ins().iconst(types::I64, TAG_NIL);
            let nilp = b.ins().iconst(types::I64, 0);
            b.def_var(tv, nilt);
            b.def_var(pv, nilp);
            fs.locals.insert(
                name.clone(),
                LocalVar {
                    tag: tv,
                    payload: pv,
                },
            );
        }
        for name in &potentials {
            fs.local_class.insert(name.clone(), None);
        }

        // Copy arguments from the caller's scratch slots.
        let mut name_index: Vec<(String, usize)> = Vec::new();
        for (i, p) in def.params.iter().enumerate() {
            name_index.push((p.name.clone(), i));
        }
        for (name, i) in &name_index {
            let arg_ptr = if nparams <= 8 {
                params[*i]
            } else {
                b.ins().iadd_imm(params[0], (*i as i64) * VALUE_SIZE)
            };
            let mf = MemFlags::new();
            let tag = b.ins().load(types::I64, mf, arg_ptr, 0);
            let payload = b.ins().load(types::I64, mf, arg_ptr, 8);
            self.store_local(&mut b, &fs, name, Val { tag, payload });
            fs.visible.insert(name.clone());
        }

        let terminated = self.emit_block(&mut fs, &mut b, &def.body)?;
        if !terminated {
            let zero = self.int_val(&mut b, 0);
            self.emit_return_value(&mut fs, &mut b, zero)?;
        }
        b.seal_all_blocks();
        b.finalize();
        self.module.define_function(fid, &mut ctx)?;
        Ok(())
    }

    fn define_main(&mut self, program: &Program) -> Result<()> {
        let mut ctx = self.module.make_context();
        ctx.func.signature = self.make_sig(&[]);
        let mut fctx = FunctionBuilderContext::new();
        let mut b = FunctionBuilder::new(&mut ctx.func, &mut fctx);
        let entry = b.create_block();
        b.append_block_params_for_function_params(entry);
        b.switch_to_block(entry);
        let mut fs = self.init_fn_state(&mut b, true, None);

        let body: Vec<Stmt> = program
            .stmts
            .iter()
            .filter(|s| !matches!(s.kind, StmtKind::Func(_) | StmtKind::Class(_)))
            .cloned()
            .collect();
        let _ = self.emit_block(&mut fs, &mut b, &body)?;
        b.ins().return_(&[]);
        b.seal_all_blocks();
        b.finalize();
        self.module.define_function(self.main_id, &mut ctx)?;
        Ok(())
    }

    fn define_start(&mut self) -> Result<()> {
        let mut ctx = self.module.make_context();
        ctx.func.signature = self.make_sig(&[]);
        let mut fctx = FunctionBuilderContext::new();
        let mut b = FunctionBuilder::new(&mut ctx.func, &mut fctx);
        let entry = b.create_block();
        b.switch_to_block(entry);
        let mids = self.main_id;
        let fref = self.module.declare_func_in_func(mids, &mut b.func);
        b.ins().call(fref, &[]);
        let zero = b.ins().iconst(types::I64, 0);
        let exit = self.rt["rt_exit"];
        let fref = self.module.declare_func_in_func(exit, &mut b.func);
        b.ins().call(fref, &[zero]);
        b.ins().return_(&[]);
        b.seal_all_blocks();
        b.finalize();
        self.module.define_function(self.start_id, &mut ctx)?;
        Ok(())
    }

    /* ------------------------------------------------------------------ */
    /* Statements                                                          */
    /* ------------------------------------------------------------------ */

    fn emit_block(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        stmts: &[Stmt],
    ) -> CResult<bool> {
        for s in stmts {
            if self.emit_stmt(fs, b, s)? {
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn emit_stmt(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        stmt: &Stmt,
    ) -> CResult<bool> {
        match &stmt.kind {
            StmtKind::Assign(lv, rv) => {
                self.emit_assign(fs, b, lv, rv, stmt.span)?;
                Ok(false)
            }
            StmtKind::Expr(e) => {
                let _ = self.emit_expr(fs, b, e)?;
                Ok(false)
            }
            StmtKind::If {
                cond,
                then_body,
                else_body,
            } => self.emit_if(fs, b, cond, then_body, else_body.as_deref(), stmt.span),
            StmtKind::While { cond, body } => self.emit_while(fs, b, cond, body, stmt.span),
            StmtKind::For { var, limit, body } => self.emit_for(fs, b, var, limit, body, stmt.span),
            StmtKind::Return(v) => {
                if fs.is_main {
                    return Err(CompileError::at(stmt.span, "return 不能出现在顶层"));
                }
                if let Some(Expr {
                    kind: ExprKind::Call(name, args),
                    ..
                }) = v
                {
                    if self.info.func_map.contains_key(name) && fs.out_ptr.is_some() {
                        let out = fs.out_ptr.unwrap();
                        self.emit_user_call(fs, b, name, args, stmt.span, Some(out))?;
                        self.stats.bump("返回值槽前传（return f(...)）");
                        b.ins().return_(&[]);
                        return Ok(true);
                    }
                }
                let val = if let Some(e) = v {
                    self.emit_expr(fs, b, e)?
                } else {
                    self.int_val(b, 0)
                };
                self.emit_return_value(fs, b, val)?;
                Ok(true)
            }
            StmtKind::Block(body) => self.emit_block(fs, b, body),
            StmtKind::Func(_) | StmtKind::Class(_) => {
                Err(CompileError::at(stmt.span, "函数/类定义必须位于顶层"))
            }
        }
    }

    fn emit_return_value(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        v: Val,
    ) -> CResult<()> {
        let out = fs
            .out_ptr
            .ok_or_else(|| CompileError::plain("内部错误: 缺少返回槽"))?;
        let mf = MemFlags::new();
        b.ins().store(mf, v.tag, out, 0);
        b.ins().store(mf, v.payload, out, 8);
        b.ins().return_(&[]);
        Ok(())
    }

    fn emit_if(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        cond: &Expr,
        then_body: &[Stmt],
        else_body: Option<&Stmt>,
        span: Span,
    ) -> CResult<bool> {
        let c = self.emit_cond(fs, b, cond)?;
        let then_b = b.create_block();
        let else_b = b.create_block();
        let join = b.create_block();
        b.ins().brif(c, then_b, &[], else_b, &[]);

        b.switch_to_block(then_b);
        let t_term = self.emit_block(fs, b, then_body)?;
        if !t_term {
            b.ins().jump(join, &[]);
        }

        b.switch_to_block(else_b);
        let e_term = match else_body {
            Some(s) => self.emit_stmt(fs, b, s)?,
            None => false,
        };
        if !e_term {
            b.ins().jump(join, &[]);
        }

        if t_term && e_term {
            b.switch_to_block(join);
            b.ins().trap(TrapCode::unwrap_user(1));
            return Ok(true);
        }
        b.switch_to_block(join);
        let _ = span;
        Ok(false)
    }

    fn emit_while(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        cond: &Expr,
        body: &[Stmt],
        span: Span,
    ) -> CResult<bool> {
        let header = b.create_block();
        let body_b = b.create_block();
        let exit = b.create_block();
        b.ins().jump(header, &[]);
        b.switch_to_block(header);
        let c = self.emit_cond(fs, b, cond)?;
        b.ins().brif(c, body_b, &[], exit, &[]);

        b.switch_to_block(body_b);
        let term = self.emit_block(fs, b, body)?;
        if !term {
            b.ins().jump(header, &[]);
        }
        b.switch_to_block(exit);
        if term && is_truthy_const(cond) {
            b.ins().trap(TrapCode::unwrap_user(1));
            return Ok(true);
        }
        let _ = span;
        Ok(false)
    }

    fn emit_for(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        var: &str,
        limit: &Expr,
        body: &[Stmt],
        span: Span,
    ) -> CResult<bool> {
        // The range limit is evaluated exactly once.
        let lim = self.emit_expr(fs, b, limit)?;
        let lim_tag = fs.new_var(b);
        let lim_payload = fs.new_var(b);
        b.def_var(lim_tag, lim.tag);
        b.def_var(lim_payload, lim.payload);

        let ctr = fs.new_var(b);
        let zero = b.ins().iconst(types::I64, 0);
        b.def_var(ctr, zero);

        let mut body_assigned = HashSet::new();
        collect_assigned_stmts(body, &mut body_assigned);
        let body_assigns_var = body_assigned.contains(var);

        // The loop variable is a local inside functions and a global in main.
        let is_local = !fs.is_main && fs.locals.contains_key(var);
        if is_local {
            fs.visible.insert(var.to_string());
            let it = b.ins().iconst(types::I64, TAG_INT);
            let zv = b.ins().iconst(types::I64, 0);
            self.store_local(
                b,
                fs,
                var,
                Val {
                    tag: it,
                    payload: zv,
                },
            );
        } else {
            let slot = self.global_slot(var);
            let zv = self.int_val(b, 0);
            self.store_global(fs, b, slot, zv);
        }

        let header = b.create_block();
        let body_b = b.create_block();
        let exit = b.create_block();
        b.ins().jump(header, &[]);

        b.switch_to_block(header);
        let ctr_val = b.use_var(ctr);
        let lhs = Val {
            tag: b.ins().iconst(types::I64, TAG_INT),
            payload: ctr_val,
        };
        let rhs = Val {
            tag: b.use_var(lim_tag),
            payload: b.use_var(lim_payload),
        };
        let c = self.emit_cmp_raw(fs, b, BinOp::Lt, lhs, rhs, span)?;
        b.ins().brif(c, body_b, &[], exit, &[]);

        b.switch_to_block(body_b);
        let ctr_now = b.use_var(ctr);
        let it = b.ins().iconst(types::I64, TAG_INT);
        if is_local {
            self.store_local(
                b,
                fs,
                var,
                Val {
                    tag: it,
                    payload: ctr_now,
                },
            );
            fs.local_class.insert(var.to_string(), None);
        } else {
            let slot = self.global_slot(var);
            self.store_global(
                fs,
                b,
                slot,
                Val {
                    tag: it,
                    payload: ctr_now,
                },
            );
        }
        let term = self.emit_block(fs, b, body)?;
        if !term {
            let cur = b.use_var(ctr);
            let next = b.ins().iadd_imm(cur, 1);
            b.def_var(ctr, next);
            b.ins().jump(header, &[]);
        }
        b.switch_to_block(exit);
        // v3 increments the visible loop variable each iteration when the body
        // does not assign it, so it ends up holding the range limit.
        if !body_assigns_var {
            let final_ctr = b.use_var(ctr);
            let it = b.ins().iconst(types::I64, TAG_INT);
            if is_local {
                self.store_local(
                    b,
                    fs,
                    var,
                    Val {
                        tag: it,
                        payload: final_ctr,
                    },
                );
            } else {
                let slot = self.global_slot(var);
                self.store_global(
                    fs,
                    b,
                    slot,
                    Val {
                        tag: it,
                        payload: final_ctr,
                    },
                );
            }
        }
        Ok(false)
    }

    fn emit_assign(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        lv: &LValue,
        rv: &Expr,
        span: Span,
    ) -> CResult<()> {
        match lv {
            LValue::Name(name) => {
                let cls = self.infer_class(fs, rv);
                let prev = self.class_of_var(fs, name);
                let new_cls = match (prev, cls) {
                    (Some(p), Some(c)) if p == c => Some(c),
                    (Some(_), _) => None,
                    (None, c) => c,
                };
                let val = self.emit_expr(fs, b, rv)?;
                let location = self.resolve_store(fs, name);
                match location {
                    Loc::Local => self.store_local(b, fs, name, val),
                    Loc::Global(slot) => self.store_global(fs, b, slot, val),
                }
                if fs.is_main {
                    self.global_class.insert(name.clone(), new_cls);
                } else {
                    fs.local_class.insert(name.clone(), new_cls);
                }
                let _ = span;
                Ok(())
            }
            LValue::Index(arr, idx) => {
                let av = self.emit_expr(fs, b, arr)?;
                let iv = self.emit_expr(fs, b, idx)?;
                let vv = self.emit_expr(fs, b, rv)?;
                self.emit_index_set(fs, b, av, iv, vv, span)?;
                Ok(())
            }
            LValue::Field(obj_expr, name) => {
                let cls = self.infer_class(fs, obj_expr);
                if let Some(c) = cls {
                    if !self.class_has_field(c, name) {
                        return Err(CompileError::at(
                            span,
                            format!(
                                "类型 {} 没有字段 {}",
                                self.info.classes[c as usize].name, name
                            ),
                        ));
                    }
                    if let Some((op, lhs_obj, rhs)) = compound_field(obj_expr, rv, name) {
                        if self.infer_class(fs, lhs_obj) == Some(c) && is_simple(rhs) {
                            let ov = self.emit_expr(fs, b, lhs_obj)?;
                            let rvv = self.emit_expr(fs, b, rhs)?;
                            let mark = fs.mark();
                            let ovp = self.materialize(fs, b, ov);
                            let rvp = self.materialize(fs, b, rvv);
                            let off = self.field_offset(c, name).unwrap();
                            let opv = b.ins().iconst(types::I64, op);
                            let clsv = b.ins().iconst(types::I64, c as i64);
                            let offv = b.ins().iconst(types::I64, off as i64);
                            let linev = b.ins().iconst(types::I64, span.line as i64);
                            self.call_rt(fs, b, "rt_fop_at", &[ovp, clsv, offv, rvp, opv, linev]);
                            fs.release(mark);
                            self.stats.bump("字段复合赋值融合（rt_fop_at）");
                            return Ok(());
                        }
                    }
                }
                let ov = self.emit_expr(fs, b, obj_expr)?;
                let vv = self.emit_expr(fs, b, rv)?;
                self.emit_field_set(fs, b, cls, name, ov, vv, span)?;
                Ok(())
            }
        }
    }

    fn resolve_store(&mut self, fs: &mut FnState, name: &str) -> Loc {
        if !fs.is_main && fs.visible.contains(name) {
            return Loc::Local;
        }
        if self.global_slots.contains_key(name) {
            return Loc::Global(self.global_slots[name]);
        }
        if !fs.is_main && fs.locals.contains_key(name) {
            fs.visible.insert(name.to_string());
            return Loc::Local;
        }
        Loc::Global(self.global_slot(name))
    }

    fn resolve_read(&mut self, fs: &FnState, name: &str) -> Loc {
        if !fs.is_main && fs.visible.contains(name) {
            Loc::Local
        } else {
            Loc::Global(self.global_slot(name))
        }
    }

    fn class_of_var(&self, fs: &FnState, name: &str) -> Option<u32> {
        if fs.is_main {
            self.global_class.get(name).copied().flatten()
        } else {
            fs.local_class.get(name).copied().flatten()
        }
    }

    fn infer_class(&self, fs: &FnState, e: &Expr) -> Option<u32> {
        match &e.kind {
            ExprKind::ClassLit(name, _) => self.info.class_map.get(name).copied(),
            ExprKind::Ident(name) => self.class_of_var(fs, name),
            _ => None,
        }
    }

    fn class_has_field(&self, cls: u32, name: &str) -> bool {
        self.info.classes[cls as usize]
            .fields
            .iter()
            .any(|f| f.name == name)
    }

    fn field_offset(&self, cls: u32, name: &str) -> Option<usize> {
        self.info.classes[cls as usize]
            .fields
            .iter()
            .position(|f| f.name == name)
    }

    /* ------------------------------------------------------------------ */
    /* Expressions                                                         */
    /* ------------------------------------------------------------------ */

    fn emit_expr(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, e: &Expr) -> CResult<Val> {
        match &e.kind {
            ExprKind::Int(v) => {
                self.stats.bump("常量整数立即数编码");
                Ok(self.int_val(b, *v))
            }
            ExprKind::Float(v) => Ok(self.const_val(b, TAG_FLT, v.to_bits() as i64)),
            ExprKind::Str(s) => Ok(self.str_val(fs, b, s)),
            ExprKind::Bool(v) => Ok(self.bool_val(b, *v)),
            ExprKind::Ident(name) => match self.resolve_read(fs, name) {
                Loc::Local => Ok(self.load_local(b, fs, name)),
                Loc::Global(slot) => Ok(self.load_global(fs, b, slot)),
            },
            ExprKind::Bin(op, a, b2) => {
                if matches!(op, BinOp::And | BinOp::Or) {
                    return self.emit_logic_value(fs, b, *op, a, b2, e.span);
                }
                let l = self.emit_expr(fs, b, a)?;
                let r = self.emit_expr(fs, b, b2)?;
                match op {
                    BinOp::Add => self.emit_arith(fs, b, "rt_add", l, r, e.span),
                    BinOp::Sub => self.emit_arith(fs, b, "rt_sub", l, r, e.span),
                    BinOp::Mul => self.emit_arith(fs, b, "rt_mul", l, r, e.span),
                    BinOp::Div => self.emit_divmod(fs, b, true, l, r, e.span),
                    BinOp::Mod => self.emit_divmod(fs, b, false, l, r, e.span),
                    BinOp::Eq => {
                        let c = self.emit_cmp_raw(fs, b, BinOp::Eq, l, r, e.span)?;
                        Ok(Val {
                            tag: b.ins().iconst(types::I64, TAG_BOL),
                            payload: c,
                        })
                    }
                    BinOp::Ne => {
                        let c = self.emit_cmp_raw(fs, b, BinOp::Ne, l, r, e.span)?;
                        Ok(Val {
                            tag: b.ins().iconst(types::I64, TAG_BOL),
                            payload: c,
                        })
                    }
                    BinOp::Lt | BinOp::Gt | BinOp::Le | BinOp::Ge => {
                        let c = self.emit_cmp_raw(fs, b, *op, l, r, e.span)?;
                        Ok(Val {
                            tag: b.ins().iconst(types::I64, TAG_BOL),
                            payload: c,
                        })
                    }
                    BinOp::And | BinOp::Or => unreachable!(),
                }
            }
            ExprKind::Un(op, a) => {
                let v = self.emit_expr(fs, b, a)?;
                match op {
                    UnOp::Not => {
                        let c = self.emit_truthy(fs, b, v)?;
                        let inv = b.ins().bxor_imm(c, 1);
                        Ok(Val {
                            tag: b.ins().iconst(types::I64, TAG_BOL),
                            payload: inv,
                        })
                    }
                    UnOp::Neg => self.emit_neg(fs, b, v, e.span),
                }
            }
            ExprKind::Call(name, args) => self.emit_call(fs, b, name, args, e.span),
            ExprKind::Array(elems) => self.emit_array(fs, b, elems),
            ExprKind::Index(arr, idx) => {
                let av = self.emit_expr(fs, b, arr)?;
                let iv = self.emit_expr(fs, b, idx)?;
                self.emit_index_get(fs, b, av, iv, e.span)
            }
            ExprKind::Field(obj, name) => {
                let ov = self.emit_expr(fs, b, obj)?;
                let cls = self.infer_class(fs, obj);
                self.emit_field_get(fs, b, cls, name, ov, e.span)
            }
            ExprKind::ClassLit(name, fields) => self.emit_class_lit(fs, b, name, fields, e.span),
        }
    }

    fn emit_logic_value(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        op: BinOp,
        a: &Expr,
        b2: &Expr,
        span: Span,
    ) -> CResult<Val> {
        let lhs = self.emit_expr(fs, b, a)?;
        let mark = fs.mark();
        let slot = fs.scratch(b);
        self.store_to_slot(b, fs, slot, lhs);
        let c = self.emit_truthy(fs, b, lhs)?;
        let eval_b = b.create_block();
        let keep = b.create_block();
        let join = b.create_block();
        if op == BinOp::And {
            b.ins().brif(c, eval_b, &[], keep, &[]);
        } else {
            b.ins().brif(c, keep, &[], eval_b, &[]);
        }
        b.switch_to_block(eval_b);
        let rhs = self.emit_expr(fs, b, b2)?;
        self.store_to_slot(b, fs, slot, rhs);
        b.ins().jump(join, &[]);
        b.switch_to_block(keep);
        b.ins().jump(join, &[]);
        b.switch_to_block(join);
        let v = self.load_from_slot(b, fs, slot);
        fs.release(mark);
        self.stats.bump("and/or 短路分支生成");
        let _ = span;
        Ok(v)
    }

    fn emit_truthy(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, v: Val) -> CResult<Value> {
        let fast = b.create_block();
        let slow = b.create_block();
        let join = b.create_block();
        b.append_block_param(join, types::I64);
        let is_int = b.ins().icmp_imm(IntCC::Equal, v.tag, TAG_INT);
        let is_bol = b.ins().icmp_imm(IntCC::Equal, v.tag, TAG_BOL);
        let fast_cond = b.ins().bor(is_int, is_bol);
        b.ins().brif(fast_cond, fast, &[], slow, &[]);
        b.switch_to_block(fast);
        let nz = b.ins().icmp_imm(IntCC::NotEqual, v.payload, 0);
        let nz64 = b.ins().uextend(types::I64, nz);
        b.ins().jump(join, &[nz64]);
        b.switch_to_block(slow);
        let mark = fs.mark();
        let p = self.materialize(fs, b, v);
        let r = self
            .call_rt(fs, b, "rt_truthy", &[p])
            .unwrap_or_else(|| b.ins().iconst(types::I64, 0));
        let nz = b.ins().icmp_imm(IntCC::NotEqual, r, 0);
        let nz64 = b.ins().uextend(types::I64, nz);
        fs.release(mark);
        b.ins().jump(join, &[nz64]);
        b.switch_to_block(join);
        Ok(b.block_params(join)[0])
    }

    fn emit_cond(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, e: &Expr) -> CResult<Value> {
        match &e.kind {
            ExprKind::Bin(op @ (BinOp::Lt | BinOp::Gt | BinOp::Le | BinOp::Ge), a, c) => {
                let l = self.emit_expr(fs, b, a)?;
                let r = self.emit_expr(fs, b, c)?;
                self.emit_cmp_raw(fs, b, *op, l, r, e.span)
            }
            ExprKind::Bin(op @ (BinOp::Eq | BinOp::Ne), a, c) => {
                let l = self.emit_expr(fs, b, a)?;
                let r = self.emit_expr(fs, b, c)?;
                self.emit_cmp_raw(fs, b, *op, l, r, e.span)
            }
            ExprKind::Bin(BinOp::And, a, c) => {
                let lhs = self.emit_cond(fs, b, a)?;
                let rhs_b = b.create_block();
                let false_b = b.create_block();
                let join = b.create_block();
                b.append_block_param(join, types::I64);
                b.ins().brif(lhs, rhs_b, &[], false_b, &[]);
                b.switch_to_block(rhs_b);
                let rhs = self.emit_cond(fs, b, c)?;
                let rhs64 = b.ins().uextend(types::I64, rhs);
                b.ins().jump(join, &[rhs64]);
                b.switch_to_block(false_b);
                let z = b.ins().iconst(types::I64, 0);
                b.ins().jump(join, &[z]);
                b.switch_to_block(join);
                let v64 = b.block_params(join)[0];
                Ok(b.ins().icmp_imm(IntCC::NotEqual, v64, 0))
            }
            ExprKind::Bin(BinOp::Or, a, c) => {
                let lhs = self.emit_cond(fs, b, a)?;
                let true_b = b.create_block();
                let rhs_b = b.create_block();
                let join = b.create_block();
                b.append_block_param(join, types::I64);
                b.ins().brif(lhs, true_b, &[], rhs_b, &[]);
                b.switch_to_block(true_b);
                let one = b.ins().iconst(types::I64, 1);
                b.ins().jump(join, &[one]);
                b.switch_to_block(rhs_b);
                let rhs = self.emit_cond(fs, b, c)?;
                let rhs64 = b.ins().uextend(types::I64, rhs);
                b.ins().jump(join, &[rhs64]);
                b.switch_to_block(join);
                let v64 = b.block_params(join)[0];
                Ok(b.ins().icmp_imm(IntCC::NotEqual, v64, 0))
            }
            ExprKind::Un(UnOp::Not, a) => {
                let c = self.emit_cond(fs, b, a)?;
                Ok(b.ins().bxor_imm(c, 1))
            }
            ExprKind::Bool(v) => Ok(b.ins().iconst(types::I8, *v as i64)),
            ExprKind::Int(v) => {
                let iv = b.ins().iconst(types::I64, *v);
                Ok(b.ins().icmp_imm(IntCC::NotEqual, iv, 0))
            }
            ExprKind::Str(s) => {
                let sv = b.ins().iconst(types::I64, s.len() as i64);
                Ok(b.ins().icmp_imm(IntCC::NotEqual, sv, 0))
            }
            _ => {
                let v = self.emit_expr(fs, b, e)?;
                let t = self.emit_truthy(fs, b, v)?;
                Ok(b.ins().icmp_imm(IntCC::NotEqual, t, 0))
            }
        }
    }

    fn emit_arith(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        rt_name: &'static str,
        l: Val,
        r: Val,
        span: Span,
    ) -> CResult<Val> {
        let fast = b.create_block();
        let slow = b.create_block();
        let join = b.create_block();
        b.append_block_param(join, types::I64);
        b.append_block_param(join, types::I64);
        let li = b.ins().icmp_imm(IntCC::Equal, l.tag, TAG_INT);
        let ri = b.ins().icmp_imm(IntCC::Equal, r.tag, TAG_INT);
        let both = b.ins().band(li, ri);
        b.ins().brif(both, fast, &[], slow, &[]);

        b.switch_to_block(fast);
        let out = match rt_name {
            "rt_add" => b.ins().iadd(l.payload, r.payload),
            "rt_sub" => b.ins().isub(l.payload, r.payload),
            _ => b.ins().imul(l.payload, r.payload),
        };
        let itag = b.ins().iconst(types::I64, TAG_INT);
        b.ins().jump(join, &[itag, out]);

        b.switch_to_block(slow);
        let mark = fs.mark();
        let lp = self.materialize(fs, b, l);
        let rp = self.materialize(fs, b, r);
        let out_slot = fs.scratch(b);
        let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
        let line = b.ins().iconst(types::I64, span.line as i64);
        self.call_rt(fs, b, rt_name, &[outp, lp, rp, line]);
        let res = self.load_from_slot(b, fs, out_slot);
        fs.release(mark);
        b.ins().jump(join, &[res.tag, res.payload]);

        b.switch_to_block(join);
        let ps = b.block_params(join);
        let _ = span;
        self.stats.bump(match rt_name {
            "rt_add" => "整数加法内联快速路径",
            "rt_sub" => "整数减法内联快速路径",
            _ => "整数乘法内联快速路径",
        });
        Ok(Val {
            tag: ps[0],
            payload: ps[1],
        })
    }

    fn emit_divmod(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        is_div: bool,
        l: Val,
        r: Val,
        span: Span,
    ) -> CResult<Val> {
        let fast = b.create_block();
        let slow = b.create_block();
        let join = b.create_block();
        b.append_block_param(join, types::I64);
        b.append_block_param(join, types::I64);
        let li = b.ins().icmp_imm(IntCC::Equal, l.tag, TAG_INT);
        let ri = b.ins().icmp_imm(IntCC::Equal, r.tag, TAG_INT);
        let ints = b.ins().band(li, ri);
        let nz = b.ins().icmp_imm(IntCC::NotEqual, r.payload, 0);
        let lmin = b.ins().icmp_imm(IntCC::Equal, l.payload, i64::MIN);
        let rm1 = b.ins().icmp_imm(IntCC::Equal, r.payload, -1);
        let ovf = b.ins().band(lmin, rm1);
        let novf = b.ins().bxor_imm(ovf, 1);
        let c1 = b.ins().band(ints, nz);
        let cond = b.ins().band(c1, novf);
        b.ins().brif(cond, fast, &[], slow, &[]);

        b.switch_to_block(fast);
        let out = if is_div {
            b.ins().sdiv(l.payload, r.payload)
        } else {
            b.ins().srem(l.payload, r.payload)
        };
        let itag = b.ins().iconst(types::I64, TAG_INT);
        b.ins().jump(join, &[itag, out]);

        b.switch_to_block(slow);
        let mark = fs.mark();
        let lp = self.materialize(fs, b, l);
        let rp = self.materialize(fs, b, r);
        let out_slot = fs.scratch(b);
        let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
        let line = b.ins().iconst(types::I64, span.line as i64);
        self.call_rt(
            fs,
            b,
            if is_div { "rt_div" } else { "rt_mod" },
            &[outp, lp, rp, line],
        );
        let res = self.load_from_slot(b, fs, out_slot);
        fs.release(mark);
        b.ins().jump(join, &[res.tag, res.payload]);

        b.switch_to_block(join);
        let ps = b.block_params(join);
        self.stats.bump(if is_div {
            "整数除法内联快速路径（零/溢出保护）"
        } else {
            "整数取模内联快速路径（零保护）"
        });
        Ok(Val {
            tag: ps[0],
            payload: ps[1],
        })
    }

    fn emit_cmp_raw(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        op: BinOp,
        l: Val,
        r: Val,
        span: Span,
    ) -> CResult<Value> {
        let fast = b.create_block();
        let slow = b.create_block();
        let join = b.create_block();
        b.append_block_param(join, types::I64);
        let li = b.ins().icmp_imm(IntCC::Equal, l.tag, TAG_INT);
        let ri = b.ins().icmp_imm(IntCC::Equal, r.tag, TAG_INT);
        let both = b.ins().band(li, ri);
        b.ins().brif(both, fast, &[], slow, &[]);

        b.switch_to_block(fast);
        let cc = match op {
            BinOp::Eq => IntCC::Equal,
            BinOp::Ne => IntCC::NotEqual,
            BinOp::Lt => IntCC::SignedLessThan,
            BinOp::Gt => IntCC::SignedGreaterThan,
            BinOp::Le => IntCC::SignedLessThanOrEqual,
            _ => IntCC::SignedGreaterThanOrEqual,
        };
        let c = b.ins().icmp(cc, l.payload, r.payload);
        let c64 = b.ins().uextend(types::I64, c);
        b.ins().jump(join, &[c64]);

        b.switch_to_block(slow);
        let mark = fs.mark();
        let lp = self.materialize(fs, b, l);
        let rp = self.materialize(fs, b, r);
        let res = match op {
            BinOp::Eq | BinOp::Ne => {
                let eq = self
                    .call_rt(fs, b, "rt_eq", &[lp, rp])
                    .unwrap_or_else(|| b.ins().iconst(types::I64, 0));
                if op == BinOp::Ne {
                    b.ins().bxor_imm(eq, 1)
                } else {
                    eq
                }
            }
            _ => {
                let opv = b.ins().iconst(
                    types::I64,
                    match op {
                        BinOp::Lt => 0,
                        BinOp::Gt => 1,
                        BinOp::Le => 2,
                        _ => 3,
                    },
                );
                let line = b.ins().iconst(types::I64, span.line as i64);
                self.call_rt(fs, b, "rt_compare", &[lp, rp, opv, line])
                    .unwrap_or_else(|| b.ins().iconst(types::I64, 0))
            }
        };
        fs.release(mark);
        b.ins().jump(join, &[res]);

        b.switch_to_block(join);
        self.stats.bump(match op {
            BinOp::Eq | BinOp::Ne => "整数相等比较快速路径",
            _ => "整数大小比较快速路径",
        });
        Ok(b.block_params(join)[0])
    }

    fn emit_neg(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        v: Val,
        span: Span,
    ) -> CResult<Val> {
        let fast = b.create_block();
        let slow = b.create_block();
        let join = b.create_block();
        b.append_block_param(join, types::I64);
        b.append_block_param(join, types::I64);
        let is_int = b.ins().icmp_imm(IntCC::Equal, v.tag, TAG_INT);
        b.ins().brif(is_int, fast, &[], slow, &[]);
        b.switch_to_block(fast);
        let out = b.ins().ineg(v.payload);
        let itag = b.ins().iconst(types::I64, TAG_INT);
        b.ins().jump(join, &[itag, out]);
        b.switch_to_block(slow);
        let mark = fs.mark();
        let vp = self.materialize(fs, b, v);
        let out_slot = fs.scratch(b);
        let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
        let line = b.ins().iconst(types::I64, span.line as i64);
        self.call_rt(fs, b, "rt_neg", &[outp, vp, line]);
        let res = self.load_from_slot(b, fs, out_slot);
        fs.release(mark);
        b.ins().jump(join, &[res.tag, res.payload]);
        b.switch_to_block(join);
        let ps = b.block_params(join);
        self.stats.bump("整数取负内联快速路径");
        Ok(Val {
            tag: ps[0],
            payload: ps[1],
        })
    }

    fn emit_call(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        name: &str,
        args: &[Expr],
        span: Span,
    ) -> CResult<Val> {
        match name {
            "print" => self.emit_print(fs, b, args),
            "input" => {
                let prompt = if args.len() == 1 {
                    self.emit_expr(fs, b, &args[0])?
                } else {
                    self.nil_val(b)
                };
                let mark = fs.mark();
                let pp = self.materialize(fs, b, prompt);
                let out_slot = fs.scratch(b);
                let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
                self.call_rt(fs, b, "rt_input", &[pp, outp]);
                let res = self.load_from_slot(b, fs, out_slot);
                fs.release(mark);
                Ok(res)
            }
            "len" | "type" => {
                let v = self.emit_expr(fs, b, &args[0])?;
                let mark = fs.mark();
                let vp = self.materialize(fs, b, v);
                let out_slot = fs.scratch(b);
                let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
                if name == "len" {
                    let line = b.ins().iconst(types::I64, span.line as i64);
                    self.call_rt(fs, b, "rt_len", &[vp, outp, line]);
                } else {
                    self.call_rt(fs, b, "rt_type", &[vp, outp]);
                }
                let res = self.load_from_slot(b, fs, out_slot);
                fs.release(mark);
                Ok(res)
            }
            _ => {
                let _ = span;
                self.emit_user_call(fs, b, name, args, span, None)
            }
        }
    }

    fn emit_user_call(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        name: &str,
        args: &[Expr],
        span: Span,
        out_override: Option<Value>,
    ) -> CResult<Val> {
        let idx = *self
            .info
            .func_map
            .get(name)
            .ok_or_else(|| CompileError::at(span, format!("未定义的函数 {}", name)))?;
        let nparams = self.info.funcs[idx as usize].params.len();
        let fid = self.user_ids[idx as usize];
        let mark = fs.mark();
        let mut vals = Vec::with_capacity(args.len());
        for a in args {
            vals.push(self.emit_expr(fs, b, a)?);
        }
        let mut arg_ptrs = Vec::with_capacity(vals.len());
        if nparams <= 8 {
            for v in &vals {
                arg_ptrs.push(self.materialize(fs, b, *v));
            }
        } else {
            let size = (vals.len() as u32) * VALUE_SIZE as u32;
            let slot = b.create_sized_stack_slot(cranelift_codegen::ir::StackSlotData::new(
                cranelift_codegen::ir::StackSlotKind::ExplicitSlot,
                size,
                3,
            ));
            let base = b.ins().stack_addr(fs.ptr_ty, slot, 0);
            for (i, v) in vals.iter().enumerate() {
                let addr = b.ins().iadd_imm(base, (i as i64) * VALUE_SIZE);
                let mf = MemFlags::new();
                b.ins().store(mf, v.tag, addr, 0);
                b.ins().store(mf, v.payload, addr, 8);
            }
            arg_ptrs.push(base);
        }
        let out_slot = if out_override.is_none() {
            Some(fs.scratch(b))
        } else {
            None
        };
        let outp = match out_override {
            Some(p) => p,
            None => b.ins().stack_addr(fs.ptr_ty, out_slot.unwrap(), 0),
        };
        arg_ptrs.push(outp);
        let fref = self.module.declare_func_in_func(fid, &mut b.func);
        b.ins().call(fref, &arg_ptrs);
        let res = if let Some(slot) = out_slot {
            self.load_from_slot(b, fs, slot)
        } else {
            self.nil_val(b)
        };
        fs.release(mark);
        self.global_class.clear();
        if nparams <= 8 {
            self.stats.bump("函数参数寄存器直传（≤8 参数）");
        }
        if nparams == 0 {
            self.stats.bump("零参数函数调用特化");
        }
        Ok(res)
    }

    fn emit_print(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        args: &[Expr],
    ) -> CResult<Val> {
        if args.is_empty() {
            self.call_rt(fs, b, "rt_print_newline", &[]);
            self.stats.bump("print 零参数特化");
            return Ok(self.nil_val(b));
        }
        // Evaluate every argument before printing anything (v3 semantics).
        let mut vals: Vec<Option<Val>> = Vec::with_capacity(args.len());
        for a in args {
            if const_of_expr(a).is_some() {
                vals.push(None);
            } else {
                vals.push(Some(self.emit_expr(fs, b, a)?));
            }
        }
        for (i, a) in args.iter().enumerate() {
            if i > 0 {
                self.call_rt(fs, b, "rt_print_space", &[]);
            }
            if let Some(k) = const_of_expr(a) {
                self.emit_print_const(fs, b, &k);
            } else if let Some(v) = vals[i] {
                let mark = fs.mark();
                let p = self.materialize(fs, b, v);
                self.call_rt(fs, b, "rt_print_value", &[p]);
                fs.release(mark);
            }
        }
        self.call_rt(fs, b, "rt_print_newline", &[]);
        if args.len() == 1 {
            self.stats.bump("print 单参数特化");
        } else {
            self.stats.bump("print 多参数先求值后输出（语义保持）");
        }
        Ok(self.nil_val(b))
    }

    fn emit_print_const(&mut self, fs: &mut FnState, b: &mut FunctionBuilder, k: &KConst) {
        match k {
            KConst::Int(v) => {
                let iv = b.ins().iconst(types::I64, *v);
                self.call_rt(fs, b, "rt_print_i64", &[iv]);
                self.stats.bump("print 常量整数特化");
            }
            KConst::Bool(v) => {
                let iv = b.ins().iconst(types::I64, *v as i64);
                self.call_rt(fs, b, "rt_print_bool", &[iv]);
                self.stats.bump("print 常量布尔特化");
            }
            KConst::Float(v) => {
                let iv = b.ins().iconst(types::I64, v.to_bits() as i64);
                self.call_rt(fs, b, "rt_print_f64", &[iv]);
                self.stats.bump("print 常量浮点特化");
            }
            KConst::Str(s) => {
                let p = self.str_addr(fs, b, s);
                let p2 = b.ins().iadd_imm(p, 8);
                let n = b.ins().iconst(types::I64, s.len() as i64);
                self.call_rt(fs, b, "rt_print_bytes", &[p2, n]);
                self.stats.bump("print 常量字符串特化（零格式化）");
            }
        }
    }

    fn emit_array(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        elems: &[Expr],
    ) -> CResult<Val> {
        let mark = fs.mark();
        let n = elems.len();
        let slot = b.create_sized_stack_slot(cranelift_codegen::ir::StackSlotData::new(
            cranelift_codegen::ir::StackSlotKind::ExplicitSlot,
            ((n as u32) * VALUE_SIZE as u32).max(16),
            3,
        ));
        let base = b.ins().stack_addr(fs.ptr_ty, slot, 0);
        for (i, e) in elems.iter().enumerate() {
            let v = self.emit_expr(fs, b, e)?;
            let addr = b.ins().iadd_imm(base, (i as i64) * VALUE_SIZE);
            let mf = MemFlags::new();
            b.ins().store(mf, v.tag, addr, 0);
            b.ins().store(mf, v.payload, addr, 8);
        }
        let out_slot = fs.scratch(b);
        let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
        let nv = b.ins().iconst(types::I64, n as i64);
        if n == 0 {
            let null = b.ins().iconst(fs.ptr_ty, 0);
            self.call_rt(fs, b, "rt_new_array", &[nv, null, outp]);
        } else {
            self.call_rt(fs, b, "rt_new_array", &[nv, base, outp]);
        }
        let res = self.load_from_slot(b, fs, out_slot);
        fs.release(mark);
        self.stats.bump("数组字面量常数长度分配");
        Ok(res)
    }

    fn emit_index_get(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        av: Val,
        iv: Val,
        span: Span,
    ) -> CResult<Val> {
        let fast = b.create_block();
        let slow = b.create_block();
        let join = b.create_block();
        b.append_block_param(join, types::I64);
        b.append_block_param(join, types::I64);
        let is_arr = b.ins().icmp_imm(IntCC::Equal, av.tag, TAG_ARR);
        let is_int = b.ins().icmp_imm(IntCC::Equal, iv.tag, TAG_INT);
        let ok = b.ins().band(is_arr, is_int);
        b.ins().brif(ok, fast, &[], slow, &[]);

        b.switch_to_block(fast);
        let mf = MemFlags::new();
        let len = b.ins().load(types::I64, mf, av.payload, 0);
        let nonneg = b
            .ins()
            .icmp_imm(IntCC::SignedGreaterThanOrEqual, iv.payload, 0);
        let inb = b.ins().icmp(IntCC::UnsignedLessThan, iv.payload, len);
        let bounds = b.ins().band(nonneg, inb);
        let fast2 = b.create_block();
        let slow2 = b.create_block();
        b.ins().brif(bounds, fast2, &[], slow2, &[]);
        b.switch_to_block(fast2);
        let data = b.ins().load(fs.ptr_ty, mf, av.payload, 8);
        let off = b.ins().imul_imm(iv.payload, VALUE_SIZE);
        let addr = b.ins().iadd(data, off);
        let tag = b.ins().load(types::I64, mf, addr, 0);
        let payload = b.ins().load(types::I64, mf, addr, 8);
        b.ins().jump(join, &[tag, payload]);
        b.switch_to_block(slow2);
        let mark = fs.mark();
        let ap = self.materialize(fs, b, av);
        let ip = self.materialize(fs, b, iv);
        let out_slot = fs.scratch(b);
        let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
        let line = b.ins().iconst(types::I64, span.line as i64);
        self.call_rt(fs, b, "rt_get", &[ap, ip, outp, line]);
        let res = self.load_from_slot(b, fs, out_slot);
        fs.release(mark);
        b.ins().jump(join, &[res.tag, res.payload]);

        b.switch_to_block(slow);
        let mark = fs.mark();
        let ap = self.materialize(fs, b, av);
        let ip = self.materialize(fs, b, iv);
        let out_slot = fs.scratch(b);
        let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
        let line = b.ins().iconst(types::I64, span.line as i64);
        self.call_rt(fs, b, "rt_get", &[ap, ip, outp, line]);
        let res = self.load_from_slot(b, fs, out_slot);
        fs.release(mark);
        b.ins().jump(join, &[res.tag, res.payload]);

        b.switch_to_block(join);
        let ps = b.block_params(join);
        self.stats.bump("数组下标快速路径（内联边界检查）");
        Ok(Val {
            tag: ps[0],
            payload: ps[1],
        })
    }

    fn emit_index_set(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        av: Val,
        iv: Val,
        vv: Val,
        span: Span,
    ) -> CResult<()> {
        let fast = b.create_block();
        let slow = b.create_block();
        let done = b.create_block();
        let is_arr = b.ins().icmp_imm(IntCC::Equal, av.tag, TAG_ARR);
        let is_int = b.ins().icmp_imm(IntCC::Equal, iv.tag, TAG_INT);
        let ok = b.ins().band(is_arr, is_int);
        b.ins().brif(ok, fast, &[], slow, &[]);

        b.switch_to_block(fast);
        let mf = MemFlags::new();
        let len = b.ins().load(types::I64, mf, av.payload, 0);
        let nonneg = b
            .ins()
            .icmp_imm(IntCC::SignedGreaterThanOrEqual, iv.payload, 0);
        let inb = b.ins().icmp(IntCC::UnsignedLessThan, iv.payload, len);
        let bounds = b.ins().band(nonneg, inb);
        let store_b = b.create_block();
        let slow2 = b.create_block();
        b.ins().brif(bounds, store_b, &[], slow2, &[]);
        b.switch_to_block(store_b);
        let data = b.ins().load(fs.ptr_ty, mf, av.payload, 8);
        let off = b.ins().imul_imm(iv.payload, VALUE_SIZE);
        let addr = b.ins().iadd(data, off);
        b.ins().store(mf, vv.tag, addr, 0);
        b.ins().store(mf, vv.payload, addr, 8);
        b.ins().jump(done, &[]);
        b.switch_to_block(slow2);
        let mark = fs.mark();
        let ap = self.materialize(fs, b, av);
        let ip = self.materialize(fs, b, iv);
        let vp = self.materialize(fs, b, vv);
        let line = b.ins().iconst(types::I64, span.line as i64);
        self.call_rt(fs, b, "rt_set", &[ap, ip, vp, line]);
        fs.release(mark);
        b.ins().jump(done, &[]);

        b.switch_to_block(slow);
        let mark = fs.mark();
        let ap = self.materialize(fs, b, av);
        let ip = self.materialize(fs, b, iv);
        let vp = self.materialize(fs, b, vv);
        let line = b.ins().iconst(types::I64, span.line as i64);
        self.call_rt(fs, b, "rt_set", &[ap, ip, vp, line]);
        fs.release(mark);
        b.ins().jump(done, &[]);

        b.switch_to_block(done);
        self.stats.bump("数组写入快速路径（内联边界检查）");
        Ok(())
    }

    fn emit_field_get(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        cls: Option<u32>,
        name: &str,
        ov: Val,
        span: Span,
    ) -> CResult<Val> {
        if let Some(c) = cls {
            let off = self.field_offset(c, name).ok_or_else(|| {
                CompileError::at(
                    span,
                    format!(
                        "类型 {} 没有字段 {}",
                        self.info.classes[c as usize].name, name
                    ),
                )
            })?;
            let check = b.create_block();
            let fast = b.create_block();
            let slow = b.create_block();
            let join = b.create_block();
            b.append_block_param(join, types::I64);
            b.append_block_param(join, types::I64);
            let is_obj = b.ins().icmp_imm(IntCC::Equal, ov.tag, TAG_OBJ);
            b.ins().brif(is_obj, check, &[], slow, &[]);
            b.switch_to_block(check);
            let mf = MemFlags::new();
            let cls_word = b.ins().load(types::I64, mf, ov.payload, 0);
            let cls_ok = b.ins().icmp_imm(IntCC::Equal, cls_word, c as i64);
            b.ins().brif(cls_ok, fast, &[], slow, &[]);
            b.switch_to_block(fast);
            let fields = b.ins().load(fs.ptr_ty, mf, ov.payload, 16);
            let addr = b.ins().iadd_imm(fields, (off as i64) * FIELD_SIZE + 8);
            let tag = b.ins().load(types::I64, mf, addr, 0);
            let payload = b.ins().load(types::I64, mf, addr, 8);
            b.ins().jump(join, &[tag, payload]);
            b.switch_to_block(slow);
            let mark = fs.mark();
            let op = self.materialize(fs, b, ov);
            let out_slot = fs.scratch(b);
            let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
            let cv = b.ins().iconst(types::I64, c as i64);
            let ofv = b.ins().iconst(types::I64, off as i64);
            let line = b.ins().iconst(types::I64, span.line as i64);
            self.call_rt(fs, b, "rt_field_get_at", &[op, cv, ofv, outp, line]);
            let res = self.load_from_slot(b, fs, out_slot);
            fs.release(mark);
            b.ins().jump(join, &[res.tag, res.payload]);
            b.switch_to_block(join);
            let ps = b.block_params(join);
            self.stats.bump("字段访问快速路径（内联类检查）");
            self.stats.bump("静态类字段偏移访问");
            Ok(Val {
                tag: ps[0],
                payload: ps[1],
            })
        } else {
            let mark = fs.mark();
            let op = self.materialize(fs, b, ov);
            let namep = self.str_addr(fs, b, name.as_bytes());
            let out_slot = fs.scratch(b);
            let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
            let line = b.ins().iconst(types::I64, span.line as i64);
            self.call_rt(fs, b, "rt_field_get", &[op, namep, outp, line]);
            let res = self.load_from_slot(b, fs, out_slot);
            fs.release(mark);
            Ok(res)
        }
    }

    fn emit_field_set(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        cls: Option<u32>,
        name: &str,
        ov: Val,
        vv: Val,
        span: Span,
    ) -> CResult<()> {
        if let Some(c) = cls {
            let off = self.field_offset(c, name).ok_or_else(|| {
                CompileError::at(
                    span,
                    format!(
                        "类型 {} 没有字段 {}",
                        self.info.classes[c as usize].name, name
                    ),
                )
            })?;
            let check = b.create_block();
            let fast = b.create_block();
            let slow = b.create_block();
            let done = b.create_block();
            let is_obj = b.ins().icmp_imm(IntCC::Equal, ov.tag, TAG_OBJ);
            b.ins().brif(is_obj, check, &[], slow, &[]);
            b.switch_to_block(check);
            let mf = MemFlags::new();
            let cls_word = b.ins().load(types::I64, mf, ov.payload, 0);
            let cls_ok = b.ins().icmp_imm(IntCC::Equal, cls_word, c as i64);
            b.ins().brif(cls_ok, fast, &[], slow, &[]);
            b.switch_to_block(fast);
            let fields = b.ins().load(fs.ptr_ty, mf, ov.payload, 16);
            let addr = b.ins().iadd_imm(fields, (off as i64) * FIELD_SIZE + 8);
            b.ins().store(mf, vv.tag, addr, 0);
            b.ins().store(mf, vv.payload, addr, 8);
            b.ins().jump(done, &[]);
            b.switch_to_block(slow);
            let mark = fs.mark();
            let op = self.materialize(fs, b, ov);
            let vp = self.materialize(fs, b, vv);
            let cv = b.ins().iconst(types::I64, c as i64);
            let ofv = b.ins().iconst(types::I64, off as i64);
            let line = b.ins().iconst(types::I64, span.line as i64);
            self.call_rt(fs, b, "rt_field_set_at", &[op, cv, ofv, vp, line]);
            fs.release(mark);
            b.ins().jump(done, &[]);
            b.switch_to_block(done);
            self.stats.bump("字段写入快速路径（内联类检查）");
        } else {
            let mark = fs.mark();
            let op = self.materialize(fs, b, ov);
            let namep = self.str_addr(fs, b, name.as_bytes());
            let vp = self.materialize(fs, b, vv);
            let line = b.ins().iconst(types::I64, span.line as i64);
            self.call_rt(fs, b, "rt_field_set", &[op, namep, vp, line]);
            fs.release(mark);
        }
        Ok(())
    }

    fn emit_class_lit(
        &mut self,
        fs: &mut FnState,
        b: &mut FunctionBuilder,
        name: &str,
        fields: &[(String, Expr)],
        span: Span,
    ) -> CResult<Val> {
        let Some(&cid) = self.info.class_map.get(name) else {
            return Err(CompileError::at(span, format!("未定义的类型 {}", name)));
        };
        let class = self.info.classes[cid as usize].clone();
        let mut seen = HashSet::new();
        for (fname, _) in fields {
            if !self.class_has_field(cid, fname) {
                return Err(CompileError::at(
                    span,
                    format!("类型 {} 没有字段 {}", class.name, fname),
                ));
            }
            if !seen.insert(fname.clone()) {
                return Err(CompileError::at(span, format!("字段 {} 重复初始化", fname)));
            }
        }
        let mark = fs.mark();
        let nf = class.fields.len();
        let slot = b.create_sized_stack_slot(cranelift_codegen::ir::StackSlotData::new(
            cranelift_codegen::ir::StackSlotKind::ExplicitSlot,
            ((nf as u32) * VALUE_SIZE as u32).max(16),
            3,
        ));
        let base = b.ins().stack_addr(fs.ptr_ty, slot, 0);
        for (i, f) in class.fields.iter().enumerate() {
            let val = if let Some((_, e)) = fields.iter().find(|(n, _)| *n == f.name) {
                self.emit_expr(fs, b, e)?
            } else {
                match &f.ty {
                    TypeAnnot::Prim(PrimTy::Int) | TypeAnnot::Prim(PrimTy::Bool) => {
                        self.int_val(b, 0)
                    }
                    TypeAnnot::Prim(PrimTy::Float) => self.const_val(b, TAG_FLT, 0),
                    _ => self.nil_val(b),
                }
            };
            let addr = b.ins().iadd_imm(base, (i as i64) * VALUE_SIZE);
            let mf = MemFlags::new();
            b.ins().store(mf, val.tag, addr, 0);
            b.ins().store(mf, val.payload, addr, 8);
        }
        let names_id = self.class_names_data[cid as usize];
        let names_gv = self.data_gv(fs, b, names_id);
        let names = b.ins().global_value(fs.ptr_ty, names_gv);
        let out_slot = fs.scratch(b);
        let outp = b.ins().stack_addr(fs.ptr_ty, out_slot, 0);
        let cv = b.ins().iconst(types::I64, cid as i64);
        let nfv = b.ins().iconst(types::I64, nf as i64);
        let class_name = self.str_addr(fs, b, class.name.as_bytes());
        self.call_rt(
            fs,
            b,
            "rt_new_obj",
            &[cv, nfv, names, base, class_name, outp],
        );
        let res = self.load_from_slot(b, fs, out_slot);
        fs.release(mark);
        self.stats.bump("对象字面量字段按声明顺序求值");
        Ok(res)
    }
}

enum Loc {
    Local,
    Global(u32),
}

#[derive(Clone)]
enum KConst {
    Int(i64),
    Float(f64),
    Str(Vec<u8>),
    Bool(bool),
}

fn const_of_expr(e: &Expr) -> Option<KConst> {
    match &e.kind {
        ExprKind::Int(v) => Some(KConst::Int(*v)),
        ExprKind::Float(v) => Some(KConst::Float(*v)),
        ExprKind::Str(v) => Some(KConst::Str(v.clone())),
        ExprKind::Bool(v) => Some(KConst::Bool(*v)),
        _ => None,
    }
}

fn is_truthy_const(e: &Expr) -> bool {
    match &e.kind {
        ExprKind::Int(v) => *v != 0,
        ExprKind::Float(v) => *v != 0.0,
        ExprKind::Str(v) => !v.is_empty(),
        ExprKind::Bool(v) => *v,
        _ => false,
    }
}

fn compound_field<'e>(
    obj: &'e Expr,
    rv: &'e Expr,
    name: &str,
) -> Option<(i64, &'e Expr, &'e Expr)> {
    let ExprKind::Bin(op, lhs, rhs) = &rv.kind else {
        return None;
    };
    let ExprKind::Field(lhs_obj, lhs_name) = &lhs.kind else {
        return None;
    };
    if lhs_name != name || !same_expr(obj, lhs_obj) {
        return None;
    }
    let op = match op {
        BinOp::Add => 0,
        BinOp::Sub => 1,
        BinOp::Mul => 2,
        BinOp::Div => 3,
        BinOp::Mod => 4,
        _ => return None,
    };
    Some((op, lhs_obj, rhs))
}

fn is_simple(e: &Expr) -> bool {
    match &e.kind {
        ExprKind::Int(_)
        | ExprKind::Float(_)
        | ExprKind::Str(_)
        | ExprKind::Bool(_)
        | ExprKind::Ident(_) => true,
        ExprKind::Un(_, a) => is_simple(a),
        ExprKind::Bin(_, a, b) => is_simple(a) && is_simple(b),
        ExprKind::Index(a, b) => is_simple(a) && is_simple(b),
        ExprKind::Field(a, _) => is_simple(a),
        _ => false,
    }
}

fn same_expr(a: &Expr, b: &Expr) -> bool {
    match (&a.kind, &b.kind) {
        (ExprKind::Ident(x), ExprKind::Ident(y)) => x == y,
        (ExprKind::Int(x), ExprKind::Int(y)) => x == y,
        (ExprKind::Float(x), ExprKind::Float(y)) => x.to_bits() == y.to_bits(),
        (ExprKind::Str(x), ExprKind::Str(y)) => x == y,
        (ExprKind::Bool(x), ExprKind::Bool(y)) => x == y,
        (ExprKind::Bin(o1, a1, b1), ExprKind::Bin(o2, a2, b2)) => {
            o1 == o2 && same_expr(a1, a2) && same_expr(b1, b2)
        }
        (ExprKind::Un(o1, a1), ExprKind::Un(o2, a2)) => o1 == o2 && same_expr(a1, a2),
        (ExprKind::Index(a1, b1), ExprKind::Index(a2, b2)) => {
            same_expr(a1, a2) && same_expr(b1, b2)
        }
        (ExprKind::Field(a1, n1), ExprKind::Field(a2, n2)) => n1 == n2 && same_expr(a1, a2),
        _ => false,
    }
}

fn collect_strings_stmts(stmts: &[Stmt], out: &mut Vec<Vec<u8>>) {
    for s in stmts {
        match &s.kind {
            StmtKind::Assign(lv, rv) => {
                match lv {
                    LValue::Name(_) => {}
                    LValue::Index(a, b) => {
                        collect_strings_expr(a, out);
                        collect_strings_expr(b, out);
                    }
                    LValue::Field(a, _) => collect_strings_expr(a, out),
                }
                collect_strings_expr(rv, out);
            }
            StmtKind::Expr(e) | StmtKind::Return(Some(e)) => collect_strings_expr(e, out),
            StmtKind::Return(None) => {}
            StmtKind::If {
                cond,
                then_body,
                else_body,
            } => {
                collect_strings_expr(cond, out);
                collect_strings_stmts(then_body, out);
                if let Some(e) = else_body {
                    collect_strings_stmts(std::slice::from_ref(e), out);
                }
            }
            StmtKind::While { cond, body } => {
                collect_strings_expr(cond, out);
                collect_strings_stmts(body, out);
            }
            StmtKind::For { limit, body, .. } => {
                collect_strings_expr(limit, out);
                collect_strings_stmts(body, out);
            }
            StmtKind::Block(body) => collect_strings_stmts(body, out),
            StmtKind::Func(f) => collect_strings_stmts(&f.body, out),
            StmtKind::Class(_) => {}
        }
    }
}

fn collect_strings_expr(e: &Expr, out: &mut Vec<Vec<u8>>) {
    match &e.kind {
        ExprKind::Str(v) => out.push(v.clone()),
        ExprKind::Bin(_, a, b) => {
            collect_strings_expr(a, out);
            collect_strings_expr(b, out);
        }
        ExprKind::Un(_, a) => collect_strings_expr(a, out),
        ExprKind::Call(_, args) => {
            for a in args {
                collect_strings_expr(a, out);
            }
        }
        ExprKind::Array(elems) => {
            for a in elems {
                collect_strings_expr(a, out);
            }
        }
        ExprKind::Index(a, b) => {
            collect_strings_expr(a, out);
            collect_strings_expr(b, out);
        }
        ExprKind::Field(a, _) => collect_strings_expr(a, out),
        ExprKind::ClassLit(_, fields) => {
            for (_, v) in fields {
                collect_strings_expr(v, out);
            }
        }
        _ => {}
    }
}

fn collect_assigned_stmts(stmts: &[Stmt], out: &mut HashSet<String>) {
    for s in stmts {
        match &s.kind {
            StmtKind::Assign(LValue::Name(n), _) => {
                out.insert(n.clone());
            }
            StmtKind::Assign(_, _) => {}
            StmtKind::If {
                then_body,
                else_body,
                ..
            } => {
                collect_assigned_stmts(then_body, out);
                if let Some(e) = else_body {
                    collect_assigned_stmts(std::slice::from_ref(e), out);
                }
            }
            StmtKind::While { body, .. } | StmtKind::Block(body) => {
                collect_assigned_stmts(body, out);
            }
            StmtKind::For { var, body, .. } => {
                out.insert(var.clone());
                collect_assigned_stmts(body, out);
            }
            _ => {}
        }
    }
}
