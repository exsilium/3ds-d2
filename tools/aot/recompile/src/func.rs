//! One function's code: its instructions and basic blocks from its entry
//! (recompile.py's Func).

use crate::cs::{reg_full, Insn, OpKind};
use crate::image::Image;
use crate::omap::OMap;
use crate::x86::{mem_written, regs_rw};
use crate::Refused;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::rc::Rc;

/// A call's or tail jump's target: a function's entry, an import's IAT
/// slot, or through a pointer (None in recompile.py).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub enum Tgt {
    Direct(u32),
    Import(u32),
    Indirect,
}

impl Tgt {
    pub fn direct(self) -> Option<u32> {
        match self {
            Tgt::Direct(v) => Some(v),
            _ => None,
        }
    }
    /// None or an import (recompile.py's `t is None or isinstance(t, tuple)`).
    pub fn is_pointer(self) -> bool {
        !matches!(self, Tgt::Direct(_))
    }
}

/// A switch's jmp: the index register, the targets, and (kept as
/// recompile.py has them) the byte table, the default and the bound.
#[derive(Clone, Debug)]
#[allow(dead_code)]
pub struct Switch {
    pub idx: u8,
    pub targets: Vec<u32>,
    pub bytes_table: Option<u32>,
    pub default: u32,
    pub bound: i64,
}

pub const NORETURN_IMPORTS: [&str; 5] = ["ExitProcess", "ExitThread", "FatalAppExitA", "FatalExit", "TerminateThread"];

#[derive(Clone)]
pub struct Func {
    pub entry: u32,
    pub insns: OMap<u32, Rc<Insn>>,
    pub succ: OMap<u32, Vec<u32>>,
    pub calls: OMap<u32, Tgt>,
    pub tails: OMap<u32, Tgt>,
    pub switches: OMap<u32, Switch>,
    pub rets: OMap<u32, i64>,
    pub noreturn: HashSet<u32>,
    pub exits: BTreeSet<u32>,
    pub callouts: OMap<u32, i64>,
    /// Pops found for callouts whose count of pushes was a guess (Program::solve_pops).
    pub pops_fix: Vec<(u32, i64)>,
    pub plain: bool,
    /// ... its calls to imports too (the second fallback).
    pub plain_all: bool,
    /// Its exits that are direct calls or tail jumps to a loose function
    /// (Program::loose), each with what that function's rets pop where they
    /// tell: callouts to its address.
    pub loose_calls: HashMap<u32, Option<i64>>,
    pub data: HashSet<u32>,
    pub refused: Option<String>,
    pub ret_pops: BTreeSet<i64>,
    pub tail_targets: BTreeSet<Tgt>,
    pub leaders: HashSet<u32>,
    pub order: Vec<u32>,
    pub n_insns: usize,
    pub n_bytes: u64,
}

fn imm_addr(v: i64) -> u32 {
    v as u32
}

impl Func {
    pub fn new(img: &Image, entry: u32, starts: &HashSet<u32>) -> Func {
        let mut f = Func {
            entry,
            insns: OMap::new(),
            succ: OMap::new(),
            calls: OMap::new(),
            tails: OMap::new(),
            switches: OMap::new(),
            rets: OMap::new(),
            noreturn: HashSet::new(),
            exits: BTreeSet::new(),
            callouts: OMap::new(),
            plain: false,
            plain_all: false,
            loose_calls: HashMap::new(),
            pops_fix: Vec::new(),
            data: HashSet::new(),
            refused: None,
            ret_pops: BTreeSet::new(),
            tail_targets: BTreeSet::new(),
            leaders: HashSet::new(),
            order: Vec::new(),
            n_insns: 0,
            n_bytes: 0,
        };
        if let Err(e) = f.walk(img, starts) {
            f.refused = Some(e.0);
        }
        f.ret_pops = f.rets.values().copied().collect();
        f.tail_targets = f.tails.values().copied().collect();
        f
    }

    fn walk(&mut self, img: &Image, starts: &HashSet<u32>) -> Result<(), Refused> {
        let mut work = vec![self.entry];
        let mut again: HashSet<u32> = HashSet::new();
        while let Some(mut va) = work.pop() {
            while !self.insns.contains_key(&va) {
                let i = img.insn(va)?;
                self.insns.insert(va, i.clone());
                let m = i.mnemonic.as_str();
                let nxt = va.wrapping_add(i.size);
                let ops = &i.ops;
                if m == "ret" {
                    self.rets.insert(va, if ops.is_empty() { 0 } else { ops[0].imm() });
                    self.succ.insert(va, vec![]);
                    break;
                }
                if m == "int3" || m == "hlt" || m == "ud2" {
                    self.succ.insert(va, vec![]);
                    break;
                }
                if m == "jmp" {
                    if ops[0].is_imm() {
                        let t = imm_addr(ops[0].imm());
                        if img.native_slots.contains(&t) {
                            // A native's site (issue #138): a tail call to it, as through an import.
                            self.tails.insert(va, Tgt::Import(t));
                            self.succ.insert(va, vec![]);
                        } else if t != self.entry && starts.contains(&t) {
                            self.tails.insert(va, Tgt::Direct(t));
                            self.succ.insert(va, vec![]);
                        } else {
                            self.succ.insert(va, vec![t]);
                            work.push(t);
                        }
                        break;
                    }
                    let sw = match self.switch(img, va, &i) {
                        // Reached before its guard was (issue #138: 57edd0's jz
                        // to the table's movzx at 57edfa, past the cmp and ja at
                        // 57edf5): once more, after the rest of the work.
                        Err(_) if !work.is_empty() && again.insert(va) => {
                            self.insns.retain(|k| *k != va);
                            work.insert(0, va);
                            break;
                        }
                        r => r?,
                    };
                    match sw {
                        None => {
                            // Not through a table: a tail call through a pointer (issue #138).
                            let op = ops[0];
                            let mm = op.mem();
                            let imp = op.is_mem()
                                && mm.base == 0
                                && mm.index == 0
                                && (0..=u32::MAX as i64).contains(&mm.disp)
                                && img.imports.contains_key(&(mm.disp as u32));
                            self.tails.insert(va, if imp { Tgt::Import(mm.disp as u32) } else { Tgt::Indirect });
                            self.succ.insert(va, vec![]);
                        }
                        Some(sw) => {
                            // Not the guard's default: the ja before goes there, and the
                            // index here is in the table (issue #138: the jmp's pushes
                            // reached a default that is the ret, two esps there).
                            let mut s: Vec<u32> = sw.targets.clone();
                            s.sort();
                            s.dedup();
                            work.extend(s.iter().copied());
                            self.succ.insert(va, s);
                            self.switches.insert(va, sw);
                        }
                    }
                    break;
                }
                if m.starts_with('j') || m.starts_with("loop") {
                    if !ops[0].is_imm() {
                        return Err(Refused(format!("indirect conditional jump at {:08x}", va)));
                    }
                    let t = imm_addr(ops[0].imm());
                    self.succ.insert(va, vec![t, nxt]);
                    work.push(t);
                    va = nxt;
                    continue;
                }
                if m == "call" {
                    let op = ops[0];
                    let mm = op.mem();
                    let t = if op.is_imm() && img.native_slots.contains(&imm_addr(op.imm())) {
                        Tgt::Import(imm_addr(op.imm()))   // a native's site (issue #138): its call as an import's
                    } else if op.is_imm() {
                        Tgt::Direct(imm_addr(op.imm()))
                    } else if op.is_mem()
                        && mm.base == 0
                        && mm.index == 0
                        && (0..=u32::MAX as i64).contains(&mm.disp)
                        && img.imports.contains_key(&(mm.disp as u32))
                    {
                        Tgt::Import(mm.disp as u32)
                    } else {
                        Tgt::Indirect
                    };
                    self.calls.insert(va, t);
                    // exit and its like end the code here: what follows may
                    // be a switch's table (issue #138: 4f5570's error path,
                    // call exit; mov edi, edi; the table, read on as code).
                    if let Tgt::Direct(d) = t {
                        if crate::program::NORETURN_FUNCS.contains(&d) {
                            self.noreturn.insert(va);
                            self.succ.insert(va, vec![]);
                            break;
                        }
                    }
                    if let Tgt::Import(slot) = t {
                        if NORETURN_IMPORTS.contains(&img.imports[&slot].1.as_str()) {
                            self.noreturn.insert(va);
                            self.succ.insert(va, vec![]);
                            break;
                        }
                    }
                    let dead = match img.insn(nxt) {
                        Ok(n) => n.mnemonic == "int3" || (starts.contains(&nxt) && nxt != self.entry),
                        Err(_) => true,
                    };
                    if dead {
                        self.noreturn.insert(va);
                        self.succ.insert(va, vec![]);
                        break;
                    }
                    self.succ.insert(va, vec![nxt]);
                    va = nxt;
                    continue;
                }
                self.succ.insert(va, vec![nxt]);
                va = nxt;
            }
        }
        for va in self.insns.keys() {
            for t in &self.succ[va] {
                if !self.insns.contains_key(t) {
                    return Err(Refused(format!("branch into the middle of an instruction at {:08x}", t)));
                }
            }
        }
        let mut data: Vec<u32> = self.data.iter().copied().collect();
        data.sort();
        for d in data {
            if self.insns.contains_key(&d) {
                return Err(Refused(format!("a switch table overlaps code at {:08x}", d)));
            }
        }
        self.blocks();
        Ok(())
    }

    /// Calls to functions that never return end their path.
    pub fn prune(&mut self, noreturn_funcs: &HashSet<u32>) -> bool {
        let hit: Vec<u32> = self
            .calls
            .iter()
            .filter(|(va, t)| t.direct().is_some_and(|d| noreturn_funcs.contains(&d)) && !self.noreturn.contains(va))
            .map(|(va, _)| *va)
            .collect();
        if hit.is_empty() {
            return false;
        }
        self.cut(&hit);
        true
    }

    /// The exits Box86 runs on from (issue #115); indirect and import calls
    /// callouts, and tail calls through a pointer where the pops are known.
    ///
    /// level 0: all of them callouts. 1 (a function whose callouts' pops
    /// threw its stack off): only its calls to imports whose pops are known,
    /// which are no guess (issue #138: HeapAlloc and GetLastError were plain
    /// exits in such functions, 3,900 a script). 2: none.
    ///
    /// loose: the functions compiled loose (Program::loose_pops). A direct
    /// call to one is a callout to its address, by its rets' pops or the
    /// site's count; a tail jump to one, as one through a pointer.
    pub fn set_exits(
        &mut self,
        sites: &BTreeSet<u32>,
        level: u8,
        tail_pops: Option<i64>,
        img: &Image,
        enabled: bool,
        loose: &HashMap<u32, Option<i64>>,
    ) {
        self.exits = sites.clone();
        self.plain = level > 0;
        self.plain_all = level > 1;
        let callouts = level == 0;
        let to_loose = |t: &Tgt| t.direct().and_then(|d| loose.get(&d).copied());
        if enabled && level < 2 {
            let mut co = OMap::new();
            for &va in sites {
                if let Some(t) = self.calls.get(&va) {
                    if let (Some(p), false) = (to_loose(t), self.noreturn.contains(&va)) {
                        co.insert(va, p.unwrap_or_else(|| self.callout_pops(va, img)));
                        continue;
                    }
                    let known = matches!(t, Tgt::Import(slot) if img.import_pops.get(slot).copied().flatten().is_some())
                        || self.reg_import_pops(&self.insns[&va], img).is_some();
                    if !self.noreturn.contains(&va) && t.is_pointer() && (callouts || known) {
                        co.insert(va, self.callout_pops(va, img));
                    }
                }
            }
            for &(va, p) in &self.pops_fix {
                if co.contains_key(&va) {
                    co.insert(va, p);
                }
            }
            self.callouts = co;
        }
        if enabled && callouts {
            let pops = if self.ret_pops.len() == 1 {
                self.ret_pops.iter().next().copied()
            } else if self.ret_pops.is_empty() {
                tail_pops
            } else {
                None
            };
            if let Some(p) = pops {
                for &va in sites {
                    if let Some(t) = self.tails.get(&va) {
                        if t.is_pointer() || to_loose(t).is_some() {
                            self.callouts.insert(va, p);
                        }
                    }
                }
            }
        }
        let hit: Vec<u32> = sites
            .iter()
            .copied()
            .filter(|va| self.calls.contains_key(va) && !self.noreturn.contains(va) && !self.callouts.contains_key(va))
            .collect();
        self.cut(&hit);
        self.loose_calls = self.loose_calls_for(loose);
    }

    /// loose_calls, were `loose` the functions compiled loose.
    pub fn loose_calls_for(&self, loose: &HashMap<u32, Option<i64>>) -> HashMap<u32, Option<i64>> {
        self.calls
            .iter()
            .chain(self.tails.iter())
            .filter(|(va, _)| self.exits.contains(va))
            .filter_map(|(va, t)| t.direct().and_then(|d| loose.get(&d)).map(|p| (*va, *p)))
            .collect()
    }

    /// What a callout's callee is taken to pop: an import's, none where the
    /// caller cleans up (add esp after), else the pushes just before it.
    pub fn callout_pops(&self, va: u32, img: &Image) -> i64 {
        if let Some(Tgt::Import(slot)) = self.calls.get(&va) {
            if let Some(Some(p)) = img.import_pops.get(slot) {
                return *p;
            }
        }
        let i = &self.insns[&va];
        if let Some(p) = self.reg_import_pops(i, img) {
            return p;
        }
        if let Some(n) = self.insns.get(&va.wrapping_add(i.size)) {
            if n.mnemonic == "add" && n.op_str.starts_with("esp, ") {
                return 0;
            }
            // ... or with pop ecx, the compiler's way to drop one or two
            // arguments (issue #138: 688fbb's pop ecx; pop ecx at 689122).
            if n.mnemonic == "pop" && n.op_str == "ecx" {
                return 0;
            }
        }
        let mut ends = std::collections::HashMap::new();
        for (k, j) in self.insns.iter() {
            ends.insert(k.wrapping_add(j.size), *k);
        }
        let (mut n, mut cur) = (0i64, va);
        let mut pushed: Vec<(u32, &str)> = Vec::new();   // the pushes walked, the last first
        // Into a block only from the conditional jump before it: the walk goes
        // on through the jump (issue #138: 66f3b0's first argument pushed
        // before the jnz at 66f4a8, its call at 66f4b2 counted 4 for 8).
        let falls_in = |cur: u32| {
            ends.get(&cur).is_some_and(|k| {
                let m = self.insns[k].mnemonic.as_str();
                m.starts_with('j')
                    && m != "jmp"
                    && !self.succ.iter().any(|(src, ts)| src != k && ts.contains(&cur))
                    && cur != self.entry
            })
        };
        while !self.leaders.contains(&cur) || falls_in(cur) {
            let Some(&k) = ends.get(&cur) else { break };
            let j = &self.insns[&k];
            if j.mnemonic == "push" {
                n += 1;
                pushed.push((k, j.op_str.as_str()));
            } else if matches!(j.mnemonic.as_str(), "call" | "pop" | "ret" | "leave" | "pushal" | "popal")
                || j.op_str.split(',').next().unwrap_or("").trim() == "esp"
                // A frame's mov ebp, esp (issue #138: renderer_line's push ebp
                // counted as an argument, 20 for 16, so its calls were exits).
                || j.op_str.split(',').nth(1).is_some_and(|o| o.trim() == "esp")
            {
                break;
            }
            cur = k;
        }
        // A pop just after the call takes a dword the callee left: where the
        // pops match the first pushes walked, register for register, and the
        // register is written between its push and the call, those were saves
        // of a register reused, not arguments (issue #138: 4f6300's push edi,
        // mov edi before its four arguments, popped after the call through
        // the renderer's table, counted 20 for 16, so its call was a plain
        // exit; 52e590's push edi is its argument, the pop after the call its
        // prologue's save).
        let writes = |r: &str, from: u32| {
            let mut a = from.wrapping_add(self.insns[&from].size);
            while a != va {
                let Some(j) = self.insns.get(&a) else { return false };
                if !matches!(j.mnemonic.as_str(), "push" | "cmp" | "test")
                    && j.op_str.contains(',')
                    && j.op_str.split(',').next().unwrap_or("").trim() == r
                {
                    return true;
                }
                a = a.wrapping_add(j.size);
            }
            false
        };
        let mut at = va.wrapping_add(i.size);
        let mut popped = 0i64;
        for &(pa, p) in pushed.iter().rev() {
            let Some(j) = self.insns.get(&at) else { break };
            if j.mnemonic != "pop"
                || j.op_str != p
                || (at != va.wrapping_add(i.size) && self.leaders.contains(&at))
                || !writes(p, pa)
            {
                break;
            }
            popped += 1;
            at = at.wrapping_add(j.size);
        }
        // The first pushes walked of ebx, esi, edi or ebp as the caller left
        // them (no instruction names the register on some path from the entry
        // to the push) are the prologue's saves: a function doesn't pass its
        // caller's register (issue #138: 410a80's push esi, push edi before
        // call edi, GetTickCount, counted 8 for 0; 476bc0's before its call
        // to the thunk 4f65c0 made the thunk pop 8).
        let mut kept = 0i64;
        for &(pa, p) in pushed.iter().rev() {
            if !self.as_entered(pa, p) {
                break;
            }
            kept += 1;
        }
        4 * (n - popped.max(kept))
    }

    /// The callouts whose pops are a count of pushes, not an import's.
    pub fn guessed_callouts(&self, img: &Image) -> Vec<(u32, i64)> {
        let mut out = Vec::new();
        for (va, p) in self.callouts.iter() {
            let Some(t) = self.calls.get(va) else { continue };
            let known = matches!(t, Tgt::Import(slot) if img.import_pops.get(slot).copied().flatten().is_some())
                || self.reg_import_pops(&self.insns[va], img).is_some()
                || self.loose_calls.get(va).is_some_and(|p| p.is_some());
            if !known {
                out.push((*va, *p));
            }
        }
        out
    }

    /// A call through a register that holds an import: every instruction of
    /// the function that writes the register, but a pop (the epilogue's),
    /// loads it from an import's slot, all with the same known pops (issue
    /// #138: 40b600's mov esi, [HeapAlloc]; call esi, its pushes counted, was
    /// a guess that threw the stack off, 3,211 plain exits a script).
    fn reg_import_pops(&self, i: &Insn, img: &Image) -> Option<i64> {
        let op = i.ops.first()?;
        if !op.is_reg() || op.size != 4 {
            return None;
        }
        let r = reg_full(op.reg())?.0;
        let slot_pops = |j: &Insn| -> Option<i64> {
            if j.mnemonic != "mov" || j.ops.len() != 2 || !j.ops[0].is_reg() || !j.ops[1].is_mem() {
                return None;
            }
            let m = j.ops[1].mem();
            if m.base != 0 || m.index != 0 || !(0..=u32::MAX as i64).contains(&m.disp) {
                return None;
            }
            img.import_pops.get(&(m.disp as u32)).copied().flatten()
        };
        // The write of the register nearest before the call, in a straight
        // line no branch comes into (the C runtime's 688b46 loads one
        // register from several imports' slots in turn, GetProcAddress among
        // them): that import's.
        let mut ends = std::collections::HashMap::new();
        for (k, j) in self.insns.iter() {
            ends.insert(k.wrapping_add(j.size), *k);
        }
        let mut cur = i.address;
        while !self.leaders.contains(&cur) {
            let Some(&k) = ends.get(&cur) else { break };
            let j = &self.insns[&k];
            if j.mnemonic == "call" && ![3u8, 5, 6, 7].contains(&r) {
                break;   // the register may be the callee's to change (688fbb's esi isn't: 688ff0)
            }
            if regs_rw(j).1 & (1 << r) != 0 {
                return slot_pops(j);
            }
            cur = k;
        }
        let mut pops: Option<i64> = None;
        for j in self.insns.values() {
            if regs_rw(j).1 & (1 << r) == 0 || j.mnemonic == "pop" {
                continue;
            }
            if j.mnemonic != "mov" || j.ops.len() != 2 || !j.ops[0].is_reg() || !j.ops[1].is_mem() {
                return None;
            }
            let m = j.ops[1].mem();
            if m.base != 0 || m.index != 0 || !(0..=u32::MAX as i64).contains(&m.disp) {
                return None;
            }
            let p = img.import_pops.get(&(m.disp as u32)).copied().flatten()?;
            if pops.is_some_and(|q| q != p) {
                return None;
            }
            pops = Some(p);
        }
        pops
    }

    /// Whether `r` (ebx, esi, edi or ebp) can be at `at` as it was at the
    /// entry: a path from the entry reaches `at` through no instruction that
    /// names it or a part of it, but a push, cmp or test.
    fn as_entered(&self, at: u32, r: &str) -> bool {
        let parts: &[&str] = match r {
            "ebx" => &["ebx", "bx", "bl", "bh"],
            "esi" => &["esi", "si", "sil"],
            "edi" => &["edi", "di", "dil"],
            "ebp" => &["ebp", "bp", "bpl"],
            _ => return false,
        };
        let names = |j: &Insn| {
            !matches!(j.mnemonic.as_str(), "push" | "cmp" | "test")
                && j.op_str
                    .split(|c: char| !c.is_ascii_alphanumeric())
                    .any(|w| parts.contains(&w))
        };
        let mut seen = HashSet::new();
        let mut work = vec![self.entry];
        while let Some(a) = work.pop() {
            if a == at {
                return true;
            }
            if !seen.insert(a) {
                continue;
            }
            let Some(j) = self.insns.get(&a) else { continue };
            if names(j) {
                continue;
            }
            if let Some(s) = self.succ.get(&a) {
                work.extend(s.iter().copied());
            }
        }
        false
    }

    pub fn cut(&mut self, hit: &[u32]) {
        for &va in hit {
            self.noreturn.insert(va);
            self.succ.insert(va, vec![]);
        }
        let mut seen = HashSet::new();
        let mut work = vec![self.entry];
        while let Some(va) = work.pop() {
            if !seen.insert(va) {
                continue;
            }
            if let Some(s) = self.succ.get(&va) {
                work.extend(s.iter().copied());
            }
        }
        self.insns.retain(|k| seen.contains(k));
        self.succ.retain(|k| seen.contains(k));
        self.calls.retain(|k| seen.contains(k));
        self.rets.retain(|k| seen.contains(k));
        self.switches.retain(|k| seen.contains(k));
        self.tails.retain(|k| seen.contains(k));
        self.noreturn.retain(|k| seen.contains(k));
        self.blocks();
    }

    fn blocks(&mut self) {
        let mut leaders = HashSet::new();
        leaders.insert(self.entry);
        for (va, s) in self.succ.iter() {
            let i = &self.insns[va];
            let nxt = va.wrapping_add(i.size);
            if s.len() != 1 || s[0] != nxt {
                leaders.extend(s.iter().copied());
                if self.insns.contains_key(&nxt) {
                    leaders.insert(nxt);
                }
            }
        }
        self.leaders = leaders;
        let mut order: Vec<u32> = self.insns.keys().copied().collect();
        order.sort();
        self.order = order;
        self.n_insns = self.insns.len();
        self.n_bytes = self.insns.values().map(|i| i.size as u64).sum();
    }

    /// The value `r` has at `at`, by a cmp r, imm; jne before it (`back`: the
    /// instructions before `at`, in order) with nothing between that writes
    /// r, in a straight line that no branch comes into before `end`.
    fn pinned(&self, r: u8, back: &[u32], at: u32, end: u32) -> Option<i64> {
        let (mut next, mut jne) = (at, None);
        for &a in back.iter().rev() {
            let p = &self.insns[&a];
            if a.wrapping_add(p.size) != next {
                return None;
            }
            if jne.is_none() && p.mnemonic == "pop" && p.ops[0].is_reg() && reg_full(p.ops[0].reg()).map(|s| s.0) == Some(r) {
                // push imm; ...; pop r (68b5a3's push 7; sar eax, 4; pop ecx).
                let mut nx = a;
                for &b in back.iter().rev().skip_while(|&&b| b >= a) {
                    let q = &self.insns[&b];
                    if b.wrapping_add(q.size) != nx {
                        return None;
                    }
                    if q.mnemonic == "push" {
                        return if q.ops[0].is_imm() { Some(q.ops[0].imm()) } else { None };
                    }
                    if regs_rw(q).1 & (1 << 4) != 0 || matches!(q.mnemonic.as_str(), "call" | "ret" | "pop") || q.mnemonic.starts_with('j') {
                        return None;
                    }
                    nx = b;
                }
                return None;
            }
            if jne.is_none()
                && p.mnemonic == "mov"
                && p.ops.len() == 2
                && p.ops[0].is_reg()
                && p.ops[0].size == 4
                && reg_full(p.ops[0].reg()).map(|s| s.0) == Some(r)
                && p.ops[1].is_imm()
            {
                // mov r, imm (41bfc0's mov ecx, 4; cmp eax, ecx; ja), with no
                // branch into the code between it and the switch.
                let into = self.succ.iter().any(|(src, ts)| {
                    let fall = src.wrapping_add(self.insns[src].size);
                    ts.iter().any(|&t| t > a && t <= end && t != fall)
                });
                return if into { None } else { Some(p.ops[1].imm()) };
            }
            if jne.is_none() && p.mnemonic == "jne" {
                jne = Some(a);
            } else if jne.is_some() {
                let ops = &p.ops;
                if p.mnemonic != "cmp" || !ops[0].is_reg() || ops[0].size != 4 || reg_full(ops[0].reg()).map(|s| s.0) != Some(r) || !ops[1].is_imm() {
                    return None;
                }
                let from = jne.unwrap();
                let into = self.succ.iter().any(|(src, ts)| {
                    let fall = src.wrapping_add(self.insns[src].size);
                    ts.iter().any(|&t| t > from && t <= end && t != fall)
                });
                return if into { None } else { Some(ops[1].imm()) };
            } else if regs_rw(p).1 & (1 << r) != 0 || matches!(p.mnemonic.as_str(), "call" | "jmp" | "ret") {
                return None;
            }
            // Another conditional jump falls through with r as it was
            // (461dc0's mov ebx, 5; jz; mov eax, [edi]; cmp eax, ebx; ja).
            next = a;
        }
        None
    }

    /// jmp [reg*4 + table], bounded by the cmp/ja that guards the index; or
    /// through a byte index table first. None when it isn't through a table.
    fn switch(&mut self, img: &Image, va: u32, i: &Insn) -> Result<Option<Switch>, Refused> {
        let op = i.ops[0];
        let OpKind::Mem(m) = op.kind else { return Ok(None) };
        if m.base != 0 || m.scale != 4 || m.index == 0 {
            return Ok(None);
        }
        let idx = reg_full(m.index).expect("index register").0;
        let table = m.disp as u32;
        let mut back: Vec<u32> = self.insns.keys().copied().filter(|&a| a < va).collect();
        back.sort();
        let mut bytes_table: Option<u32> = None;
        let mut bound: Option<i64> = None;
        let mut default: Option<i64> = None;
        let mut slot: Option<(u16, i64)> = None;
        let mut cur = idx;
        let mut k = back.len() as i64 - 1;
        while k >= 0 && bound.is_none() {
            let a = back[k as usize];
            let p = self.insns[&a].clone();
            let next = if ((k + 1) as usize) < back.len() { back[(k + 1) as usize] } else { va };
            if a.wrapping_add(p.size) != next {
                break;
            }
            let ops = &p.ops;
            let mn = p.mnemonic.as_str();
            if mn == "movzx"
                && ops[0].is_reg()
                && reg_full(ops[0].reg()).map(|s| s.0) == Some(cur)
                && ops[1].is_mem()
                && ops[1].size == 1
                && ops[1].mem().index == 0
                && ops[1].mem().base != 0
            {
                bytes_table = Some(ops[1].mem().disp as u32);
                cur = reg_full(ops[1].mem().base).expect("base register").0;
            } else if mn == "ja" && default.is_none() {
                default = Some(ops[0].imm());
            } else if mn == "cmp"
                && default.is_some()
                && ops[0].is_reg()
                && reg_full(ops[0].reg()).map(|s| s.0) == Some(cur)
                && ops[1].is_imm()
            {
                bound = Some(ops[1].imm());
            } else if mn == "cmp"
                && default.is_some()
                && ops[0].is_reg()
                && reg_full(ops[0].reg()).map(|s| s.0) == Some(cur)
                && ops[1].is_reg()
                && ops[1].size == 4
            {
                // The index against a register an earlier cmp reg, imm; jne
                // pins on the way here (issue #138: 61bb80's cmp edi, 3; jnz;
                // add ecx, -0x67; cmp ecx, edi; ja; jmp [ecx*4 + table]).
                bound = self.pinned(reg_full(ops[1].reg()).expect("a register").0, &back[..k as usize], a, va);
                break;
            } else if slot.is_none()
                && bytes_table.is_none()
                && default.is_none()
                && mn == "mov"
                && ops.len() == 2
                && ops[0].is_reg()
                && reg_full(ops[0].reg()).map(|s| s.0) == Some(cur)
                && ops[1].is_mem()
                && ops[1].size == 4
                && ops[1].mem().index == 0
                && ops[1].mem().base != 0
            {
                // The index loaded from a stack slot the guard compared
                // (issue #138: 555230's cmp dword [ebp - 4], 5; ...; ja;
                // mov eax, [ebp - 4]; jmp [eax*4 + table]).
                slot = Some((ops[1].mem().base, ops[1].mem().disp));
            } else if mn == "cmp"
                && default.is_some()
                && slot.is_some()
                && ops[0].is_mem()
                && ops[0].size == 4
                && ops[0].mem().index == 0
                && Some((ops[0].mem().base, ops[0].mem().disp)) == slot
                && ops[1].is_imm()
            {
                bound = Some(ops[1].imm());
            } else if let Some((base, _)) = slot {
                // Until the guard: nothing that stores, calls, or moves the slot's base.
                let (_, w) = regs_rw(&p);
                let b = reg_full(base).map(|s| s.0).unwrap_or(4);
                if !mem_written(&p).is_empty() || mn == "call" || w & (1 << b) != 0 {
                    break;
                }
            } else {
                let (_, w) = regs_rw(&p);
                if w & (1 << cur) != 0 || (bytes_table.is_none() && w & (1 << idx) != 0) {
                    break;
                }
            }
            k -= 1;
        }
        // No guard at all (issue #138: 6bd780's sub ebx, 1; jmp [ebx*4 + table],
        // its index from the game's data): the table's length is its entries
        // that point into this function's code before it, and an index past
        // them, which the game's own code would jump into the bytes after
        // the table with, goes to the first.
        let (bound, default) = match (bound, default) {
            (Some(b), Some(d)) => (b, d),
            _ if table > self.entry => {
                let mut n = 0u32;
                while n < 512 {
                    let t = img.u32(table.wrapping_add(4 * n));
                    if t < self.entry || t >= table {
                        break;
                    }
                    n += 1;
                }
                // ... or, where that finds none, into its code after it too
                // (issue #138: the C runtime's memcpy and memmove, 6829c0 and
                // 683840, whose and edx, 3 is the bound and whose tables
                // stand between their pieces): while the entries are
                // addresses near the function's and outside the table.
                if n < 2 && bytes_table.is_none() {
                    n = 0;
                    while n < 512 {
                        let t = img.u32(table.wrapping_add(4 * n));
                        let inside = t >= table && t < table.wrapping_add(4 * (n + 1));
                        if t < self.entry || t >= self.entry.wrapping_add(0x1000) || inside || img.insn(t).is_err() {
                            break;
                        }
                        n += 1;
                    }
                }
                if n < 2 {
                    return Err(Refused(format!("a switch at {:08x} the guard rule doesn't bound", va)));
                }
                match bytes_table {
                    None => (n as i64 - 1, img.u32(table) as i64),
                    // Through a byte table too (6bdbdd): its bytes while they
                    // are entries of the table found.
                    Some(bt) => {
                        let mut b = 0u32;
                        while b < 256 && (img.u8(bt.wrapping_add(b)) as u32) < n {
                            b += 1;
                        }
                        if b < 2 {
                            return Err(Refused(format!("a switch at {:08x} the guard rule doesn't bound", va)));
                        }
                        (b as i64 - 1, img.u32(table) as i64)
                    }
                }
            }
            _ => return Err(Refused(format!("a switch at {:08x} the guard rule doesn't bound", va))),
        };
        if bound > 512 {
            return Err(Refused(format!("a switch at {:08x} the guard rule doesn't bound", va)));
        }
        let n = if let Some(bt) = bytes_table {
            let mut mx = 0u32;
            for j in 0..=bound {
                mx = mx.max(img.u8(bt.wrapping_add(j as u32)) as u32);
            }
            for j in 0..=bound {
                self.data.insert(bt.wrapping_add(j as u32));
            }
            1 + mx as i64
        } else {
            bound + 1
        };
        let targets: Vec<u32> = (0..n).map(|j| img.u32(table.wrapping_add(4 * j as u32))).collect();
        for j in 0..4 * n {
            self.data.insert(table.wrapping_add(j as u32));
        }
        for &t in &targets {
            if !img.in_text(t) {
                return Err(Refused(format!("a switch target outside .text at {:08x}", va)));
            }
        }
        Ok(Some(Switch { idx, targets, bytes_table, default: default as u32, bound }))
    }
}
