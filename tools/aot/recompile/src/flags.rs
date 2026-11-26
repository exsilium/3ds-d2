//! Which instruction's flags each reader reads, and which definers store
//! their operands for it (recompile.py's Flags).

use crate::func::Func;
use crate::program::Program;
use crate::x86::{flags_rw, kind, FA, FALL};
use crate::Refused;
use std::collections::{BTreeSet, HashMap, HashSet};

const ENTRY: i64 = -1;

/// A set of definers (ENTRY for the function's entry).
type Defs = BTreeSet<i64>;

pub struct Flags {
    /// reader -> (its definers in order, their (kind, width) where they agree)
    pub readers: HashMap<u32, (Vec<i64>, Option<(u8, u8)>)>,
    /// The definers some reader reads (AT_ENTRY for the function's entry).
    pub live_defs: HashSet<u32>,
    pub dynamic: HashSet<u32>,
    /// A jcc or its like reads the flags the function was entered with: its
    /// callers hand them on (Program::flag_in).
    pub needs_in: bool,
    /// The functions whose flags this one reads after its call to them, or
    /// hands on to a callee from: they hand them back (Program::flag_out).
    pub needs_out: BTreeSet<u32>,
}

/// The function's entry as a definer, in live_defs and dynamic.
pub const AT_ENTRY: u32 = u32::MAX;
/// What a call or a ret hands across (aot_rt.h's aot_flw: not the adjust flag).
const XF: u8 = FALL & !FA;
/// AOT_EFL's (kind, width): the flags as a word, a call's or the entry's (aot_xf).
const EFL: (u8, u8) = (12, 4);

/// (kind, width) of a definer's flags, as aot_rt.h numbers them.
pub fn kind_of(f: &Func, va: u32) -> Option<(u8, u8)> {
    if va == AT_ENTRY || f.calls.contains_key(&va) {
        return Some(EFL);
    }
    let i = &f.insns[&va];
    let m = i.mnemonic.as_str();
    let k = kind(m)?;
    if k == EFL.0 {
        return Some(EFL);
    }
    if matches!(m, "shl" | "sal" | "shr" | "sar") && !i.ops[1].is_imm() {
        return None;
    }
    if (m == "imul" || m == "mul") && i.ops.len() == 1 && i.ops[0].size != 4 {
        return None;
    }
    if m.ends_with("cmpxchg8b") {
        return Some((k, 4));
    }
    if m.ends_with(" cmpsb") {
        return Some((k, 1));
    }
    if m.ends_with(" cmpsw") {
        return Some((k, 2));
    }
    if m.ends_with(" cmpsd") {
        return Some((k, 4));
    }
    Some((k, i.ops[0].size))
}

fn need_reader(m: &str, va: u32) -> Result<(), Refused> {
    if m.starts_with('j') || m.starts_with("set") || m.starts_with("cmov") || m == "adc" || m == "sbb" || m == "rcr" {
        return Ok(());
    }
    Err(Refused(format!("{} reads flags at {:08x}", m, va)))
}

impl Flags {
    pub fn new(f: &Func, p: &Program) -> Result<Flags, Refused> {
        Flags::new_x(f, p, false)
    }

    /// With `discover`, what the function needs of others (needs_in,
    /// needs_out) whether or not they are known to give it yet: the driver's
    /// pass that finds Program::flag_in and flag_out.
    ///
    /// Flags across calls (issue #138, a build without Box86 alone: Box86's
    /// flags aren't aot_xf). A call to a flag_in function and a ret of a
    /// flag_out one read the flags, to store them in aot_xf; the entry and a
    /// call are definers, of what aot_xf holds then. Only a jcc's read of
    /// the entry's flags makes its function a flag_in one: a call or a ret
    /// that passes the entry's on stores what aot_xf held, whoever set it.
    pub fn new_x(f: &Func, p: &Program, discover: bool) -> Result<Flags, Refused> {
        let across = p.loose_on;
        let mut defs: HashMap<u32, u8> = HashMap::new();
        let mut uses: Vec<(u32, u8)> = Vec::new();
        // The calls, tail jumps and rets that store the flags for another function.
        let mut hands: HashSet<u32> = HashSet::new();
        for (va, i) in f.insns.iter() {
            let (mut d, mut u) = flags_rw(i);
            if f.calls.contains_key(va) {
                d = FALL;
                u = 0;
            }
            if across {
                let to = f.calls.get(va).or_else(|| f.tails.get(va)).and_then(|t| t.direct());
                let on = to.is_some_and(|t| p.flag_in.contains(&t)) && !(f.exits.contains(va) && !f.callouts.contains_key(va));
                let back = i.mnemonic == "ret" && p.flag_out.contains(&f.entry);
                if on || back {
                    u = XF;
                    hands.insert(*va);
                }
            }
            defs.insert(*va, d);
            uses.push((*va, u));
            if u != 0 && !hands.contains(va) {
                need_reader(&i.mnemonic, *va)?;
            }
        }
        // Reaching definitions per flag bit.
        let entry_st: [Defs; 6] = std::array::from_fn(|_| [ENTRY].into_iter().collect());
        let mut inb: HashMap<u32, [Defs; 6]> = HashMap::new();
        inb.insert(f.entry, entry_st);
        let mut work = vec![f.entry];
        let mut reach_at: HashMap<u32, [Defs; 6]> = HashMap::new();
        while let Some(b) = work.pop() {
            let mut st = inb[&b].clone();
            let mut va = b;
            loop {
                reach_at.insert(va, st.clone());
                let d = defs[&va];
                for k in 0..6 {
                    if d & (1 << k) != 0 {
                        st[k] = [va as i64].into_iter().collect();
                    }
                }
                let i = &f.insns[&va];
                let succ = &f.succ[&va];
                let nxt = va.wrapping_add(i.size);
                if succ.len() == 1 && succ[0] == nxt && !f.leaders.contains(&nxt) {
                    va = nxt;
                    continue;
                }
                for &t in succ {
                    let new = match inb.get(&t) {
                        None => st.clone(),
                        Some(old) => std::array::from_fn(|k| old[k].union(&st[k]).copied().collect()),
                    };
                    if inb.get(&t) != Some(&new) {
                        inb.insert(t, new);
                        work.push(t);
                    }
                }
                break;
            }
        }
        let mut fl = Flags { readers: HashMap::new(), live_defs: HashSet::new(), dynamic: HashSet::new(), needs_in: false, needs_out: BTreeSet::new() };
        for (va, u) in uses {
            if u == 0 {
                continue;
            }
            let mut src: Defs = BTreeSet::new();
            let mut per: Vec<&Defs> = Vec::new();
            for k in 0..6 {
                if u & (1 << k) != 0 {
                    per.push(&reach_at[&va][k]);
                    src.extend(reach_at[&va][k].iter().copied());
                }
            }
            for &x in &src {
                let hand = hands.contains(&va);
                if x == ENTRY {
                    // (A resume entry starts with no flags: read_before_set.)
                    if !across {
                        return Err(Refused(format!("flags read at {:08x} come from a call or the entry", va)));
                    }
                    if !hand {
                        if !discover && !p.flag_in.contains(&f.entry) {
                            return Err(Refused(format!("flags read at {:08x} come from the entry, and its callers don't hand them on", va)));
                        }
                        fl.needs_in = true;
                    }
                } else if let Some(t) = f.calls.get(&(x as u32)) {
                    if !across {
                        return Err(Refused(format!("flags read at {:08x} come from a call or the entry", va)));
                    }
                    // A ret passes on what its last call left, whatever that is.
                    if hand && f.insns[&va].mnemonic == "ret" {
                        continue;
                    }
                    let Some(t) = t.direct().filter(|t| p.can_flag_out(*t)) else {
                        return Err(Refused(format!("flags read at {:08x} come from the call at {:08x}, which no compiled function answers", va, x)));
                    };
                    if !discover && !p.flag_out.contains(&t) {
                        return Err(Refused(format!("flags read at {:08x} come from the call to {:08x}, which doesn't hand them back", va, t)));
                    }
                    fl.needs_out.insert(t);
                }
            }
            if per.iter().any(|p| *p != per[0]) {
                return Err(Refused(format!("flags read at {:08x} come from different instructions", va)));
            }
            let mut kinds: Vec<Option<(u8, u8)>> = Vec::new();
            for &x in &src {
                let k = kind_of(f, x as u32);
                if !kinds.contains(&k) {
                    kinds.push(k);
                }
            }
            if kinds.len() > 1 {
                fl.dynamic.extend(src.iter().map(|&x| x as u32));
            }
            let kd = if kinds.len() == 1 { kinds[0] } else { None };
            fl.readers.insert(va, (src.iter().copied().collect(), kd));
            fl.live_defs.extend(src.iter().map(|&x| x as u32));
        }
        let mut ld: Vec<u32> = fl.live_defs.iter().copied().collect();
        ld.sort();
        for d in ld {
            if kind_of(f, d).is_none() {
                return Err(Refused(format!("flags of {} read at {:08x}", f.insns[&d].mnemonic, d)));
            }
        }
        // A flag_out function's tail jump leaves its target's rets to hand
        // the flags back: a compiled function's, then.
        if across && p.flag_out.contains(&f.entry) {
            let mut ts: Vec<(&u32, &crate::func::Tgt)> = f.tails.iter().collect();
            ts.sort_by_key(|(va, _)| **va);
            for (va, t) in ts {
                match t.direct().filter(|t| p.can_flag_out(*t)) {
                    Some(t) if discover || p.flag_out.contains(&t) => {
                        fl.needs_out.insert(t);
                    }
                    _ => return Err(Refused(format!("its callers read its flags, and the tail jump at {:08x} leaves them to code that isn't known to hand them back", va))),
                }
            }
        }
        fl.check_slots(f)?;
        Ok(fl)
    }

    /// A flag read that code entered at x reaches before any instruction
    /// sets that flag (issue #138: a resume entry, which starts with no
    /// flags), or None.
    pub fn read_before_set(f: &Func, x: u32) -> Option<u32> {
        // Per address, the flag bits still unset on some path from x.
        let mut seen: HashMap<u32, u8> = HashMap::new();
        let mut work = vec![(x, FALL)];
        while let Some((va, unset)) = work.pop() {
            let had = seen.get(&va).copied().unwrap_or(0);
            if unset & !had == 0 {
                continue;
            }
            let unset = unset | had;
            seen.insert(va, unset);
            let i = &f.insns[&va];
            let (mut d, mut u) = flags_rw(i);
            if f.calls.contains_key(&va) {
                d = FALL;
                u = 0;
            }
            if u & unset != 0 {
                return Some(va);
            }
            let left = unset & !d;
            if left != 0 {
                for &t in &f.succ[&va] {
                    work.push((t, left));
                }
            }
        }
        None
    }

    /// The shared operand slots: no other storing definer between a definer
    /// and its reader.
    fn check_slots(&self, f: &Func) -> Result<(), Refused> {
        let mut inb: HashMap<u32, Defs> = HashMap::new();
        inb.insert(f.entry, [ENTRY].into_iter().collect());
        let mut work = vec![f.entry];
        let mut last: HashMap<u32, Defs> = HashMap::new();
        while let Some(b) = work.pop() {
            let mut st = inb[&b].clone();
            let mut va = b;
            loop {
                last.insert(va, st.clone());
                if self.live_defs.contains(&va) {
                    st = [va as i64].into_iter().collect();
                }
                let i = &f.insns[&va];
                let succ = &f.succ[&va];
                let nxt = va.wrapping_add(i.size);
                if succ.len() == 1 && succ[0] == nxt && !f.leaders.contains(&nxt) {
                    va = nxt;
                    continue;
                }
                for &t in succ {
                    let new = match inb.get(&t) {
                        None => st.clone(),
                        Some(old) => old.union(&st).copied().collect(),
                    };
                    if inb.get(&t) != Some(&new) {
                        inb.insert(t, new);
                        work.push(t);
                    }
                }
                break;
            }
        }
        // In the order recompile.py's readers dict has them: the insns' order.
        for va in f.insns.keys() {
            if let Some((src, _)) = self.readers.get(va) {
                let s: Defs = src.iter().copied().collect();
                if last[va] != s {
                    return Err(Refused(format!("flags at {:08x}: another instruction's operands are in the way", va)));
                }
            }
        }
        Ok(())
    }
}
