//! C emission (recompile.py's Emitter): one compiled function as C, in the
//! register-signature form (sig) or through a context (ctx).

use crate::analysis::Analysis;
use crate::cs::{reg_full, reg_name, Insn, Op, OpKind};
use crate::flags::{kind_of, Flags};
use crate::func::{Func, Tgt};
use crate::program::{Program, FRAME_NAMES};
use crate::x86::*;
use crate::Refused;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Sig {
    pub ins: Vec<u8>,
    pub outs: Vec<u8>,
    pub preserved: Vec<u8>,
}

type R<T> = Result<T, Refused>;

fn refused<T>(s: String) -> R<T> {
    Err(Refused(s))
}

fn sizes(n: u32) -> u32 {
    match n {
        1 => 8,
        2 => 16,
        4 => 32,
        _ => panic!("no load or store of {} bytes", n),
    }
}

fn mask(n: u32) -> &'static str {
    match n {
        1 => "0xffu",
        2 => "0xffffu",
        4 => "0xffffffffu",
        _ => panic!("no mask of {} bytes", n),
    }
}

fn imm_mask(v: i64, size: u32) -> u64 {
    let m: u128 = (1u128 << (8 * size)) - 1;
    ((v as i128 as u128) & m) as u64
}

const X87_PREFIX: [u8; 6] = [0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65];

/// (opcode, modrm) of an x87 instruction, else None.
pub fn x87_op(i: &Insn) -> Option<(u8, u8)> {
    let b = &i.bytes;
    let mut k = 0;
    while k < b.len() && X87_PREFIX.contains(&b[k]) {
        k += 1;
    }
    if k + 1 < b.len() && (0xd8..=0xdf).contains(&b[k]) {
        return Some((b[k], b[k + 1]));
    }
    None
}

/// xmm0..7's number, for an operand that is one.
fn xmm_of(o: &Op) -> Option<u8> {
    let OpKind::Reg(r) = o.kind else { return None };
    crate::cs::reg_name(r).strip_prefix("xmm").and_then(|n| n.parse::<u8>().ok()).filter(|n| *n < 8)
}

fn sse_arith(m: &str) -> &'static str {
    match &m[..3] {
        "add" => "+",
        "sub" => "-",
        "mul" => "*",
        _ => "/",
    }
}

/// (predicate, packed) of an SSE2 compare of doubles: cmpeqsd and its like
/// by name, or cmpsd and cmppd with the predicate as a third operand.
fn sse_cmp(m: &str, ops: &[Op]) -> Option<(u32, bool)> {
    let body = m.strip_prefix("cmp")?;
    let (pred, pd) = if let Some(p) = body.strip_suffix("sd") { (p, false) } else { (body.strip_suffix("pd")?, true) };
    if pred.is_empty() {
        return if ops.len() == 3 && ops[2].is_imm() { Some(((ops[2].imm() & 7) as u32, pd)) } else { None };
    }
    let k = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"].iter().position(|p| *p == pred)?;
    if ops.len() != 2 {
        return None;
    }
    Some((k as u32, pd))
}

fn g(k: u8) -> String {
    format!("aot_fget(F, {})", k)
}

fn s_(k: u8, v: &str) -> String {
    format!("aot_fset(F, {}, {});", k, v)
}

fn arith(opn: &str, a: &str, b: &str) -> String {
    if let Some(o) = opn.strip_suffix('r') {
        return format!("({}) {} ({})", b, o, a);
    }
    format!("({}) {} ({})", a, opn, b)
}

fn arith_op(reg: u8) -> Option<&'static str> {
    Some(match reg {
        0 => "+",
        1 => "*",
        4 => "-",
        5 => "-r",
        6 => "/",
        7 => "/r",
        _ => return None,
    })
}

/// A resume entry's C name (issue #138).
pub fn resume_name(x: u32) -> String {
    format!("AOT_SYM(resume_{:08x})", x)
}

pub fn reent_name(va: u32) -> String {
    format!("AOT_SYM(reent_{:08x})", va)
}

pub struct Emitter<'p, 'a> {
    p: &'p Program<'a>,
    pub mode: &'static str,
    sigs: &'p HashMap<u32, Sig>,
    index: RefCell<Option<HashMap<u32, usize>>>,
    f: Option<Rc<Func>>,
    flags: Option<Flags>,
    sig: Option<Sig>,
    df: HashMap<u32, Option<u8>>,
    pub ana: Option<Analysis<'p, 'a>>,
    trace_all: Option<u32>,
    /// A resume entry being written (issue #138): where a native hands the
    /// call back, and the stack offset there from the entry's esp. Or, with
    /// reent, the function's return sites (reent_<va>, entered at one).
    resume: Option<Vec<(u32, i64)>>,
    reent: bool,
}

impl<'p, 'a> Emitter<'p, 'a> {
    pub fn new(p: &'p Program<'a>, mode: &'static str, sigs: &'p HashMap<u32, Sig>) -> Emitter<'p, 'a> {
        let trace_all = std::env::var("AOT_TRACE_ALL").ok().filter(|s| !s.is_empty()).map(|s| {
            u32::from_str_radix(s.trim_start_matches("0x"), 16).expect("AOT_TRACE_ALL: a hex address")
        });
        Emitter {
            p,
            mode,
            sigs,
            index: RefCell::new(None),
            f: None,
            flags: None,
            sig: None,
            df: HashMap::new(),
            ana: None,
            trace_all,
            resume: None,
            reent: false,
        }
    }

    /// AOT_FN, or AOT_FNC for a function off AOT_HOT's list (Program::is_cold).
    fn fn_kw(&self, va: u32) -> &'static str {
        if self.p.is_cold(va) { "AOT_FNC" } else { "AOT_FN" }
    }

    pub fn name(&self, va: u32) -> String {
        format!("AOT_SYM({}_{:08x})", self.mode, va)
    }

    /// All eight registers in and out through AotRegs: ctx mode, or a
    /// resume entry (whose calls stay sig mode's).
    fn regs_ctx(&self) -> bool {
        self.mode == "ctx" || self.resume.is_some() || self.loose()
    }

    /// The function being written is a loose one (Program::loose): its
    /// registers through AotRegs in either mode, no esp0 to check a ret
    /// against, and where control goes on is what it returns.
    fn loose(&self) -> bool {
        self.f.as_ref().is_some_and(|f| self.p.is_loose(f.entry))
    }

    /// resume_<x> (issue #138): `va`'s code entered at x, where a native that
    /// did the function's first instructions hands the call back
    /// (resume_original), with all eight registers as there; its ret returns
    /// them as ctx mode's does (esp after the ret). Sig mode only; the
    /// analysis must be one without skipped saves (the native stored them).
    pub fn resume_fn(&mut self, va: u32, x: u32, sp: i64) -> R<String> {
        self.resume = Some(vec![(x, sp)]);
        let r = self.function(va);
        self.resume = None;
        r
    }

    /// reent_<va> (issue #138): `va`'s code entered at one of its return
    /// sites (`at`, each with its stack offset from the entry's esp), with all
    /// eight registers as there, as resume_fn's is: where a thread that
    /// stopped in compiled code (a shim's yield, the C frames above dropped)
    /// goes on when the scheduler runs it again.
    pub fn reent_fn(&mut self, va: u32, sites: &[(u32, i64)]) -> R<String> {
        self.resume = Some(sites.to_vec());
        self.reent = true;
        let r = self.function(va);
        self.resume = None;
        self.reent = false;
        r
    }

    fn f(&self) -> &Func {
        self.f.as_ref().unwrap()
    }

    fn flags(&self) -> &Flags {
        self.flags.as_ref().unwrap()
    }

    fn sig(&self) -> &Sig {
        self.sig.as_ref().unwrap()
    }

    // Operands
    fn reg_get(&self, i: &Insn, rid: u16) -> R<String> {
        // A segment register read (issue #138: the C runtime's 6889d6 and
        // 689501 fill a CONTEXT with them for a fatal error's report): the
        // selectors a Win32 thread has.
        match crate::cs::reg_name(rid) {
            "cs" => return Ok("0x1bu".to_string()),
            "ds" | "es" | "ss" => return Ok("0x23u".to_string()),
            "fs" => return Ok("0x3bu".to_string()),
            "gs" => return Ok("0u".to_string()),
            _ => {}
        }
        let Some((r, bits, sh)) = reg_full(rid) else {
            return refused(format!("{} {} at {:08x}", i.mnemonic, i.op_str, i.address));
        };
        let n = REGS[r as usize];
        Ok(if bits == 32 {
            n.to_string()
        } else if bits == 16 {
            format!("({} & 0xffffu)", n)
        } else if sh == 0 {
            format!("({} & 0xffu)", n)
        } else {
            format!("(({} >> 8) & 0xffu)", n)
        })
    }

    fn reg_set(&self, i: &Insn, rid: u16, v: &str) -> R<String> {
        let Some((r, bits, sh)) = reg_full(rid) else {
            return refused(format!("{} {} at {:08x}", i.mnemonic, i.op_str, i.address));
        };
        let n = REGS[r as usize];
        Ok(if bits == 32 {
            format!("{} = (uint32_t)({});", n, v)
        } else if bits == 16 {
            format!("{} = ({} & 0xffff0000u) | ((uint32_t)({}) & 0xffffu);", n, n, v)
        } else if sh == 0 {
            format!("{} = ({} & 0xffffff00u) | ((uint32_t)({}) & 0xffu);", n, n, v)
        } else {
            format!("{} = ({} & 0xffff00ffu) | (((uint32_t)({}) & 0xffu) << 8);", n, n, v)
        })
    }

    pub fn addr(&self, i: &Insn, op: &Op) -> R<String> {
        let m = op.mem();
        let mut parts: Vec<String> = Vec::new();
        if m.base != 0 {
            parts.push(REGS[reg_full(m.base).expect("base register").0 as usize].to_string());
        }
        if m.index != 0 {
            parts.push(format!("{} * {}u", REGS[reg_full(m.index).expect("index register").0 as usize], m.scale));
        }
        if m.disp != 0 || parts.is_empty() {
            parts.push(format!("0x{:x}u", m.disp as u32));
        }
        let mut a = parts.join(" + ");
        let seg = if m.segment != 0 { reg_name(m.segment) } else { "" };
        if seg == "fs" {
            a = format!("aot_fs_base() + {}", a);
        } else if !["", "ds", "es", "ss"].contains(&seg) {
            return refused(format!("segment {} at {:08x}", seg, i.address));
        }
        Ok(format!("(uint32_t)({})", a))
    }

    fn get(&self, i: &Insn, op: &Op, size: Option<u32>) -> R<String> {
        let size = match size {
            Some(s) if s != 0 => s,
            _ => op.size as u32,
        };
        match op.kind {
            OpKind::Reg(r) => self.reg_get(i, r),
            OpKind::Imm(v) => Ok(format!("0x{:x}u", imm_mask(v, size))),
            _ => Ok(format!("LD{}({})", sizes(size), self.addr(i, op)?)),
        }
    }

    fn put(&self, i: &Insn, op: &Op, v: &str) -> R<String> {
        if let OpKind::Reg(r) = op.kind {
            return self.reg_set(i, r, v);
        }
        Ok(format!("ST{}({}, {});", sizes(op.size as u32), self.addr(i, op)?, v))
    }

    fn flag_store(&self, va: u32, a: &str, b: &str, r: &str, c: &str) -> String {
        let fl = self.flags();
        if !fl.live_defs.contains(&va) {
            return String::new();
        }
        let mut s = format!("F_A = {}; F_B = {}; F_R = {}; ", a, b, r);
        let (k, w) = kind_of(self.f(), va).expect("a live definer's kind");
        if c != "0" || [9, 10, 11].contains(&k) {
            s += &format!("F_C = {}; ", c);
        }
        if fl.dynamic.contains(&va) {
            s += &format!("F_K = {}; ", k as u32 * 8 + w as u32);
        }
        s
    }

    fn cond(&self, va: u32, cc: &str) -> String {
        let (_, kind) = &self.flags().readers[&va];
        match kind {
            None => format!("aot_cc_dyn({}, F_K, F_A, F_B, F_R, F_C)", cc_num(cc)),
            Some((k, w)) => format!("aot_cc({}, {}, {}, F_A, F_B, F_R, F_C)", cc_num(cc), k, w),
        }
    }

    fn carry(&self, va: u32) -> String {
        let (_, kind) = &self.flags().readers[&va];
        match kind {
            None => "aot_cc_dyn(2, F_K, F_A, F_B, F_R, F_C)".to_string(),
            Some((k, w)) => format!("aot_cc(2, {}, {}, F_A, F_B, F_R, F_C)", k, w),
        }
    }

    /// aot_xf set from the flags as they are at `va`, where it hands them to
    /// another function (a call to one that reads its entry's, a ret of one
    /// whose callers read them): flags.rs.
    fn xf_store(&self, va: u32) -> String {
        match self.flags().readers.get(&va) {
            None => String::new(),
            Some((_, None)) => "aot_xf = aot_flw(F_K >> 3, F_K & 7, F_A, F_B, F_R, F_C); ".to_string(),
            Some((_, Some((k, w)))) => format!("aot_xf = aot_flw({}, {}, F_A, F_B, F_R, F_C); ", k, w),
        }
    }

    /// The flags a definer that is a call (or the entry, AT_ENTRY) leaves:
    /// aot_xf, as AOT_EFL's word.
    fn xf_take(&self, va: u32) -> String {
        let fl = self.flags();
        if !fl.live_defs.contains(&va) {
            return String::new();
        }
        format!(" F_A = aot_xf;{}", if fl.dynamic.contains(&va) { " F_K = 100;" } else { "" })
    }

    /// One function's C.
    pub fn function(&mut self, va: u32) -> R<String> {
        let p = self.p;
        let f = p.func(va);
        self.f = Some(f.clone());
        self.flags = None;
        self.flags = Some(Flags::new(&f, p)?);
        let sig = self.sigs[&va].clone();
        self.sig = Some(sig.clone());
        self.df = direction(&f);
        let mut out: Vec<String> = Vec::new();
        out.push(match &self.resume {
            Some(_) if self.reent => format!("{} void {}(AotRegs *R, uint32_t at)\n{{", self.fn_kw(va), reent_name(va)),
            Some(v) => format!("AOT_FN void {}(AotRegs *R)\n{{", resume_name(v[0].0)),
            None => self.proto(va) + "\n{",
        });
        let mut decl: Vec<String> = Vec::new();
        for r in 0..8u8 {
            if self.regs_ctx() {
                decl.push(format!("uint32_t {} = R->r[{}];", REGS[r as usize], r));
            } else if r == ESP || sig.ins.contains(&r) {
                continue;
            } else {
                decl.push(format!("uint32_t {} = 0;", REGS[r as usize]));
            }
        }
        decl.push(
            "uint32_t F_A = 0, F_B = 0, F_R = 0, F_C = 0; int F_K = 0; (void)F_A; (void)F_B; (void)F_R; (void)F_C; (void)F_K;"
                .to_string(),
        );
        decl.push(match &self.resume {
            _ if self.loose() => "uint8_t *const M = aot_mem;".to_string(),
            Some(_) if self.reent => "uint8_t *const M = aot_mem; uint32_t esp0 = 0, ra0 = 0;".to_string(),
            // The entry's esp, from the stack offset at x.
            Some(v) => format!("uint8_t *const M = aot_mem; const uint32_t esp0 = esp + 0x{:x}u, ra0 = LD32(esp0);", (v[0].1 as i32).wrapping_neg() as u32),
            None => "uint8_t *const M = aot_mem; const uint32_t esp0 = esp, ra0 = LD32(esp);".to_string(),
        });
        let fmath = f.calls.values().any(|t| t.direct().is_some_and(|d| self.p.intrinsics.get(&d).is_some_and(|n| crate::program::FMATH.contains(n))));
        if fmath || f.insns.values().any(|i| x87_op(i).is_some()) {
            decl.push("AotFpu *const F = aot_fpu;".to_string());
        }
        out.push(format!("    {}", decl.join(" ")));
        let at_entry = self.xf_take(crate::flags::AT_ENTRY);
        if !at_entry.is_empty() {
            out.push(format!("   {}", at_entry));
        }
        out.push(format!("    AOT_TRACE(0x{:08x}u, eax, ecx, edx, ebx, esp, ebp, esi, edi);", va));
        out.push(format!("    {}", (0..8).map(|r| format!("(void){};", REGS[r])).collect::<Vec<_>>().join(" ")));
        let starts: Vec<u32> = self.resume.as_ref().map_or(vec![f.entry], |v| v.iter().map(|e| e.0).collect());
        if let (true, Some(v)) = (self.reent, &self.resume) {
            // Each site's entry esp from its stack offset.
            let cases: Vec<String> = v
                .iter()
                .map(|&(x, sp)| format!("case 0x{:08x}u: esp0 = esp + 0x{:x}u; ra0 = LD32(esp0); goto L_{:08x};", x, (sp as i32).wrapping_neg() as u32, x))
                .collect();
            out.push(format!("    switch (at) {{ {} }}\n    aot_unreachable(at); return;", cases.join(" ")));
        } else {
            out.push(format!("    goto L_{:08x};", starts[0]));
        }
        for &a in &f.order {
            let i = f.insns[&a].clone();
            if f.leaders.contains(&a) || starts.contains(&a) {
                out.push(format!("L_{:08x}:", a));
            }
            out.push(format!("    /* {:08x} {} {} */", a, i.mnemonic, i.op_str));
            if self.trace_all == Some(f.entry) {
                out.push(format!("    AOT_TRACE(0x{:08x}u, eax, ecx, edx, ebx, esp, ebp, esi, edi);", a));
            }
            let code = self.insn(&i)?;
            out.push(format!("    {}", code));
            let nxt = a.wrapping_add(i.size);
            let succ = &f.succ[&a];
            if succ.len() == 1 && succ[0] == nxt && i.mnemonic != "jmp" && f.leaders.contains(&nxt) {
                out.push(format!("    goto L_{:08x};", nxt));
            }
        }
        out.push("}".to_string());
        Ok(out.join("\n"))
    }

    /// sig_<va> for a function with a return sites' entry (issue #138): its
    /// code is reent_<va>'s, entered at the entry, so it is there once.
    pub fn sig_by_reent(&self, va: u32) -> String {
        let sig = &self.sigs[&va];
        let fill: Vec<String> =
            (0..8u8).map(|r| if r == ESP || sig.ins.contains(&r) { REGS[r as usize].to_string() } else { "0u".to_string() }).collect();
        let outs = &sig.outs;
        let back = match outs.len() {
            0 => "return;".to_string(),
            1 => format!("return t.r[{}];", outs[0]),
            2 => format!("return (uint64_t)t.r[{}] | (uint64_t)t.r[{}] << 32;", outs[0], outs[1]),
            _ => {
                let s: Vec<String> = outs.iter().enumerate().map(|(k, &r)| format!("o[{}] = t.r[{}];", k, r)).collect();
                s.join(" ") + " return;"
            }
        };
        format!(
            "AOT_FN void {}(AotRegs *R, uint32_t at);\n{}\n{{\n    AotRegs t = {{{{{}}}}};\n    {}(&t, 0x{:08x}u);\n    {}\n}}",
            reent_name(va),
            self.proto(va),
            fill.join(", "),
            reent_name(va),
            va,
            back
        )
    }

    pub fn proto(&self, va: u32) -> String {
        if self.p.is_loose(va) {
            return format!("{} uint32_t {}(AotRegs *R)", self.fn_kw(va), self.name(va));
        }
        if self.mode == "ctx" {
            return format!("{} void {}(AotRegs *R)", self.fn_kw(va), self.name(va));
        }
        let sig = &self.sigs[&va];
        let mut args = vec!["uint32_t esp".to_string()];
        args.extend(sig.ins.iter().map(|&r| format!("uint32_t {}", REGS[r as usize])));
        let ret = match sig.outs.len() {
            0 => "void",
            1 => "uint32_t",
            2 => "uint64_t",
            _ => {
                args.push("uint32_t *o".to_string());
                "void"
            }
        };
        format!("{} {} {}({})", self.fn_kw(va), ret, self.name(va), args.join(", "))
    }

    fn ret_outs(&self) -> String {
        let outs = &self.sig().outs;
        match outs.len() {
            0 => "return;".to_string(),
            1 => format!("return {};", REGS[outs[0] as usize]),
            2 => format!("return (uint64_t){} | (uint64_t){} << 32;", REGS[outs[0] as usize], REGS[outs[1] as usize]),
            _ => {
                let s: Vec<String> = outs.iter().enumerate().map(|(k, &r)| format!("o[{}] = {};", k, REGS[r as usize])).collect();
                s.join(" ") + " return;"
            }
        }
    }

    fn ctx_sets(&self) -> String {
        (0..8u8).filter(|&r| r != ESP).map(|r| format!("R->r[{}] = {};", r, REGS[r as usize])).collect::<Vec<_>>().join(" ")
    }

    fn ret_stmt(&self, pops: i64) -> String {
        if self.loose() {
            // The return address as it is on the stack, and esp past it and
            // what the ret pops: the dispatcher goes on there.
            return format!("{{ uint32_t to = LD32(esp); {} R->r[4] = esp + {}u; return to; }}", self.ctx_sets(), 4 + pops);
        }
        let chk = format!("if (esp != esp0 || LD32(esp) != ra0) aot_bad_ret(0x{:08x}u, esp0, esp); ", self.f().entry);
        if self.regs_ctx() {
            return format!("{}{} R->r[4] = esp + {}u; return;", chk, self.ctx_sets(), 4 + pops);
        }
        chk + &self.ret_outs()
    }

    fn call_compiled(&self, t: u32, after: bool) -> String {
        if self.mode == "ctx" {
            let fill = REGS.join(", ");
            let back: Vec<String> = (0..8).map(|r| format!("{} = t.r[{}];", REGS[r], r)).collect();
            return format!(
                "{{ AotRegs t = {{{{{}}}}}; if (AOT_STALE({}u)) aot_call_stale(0x{:08x}u, &t); else {}(&t); {} }}",
                fill,
                self.row(t),
                t,
                self.name(t),
                back.join(" ")
            );
        }
        let sig = &self.sigs[&t];
        let mut args = vec!["esp".to_string()];
        args.extend(sig.ins.iter().map(|&r| REGS[r as usize].to_string()));
        let args = args.join(", ");
        let outs = &sig.outs;
        let tail = match self.p.summ[&t].pops {
            Some(pops) if after => format!(" esp += {}u;", 4 + pops),
            _ => String::new(),
        };
        let w = self.wrap(t);
        match outs.len() {
            0 => format!("{}({});{}", w, args, tail),
            1 => format!("{} = {}({});{}", REGS[outs[0] as usize], w, args, tail),
            2 => format!(
                "{{ uint64_t t = {}({}); {} = (uint32_t)t; {} = (uint32_t)(t >> 32); }}{}",
                w,
                args,
                REGS[outs[0] as usize],
                REGS[outs[1] as usize],
                tail
            ),
            n => {
                let back: Vec<String> = outs.iter().enumerate().map(|(k, &r)| format!("{} = o[{}];", REGS[r as usize], k)).collect();
                format!("{{ uint32_t o[{}]; {}({}, o); {} }}{}", n, w, args, back.join(" "), tail)
            }
        }
    }

    pub fn row(&self, t: u32) -> usize {
        let mut idx = self.index.borrow_mut();
        if idx.is_none() {
            *idx = Some(self.p.compiled.iter().enumerate().map(|(k, &v)| (v, k)).collect());
        }
        idx.as_ref().unwrap()[&t]
    }

    fn wrap(&self, t: u32) -> String {
        format!("AOT_SYM(call_{:08x})", t)
    }

    /// call_ and stale_ for va (sig mode).
    pub fn wrappers(&self, va: u32) -> String {
        if self.p.is_loose(va) {
            return String::new();   // never called in C: a callout to its address
        }
        let sig = &self.sigs[&va];
        let mut params = vec!["uint32_t esp".to_string()];
        params.extend(sig.ins.iter().map(|&r| format!("uint32_t {}", REGS[r as usize])));
        let mut args = vec!["esp".to_string()];
        args.extend(sig.ins.iter().map(|&r| REGS[r as usize].to_string()));
        let outs = &sig.outs;
        let ret = match outs.len() {
            0 => "void",
            1 => "uint32_t",
            2 => "uint64_t",
            _ => {
                params.push("uint32_t *o".to_string());
                args.push("o".to_string());
                "void"
            }
        };
        let fill: Vec<String> =
            (0..8u8).map(|r| if r == ESP || sig.ins.contains(&r) { REGS[r as usize].to_string() } else { "0u".to_string() }).collect();
        let back = match outs.len() {
            0 => "return;".to_string(),
            1 => format!("return t.r[{}];", outs[0]),
            2 => format!("return (uint64_t)t.r[{}] | (uint64_t)t.r[{}] << 32;", outs[0], outs[1]),
            _ => {
                let s: Vec<String> = outs.iter().enumerate().map(|(k, &r)| format!("o[{}] = t.r[{}];", k, r)).collect();
                s.join(" ") + " return;"
            }
        };
        let stale = format!(
            "static AOT_COLD {} AOT_SYM(stale_{:08x})({})\n{{\n    AotRegs t = {{{{{}}}}};\n    aot_call_stale(0x{:08x}u, &t);\n    {}\n}}",
            ret,
            va,
            params.join(", "),
            fill.join(", "),
            va,
            back
        );
        let ret_kw = if ret == "void" { "" } else { "return " };
        let tail = if ret == "void" { "\n        return;" } else { "" };
        let call = format!(
            "static inline {} AOT_SYM(call_{:08x})({})\n{{\n    if (AOT_STALE({}u)) {{\n        {}AOT_SYM(stale_{:08x})({});{}\n    }}\n    {}{}({});\n}}",
            ret,
            va,
            params.join(", "),
            self.row(va),
            ret_kw,
            va,
            args.join(", "),
            tail,
            ret_kw,
            self.name(va),
            args.join(", ")
        );
        stale + "\n" + &call
    }

    fn call_guest(&self, t: u32) -> String {
        let fill = REGS.join(", ");
        let back: Vec<String> = if self.mode == "sig" {
            let s = self.p.callee_summary(Tgt::Direct(t));
            (0..8u8).filter(|&r| r == ESP || !has(s.preserved, r)).map(|r| format!("{} = t.r[{}];", REGS[r as usize], r)).collect()
        } else {
            (0..8).map(|r| format!("{} = t.r[{}];", REGS[r], r)).collect()
        };
        format!("{{ AotRegs t = {{{{{}}}}}; aot_guest_call(0x{:08x}u, &t); {} }}", fill, t, back.join(" "))
    }

    fn insn(&mut self, i: &Insn) -> R<String> {
        let va = i.address;
        let m = i.mnemonic.as_str();
        let ops = &i.ops;
        let f = self.f.clone().unwrap();
        if f.calls.contains_key(&va) || f.tails.contains_key(&va) {
            // Flags across the call (flags.rs): stored for a callee that
            // reads them, taken from one that hands them back.
            let back = if f.calls.contains_key(&va) { self.xf_take(va) } else { String::new() };
            return Ok(format!("{}{}{}", self.xf_store(va), self.call(i)?, back));
        }
        if m == "ret" {
            return Ok(format!("{}{}", self.xf_store(va), self.ret_stmt(if ops.is_empty() { 0 } else { ops[0].imm() })));
        }
        if m == "int3" || m == "hlt" || m == "ud2" {
            return Ok(format!("aot_unreachable(0x{:08x}u);", va));
        }
        if m == "int" || m == "into" {
            // A software interrupt (issue #138: int 6 in the entries of
            // 6a9a17's table that stand for an opcode it can't have): Windows
            // raises an exception there; here it is the stop.
            return Ok(format!("aot_unreachable(0x{:08x}u);", va));
        }
        if m == "sahf" {
            // ah as the flags (CF, PF, AF, ZF, SF; OF isn't sahf's).
            let s = self.flag_store(va, "((eax >> 8) & 0xd5u)", "0u", "0u", "0");
            return Ok(if s.is_empty() { ";".to_string() } else { s });
        }
        if ["bt", "bts", "btr", "btc"].contains(&m) && ops.len() == 2 && ops[0].size == 4 {
            // The bit into CF, then set, cleared or flipped. In a register,
            // or by an immediate, its number is taken within the operand; in
            // memory by a register it counts on from the operand's address,
            // by dwords (issue #138: 686990's bts [esp], eax builds a set of
            // 256 characters on the stack, and its bt [esp], eax asks it).
            let n = self.get(i, &ops[1], Some(4))?;
            let cf = self.flag_store(va, "((v >> k) & 1u)", "0u", "0u", "0");
            let new = match m {
                "bt" => None,
                "bts" => Some("v | (1u << k)"),
                "btr" => Some("v & ~(1u << k)"),
                _ => Some("v ^ (1u << k)"),
            };
            if ops[0].is_mem() && !ops[1].is_imm() {
                let st = new.map(|e| format!("ST32(a, {});", e)).unwrap_or_default();
                return Ok(format!(
                    "{{ const uint32_t n = {}; const uint32_t a = {} + (uint32_t)(((int32_t)n >> 5) * 4), k = n & 31u, v = LD32(a); (void)v; {}{} }}",
                    n,
                    self.addr(i, &ops[0])?,
                    cf,
                    st
                ));
            }
            let st = match new {
                Some(e) => self.put(i, &ops[0], e)?,
                None => String::new(),
            };
            return Ok(format!("{{ const uint32_t k = ({}) & 31u, v = {}; (void)v; (void)k; {}{} }}", n, self.get(i, &ops[0], None)?, cf, st));
        }
        if self.p.loose_on {
            if let Some(s) = self.sse(i)? {
                return Ok(s);
            }
        }
        if m == "nop" {
            return Ok(";".to_string());
        }
        if m == "jmp" {
            if let Some(sw) = f.switches.get(&va) {
                let mut s = format!("switch ({}) {{ ", REGS[sw.idx as usize]);
                for (k, t) in sw.targets.iter().enumerate() {
                    s += &format!("case {}: goto L_{:08x}; ", k, t);
                }
                s += &format!("default: aot_unreachable(0x{:08x}u); }}", va);
                return Ok(s);
            }
            // A jump to the instruction after it (683080's jmp +0) that
            // nothing else jumps to has no label: the code just goes on.
            if ops[0].imm() as u32 == va.wrapping_add(i.size) && !f.leaders.contains(&va.wrapping_add(i.size)) {
                return Ok(";".to_string());
            }
            return Ok(format!("goto L_{:08x};", ops[0].imm()));
        }
        if m.starts_with('j') {
            let cc = cc_of(m, "j");
            if cc == "ecxz" || cc == "cxz" {
                return Ok(format!("if (ecx == 0) goto L_{:08x}; goto L_{:08x};", ops[0].imm(), va.wrapping_add(i.size)));
            }
            return Ok(format!("if ({}) goto L_{:08x}; goto L_{:08x};", self.cond(va, cc), ops[0].imm(), va.wrapping_add(i.size)));
        }
        if m.starts_with("set") {
            let c = format!("({}) ? 1u : 0u", self.cond(va, cc_of(m, "set")));
            return self.put(i, &ops[0], &c);
        }
        if m.starts_with("cmov") {
            let c = self.cond(va, cc_of(m, "cmov"));
            let v = self.get(i, &ops[1], None)?;
            return Ok(format!("if ({}) {{ {} }}", c, self.put(i, &ops[0], &v)?));
        }
        if m == "mov" {
            let v = self.get(i, &ops[1], None)?;
            return self.put(i, &ops[0], &v);
        }
        if m == "movzx" || m == "movsx" {
            let mut v = self.get(i, &ops[1], None)?;
            if m == "movsx" {
                let t = if ops[1].size == 1 { "int8_t" } else { "int16_t" };
                v = format!("(uint32_t)(int32_t)({})({})", t, v);
            }
            return self.put(i, &ops[0], &v);
        }
        if m == "lea" {
            let a = self.addr(i, &ops[1])?.replace("aot_fs_base() + ", "");
            return self.put(i, &ops[0], &a);
        }
        if m == "push" {
            let v = if !ops[0].is_imm() { self.get(i, &ops[0], Some(4))? } else { format!("0x{:x}u", imm_mask(ops[0].imm(), 4)) };
            if self.mode == "sig" {
                let ana = self.ana.as_ref().unwrap();
                if let Some(&r) = ana.save_push.get(&va) {
                    if has(ana.skip_saves, r) {
                        return Ok(format!("esp -= 4u; /* saves {}, which it preserves: not stored */", REGS[r as usize]));
                    }
                }
            }
            return Ok(format!("{{ uint32_t v = {}; esp -= 4u; ST32(esp, v); }}", v));
        }
        if m == "pop" {
            if let OpKind::Reg(rid) = ops[0].kind {
                let r = reg_full(rid).expect("pop of a segment register").0;
                if r == ESP {
                    return Ok("esp = LD32(esp);".to_string());
                }
            }
            return Ok(format!("{{ uint32_t v = LD32(esp); esp += 4u; {} }}", self.put(i, &ops[0], "v")?));
        }
        if m == "leave" {
            return Ok("esp = ebp; ebp = LD32(esp); esp += 4u;".to_string());
        }
        if m == "xchg" {
            let a = self.get(i, &ops[0], None)?;
            let b = self.get(i, &ops[1], None)?;
            return Ok(format!("{{ uint32_t a = {}, b = {}; {} {} }}", a, b, self.put(i, &ops[0], "b")?, self.put(i, &ops[1], "a")?));
        }
        if ["add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb"].contains(&m) {
            return self.alu(i);
        }
        if ["inc", "dec", "neg", "not"].contains(&m) {
            let w = ops[0].size as u32;
            let a = self.get(i, &ops[0], None)?;
            if m == "not" {
                return Ok(format!("{{ uint32_t a = {}; {} }}", a, self.put(i, &ops[0], &format!("~a & {}", mask(w)))?));
            }
            let r = if m == "neg" {
                format!("(0u - a) & {}", mask(w))
            } else {
                format!("(a {} 1u) & {}", if m == "inc" { "+" } else { "-" }, mask(w))
            };
            return Ok(format!(
                "{{ uint32_t a = {}, r = {}; {}{} }}",
                a,
                r,
                self.flag_store(va, "a", "1u", "r", "0"),
                self.put(i, &ops[0], "r")?
            ));
        }
        if ["shl", "sal", "shr", "sar", "rol", "ror"].contains(&m) {
            return self.shift(i);
        }
        if m == "imul" {
            return self.imul(i);
        }
        if m == "rcr" && ops.len() == 2 && ops[0].size == 4 && ops[1].is_imm() && ops[1].imm() == 1 {
            // Through the carry, by one (issue #138: the C runtime's 64-bit
            // shifts, 686c00 and 684240, shr the high half and rcr the low).
            // The flags it leaves aren't modelled: a reader refuses the function.
            let c = self.carry(va);
            let a = self.get(i, &ops[0], None)?;
            return Ok(format!(
                "{{ uint32_t c = {}; uint32_t a = {}; uint32_t r = (a >> 1) | (c << 31); {} }}",
                c,
                a,
                self.put(i, &ops[0], "r")?
            ));
        }
        // The flags register as a whole (issue #138: 69a860 tells cpuid is
        // there by flipping the ID bit): the word popfd last stored, not the
        // arithmetic flags, which nothing here reads back from it.
        if m == "pushfd" {
            return Ok("{ esp -= 4u; ST32(esp, aot_eflags); }".to_string());
        }
        if m == "popfd" {
            return Ok("{ aot_eflags = (LD32(esp) & 0x00244dd5u) | 2u; esp += 4u; }".to_string());
        }
        if (m == "shld" || m == "shrd") && ops.len() == 3 && ops[0].size == 4 {
            // Bits shifted in from a second register (issue #138: the C
            // runtime's 6939ac and 68dff5). A count of 0 changes nothing; the
            // flags aren't modelled.
            let a = self.get(i, &ops[0], None)?;
            let b = self.get(i, &ops[1], None)?;
            let n = self.get(i, &ops[2], None)?;
            let r = if m == "shld" { "(a << n) | (b >> (32u - n))" } else { "(a >> n) | (b << (32u - n))" };
            return Ok(format!(
                "{{ uint32_t a = {}, b = {}, n = ({}) & 31u; if (n) {{ uint32_t r = {}; {} }} }}",
                a,
                b,
                n,
                r,
                self.put(i, &ops[0], "r")?
            ));
        }
        if m == "rdtsc" {
            return Ok("{ uint64_t t = aot_rdtsc(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }".to_string());
        }
        if m == "cpuid" {
            return Ok("aot_cpuid(&eax, &ebx, &ecx, &edx);".to_string());
        }
        if m == "stmxcsr" {
            // No SSE state is kept: the value a thread starts with (all
            // exceptions masked, round to nearest).
            return Ok(format!("{{ uint32_t r = 0x1f80u; {} }}", self.put(i, &ops[0], "r")?));
        }
        if m.ends_with("cmpxchg8b") {
            // edx:eax against the 8 bytes: equal, ecx:ebx stored and ZF set;
            // else they are loaded and ZF cleared. The lock prefix is nothing
            // here: one guest thread runs at a time (issue #138: 4107b0 and
            // 4107d0, the 64-bit counters' compare and swap).
            let a = self.addr(i, &ops[0])?;
            return Ok(format!(
                "{{ uint32_t a = {}, lo = LD32(a), hi = LD32(a + 4u), r = !(lo == eax && hi == edx); \
if (!r) {{ ST32(a, ebx); ST32(a + 4u, ecx); }} else {{ eax = lo; edx = hi; }} {}}}",
                a,
                self.flag_store(va, "0u", "0u", "r", "0")
            ));
        }
        if m == "mul" && ops[0].size == 4 {
            let v = self.get(i, &ops[0], None)?;
            return Ok(format!(
                "{{ uint64_t p = (uint64_t)eax * (uint64_t)({}); eax = (uint32_t)p; edx = (uint32_t)(p >> 32); {}}}",
                v,
                self.flag_store(va, "0u", "0u", "eax", "edx != 0u")
            ));
        }
        if (m == "div" || m == "idiv") && ops[0].size == 4 {
            let v = self.get(i, &ops[0], None)?;
            if m == "div" {
                return Ok(format!(
                    "{{ uint64_t n = (uint64_t)edx << 32 | eax; uint32_t d = {}; if (!d) aot_fault(0x{:08x}u); eax = (uint32_t)(n / d); edx = (uint32_t)(n % d); }}",
                    v, va
                ));
            }
            return Ok(format!(
                "{{ int64_t n = (int64_t)((uint64_t)edx << 32 | eax); int32_t d = (int32_t)({}); if (!d) aot_fault(0x{:08x}u); eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n % d); }}",
                v, va
            ));
        }
        if m == "bsf" || m == "bsr" {
            let a = self.get(i, &ops[1], None)?;
            let r = if m == "bsf" { "__builtin_ctz(a)" } else { "31 - __builtin_clz(a)" };
            return Ok(format!(
                "{{ uint32_t a = {}; {}if (a) {{ {} }} }}",
                a,
                self.flag_store(va, "a", "0u", "a", "0"),
                self.put(i, &ops[0], r)?
            ));
        }
        if m == "cdq" {
            return Ok("edx = (eax & 0x80000000u) ? 0xffffffffu : 0u;".to_string());
        }
        if m == "cwde" {
            return Ok("eax = (uint32_t)(int32_t)(int16_t)eax;".to_string());
        }
        if m == "cld" || m == "std" {
            return Ok(";".to_string());
        }
        // _ftol2's SSE2 path (issue #138): out of range, x86 gives 0x80000000.
        if m == "cvttsd2si" && ops[0].is_reg() && ops[0].size == 4 {
            let src = if ops[1].is_mem() {
                format!("aot_ldf64(M, {})", self.addr(i, &ops[1])?)
            } else {
                return refused(format!("cvttsd2si from a register at {:08x}", va));
            };
            return Ok(format!(
                "{{ double d = {}; {} }}",
                src,
                self.put(i, &ops[0], "(d > -2147483649.0 && d < 2147483648.0) ? (uint32_t)(int32_t)d : 0x80000000u")?
            ));
        }
        if m == "wait" || x87_op(i).is_some() {
            return self.x87(i);
        }
        if (m.starts_with("rep") || ["stosd", "stosb", "stosw", "movsd", "movsb", "movsw"].contains(&m)) && !ops.iter().any(|o| xmm_of(o).is_some()) {
            return self.string(i);
        }
        refused(format!("{} at {:08x}", m, va))
    }

    /// An SSE2 instruction (issue #138, a build without Box86): the C
    /// runtime's pow, log, log10 and exp for a CPU with SSE2, which cpuid
    /// reports, and its memset and memcpy by 16 bytes. On aot_xmm, a double
    /// as its bits; rounding to nearest, no exception raised (aot_rt.h).
    /// None for an instruction that isn't one of these.
    fn sse(&self, i: &Insn) -> R<Option<String>> {
        let va = i.address;
        let m = i.mnemonic.as_str();
        let ops = &i.ops;
        if m == "ldmxcsr" {
            return Ok(Some("; /* ldmxcsr: no MXCSR is kept */".to_string()));
        }
        if !ops.iter().any(|o| xmm_of(o).is_some()) {
            return Ok(None);
        }
        let x = |o: &Op| format!("aot_xmm[{}]", xmm_of(o).unwrap());
        // An operand's 128 bits, or its low 64.
        let s128 = |o: &Op| -> R<String> {
            Ok(match xmm_of(o) {
                Some(n) => format!("aot_xmm[{}]", n),
                None => format!("aot_xld(M, {})", self.addr(i, o)?),
            })
        };
        let s64 = |o: &Op| -> R<String> {
            Ok(match xmm_of(o) {
                Some(n) => format!("aot_xmm[{}].q[0]", n),
                None => format!("aot_ld64(M, {})", self.addr(i, o)?),
            })
        };
        let no = || refused(format!("{} {} at {:08x}", m, i.op_str, va));
        let d_is_x = xmm_of(&ops[0]).is_some();
        let two = ops.len() == 2;
        let out = match m {
            "movapd" | "movupd" | "movaps" | "movups" | "movdqa" | "movdqu" if two => {
                if d_is_x {
                    format!("{{ const AotXmm s = {}; {} = s; }}", s128(&ops[1])?, x(&ops[0]))
                } else {
                    format!("aot_xst(M, {}, {});", self.addr(i, &ops[0])?, x(&ops[1]))
                }
            }
            "movlpd" | "movhpd" if two => {
                let k = if m == "movlpd" { 0 } else { 1 };
                if d_is_x {
                    format!("{}.q[{}] = aot_ld64(M, {});", x(&ops[0]), k, self.addr(i, &ops[1])?)
                } else {
                    format!("aot_st64(M, {}, {}.q[{}]);", self.addr(i, &ops[0])?, x(&ops[1]), k)
                }
            }
            "movsd" | "movq" if two => {
                if !d_is_x {
                    format!("aot_st64(M, {}, {}.q[0]);", self.addr(i, &ops[0])?, x(&ops[1]))
                } else if m == "movsd" && xmm_of(&ops[1]).is_some() {
                    format!("{}.q[0] = {}.q[0];", x(&ops[0]), x(&ops[1]))   // the high half stays
                } else {
                    format!("{{ const uint64_t s = {}; {}.q[0] = s; {}.q[1] = 0; }}", s64(&ops[1])?, x(&ops[0]), x(&ops[0]))
                }
            }
            "movd" if two => {
                if d_is_x {
                    format!("{{ const uint32_t s = {}; {}.q[0] = s; {}.q[1] = 0; }}", self.get(i, &ops[1], None)?, x(&ops[0]), x(&ops[0]))
                } else {
                    self.put(i, &ops[0], &format!("(uint32_t){}.q[0]", x(&ops[1])))?
                }
            }
            "andpd" | "andps" | "pand" | "orpd" | "orps" | "por" | "xorpd" | "xorps" | "pxor" | "andnpd" | "andnps" | "pandn" if two && d_is_x => {
                let e = |k: u8| match m {
                    "andpd" | "andps" | "pand" => format!("d->q[{0}] &= s.q[{0}];", k),
                    "orpd" | "orps" | "por" => format!("d->q[{0}] |= s.q[{0}];", k),
                    "xorpd" | "xorps" | "pxor" => format!("d->q[{0}] ^= s.q[{0}];", k),
                    _ => format!("d->q[{0}] = ~d->q[{0}] & s.q[{0}];", k),
                };
                format!("{{ const AotXmm s = {}; AotXmm *const d = &{}; {} {} }}", s128(&ops[1])?, x(&ops[0]), e(0), e(1))
            }
            "addpd" | "subpd" | "mulpd" | "divpd" if two && d_is_x => {
                let o = sse_arith(m);
                let e = |k: u8| format!("d->q[{0}] = aot_xq(aot_xd(d->q[{0}]) {1} aot_xd(s.q[{0}]));", k, o);
                format!("{{ const AotXmm s = {}; AotXmm *const d = &{}; {} {} }}", s128(&ops[1])?, x(&ops[0]), e(0), e(1))
            }
            "addsd" | "subsd" | "mulsd" | "divsd" if two && d_is_x => format!(
                "{{ const uint64_t s = {}; AotXmm *const d = &{}; d->q[0] = aot_xq(aot_xd(d->q[0]) {} aot_xd(s)); }}",
                s64(&ops[1])?,
                x(&ops[0]),
                sse_arith(m)
            ),
            "sqrtsd" if two && d_is_x => format!("{{ const uint64_t s = {}; {}.q[0] = aot_xq(__builtin_sqrt(aot_xd(s))); }}", s64(&ops[1])?, x(&ops[0])),
            "psrlq" | "psllq" if two && d_is_x => {
                let n = if ops[1].is_imm() { format!("{}u", ops[1].imm() & 0xff) } else { s64(&ops[1])? };
                let o = if m == "psrlq" { ">>" } else { "<<" };
                format!(
                    "{{ const uint64_t n = {}; AotXmm *const d = &{}; d->q[0] = n > 63 ? 0 : d->q[0] {} n; d->q[1] = n > 63 ? 0 : d->q[1] {} n; }}",
                    n,
                    x(&ops[0]),
                    o,
                    o
                )
            }
            "psubq" | "paddq" if two && d_is_x => {
                let o = if m == "psubq" { "-" } else { "+" };
                format!("{{ const AotXmm s = {}; AotXmm *const d = &{}; d->q[0] {}= s.q[0]; d->q[1] {}= s.q[1]; }}", s128(&ops[1])?, x(&ops[0]), o, o)
            }
            "psubd" | "paddd" if two && d_is_x => {
                let o = if m == "psubd" { "-" } else { "+" };
                let e = |k: u8| {
                    format!(
                        "d->q[{0}] = (uint64_t)(uint32_t)((uint32_t)d->q[{0}] {1} (uint32_t)s.q[{0}]) | (uint64_t)(uint32_t)((uint32_t)(d->q[{0}] >> 32) {1} (uint32_t)(s.q[{0}] >> 32)) << 32;",
                        k, o
                    )
                };
                format!("{{ const AotXmm s = {}; AotXmm *const d = &{}; {} {} }}", s128(&ops[1])?, x(&ops[0]), e(0), e(1))
            }
            "unpcklpd" | "punpcklqdq" if two && d_is_x => format!("{{ const AotXmm s = {}; {}.q[1] = s.q[0]; }}", s128(&ops[1])?, x(&ops[0])),
            "unpckhpd" | "punpckhqdq" if two && d_is_x => {
                format!("{{ const AotXmm s = {}; AotXmm *const d = &{}; d->q[0] = d->q[1]; d->q[1] = s.q[1]; }}", s128(&ops[1])?, x(&ops[0]))
            }
            "pextrw" if ops.len() == 3 && ops[0].is_reg() && xmm_of(&ops[1]).is_some() && ops[2].is_imm() => {
                let k = (ops[2].imm() & 7) as u32;
                self.put(i, &ops[0], &format!("(uint32_t)(({}.q[{}] >> {}) & 0xffffu)", x(&ops[1]), k / 4, (k % 4) * 16))?
            }
            "pinsrw" if ops.len() == 3 && d_is_x && ops[2].is_imm() => {
                let k = (ops[2].imm() & 7) as u32;
                let v = if ops[1].is_mem() { format!("LD16({})", self.addr(i, &ops[1])?) } else { self.get(i, &ops[1], None)? };
                format!(
                    "{{ const uint64_t v = ({}) & 0xffffu; AotXmm *const d = &{}; d->q[{}] = (d->q[{}] & ~((uint64_t)0xffffu << {})) | (v << {}); }}",
                    v,
                    x(&ops[0]),
                    k / 4,
                    k / 4,
                    (k % 4) * 16,
                    (k % 4) * 16
                )
            }
            "cvtsi2sd" if two && d_is_x => format!("{}.q[0] = aot_xq((double)(int32_t)({}));", x(&ops[0]), self.get(i, &ops[1], Some(4))?),
            "cvtsd2si" | "cvttsd2si" if two && ops[0].is_reg() && ops[0].size == 4 => {
                if m == "cvttsd2si" && ops[1].is_mem() {
                    return Ok(None);   // _ftol2's, written as it always was (insn)
                }
                self.put(i, &ops[0], &format!("aot_xcvt(aot_xd({}), {})", s64(&ops[1])?, if m == "cvttsd2si" { 1 } else { 0 }))?
            }
            "cvtdq2pd" if two && d_is_x => format!(
                "{{ const uint64_t s = {}; AotXmm *const d = &{}; d->q[0] = aot_xq((double)(int32_t)(uint32_t)s); d->q[1] = aot_xq((double)(int32_t)(uint32_t)(s >> 32)); }}",
                s64(&ops[1])?,
                x(&ops[0])
            ),
            "pshufd" if ops.len() == 3 && d_is_x && ops[2].is_imm() => {
                let k = ops[2].imm() as u32;
                format!(
                    "{{ const AotXmm s = {}; const uint32_t w[4] = {{(uint32_t)s.q[0], (uint32_t)(s.q[0] >> 32), (uint32_t)s.q[1], (uint32_t)(s.q[1] >> 32)}}; \
AotXmm *const d = &{}; d->q[0] = w[{}] | (uint64_t)w[{}] << 32; d->q[1] = w[{}] | (uint64_t)w[{}] << 32; }}",
                    s128(&ops[1])?,
                    x(&ops[0]),
                    k & 3,
                    (k >> 2) & 3,
                    (k >> 4) & 3,
                    (k >> 6) & 3
                )
            }
            "pmaxsw" if two && d_is_x => format!("{0} = aot_xmaxsw({0}, {1});", x(&ops[0]), s128(&ops[1])?),
            "pcmpeqd" if two && d_is_x => format!("{0} = aot_xcmpeqd({0}, {1});", x(&ops[0]), s128(&ops[1])?),
            "pmovmskb" if two && ops[0].is_reg() && xmm_of(&ops[1]).is_some() => self.put(i, &ops[0], &format!("aot_xmovmskb({})", x(&ops[1])))?,
            "ucomisd" | "comisd" if two && d_is_x => {
                let s = self.flag_store(va, &format!("aot_xcomi(aot_xd({}.q[0]), aot_xd({}))", x(&ops[0]), s64(&ops[1])?), "0u", "0u", "0");
                if s.is_empty() { ";".to_string() } else { s }
            }
            _ => {
                // cmpeqsd, cmpltpd and the rest: the predicate in the name, or cmpsd's third operand.
                let Some((pred, pd)) = sse_cmp(m, ops) else { return no() };
                if !d_is_x {
                    return no();
                }
                if pd {
                    format!(
                        "{{ const AotXmm s = {}; AotXmm *const d = &{}; d->q[0] = aot_xcmp(aot_xd(d->q[0]), aot_xd(s.q[0]), {}); d->q[1] = aot_xcmp(aot_xd(d->q[1]), aot_xd(s.q[1]), {}); }}",
                        s128(&ops[1])?,
                        x(&ops[0]),
                        pred,
                        pred
                    )
                } else {
                    format!("{{ const uint64_t s = {}; AotXmm *const d = &{}; d->q[0] = aot_xcmp(aot_xd(d->q[0]), aot_xd(s), {}); }}", s64(&ops[1])?, x(&ops[0]), pred)
                }
            }
        };
        Ok(Some(out))
    }

    fn x87_next(&self, i: &Insn) -> R<Vec<u8>> {
        let n = self.p.img.insn(i.address.wrapping_add(i.size))?;
        Ok(n.bytes.clone())
    }

    fn x87_f2xm1_pair(&self, va: u32) -> bool {
        let f = self.f();
        [(va, &[0xd9u8, 0xf0][..]), (va.wrapping_add(2), &[0xd9, 0xe8][..]), (va.wrapping_add(4), &[0xde, 0xc1][..])]
            .iter()
            .all(|(a, b)| f.insns.get(a).is_some_and(|i| i.bytes.as_slice() == *b))
    }

    fn x87(&self, i: &Insn) -> R<String> {
        let va = i.address;
        if i.mnemonic == "wait" {
            return Ok(";".to_string());
        }
        let op = x87_op(i);
        if [0x66, 0x67, 0xf2, 0xf3].contains(&i.bytes[0]) {
            return refused(format!("x87 ({}) with a prefix at {:08x}", i.mnemonic, va));
        }
        let (o, mr) = op.expect("an x87 instruction");
        let reg = (mr >> 3) & 7;
        let rm = mr & 7;
        let pop3 = |r: u8, n: u8| if r == n { " aot_fpop(F);" } else { "" };
        if mr < 0xc0 {
            let mem: Vec<&Op> = i.ops.iter().filter(|x| x.is_mem()).collect();
            let a = self.addr(i, mem[0])?;
            if [0xd8, 0xdc, 0xda].contains(&o) {
                let v = match o {
                    0xd8 => format!("aot_ldf32(M, {})", a),
                    0xdc => format!("aot_ldf64(M, {})", a),
                    _ => format!("(double)(int32_t)LD32({})", a),
                };
                if let Some(opn) = arith_op(reg) {
                    return Ok(format!("{{ double v = {}; {} }}", v, s_(0, &arith(opn, &g(0), "v"))));
                }
                if reg == 2 || reg == 3 {
                    if o == 0xda {
                        return refused(format!("x87 (ficom) at {:08x}", va));
                    }
                    return Ok(format!("aot_fcom(F, {}, {});{}", g(0), v, pop3(reg, 3)));
                }
            }
            if o == 0xd9 {
                match reg {
                    0 => return Ok(format!("aot_fpush(F, aot_ldf32(M, {}));", a)),
                    2 | 3 => return Ok(format!("aot_stf32(M, {}, {});{}", a, g(0), pop3(reg, 3))),
                    5 => return Ok(format!("F->cw = (uint16_t)LD16({});", a)),
                    7 => return Ok(format!("ST16({}, F->cw);", a)),
                    _ => {}
                }
            }
            if o == 0xdd {
                match reg {
                    0 => return Ok(format!("aot_fpush(F, aot_ldf64(M, {}));", a)),
                    2 | 3 => return Ok(format!("aot_stf64(M, {}, {});{}", a, g(0), pop3(reg, 3))),
                    4 => return Ok(format!("aot_frstor(F, M, {});", a)),
                    6 => return Ok(format!("aot_fnsave(F, M, {});", a)),
                    7 => return Ok(format!("ST16({}, aot_fnstsw(F));", a)),
                    _ => {}
                }
            }
            if o == 0xdb {
                match reg {
                    0 => return Ok(format!("aot_fpush(F, (double)(int32_t)LD32({}));", a)),
                    2 | 3 => return Ok(format!("ST32({}, aot_fist32(F, {}));{}", a, g(0), pop3(reg, 3))),
                    5 => {
                        let n = self.x87_next(i)?;
                        if n.len() > 1 && n[0] == 0xdb && (n[1] >> 3) & 7 == 7 {
                            return refused(format!("x87 (fld m80 then fstp m80) at {:08x}", va));
                        }
                        return Ok(format!("aot_fld80(F, M, {});", a));
                    }
                    7 => return Ok(format!("aot_fstp80(F, M, {});", a)),
                    _ => {}
                }
            }
            if o == 0xdf {
                match reg {
                    0 => return Ok(format!("aot_fpush(F, (double)(int16_t)LD16({}));", a)),
                    2 | 3 => return Ok(format!("ST16({}, aot_fist16(F, {}));{}", a, g(0), pop3(reg, 3))),
                    5 => {
                        let n = self.x87_next(i)?;
                        if n.len() > 1 && n[0] == 0xdf && (n[1] >> 3) & 7 == 7 {
                            return refused(format!("x87 (fild m64 then fistp m64) at {:08x}", va));
                        }
                        return Ok(format!("aot_fild64(F, aot_ld64(M, {}));", a));
                    }
                    7 => return Ok(format!("aot_st64(M, {}, aot_fist64(F, {})); aot_fpop(F);", a, g(0))),
                    _ => {}
                }
            }
            return refused(format!("x87 ({}) at {:08x}", i.mnemonic, va));
        }
        // The register stack.
        if (o, mr) == (0xde, 0xc1) && self.x87_f2xm1_pair(va.wrapping_sub(4)) {
            return Ok("; /* in the f2xm1 before it, as Box86 runs it */".to_string());
        }
        if [0xd8, 0xdc, 0xde].contains(&o) && arith_op(reg).is_some() {
            if o == 0xd8 {
                return Ok(s_(0, &arith(arith_op(reg).unwrap(), &g(0), &g(rm))));
            }
            let opn = match reg {
                0 => "+",
                1 => "*",
                4 => "-r",
                5 => "-",
                6 => "/r",
                _ => "/",
            };
            let s = s_(rm, &arith(opn, &g(rm), &g(0)));
            return Ok(s + if o == 0xde { " aot_fpop(F);" } else { "" });
        }
        if o == 0xd8 && (reg == 2 || reg == 3) {
            return Ok(format!("aot_fcom(F, {}, {});{}", g(0), g(rm), pop3(reg, 3)));
        }
        if o == 0xdd && (reg == 4 || reg == 5) {
            return Ok(format!("aot_fcom(F, {}, {});{}", g(0), g(rm), pop3(reg, 5)));
        }
        if (o, mr) == (0xde, 0xd9) || (o, mr) == (0xda, 0xe9) {
            return Ok(format!("aot_fcom(F, {}, {}); aot_fpop(F); aot_fpop(F);", g(0), g(1)));
        }
        if o == 0xd9 && reg == 0 {
            return Ok(format!("{{ double v = {}; aot_fpush(F, v); }}", g(rm)));
        }
        if o == 0xd9 && reg == 1 {
            return Ok(format!("{{ double a = {}, b = {}; {} {} }}", g(0), g(rm), s_(0, "b"), s_(rm, "a")));
        }
        if o == 0xdd && (reg == 2 || reg == 3) {
            let s = if rm != 0 { s_(rm, &g(0)) } else { ";".to_string() };
            return Ok(s + if reg == 3 { " aot_fpop(F);" } else { "" });
        }
        if o == 0xdf && mr == 0xe0 {
            return Ok("eax = (eax & 0xffff0000u) | aot_fnstsw(F);".to_string());
        }
        if o == 0xdb && mr == 0xe2 {
            return Ok("F->sw &= (uint16_t)~0x80ffu;".to_string());
        }
        if o == 0xd9 {
            let k = match mr {
                0xe8 => Some("1.0"),
                0xe9 => Some("3.3219280948873623"),
                0xea => Some("1.4426950408889634"),
                0xeb => Some("3.14159265358979323846"),
                0xec => Some("0.3010299956639812"),
                0xed => Some("0.69314718055994531"),
                0xee => Some("0.0"),
                _ => None,
            };
            if let Some(k) = k {
                return Ok(format!("aot_fpush(F, {});", k));
            }
            let one = match mr {
                0xe0 => Some(format!("-{}", g(0))),
                0xe1 => Some(format!("__builtin_fabs({})", g(0))),
                0xfa => Some(format!("__builtin_sqrt({})", g(0))),
                _ => None,
            };
            if let Some(v) = one {
                return Ok(s_(0, &v));
            }
            if mr == 0xfe || mr == 0xff {
                let fun = if mr == 0xfe { "__builtin_sin" } else { "__builtin_cos" };
                return Ok(format!("{} F->sw &= (uint16_t)~(C87_C1 | C87_C2);", s_(0, &format!("{}({})", fun, g(0)))));
            }
            if mr == 0xf1 {
                return Ok(format!("{} aot_fpop(F);", s_(1, &format!("({}) * __builtin_log2({})", g(1), g(0)))));
            }
            if mr == 0xfd {
                return Ok(s_(0, &format!("__builtin_ldexp({}, aot_cvt_s32({}))", g(0), g(1))));
            }
            if mr == 0xfc {
                return Ok(format!("{{ double v = {}; if (v == v && v != 0.0) {} }}", g(0), s_(0, "aot_fround(F, v)")));
            }
            if mr == 0xf5 {
                return Ok("aot_fprem1(F);".to_string());
            }
            if mr == 0xf0 {
                if self.x87_f2xm1_pair(va) {
                    return Ok(s_(0, &format!("__builtin_exp2({})", g(0))));
                }
                return Ok(s_(0, &format!("__builtin_expm1(0.69314718055994531 * {})", g(0))));
            }
        }
        refused(format!("x87 ({}) at {:08x}", i.mnemonic, va))
    }

    fn alu(&self, i: &Insn) -> R<String> {
        let va = i.address;
        let m = i.mnemonic.as_str();
        let ops = &i.ops;
        let w = ops[0].size as u32;
        let a = self.get(i, &ops[0], None)?;
        let mut b = self.get(i, &ops[1], Some(w))?;
        let mm = mask(w);
        if ops[1].is_imm() {
            b = format!("0x{:x}u", imm_mask(ops[1].imm(), w));
        }
        if (m == "xor" || m == "sub") && ops[1].is_reg() && ops[0].is_reg() && ops[0].reg() == ops[1].reg() {
            return Ok(format!("{{ uint32_t a = 0u, r = 0u; {}{} }}", self.flag_store(va, "a", "a", "r", "0"), self.put(i, &ops[0], "0u")?));
        }
        let r = match m {
            "add" => format!("(a + b) & {}", mm),
            "sub" | "cmp" => format!("(a - b) & {}", mm),
            "and" | "test" => "a & b".to_string(),
            "or" => "a | b".to_string(),
            "xor" => "a ^ b".to_string(),
            "adc" => format!("(a + b + c) & {}", mm),
            _ => format!("(a - b - c) & {}", mm),
        };
        let (mut c, mut cv) = (String::new(), "0");
        if m == "adc" || m == "sbb" {
            c = format!(", c = {} ? 1u : 0u", self.carry(va));
            cv = "c";
        }
        let st = self.flag_store(va, "a", "b", "r", cv);
        let wr = if m == "cmp" || m == "test" { String::new() } else { self.put(i, &ops[0], "r")? };
        Ok(format!("{{ uint32_t a = {}, b = {}{}, r = {}; {}{} }}", a, b, c, r, st, wr))
    }

    fn shift(&self, i: &Insn) -> R<String> {
        let va = i.address;
        let m = i.mnemonic.as_str();
        let ops = &i.ops;
        let w = ops[0].size as u32;
        let bits = 8 * w;
        let a = self.get(i, &ops[0], None)?;
        let n = if ops.len() == 1 {
            "1u".to_string()
        } else if ops[1].is_imm() {
            format!("{}u", ops[1].imm() & 31)
        } else {
            "(ecx & 31u)".to_string()
        };
        let mm = mask(w);
        let r = match m {
            "shl" | "sal" => format!("(n ? (a << n) & {} : a)", mm),
            "shr" => "(n ? (a >> n) : a)".to_string(),
            "sar" => {
                let st = match w {
                    1 => "int8_t",
                    2 => "int16_t",
                    _ => "int32_t",
                };
                format!("(n ? (uint32_t)((int32_t)({})a >> n) & {} : a)", st, mm)
            }
            "rol" => format!("(n % {b} ? ((a << (n % {b})) | (a >> ({b} - n % {b}))) & {mm} : a)", b = bits, mm = mm),
            _ => format!("(n % {b} ? ((a >> (n % {b})) | (a << ({b} - n % {b}))) & {mm} : a)", b = bits, mm = mm),
        };
        let st = self.flag_store(va, "a", "n", "r", "0");
        Ok(format!("{{ uint32_t a = {}, n = {}, r = {}; {}{} }}", a, n, r, st, self.put(i, &ops[0], "r")?))
    }

    fn imul(&self, i: &Insn) -> R<String> {
        let va = i.address;
        let ops = &i.ops;
        if ops.len() == 1 {
            if ops[0].size == 1 {
                // ax = al * the byte, signed; its flags aren't modelled (flags.rs
                // gives it no kind, so a function that reads them is refused).
                let v = self.get(i, &ops[0], None)?;
                return Ok(format!(
                    "{{ int32_t p = (int32_t)(int8_t)eax * (int32_t)(int8_t)({}); eax = (eax & 0xffff0000u) | ((uint32_t)p & 0xffffu); }}",
                    v
                ));
            }
            if ops[0].size != 4 {
                return refused(format!("imul of {} bytes at {:08x}", ops[0].size, va));
            }
            let v = self.get(i, &ops[0], None)?;
            return Ok(format!(
                "{{ int64_t p = (int64_t)(int32_t)eax * (int64_t)(int32_t)({}); eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32); {}}}",
                v,
                self.flag_store(va, "0u", "0u", "eax", "(p != (int64_t)(int32_t)p)")
            ));
        }
        let w = ops[0].size;
        if w == 2 {
            // 16 bits (49d3f3's imul cx, cx, 0xf): the product's low word,
            // carry and overflow where it doesn't fit a signed word.
            let a = self.get(i, if ops.len() == 3 { &ops[1] } else { &ops[0] }, None)?;
            let b = self.get(i, if ops.len() == 3 { &ops[2] } else { &ops[1] }, Some(2))?;
            return Ok(format!(
                "{{ int32_t p = (int32_t)(int16_t)({}) * (int32_t)(int16_t)({}); uint32_t r = (uint32_t)p & 0xffffu; {}{} }}",
                a,
                b,
                self.flag_store(va, "0u", "0u", "r", "(p != (int32_t)(int16_t)p)"),
                self.put(i, &ops[0], "r")?
            ));
        }
        if w != 4 {
            return refused(format!("imul of {} bytes at {:08x}", w, va));
        }
        let a = self.get(i, if ops.len() == 3 { &ops[1] } else { &ops[0] }, None)?;
        let b = self.get(i, if ops.len() == 3 { &ops[2] } else { &ops[1] }, Some(4))?;
        Ok(format!(
            "{{ int64_t p = (int64_t)(int32_t)({}) * (int64_t)(int32_t)({}); uint32_t r = (uint32_t)p; {}{} }}",
            a,
            b,
            self.flag_store(va, "0u", "0u", "r", "(p != (int64_t)(int32_t)p)"),
            self.put(i, &ops[0], "r")?
        ))
    }

    fn string(&self, i: &Insn) -> R<String> {
        let va = i.address;
        let m = i.mnemonic.as_str();
        let rep = m.starts_with("rep ");
        let op = m.split_whitespace().last().unwrap_or("");
        let Some(Some(d)) = self.df.get(&va).copied() else {
            return refused(format!("direction flag not known at {:08x}", va));
        };
        let size: Option<i64> = match op.chars().last() {
            Some('b') => Some(1),
            Some('w') => Some(2),
            Some('d') => Some(4),
            _ => None,
        };
        let Some(size) = size else { return refused(format!("{} at {:08x}", m, va)) };
        if op.starts_with("scas") && (m.starts_with("repe ") || m.starts_with("repne ")) {
            // al, ax or eax against what edi is at, while they differ (repne)
            // or are equal (repe) and ecx lasts (issue #138: 687860's and
            // strrchr's repne scasb for a string's end). The flags it leaves
            // aren't modelled: a reader refuses the function.
            let stop = if m.starts_with("repne ") { "v == a" } else { "v != a" };
            let step = (if d != 0 { -size } else { size }) as u32;
            return Ok(format!(
                "{{ const uint32_t a = eax & {}; while (ecx) {{ const uint32_t v = LD{}(edi); edi += {}u; ecx--; if ({}) break; }} }}",
                mask(size as u32),
                sizes(size as u32),
                step,
                stop
            ));
        }
        if op.starts_with("cmps") && size != 1 && (m.starts_with("repe ") || m.starts_with("repne ")) {
            // As cmpsb below, by words or dwords (517a36's repe cmpsd).
            let stop = if m.starts_with("repe ") { "a != b" } else { "a == b" };
            let step = (if d != 0 { -size } else { size }) as u32;
            return Ok(format!(
                "{{ uint32_t a = 0, b = 0; while (ecx) {{ a = LD{}(esi); b = LD{}(edi); esi += {}u; edi += {}u; ecx--; if ({}) break; }} \
uint32_t r = (a - b) & {}; {} }}",
                sizes(size as u32),
                sizes(size as u32),
                step,
                step,
                stop,
                mask(size as u32),
                self.flag_store(va, "a", "b", "r", "0")
            ));
        }
        if op == "cmpsb" && (m.starts_with("repe ") || m.starts_with("repne ")) {
            // Bytes at esi against those at edi, while they are equal (repe) or
            // differ (repne) and ecx lasts; the flags are the last pair's, a
            // byte cmp's. With ecx 0 none is compared and the flags are left:
            // here they read as an equal pair's, which is what the code before
            // leaves where it compares counted strings (issue #138: 40e250's
            // mov ecx, 13; xor eax, eax; repe cmpsb; jnz).
            let stop = if m.starts_with("repe ") { "a != b" } else { "a == b" };
            let step = (if d != 0 { -1i64 } else { 1 }) as u32;
            return Ok(format!(
                "{{ uint32_t a = 0, b = 0; while (ecx) {{ a = M[esi]; b = M[edi]; esi += {}u; edi += {}u; ecx--; if ({}) break; }} \
uint32_t r = (a - b) & 0xffu; {} }}",
                step,
                step,
                stop,
                self.flag_store(va, "a", "b", "r", "0")
            ));
        }
        if !(op.starts_with("movs") || op.starts_with("stos")) || m.starts_with("repe ") || m.starts_with("repne ") {
            return refused(format!("{} at {:08x}", m, va));
        }
        let step = if d != 0 { -size } else { size };
        let st = step as u32;
        let (body, fast) = if op.starts_with("movs") {
            (
                format!("ST{}(edi, LD{}(esi)); esi += {}u; edi += {}u;", sizes(size as u32), sizes(size as u32), st, st),
                format!("aot_movs(edi, esi, ecx, {}, {});", size, d),
            )
        } else {
            let v = match size {
                1 => "(eax & 0xffu)",
                2 => "(eax & 0xffffu)",
                _ => "eax",
            };
            (format!("ST{}(edi, {}); edi += {}u;", sizes(size as u32), v, st), format!("aot_stos(edi, eax, ecx, {}, {});", size, d))
        };
        if !rep {
            return Ok(format!("{{ {} }}", body));
        }
        Ok(format!(
            "{{ uint32_t n = ecx; {} edi += n * {}u; {} ecx = 0; }}",
            fast,
            st,
            if op.starts_with("movs") { format!("esi += n * {}u;", st) } else { String::new() }
        ))
    }

    fn exit(&self, target: &str) -> String {
        format!("aot_exit({}, eax, ecx, edx, ebx, esp, ebp, esi, edi);", target)
    }

    fn fmt_pops(p: Option<i64>) -> String {
        match p {
            Some(v) => v.to_string(),
            None => "None".to_string(),
        }
    }

    fn tail_call(&self, i: &Insn, t: Tgt) -> R<String> {
        let va = i.address;
        let f = self.f();
        let p = self.p;
        if self.loose() {
            // A jump, with esp as it is: the dispatcher goes on at the target
            // (4bae60's pop ebp; jmp eax, whatever the target pops).
            let x = match t {
                Tgt::Direct(d) => format!("0x{:08x}u", d),
                _ => self.get(i, &i.ops[0], None)?,
            };
            return Ok(format!("{{ uint32_t to = {}; {} R->r[4] = esp; return to; }}", x, self.ctx_sets()));
        }
        if f.callouts.contains_key(&va) {
            return self.tail_call_out(i);
        }
        if f.exits.contains(&va) {
            if t.is_pointer() {
                return Ok(format!("{{ uint32_t x = {}; {} }}", self.get(i, &i.ops[0], None)?, self.exit("x")));
            }
            return Ok(self.exit(&format!("0x{:08x}u", t.direct().unwrap())));
        }
        let td = match t {
            Tgt::Direct(d) if !p.intrinsics.contains_key(&d) && !p.natives.contains_key(&d) => d,
            _ => return refused(format!("tail call at {:08x} to an import, a native or an intrinsic", va)),
        };
        let ours = p.summ[&f.entry].pops;
        let theirs = p.callee_summary(t).pops;
        if ours.is_none() || theirs.is_none() || ours != theirs {
            return refused(format!("tail call at {:08x}: pops {}, the target's {}", va, Self::fmt_pops(ours), Self::fmt_pops(theirs)));
        }
        let chk = format!("if (esp != esp0 || LD32(esp) != ra0) aot_bad_ret(0x{:08x}u, esp0, esp); ", f.entry);
        let inner = if p.compiled.contains(&td) {
            format!("\n#ifdef AOT_SPLIT\n    {}\n#else\n    {}\n#endif\n    ", self.call_guest(td), self.call_compiled(td, self.resume.is_some()))
        } else {
            self.call_guest(td)
        };
        if f.noreturn.contains(&va) {
            return Ok(format!("{}{} aot_unreachable(0x{:08x}u);", chk, inner, va));
        }
        if self.regs_ctx() {
            return Ok(format!("{}{} {} R->r[4] = esp; return;", chk, inner, self.ctx_sets()));
        }
        Ok(format!("{}{} {}", chk, inner, self.ret_outs()))
    }

    fn tail_call_out(&self, i: &Insn) -> R<String> {
        let va = i.address;
        let f = self.f();
        let pops = self.p.summ[&f.entry].pops;
        let co = f.callouts[&va];
        if pops != Some(co) {
            return refused(format!("tail call through a pointer at {:08x}: pops {}, the rets' {}", va, Self::fmt_pops(pops), co));
        }
        // With --loose esp is the entry's here by the analysis, as at a ret
        // (else the function is a loose one): the same check as a ret's.
        let off = if self.p.loose_on { format!("aot_bad_ret(0x{:08x}u, esp0, esp);", f.entry) } else { self.exit("x") };
        let s = format!(
            "{{ uint32_t x = {}; if (esp != esp0 || LD32(esp) != ra0) {{ {} }} AotRegs t = {{{{eax, ecx, edx, ebx, esp, ebp, esi, edi}}}}; aot_call_out(x, &t, {}u); {} }} ",
            self.callout_target(i, f.tails[&va])?,
            off,
            co,
            self.callout_back(f.tails[&va])
        );
        if self.regs_ctx() {
            return Ok(format!("{}{} R->r[4] = esp; return;", s, self.ctx_sets()));
        }
        Ok(s + &self.ret_outs())
    }

    /// A callout's target: the pointer, or a loose function's address.
    fn callout_target(&self, i: &Insn, t: Tgt) -> R<String> {
        match t {
            Tgt::Direct(d) => Ok(format!("0x{:08x}u", d)),
            _ => self.get(i, &i.ops[0], None),
        }
    }

    /// The registers a callout's caller takes back: esp, and those its target
    /// isn't taken to keep (through a pointer, eax, ecx and edx; a loose
    /// function's by its summary).
    fn callout_back(&self, t: Tgt) -> String {
        let s = self.p.callee_summary(t);
        let keeps = if t.is_pointer() { crate::program::CALLEE_SAVED } else { s.preserved };
        (0..8u8)
            .filter(|&r| r == ESP || !has(keeps, r))
            .map(|r| format!("{} = t.r[{}];", REGS[r as usize], r))
            .collect::<Vec<_>>()
            .join(" ")
    }

    fn frame_helper(&self, name: &str, ra: u32) -> String {
        let empty = HashMap::new();
        let k = self.p.frame_consts.get(name).unwrap_or(&empty);
        let fs0 = "(uint32_t)(aot_fs_base() + 0x0u)";
        if name == "seh_prolog4" {
            return format!(
                "{{ uint32_t s = esp, n = LD32(s + 4u), e; ST32(s - 4u, 0x{:08x}u); ST32(s - 8u, 0x{:08x}u); \
ST32(s - 12u, LD32({})); ST32(s + 4u, ebp); ebp = s + 4u; e = s - 12u - n; \
ST32(e - 4u, ebx); ST32(e - 8u, esi); ST32(e - 12u, edi); \
{{ uint32_t c = LD32(0x{:08x}u); ST32(ebp - 4u, LD32(ebp - 4u) ^ c); ST32(e - 16u, c ^ ebp); }} \
esp = e - 16u; ST32(ebp - 0x18u, esp); ST32(esp - 4u, LD32(ebp - 8u)); \
ST32(ebp - 8u, LD32(ebp - 4u)); ST32(ebp - 4u, 0xfffffffeu); eax = ebp - 0x10u; \
ST32({}, eax); }}",
                ra, k["h"], fs0, k["c"], fs0
            );
        }
        if name == "eh_prolog3_catch" {
            return format!(
                "{{ uint32_t s = esp, n = LD32(s), e; ST32(s - 4u, 0x{:08x}u); ST32(s - 8u, eax); \
ST32(s - 12u, LD32({})); e = s - 12u - n; \
ST32(e - 4u, ebx); ST32(e - 8u, esi); ST32(e - 12u, edi); ST32(s, ebp); ebp = s; \
ST32(e - 16u, LD32(0x{:08x}u) ^ ebp); esp = e - 16u; ST32(ebp - 0x10u, esp); \
ST32(esp - 4u, LD32(ebp - 4u)); ST32(ebp - 4u, 0xffffffffu); eax = ebp - 0xcu; \
ST32({}, eax); }}",
                ra, fs0, k["c"], fs0
            );
        }
        let off = if name == "seh_epilog4" { 0x10 } else { 0xc };
        format!(
            "{{ uint32_t s = esp, f = ebp; ST32(s - 4u, 0x{:08x}u); ST32({}, LD32(f - 0x{:x}u)); ecx = 0x{:08x}u; \
edi = LD32(s + 4u); esi = LD32(s + 8u); ebx = LD32(s + 12u); ebp = LD32(f); ST32(f, ecx); \
esp = f + 4u; }}",
            ra, fs0, off, ra
        )
    }

    fn call(&self, i: &Insn) -> R<String> {
        let va = i.address;
        let f = self.f();
        let p = self.p;
        let ra = va.wrapping_add(i.size);
        if let Some(&t) = f.tails.get(&va) {
            return self.tail_call(i, t);
        }
        let t = f.calls[&va];
        let push = format!("esp -= 4u; ST32(esp, 0x{:08x}u); ", ra);
        if let Some(&co) = f.callouts.get(&va) {
            return Ok(format!(
                "{{ uint32_t x = {}; {}AotRegs t = {{{{eax, ecx, edx, ebx, esp, ebp, esi, edi}}}}; aot_call_out(x, &t, {}u); {} }}",
                self.callout_target(i, t)?,
                push,
                co,
                self.callout_back(t)
            ));
        }
        if f.exits.contains(&va) {
            let x = if t.is_pointer() { self.get(i, &i.ops[0], None)? } else { format!("0x{:08x}u", t.direct().unwrap()) };
            return Ok(format!("{{ uint32_t x = {}; {}{} }}", x, push, self.exit("x")));
        }
        if let Some(&name) = t.direct().and_then(|d| p.intrinsics.get(&d)) {
            if name == "memset" {
                return Ok(push
                    + "{ uint32_t d = LD32(esp + 4u); aot_memset(d, LD32(esp + 8u), LD32(esp + 12u)); eax = d; esp += 4u; }");
            }
            if name == "memcpy" {
                return Ok(push
                    + "{ uint32_t d = LD32(esp + 4u); aot_memmove(d, LD32(esp + 8u), LD32(esp + 12u)); eax = d; esp += 4u; }");
            }
            if name == "strrchr" {
                // The last of a character in a string, its end included; ecx
                // and edx are the callee's scratch.
                return Ok(push
                    + "{ uint32_t s = LD32(esp + 4u), n = 0; uint8_t ch = (uint8_t)LD32(esp + 8u); while (M[s + n]) n++; \
eax = 0; for (uint32_t k = n + 1u; k-- > 0u;) if (M[s + k] == ch) { eax = s + k; break; } esp += 4u; }");
            }
            if name == "fpow" {
                // st1 to the power st0, the two for one; eax, ecx and edx are the callee's scratch.
                return Ok(push
                    + "{ double y = aot_fget(F, 0), x = aot_fget(F, 1); aot_fpop(F); aot_fset(F, 0, __builtin_pow(x, y)); esp += 4u; }");
            }
            if crate::program::FMATH.contains(&name) {
                // st0's sine, cosine, square root or logarithm, in st0; eax, ecx and edx are the callee's scratch.
                return Ok(push + &format!("{{ aot_fset(F, 0, __builtin_{}(aot_fget(F, 0))); esp += 4u; }}", &name[1..]));
            }
            if name == "alloca_probe_16" {
                return Ok(push
                    + &format!(
                        "{{ uint32_t c = ((esp + 4u) - eax) & 0xfu, n = eax + c; eax = n < c ? 0xffffffffu : n; }} \
ST32(esp - 4u, ecx); ST32(esp - eax, 0x{:08x}u); esp = esp - eax + 4u; eax = 0x{:08x}u;",
                        ra, ra
                    ));
            }
            if name == "alloca_probe" {
                return Ok(push
                    + &format!(
                        "ST32(esp - 4u, ecx); ST32(esp - eax, 0x{:08x}u); esp = esp - eax + 4u; eax = 0x{:08x}u;",
                        ra, ra
                    ));
            }
            if FRAME_NAMES.contains(&name) {
                return Ok(self.frame_helper(name, ra));
            }
            return refused(format!("intrinsic {}", name));
        }
        let Tgt::Direct(td) = t else {
            // An import that doesn't return (ExitProcess in the C runtime's
            // own exit, 681aa1): an exit to its shim, the thread's last. With
            // Box86 the thread goes on there; without, the runtime runs the
            // shim in place (aot_exit).
            if f.noreturn.contains(&va) {
                return Ok(format!("{{ uint32_t x = {}; {}{} }}", self.get(i, &i.ops[0], None)?, push, self.exit("x")));
            }
            return refused(format!("an import or indirect call at {:08x}", va));
        };
        if p.loose_jump(f, va) {
            return Ok(format!("{}{{ {} R->r[4] = esp; return 0x{:08x}u; }}", push, self.ctx_sets(), td));
        }
        let nat = p.natives.get(&td);
        let mut inner = if p.compiled.contains(&td) {
            format!("\n#ifdef AOT_SPLIT\n    {}\n#else\n    {}\n#endif\n    ", self.call_guest(td), self.call_compiled(td, true))
        } else {
            self.call_guest(td)
        };
        if f.noreturn.contains(&va) {
            inner += &format!(" aot_unreachable(0x{:08x}u);", va);
        }
        if let Some(n) = nat {
            let mut args: Vec<String> = n.args.iter().map(|&r| REGS[r as usize].to_string()).collect();
            args.extend((0..n.stack).map(|k| format!("LD32(esp + {}u)", 4 + 4 * k)));
            if !n.handback {
                return Ok(format!("{}eax = (uint32_t){}({}); esp += {}u;", push, n.glue, args.join(", "), 4 + 4 * n.stack));
            }
            return Ok(format!(
                "{}{{ uint64_t n = {}({}); if ((uint32_t)(n >> 32)) {{ eax = (uint32_t)n; esp += {}u; }} else {{ {} }} }}",
                push,
                n.glue,
                args.join(", "),
                4 + 4 * n.stack,
                inner
            ));
        }
        Ok(push + &inner)
    }
}

/// The direction flag at each instruction: clear at entry and after calls
/// (MSVC), std/cld change it; None where paths disagree.
pub fn direction(f: &Func) -> HashMap<u32, Option<u8>> {
    let mut df = HashMap::new();
    let mut inb: HashMap<u32, Option<u8>> = HashMap::new();
    inb.insert(f.entry, Some(0));
    let mut work = vec![f.entry];
    while let Some(b) = work.pop() {
        let mut d = inb[&b];
        let mut va = b;
        loop {
            df.insert(va, d);
            let i = &f.insns[&va];
            if i.mnemonic == "std" {
                d = Some(1);
            } else if i.mnemonic == "cld" || f.calls.contains_key(&va) {
                d = Some(0);
            }
            let succ = &f.succ[&va];
            let nxt = va.wrapping_add(i.size);
            if succ.len() == 1 && succ[0] == nxt && !f.leaders.contains(&nxt) {
                va = nxt;
                continue;
            }
            for &t in succ {
                let o = inb.get(&t).copied().unwrap_or(d);
                let nv = if o == d { d } else { None };
                if !inb.contains_key(&t) || inb[&t] != nv {
                    inb.insert(t, nv);
                    work.push(t);
                }
            }
            break;
        }
    }
    df
}
