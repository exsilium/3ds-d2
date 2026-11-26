//! Game.exe as mapped (pefile's get_memory_mapped_image), its imports and
//! what each one's ret pops, and its instructions (recompile.py's Image).

use crate::cs::{Cs, Insn};
use crate::omap::OMap;
use crate::Refused;
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::rc::Rc;

pub struct Image {
    pub base: u32,
    pub mem: Vec<u8>,
    pub text_lo: u32,
    pub text_hi: u32,
    /// IAT slot -> (dll, name or #ordinal), in pefile's order.
    pub imports: OMap<u32, (String, String)>,
    pub import_pops: HashMap<u32, Option<i64>>,
    /// Where our natives hand a call back to the game's code (issue #138):
    /// d2_native.cpp's resume_original(c, 0x..., ...), in order.
    pub resumes: Vec<u32>,
    /// The natives' entries as the code compiled has them (issue #138,
    /// src/game/native_sites_table.h): imports named "native!<name>" at
    /// 0xfee00000 + 16 k, which a native's sites call.
    pub native_slots: HashSet<u32>,
    /// The natives' sites applied to `mem`: (group, va, bytes), and why a group wasn't.
    pub sites: Vec<(String, u32, usize)>,
    pub sites_skipped: Vec<String>,
    /// The dwords in .rdata and .data that the base relocations name (issue
    /// #138: the code pointers in the game's tables, the renderer's among
    /// them), as (where, the value there).
    pub data_pointers: Vec<(u32, u32)>,
    cs: Cs,
    cache: RefCell<HashMap<u32, Rc<Insn>>>,
}

fn rd16(d: &[u8], o: usize) -> u32 {
    if o + 2 > d.len() {
        return 0;
    }
    u16::from_le_bytes([d[o], d[o + 1]]) as u32
}

fn rd32(d: &[u8], o: usize) -> u32 {
    if o + 4 > d.len() {
        return 0;
    }
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

fn cstr(d: &[u8], o: usize) -> Vec<u8> {
    let mut out = Vec::new();
    let mut k = o;
    while k < d.len() && d[k] != 0 {
        out.push(d[k]);
        k += 1;
    }
    out
}

struct Section {
    name: Vec<u8>,
    vsize: u32,
    va: u32,
    raw_size: u32,
    raw_ptr: u32,
}

/// pefile's cache_adjust_SectionAlignment.
fn adjust_section_alignment(val: u32, section_alignment: u32, file_alignment: u32) -> u32 {
    let sa = if section_alignment < 0x1000 { file_alignment } else { section_alignment };
    if sa != 0 && val % sa != 0 {
        return sa * (val / sa);
    }
    val
}

impl Image {
    pub fn load(path: &Path, src_root: &Path) -> Result<Image, String> {
        let data = std::fs::read(path).map_err(|e| format!("{}: {}", path.display(), e))?;
        let pe = rd32(&data, 0x3c) as usize;
        if data.get(pe..pe + 4) != Some(b"PE\0\0") {
            return Err(format!("{}: not a PE file", path.display()));
        }
        let coff = pe + 4;
        let nsec = rd16(&data, coff + 2) as usize;
        let opt_size = rd16(&data, coff + 16) as usize;
        let opt = coff + 20;
        let base = rd32(&data, opt + 28);
        let section_alignment = rd32(&data, opt + 32);
        let file_alignment = rd32(&data, opt + 36);
        let import_rva = rd32(&data, opt + 96 + 8);
        let (reloc_rva, reloc_size) = (rd32(&data, opt + 96 + 8 * 5), rd32(&data, opt + 96 + 8 * 5 + 4));
        let sec0 = opt + opt_size;
        let mut sections = Vec::new();
        for k in 0..nsec {
            let s = sec0 + 40 * k;
            sections.push(Section {
                name: data[s..s + 8].to_vec(),
                vsize: rd32(&data, s + 8),
                va: rd32(&data, s + 12),
                raw_size: rd32(&data, s + 16),
                raw_ptr: rd32(&data, s + 20),
            });
        }
        // The header: up to the lowest section's raw data, or the end of the
        // section table if that comes later.
        let table_end = sec0 + 40 * nsec;
        let lowest = sections.iter().filter(|s| s.raw_ptr > 0).map(|s| (s.raw_ptr & !0x1ff) as usize).min();
        let hdr = match lowest {
            Some(l) if l != 0 && l >= table_end => l,
            _ => table_end,
        };
        let mut mem: Vec<u8> = data[..hdr.min(data.len())].to_vec();
        for s in &sections {
            if s.vsize == 0 && s.raw_size == 0 {
                continue;
            }
            let srd = s.raw_size as usize;
            let prd = (s.raw_ptr & !0x1ff) as usize;
            let va = adjust_section_alignment(s.va, section_alignment, file_alignment) as usize;
            if srd > data.len() || prd > data.len() || srd + prd > data.len() || va >= 0x1000_0000 {
                continue;
            }
            if va > mem.len() {
                mem.resize(va, 0);
            } else {
                mem.truncate(va);
            }
            let end = (prd + srd).min(s.raw_ptr as usize + srd).min(data.len());
            mem.extend_from_slice(&data[prd..end.max(prd)]);
        }
        let text = sections
            .iter()
            .find(|s| {
                let n: Vec<u8> = s.name.iter().copied().take_while(|&b| b != 0).collect();
                n == b".text"
            })
            .ok_or("no .text")?;
        let text_lo = base + text.va;
        let text_hi = text_lo + text.vsize;
        // The imports, from the mapped image as pefile reads them.
        let mut imports = OMap::new();
        if import_rva != 0 {
            let mut d = import_rva as usize;
            loop {
                let oft = rd32(&mem, d);
                let name_rva = rd32(&mem, d + 12);
                let ft = rd32(&mem, d + 16);
                if oft == 0 && name_rva == 0 && ft == 0 && rd32(&mem, d + 4) == 0 && rd32(&mem, d + 8) == 0 {
                    break;
                }
                let dll = String::from_utf8_lossy(&cstr(&mem, name_rva as usize)).into_owned();
                let ilt = if oft != 0 { oft } else { ft };
                let mut k = 0usize;
                loop {
                    let v = rd32(&mem, ilt as usize + 4 * k);
                    if v == 0 {
                        break;
                    }
                    let name = if v & 0x8000_0000 != 0 {
                        // pefile names some DLLs' ordinals from its own tables (ord<n> where it has none).
                        let ord = v & 0xffff;
                        match crate::ordlookup::table(&dll.to_lowercase()) {
                            Some(t) => t.iter().find(|(o, _)| *o == ord).map(|(_, n)| n.to_string()).unwrap_or(format!("ord{}", ord)),
                            None => format!("#{}", ord),
                        }
                    } else {
                        String::from_utf8_lossy(&cstr(&mem, v as usize + 2)).into_owned()
                    };
                    imports.insert(base + ft + 4 * k as u32, (dll.clone(), name));
                    k += 1;
                }
                d += 20;
            }
        }
        // The base relocations' HIGHLOW entries (type 3) in the data sections.
        let data_secs: Vec<(u32, u32)> = sections
            .iter()
            .filter(|s| {
                let n: Vec<u8> = s.name.iter().copied().take_while(|&b| b != 0).collect();
                n == b".rdata" || n == b".data"
            })
            .map(|s| (s.va, s.va + s.vsize.max(s.raw_size)))
            .collect();
        let mut data_pointers = Vec::new();
        let (mut b, end) = (reloc_rva as usize, (reloc_rva + reloc_size) as usize);
        while reloc_rva != 0 && b + 8 <= end.min(mem.len()) {
            let (page, size) = (rd32(&mem, b), rd32(&mem, b + 4) as usize);
            if size < 8 {
                break;
            }
            for k in 0..(size - 8) / 2 {
                let e = rd16(&mem, b + 8 + 2 * k);
                let at = page + (e & 0xfff);
                if e >> 12 == 3 && data_secs.iter().any(|&(lo, hi)| lo <= at && at < hi) {
                    data_pointers.push((base + at, rd32(&mem, at as usize)));
                }
            }
            b += size;
        }
        let mut import_pops = import_pops(&imports, src_root);
        let resumes = resumes(&src_root.join("src/game/d2_native.cpp"));
        let (native_slots, sites, sites_skipped) =
            apply_sites(&mut mem, base, &mut imports, &mut import_pops, &src_root.join("src/game/native_sites_table.h"));
        Ok(Image {
            base,
            mem,
            text_lo,
            text_hi,
            imports,
            import_pops,
            resumes,
            native_slots,
            sites,
            sites_skipped,
            data_pointers,
            cs: Cs::new(),
            cache: RefCell::new(HashMap::new()),
        })
    }

    pub fn u32(&self, va: u32) -> u32 {
        let o = va.wrapping_sub(self.base) as usize;
        let mut v = 0u32;
        for k in 0..4 {
            if let Some(&b) = self.mem.get(o + k) {
                v |= (b as u32) << (8 * k);
            }
        }
        v
    }

    pub fn u8(&self, va: u32) -> u8 {
        self.mem[va.wrapping_sub(self.base) as usize]
    }

    /// In .text, or in a natives' site applied past .text's virtual size (a
    /// stub in the section's zero slack, issue #138).
    pub fn in_text(&self, va: u32) -> bool {
        (self.text_lo <= va && va < self.text_hi)
            || self.sites.iter().any(|&(_, s, n)| s <= va && va < s + n as u32)
    }

    /// The targets of the direct calls in the applied sites that land in
    /// another applied site (issue #138: the decode timer's stub calls its
    /// own tail, which runs the function's first bytes and jumps to the
    /// rest). Each is a function of its own. The sites are code from their
    /// first byte, at instruction boundaries.
    pub fn site_call_targets(&self) -> Vec<u32> {
        let mut out = Vec::new();
        for &(_, s, n) in &self.sites {
            let mut va = s;
            while va < s + n as u32 {
                let Ok(i) = self.insn(va) else { break };
                if i.mnemonic == "call" && i.bytes[0] == 0xe8 {
                    let t = va.wrapping_add(i.size).wrapping_add(i32::from_le_bytes(i.bytes[1..5].try_into().unwrap()) as u32);
                    if t != s && self.sites.iter().any(|&(_, s2, n2)| s2 <= t && t < s2 + n2 as u32) && !out.contains(&t) {
                        out.push(t);
                    }
                }
                va += i.size;
            }
        }
        out
    }

    /// The instruction at va; `rep ret` (F3 C3) is a ret (issue #135).
    pub fn insn(&self, va: u32) -> Result<Rc<Insn>, Refused> {
        if let Some(i) = self.cache.borrow().get(&va) {
            return Ok(i.clone());
        }
        if !self.in_text(va) {
            return Err(Refused(format!("code outside .text at {:08x}", va)));
        }
        let o = (va - self.base) as usize;
        let lo = o.min(self.mem.len());
        let hi = (o + 16).min(self.mem.len());
        let mut i = match self.cs.disasm1(&self.mem[lo..hi], va) {
            Some(i) => i,
            None => return Err(Refused(format!("no instruction decodes at {:08x}", va))),
        };
        if i.mnemonic == "repz ret" || i.mnemonic == "rep ret" {
            i.mnemonic = "ret".to_string();
        }
        let i = Rc::new(i);
        self.cache.borrow_mut().insert(va, i.clone());
        Ok(i)
    }
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

fn is_space(c: char) -> bool {
    c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c)
}

/// recompile.py's `\b[A-Z][A-Za-z0-9_]*\(\s*"([A-Za-z0-9_]+)",\s*(\d+)\s*,`
/// over a shim source, each (name, argc) in order.
/// The shims declared with their DLL first, `X("IMM32.dll", "ImmGetContext",
/// 1, ...`, or in a table by ordinal, `{115, "WSAStartup", 2, ...` (issue
/// #138: their thunks' pops weren't known, 102 functions refused): each
/// name with the argument count after it.
fn shim_decls_by_dll(text: &str, out: &mut Vec<(String, i64)>) {
    let b = text.as_bytes();
    let n = b.len();
    let skip = |mut k: usize| {
        while k < n && (b[k] == b' ' || b[k] == b'\n' || b[k] == b'\t' || b[k] == b'\r') {
            k += 1;
        }
        k
    };
    // A string literal at k: (its text, the index after its closing quote).
    let lit = |k: usize| -> Option<(String, usize)> {
        if k >= n || b[k] != b'"' {
            return None;
        }
        let e = (k + 1..n).find(|&j| b[j] == b'"' || b[j] == b'\n')?;
        if b[e] != b'"' {
            return None;
        }
        Some((String::from_utf8_lossy(&b[k + 1..e]).into_owned(), e + 1))
    };
    // `, <digits>` at k: the number.
    let count = |k: usize| -> Option<i64> {
        let k = skip(k);
        if k >= n || b[k] != b',' {
            return None;
        }
        let k = skip(k + 1);
        let e = (k..n).find(|&j| !b[j].is_ascii_digit())?;
        if e == k {
            return None;
        }
        std::str::from_utf8(&b[k..e]).ok()?.parse().ok()
    };
    let mut p = 0;
    while p < n {
        if b[p] != b'"' {
            p += 1;
            continue;
        }
        let Some((first, after)) = lit(p) else {
            p += 1;
            continue;
        };
        if first.to_ascii_lowercase().ends_with(".dll") {
            let k = skip(after);
            if k < n && b[k] == b',' {
                if let Some((name, e)) = lit(skip(k + 1)) {
                    if let Some(c) = count(e) {
                        out.push((name, c));
                    }
                }
            }
        } else {
            // { <digits>, "Name", <digits>
            let mut k = p;
            while k > 0 && (b[k - 1] == b' ' || b[k - 1] == b',') {
                k -= 1;
            }
            let d = k;
            while k > 0 && b[k - 1].is_ascii_digit() {
                k -= 1;
            }
            if k < d && k > 0 && b[k - 1] == b'{' {
                if let Some(c) = count(after) {
                    out.push((first, c));
                }
            }
        }
        p = after;
    }
}

fn shim_decls(text: &str, out: &mut Vec<(String, i64)>) {
    let s: Vec<char> = text.chars().collect();
    let n = s.len();
    let ident = |c: char| c.is_ascii_alphanumeric() || c == '_';
    let mut p = 0;
    while p < n {
        let m = (|| -> Option<(usize, String, i64)> {
            if !s[p].is_ascii_uppercase() || (p > 0 && is_word(s[p - 1])) {
                return None;
            }
            let mut k = p + 1;
            while k < n && ident(s[k]) {
                k += 1;
            }
            if k >= n || s[k] != '(' {
                return None;
            }
            k += 1;
            while k < n && is_space(s[k]) {
                k += 1;
            }
            if k >= n || s[k] != '"' {
                return None;
            }
            k += 1;
            let a = k;
            while k < n && ident(s[k]) {
                k += 1;
            }
            if k == a || k >= n || s[k] != '"' {
                return None;
            }
            let name: String = s[a..k].iter().collect();
            k += 1;
            if k >= n || s[k] != ',' {
                return None;
            }
            k += 1;
            while k < n && is_space(s[k]) {
                k += 1;
            }
            let d = k;
            while k < n && s[k].is_ascii_digit() {
                k += 1;
            }
            if k == d {
                return None;
            }
            let num: String = s[d..k].iter().collect();
            while k < n && is_space(s[k]) {
                k += 1;
            }
            if k >= n || s[k] != ',' {
                return None;
            }
            Some((k + 1, name, num.parse().ok()?))
        })();
        match m {
            Some((end, name, argc)) => {
                out.push((name, argc));
                p = end;
            }
            None => p += 1,
        }
    }
}

fn glob_dir(dir: &Path, prefix: &str, suffix: &str) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(rd) = std::fs::read_dir(dir) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !n.starts_with('.') && n.starts_with(prefix) && n.ends_with(suffix) && n.len() >= prefix.len() + suffix.len()
            {
                out.push(dir.join(n));
            }
        }
    }
    out
}

/// What each import's ret pops (recompile.py's import_pops): from our
/// runtime's shims, which declare their argument counts, and from decorated
/// names; wsprintfA is cdecl.
fn import_pops(imports: &OMap<u32, (String, String)>, root: &Path) -> HashMap<u32, Option<i64>> {
    let mut files = glob_dir(&root.join("src/runtime"), "rt_shims_", ".cpp");
    files.extend(glob_dir(&root.join("third_party/winx86/src/runtime"), "", ".cpp"));
    files.sort_by(|a, b| a.to_string_lossy().cmp(&b.to_string_lossy()));
    let mut argc: HashMap<String, i64> = HashMap::new();
    for f in files {
        if let Ok(b) = std::fs::read(&f) {
            let mut decls = Vec::new();
            shim_decls(&String::from_utf8_lossy(&b), &mut decls);
            shim_decls_by_dll(&String::from_utf8_lossy(&b), &mut decls);
            for (name, n) in decls {
                argc.entry(name).or_insert(n);
            }
        }
    }
    let mut out = HashMap::new();
    for (slot, (dll, name)) in imports.iter() {
        // Shims registered with no argument count in their source: Win32's
        // own signatures (DSOUND.dll's are imported by ordinal).
        let win32 = match (dll.to_lowercase().as_str(), name.as_str()) {
            ("dsound.dll", "#1") => Some(12),                    // DirectSoundCreate
            ("dsound.dll", "#2") => Some(8),                     // DirectSoundEnumerateA
            ("psapi.dll", "GetModuleFileNameExA") => Some(16),
            ("version.dll", "VerQueryValueA") => Some(16),
            _ => None,
        };
        let v = if win32.is_some() {
            win32
        } else if name == "wsprintfA" || name == "wsprintfW" {
            Some(0)
        } else if let Some(at) = name.rfind('@').filter(|&k| {
            let t = &name[k + 1..];
            !t.is_empty() && t.chars().all(|c| c.is_ascii_digit())
        }) {
            name[at + 1..].parse().ok()
        } else if let Some(n) = argc.get(name) {
            Some(4 * n)
        } else if name == "timeGetTime" {
            Some(0)
        } else {
            None
        };
        out.insert(*slot, v);
    }
    out
}

/// The constant addresses d2_native.cpp resumes the game's code at
/// (`resume_original(c, 0x<hex>`), each once, in order.
fn resumes(path: &Path) -> Vec<u32> {
    let text = String::from_utf8_lossy(&std::fs::read(path).unwrap_or_default()).into_owned();
    let mut out = Vec::new();
    let key = "resume_original(c, 0x";
    let mut rest = text.as_str();
    while let Some(k) = rest.find(key) {
        rest = &rest[k + key.len()..];
        let hex: String = rest.chars().take_while(|c| c.is_ascii_hexdigit()).collect();
        if let Ok(v) = u32::from_str_radix(&hex, 16) {
            if !hex.is_empty() && !out.contains(&v) {
                out.push(v);
            }
        }
    }
    out
}

/// The arguments of a table row's macro: `NAME(a, "b c", d)` -> [a, b c, d].
fn row_args(line: &str, name: &str) -> Option<Vec<String>> {
    let rest = line.strip_prefix(name)?.strip_prefix('(')?;
    let rest = rest.trim_end().strip_suffix(')')?;
    let mut out = Vec::new();
    let (mut cur, mut quoted) = (String::new(), false);
    for ch in rest.chars() {
        match ch {
            '"' => quoted = !quoted,
            ',' if !quoted => {
                out.push(cur.trim().to_string());
                cur.clear();
            }
            _ => cur.push(ch),
        }
    }
    out.push(cur.trim().to_string());
    Some(out)
}

fn hex_bytes(s: &str) -> Option<Vec<u8>> {
    s.split_whitespace().map(|b| u8::from_str_radix(b, 16).ok().filter(|_| b.len() == 2)).collect()
}

/// native_sites_table.h's sites, of the groups marked compiled, written into
/// `mem` as the install code writes them, each native's rel32 to its slot
/// (an import of its own, popping its stack dwords). A group whose game
/// bytes differ anywhere is left out whole, as the install code leaves it.
fn apply_sites(
    mem: &mut [u8],
    base: u32,
    imports: &mut OMap<u32, (String, String)>,
    import_pops: &mut HashMap<u32, Option<i64>>,
    table: &Path,
) -> (HashSet<u32>, Vec<(String, u32, usize)>, Vec<String>) {
    const SLOT_BASE: u32 = 0xfee0_0000;
    const SLOT_STEP: u32 = 16;
    let text = std::fs::read_to_string(table).unwrap_or_default();
    let mut natives: Vec<(String, i64)> = Vec::new();
    let mut compiled: HashSet<String> = HashSet::new();
    let mut rows: Vec<(String, u32, String, String)> = Vec::new();
    for line in text.lines().map(str::trim) {
        if let Some(a) = row_args(line, "NATIVE") {
            natives.push((a[0].clone(), a[1].parse().expect("NATIVE's stack dwords")));
        } else if let Some(a) = row_args(line, "GROUP") {
            if a[1] == "1" {
                compiled.insert(a[0].clone());
            }
        } else if let Some(a) = row_args(line, "SITE") {
            let va = crate::driver::parse_hex(&a[1]).expect("SITE's address");
            rows.push((a[0].clone(), va, a[2].clone(), a[3].clone()));
        }
    }
    let mut slots = HashSet::new();
    for (k, (name, n)) in natives.iter().enumerate() {
        let slot = SLOT_BASE + SLOT_STEP * k as u32;
        imports.insert(slot, ("native".to_string(), name.clone()));
        import_pops.insert(slot, Some(4 * n));
        slots.insert(slot);
    }
    let build = |va: u32, w: &str| -> Option<Vec<u8>> {
        let mut out = Vec::new();
        let mut rest = w;
        while !rest.trim_start().is_empty() {
            rest = rest.trim_start();
            if let Some(r) = rest.strip_prefix('{') {
                let end = r.find('}')?;
                let name = &r[..end];
                let target = if let Some(a) = name.strip_prefix('=') {
                    crate::driver::parse_hex(a).ok()?
                } else {
                    SLOT_BASE + SLOT_STEP * natives.iter().position(|(n, _)| n == name)? as u32
                };
                let rel = target.wrapping_sub(va + out.len() as u32 + 4);
                out.extend_from_slice(&rel.to_le_bytes());
                rest = &r[end + 1..];
            } else {
                let b = rest.get(..2)?;
                out.push(u8::from_str_radix(b, 16).ok()?);
                rest = &rest[2..];
            }
        }
        Some(out)
    };
    let mut done = Vec::new();
    let mut skipped = Vec::new();
    let mut groups: Vec<String> = Vec::new();
    for (g, ..) in &rows {
        if compiled.contains(g) && !groups.contains(g) {
            groups.push(g.clone());
        }
    }
    'group: for g in groups {
        let mut writes = Vec::new();
        for (rg, va, game, written) in rows.iter().filter(|r| r.0 == g) {
            let o = va.wrapping_sub(base) as usize;
            let (Some(want), Some(bytes)) = (hex_bytes(game), build(*va, written)) else {
                skipped.push(format!("{}: the row at {:08x} doesn't parse", rg, va));
                continue 'group;
            };
            if bytes.len() != want.len() || mem.get(o..o + want.len()) != Some(&want[..]) {
                skipped.push(format!("{}: other bytes at {:08x}", rg, va));
                continue 'group;
            }
            writes.push((*va, o, bytes));
        }
        for (va, o, bytes) in writes {
            mem[o..o + bytes.len()].copy_from_slice(&bytes);
            done.push((g.clone(), va, bytes.len()));
        }
    }
    (slots, done, skipped)
}
