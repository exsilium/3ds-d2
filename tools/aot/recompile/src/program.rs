//! The program: every function decoded so far, their summaries and exits
//! (recompile.py's Summary and Program).

use crate::analysis::Analysis;
use crate::func::{Func, Tgt};
use crate::image::Image;
use crate::omap::OMap;
use crate::x86::*;
use std::cell::RefCell;
use std::collections::{BTreeSet, HashMap, HashSet, VecDeque};
use std::rc::Rc;

/// What a caller needs to know of a function.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Summary {
    pub reads: Regs,
    pub preserved: Regs,
    pub pops: Option<i64>,
    pub known: bool,
}

pub const UNKNOWN: Summary = Summary { reads: ALL, preserved: 0, pops: None, known: false };
pub const CALLEE_SAVED: Regs = (1 << EBX) | (1 << EBP) | (1 << ESI) | (1 << EDI);

/// Functions that never return: exit(-1) (0x681e09, the game's internal
/// errors), and __report_gsfailure (0x6889d6, issue #138), which ends in
/// TerminateProcess(GetCurrentProcess(), 0xc0000409) before a ret the
/// decoding sees.
pub const NORETURN_FUNCS: [u32; 2] = [0x00681e09, 0x006889d6];
pub const ERROR_PATH_FUNCS: [u32; 2] = [0x00408090, 0x00408a60];

#[derive(Clone, Debug)]
#[allow(dead_code)]
pub struct Native {
    pub name: &'static str,
    pub glue: &'static str,
    pub args: Vec<u8>,
    pub stack: i64,
    pub reads: Vec<u8>,
    pub handback: bool,
}

pub fn natives() -> HashMap<u32, Native> {
    let mut m = HashMap::new();
    m.insert(
        0x00463740,
        Native { name: "RoomFind", glue: "aot_native_463740", args: vec![ECX, EDX], stack: 1, reads: vec![ECX, EDX], handback: false },
    );
    m.insert(
        0x0064d4e0,
        Native { name: "CollisionCross", glue: "aot_native_64d4e0", args: vec![ESI], stack: 3, reads: vec![ESI], handback: true },
    );
    m
}

/// The C runtime's x87 math entries compiled as libm's (st0 in, st0 out; fpow
/// takes st1 and st0).
pub const FMATH: [&str; 6] = ["fsin", "fcos", "fsqrt", "flog", "flog10", "fpow"];

pub fn intrinsics() -> HashMap<u32, &'static str> {
    let mut m = HashMap::new();
    m.insert(0x00681ef0, "memset");
    m.insert(0x006829c0, "memcpy");
    m.insert(0x00683840, "memcpy");   // memmove: the same glue (aot_memmove)
    // The C runtime's sine and cosine of st0, in st0 (issue #138): hand-written
    // code that passes flags and the stack between its pieces (688590 into
    // 6885e8, 6987d8, 6987fb), refused. 61bb80 calls them.
    m.insert(0x00682990, "strrchr");   // repne scasb twice, the second with the direction flag set
    m.insert(0x00688590, "fsin");
    m.insert(0x006886c0, "fcos");
    // ... and its square root, logarithms and power, the same way (6879dd,
    // 687b18, 687ef8, 687c6d refused). A build that reports no SSE2 (the
    // 3DS's) takes these x87 paths; the lane's takes the SSE2 ones.
    m.insert(0x006879c0, "fsqrt");
    m.insert(0x00687ac0, "flog");
    m.insert(0x00687ea0, "flog10");
    m.insert(0x00687c10, "fpow");   // st1 to the power st0, popped
    m.insert(0x006831b0, "alloca_probe");
    // The same with the block's address on 16 bytes: eax rounded up by what
    // the caller's esp less eax leaves over, then alloca_probe (a tail call).
    m.insert(0x006869d0, "alloca_probe_16");
    m
}

pub const FRAME_NAMES: [&str; 4] = ["seh_prolog4", "seh_epilog4", "eh_prolog3_catch", "eh_epilog3"];

fn epilog(off: &str) -> Vec<String> {
    [
        "mov ecx, dword ptr [ebp - {off}]",
        "mov dword ptr fs:[0], ecx",
        "pop ecx",
        "pop edi",
        "pop edi",
        "pop esi",
        "pop ebx",
        "mov esp, ebp",
        "pop ebp",
        "push ecx",
        "ret ",
    ]
    .iter()
    .map(|x| x.replace("{off}", off))
    .collect()
}

/// The CRT's frame helpers (issue #138), each only where its code is this.
pub fn frame_helpers() -> Vec<(u32, &'static str, Vec<String>)> {
    let s = |v: &[&str]| v.iter().map(|x| x.to_string()).collect::<Vec<_>>();
    vec![
        (
            0x00687fb0,
            "seh_prolog4",
            s(&[
                "push {h}",
                "push dword ptr fs:[0]",
                "mov eax, dword ptr [esp + 0x10]",
                "mov dword ptr [esp + 0x10], ebp",
                "lea ebp, [esp + 0x10]",
                "sub esp, eax",
                "push ebx",
                "push esi",
                "push edi",
                "mov eax, dword ptr [{c}]",
                "xor dword ptr [ebp - 4], eax",
                "xor eax, ebp",
                "push eax",
                "mov dword ptr [ebp - 0x18], esp",
                "push dword ptr [ebp - 8]",
                "mov eax, dword ptr [ebp - 4]",
                "mov dword ptr [ebp - 4], 0xfffffffe",
                "mov dword ptr [ebp - 8], eax",
                "lea eax, [ebp - 0x10]",
                "mov dword ptr fs:[0], eax",
                "ret ",
            ]),
        ),
        (0x00687ff5, "seh_epilog4", epilog("0x10")),
        (
            0x0069d4df,
            "eh_prolog3_catch",
            s(&[
                "push eax",
                "push dword ptr fs:[0]",
                "lea eax, [esp + 0xc]",
                "sub esp, dword ptr [esp + 0xc]",
                "push ebx",
                "push esi",
                "push edi",
                "mov dword ptr [eax], ebp",
                "mov ebp, eax",
                "mov eax, dword ptr [{c}]",
                "xor eax, ebp",
                "push eax",
                "mov dword ptr [ebp - 0x10], esp",
                "push dword ptr [ebp - 4]",
                "mov dword ptr [ebp - 4], 0xffffffff",
                "lea eax, [ebp - 0xc]",
                "mov dword ptr fs:[0], eax",
                "ret ",
            ]),
        ),
        (0x0069d584, "eh_epilog3", epilog("0xc")),
    ]
}

/// `text` against a template line with {h} and {c} for 0x-hex numbers.
fn match_template(w: &str, text: &str, got: &mut HashMap<String, u32>) -> bool {
    let (mut wi, mut ti) = (0usize, 0usize);
    let wb = w.as_bytes();
    let tb = text.as_bytes();
    while wi < wb.len() {
        if w[wi..].starts_with("{h}") || w[wi..].starts_with("{c}") {
            let key = &w[wi + 1..wi + 2];
            if !text[ti..].starts_with("0x") {
                return false;
            }
            let mut k = ti + 2;
            while k < tb.len() && (tb[k].is_ascii_digit() || (b'a'..=b'f').contains(&tb[k])) {
                k += 1;
            }
            if k == ti + 2 {
                return false;
            }
            let v = u32::from_str_radix(&text[ti + 2..k], 16).unwrap_or(0);
            got.insert(key.to_string(), v);
            ti = k;
            wi += 3;
            continue;
        }
        if ti >= tb.len() || tb[ti] != wb[wi] {
            return false;
        }
        ti += 1;
        wi += 1;
    }
    ti == tb.len()
}

fn frame_helper_consts(img: &Image, mut va: u32, want: &[String]) -> Option<HashMap<String, u32>> {
    let mut got = HashMap::new();
    for w in want {
        let i = img.insn(va).ok()?;
        let text = format!("{} {}", i.mnemonic, i.op_str);
        if !match_template(w, &text, &mut got) {
            return None;
        }
        va = va.wrapping_add(i.size);
    }
    Some(got)
}

pub struct Program<'a> {
    pub img: &'a Image,
    pub starts: HashSet<u32>,
    pub natives: HashMap<u32, Native>,
    pub intrinsics: HashMap<u32, &'static str>,
    pub frame_consts: HashMap<&'static str, HashMap<String, u32>>,
    pub garbage: HashMap<u32, Regs>,
    pub thunk_pops: Option<HashMap<u32, i64>>,
    funcs: RefCell<OMap<u32, Rc<Func>>>,
    pub summ: HashMap<u32, Summary>,
    pub compiled: BTreeSet<u32>,
    pub may_exit: HashSet<u32>,
    pub may_hard: HashSet<u32>,
    pub plain: HashSet<u32>,
    /// ... and those whose calls to imports are plain exits too.
    pub plain_all: HashSet<u32>,
    /// Pops found for a function's guessed callouts (solve_pops), kept across its rebuilds.
    pub pops_fix: HashMap<u32, Vec<(u32, i64)>>,
    /// Why each of plain's was made plain: the refusal its callouts gave.
    pub plain_why: HashMap<u32, String>,
    pub noreturn_funcs: HashSet<u32>,
    pub why: HashMap<u32, String>,
    pub cfg_pops: HashMap<u32, Option<i64>>,
    pub summarized: bool,
    pub callouts: bool,
    /// --loose (issue #138, a build without Box86): a function refused only
    /// for its stack is compiled loose instead.
    pub loose_on: bool,
    /// AOT_HOT=<file> (issue #153): the functions that ran in the profiles
    /// (tools/aot/hot_list.py), one address a line. The others are written
    /// AOT_FNC, which a build may compile for size: 72% of the code's bytes
    /// never ran in the lane's scripts. None: every function AOT_FN.
    pub hot: Option<HashSet<u32>>,
    /// The functions compiled loose: esp at their rets, or what their tail
    /// jumps' targets pop, isn't known (4bae60's pop ebp; jmp eax through a
    /// vtable). All eight registers in and out, no check at the ret: it says
    /// where control goes on and with what esp, and the dispatcher takes both
    /// from it. No compiled caller calls one in C: a call to one is a callout
    /// to its address.
    pub loose: BTreeSet<u32>,
    /// Why each of them is: the refusal it would have had.
    pub loose_why: HashMap<u32, String>,
    /// With loose_on, flags across calls (flags.rs): the functions a jcc of
    /// whose reads the flags they were entered with (the C runtime's 687b18:
    /// push edx; fstcw [esp]; jz), and those whose callers read the flags
    /// they return with (698795). Callers of the first store theirs in
    /// aot_xf at the call, the second at each ret.
    pub flag_in: HashSet<u32>,
    pub flag_out: HashSet<u32>,
}

impl<'a> Program<'a> {
    pub fn new(img: &'a Image, starts: HashSet<u32>, natives: HashMap<u32, Native>, callouts: bool) -> Program<'a> {
        let mut intr = intrinsics();
        let mut frame_consts = HashMap::new();
        for (va, name, want) in frame_helpers() {
            if let Some(got) = frame_helper_consts(img, va, &want) {
                intr.insert(va, name);
                frame_consts.insert(name, got);
            }
        }
        Program {
            img,
            starts,
            natives,
            intrinsics: intr,
            frame_consts,
            garbage: HashMap::new(),
            thunk_pops: None,
            funcs: RefCell::new(OMap::new()),
            summ: HashMap::new(),
            compiled: BTreeSet::new(),
            may_exit: HashSet::new(),
            may_hard: HashSet::new(),
            plain: HashSet::new(),
            plain_all: HashSet::new(),
            pops_fix: HashMap::new(),
            plain_why: HashMap::new(),
            noreturn_funcs: HashSet::new(),
            why: HashMap::new(),
            cfg_pops: HashMap::new(),
            summarized: false,
            callouts,
            loose_on: false,
            hot: hot_list(),
            loose: BTreeSet::new(),
            loose_why: HashMap::new(),
            flag_in: HashSet::new(),
            flag_out: HashSet::new(),
        }
    }

    pub fn func(&self, va: u32) -> Rc<Func> {
        if let Some(f) = self.funcs.borrow().get(&va) {
            return f.clone();
        }
        let f = Rc::new(Func::new(self.img, va, &self.starts));
        self.funcs.borrow_mut().insert(va, f.clone());
        f
    }

    pub fn set_func(&self, va: u32, f: Func) {
        self.funcs.borrow_mut().insert(va, Rc::new(f));
    }

    /// The decoded functions' addresses, in the order they were first decoded.
    pub fn func_keys(&self) -> Vec<u32> {
        self.funcs.borrow().keys().copied().collect()
    }

    pub fn native_direct(&self, t: u32) -> bool {
        self.natives.get(&t).is_some_and(|n| !n.handback)
    }

    /// A function that can hand its flags back at its rets: one this
    /// recompiler writes (not an intrinsic's or a native's, written in line
    /// or run by the host).
    pub fn can_flag_out(&self, t: u32) -> bool {
        !self.intrinsics.contains_key(&t) && !self.natives.contains_key(&t) && !ERROR_PATH_FUNCS.contains(&t) && self.func(t).refused.is_none()
    }

    /// Not on AOT_HOT's list (false with no list).
    pub fn is_cold(&self, va: u32) -> bool {
        self.hot.as_ref().is_some_and(|h| !h.contains(&va))
    }

    /// Compiled loose (and not refused since for something else).
    pub fn is_loose(&self, va: u32) -> bool {
        self.loose.contains(&va) && self.compiled.contains(&va)
    }

    /// A call in a loose function to a loose one that doesn't come back to
    /// the code after it (40d809's call 0x40d80e, the next instruction, for
    /// its own address; d2d's list starts a function there): the return
    /// address pushed and a jump, the dispatcher's as a tail jump is.
    pub fn loose_jump(&self, f: &Func, va: u32) -> bool {
        self.is_loose(f.entry)
            && f.noreturn.contains(&va)
            && !f.exits.contains(&va)
            && f.calls.get(&va).and_then(|t| t.direct()).is_some_and(|d| self.is_loose(d))
    }

    /// Each loose function with what its rets pop, where they all pop the
    /// same: what a callout to it counts on. None: the site's own count.
    pub fn loose_pops(&self) -> HashMap<u32, Option<i64>> {
        self.loose
            .iter()
            .filter(|va| self.compiled.contains(va))
            .map(|&va| {
                let f = self.func(va);
                (va, if f.ret_pops.len() == 1 { f.ret_pops.iter().next().copied() } else { None })
            })
            .collect()
    }

    /// Every function the roots reach by direct calls.
    pub fn closure(&self, roots: &[u32], limit: usize) -> HashSet<u32> {
        let mut seen = HashSet::new();
        let mut work: Vec<u32> = roots.to_vec();
        while !work.is_empty() && seen.len() < limit {
            let va = work.pop().unwrap();
            if seen.contains(&va) || self.intrinsics.contains_key(&va) {
                continue;
            }
            seen.insert(va);
            let f = self.func(va);
            if f.refused.is_some() {
                continue;
            }
            for t in f.calls.values().chain(f.tails.values()) {
                if let Tgt::Direct(t) = t {
                    work.push(*t);
                }
            }
        }
        seen
    }

    pub fn callee_summary(&self, t: Tgt) -> Summary {
        let t = match t {
            Tgt::Indirect => {
                return Summary { reads: set_of(&[ECX, EDX, ESP]), preserved: CALLEE_SAVED, pops: None, known: true }
            }
            Tgt::Import(slot) => {
                return Summary {
                    // A native (issue #138) reads what it likes of the registers.
                    reads: if self.img.native_slots.contains(&slot) { ALL } else { bit(ESP) },
                    preserved: CALLEE_SAVED,
                    pops: self.img.import_pops.get(&slot).copied().flatten(),
                    known: true,
                }
            }
            Tgt::Direct(t) => t,
        };
        if let Some(n) = self.natives.get(&t) {
            if !n.handback {
                return Summary { reads: set_of(&n.reads) | bit(ESP), preserved: ALL & !bit(EAX), pops: Some(4 * n.stack), known: true };
            }
        }
        if let Some(&name) = self.intrinsics.get(&t) {
            return match name {
                "alloca_probe" | "alloca_probe_16" => Summary {
                    reads: set_of(&[EAX, ECX, ESP]),
                    preserved: set_of(&[ECX, EDX, EBX, EBP, ESI, EDI]),
                    pops: Some(0),
                    known: true,
                },
                "seh_prolog4" | "eh_prolog3_catch" => Summary {
                    reads: set_of(&[EBX, EBP, ESI, EDI, ESP]) | if name == "eh_prolog3_catch" { bit(EAX) } else { 0 },
                    preserved: set_of(&[ECX, EDX, EBX, ESI, EDI]),
                    pops: None,
                    known: true,
                },
                "seh_epilog4" | "eh_epilog3" => {
                    Summary { reads: set_of(&[EBP, ESP]), preserved: set_of(&[EAX, EDX]), pops: None, known: true }
                }
                _ => Summary { reads: bit(ESP), preserved: CALLEE_SAVED, pops: Some(0), known: true },
            };
        }
        let mut s = self.summ.get(&t).copied().unwrap_or(UNKNOWN);
        if !self.compiled.contains(&t) {
            s.preserved |= CALLEE_SAVED;
        }
        if self.is_loose(t) {
            // Reached through the dispatcher with every register, and what it
            // pops is the call site's to count (its callout's). Of what it
            // keeps, only the callee-saved registers its own code shows kept.
            return Summary { reads: ALL, preserved: s.preserved & CALLEE_SAVED, pops: None, known: true };
        }
        s
    }

    /// A compiled function's exits (issue #115).
    pub fn exit_sites(&self, f: &Func, blocks: &HashMap<u32, bool>) -> BTreeSet<u32> {
        let mut out = BTreeSet::new();
        let loose = self.is_loose(f.entry);
        for (site, t) in f.calls.iter().chain(f.tails.iter()) {
            if f.noreturn.contains(site) && !f.exits.contains(site) {
                continue;
            }
            // A loose function's tail jumps leave through the dispatcher,
            // wherever they go: no exit, no callout.
            if loose && f.tails.contains_key(site) {
                continue;
            }
            let t = match t {
                Tgt::Direct(t) => *t,
                _ => {
                    out.insert(*site);
                    continue;
                }
            };
            // A callout to its address (but alloca_probe and its like, loose
            // where they stand as functions, are still written in line).
            if self.is_loose(t) && !self.intrinsics.contains_key(&t) && !self.native_direct(t) {
                out.insert(*site);
                continue;
            }
            if self.compiled.contains(&t)
                || self.intrinsics.contains_key(&t)
                || ERROR_PATH_FUNCS.contains(&t)
                || NORETURN_FUNCS.contains(&t)
            {
                continue;
            }
            if self.native_direct(t) {
                continue;
            }
            if *blocks.get(&t).unwrap_or(&true) {
                out.insert(*site);
            }
        }
        out
    }

    fn renderer_slot_pops(&self, f: &Func) -> Option<i64> {
        const RENDERER: i64 = 0x007c8cc0;
        const GDI_TABLE: u32 = 0x0074c4a8;
        let mut order: Vec<u32> = f.insns.keys().copied().collect();
        order.sort();
        let mut table_reg: Option<u16> = None;
        for a in order {
            let i = &f.insns[&a];
            for (k, op) in i.ops.iter().enumerate() {
                if !op.is_mem() {
                    continue;
                }
                let m = op.mem();
                if m.base == 0 && m.index == 0 && m.disp == RENDERER && i.mnemonic == "mov" && k == 1 && i.ops[0].is_reg() {
                    table_reg = Some(i.ops[0].reg());
                } else if table_reg.is_some_and(|r| m.base == r) && m.index == 0 && (0..0x400).contains(&m.disp) && m.disp % 4 == 0 {
                    // The three video slots hold cinematics.cpp's play loop
                    // and two natives when it is installed, with the game's
                    // own pops ("return 0", "ret 4", "return 0").
                    match m.disp {
                        0x34 | 0x3c => return Some(0),
                        0x38 => return Some(4),
                        _ => {}
                    }
                    let mut b = [0u8; 4];
                    for (j, x) in b.iter_mut().enumerate() {
                        *x = self.img.u8(GDI_TABLE + m.disp as u32 + j as u32);
                    }
                    let t = u32::from_le_bytes(b);
                    if !self.img.in_text(t) {
                        return None;
                    }
                    let g = self.func(t);
                    if g.refused.is_some() || g.ret_pops.len() != 1 {
                        return None;
                    }
                    return g.ret_pops.iter().next().copied();
                }
            }
        }
        None
    }

    /// What each thunk through a pointer pops (issue #138).
    pub fn compute_thunk_pops(&self) -> HashMap<u32, i64> {
        let img = self.img;
        // A pop count, or "-1" for a target through a pointer: None stays None.
        let mut thunks: HashMap<u32, Vec<Option<i64>>> = HashMap::new();
        let keys = self.func_keys();
        for &va in &keys {
            let f = self.func(va);
            if f.refused.is_some() || !f.ret_pops.is_empty() || f.tail_targets.is_empty() || !f.tail_targets.iter().all(|t| t.is_pointer())
            {
                continue;
            }
            let mut imp: Vec<Option<i64>> = Vec::new();
            for t in &f.tail_targets {
                let v = match t {
                    Tgt::Import(slot) => img.import_pops.get(slot).copied().flatten(),
                    _ => Some(-1),
                };
                if !imp.contains(&v) {
                    imp.push(v);
                }
            }
            // A jump through the renderer's table (issue #138: 4f5d60's mov
            // eax, [7c8cc0]; mov eax, [eax + 0x34]; jmp eax): what the GDI
            // renderer's function in that slot pops (its table at 0x74c4a8;
            // every renderer's has the slot's signature).
            if imp == vec![Some(-1)] {
                if let Some(p) = self.renderer_slot_pops(&f) {
                    imp = vec![Some(p)];
                }
            }
            thunks.insert(va, if imp == vec![Some(-1)] { Vec::new() } else { imp });
        }
        let mut guess: HashMap<u32, Vec<Option<i64>>> = HashMap::new();
        for &va in &keys {
            let f = self.func(va);
            if f.refused.is_some() {
                continue;
            }
            for (site, t) in f.calls.iter() {
                if let Tgt::Direct(t) = t {
                    if thunks.get(t).is_some_and(|s| s.is_empty()) {
                        let v = Some(f.callout_pops(*site, img));
                        let g = guess.entry(*t).or_default();
                        if !g.contains(&v) {
                            g.push(v);
                        }
                    }
                }
            }
            for t in f.tails.values() {
                if let Tgt::Direct(t) = t {
                    if thunks.contains_key(t) {
                        let g = guess.entry(*t).or_default();
                        if !g.contains(&None) {
                            g.push(None);
                        }
                    }
                }
            }
        }
        let mut out = HashMap::new();
        for (va, ps) in thunks {
            let ps = if ps.is_empty() { guess.get(&va).cloned().unwrap_or_default() } else { ps };
            if ps.len() == 1 && ps[0].is_some() && ps[0] != Some(-1) {
                out.insert(va, ps[0].unwrap());
            }
        }
        out
    }

    /// The function with `fix` as its guessed callouts' pops (solve_pops).
    pub fn with_pops(&mut self, va: u32, fix: Vec<(u32, i64)>) {
        let f = self.func(va);
        let mut g = Func::new(self.img, va, &self.starts);
        g.pops_fix = fix.clone();
        let tp = self.thunk_pops.as_ref().and_then(|m| m.get(&va).copied());
        g.set_exits(&f.exits, 0, tp, self.img, self.callouts, &self.loose_pops());
        self.pops_fix.insert(va, fix);
        self.set_func(va, g);
    }

    /// A function whose callouts' pops threw its stack off: its indirect
    /// calls plain exits instead, its calls to imports whose pops are known
    /// still callouts; a second time, those too. False if they all were
    /// already.
    pub fn make_plain(&mut self, va: u32, why: &str) -> bool {
        let f = self.func(va);
        if f.plain_all || f.callouts.is_empty() {
            return false;
        }
        let level = if f.plain { 2 } else { 1 };
        if level == 2 {
            self.plain_all.insert(va);
        }
        self.plain.insert(va);
        self.plain_why.insert(va, why.to_string());
        let mut g = Func::new(self.img, va, &self.starts);
        g.pops_fix = self.pops_fix.get(&va).cloned().unwrap_or_default();
        let tp = self.thunk_pops.as_ref().and_then(|m| m.get(&va).copied());
        g.set_exits(&f.exits, level, tp, self.img, self.callouts, &self.loose_pops());
        self.set_func(va, g);
        true
    }

    /// A function refused only for its stack, compiled loose instead (see
    /// `loose`). Its own exits and its callers' are the next apply_exits'.
    pub fn make_loose(&mut self, va: u32, why: &str) {
        self.loose.insert(va);
        self.loose_why.insert(va, why.to_string());
        self.plain.remove(&va);
        self.plain_all.remove(&va);
        self.plain_why.remove(&va);
        self.pops_fix.remove(&va);
    }

    /// These functions' exits for the compiled set as it is now (apply_exits'
    /// first half; alone, for a function just made loose and its callers).
    pub fn exits_of(&mut self, vas: &[u32], blocks: &HashMap<u32, bool>, loose: &HashMap<u32, Option<i64>>) -> usize {
        let mut n = 0;
        for &va in vas {
            let f = self.func(va);
            if f.refused.is_some() {
                continue;
            }
            let want = if self.compiled.contains(&va) {
                if !f.exits.is_empty() {
                    self.exit_sites(&Func::new(self.img, va, &self.starts), blocks)
                } else {
                    self.exit_sites(&f, blocks)
                }
            } else {
                BTreeSet::new()
            };
            // A callee made loose, or refused since: the site was a callout to
            // it by its rets' pops, or will be.
            let same_loose = f.loose_calls == f.loose_calls_for(loose);
            if want == f.exits && same_loose && f.plain == self.plain.contains(&va) && f.plain_all == self.plain_all.contains(&va) {
                continue;
            }
            let mut g = Func::new(self.img, va, &self.starts);
            g.pops_fix = self.pops_fix.get(&va).cloned().unwrap_or_default();
            if !want.is_empty() {
                let tp = self.thunk_pops.as_ref().and_then(|m| m.get(&va).copied());
                let level = if self.plain_all.contains(&va) { 2 } else if self.plain.contains(&va) { 1 } else { 0 };
                g.set_exits(&want, level, tp, self.img, self.callouts, loose);
            }
            self.set_func(va, g);
            n += 1;
        }
        n
    }

    /// Each compiled function's exits for the compiled set as it is now.
    pub fn apply_exits(&mut self, blocks: &HashMap<u32, bool>) -> usize {
        if self.thunk_pops.is_none() {
            self.thunk_pops = Some(self.compute_thunk_pops());
        }
        let loose = self.loose_pops();
        let mut n = self.exits_of(&self.func_keys(), blocks, &loose);
        let calls_of = |f: &Func| -> Vec<u32> { f.calls.values().chain(f.tails.values()).filter_map(|t| t.direct()).collect() };
        let mut me: HashSet<u32> = self.compiled.iter().copied().filter(|&va| !self.func(va).exits.is_empty()).collect();
        let mut changed = true;
        while changed {
            changed = false;
            for &va in &self.compiled {
                if me.contains(&va) {
                    continue;
                }
                if calls_of(&self.func(va)).iter().any(|t| me.contains(t)) {
                    me.insert(va);
                    changed = true;
                }
            }
        }
        if me != self.may_exit {
            n += 1;
        }
        self.may_exit = me;
        let mut hard: HashSet<u32> = self
            .compiled
            .iter()
            .copied()
            .filter(|&va| {
                let f = self.func(va);
                f.exits.iter().any(|s| !f.callouts.contains_key(s))
            })
            .collect();
        let mut changed = true;
        while changed {
            changed = false;
            for &va in &self.compiled {
                if hard.contains(&va) {
                    continue;
                }
                if calls_of(&self.func(va)).iter().any(|t| hard.contains(t)) {
                    hard.insert(va);
                    changed = true;
                }
            }
        }
        self.may_hard = hard;
        n
    }

    /// Functions with no ret left once their calls to such functions end
    /// their paths.
    pub fn mark_noreturn(&mut self, vas: &[u32]) {
        let mut nr: HashSet<u32> = NORETURN_FUNCS.iter().copied().collect();
        let mut changed = true;
        while changed {
            changed = false;
            for &va in vas {
                let mut f = self.func(va);
                if f.refused.is_some() || nr.contains(&va) {
                    continue;
                }
                let would = f.calls.iter().any(|(s, t)| t.direct().is_some_and(|d| nr.contains(&d)) && !f.noreturn.contains(s));
                if would {
                    let mut g = (*f).clone();
                    g.prune(&nr);
                    self.set_func(va, g);
                    f = self.func(va);
                    changed = true;
                }
                if f.rets.is_empty() && f.exits.is_empty() && f.tails.values().all(|t| t.direct().is_some_and(|d| nr.contains(&d))) {
                    nr.insert(va);
                    changed = true;
                }
            }
        }
        self.noreturn_funcs = nr;
    }

    /// Summaries to a fixed point: reads from nothing up, preserved from
    /// everything down.
    pub fn summarize(&mut self, vas: &[u32], rounds: usize) {
        self.mark_noreturn(vas);
        self.why = HashMap::new();
        self.cfg_pops = HashMap::new();
        self.summarized = true;
        for &va in vas {
            let f = self.func(va);
            let mut pops: BTreeSet<i64> = if f.refused.is_none() { f.rets.values().copied().collect() } else { BTreeSet::new() };
            if pops.is_empty() && !f.exits.is_empty() && f.refused.is_none() {
                pops = f.ret_pops.clone();
            }
            if pops.is_empty() && f.refused.is_none() {
                if let Some(p) = self.thunk_pops.as_ref().and_then(|m| m.get(&va)) {
                    pops.insert(*p);
                }
            }
            let cp = if pops.len() == 1 { pops.iter().next().copied() } else { None };
            self.cfg_pops.insert(va, cp);
            // (A loose function's rets may pop what they like: no caller counts on it.)
            let known = f.refused.is_none() && (pops.len() <= 1 || self.is_loose(va));
            self.summ.insert(va, Summary { reads: 0, preserved: ALL, pops: cp, known });
        }
        let mut changed = true;
        while changed {
            changed = false;
            for &va in vas {
                let f = self.func(va);
                if self.cfg_pops[&va].is_some()
                    || f.refused.is_some()
                    || !f.ret_pops.is_empty()
                    || f.tail_targets.is_empty()
                    || self.noreturn_funcs.contains(&va)
                {
                    continue;
                }
                let mut ps: Vec<Option<i64>> = Vec::new();
                for t in &f.tail_targets {
                    let v = match t {
                        Tgt::Direct(t) => self.cfg_pops.get(t).copied().flatten(),
                        _ => None,
                    };
                    if !ps.contains(&v) {
                        ps.push(v);
                    }
                }
                if ps.len() == 1 && ps[0].is_some() {
                    self.cfg_pops.insert(va, ps[0]);
                    self.summ.insert(va, Summary { reads: 0, preserved: ALL, pops: ps[0], known: true });
                    changed = true;
                }
            }
        }
        let inset: HashSet<u32> = vas.iter().copied().collect();
        let mut callers: HashMap<u32, BTreeSet<u32>> = HashMap::new();
        for &va in vas {
            let f = self.func(va);
            if f.refused.is_none() {
                for t in f.calls.values().chain(f.tails.values()) {
                    if let Tgt::Direct(t) = t {
                        if inset.contains(t) {
                            callers.entry(*t).or_default().insert(va);
                        }
                    }
                }
            }
        }
        let order = self.callees_first(vas);
        let mut queued: HashSet<u32> = order.iter().copied().collect();
        let mut work: VecDeque<u32> = order.into_iter().collect();
        let mut budget = (rounds * vas.len().max(1)) as i64;
        while let Some(va) = work.pop_front() {
            budget -= 1;
            if budget < 0 {
                panic!("summaries didn't settle");
            }
            queued.remove(&va);
            let f = self.func(va);
            let s = if f.refused.is_some() || !self.summ[&va].known {
                UNKNOWN
            } else {
                match Analysis::new(self, f.clone(), None, true) {
                    Ok(a) => {
                        let s = Summary {
                            reads: a.live_in,
                            preserved: a.preserved,
                            pops: if a.esp_moved { None } else { self.cfg_pops[&va] },
                            known: true,
                        };
                        self.why.remove(&va);
                        s
                    }
                    Err(e) => {
                        self.why.insert(va, e.0);
                        if self.noreturn_funcs.contains(&va) {
                            Summary { reads: ALL, preserved: 0, pops: self.cfg_pops[&va], known: true }
                        } else {
                            UNKNOWN
                        }
                    }
                }
            };
            if s != self.summ[&va] {
                self.summ.insert(va, s);
                if let Some(cs) = callers.get(&va) {
                    for &c in cs {
                        if queued.insert(c) {
                            work.push_back(c);
                        }
                    }
                }
            }
        }
    }

    /// The functions in post-order of their direct calls.
    pub fn callees_first(&self, vas: &[u32]) -> Vec<u32> {
        let inset: HashSet<u32> = vas.iter().copied().collect();
        let mut seen = HashSet::new();
        let mut out = Vec::new();
        let mut roots: Vec<u32> = vas.to_vec();
        roots.sort();
        let targets = |va: u32| -> Vec<Tgt> {
            let f = self.func(va);
            if f.refused.is_some() {
                Vec::new()
            } else {
                f.calls.values().chain(f.tails.values()).copied().collect()
            }
        };
        for root in roots {
            if seen.contains(&root) {
                continue;
            }
            seen.insert(root);
            let mut stack: Vec<(u32, Vec<Tgt>, usize)> = vec![(root, targets(root), 0)];
            while let Some(top) = stack.last_mut() {
                let mut nxt = None;
                while top.2 < top.1.len() {
                    let t = top.1[top.2];
                    top.2 += 1;
                    if let Tgt::Direct(t) = t {
                        if inset.contains(&t) && !seen.contains(&t) {
                            nxt = Some(t);
                            break;
                        }
                    }
                }
                match nxt {
                    None => {
                        out.push(top.0);
                        stack.pop();
                    }
                    Some(t) => {
                        seen.insert(t);
                        stack.push((t, targets(t), 0));
                    }
                }
            }
        }
        out
    }
}

/// AOT_HOT's file: a hexadecimal address a line, `#` comments and blank lines aside.
fn hot_list() -> Option<HashSet<u32>> {
    let path = std::env::var("AOT_HOT").ok().filter(|s| !s.is_empty())?;
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("AOT_HOT={}: {}", path, e));
    let mut set = HashSet::new();
    for line in text.lines() {
        let s = line.split('#').next().unwrap_or("").trim();
        if s.is_empty() {
            continue;
        }
        let v = u32::from_str_radix(s.trim_start_matches("0x"), 16).unwrap_or_else(|_| panic!("AOT_HOT={}: not an address: {}", path, line));
        set.insert(v);
    }
    eprintln!("aot: AOT_HOT: {} functions on the list; the others are AOT_FNC", set.len());
    Some(set)
}
