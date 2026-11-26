//! A set of functions compiled to C (recompile.py's compile_set and its
//! helpers: may_block, flags_read_after, hookable, leaves, the parts).

use crate::analysis::Analysis;
use crate::cs::{OpKind, GRP_JUMP, REG_EIP};
use crate::emit::{reent_name, resume_name, x87_op, Emitter, Sig};
use crate::flags::Flags;
use crate::func::{Func, Tgt};
use crate::image::Image;
use crate::json::J;
use crate::omap::OMap;
use crate::program::{natives, Program, Summary, ERROR_PATH_FUNCS, NORETURN_FUNCS};
use crate::x86::*;
use crate::Refused;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::time::Instant;

pub const BLOCKING_IMPORTS: [&str; 18] = [
    "Wait",
    "Sleep",
    "EnterCriticalSection",
    "ReadFile",
    "GetOverlappedResult",
    "GetMessage",
    "MsgWait",
    "SignalObjectAndWait",
    "DeviceIoControl",
    "recv",
    "send",
    "select",
    "connect",
    "accept",
    "gethostbyname",
    "DirectSound",
    "DirectDraw",
    "SwitchToThread",
];

/// A line on stderr every AOT_PROGRESS seconds (30; 0: off) in the long loops.
pub struct Progress {
    t0: Instant,
    last: Instant,
    every: f64,
}

impl Progress {
    pub fn new(t0: Instant) -> Progress {
        let every = std::env::var("AOT_PROGRESS").ok().and_then(|s| s.parse().ok()).unwrap_or(30.0);
        Progress { t0, last: Instant::now(), every }
    }
    pub fn tick(&mut self, what: &str, done: usize, total: usize) {
        let now = Instant::now();
        if self.every <= 0.0 || (now - self.last).as_secs_f64() < self.every {
            return;
        }
        self.last = now;
        let of = if total > 0 { format!(" of {} ({:.0}%)", total, 100.0 * done as f64 / total as f64) } else { String::new() };
        eprintln!("aot: {}: {}{}, {:.0} s", what, done, of, (now - self.t0).as_secs_f64());
    }
}

pub fn load_starts(tsv: &str) -> HashSet<u32> {
    let mut starts = HashSet::new();
    if let Ok(text) = std::fs::read_to_string(tsv) {
        for line in text.split_inclusive('\n').skip(1) {
            let a = line.split('\t').next().unwrap_or("");
            if let Ok(v) = parse_hex(a) {
                starts.insert(v);
            }
        }
    }
    starts
}

/// Python's int(s, 16): optional whitespace, sign, 0x, underscores between digits.
pub fn parse_hex(s: &str) -> Result<u32, ()> {
    let t = s.trim();
    let t = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    if t.is_empty() || t.starts_with('_') || t.ends_with('_') || t.contains("__") {
        return Err(());
    }
    let d: String = t.chars().filter(|&c| c != '_').collect();
    u64::from_str_radix(&d, 16).map(|v| v as u32).map_err(|_| ())
}

/// The whole instructions a trampoline copies (5 bytes or more) and the game's 16 bytes.
pub fn root_head(img: &Image, va: u32) -> Result<(u32, Vec<u8>), Refused> {
    let mut keep = 0u32;
    while keep < 5 {
        let i = img.insn(va + keep)?;
        if i.mnemonic == "call"
            || i.mnemonic == "jmp"
            || i.mnemonic == "ret"
            || i.mnemonic.starts_with('j')
            || i.ops.iter().any(|o| matches!(o.kind, OpKind::Mem(m) if m.base == REG_EIP))
        {
            return Err(Refused(format!("a branch in the first bytes at {:08x}: no trampoline", va)));
        }
        keep += i.size;
    }
    Ok((keep, (0..16).map(|k| img.u8(va + k)).collect()))
}

/// A function's trampoline where its first bytes have a branch (issue #138):
/// the code, with each rel32 written as the absolute target, and where those
/// are (the runtime makes them relative to where it puts the code).
pub type Tramp = (Vec<u8>, Vec<u8>);

/// root_head, and where the first bytes have a call, a jmp or a jcc that the
/// trampoline can do from elsewhere (rel8 and rel32, to rel32; an indirect
/// one as it is), that trampoline. Not a ret, a loop or jecxz, nor a jmp
/// before the 5 bytes (what follows it isn't the function's to overwrite).
pub fn fn_head(img: &Image, va: u32) -> Result<(u32, Vec<u8>, Option<Tramp>), Refused> {
    if let Ok((keep, hd)) = root_head(img, va) {
        return Ok((keep, hd, None));
    }
    let no = || Refused(format!("a branch in the first bytes at {:08x}: no trampoline", va));
    let mut keep = 0u32;
    let mut code: Vec<u8> = Vec::new();
    let mut rel: Vec<u8> = Vec::new();
    while keep < 5 {
        let a = va + keep;
        let i = img.insn(a)?;
        if i.ops.iter().any(|o| matches!(o.kind, OpKind::Mem(m) if m.base == REG_EIP)) {
            return Err(no());
        }
        let b = &i.bytes;
        let target = |d: i64| (a as i64 + i.size as i64 + d) as u32;
        let i8_at = |k: usize| b[k] as i8 as i64;
        let i32_at = |k: usize| i32::from_le_bytes([b[k], b[k + 1], b[k + 2], b[k + 3]]) as i64;
        let mut push_rel = |code: &mut Vec<u8>, op: &[u8], t: u32| {
            code.extend_from_slice(op);
            rel.push(code.len() as u8);
            code.extend_from_slice(&t.to_le_bytes());
        };
        let last;
        match (b[0], b.get(1).copied().unwrap_or(0)) {
            (0xe8, _) if i.size == 5 => {
                push_rel(&mut code, &[0xe8], target(i32_at(1)));
                last = false;
            }
            (0xe9, _) if i.size == 5 => {
                push_rel(&mut code, &[0xe9], target(i32_at(1)));
                last = true;
            }
            (0xeb, _) if i.size == 2 => {
                push_rel(&mut code, &[0xe9], target(i8_at(1)));
                last = true;
            }
            (0x70..=0x7f, _) if i.size == 2 => {
                push_rel(&mut code, &[0x0f, 0x80 + (b[0] - 0x70)], target(i8_at(1)));
                last = false;
            }
            (0x0f, 0x80..=0x8f) if i.size == 6 => {
                push_rel(&mut code, &[0x0f, b[1]], target(i32_at(2)));
                last = false;
            }
            (0xff, m) if (m >> 3) & 7 == 2 || (m >> 3) & 7 == 4 => {
                code.extend_from_slice(b);
                last = (m >> 3) & 7 == 4;
            }
            _ => {
                if i.mnemonic == "ret" || i.mnemonic.starts_with('j') || i.mnemonic.starts_with("loop") || i.mnemonic == "call" {
                    return Err(no());
                }
                code.extend_from_slice(b);
                last = false;
            }
        }
        keep += i.size;
        if last && keep < 5 {
            return Err(no());
        }
    }
    if code.len() + 5 > 32 || keep > 16 {
        return Err(no());
    }
    Ok((keep, (0..16).map(|k| img.u8(va + k)).collect(), Some((code, rel))))
}

fn text_bytes(img: &Image) -> &[u8] {
    let lo = (img.text_lo - img.base) as usize;
    let hi = (lo + (img.text_hi - img.text_lo) as usize).min(img.mem.len());
    &img.mem[lo.min(hi)..hi]
}

fn i32_at(mem: &[u8], k: usize) -> i64 {
    let mut b = [0u8; 4];
    for j in 0..4 {
        b[j] = *mem.get(k + j).unwrap_or(&0);
    }
    i32::from_le_bytes(b) as i64
}

/// Of `want`, those a rel32 call, jmp or jcc anywhere in .text could reach.
pub fn rel32_targets(img: &Image, want: &HashSet<u32>) -> BTreeSet<u32> {
    let mut hit = BTreeSet::new();
    let mem = text_bytes(img);
    let n = mem.len() as i64 - 5;
    let mut k = 0i64;
    while k < n {
        let ku = k as usize;
        let b = mem[ku];
        let (nn, d) = if b == 0xE8 || b == 0xE9 {
            (k + 5, i32_at(mem, ku + 1))
        } else if b == 0x0F && (0x80..=0x8F).contains(&mem[ku + 1]) && ku + 6 <= mem.len() {
            (k + 6, i32_at(mem, ku + 2))
        } else {
            k += 1;
            continue;
        };
        let t = (img.text_lo as i64 + nn + d) as u32;
        if want.contains(&t) {
            hit.insert(t);
        }
        k += 1;
    }
    hit
}

/// The compiled functions whose C, with every compiled callee's, never calls Box86.
pub fn leaves(prog: &Program, compiled: &BTreeSet<u32>) -> HashSet<u32> {
    let direct = |va: u32| -> bool {
        if prog.is_loose(va) {
            return true;   // only the dispatcher runs it
        }
        let f = prog.func(va);
        for t in f.calls.values().chain(f.tails.values()) {
            if let Tgt::Direct(t) = t {
                if (compiled.contains(t) && !prog.is_loose(*t)) || prog.intrinsics.contains_key(t) || prog.native_direct(*t) {
                    continue;
                }
            }
            return true;
        }
        false
    };
    let mut out: HashSet<u32> = compiled.iter().copied().filter(|&va| !direct(va)).collect();
    let mut changed = true;
    while changed {
        changed = false;
        let mut snap: Vec<u32> = out.iter().copied().collect();
        snap.sort();
        for va in snap {
            let f = prog.func(va);
            if f.calls.values().chain(f.tails.values()).any(|t| t.direct().is_some_and(|t| compiled.contains(&t) && !out.contains(&t))) {
                out.remove(&va);
                changed = true;
            }
        }
    }
    out
}

/// Of vas, those that may block their thread when Box86 runs them nested.
/// See its use: true with the function rebuilt where some pops balance it.
fn solve_pops(prog: &mut Program, va: u32, full_outs: Regs) -> bool {
    let f = prog.func(va);
    if f.plain || !prog.pops_fix.get(&va).map_or(true, |v| v.is_empty()) {
        return false;
    }
    let guessed = f.guessed_callouts(prog.img);
    if guessed.is_empty() || guessed.len() > 4 {
        return false;
    }
    let n = guessed.len();
    let mut combos: Vec<Vec<i64>> = vec![vec![]];
    for _ in 0..n {
        combos = combos.iter().flat_map(|c| (-3..=3).map(move |d| { let mut c = c.clone(); c.push(d); c })).collect();
    }
    // Less as well as more: a count takes for arguments what the caller drops
    // later (6b0330's read callback, add esp after), by up to 12 either way.
    combos.sort_by_key(|c| c.iter().map(|d| d.abs()).sum::<i64>());
    for c in combos.iter().skip(1) {
        if guessed.iter().zip(c).any(|(&(_, p), &d)| p + 4 * d < 0) {
            continue;
        }
        let fix: Vec<(u32, i64)> = guessed.iter().zip(c).map(|(&(site, p), &d)| (site, p + 4 * d)).collect();
        prog.with_pops(va, fix);
        if Analysis::new(prog, prog.func(va), Some(full_outs), true).is_ok_and(|a| stack_why(prog, &a).is_none()) {
            return true;
        }
    }
    prog.pops_fix.remove(&va);
    prog.with_pops(va, Vec::new());
    prog.pops_fix.remove(&va);
    false
}

/// Why a function can't have the static stack: esp at each of its rets known
/// to be the entry's. None where it can, and for a loose one, which has none
/// (Program::loose).
///
/// With --loose, its tail jumps through a pointer or to a loose function
/// too: a callout there is a tail call only with esp the entry's, and where
/// it isn't, or the target's pops aren't the function's own, the code has
/// nothing but Box86 to go on in (issue #138: the guard's exit).
fn stack_why(prog: &Program, a: &Analysis) -> Option<String> {
    let f = &a.f;
    if prog.is_loose(f.entry) {
        return None;
    }
    if !a.esp_ok {
        return Some(format!("esp at a ret isn't known to be the entry's: {}", a.esp_why()));
    }
    if prog.loose_on {
        let mut sites: Vec<(&u32, &Tgt)> = f.tails.iter().filter(|(va, _)| f.exits.contains(va)).collect();
        sites.sort();
        for (va, t) in sites {
            if !t.is_pointer() && !t.direct().is_some_and(|d| prog.is_loose(d)) {
                continue;
            }
            if !f.callouts.contains_key(va) {
                return Some(format!("the tail jump at {:08x}: what its target pops isn't known", va));
            }
            if a.sp_at.get(va).copied().flatten() != Some(0) {
                return Some(format!("esp at the tail jump at {:08x} isn't known to be the entry's", va));
            }
        }
        // A call to a loose function that doesn't come back to the code after
        // it is a jump of the dispatcher's (Program::loose_jump).
        let mut gone: Vec<&u32> = f.noreturn.iter().filter(|va| !f.exits.contains(va)).collect();
        gone.sort();
        for va in gone {
            if f.calls.get(va).and_then(|t| t.direct()).is_some_and(|d| prog.is_loose(d)) {
                return Some(format!("the call at {:08x} to a loose function doesn't come back", va));
            }
        }
    }
    None
}

/// A refusal for the stack alone (stack_why's, and the emitter's for a tail
/// call whose pops don't agree): what --loose compiles loose.
fn for_stack(why: &str) -> bool {
    why.starts_with("esp at ") || why.starts_with("the tail jump at ") || why.starts_with("the call at ") || why.starts_with("tail call ")
}

pub fn may_block(img: &Image, prog: &Program, vas: &HashSet<u32>) -> HashMap<u32, bool> {
    let mut out: HashMap<u32, bool> = HashMap::new();
    let skip = |f: &Func, site: &u32, t: &Tgt| -> bool {
        (f.noreturn.contains(site) && !f.exits.contains(site))
            || t.direct().is_some_and(|d| ERROR_PATH_FUNCS.contains(&d) || NORETURN_FUNCS.contains(&d))
    };
    let direct = |va: u32| -> bool {
        let f = prog.func(va);
        if f.refused.is_some() {
            return true;
        }
        for (site, t) in f.calls.iter().chain(f.tails.iter()) {
            if skip(&f, site, t) {
                continue;
            }
            match t {
                Tgt::Indirect => return true,
                Tgt::Import(slot) => {
                    let name = img.imports.get(slot).map(|x| x.1.as_str()).unwrap_or("");
                    if BLOCKING_IMPORTS.iter().any(|b| name.starts_with(b)) {
                        return true;
                    }
                }
                _ => {}
            }
        }
        false
    };
    let mut work: Vec<u32> = vas.iter().copied().collect();
    work.sort();
    for &va in &work {
        out.insert(va, direct(va));
    }
    let mut changed = true;
    while changed {
        changed = false;
        for &va in &work {
            if out[&va] {
                continue;
            }
            let f = prog.func(va);
            for (site, t) in f.calls.iter().chain(f.tails.iter()) {
                let Tgt::Direct(td) = t else { continue };
                if skip(&f, site, t) {
                    continue;
                }
                if prog.intrinsics.contains_key(td) || prog.native_direct(*td) {
                    continue;
                }
                if *out.get(td).unwrap_or(&true) {
                    out.insert(va, true);
                    changed = true;
                    break;
                }
            }
        }
    }
    out
}

fn reads_first(img: &Image, mut va: u32, depth: u32, steps: u32) -> Result<bool, Refused> {
    for _ in 0..steps {
        let i = img.insn(va)?;
        let (d, u) = flags_rw(&i);
        if u != 0 {
            return Ok(true);
        }
        if i.mnemonic == "call" {
            let op = i.ops[0];
            if depth == 0 && op.is_imm() && reads_first(img, op.imm() as u32, 1, steps)? {
                return Ok(true);
            }
            return Ok(false);
        }
        if d != 0 || i.mnemonic.starts_with("ret") || i.mnemonic.starts_with("jmp") || i.groups.contains(&GRP_JUMP) {
            return Ok(false);
        }
        va = va.wrapping_add(i.size);
    }
    Ok(false)
}

/// Of vas, the functions whose flags a caller may read after they return. A
/// function that never returns has no after (issue #138: exit(-1) at 46169c
/// ends the function, the bytes after the call being the next one's).
pub fn flags_read_after(img: &Image, vas: &BTreeSet<u32>) -> OMap<u32, u32> {
    let mut hit = OMap::new();
    let mem = text_bytes(img);
    let n = mem.len() as i64 - 5;
    let mut k = 0i64;
    while k < n {
        let ku = k as usize;
        if mem[ku] != 0xE8 {
            k += 1;
            continue;
        }
        let t = (img.text_lo as i64 + k + 5 + i32_at(mem, ku + 1)) as u32;
        if vas.contains(&t) && !NORETURN_FUNCS.contains(&t) && !hit.contains_key(&t) {
            if let Ok(true) = reads_first(img, img.text_lo + ku as u32 + 5, 0, 12) {
                hit.insert(t, img.text_lo + ku as u32);
            }
        }
        k += 1;
    }
    hit
}

/// A function's instructions as [lo, hi) runs of bytes, in order.
pub fn code_ranges(f: &Func) -> Vec<(u32, u32)> {
    let mut ranges: Vec<(u32, u32)> = Vec::new();
    for &a in &f.order {
        let e = a.wrapping_add(f.insns[&a].size);
        match ranges.last_mut() {
            Some(r) if r.1 == a => r.1 = e,
            _ => ranges.push((a, e)),
        }
    }
    ranges
}

/// FNV-1a over those bytes, as aot_spike.cpp's code_hash() takes it.
pub fn code_hash(img: &Image, ranges: &[(u32, u32)]) -> u32 {
    let mut h: u32 = 0x811c9dc5;
    for &(lo, hi) in ranges {
        let a = (lo - img.base) as usize;
        let b = ((hi - img.base) as usize).min(img.mem.len());
        for &x in &img.mem[a.min(b)..b] {
            h = (h ^ x as u32).wrapping_mul(0x01000193);
        }
    }
    h
}

/// Each compiled function's entry for the check: (keep, head) where it can be hooked, else why not.
/// The trampolines with a branch moved are in the third map (AOT_NO_RELOC=1: none, those not hooked).
#[allow(clippy::type_complexity)]
pub fn hookable(img: &Image, prog: &Program) -> (BTreeMap<u32, (u32, Vec<u8>)>, BTreeMap<u32, String>, BTreeMap<u32, Tramp>) {
    let mut heads: BTreeMap<u32, (u32, Vec<u8>)> = BTreeMap::new();
    let mut why: BTreeMap<u32, String> = BTreeMap::new();
    let mut tramps: BTreeMap<u32, Tramp> = BTreeMap::new();
    let reloc = std::env::var("AOT_NO_RELOC").map(|v| v != "1").unwrap_or(true);
    for &va in &prog.compiled {
        let h = if reloc { fn_head(img, va) } else { root_head(img, va).map(|(k, h)| (k, h, None)) };
        match h {
            Err(e) => {
                why.insert(va, e.0);
                continue;
            }
            Ok((keep, hd, t)) => {
                if prog.is_loose(va) {
                    why.insert(va, "loose: the dispatcher's alone".to_string());
                    continue;
                }
                if prog.summ[&va].pops.is_none() {
                    why.insert(va, "its ret pops aren't known".to_string());
                    continue;
                }
                heads.insert(va, (keep, hd));
                if let Some(t) = t {
                    tramps.insert(va, t);
                }
            }
        }
    }
    let mut inner: HashMap<u32, u32> = HashMap::new();
    for (&va, (keep, _)) in &heads {
        for k in 1..*keep {
            inner.insert(va + k, va);
        }
    }
    let mut region = inner.clone();
    for &va in heads.keys() {
        region.insert(va, va);
    }
    let mut keys = prog.func_keys();
    keys.sort();
    for va in keys {
        let f = prog.func(va);
        for (src, succ) in f.succ.iter() {
            let i = f.insns.get(src);
            for t in succ {
                if let Some(&it) = inner.get(t) {
                    let fall = i.is_some_and(|i| *t == src.wrapping_add(i.size)) && region.get(src) == Some(&it);
                    if !fall {
                        why.insert(it, format!("a branch from {:08x} into its first bytes ({:08x})", src, t));
                    }
                }
            }
        }
        for (src, t) in f.calls.iter().chain(f.tails.iter()) {
            if let Tgt::Direct(t) = t {
                if let Some(&it) = inner.get(t) {
                    why.insert(it, format!("a call or jump from {:08x} into its first bytes ({:08x})", src, t));
                }
            }
        }
    }
    let want: HashSet<u32> = inner.keys().copied().collect();
    for t in rel32_targets(img, &want) {
        why.entry(inner[&t]).or_insert_with(|| format!("a 32-bit branch somewhere in .text could reach its first bytes ({:08x})", t));
    }
    heads.retain(|va, _| !why.contains_key(va));
    tramps.retain(|va, _| heads.contains_key(va));
    (heads, why, tramps)
}

fn mask(rs: &[u8]) -> u32 {
    rs.iter().filter(|&&r| r != ESP).map(|&r| 1u32 << r).sum()
}

fn round3(v: f64) -> f64 {
    (v * 1000.0).round() / 1000.0
}

fn int_list(v: &[u8]) -> J {
    J::List(v.iter().map(|&x| J::Int(x as i64)).collect())
}

pub struct Compiled<'a> {
    pub prog: Program<'a>,
    pub report: J,
    pub refused: OMap<u32, String>,
    pub x86_insns: usize,
    pub x86_bytes: u64,
    pub leaves: usize,
    pub rounds: usize,
    pub analysis_s: f64,
    pub emit_s: f64,
}

fn calls_tails(f: &Func) -> impl Iterator<Item = (&u32, &Tgt)> {
    f.calls.iter().chain(f.tails.iter())
}

#[allow(clippy::too_many_arguments)]
pub fn compile_set<'a>(
    img: &'a Image,
    starts: &HashSet<u32>,
    roots: &[u32],
    extra: &[u32],
    mode_list: &[&'static str],
    outdir: &str,
    names: &HashMap<u32, String>,
    callouts: bool,
    loose: bool,
) -> Compiled<'a> {
    let t0 = Instant::now();
    let mut progress = Progress::new(t0);
    let mut all = starts.clone();
    all.extend(roots.iter().copied());
    all.extend(extra.iter().copied());
    let mut prog = Program::new(img, all, natives(), callouts);
    prog.loose_on = loose;
    let mut want: Vec<u32> = roots.to_vec();
    for &v in extra {
        if !roots.contains(&v) {
            want.push(v);
        }
    }
    let mut reach = prog.closure(&want, 30000);
    reach.extend(want.iter().copied());
    let mut reach_sorted: Vec<u32> = reach.iter().copied().collect();
    reach_sorted.sort();
    eprintln!("aot: {} functions to compile, {} with their callees, {:.0} s", want.len(), reach.len(), t0.elapsed().as_secs_f64());
    let mut refused: OMap<u32, String> = OMap::new();
    // What --loose may compile loose: not a root (its trap takes its pops),
    // nor a function a native stands for (called in C where it hands back).
    let wanted: HashSet<u32> = want.iter().copied().collect();
    let nat = natives();
    let can_loose = |va: u32| loose && wanted.contains(&va) && !roots.contains(&va) && !nat.contains_key(&va);
    // Each function's direct callers, before any is cut at an exit: theirs
    // are the exits to set again when it is made loose.
    let mut callers_of: HashMap<u32, Vec<u32>> = HashMap::new();
    if loose {
        for &va in &reach_sorted {
            let f = prog.func(va);
            let ts: BTreeSet<u32> = calls_tails(&f).filter_map(|(_, t)| t.direct()).collect();
            for t in ts {
                callers_of.entry(t).or_default().push(va);
            }
        }
    }
    // Thunks through a pointer whose pops nothing tells (issue #138): refused, as their decoding would be.
    // With --loose, loose: its jmp is the dispatcher's, with esp as it is there.
    prog.thunk_pops = Some(prog.compute_thunk_pops());
    for &va in &reach_sorted {
        let f = prog.func(va);
        let mut ptr: Vec<u32> = f.tails.iter().filter(|(_, t)| t.is_pointer()).map(|(k, _)| *k).collect();
        ptr.sort();
        if f.refused.is_none() && f.ret_pops.is_empty() && !ptr.is_empty() && !prog.thunk_pops.as_ref().unwrap().contains_key(&va) {
            let why = format!("a thunk through a pointer at {:08x} whose pops aren't known", ptr[0]);
            if can_loose(va) {
                prog.make_loose(va, &why);
                continue;
            }
            let mut g = (*f).clone();
            g.refused = Some(why);
            prog.set_func(va, g);
        }
    }
    // AOT_LOOSE=<va>,<va>... (with --loose): those compiled loose whatever
    // their stack, to see the lane run some (its scripts reach none of the
    // others; the dispatcher names each one's first run).
    if let Ok(list) = std::env::var("AOT_LOOSE") {
        for v in list.split(',').filter(|v| !v.trim().is_empty()) {
            let va = parse_hex(v).expect("AOT_LOOSE: hex addresses");
            if can_loose(va) && prog.func(va).refused.is_none() {
                prog.make_loose(va, "AOT_LOOSE");
            } else {
                eprintln!("aot: AOT_LOOSE: {:08x} can't be compiled loose", va);
            }
        }
    }
    // Flags across calls (flags.rs), with --loose: which functions read the
    // flags they are entered with and which hand theirs back, found before
    // any is compiled, since each changes what its callers and callees are.
    if loose {
        loop {
            let mut grew = false;
            for &va in &reach_sorted {
                let f = prog.func(va);
                if f.refused.is_some() {
                    continue;
                }
                let Ok(fl) = Flags::new_x(&f, &prog, true) else { continue };
                if fl.needs_in && prog.flag_in.insert(va) {
                    grew = true;
                }
                for t in fl.needs_out {
                    if prog.flag_out.insert(t) {
                        grew = true;
                    }
                }
            }
            if !grew {
                break;
            }
        }
        eprintln!("aot: flags across calls: {} functions read their entry's, {} hand theirs back", prog.flag_in.len(), prog.flag_out.len());
    }
    for &va in &want {
        let f = prog.func(va);
        if let Some(r) = &f.refused {
            refused.insert(va, r.clone());
            continue;
        }
        prog.compiled.insert(va);
    }
    let blocks = may_block(img, &prog, &reach);
    prog.apply_exits(&blocks);
    prog.summarize(&reach_sorted, 50);
    // What the analysis refuses whatever the signatures, screened once.
    let compiled_sorted: Vec<u32> = prog.compiled.iter().copied().collect();
    let screen = prog.callees_first(&compiled_sorted);
    let full_outs = set_of(&[EAX, ECX, EDX, EBX, EBP, ESI, EDI]);
    for (n, &va) in screen.iter().enumerate() {
        progress.tick("screened", n, screen.len());
        for attempt in 0..4 {
            let res: Result<(Regs, Option<Summary>), Refused> = (|| {
                let a = Analysis::new(&prog, prog.func(va), Some(full_outs), true)?;
                if let Some(why) = stack_why(&prog, &a) {
                    return Err(Refused(why));
                }
                Flags::new(&prog.func(va), &prog)?;
                let g = a.garbage;
                let s2 = if attempt >= 1 {
                    let s2 = Analysis::new(&prog, prog.func(va), None, true)?;
                    Some(Summary {
                        reads: s2.live_in,
                        preserved: s2.preserved,
                        pops: if s2.esp_moved { None } else { prog.cfg_pops.get(&va).copied().flatten() },
                        known: true,
                    })
                } else {
                    None
                };
                Ok((g, s2))
            })();
            match res {
                Ok((g, s2)) => {
                    prog.garbage.insert(va, g);
                    if let Some(s2) = s2 {
                        prog.summ.insert(va, s2);
                    }
                    break;
                }
                Err(e) => {
                    // The pops of the callouts that are a count of pushes, by
                    // what balances every ret (issue #138: 688fbb's call eax
                    // of a decoded pointer, with one argument pushed before
                    // the call that decodes it): each up to 12 more, the
                    // least change in all first, where there are at most four.
                    if attempt == 0 && e.0.starts_with("esp at ") && solve_pops(&mut prog, va, full_outs) {
                        continue;
                    }
                    // Loose, where plain exits would have nothing to exit
                    // to: its callers' calls become callouts now, for their
                    // own screening.
                    if attempt < 3 && for_stack(&e.0) && can_loose(va) && !prog.is_loose(va) {
                        prog.make_loose(va, &e.0);
                        let mut vas = vec![va];
                        vas.extend(callers_of.get(&va).into_iter().flatten().copied());
                        let lp = prog.loose_pops();
                        prog.exits_of(&vas, &blocks, &lp);
                        continue;
                    }
                    if attempt < 3 && e.0.starts_with("esp at a ret") && prog.make_plain(va, &e.0) {
                        continue;
                    }
                    refused.insert(va, e.0);
                    break;
                }
            }
        }
    }
    for (va, site) in flags_read_after(img, &prog.compiled).iter() {
        if prog.flag_out.contains(va) {
            continue;   // it hands them back in aot_xf (flags.rs), and the caller that reads them was compiled for that
        }
        refused.insert(*va, format!("its flags are read after the call at {:08x}", site));
    }
    for va in refused.keys() {
        prog.compiled.remove(va);
    }
    prog.apply_exits(&blocks);
    prog.summarize(&reach_sorted, 50);
    // A callee's leftovers (garbage) in ecx or edx that reach a caller's ret
    // are its leftovers too, also where the callee is screened after it (a
    // cycle): 55c270 returned 55c110's ecx, which its compiled code doesn't
    // keep, and its callers' read of it was compared.
    let mut more = 0usize;
    for _ in 0..20 {
        let mut changed = 0usize;
        for &va in &screen {
            if !prog.compiled.contains(&va) {
                continue;
            }
            let f = prog.func(va);
            if !calls_tails(&f).any(|(_, t)| t.direct().is_some_and(|d| prog.garbage.get(&d).is_some_and(|&g| g != 0))) {
                continue;
            }
            let Ok(a) = Analysis::new(&prog, f, Some(full_outs), true) else { continue };
            let old = prog.garbage.get(&va).copied().unwrap_or(0);
            if a.garbage & !old != 0 {
                prog.garbage.insert(va, old | a.garbage);
                changed += 1;
            }
        }
        more += changed;
        if changed == 0 {
            break;
        }
    }
    if more > 0 {
        eprintln!("aot: {} functions' leftovers found again after their callees'", more);
    }
    let mut rounds = 0usize;
    let mut outs: HashMap<u32, Regs>;
    let sigs: HashMap<u32, Sig>;
    loop {
        rounds += 1;
        loop {
            for va in prog.compiled.clone() {
                let s = prog.summ[&va];
                prog.summ.insert(va, Summary { reads: 0, preserved: s.preserved, pops: s.pops, known: true });
            }
            outs = prog
                .compiled
                .iter()
                .map(|&va| {
                    let g = prog.garbage.get(&va).copied().unwrap_or(0);
                    (va, (set_of(&[EAX, ECX, EDX]) & !g) | (CALLEE_SAVED_REGS & !prog.summ[&va].preserved))
                })
                .collect();
            let mut callers: HashMap<u32, BTreeSet<u32>> = HashMap::new();
            for &va in &prog.compiled {
                let g = prog.func(va);
                for (_, t) in calls_tails(&g) {
                    if let Tgt::Direct(t) = t {
                        if prog.compiled.contains(t) {
                            callers.entry(*t).or_default().insert(va);
                        }
                    }
                }
            }
            let cs: Vec<u32> = prog.compiled.iter().copied().collect();
            let order = prog.callees_first(&cs);
            let mut queued: HashSet<u32> = order.iter().copied().collect();
            let mut work: VecDeque<u32> = order.into_iter().collect();
            let mut budget = 100 * prog.compiled.len().max(1) as i64;
            let mut settled = 0usize;
            let mut hit: OMap<u32, String> = OMap::new();
            let mut put_off: HashSet<u32> = HashSet::new();
            while let Some(va) = work.pop_front() {
                settled += 1;
                progress.tick(
                    &format!("signatures, round {}: {} analysed, {} refused this pass, still queued", rounds, settled, hit.len()),
                    work.len() + 1,
                    0,
                );
                budget -= 1;
                if budget < 0 {
                    panic!("signatures didn't settle");
                }
                queued.remove(&va);
                if hit.contains_key(&va) {
                    continue;
                }
                let f = prog.func(va);
                let res = (|| {
                    let a = Analysis::new(&prog, f.clone(), Some(outs[&va]), true)?;
                    if let Some(why) = stack_why(&prog, &a) {
                        return Err(Refused(why));
                    }
                    Flags::new(&f, &prog)?;
                    Ok((a.live_in, a.live_out.clone()))
                })();
                let (live_in, live_out) = match res {
                    Ok(x) => x,
                    Err(e) => {
                        if calls_tails(&f).any(|(_, t)| t.direct().is_some_and(|t| hit.contains_key(&t))) {
                            put_off.insert(va);
                        } else {
                            hit.insert(va, e.0);
                        }
                        continue;
                    }
                };
                put_off.remove(&va);
                let s = prog.summ[&va];
                if live_in != s.reads {
                    prog.summ.insert(va, Summary { reads: live_in, preserved: s.preserved, pops: s.pops, known: true });
                    if let Some(cs) = callers.get(&va) {
                        for &c in cs {
                            if queued.insert(c) {
                                work.push_back(c);
                            }
                        }
                    }
                }
                for (site, t) in calls_tails(&f) {
                    let Tgt::Direct(t) = *t else { continue };
                    if !prog.compiled.contains(&t) || prog.is_loose(t) {
                        continue;   // (a loose one's outs are every register)
                    }
                    let live = if f.tails.contains_key(site) { outs[&va] } else { live_out[site] };
                    let g = prog.garbage.get(&t).copied().unwrap_or(0);
                    let add = live & !prog.summ[&t].preserved & !bit(ESP) & !g;
                    if add & !outs[&t] != 0 {
                        *outs.get_mut(&t).unwrap() |= add;
                        if queued.insert(t) {
                            work.push_back(t);
                        }
                    }
                }
            }
            if hit.is_empty() && put_off.is_empty() {
                break;
            }
            if hit.is_empty() {
                panic!("signatures: {} functions put off with no refusal", put_off.len());
            }
            for (va, why) in hit.iter() {
                if for_stack(why) && can_loose(*va) && !prog.is_loose(*va) {
                    prog.make_loose(*va, why);
                    continue;
                }
                if why.starts_with("esp at a ret") && prog.make_plain(*va, why) {
                    continue;
                }
                refused.insert(*va, why.clone());
                prog.compiled.remove(va);
            }
            eprintln!(
                "aot: signatures, round {}: a pass refused {} ({} put off), {:.0} s",
                rounds,
                hit.len(),
                put_off.len(),
                t0.elapsed().as_secs_f64()
            );
            prog.apply_exits(&blocks);
            prog.summarize(&reach_sorted, 50);
        }
        let s: HashMap<u32, Sig> = prog
            .compiled
            .iter()
            .map(|&va| {
                let sm = prog.summ[&va];
                if prog.is_loose(va) {
                    // Every register in and out, none taken as kept.
                    return (va, Sig { ins: regs_of(full_outs), outs: regs_of(full_outs), preserved: Vec::new() });
                }
                (va, Sig { ins: regs_of(sm.reads & !bit(ESP)), outs: regs_of(outs[&va]), preserved: regs_of(sm.preserved) })
            })
            .collect();
        // Every function emitted once, both ways: what the emitter refuses goes to Box86.
        let mut bad: OMap<u32, String> = OMap::new();
        for &mode in mode_list {
            let mut em = Emitter::new(&prog, mode, &s);
            let cs: Vec<u32> = prog.compiled.iter().copied().collect();
            for (n, &va) in cs.iter().enumerate() {
                progress.tick(&format!("emitter check, round {}, {}", rounds, mode), n, cs.len());
                if bad.contains_key(&va) {
                    continue;
                }
                let r = (|| -> Result<(), Refused> {
                    let out = set_of(&s[&va].outs);
                    em.ana = Some(Analysis::new(&prog, prog.func(va), Some(out), mode == "sig")?);
                    em.function(va)?;
                    Ok(())
                })();
                if let Err(e) = r {
                    bad.insert(va, e.0);
                }
            }
        }
        if bad.is_empty() {
            sigs = s;
            break;
        }
        for (va, why) in bad.iter() {
            // A tail call whose pops aren't its target's: a jump the
            // dispatcher makes, from a loose function.
            if for_stack(why) && can_loose(*va) && !prog.is_loose(*va) {
                prog.make_loose(*va, why);
                continue;
            }
            refused.insert(*va, why.clone());
            prog.compiled.remove(va);
        }
        prog.apply_exits(&blocks);
        prog.summarize(&reach_sorted, 50);
        eprintln!("aot: round {}: {} refused by the emitter, {:.0} s", rounds, bad.len(), t0.elapsed().as_secs_f64());
    }
    let t1 = Instant::now();
    write_out(img, prog, sigs, refused, roots, mode_list, outdir, names, reach.len(), rounds, t0, t1)
}

const CALLEE_SAVED_REGS: Regs = (1 << EBX) | (1 << EBP) | (1 << ESI) | (1 << EDI);

#[allow(clippy::too_many_arguments)]
fn write_out<'a>(
    img: &'a Image,
    prog: Program<'a>,
    sigs: HashMap<u32, Sig>,
    refused: OMap<u32, String>,
    roots: &[u32],
    mode_list: &[&'static str],
    outdir: &str,
    names: &HashMap<u32, String>,
    closure: usize,
    rounds: usize,
    t0: Instant,
    t1: Instant,
) -> Compiled<'a> {
    let mut progress = Progress::new(t0);
    let head: Vec<String> = vec![
        "/* Generated by tools/aot/recompile from Game.exe: never commit it (docs/aot.md). */".to_string(),
        "#include \"game/aot_rt.h\"".to_string(),
        String::new(),
    ];
    let mut chunks: Vec<String> = head.clone();
    let mut report = J::obj();
    report.set("functions", J::obj());
    report.set("refused", J::Obj(refused.iter().map(|(k, v)| (format!("{:08x}", k), J::Str(v.clone()))).collect()));
    report.set("fallbacks", J::obj());
    let compiled: Vec<u32> = prog.compiled.iter().copied().collect();
    let mut texts: HashMap<u32, Vec<(&'static str, String)>> = HashMap::new();
    let mut ems: Vec<Emitter> = Vec::new();
    for &mode in mode_list {
        let mut em = Emitter::new(&prog, mode, &sigs);
        chunks.extend(compiled.iter().map(|&va| em.proto(va) + ";"));
        chunks.push(String::new());
        for (n, &va) in compiled.iter().enumerate() {
            progress.tick(&format!("writing the C, {}", mode), n, compiled.len());
            let out = set_of(&sigs[&va].outs);
            em.ana = Some(Analysis::new(&prog, prog.func(va), Some(out), mode == "sig").expect("analysis passed before"));
            let text = em.function(va).expect("emitted before");
            texts.entry(va).or_default().push((mode, text));
        }
        em.ana = None;
        ems.push(em);
    }
    // The natives' hand-backs (issue #138): each compiled as an entry into
    // the function that holds it, sig mode only.
    let mut resume_texts: HashMap<u32, Vec<String>> = HashMap::new();
    let mut resumed: Vec<(u32, u32, i64)> = Vec::new();
    let mut resume_why: Vec<(u32, String)> = Vec::new();
    if let Some(mi) = mode_list.iter().position(|&m| m == "sig") {
        let em = &mut ems[mi];
        for &x in &img.resumes {
            let at = compiled.iter().copied().filter(|&va| va <= x && prog.func(va).insns.contains_key(&x)).max();
            let Some(va) = at else {
                resume_why.push((x, "in no compiled function".to_string()));
                continue;
            };
            let f = prog.func(va);
            if x == f.entry {
                resume_why.push((x, "a compiled function's entry".to_string()));
                continue;
            }
            if prog.is_loose(va) {
                resume_why.push((x, "in a loose function: no stack offset there".to_string()));
                continue;
            }
            if let Some(r) = Flags::read_before_set(&f, x) {
                resume_why.push((x, format!("flags read at {:08x} before anything sets them", r)));
                continue;
            }
            let out = set_of(&sigs[&va].outs);
            let ana = match Analysis::new(&prog, f.clone(), Some(out), false) {
                Ok(a) => a,
                Err(Refused(e)) => {
                    resume_why.push((x, e));
                    continue;
                }
            };
            let Some(Some(sp)) = ana.sp_at.get(&x).copied() else {
                resume_why.push((x, "the stack offset there not known".to_string()));
                continue;
            };
            em.ana = Some(ana);
            match em.resume_fn(va, x, sp) {
                Ok(t) => {
                    resume_texts.entry(va).or_default().push(t);
                    resumed.push((x, va, sp));
                }
                Err(Refused(e)) => resume_why.push((x, e)),
            }
            em.ana = None;
        }
    }
    // The return sites (issue #138): where a thread stopped in compiled code
    // goes on (a shim's yield at a callout, the C frames above it dropped),
    // each function's in one entry, reent_<va>. A callout's return, and a
    // direct call's to a compiled function that may exit. AOT_NO_REENT=1:
    // none.
    let mut reents: Vec<(u32, u32, i64)> = Vec::new();
    let mut reent_fns = 0usize;
    let mut reent_why: BTreeMap<String, usize> = BTreeMap::new();
    let mut reent_taken: HashSet<u32> = HashSet::new();   // a site in two functions' code: the first's
    if let (Some(mi), false) = (mode_list.iter().position(|&m| m == "sig"), std::env::var("AOT_NO_REENT").is_ok_and(|v| v == "1")) {
        let em = &mut ems[mi];
        for (n, &va) in compiled.iter().enumerate() {
            progress.tick("writing the return sites' entries", n, compiled.len());
            if !prog.may_exit.contains(&va) || prog.is_loose(va) {
                continue;   // (a loose function's sites have no stack offset to enter with)
            }
            let f = prog.func(va);
            let mut want: Vec<u32> = Vec::new();
            for (&c, t) in f.calls.iter() {
                if f.noreturn.contains(&c) {
                    continue;
                }
                let ra = c.wrapping_add(f.insns[&c].size);
                if !f.insns.contains_key(&ra) {
                    continue;
                }
                let exits = f.callouts.contains_key(&c)
                    || t.direct().is_some_and(|d| prog.compiled.contains(&d) && prog.may_exit.contains(&d));
                if exits {
                    want.push(ra);
                }
            }
            if want.is_empty() {
                continue;
            }
            let out = set_of(&sigs[&va].outs);
            let ana = match Analysis::new(&prog, f.clone(), Some(out), false) {
                Ok(a) => a,
                Err(Refused(e)) => {
                    *reent_why.entry(e.split(" at ").next().unwrap_or("").to_string()).or_default() += want.len();
                    continue;
                }
            };
            let mut sites: Vec<(u32, i64)> = Vec::new();
            for x in want {
                if !reent_taken.insert(x) {
                    *reent_why.entry("a site of another function's entry".to_string()).or_default() += 1;
                } else if Flags::read_before_set(&f, x).is_some() {
                    *reent_why.entry("flags read before anything sets them".to_string()).or_default() += 1;
                } else if let Some(Some(sp)) = ana.sp_at.get(&x).copied() {
                    sites.push((x, sp));
                } else {
                    *reent_why.entry("the stack offset there not known".to_string()).or_default() += 1;
                }
            }
            if sites.is_empty() {
                continue;
            }
            // The entry too: sig_<va> calls reent_<va> there, so the code is
            // there once (AOT_REENT_COPY=1: twice, sig_ its own).
            let by_reent = !std::env::var("AOT_REENT_COPY").is_ok_and(|v| v == "1");
            let mut all = sites.clone();
            if by_reent && !sites.iter().any(|e| e.0 == f.entry) {
                all.push((f.entry, 0));
            }
            em.ana = Some(ana);
            match em.reent_fn(va, &all) {
                Ok(t) => {
                    resume_texts.entry(va).or_default().push(t);
                    reents.extend(sites.iter().map(|&(x, sp)| (x, va, sp)));
                    reent_fns += 1;
                    if by_reent {
                        let w = em.sig_by_reent(va);
                        if let Some(v) = texts.get_mut(&va) {
                            for (m, text) in v.iter_mut() {
                                if *m == "sig" {
                                    *text = w.clone();
                                }
                            }
                        }
                    }
                }
                Err(Refused(e)) => *reent_why.entry(e.split(" at ").next().unwrap_or("").to_string()).or_default() += sites.len(),
            }
            em.ana = None;
        }
    }
    let mut ej = J::obj();
    ej.set("functions", J::Int(reent_fns as i64));
    ej.set("sites", J::Int(reents.len() as i64));
    ej.set("refused", J::Obj(reent_why.iter().map(|(k, v)| (k.clone(), J::Int(*v as i64))).collect()));
    report.set("reents", ej);
    let mut rj = J::obj();
    for &(x, va, _) in &resumed {
        rj.set(&format!("{:08x}", x), J::Str(format!("{:08x}", va)));
    }
    report.set("resumes", rj);
    report.set("resumes_refused", J::Obj(resume_why.iter().map(|(x, e)| (format!("{:08x}", x), J::Str(e.clone()))).collect()));
    let part_max: usize = (std::env::var("AOT_PART_MB").ok().and_then(|s| s.parse::<f64>().ok()).unwrap_or(8.0) * 1e6) as usize;
    let cset: HashSet<u32> = compiled.iter().copied().collect();
    let callees_of: HashMap<u32, BTreeSet<u32>> = compiled
        .iter()
        .map(|&va| {
            let f = prog.func(va);
            (va, calls_tails(&f).filter_map(|(_, t)| t.direct()).filter(|t| cset.contains(t)).collect())
        })
        .collect();
    // With the entries written beside it (resume_, reent_): sig_ may be only a
    // call to reent_ (issue #138: counted alone, one part had 105 MB of C).
    let size_of = |va: u32| -> usize {
        texts[&va].iter().map(|(_, t)| t.len()).sum::<usize>() + resume_texts.get(&va).map_or(0, |v| v.iter().map(|t| t.len()).sum())
    };
    let cut = |order: &[u32]| -> (usize, Vec<Vec<u32>>) {
        let mut parts: Vec<Vec<u32>> = Vec::new();
        let mut cur: Vec<u32> = Vec::new();
        let mut size = 0usize;
        for &va in order {
            let n = size_of(va);
            if !cur.is_empty() && size + n > part_max {
                parts.push(std::mem::take(&mut cur));
                size = 0;
            }
            cur.push(va);
            size += n;
        }
        if !cur.is_empty() {
            parts.push(cur);
        }
        let mut wh: HashMap<u32, usize> = HashMap::new();
        for (k, p) in parts.iter().enumerate() {
            for &va in p {
                wh.insert(va, k);
            }
        }
        let across = compiled.iter().map(|va| callees_of[va].iter().filter(|t| wh[t] != wh[va]).count()).sum();
        (across, parts)
    };
    let clusters = || -> Vec<u32> {
        let mut w: BTreeMap<(u32, u32), i64> = BTreeMap::new();
        for &va in &compiled {
            let g = prog.func(va);
            for (_, t) in calls_tails(&g) {
                if let Tgt::Direct(t) = *t {
                    if cset.contains(&t) && t != va {
                        *w.entry((va.min(t), va.max(t))).or_insert(0) += 1;
                    }
                }
            }
        }
        let mut up: HashMap<u32, u32> = compiled.iter().map(|&v| (v, v)).collect();
        let mut csize: HashMap<u32, usize> = compiled.iter().map(|&v| (v, size_of(v))).collect();
        let mut members: HashMap<u32, Vec<u32>> = compiled.iter().map(|&v| (v, vec![v])).collect();
        fn find(up: &mut HashMap<u32, u32>, mut x: u32) -> u32 {
            while up[&x] != x {
                let g = up[&up[&x]];
                up.insert(x, g);
                x = g;
            }
            x
        }
        let mut pairs: Vec<((u32, u32), i64)> = w.into_iter().collect();
        pairs.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
        for ((a, b), _) in pairs {
            let (mut ra, mut rb) = (find(&mut up, a), find(&mut up, b));
            if ra == rb || csize[&ra] + csize[&rb] > part_max {
                continue;
            }
            if csize[&ra] < csize[&rb] {
                std::mem::swap(&mut ra, &mut rb);
            }
            up.insert(rb, ra);
            let sb = csize.remove(&rb).unwrap();
            *csize.get_mut(&ra).unwrap() += sb;
            let mb = members.remove(&rb).unwrap();
            members.get_mut(&ra).unwrap().extend(mb);
        }
        let mut rs: Vec<u32> = members.keys().copied().collect();
        rs.sort_by(|a, b| csize[b].cmp(&csize[a]).then(a.cmp(b)));
        let mut bins: Vec<(usize, Vec<u32>)> = Vec::new();
        for r in rs {
            let mut placed = false;
            for b in bins.iter_mut() {
                if b.0 + csize[&r] <= part_max {
                    b.0 += csize[&r];
                    b.1.extend(members[&r].iter().copied());
                    placed = true;
                    break;
                }
            }
            if !placed {
                bins.push((csize[&r], members[&r].clone()));
            }
        }
        let mut order = Vec::new();
        for (_, mut b) in bins {
            b.sort();
            order.extend(b);
        }
        order
    };
    let cands: Vec<(&str, (usize, Vec<Vec<u32>>))> =
        vec![("address", cut(&compiled)), ("post-order", cut(&prog.callees_first(&compiled))), ("clusters", cut(&clusters()))];
    let mut best = 0;
    for k in 1..cands.len() {
        let (a, b) = (&cands[k].1, &cands[best].1);
        if (a.0, a.1.len()) < (b.0, b.1.len()) {
            best = k;
        }
    }
    eprintln!(
        "aot: calls across parts by order: {}",
        cands.iter().map(|(k, v)| format!("{} {} ({} parts)", k, v.0, v.1.len())).collect::<Vec<_>>().join(", ")
    );
    let order_name = cands[best].0;
    let parts: Vec<Vec<u32>> = cands[best]
        .1
         .1
        .iter()
        .map(|p| {
            let mut p = p.clone();
            p.sort();
            p
        })
        .collect();
    let mut part_of: HashMap<u32, usize> = HashMap::new();
    for (k, p) in parts.iter().enumerate() {
        for &va in p {
            part_of.insert(va, k);
        }
    }
    let (mut cross, mut local) = (0i64, 0i64);
    let mut part_texts: Vec<Vec<String>> = Vec::new();
    for (k, part) in parts.iter().enumerate() {
        let mut out = head.clone();
        let mut callees: BTreeSet<u32> = BTreeSet::new();
        for &va in part {
            let g = prog.func(va);
            for (_, t) in calls_tails(&g) {
                if let Tgt::Direct(t) = *t {
                    if cset.contains(&t) {
                        callees.insert(t);
                        if part_of[&t] == k {
                            local += 1;
                        } else {
                            cross += 1;
                        }
                    }
                }
            }
        }
        let mut need: BTreeSet<u32> = part.iter().copied().collect();
        need.extend(callees.iter().copied());
        for (mi, &mode) in mode_list.iter().enumerate() {
            let em = &ems[mi];
            out.extend(need.iter().map(|&va| em.proto(va) + ";"));
            out.push(String::new());
            if mode == "sig" {
                out.extend(callees.iter().map(|&va| em.wrappers(va)));
                out.push(String::new());
            }
            for &va in part {
                out.push(texts[&va].iter().find(|(m, _)| *m == mode).unwrap().1.clone());
                out.push(String::new());
                if mode == "sig" {
                    for t in resume_texts.get(&va).into_iter().flatten() {
                        out.push(t.clone());
                        out.push(String::new());
                    }
                }
            }
        }
        part_texts.push(out);
    }
    let mut pj = J::obj();
    pj.set("files", J::Int(parts.len() as i64));
    pj.set("calls_local", J::Int(local));
    pj.set("calls_across", J::Int(cross));
    pj.set("order", J::Str(order_name.to_string()));
    report.set("parts", pj);
    for (mi, &mode) in mode_list.iter().enumerate() {
        let em = &ems[mi];
        for &va in roots {
            if !cset.contains(&va) {
                continue;
            }
            let mut body: Vec<String> = Vec::new();
            if mode == "ctx" {
                body.push(format!("AotRegs t; for (int k = 0; k < 8; k++) t.r[k] = r[k]; {}(&t);", em.name(va)));
                body.push("for (int k = 0; k < 8; k++) if (k != 4) r[k] = t.r[k];".to_string());
            } else {
                let sig = &sigs[&va];
                let mut a = vec!["r[4]".to_string()];
                a.extend(sig.ins.iter().map(|x| format!("r[{}]", x)));
                let args = a.join(", ");
                let o = &sig.outs;
                if o.len() == 1 {
                    body.push(format!("r[{}] = {}({});", o[0], em.name(va), args));
                } else if o.len() == 2 {
                    body.push(format!("uint64_t t = {}({}); r[{}] = (uint32_t)t; r[{}] = (uint32_t)(t >> 32);", em.name(va), args, o[0], o[1]));
                } else {
                    body.push(format!("uint32_t o[8]; {}({}{});", em.name(va), args, if o.is_empty() { "" } else { ", o" }));
                    body.push(o.iter().enumerate().map(|(k, x)| format!("r[{}] = o[{}];", x, k)).collect::<Vec<_>>().join(" "));
                }
            }
            chunks.push(format!(
                "static uint32_t AOT_SYM({}_root_{:08x})(uint32_t *r)\n{{\n    {}\n    return r[0];\n}}\n",
                mode,
                va,
                body.join("\n    ")
            ));
        }
    }
    let (heads, why, tramps) = hookable(img, &prog);
    let mut leaf_n = 0usize;
    if mode_list.contains(&"sig") {
        let em = Emitter::new(&prog, "sig", &sigs);
        for &va in &compiled {
            if prog.is_loose(va) {
                // All eight registers in and out, r[4] as its ret or tail
                // jump left it; what it returns is where control goes on.
                chunks.push(format!(
                    "static uint32_t AOT_SYM(fn_{:08x})(uint32_t *r) {{ AotRegs t; for (int k = 0; k < 8; k++) t.r[k] = r[k]; uint32_t to = {}(&t); for (int k = 0; k < 8; k++) r[k] = t.r[k]; return to; }}",
                    va,
                    em.name(va)
                ));
                continue;
            }
            let sig = &sigs[&va];
            let mut a = vec!["r[4]".to_string()];
            a.extend(sig.ins.iter().map(|x| format!("r[{}]", x)));
            let args = a.join(", ");
            let o = &sig.outs;
            let body = if o.len() == 1 {
                format!("r[{}] = {}({});", o[0], em.name(va), args)
            } else if o.len() == 2 {
                format!("uint64_t t = {}({}); r[{}] = (uint32_t)t; r[{}] = (uint32_t)(t >> 32);", em.name(va), args, o[0], o[1])
            } else {
                format!(
                    "uint32_t o[8]; {}({}{}); {}",
                    em.name(va),
                    args,
                    if o.is_empty() { "" } else { ", o" },
                    o.iter().enumerate().map(|(k, x)| format!("r[{}] = o[{}];", x, k)).collect::<Vec<_>>().join(" ")
                )
            };
            chunks.push(format!("static uint32_t AOT_SYM(fn_{:08x})(uint32_t *r) {{ {} return r[0]; }}", va, body));
        }
        let index: HashMap<u32, usize> = compiled.iter().enumerate().map(|(k, &v)| (v, k)).collect();
        // The compiled functions `f` calls in C: not a loose one, a callout
        // (and from a loose function, not its tail jumps' targets).
        let in_c = |f: &Func| -> BTreeSet<u32> {
            calls_tails(f)
                .filter(|(site, _)| !(prog.is_loose(f.entry) && f.tails.contains_key(site)))
                .filter_map(|(_, t)| t.direct())
                .filter(|t| cset.contains(t) && !prog.is_loose(*t))
                .collect()
        };
        for &va in &compiled {
            let f = prog.func(va);
            let rg = code_ranges(&f);
            let cl: BTreeSet<usize> = in_c(&f).into_iter().map(|t| index[&t]).collect();
            chunks.push(format!(
                "static const uint32_t AOT_SYM(rg_{:08x})[] = {{{}}};",
                va,
                rg.iter().map(|(lo, hi)| format!("0x{:08x}u, 0x{:08x}u", lo, hi)).collect::<Vec<_>>().join(", ")
            ));
            if !cl.is_empty() {
                chunks.push(format!(
                    "static const uint16_t AOT_SYM(cl_{:08x})[] = {{{}}};",
                    va,
                    cl.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", ")
                ));
            }
        }
        let leaf = leaves(&prog, &prog.compiled);
        leaf_n = leaf.len();
        report.set("leaves", J::Int(leaf.len() as i64));
        report.set("x87", J::Int(compiled.iter().filter(|&&va| prog.func(va).insns.values().any(|i| x87_op(i).is_some())).count() as i64));
        let mut ex = J::obj();
        ex.set("functions", J::Int(compiled.iter().filter(|&&va| !prog.func(va).exits.is_empty()).count() as i64));
        ex.set("sites", J::Int(compiled.iter().map(|&va| prog.func(va).exits.len() as i64).sum()));
        ex.set("may_exit", J::Int(prog.may_exit.iter().filter(|v| cset.contains(v)).count() as i64));
        ex.set("callouts", J::Int(compiled.iter().map(|&va| prog.func(va).callouts.len() as i64).sum()));
        ex.set("plain", J::Int(prog.plain.iter().filter(|v| cset.contains(v)).count() as i64));
        let mut pw = J::obj();   // each plain one's refusal with its callouts
        let mut pl: Vec<u32> = prog.plain.iter().copied().filter(|v| cset.contains(v)).collect();
        pl.sort();
        for va in pl {
            pw.set(&format!("{:08x}", va), J::Str(prog.plain_why.get(&va).cloned().unwrap_or_default()));
        }
        ex.set("plain_why", pw);
        report.set("exits", ex);
        if prog.loose_on {
            // The loose functions, each with the refusal it would have had.
            let lj: Vec<(String, J)> = compiled
                .iter()
                .filter(|&&va| prog.is_loose(va))
                .map(|va| (format!("{:08x}", va), J::Str(prog.loose_why.get(va).cloned().unwrap_or_default())))
                .collect();
            report.set("loose", J::Obj(lj));
        }
        chunks.push("const AotFunc AOT_SYM(funcs)[] = {".to_string());
        for &va in &compiled {
            let (keep, hd) = heads.get(&va).cloned().unwrap_or((0, vec![0u8; 16]));
            let f = prog.func(va);
            let rg = code_ranges(&f);
            let ncl = in_c(&f).len();
            let name = names.get(&va).cloned().unwrap_or_else(|| format!("{:08x}", va));
            let hard = f.exits.iter().any(|s| !f.callouts.contains_key(s));
            chunks.push(format!(
                "    {{0x{:08x}u, AOT_SYM(fn_{:08x}), \"{}\", {}, {}, 0x{:02x}, 0x{:02x}, {}, {{{}}}, 0x{:08x}u, {}, {}, AOT_SYM(rg_{:08x}), {}, AOT_LEAF({}), AOT_EXIT({}, {}), AOT_EXIT({}, {}), {}}},",
                va,
                va,
                name,
                keep,
                if prog.is_loose(va) { 0xff } else { prog.summ[&va].pops.unwrap_or(0) },   // AOT_POPS_LOOSE
                mask(&sigs[&va].outs),
                mask(&sigs[&va].preserved),
                if roots.contains(&va) { 1 } else { 0 },
                hd.iter().map(|b| format!("0x{:02x}", b)).collect::<Vec<_>>().join(", "),
                code_hash(img, &rg),
                rg.len(),
                ncl,
                va,
                if ncl > 0 { format!("AOT_SYM(cl_{:08x})", va) } else { "0".to_string() },
                if leaf.contains(&va) { 1 } else { 0 },
                if f.exits.is_empty() { 0 } else { 1 },
                if prog.may_exit.contains(&va) { 1 } else { 0 },
                if hard { 1 } else { 0 },
                if prog.may_hard.contains(&va) { 1 } else { 0 },
                if f.insns.values().any(|i| x87_op(i).is_some()) { 1 } else { 0 }
            ));
        }
        chunks.push("    {0, 0, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0}".to_string());
        chunks.push("};".to_string());
    }
    // The resume entries' table: all eight registers in and out, esp after
    // the function's ret.
    for &(x, _, _) in &resumed {
        chunks.push(format!("AOT_FN void {}(AotRegs *R);", resume_name(x)));
        chunks.push(format!(
            "static uint32_t AOT_SYM(rs_{:08x})(uint32_t *r) {{ AotRegs t; for (int k = 0; k < 8; k++) t.r[k] = r[k]; {}(&t); for (int k = 0; k < 8; k++) r[k] = t.r[k]; return r[0]; }}",
            x,
            resume_name(x)
        ));
    }
    chunks.push("const AotResume AOT_SYM(resumes)[] = {".to_string());
    for &(x, va, sp) in &resumed {
        chunks.push(format!("    {{0x{:08x}u, 0x{:08x}u, 0x{:x}u, AOT_SYM(rs_{:08x})}},", x, va, sp as i32 as u32, x));
    }
    chunks.push("    {0, 0, 0, 0}".to_string());
    chunks.push("};".to_string());
    // The return sites' entries: reent_<va>, one per function, called with the site.
    let mut declared: HashSet<u32> = HashSet::new();
    for &(_, va, _) in &reents {
        if declared.insert(va) {
            chunks.push(format!("AOT_FN void {}(AotRegs *R, uint32_t at);", reent_name(va)));
        }
    }
    chunks.push("const AotReent AOT_SYM(reents)[] = {".to_string());
    for &(x, va, sp) in &reents {
        chunks.push(format!("    {{0x{:08x}u, 0x{:08x}u, 0x{:x}u, {}}},", x, va, sp as i32 as u32, reent_name(va)));
    }
    chunks.push("    {0, 0, 0, 0}".to_string());
    chunks.push("};".to_string());
    chunks.push("const AotTramp AOT_SYM(tramps)[] = {".to_string());
    for (va, (code, rel)) in &tramps {
        let mut r4 = rel.clone();
        r4.resize(4, 0);
        chunks.push(format!(
            "    {{0x{:08x}u, {}, {}, {{{}}}, {{{}}}}},",
            va,
            code.len(),
            rel.len(),
            r4.iter().map(|b| b.to_string()).collect::<Vec<_>>().join(", "),
            code.iter().map(|b| format!("0x{:02x}", b)).collect::<Vec<_>>().join(", ")
        ));
    }
    chunks.push("    {0, 0, 0, {0}, {0}}".to_string());
    chunks.push("};".to_string());
    let mut hk = J::obj();
    hk.set("hookable", J::Int(heads.len() as i64));
    hk.set("moved", J::Int(tramps.len() as i64));
    hk.set("not", J::Obj(why.iter().map(|(k, v)| (format!("{:08x}", k), J::Str(v.clone()))).collect()));
    report.set("hooks", hk);
    chunks.push("const AotRoot AOT_SYM(roots)[] = {".to_string());
    for &va in roots {
        if cset.contains(&va) {
            let (keep, hd) = root_head(img, va).expect("a root's head");
            let name = names.get(&va).cloned().unwrap_or_else(|| format!("{:08x}", va));
            chunks.push(format!(
                "    {{0x{:08x}u, {}, {}, \"{}\", {}, {}, {{{}}}}},",
                va,
                if mode_list.contains(&"sig") { format!("AOT_SYM(sig_root_{:08x})", va) } else { "0".to_string() },
                if mode_list.contains(&"ctx") { format!("AOT_SYM(ctx_root_{:08x})", va) } else { "0".to_string() },
                name,
                keep,
                prog.summ[&va].pops.expect("a root's pops"),
                hd.iter().map(|b| format!("0x{:02x}", b)).collect::<Vec<_>>().join(", ")
            ));
        }
    }
    chunks.push("    {0, 0, 0, 0, 0, 0, {0}}".to_string());
    chunks.push("};".to_string());
    std::fs::create_dir_all(outdir).expect("the output directory");
    if let Ok(rd) = std::fs::read_dir(outdir) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if let Some(mid) = n.strip_prefix("aot_spike_").and_then(|x| x.strip_suffix(".c")) {
                if !mid.is_empty() && mid.chars().all(|c| c.is_ascii_digit()) {
                    let _ = std::fs::remove_file(e.path());
                }
            }
        }
    }
    let p = std::path::Path::new(outdir);
    std::fs::write(p.join("aot_spike.c"), chunks.join("\n") + "\n").expect("aot_spike.c");
    for (k, out) in part_texts.iter().enumerate() {
        std::fs::write(p.join(format!("aot_spike_{:02}.c", k + 1)), out.join("\n") + "\n").expect("a part");
    }
    let mut insns = 0usize;
    let mut fj = Vec::new();
    for &va in &compiled {
        let f = prog.func(va);
        insns += f.n_insns;
        let falls: BTreeSet<u32> = calls_tails(&f)
            .filter_map(|(_, t)| t.direct())
            .filter(|t| !cset.contains(t) && !crate::program::intrinsics().contains_key(t))
            .collect();
        let nat = natives();
        let mut o = J::obj();
        o.set("insns", J::Int(f.n_insns as i64));
        o.set("bytes", J::Int(f.n_bytes as i64));
        let mut sj = J::obj();
        sj.set("in", int_list(&sigs[&va].ins));
        sj.set("out", int_list(&sigs[&va].outs));
        sj.set("preserved", int_list(&sigs[&va].preserved));
        o.set("sig", sj);
        o.set("ranges", J::List(code_ranges(&f).iter().map(|(a, b)| J::List(vec![J::Int(*a as i64), J::Int(*b as i64)])).collect()));
        o.set("switches", J::Int(f.switches.len() as i64));
        o.set("natives", J::List(falls.iter().filter(|t| nat.contains_key(t)).map(|t| J::Str(format!("{:08x}", t))).collect()));
        o.set("box86", J::List(falls.iter().filter(|t| !nat.contains_key(t)).map(|t| J::Str(format!("{:08x}", t))).collect()));
        fj.push((format!("{:08x}", va), o));
    }
    report.set("functions", J::Obj(fj));
    let mut trees = J::obj();
    for &r in roots {
        let mut seen: BTreeSet<u32> = BTreeSet::new();
        let mut work = vec![r];
        while let Some(v) = work.pop() {
            if seen.contains(&v) || !cset.contains(&v) {
                continue;
            }
            seen.insert(v);
            let g = prog.func(v);
            work.extend(calls_tails(&g).filter_map(|(_, t)| t.direct()));
        }
        trees.set(&format!("{:08x}", r), J::List(seen.iter().map(|v| J::Str(format!("{:08x}", v))).collect()));
    }
    report.set("trees", trees);
    let x86_bytes: u64 = compiled.iter().map(|&va| prog.func(va).n_bytes).sum();
    report.set("x86_insns", J::Int(insns as i64));
    report.set("x86_bytes", J::Int(x86_bytes as i64));
    let analysis_s = round3((t1 - t0).as_secs_f64());
    let emit_s = round3(t1.elapsed().as_secs_f64());
    report.set("analysis_s", J::Float(analysis_s));
    report.set("rounds", J::Int(rounds as i64));
    report.set("emit_s", J::Float(emit_s));
    report.set("closure", J::Int(closure as i64));
    std::fs::write(p.join("aot_spike.json"), report.dump()).expect("aot_spike.json");
    drop(ems);
    Compiled { prog, report, refused, x86_insns: insns, x86_bytes, leaves: leaf_n, rounds, analysis_s, emit_s }
}
