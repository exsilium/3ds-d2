//! The AOT recompiler (issue #144: tools/aot/recompile.py in Rust, its
//! output byte for byte): x86 functions of the user's Game.exe written out
//! as C for devkitARM's gcc (or the host test's).
//!
//!     recompile Game.exe --out build/aot/spike --root 67b850:PathSearch --compile 67aed0,...
//!     recompile Game.exe --census --modes sig --out build/aot/census [--hot log ...]
//!     recompile Game.exe --survey
//!     recompile Game.exe --all --modes sig --out build/aot/spike   (every function on --functions' list)
//!     ... --loose   (for a build without Box86: a function refused only for its stack compiled loose)
//!
//! The output is Game.exe in another form: it goes under build/ and into
//! the CIA (docs/aot.md's Distribution), never into the repository.

mod analysis;
mod census;
mod cs;
mod driver;
mod emit;
mod flags;
mod func;
mod image;
mod json;
mod omap;
mod ordlookup;
mod program;
mod x86;

use driver::parse_hex;
use func::Tgt;
use program::{intrinsics, natives, Program};
use std::collections::{BTreeSet, HashMap, HashSet};

/// A function the compiler leaves to Box86, and why.
#[derive(Debug, Clone)]
pub struct Refused(pub String);

struct Args {
    exe: String,
    out: String,
    root: Vec<String>,
    compile: Vec<String>,
    tree: Vec<String>,
    functions: String,
    modes: String,
    callees: usize,
    no_callouts: bool,
    loose: bool,
    survey: bool,
    census: bool,
    all: bool,
    hot: Vec<String>,
    explain: Option<String>,
    src_root: String,
}

fn usage() -> ! {
    eprintln!(
        "usage: recompile EXE [--out DIR] [--root VA[:NAME]] [--compile VA,...] [--tree VA:DEPTH] [--functions TSV] \
         [--modes sig,ctx] [--callees N] [--no-callouts] [--loose] [--survey] [--census] [--all] [--hot LOG] [--explain VA,...] [--src-root DIR]"
    );
    std::process::exit(2);
}

fn parse_args() -> Args {
    let mut a = Args {
        exe: String::new(),
        out: "build/aot/spike".to_string(),
        root: Vec::new(),
        compile: Vec::new(),
        tree: Vec::new(),
        functions: "third_party/d2d/docs/research/re/game-functions.tsv".to_string(),
        modes: "sig,ctx".to_string(),
        callees: 0,
        no_callouts: false,
        loose: false,
        survey: false,
        census: false,
        all: false,
        hot: Vec::new(),
        explain: None,
        src_root: ".".to_string(),
    };
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut k = 0;
    let mut exe = None;
    while k < argv.len() {
        let s = &argv[k];
        k += 1;
        if !s.starts_with("--") {
            if exe.is_some() {
                usage();
            }
            exe = Some(s.clone());
            continue;
        }
        let (name, inline) = match s.split_once('=') {
            Some((n, v)) => (n.to_string(), Some(v.to_string())),
            None => (s.clone(), None),
        };
        let flag = |n: &str| ["--no-callouts", "--loose", "--survey", "--census", "--all"].contains(&n);
        if flag(&name) {
            match name.as_str() {
                "--no-callouts" => a.no_callouts = true,
                "--loose" => a.loose = true,
                "--survey" => a.survey = true,
                "--all" => a.all = true,
                _ => a.census = true,
            }
            continue;
        }
        let v = match inline {
            Some(v) => v,
            None => {
                if k >= argv.len() {
                    usage();
                }
                k += 1;
                argv[k - 1].clone()
            }
        };
        match name.as_str() {
            "--out" => a.out = v,
            "--root" => a.root.push(v),
            "--compile" => a.compile.push(v),
            "--tree" => a.tree.push(v),
            "--functions" => a.functions = v,
            "--modes" => a.modes = v,
            "--callees" => a.callees = v.trim().parse().unwrap_or_else(|_| usage()),
            "--hot" => a.hot.push(v),
            "--explain" => a.explain = Some(v),
            "--src-root" => a.src_root = v,
            _ => usage(),
        }
    }
    a.exe = exe.unwrap_or_else(|| usage());
    a
}

fn parse_vas(s: &str) -> (Vec<u32>, HashMap<u32, String>) {
    let mut out = Vec::new();
    let mut names = HashMap::new();
    for part in s.split(',').filter(|p| !p.is_empty()) {
        let (a, n) = part.split_once(':').unwrap_or((part, ""));
        let v = parse_hex(a).unwrap_or_else(|_| panic!("not an address: {}", a));
        out.push(v);
        if !n.is_empty() {
            names.insert(v, n.to_string());
        }
    }
    (out, names)
}

fn modes(s: &str) -> Vec<&'static str> {
    s.split(',')
        .map(|m| match m {
            "sig" => "sig",
            "ctx" => "ctx",
            _ => panic!("no mode {}", m),
        })
        .collect()
}

fn main() {
    let a = parse_args();
    let callouts = !a.no_callouts;
    let img = image::Image::load(std::path::Path::new(&a.exe), std::path::Path::new(&a.src_root)).unwrap_or_else(|e| {
        eprintln!("recompile: {}", e);
        std::process::exit(1);
    });
    println!(
        "aot: {} natives' sites compiled as changed ({} bytes){}",
        img.sites.len(),
        img.sites.iter().map(|s| s.2).sum::<usize>(),
        if img.sites_skipped.is_empty() { String::new() } else { format!("; not: {}", img.sites_skipped.join(", ")) }
    );
    let mut starts = driver::load_starts(&a.functions);
    let site_calls = img.site_call_targets();
    starts.extend(site_calls.iter().copied());
    // An address inside an applied site is no function's entry any more
    // (issue #138: cinematics' play loop goes over the GDI renderer's three
    // video stubs; 6c7cd0 and 6c7ce0, still listed, cut it at its second call).
    let within_site = |v: u32| img.sites.iter().any(|&(_, s, n)| s < v && v < s + n as u32) && !site_calls.contains(&v);
    starts.retain(|v| !within_site(*v));
    if a.census {
        census::census(&img, &starts, &a.functions, &a.out, &a.hot, &modes(&a.modes), callouts, a.loose);
        return;
    }
    if a.survey {
        census::survey(&img, &starts, &a.functions, callouts);
        return;
    }
    let mut roots: Vec<u32> = Vec::new();
    let mut names: HashMap<u32, String> = HashMap::new();
    for r in &a.root {
        let (v, n) = parse_vas(r);
        roots.extend(v);
        names.extend(n);
    }
    let (mut extra, _) = parse_vas(&a.compile.join(","));
    for &t in &site_calls {
        if !extra.contains(&t) && !roots.contains(&t) {
            extra.push(t);
        }
    }
    if a.all {
        // d2d's whole list, as the census compiles it (#138: the lane on all of it).
        let mut l: Vec<u32> = census::load_list(&a.functions, None).keys().copied().collect();
        l.sort();
        let have: HashSet<u32> = extra.iter().chain(roots.iter()).copied().collect();
        extra.extend(l.into_iter().filter(|v| !have.contains(v)));
        // And the code pointers in their immediates (issue #138: callbacks the
        // game stores, 501650 into [841da0], 463d80 into ebx, not on d2d's
        // list): an address in .text on 16 bytes, after an int3 or nop pad,
        // and in no listed function's code.
        let prog = Program::new(&img, starts.clone(), natives(), callouts);
        let mut have: HashSet<u32> = extra.iter().chain(roots.iter()).copied().collect();
        let mut inside: HashSet<u32> = HashSet::new();
        let mut found: BTreeSet<u32> = BTreeSet::new();
        let mut insn_at: std::collections::BTreeMap<u32, u32> = std::collections::BTreeMap::new();
        for &va in &extra {
            let f = prog.func(va);
            inside.extend(f.insns.keys().filter(|&&a| a != va).copied());
            insn_at.extend(f.insns.iter().map(|(a, i)| (*a, i.size)));
            for i in f.insns.values() {
                if i.mnemonic.starts_with("call") || i.mnemonic.starts_with('j') {
                    continue;
                }
                for op in &i.ops {
                    if let cs::OpKind::Imm(v) = op.kind {
                        let t = v as u32;
                        if t >= img.text_lo && t < img.text_hi && !have.contains(&t) {
                            let pad = img.mem[(t - 1 - img.base) as usize];
                            // ... or, moved or pushed, right after a ret on any
                            // byte: the C runtime's functions have no pads
                            // (688c2b, its TlsAlloc wrapper, stored by 688fbb).
                            let ret = pad == 0xc3 || img.mem[(t - 3 - img.base) as usize] == 0xc2;
                            // ... or, moved or pushed, on 16 bytes after
                            // anything (a switch's table before it: 646be0,
                            // the callback 644a6e and six more push), where
                            // it decodes as a function that returns.
                            let stored = matches!(i.mnemonic.as_str(), "mov" | "push");
                            let decodes = || {
                                let g = prog.func(t);
                                g.refused.is_none() && !g.rets.is_empty()
                            };
                            if (t % 16 == 0 && (pad == 0xcc || pad == 0x90))
                                || (ret && stored)
                                || (t % 16 == 0 && stored && decodes())
                            {
                                found.insert(t);
                            }
                        }
                    }
                }
            }
        }
        // Not at, nor inside, an instruction of a listed function.
        found.retain(|t| {
            !inside.contains(t) && !insn_at.range(..*t).next_back().is_some_and(|(a, n)| a.wrapping_add(*n) > *t)
        });
        println!("aot: {} code pointers in the listed functions' immediates added", found.len());
        for t in found {
            have.insert(t);
            extra.push(t);
        }
        // And those in the game's tables, by the same rule: a relocated dword
        // in .rdata or .data (the renderer's table at 0x74c4a8 holds 6c8060,
        // which a compiled caller reached only as an exit).
        let mut found: BTreeSet<u32> = BTreeSet::new();
        for &(_, t) in &img.data_pointers {
            if t >= img.text_lo && t < img.text_hi && !have.contains(&t) && !inside.contains(&t) {
                let pad = img.mem[(t - 1 - img.base) as usize];
                // ... or right after a ret (6c9270, in the table at 0x74c56c,
                // follows 6c9260's ret 4 with no pad), on any byte: the C
                // runtime's initializers in its table at 0x6cc728 (6837b9)
                // have no pads. Not inside an instruction of a listed function.
                let ret = pad == 0xc3 || img.mem[(t - 3 - img.base) as usize] == 0xc2;
                let within = insn_at.range(..t).next_back().is_some_and(|(a, n)| a.wrapping_add(*n) > t);
                // ... or on 16 bytes after anything (a switch's table before
                // it: 676280, 6768c0, 58ea50), where it decodes as a function
                // that returns.
                let decodes = || {
                    let f = prog.func(t);
                    f.refused.is_none() && !f.rets.is_empty()
                };
                if (t % 16 == 0 && (pad == 0xcc || pad == 0x90)) || (ret && !within) || (t % 16 == 0 && !within && decodes()) {
                    found.insert(t);
                }
            }
        }
        println!("aot: {} code pointers in the game's data added", found.len());
        for t in found {
            have.insert(t);
            extra.push(t);
        }
        // And what all of these call or jump to directly that isn't on any
        // list (issue #138: 43eb30, 4408b0 and some 25 more were exits, with
        // nothing to exit to on CpuAot): the targets of direct calls are
        // functions' entries.
        let all: Vec<u32> = extra.iter().chain(roots.iter()).copied().collect();
        let mut callees: Vec<u32> = prog
            .closure(&all, 1 << 20)
            .into_iter()
            .filter(|t| !have.contains(t) && *t >= img.text_lo && *t < img.text_hi)
            .collect();
        callees.sort();
        println!("aot: {} direct callees of those added", callees.len());
        for t in callees {
            have.insert(t);
            extra.push(t);
        }
        // And the code pointers in the immediates of the functions found
        // since, with their direct callees, until none is new (44bbb0, pushed
        // by 44bc30, which the data's pointers found).
        let mut scanned: HashSet<u32> = HashSet::new();
        let mut more = 0usize;
        loop {
            let mut found: BTreeSet<u32> = BTreeSet::new();
            for &va in &extra {
                if !scanned.insert(va) {
                    continue;
                }
                let f = prog.func(va);
                inside.extend(f.insns.keys().filter(|&&a| a != va).copied());
                insn_at.extend(f.insns.iter().map(|(a, i)| (*a, i.size)));
                for i in f.insns.values() {
                    if i.mnemonic.starts_with("call") || i.mnemonic.starts_with('j') {
                        continue;
                    }
                    for op in &i.ops {
                        if let cs::OpKind::Imm(v) = op.kind {
                            let t = v as u32;
                            if t >= img.text_lo && t < img.text_hi && !have.contains(&t) {
                                let pad = img.mem[(t - 1 - img.base) as usize];
                                let ret = pad == 0xc3 || img.mem[(t - 3 - img.base) as usize] == 0xc2;
                                // ... or, moved or pushed, on 16 bytes after
                                // anything (a switch's table before it: 646be0,
                                // the callback 644a6e and six more push), where
                                // it decodes as a function that returns.
                                let stored = matches!(i.mnemonic.as_str(), "mov" | "push");
                                let decodes = || {
                                    let g = prog.func(t);
                                    g.refused.is_none() && !g.rets.is_empty()
                                };
                                if (t % 16 == 0 && (pad == 0xcc || pad == 0x90))
                                    || (ret && stored)
                                    || (t % 16 == 0 && stored && decodes())
                                {
                                    found.insert(t);
                                }
                            }
                        }
                    }
                }
            }
            found.retain(|t| {
                !inside.contains(t) && !insn_at.range(..*t).next_back().is_some_and(|(a, n)| a.wrapping_add(*n) > *t)
            });
            let mut add: Vec<u32> = found.into_iter().collect();
            let reach: Vec<u32> = prog
                .closure(&add, 1 << 20)
                .into_iter()
                .filter(|t| !have.contains(t) && *t >= img.text_lo && *t < img.text_hi)
                .collect();
            add.extend(reach);
            add.sort();
            add.dedup();
            if add.is_empty() {
                break;
            }
            more += add.len();
            for t in add {
                have.insert(t);
                extra.push(t);
            }
        }
        println!("aot: {} more from the immediates of those found", more);
        extra.retain(|v| !within_site(*v));
    }
    let intr = intrinsics();
    let nat = natives();
    for t in &a.tree {
        let (va, d) = t.split_once(':').unwrap_or((t, ""));
        let prog = Program::new(&img, starts.clone(), natives(), callouts);
        let v0 = parse_hex(va).expect("--tree address");
        let mut lvl: HashSet<u32> = [v0].into_iter().collect();
        let mut seen: HashSet<u32> = lvl.clone();
        let depth: usize = if d.is_empty() { 2 } else { d.parse().expect("--tree depth") };
        for _ in 0..depth {
            let mut nxt = HashSet::new();
            for &g in &lvl {
                let f = prog.func(g);
                if f.refused.is_some() {
                    continue;
                }
                for c in f.calls.values() {
                    if let Tgt::Direct(c) = c {
                        if !seen.contains(c) && !intr.contains_key(c) && !nat.contains_key(c) {
                            nxt.insert(*c);
                        }
                    }
                }
            }
            seen.extend(nxt.iter().copied());
            lvl = nxt;
        }
        let have: HashSet<u32> = extra.iter().copied().collect();
        let add: BTreeSet<u32> = seen.into_iter().filter(|v| !have.contains(v)).collect();
        extra.extend(add);
    }
    if a.callees > 0 {
        let prog = Program::new(&img, starts.clone(), natives(), callouts);
        let mut seen: HashSet<u32> = extra.iter().chain(roots.iter()).copied().collect();
        let mut lvl = seen.clone();
        for _ in 0..a.callees {
            let mut nxt = HashSet::new();
            let mut l: Vec<u32> = lvl.iter().copied().collect();
            l.sort();
            for g in l {
                let f = prog.func(g);
                if f.refused.is_some() {
                    continue;
                }
                for c in f.calls.values().chain(f.tails.values()) {
                    if let Tgt::Direct(c) = c {
                        if !seen.contains(c) && !intr.contains_key(c) && !nat.contains_key(c) {
                            nxt.insert(*c);
                        }
                    }
                }
            }
            seen.extend(nxt.iter().copied());
            lvl = nxt;
        }
        let have: HashSet<u32> = extra.iter().chain(roots.iter()).copied().collect();
        let add: BTreeSet<u32> = seen.into_iter().filter(|v| !have.contains(v)).collect();
        println!("aot: {} callees added (--callees {})", add.len(), a.callees);
        extra.extend(add);
    }
    if let Some(ex) = &a.explain {
        let mut want = roots.clone();
        want.extend(extra.iter().copied().filter(|v| !roots.contains(v)));
        census::explain(&img, &starts, &want, &parse_vas(ex).0, callouts);
        return;
    }
    let rep = driver::compile_set(&img, &starts, &roots, &extra, &modes(&a.modes), &a.out, &names, callouts, a.loose);
    let get = |k: &str| -> i64 {
        match rep.report.get("exits").and_then(|e| e.get(k)) {
            Some(json::J::Int(n)) => *n,
            _ => 0,
        }
    };
    println!(
        "aot: {} functions compiled ({} x86 instructions, {} bytes; {} never call Box86; {} hand Box86 the rest of a call at {} sites, {} with their callers; {} of the sites callouts, plain exits in {} functions), {} left to Box86, in {}",
        rep.prog.compiled.len(),
        rep.x86_insns,
        rep.x86_bytes,
        rep.leaves,
        get("functions"),
        get("sites"),
        get("may_exit"),
        get("callouts"),
        get("plain"),
        rep.refused.len(),
        a.out
    );
    if a.loose {
        println!(
            "aot: {} of them loose (--loose): reached through the dispatcher, their rets and tail jumps its to follow",
            rep.prog.compiled.iter().filter(|&&va| rep.prog.is_loose(va)).count()
        );
    }
    if let Some(pt) = rep.report.get("parts") {
        let n = |k: &str| match pt.get(k) {
            Some(json::J::Int(n)) => n.to_string(),
            Some(json::J::Str(s)) => s.clone(),
            _ => "?".to_string(),
        };
        println!(
            "aot: the C in {} parts (AOT_PART_MB, by {}); calls between compiled functions: {} in their part, {} across",
            n("files"),
            n("order"),
            n("calls_local"),
            n("calls_across")
        );
    }
    let mut left: Vec<(&u32, &String)> = rep.refused.iter().collect();
    left.sort();
    for (k, v) in left {
        println!("  left to Box86: {:08x}: {}", k, v);
    }
}
