//! The census of d2d's whole list (issue #135), the survey and explain
//! (recompile.py's census, survey, x87_depth, explain, refusal_kind).

use crate::analysis::Analysis;
use crate::driver::{compile_set, parse_hex};
use crate::flags::Flags;
use crate::func::{Func, Tgt};
use crate::image::Image;
use crate::json::J;
use crate::omap::OMap;
use crate::program::{intrinsics, natives, Program, ERROR_PATH_FUNCS, NORETURN_FUNCS};
use crate::x86::*;
use std::collections::{BTreeSet, HashMap, HashSet};

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// re.sub(r"\b[0-9a-f]{8}\b", "X", s)
pub fn hex_to_x(s: &str) -> String {
    let c: Vec<char> = s.chars().collect();
    let mut out = String::new();
    let mut i = 0;
    while i < c.len() {
        let hex = |k: usize| k < c.len() && (c[k].is_ascii_digit() || ('a'..='f').contains(&c[k]));
        if (i == 0 || !is_word(c[i - 1])) && (0..8).all(|k| hex(i + k)) && (i + 8 == c.len() || !is_word(c[i + 8])) {
            out.push('X');
            i += 8;
            continue;
        }
        out.push(c[i]);
        i += 1;
    }
    out
}

/// A refusal's cause without its addresses.
pub fn refusal_kind(reason: &str) -> String {
    let mut r = hex_to_x(reason);
    if r.starts_with("x87 (") {
        return r.replace(" at X", "");
    }
    // ^(\S+)( \S.*)? at X$
    if let Some(body) = r.strip_suffix(" at X") {
        let tok_end = body.find(char::is_whitespace).unwrap_or(body.len());
        let (g1, rest) = body.split_at(tok_end);
        let ok = !g1.is_empty()
            && (rest.is_empty()
                || (rest.starts_with(' ') && rest[1..].chars().next().is_some_and(|c| !c.is_whitespace()) && !rest.contains('\n')));
        let skip = ["a ", "indirect", "branch", "tail", "flags", "direction", "esp", "code", "no ", "stack", "imul", "an ", "its "];
        if ok && !skip.iter().any(|p| r.starts_with(p)) {
            let g2 = if rest.is_empty() { None } else { Some(rest) };
            if ["rep", "repz", "repe", "repne", "repnz", "lock"].contains(&g1) && g2.is_some() {
                return format!("instruction {} {}", g1, g2.unwrap().split_whitespace().next().unwrap());
            }
            if g1.starts_with('f') {
                return format!("x87 ({})", g1);
            }
            if g2.is_some() {
                return format!("segment or system register ({})", g1);
            }
            return format!("instruction {}", g1);
        }
    }
    if r.starts_with("segment ") {
        return format!("segment {}", r.split_whitespace().nth(1).unwrap_or(""));
    }
    // imul of \d+ bytes
    loop {
        let Some(k) = r.find("imul of ") else { break };
        let after = &r[k + 8..];
        let d = after.chars().take_while(|c| c.is_ascii_digit()).count();
        if d > 0 && after[d..].starts_with(" bytes") {
            r = format!("{}imul of 8 or 16 bits{}", &r[..k], &after[d + 6..]);
        } else {
            break;
        }
    }
    if let Some(rest) = r.strip_prefix("flags of ") {
        let t = rest.find(char::is_whitespace).unwrap_or(rest.len());
        if t > 0 && rest[t..].starts_with(" read") {
            r = format!("flags of an instruction read{}", &rest[t + 5..]);
        }
    }
    {
        let t = r.find(char::is_whitespace).unwrap_or(r.len());
        if t > 0 && r[t..].starts_with(" reads flags") {
            r = format!("an instruction reads flags{}", &r[t + 12..]);
        }
    }
    if let Some(k) = r.find("tail call at X: pops ") {
        r = format!("{}tail call at X: pops differ from the target's", &r[..k]);
    }
    if let Some(k) = r.find("tail call through a pointer at X: pops ") {
        r = format!("{}tail call through a pointer: pops not known", &r[..k]);
    }
    if r.starts_with("a thunk through a pointer at X ") {
        r = "a thunk through a pointer whose pops aren't known".to_string();
    }
    if let Some(rest) = r.strip_prefix("its flags are read after the call at X") {
        r = format!("its flags are read after a call to it{}", rest);
    }
    if let Some(k) = r.rfind("is the entry's ") {
        let tail = &r[k + 15..];
        if tail.len() > 1 && (tail.starts_with('+') || tail.starts_with('-')) && tail[1..].chars().all(|c| c.is_ascii_digit()) {
            r = format!("{}is the entry's +n", &r[..k]);
        }
    }
    r.replace(" at X", "").replace(" (X)", "")
}

/// x86 bytes executed per function (d2d's starts and sizes) from the lane's [hotprof] lines.
pub fn load_hot(logs: &[String], sizes: &OMap<u32, i64>) -> HashMap<Option<u32>, i64> {
    let mut order: Vec<u32> = sizes.keys().copied().collect();
    order.sort();
    let mut hot: HashMap<Option<u32>, i64> = HashMap::new();
    for log in logs {
        let Ok(b) = std::fs::read(log) else { continue };
        let text = String::from_utf8_lossy(&b).into_owned();
        for line in text.split(['\n', '\r']) {
            if let Some((addr, n)) = hot_block(line) {
                let k = order.partition_point(|&x| x <= addr) as i64 - 1;
                let f = if k >= 0 && (addr as i64) < order[k as usize] as i64 + sizes[&order[k as usize]].max(1) {
                    Some(order[k as usize])
                } else {
                    None
                };
                *hot.entry(f).or_insert(0) += n;
            }
        }
    }
    hot
}

/// re.search(r"\[hotprof\]\s+\d+ ([0-9a-f]{8}) \+(\d+)\s+x(\d+)", line)
fn hot_block(line: &str) -> Option<(u32, i64)> {
    let b = line.as_bytes();
    let mut from = 0;
    while let Some(k) = line[from..].find("[hotprof]") {
        let s = from + k + 9;
        from = from + k + 1;
        let mut p = s;
        let ws = |p: &mut usize| {
            let st = *p;
            while *p < b.len() && (b[*p] as char).is_ascii_whitespace() {
                *p += 1;
            }
            *p > st
        };
        let digits = |p: &mut usize| {
            let st = *p;
            while *p < b.len() && b[*p].is_ascii_digit() {
                *p += 1;
            }
            (*p > st).then(|| line[st..*p].to_string())
        };
        if !ws(&mut p) || digits(&mut p).is_none() || p >= b.len() || b[p] != b' ' {
            continue;
        }
        p += 1;
        if p + 8 > b.len() || !b[p..p + 8].iter().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(c)) {
            continue;
        }
        let addr = u32::from_str_radix(&line[p..p + 8], 16).ok()?;
        p += 8;
        if p + 2 > b.len() || &b[p..p + 2] != b" +" {
            continue;
        }
        p += 2;
        let Some(sz) = digits(&mut p) else { continue };
        if !ws(&mut p) || p >= b.len() || b[p] != b'x' {
            continue;
        }
        p += 1;
        let Some(cnt) = digits(&mut p) else { continue };
        return Some((addr, sz.parse::<i64>().ok()? * cnt.parse::<i64>().ok()?));
    }
    None
}

/// d2d's list: start -> size, and names, in its order.
pub fn load_list(tsv: &str, names: Option<&mut HashMap<u32, String>>) -> OMap<u32, i64> {
    let mut sizes = OMap::new();
    let mut nm = HashMap::new();
    let text = std::fs::read_to_string(tsv).unwrap_or_else(|e| panic!("{}: {}", tsv, e));
    for line in text.split_inclusive('\n').skip(1) {
        let a: Vec<&str> = line.trim_end_matches('\n').split('\t').collect();
        let (Some(a0), Some(a2)) = (a.first(), a.get(2)) else { continue };
        let (Ok(va), Ok(sz)) = (parse_hex(a0), a2.trim().parse::<i64>()) else { continue };
        sizes.insert(va, sz);
        nm.insert(va, a[1].to_string());
    }
    if let Some(n) = names {
        *n = nm;
    }
    sizes
}

#[derive(Default)]
struct Row {
    functions: i64,
    sites: i64,
    bytes: i64,
    hot: i64,
    examples: Vec<u32>,
    seen: HashSet<u32>,
}

fn add(d: &mut OMap<String, Row>, key: String, va: u32, sizes: &OMap<u32, i64>, hot: &HashMap<Option<u32>, i64>) {
    if !d.contains_key(&key) {
        d.insert(key.clone(), Row::default());
    }
    let r = d.get_mut(&key).unwrap();
    r.sites += 1;
    if !r.seen.insert(va) {
        return;
    }
    r.functions += 1;
    r.bytes += sizes.get(&va).copied().unwrap_or(0);
    r.hot += hot.get(&Some(va)).copied().unwrap_or(0);
    if r.examples.len() < 8 {
        r.examples.push(va);
    }
}

fn pct(n: i64, d: i64) -> String {
    if d == 0 {
        return "-".to_string();
    }
    format!("{:.1}%", 100.0 * n as f64 / d as f64)
}

fn rows_json(d: &OMap<String, Row>) -> J {
    J::Obj(
        d.iter()
            .map(|(k, r)| {
                let mut o = J::obj();
                o.set("functions", J::Int(r.functions));
                o.set("sites", J::Int(r.sites));
                o.set("bytes", J::Int(r.bytes));
                o.set("hot", J::Int(r.hot));
                o.set("examples", J::List(r.examples.iter().map(|v| J::Str(format!("{:08x}", v))).collect()));
                (k.clone(), o)
            })
            .collect(),
    )
}

/// Counter.most_common: by count, ties in first-seen order.
fn most_common(c: &OMap<String, i64>) -> Vec<(String, i64)> {
    let mut v: Vec<(String, i64)> = c.iter().map(|(k, n)| (k.clone(), *n)).collect();
    v.sort_by(|a, b| b.1.cmp(&a.1));
    v
}

pub fn census(img: &Image, starts: &HashSet<u32>, tsv: &str, outdir: &str, hot_logs: &[String], mode_list: &[&'static str], callouts: bool, loose: bool) {
    let mut fnames = HashMap::new();
    let sizes = load_list(tsv, Some(&mut fnames));
    let hot = if hot_logs.is_empty() { HashMap::new() } else { load_hot(hot_logs, &sizes) };
    let hot_sum: i64 = hot.values().sum();
    let hot_total = if hot_sum == 0 { 1 } else { hot_sum };
    let mut extra: Vec<u32> = sizes.keys().copied().collect();
    extra.sort();
    let rep = compile_set(img, starts, &[], &extra, mode_list, outdir, &fnames, callouts, loose);
    let prog = &rep.prog;
    let compiled: BTreeSet<u32> = prog.compiled.clone();
    let refused = &rep.refused;
    let mut by_refusal: OMap<String, Row> = OMap::new();
    for (va, why) in refused.iter() {
        add(&mut by_refusal, refusal_kind(why), *va, &sizes, &hot);
    }
    let mut kinds: OMap<String, Row> = OMap::new();
    for &va in sizes.keys() {
        let f = prog.func(va);
        if f.refused.is_some() && f.insns.is_empty() {
            continue;
        }
        let mut seen: BTreeSet<String> = BTreeSet::new();
        for i in f.insns.values() {
            let m = i.mnemonic.as_str();
            if m.starts_with('f') {
                seen.insert("x87".to_string());
            }
            if m.starts_with("rep") {
                seen.insert("rep string".to_string());
            }
            if ["cpuid", "rdtsc", "int", "int3", "in", "out", "sti", "cli", "hlt"].contains(&m) {
                seen.insert(format!("instruction {}", m));
            }
            if i.op_str.contains("fs:") {
                seen.insert("fs: (thread block)".to_string());
            }
        }
        for k in seen {
            add(&mut kinds, k, va, &sizes, &hot);
        }
    }
    let base_intr = intrinsics();
    let nat = natives();
    let mut exits: OMap<String, Row> = OMap::new();
    let mut nested: OMap<String, Row> = OMap::new();
    let mut imports: OMap<String, i64> = OMap::new();
    for &va in &compiled {
        let f = prog.func(va);
        for &site in &f.exits {
            let t = f.calls.get(&site).or(f.tails.get(&site)).copied().unwrap_or(Tgt::Indirect);
            let how = if f.callouts.contains_key(&site) { "callout" } else { "plain exit" };
            let k = if f.tails.contains_key(&site) && t.is_pointer() {
                format!("tail call through a pointer ({})", how)
            } else {
                match t {
                    Tgt::Indirect => format!("indirect call ({})", how),
                    Tgt::Import(slot) => {
                        let (dll, name) = &img.imports[&slot];
                        let key = format!("{}!{}", dll, name);
                        let n = imports.get(&key).copied().unwrap_or(0);
                        imports.insert(key, n + 1);
                        if f.callouts.contains_key(&site) { "import (callout)" } else { "import (plain exit)" }.to_string()
                    }
                    Tgt::Direct(_) => "call to a function left to Box86 that may block".to_string(),
                }
            };
            add(&mut exits, k, va, &sizes, &hot);
        }
        for (site, t) in f.calls.iter().chain(f.tails.iter()) {
            let Tgt::Direct(t) = *t else { continue };
            if f.exits.contains(site) || compiled.contains(&t) || base_intr.contains_key(&t) {
                continue;
            }
            let k = if nat.contains_key(&t) {
                "a native of ours".to_string()
            } else if ERROR_PATH_FUNCS.contains(&t) || NORETURN_FUNCS.contains(&t) {
                "an error path or exit (no return)".to_string()
            } else if let Some(w) = refused.get(&t) {
                format!("refused callee: {}", refusal_kind(w))
            } else {
                "callee not on the list".to_string()
            };
            add(&mut nested, k, va, &sizes, &hot);
        }
    }
    let total_b: i64 = sizes.values().sum();
    let comp_b: i64 = compiled.iter().map(|va| sizes.get(va).copied().unwrap_or(0)).sum();
    let comp_hot: i64 = compiled.iter().map(|va| hot.get(&Some(*va)).copied().unwrap_or(0)).sum();
    let may_exit = rep.report.get("exits").and_then(|e| e.get("may_exit")).map(|v| if let J::Int(n) = v { *n } else { 0 }).unwrap_or(0);
    let hot_names: Vec<String> = hot_logs
        .iter()
        .map(|x| std::path::Path::new(x).file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default())
        .collect();
    let mut out = J::obj();
    out.set("functions", J::Int(sizes.len() as i64));
    out.set("bytes", J::Int(total_b));
    out.set("hot_listed", J::Int(hot_sum));
    let mut cj = J::obj();
    cj.set("functions", J::Int(compiled.len() as i64));
    cj.set("bytes", J::Int(comp_b));
    cj.set("hot", J::Int(comp_hot));
    cj.set("x86_bytes", J::Int(rep.x86_bytes as i64));
    cj.set("may_exit", J::Int(may_exit));
    cj.set("leaves", J::Int(rep.leaves as i64));
    out.set("compiled", cj);
    out.set("refused", rows_json(&by_refusal));
    out.set("kinds", rows_json(&kinds));
    out.set("exits", rows_json(&exits));
    out.set("nested", rows_json(&nested));
    out.set("imports", J::Obj(most_common(&imports).into_iter().map(|(k, n)| (k, J::Int(n))).collect()));
    out.set("hot_logs", J::List(hot_names.iter().map(|s| J::Str(s.clone())).collect()));
    out.set("rounds", J::Int(rep.rounds as i64));
    out.set("analysis_s", J::Float(rep.analysis_s));
    out.set("emit_s", J::Float(rep.emit_s));
    out.set("hot_unlisted", J::Int(hot.get(&None).copied().unwrap_or(0)));
    std::fs::write(std::path::Path::new(outdir).join("census.json"), out.dump()).expect("census.json");
    let mut lines = vec![
        format!(
            "census (#135): {} functions on d2d's list, {} bytes; the lane's hot bytes from {}",
            sizes.len(),
            total_b,
            if hot_names.is_empty() { "no logs".to_string() } else { hot_names.join(", ") }
        ),
        format!(
            "compiled: {} functions ({}), {} bytes ({}), {} of the hot bytes; {} may hand a call to Box86",
            compiled.len(),
            pct(compiled.len() as i64, sizes.len() as i64),
            comp_b,
            pct(comp_b, total_b),
            pct(comp_hot, hot_total),
            may_exit
        ),
    ];
    for (title, d) in [
        ("refused, by first cause", &by_refusal),
        ("instruction kinds present", &kinds),
        ("exits in compiled functions (sites)", &exits),
        ("calls from compiled code run nested by Box86 (sites)", &nested),
    ] {
        lines.push(String::new());
        lines.push(format!("{}:", title));
        lines.push(format!("  {:>6} {:>6} {:>9} {:>7} {:>7}  {}", "funcs", "sites", "bytes", "bytes%", "hot%", "cause"));
        let mut items: Vec<(&String, &Row)> = d.iter().collect();
        items.sort_by(|a, b| (-a.1.hot, -a.1.bytes, -a.1.functions).cmp(&(-b.1.hot, -b.1.bytes, -b.1.functions)));
        for (k, r) in items {
            lines.push(format!(
                "  {:>6} {:>6} {:>9} {:>7} {:>7}  {}  [{}]",
                r.functions,
                r.sites,
                r.bytes,
                pct(r.bytes, total_b),
                pct(r.hot, hot_total),
                k,
                r.examples.iter().take(4).map(|v| format!("{:08x}", v)).collect::<Vec<_>>().join(" ")
            ));
        }
    }
    lines.push(String::new());
    lines.push(format!(
        "imports called from compiled code (sites): {}",
        most_common(&imports).iter().take(40).map(|(k, n)| format!("{} {}", k, n)).collect::<Vec<_>>().join(", ")
    ));
    let text = lines.join("\n") + "\n";
    std::fs::write(std::path::Path::new(outdir).join("census.txt"), &text).expect("census.txt");
    println!("{}", text);
}

const X87_PUSH: [&str; 9] = ["fld", "fild", "fld1", "fldz", "fldpi", "fldl2e", "fldln2", "fldlg2", "fldl2t"];
const X87_POP1: [&str; 14] =
    ["fstp", "fistp", "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp", "fcomp", "ficomp", "fucomp", "fcomip", "fucomip", "fisttp"];
const X87_POP2: [&str; 2] = ["fcompp", "fucompp"];

/// (state, delta): state 0 no x87, 1 the depth known everywhere, None not;
/// delta the depth at its rets.
pub fn x87_depth(f: &Func, prog: &Program, deltas: &HashMap<u32, Option<i64>>) -> (Option<u8>, Option<i64>) {
    let has_x87 = f.insns.values().any(|i| i.mnemonic.starts_with('f'));
    if !has_x87 {
        return (Some(0), Some(0));
    }
    let mut depth: HashMap<u32, i64> = HashMap::new();
    depth.insert(f.entry, 0);
    let mut work = vec![f.entry];
    let mut at_ret: BTreeSet<i64> = BTreeSet::new();
    while let Some(b) = work.pop() {
        let mut d = depth[&b];
        let mut va = b;
        loop {
            let i = &f.insns[&va];
            let m = i.mnemonic.as_str();
            if X87_PUSH.contains(&m) {
                d += 1;
            } else if X87_POP2.contains(&m) {
                d -= 2;
            } else if X87_POP1.contains(&m) || (m.starts_with('f') && m.ends_with('p') && !X87_POP2.contains(&m)) {
                d -= 1;
            } else if m == "fninit" || m == "finit" {
                d = 0;
            } else if let Some(t) = f.calls.get(&va) {
                if let Tgt::Direct(t) = t {
                    if !prog.intrinsics.contains_key(t) {
                        match deltas.get(t).copied().unwrap_or(Some(0)) {
                            None => return (None, None),
                            Some(dt) => d += dt,
                        }
                    }
                }
            }
            if f.rets.contains_key(&va) {
                at_ret.insert(d);
            }
            if !(0..=8).contains(&d) {
                return (None, None);
            }
            let succ = &f.succ[&va];
            let nxt = va.wrapping_add(i.size);
            if succ.len() == 1 && succ[0] == nxt && !f.leaders.contains(&nxt) {
                va = nxt;
                continue;
            }
            for &t in succ {
                match depth.get(&t) {
                    None => {
                        depth.insert(t, d);
                        work.push(t);
                    }
                    Some(&x) if x != d => return (None, None),
                    _ => {}
                }
            }
            break;
        }
    }
    let delta = if at_ret.len() == 1 { at_ret.iter().next().copied() } else if at_ret.is_empty() { Some(0) } else { None };
    (Some(1), delta)
}

pub fn survey(img: &Image, starts: &HashSet<u32>, tsv: &str, callouts: bool) {
    let sizes = load_list(tsv, None);
    let prog = Program::new(img, starts.clone(), HashMap::new(), callouts);
    let mut res: OMap<String, i64> = OMap::new();
    let mut why: OMap<String, i64> = OMap::new();
    let mut x87_unknown: Vec<u32> = Vec::new();
    let mut flags_live: Vec<u32> = Vec::new();
    let inc = |m: &mut OMap<String, i64>, k: &str, n: i64| {
        let v = m.get(&k.to_string()).copied().unwrap_or(0);
        m.insert(k.to_string(), v + n);
    };
    let mut order: Vec<u32> = sizes.keys().copied().collect();
    order.sort();
    for &va in &order {
        let f = prog.func(va);
        if let Some(r) = &f.refused {
            inc(&mut res, "cfg_refused", 1);
            let a = r.split(" at ").next().unwrap();
            inc(&mut why, a.split(" %").next().unwrap(), 1);
            continue;
        }
        inc(&mut res, "functions", 1);
        inc(&mut res, "switches_bounded", f.switches.len() as i64);
        match Flags::new(&f, &prog) {
            Ok(_) => inc(&mut res, "flags_ok", 1),
            Err(e) => {
                if e.0.contains("from a call or the entry") {
                    inc(&mut res, "flags_live_at_call_or_ret", 1);
                    flags_live.push(va);
                } else {
                    inc(&mut res, "flags_other", 1);
                    inc(&mut why, &format!("flags: {}", e.0.split(" at ").next().unwrap()), 1);
                }
            }
        }
    }
    let mut deltas: HashMap<u32, Option<i64>> = HashMap::new();
    for _ in 0..20 {
        let mut changed = false;
        for &va in &order {
            let f = prog.func(va);
            if f.refused.is_some() {
                continue;
            }
            let (_, dl) = x87_depth(&f, &prog, &deltas);
            if deltas.get(&va).copied().unwrap_or(Some(0)) != dl {
                deltas.insert(va, dl);
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    for &va in &order {
        let f = prog.func(va);
        if f.refused.is_some() {
            continue;
        }
        let (st, _) = x87_depth(&f, &prog, &deltas);
        match st {
            None => x87_unknown.push(va),
            Some(1) => inc(&mut res, "x87_known", 1),
            _ => {}
        }
    }
    let unb = if let Some(n) = why.get(&"a switch".to_string()) {
        *n
    } else {
        why.iter().filter(|(k, _)| k.starts_with("a switch")).map(|(_, v)| *v).sum()
    };
    res.insert("switches_unbounded".to_string(), unb);
    res.insert("x87_unknown".to_string(), x87_unknown.len() as i64);
    let mut keys: Vec<&String> = res.keys().collect();
    keys.sort();
    for k in keys {
        println!("{:<28} {}", k, res[k]);
    }
    println!("flags live at a call or ret: {}", flags_live.iter().map(|v| format!("{:08x}", v)).collect::<Vec<_>>().join(" "));
    println!(
        "x87 depth not known: {} {}",
        x87_unknown.iter().take(40).map(|v| format!("{:08x}", v)).collect::<Vec<_>>().join(" "),
        if x87_unknown.len() > 40 { "..." } else { "" }
    );
    println!("refusals:");
    for (k, v) in most_common(&why).into_iter().take(20) {
        println!("  {:>6}  {}", v, k);
    }
}

fn py_set(v: &[u8]) -> String {
    format!("[{}]", v.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", "))
}

pub fn explain(img: &Image, starts: &HashSet<u32>, want: &[u32], vas: &[u32], callouts: bool) {
    let mut all = starts.clone();
    all.extend(want.iter().copied());
    all.extend(vas.iter().copied());
    let mut prog = Program::new(img, all, natives(), callouts);
    let mut roots: Vec<u32> = want.to_vec();
    roots.extend(vas.iter().copied());
    let mut reach = prog.closure(&roots, 30000);
    reach.extend(want.iter().copied());
    reach.extend(vas.iter().copied());
    for &v in want {
        if prog.func(v).refused.is_none() {
            prog.compiled.insert(v);
        }
    }
    let mut rs: Vec<u32> = reach.into_iter().collect();
    rs.sort();
    prog.summarize(&rs, 50);
    let mut kinds: OMap<String, i64> = OMap::new();
    let bump = |kinds: &mut OMap<String, i64>, k: &str| {
        let mut s: String = String::new();
        let c: Vec<char> = k.chars().collect();
        let mut i = 0;
        while i < c.len() {
            if i + 8 <= c.len() && (0..8).all(|j| c[i + j].is_ascii_digit() || ('a'..='f').contains(&c[i + j])) {
                i += 8;
                continue;
            }
            s.push(c[i]);
            i += 1;
        }
        let key: String = s.chars().take(60).collect();
        let n = kinds.get(&key).copied().unwrap_or(0);
        kinds.insert(key, n + 1);
    };
    for &va in vas {
        let f = prog.func(va);
        if let Some(sm) = prog.summ.get(&va) {
            println!(
                "{:08x}: summary reads {}, preserved {}, pops {}{}; {} rets{}",
                va,
                regs_of(sm.reads).iter().map(|&r| REGS[r as usize]).collect::<Vec<_>>().join(","),
                regs_of(sm.preserved).iter().map(|&r| REGS[r as usize]).collect::<Vec<_>>().join(","),
                sm.pops.map(|p| p.to_string()).unwrap_or("None".to_string()),
                if sm.known { "" } else { " (not known)" },
                f.rets.len(),
                if prog.noreturn_funcs.contains(&va) { ", never returns" } else { "" }
            );
        }
        if let Some(r) = &f.refused {
            println!("{:08x}: {}", va, r);
            continue;
        }
        let a = match Analysis::new(&prog, f.clone(), None, true) {
            Ok(a) => a,
            Err(e) => {
                println!("{:08x}: {}", va, e.0);
                continue;
            }
        };
        if a.esp_ok {
            println!("{:08x}: esp known at every ret", va);
            continue;
        }
        let mut sites: Vec<String> = Vec::new();
        let mut calls: Vec<(u32, Tgt)> = f.calls.iter().map(|(k, v)| (*k, *v)).collect();
        calls.sort();
        for &(site, t) in &calls {
            if f.noreturn.contains(&site) || prog.callee_summary(t).pops.is_some() {
                continue;
            }
            let k = match t {
                Tgt::Indirect => "indirect".to_string(),
                Tgt::Import(slot) => format!("import {}, pops not known", slot),
                Tgt::Direct(t) => {
                    let g = prog.func(t);
                    let rets: BTreeSet<i64> = if g.refused.is_none() { g.rets.values().copied().collect() } else { BTreeSet::new() };
                    if let Some(r) = &g.refused {
                        format!("{:08x} refused: {}", t, r)
                    } else if rets.len() > 1 {
                        format!("{:08x} rets pop [{}]", t, rets.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", "))
                    } else {
                        format!("{:08x}: {}", t, prog.why.get(&t).cloned().unwrap_or("summary not known".to_string()))
                    }
                }
            };
            sites.push(format!("{:08x} {}", site, k));
            bump(&mut kinds, &k);
        }
        for &(site, t) in &calls {
            let Tgt::Direct(td) = t else { continue };
            if prog.compiled.contains(&td) && !has(prog.callee_summary(t).preserved, EBP) && !f.noreturn.contains(&site) {
                let k = format!(
                    "{:08x} compiled, ebp not shown preserved ({})",
                    td,
                    prog.why.get(&td).cloned().unwrap_or("its analysis".to_string())
                );
                bump(&mut kinds, &k);
                sites.push(format!("{:08x} {}", site, k));
            }
        }
        println!("{:08x}: esp not known at a ret; {} calls to blame:", va, sites.len());
        for x in sites.iter().take(12) {
            println!("    {}", x);
        }
    }
    let _ = py_set;
    println!("kinds: {}", most_common(&kinds).iter().map(|(k, n)| format!("{} {}", k, n)).collect::<Vec<_>>().join(", "));
}
