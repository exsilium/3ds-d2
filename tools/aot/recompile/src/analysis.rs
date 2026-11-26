//! One function's stack offsets, preserved registers and register liveness
//! (recompile.py's Analysis).

use crate::cs::{reg_full, Insn, Op, OpKind};
use crate::func::{Func, Tgt};
use crate::omap::OMap;
use crate::program::{Program, Summary, FRAME_NAMES};
use crate::x86::*;
use crate::Refused;
use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::rc::Rc;

/// Abstract values: a register's value at entry, the entry esp + k, a
/// constant, a library call's leftovers.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Val {
    In(u8),
    Sp(i64),
    K(i64),
    Clob,
}
pub type V = Option<Val>;
pub type RegSt = [V; 8];
pub type Slots = HashMap<i64, V>;

fn sp_of(v: V) -> Option<i64> {
    match v {
        Some(Val::Sp(k)) => Some(k),
        _ => None,
    }
}

pub struct Analysis<'p, 'a> {
    p: &'p Program<'a>,
    pub f: Rc<Func>,
    pub sp_at: HashMap<u32, Option<i64>>,
    pub save_push: OMap<u32, u8>,
    pub save_slot: HashMap<u32, i64>,
    prologue: bool,
    pub ret_states: Vec<(u32, RegSt)>,
    pub esp_lost: HashMap<u32, String>,
    pub esp_conflict: HashSet<u32>,
    pub esp_ok: bool,
    pub esp_moved: bool,
    pub garbage: Regs,
    pub preserved: Regs,
    pub outs: Regs,
    pub skip_saves: Regs,
    pub live_out: HashMap<u32, Regs>,
    pub live_in: Regs,
}

impl<'p, 'a> Analysis<'p, 'a> {
    pub fn new(p: &'p Program<'a>, f: Rc<Func>, outs: Option<Regs>, save_rule: bool) -> Result<Analysis<'p, 'a>, Refused> {
        // A loose function (Program::loose) stores every save too: its stack
        // offsets aren't known, so no pop is known to take one back.
        let save_rule = save_rule && !p.may_exit.contains(&f.entry) && !p.is_loose(f.entry);
        let mut a = Analysis {
            p,
            f,
            sp_at: HashMap::new(),
            save_push: OMap::new(),
            save_slot: HashMap::new(),
            prologue: true,
            ret_states: Vec::new(),
            esp_lost: HashMap::new(),
            esp_conflict: HashSet::new(),
            esp_ok: false,
            esp_moved: false,
            garbage: 0,
            preserved: 0,
            outs: 0,
            skip_saves: 0,
            live_out: HashMap::new(),
            live_in: 0,
        };
        a.stack()?;
        a.liveness(outs, save_rule);
        Ok(a)
    }

    fn stack(&mut self) -> Result<(), Refused> {
        let f = self.f.clone();
        let p = self.p;
        let mut init: RegSt = [None; 8];
        for r in 0..8u8 {
            init[r as usize] = Some(Val::In(r));
        }
        init[ESP as usize] = Some(Val::Sp(0));
        let mut states: HashMap<u32, (RegSt, Slots)> = HashMap::new();
        states.insert(f.entry, (init, HashMap::new()));
        let mut work = vec![f.entry];
        let mut tail_states: Vec<(u32, RegSt)> = Vec::new();
        let mut visited: HashMap<u32, u32> = HashMap::new();
        while let Some(b) = work.pop() {
            let (r0, s0) = states[&b].clone();
            let (mut regs, mut slots) = (r0, s0);
            let mut va = b;
            loop {
                let i = f.insns[&va].clone();
                let n = visited.entry(va).or_insert(0);
                *n += 1;
                if *n > 200 {
                    return Err(Refused(format!("stack analysis doesn't settle at {:08x}", va)));
                }
                self.sp_at.insert(va, sp_of(regs[ESP as usize]));
                if f.calls.contains_key(&va) || b != f.entry {
                    self.prologue = false;
                }
                let before = regs[ESP as usize];
                self.step(&i, &mut regs, &mut slots);
                if before.is_some() && regs[ESP as usize].is_none() && !self.esp_lost.contains_key(&va) {
                    let c = self.esp_cause(&i);
                    self.esp_lost.insert(va, c);
                }
                if f.rets.contains_key(&va) {
                    self.ret_states.push((va, regs));
                }
                // A tail call to a function that never returns (__report_gsfailure's, from
                // __security_check_cookie) says nothing of what this one keeps (issue #138).
                // (A loose function's tail jumps are no exits: all of them.)
                if f.tails.contains_key(&va)
                    && (!f.exits.contains(&va) || f.callouts.contains_key(&va))
                    && !f.tails[&va].direct().is_some_and(|d| p.noreturn_funcs.contains(&d))
                {
                    tail_states.push((va, regs));
                }
                let succ = &f.succ[&va];
                let nxt = va.wrapping_add(i.size);
                if succ.len() == 1 && succ[0] == nxt && !f.leaders.contains(&nxt) {
                    va = nxt;
                    continue;
                }
                for &t in succ {
                    match states.get(&t) {
                        None => {
                            states.insert(t, (regs, slots.clone()));
                            work.push(t);
                        }
                        Some((r0, s0)) => {
                            let mut r1: RegSt = [None; 8];
                            for k in 0..8 {
                                r1[k] = if r0[k] == regs[k] { regs[k] } else { None };
                            }
                            for k in [ECX as usize, EDX as usize] {
                                if r0[k] == Some(Val::Clob) || regs[k] == Some(Val::Clob) {
                                    r1[k] = Some(Val::Clob);
                                }
                            }
                            let e = ESP as usize;
                            if r0[e].is_some() && regs[e].is_some() && r0[e] != regs[e] {
                                self.esp_conflict.insert(t);
                            }
                            if (r0[e].is_none() || regs[e].is_none()) && !self.esp_conflict.contains(&t) {
                                r1[e] = r0[e].or(regs[e]);
                            }
                            let mut s1: Slots =
                                slots.iter().filter(|(k, v)| s0.get(k) == Some(v)).map(|(k, v)| (*k, *v)).collect();
                            if r1 != *r0 || s1 != *s0 {
                                if r1[e].is_none() {
                                    s1 = HashMap::new();
                                }
                                states.insert(t, (r1, s1));
                                work.push(t);
                            }
                        }
                    }
                }
                break;
            }
        }
        let mut pre: Regs = ALL;
        self.esp_ok = self.ret_states.iter().all(|(_, r)| r[ESP as usize] == Some(Val::Sp(0)));
        let mut garbage = 0u8;
        if !self.ret_states.is_empty() || !tail_states.is_empty() {
            for r in [ECX, EDX] {
                let at_ret = self.ret_states.iter().any(|(_, regs)| regs[r as usize] == Some(Val::Clob));
                let at_tail = tail_states.iter().any(|(va, regs)| {
                    let t = f.tails[va];
                    let g = match t {
                        Tgt::Direct(t) => p.garbage.get(&t).copied().unwrap_or(0),
                        _ => 0,
                    };
                    has(g, r) || (regs[r as usize] == Some(Val::Clob) && has(p.callee_summary(t).preserved, r))
                });
                if at_ret || at_tail {
                    garbage |= bit(r);
                }
            }
        }
        self.garbage = garbage;
        self.esp_moved = self.ret_states.iter().any(|(_, r)| r[ESP as usize].is_some() && r[ESP as usize] != Some(Val::Sp(0)));
        for (_, regs) in &self.ret_states {
            for r in 0..8u8 {
                if r != ESP && regs[r as usize] != Some(Val::In(r)) {
                    pre &= !bit(r);
                }
            }
        }
        let tail_keeps: HashMap<u32, Regs> = tail_states.iter().map(|(va, _)| (*va, p.callee_summary(f.tails[va]).preserved)).collect();
        for (va, regs) in &tail_states {
            for r in 0..8u8 {
                if r != ESP && (regs[r as usize] != Some(Val::In(r)) || !has(tail_keeps[va], r)) {
                    pre &= !bit(r);
                }
            }
        }
        if self.ret_states.is_empty() && tail_states.is_empty() {
            pre = ALL;
        }
        for r in [EBX, EBP, ESI, EDI] {
            let ok = |v: V| v == Some(Val::In(r)) || v.is_none();
            if !self.esp_moved
                && self.ret_states.iter().all(|(_, regs)| ok(regs[r as usize]))
                && tail_states.iter().all(|(va, regs)| ok(regs[r as usize]) && has(tail_keeps[va], r))
            {
                pre |= bit(r);
            }
        }
        self.preserved = pre;
        Ok(())
    }

    fn val(&self, op: &Op, regs: &RegSt, slots: &Slots) -> V {
        match op.kind {
            OpKind::Imm(v) => Some(Val::K(v & 0xffff_ffff)),
            OpKind::Reg(r) => match reg_full(r) {
                Some(s) if s.1 == 32 => regs[s.0 as usize],
                _ => None,
            },
            OpKind::Mem(_) if op.size == 4 => match self.addr(op, regs) {
                Some(a) => slots.get(&a).copied().flatten(),
                None => None,
            },
            _ => None,
        }
    }

    fn addr(&self, op: &Op, regs: &RegSt) -> Option<i64> {
        let m = op.mem();
        if m.segment != 0 || m.index != 0 || m.base == 0 {
            return None;
        }
        let s = reg_full(m.base)?;
        sp_of(regs[s.0 as usize]).map(|k| k + m.disp)
    }

    fn kill_mem(&self, op: &Op, regs: &RegSt, slots: &mut Slots, size: u32) {
        if let Some(a) = self.addr(op, regs) {
            let n = if size == 0 { op.size as i64 } else { size as i64 };
            for k in (a - 3)..(a + n) {
                slots.remove(&k);
            }
        }
    }

    fn step(&mut self, i: &Insn, regs: &mut RegSt, slots: &mut Slots) {
        let m = i.mnemonic.as_str();
        let ops = &i.ops;
        let p = self.p;
        let f = self.f.clone();
        let Some(sp) = sp_of(regs[ESP as usize]) else {
            self.step_nosp(i, regs, slots);
            return;
        };
        let e = ESP as usize;
        if m == "pushfd" {
            self.prologue = false;
            slots.insert(sp - 4, None);
            regs[e] = Some(Val::Sp(sp - 4));
            return;
        }
        if m == "popfd" {
            regs[e] = Some(Val::Sp(sp + 4));
            return;
        }
        if m == "push" {
            let v = self.val(&ops[0], regs, slots);
            let r = if ops[0].is_reg() { reg_full(ops[0].reg()).map(|s| s.0) } else { None };
            match r {
                Some(r) if self.prologue && [EBX, EBP, ESI, EDI].contains(&r) && v == Some(Val::In(r)) => {
                    self.save_push.insert(i.address, r);
                    self.save_slot.insert(i.address, sp - 4);
                }
                _ => self.prologue = false,
            }
            slots.insert(sp - 4, v);
            regs[e] = Some(Val::Sp(sp - 4));
            return;
        }
        if m == "pop" {
            let v = slots.get(&sp).copied().flatten();
            regs[e] = Some(Val::Sp(sp + 4));
            if ops[0].is_reg() {
                if let Some(s) = reg_full(ops[0].reg()) {
                    regs[s.0 as usize] = v;
                }
            } else {
                self.kill_mem(&ops[0], regs, slots, 0);
            }
            return;
        }
        if m == "leave" {
            let Some(fp) = sp_of(regs[EBP as usize]) else {
                regs[EBP as usize] = None;
                regs[e] = None;
                return;
            };
            regs[EBP as usize] = slots.get(&fp).copied().flatten();
            regs[e] = Some(Val::Sp(fp + 4));
            return;
        }
        if m == "call" {
            let t = f.calls[&i.address];
            let ra = Some(Val::K((i.address + i.size) as i64));
            if f.noreturn.contains(&i.address) {
                return;
            }
            let intr = t.direct().and_then(|d| p.intrinsics.get(&d).copied());
            if let Some(name) = intr {
                if FRAME_NAMES.contains(&name) {
                    self.frame_helper(name, i, regs, slots);
                    return;
                }
                if name == "alloca_probe_16" {
                    // The size rounded by esp's low bits: not known here; the
                    // frame's own register finds the stack again.
                    regs[e] = None;
                    regs[EAX as usize] = None;
                    return;
                }
                if name == "alloca_probe" {
                    let Some(Val::K(n)) = regs[EAX as usize] else {
                        regs[e] = None;
                        regs[EAX as usize] = None;
                        return;
                    };
                    slots.insert(sp - 4, ra);
                    regs[e] = Some(Val::Sp(sp - n));
                    regs[EAX as usize] = None;
                    slots.retain(|k, _| *k >= sp);
                    slots.insert(sp - n, ra);
                    return;
                }
            }
            let mut s = p.callee_summary(t);
            if let Some(&c) = f.callouts.get(&i.address) {
                s = Summary { reads: s.reads, preserved: s.preserved, pops: Some(c), known: true };
            }
            slots.insert(sp - 4, ra);
            for r in 0..8u8 {
                if r != ESP && !has(s.preserved, r) {
                    regs[r as usize] = None;
                }
            }
            let mm = matches!(intr, Some("memset") | Some("memcpy") | Some("strrchr"));
            if mm || t.direct().is_some_and(|d| p.native_direct(d)) {
                regs[ECX as usize] = Some(Val::Clob);
                regs[EDX as usize] = Some(Val::Clob);
            }
            if intr.is_some_and(|n| crate::program::FMATH.contains(&n)) {
                regs[EAX as usize] = Some(Val::Clob);
                regs[ECX as usize] = Some(Val::Clob);
                regs[EDX as usize] = Some(Val::Clob);
            }
            if let Some(d) = t.direct() {
                if let Some(&g) = p.garbage.get(&d) {
                    for r in regs_of(g) {
                        regs[r as usize] = Some(Val::Clob);
                    }
                }
            }
            let Some(pops) = s.pops else {
                regs[e] = None;
                slots.clear();
                return;
            };
            regs[e] = Some(Val::Sp(sp + pops));
            slots.retain(|k, _| *k >= sp + pops);
            return;
        }
        if m == "ret" || f.tails.contains_key(&i.address) {
            return;
        }
        if m == "mov" && ops[0].is_reg() && reg_full(ops[0].reg()).map(|s| s.1) == Some(32) {
            let r = reg_full(ops[0].reg()).unwrap().0;
            regs[r as usize] = self.val(&ops[1], regs, slots);
            return;
        }
        if m == "mov" && ops[0].is_mem() {
            if let Some(a) = self.addr(&ops[0], regs) {
                if ops[0].size == 4 {
                    let v = self.val(&ops[1], regs, slots);
                    slots.insert(a, v);
                } else {
                    for k in (a - 3)..(a + ops[0].size as i64) {
                        slots.remove(&k);
                    }
                }
            }
            return;
        }
        if m == "lea" && reg_full(ops[0].reg()).map(|s| s.1) == Some(32) {
            let r = reg_full(ops[0].reg()).unwrap().0;
            let mm = ops[1].mem();
            let mut v = None;
            if mm.index == 0 && mm.base != 0 && mm.segment == 0 {
                let b = regs[reg_full(mm.base).expect("base").0 as usize];
                if mm.disp == 0 {
                    v = b;
                } else if let Some(Val::Sp(k)) = b {
                    v = Some(Val::Sp(k + mm.disp));
                } else if let Some(Val::K(c)) = b {
                    v = Some(Val::K((c + mm.disp) & 0xffff_ffff));
                }
            }
            regs[r as usize] = v;
            return;
        }
        if (m == "add" || m == "sub") && ops[0].is_reg() && ops[1].is_imm() && reg_full(ops[0].reg()).map(|s| s.1) == Some(32) {
            let r = reg_full(ops[0].reg()).unwrap().0;
            let v = regs[r as usize];
            let d = if m == "add" { ops[1].imm() } else { -ops[1].imm() };
            regs[r as usize] = match v {
                Some(Val::Sp(k)) => Some(Val::Sp(k + d)),
                Some(Val::K(c)) => Some(Val::K((c + d) & 0xffff_ffff)),
                _ => None,
            };
            if r == ESP {
                if let Some(Val::Sp(k)) = regs[e] {
                    slots.retain(|x, _| *x >= k);
                } else if let Some(Val::K(k)) = regs[e] {
                    slots.retain(|x, _| *x >= k);
                }
            }
            return;
        }
        let (_, w) = regs_rw(i);
        for r in regs_of(w) {
            if r == ESP && m != "push" && m != "pop" {
                if m.starts_with("rep") || ["stosd", "movsd", "stosb", "movsb", "stosw", "movsw"].contains(&m) {
                    continue;
                }
                slots.clear();
            }
            regs[r as usize] = None;
        }
        for (op, n) in mem_written(i) {
            self.kill_mem(&op, regs, slots, n);
        }
    }

    fn frame_helper(&mut self, name: &str, i: &Insn, regs: &mut RegSt, slots: &mut Slots) {
        let s = sp_of(regs[ESP as usize]).expect("esp known");
        let ra = Some(Val::K((i.address + i.size) as i64));
        if name.ends_with("prolog4") || name == "eh_prolog3_catch" {
            let seh = name == "seh_prolog4";
            let fp = if seh { s + 4 } else { s };
            let n = slots.get(&(if seh { fp } else { s })).copied().flatten();
            let saved = [regs[EBX as usize], regs[ESI as usize], regs[EDI as usize]];
            let old_ebp = regs[EBP as usize];
            slots.retain(|k, _| *k > fp);
            regs[EBP as usize] = Some(Val::Sp(fp));
            regs[EAX as usize] = Some(Val::Sp(s - 12));
            slots.insert(fp, old_ebp);
            let n = match n {
                Some(Val::K(n)) if n < 0x1000_0000 => n,
                _ => {
                    regs[ESP as usize] = None;
                    slots.clear();
                    return;
                }
            };
            let e = s - 12 - n;
            slots.insert(e - 4, saved[0]);
            slots.insert(e - 8, saved[1]);
            slots.insert(e - 12, saved[2]);
            regs[ESP as usize] = Some(Val::Sp(e - 16));
            return;
        }
        let fp = regs[EBP as usize];
        regs[ECX as usize] = ra;
        for (r, k) in [(EDI, 4), (ESI, 8), (EBX, 12)] {
            regs[r as usize] = slots.get(&(s + k)).copied().flatten();
        }
        let Some(fp) = sp_of(fp) else {
            regs[EBP as usize] = None;
            regs[ESP as usize] = None;
            slots.clear();
            return;
        };
        regs[EBP as usize] = slots.get(&fp).copied().flatten();
        regs[ESP as usize] = Some(Val::Sp(fp + 4));
        slots.retain(|k, _| *k >= fp + 4);
    }

    fn esp_cause(&self, i: &Insn) -> String {
        let m = i.mnemonic.as_str();
        let p = self.p;
        if m != "call" {
            if m == "leave" || (m == "mov" && i.ops[1].is_reg()) {
                return format!("{} from a register not known as a frame address", m);
            }
            return format!("{} of esp", m);
        }
        let t = self.f.calls[&i.address];
        let t = match t {
            Tgt::Indirect => return "an indirect call whose pops aren't known".to_string(),
            Tgt::Import(slot) => return format!("an import whose pops aren't known ({})", slot),
            Tgt::Direct(t) => t,
        };
        if let Some(n) = p.intrinsics.get(&t) {
            return format!("{} of a size not known", n);
        }
        let g = p.func(t);
        if g.refused.is_some() {
            return "a call to a function the decoder refuses".to_string();
        }
        let rets: HashSet<i64> = g.rets.values().copied().collect();
        if rets.len() > 1 {
            return "a call to a function whose rets pop different amounts".to_string();
        }
        if let Some(w) = p.why.get(&t) {
            return format!("a call to a function whose own analysis refuses: {}", w);
        }
        if let Some(s) = p.summ.get(&t) {
            if s.known && s.pops.is_none() && p.cfg_pops.get(&t).copied().flatten().is_some() {
                return "a call to a helper that moves its caller's frame".to_string();
            }
        }
        if g.rets.is_empty() {
            return "a call to a function with no ret".to_string();
        }
        "a call to a function whose pops aren't known".to_string()
    }

    /// Why esp isn't the entry's at a ret (issue #138).
    pub fn esp_why(&self) -> String {
        for (va, regs) in &self.ret_states {
            let v = regs[ESP as usize];
            if v.is_some() && v != Some(Val::Sp(0)) {
                let n = match v.unwrap() {
                    Val::Sp(k) | Val::K(k) => k,
                    Val::In(r) => r as i64,
                    Val::Clob => panic!("esp_why: a library call's leftovers in esp"),
                };
                return format!("esp at the ret at {:08x} is the entry's {}{}", va, if n >= 0 { "+" } else { "-" }, n.abs());
            }
        }
        if let Some(va) = self.esp_lost.keys().min() {
            return format!("{} at {:08x}", self.esp_lost[va], va);
        }
        if let Some(va) = self.esp_conflict.iter().min() {
            return format!("two paths bring different esps to {:08x}", va);
        }
        "no cause found".to_string()
    }

    fn step_nosp(&mut self, i: &Insn, regs: &mut RegSt, _slots: &mut Slots) {
        let m = i.mnemonic.as_str();
        let ops = &i.ops;
        let e = ESP as usize;
        let slots = _slots;
        if m == "leave"
            || (m == "mov"
                && ops[0].is_reg()
                && ops[1].is_reg()
                && reg_full(ops[0].reg()).map(|s| s.0) == Some(ESP)
                && reg_full(ops[1].reg()).map(|s| s.0) == Some(EBP))
        {
            let v = regs[EBP as usize];
            regs[e] = if matches!(v, Some(Val::Sp(_))) { v } else { None };
            if m == "leave" {
                if let Some(Val::Sp(k)) = regs[e] {
                    regs[EBP as usize] = slots.get(&k).copied().flatten();
                    regs[e] = Some(Val::Sp(k + 4));
                } else {
                    regs[EBP as usize] = None;
                }
            }
            return;
        }
        if m == "call" {
            if self.f.noreturn.contains(&i.address) {
                return;
            }
            let t = self.f.calls[&i.address];
            let intr = t.direct().and_then(|d| self.p.intrinsics.get(&d).copied());
            if matches!(intr, Some("seh_epilog4") | Some("eh_epilog3")) {
                let v = regs[EBP as usize];
                regs[ECX as usize] = Some(Val::K((i.address + i.size) as i64));
                regs[EBX as usize] = None;
                regs[ESI as usize] = None;
                regs[EDI as usize] = None;
                regs[EBP as usize] = None;
                regs[e] = sp_of(v).map(|k| Val::Sp(k + 4));
                return;
            }
            let s = self.p.callee_summary(t);
            for r in 0..8u8 {
                if !has(s.preserved, r) {
                    regs[r as usize] = None;
                }
            }
            return;
        }
        if m == "ret" || self.f.tails.contains_key(&i.address) {
            return;
        }
        if m == "pop" && ops[0].is_reg() {
            if let Some(s) = reg_full(ops[0].reg()) {
                regs[s.0 as usize] = None;
            }
            return;
        }
        let (_, w) = regs_rw(i);
        for r in regs_of(w) {
            regs[r as usize] = None;
        }
    }

    // ---- register liveness, backward over the CFG ----
    pub fn uses_defs(&self, va: u32, outs: Regs, skip_saves: Regs) -> (Parts, Parts) {
        let f = &self.f;
        let p = self.p;
        let i = &f.insns[&va];
        let m = i.mnemonic.as_str();
        if f.exits.contains(&va) || p.loose_jump(f, va) {
            return (p_of(ALL), p_of(ALL));
        }
        if f.noreturn.contains(&va) {
            return (p_of(bit(ESP)), p_of(ALL));
        }
        if let Some(&t) = f.calls.get(&va) {
            let s = p.callee_summary(t);
            if let Some(d) = t.direct() {
                if let Some(n) = p.natives.get(&d) {
                    if !n.handback {
                        return (p_of(set_of(&n.reads) | bit(ESP)), p_of(bit(EAX) | bit(ESP)));
                    }
                    return (p_of(set_of(&n.reads) | s.reads | bit(ESP)), p_of((ALL & !s.preserved) | bit(EAX) | bit(ESP)));
                }
            }
            return (p_of(s.reads | bit(ESP)), p_of((ALL & !s.preserved) | bit(ESP)));
        }
        if let Some(&t) = f.tails.get(&va) {
            // A loose function's tail jump hands every register to the
            // dispatcher, as its ret does.
            if p.is_loose(f.entry) {
                return (p_of(ALL), p_of(ALL));
            }
            let s = p.callee_summary(t);
            return (p_of(s.reads | (outs & s.preserved) | bit(ESP)), p_of(ALL));
        }
        if m == "ret" {
            return (p_of(outs | bit(ESP)), p_of(bit(ESP)));
        }
        let (mut r, w) = parts_rw(i);
        if skip_saves != 0 {
            if let Some(&sr) = self.save_push.get(&va) {
                if has(skip_saves, sr) {
                    r &= !p_of(bit(sr));
                }
            }
        }
        (r, w)
    }

    fn liveness(&mut self, outs: Option<Regs>, save_rule: bool) {
        let f = self.f.clone();
        let mut outs = outs.unwrap_or((ALL & !self.preserved) & !bit(ESP));
        if self.p.is_loose(f.entry) {
            outs = ALL & !bit(ESP);   // every register is the dispatcher's to pass on
        }
        self.outs = outs;
        let mut skip: Regs = 0;
        if save_rule {
            let mut popped: HashSet<(u8, i64)> = HashSet::new();
            for (va, i) in f.insns.iter() {
                if i.mnemonic == "pop" && i.ops[0].is_reg() {
                    if let Some(Some(sp)) = self.sp_at.get(va) {
                        popped.insert((reg_full(i.ops[0].reg()).expect("pop reg").0, *sp));
                    }
                }
            }
            for (va, &r) in self.save_push.iter() {
                if has(self.preserved, r) && popped.contains(&(r, self.save_slot[va])) {
                    skip |= bit(r);
                }
            }
            let mut sp2 = OMap::new();
            for (va, &r) in self.save_push.iter() {
                if has(skip, r) {
                    sp2.insert(*va, r);
                }
            }
            self.save_push = sp2;
        }
        let mut live_out = HashMap::new();
        for _ in 0..4 {
            live_out = self.solve(outs, skip);
            let mut bad: Regs = 0;
            for (va, i) in f.insns.iter() {
                if i.mnemonic == "pop" && i.ops[0].is_reg() {
                    let r = reg_full(i.ops[0].reg()).expect("pop reg").0;
                    if has(skip, r) && p_of(bit(r)) & live_out[va] != 0 {
                        bad |= bit(r);
                    }
                }
            }
            if bad == 0 {
                break;
            }
            skip &= !bad;
        }
        self.skip_saves = skip;
        self.live_out = live_out.iter().map(|(va, v)| (*va, r_of(*v))).collect();
        let (u, d) = self.uses_defs(f.entry, outs, skip);
        self.live_in = r_of((live_out[&f.entry] & !d) | u) & !bit(ESP);
    }

    fn solve(&self, outs: Regs, skip: Regs) -> HashMap<u32, Parts> {
        let f = &self.f;
        let mut live_out: HashMap<u32, Parts> = f.insns.keys().map(|va| (*va, 0)).collect();
        let mut preds: HashMap<u32, Vec<u32>> = HashMap::new();
        for (va, s) in f.succ.iter() {
            for t in s {
                preds.entry(*t).or_default().push(*va);
            }
        }
        let ud: HashMap<u32, (Parts, Parts)> = f.insns.keys().map(|va| (*va, self.uses_defs(*va, outs, skip))).collect();
        let mut order: Vec<u32> = f.insns.keys().copied().collect();
        order.sort_by(|a, b| b.cmp(a));
        let mut inq: HashSet<u32> = order.iter().copied().collect();
        let mut work: VecDeque<u32> = order.into_iter().collect();
        let mut live_in: BTreeMap<u32, Parts> = f.insns.keys().map(|va| (*va, 0)).collect();
        while let Some(va) = work.pop_front() {
            inq.remove(&va);
            let mut lo: Parts = 0;
            for t in &f.succ[&va] {
                lo |= live_in[t];
            }
            live_out.insert(va, lo);
            let (u, d) = ud[&va];
            let li = (lo & !d) | u;
            if li != live_in[&va] {
                live_in.insert(va, li);
                if let Some(ps) = preds.get(&va) {
                    for &q in ps {
                        if inq.insert(q) {
                            work.push_back(q);
                        }
                    }
                }
            }
        }
        live_out
    }
}
