//! What an instruction reads and writes: registers, half registers (the
//! liveness), flags and memory (recompile.py's regs_rw, parts_rw, flags_rw,
//! mem_written), and the condition codes.

use crate::cs::{reg_full, Insn, Op, OpKind, AC_WRITE};

pub const REGS: [&str; 8] = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];
pub const EAX: u8 = 0;
pub const ECX: u8 = 1;
pub const EDX: u8 = 2;
pub const EBX: u8 = 3;
pub const ESP: u8 = 4;
pub const EBP: u8 = 5;
pub const ESI: u8 = 6;
pub const EDI: u8 = 7;
/// A set of the eight registers, bit r for register r.
pub type Regs = u8;
pub const ALL: Regs = 0xff;

pub fn bit(r: u8) -> Regs {
    1 << r
}

pub fn has(s: Regs, r: u8) -> bool {
    s & (1 << r) != 0
}

/// The registers of a set, in order.
pub fn regs_of(s: Regs) -> Vec<u8> {
    (0..8).filter(|&r| has(s, r)).collect()
}

pub fn set_of(rs: &[u8]) -> Regs {
    rs.iter().fold(0, |a, &r| a | bit(r))
}

// Flags: carry, parity, adjust, zero, sign, overflow.
pub const FC: u8 = 1;
pub const FP: u8 = 2;
pub const FA: u8 = 4;
pub const FZ: u8 = 8;
pub const FS: u8 = 16;
pub const FO: u8 = 32;
pub const FALL: u8 = 0x3f;

const fn b(n: u32) -> u64 {
    1u64 << n
}
// Capstone's x86.h eflags bits: (ours, modify|reset|set|undefined, test).
const EFL: [(u8, u64, u64); 6] = [
    (FC, b(1) | b(22) | b(30) | b(45), b(37)),
    (FP, b(4) | b(29) | b(56) | b(43), b(36)),
    (FA, b(0) | b(26) | b(55) | b(44), b(50)),
    (FZ, b(3) | b(51) | b(54) | b(42), b(35)),
    (FS, b(2) | b(25) | b(53) | b(41), b(34)),
    (FO, b(5) | b(21) | b(52) | b(40), b(33)),
];

pub const CC_NAMES: [&str; 16] = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"];

pub fn cc_flags(cc: &str) -> u8 {
    match cc {
        "o" | "no" => FO,
        "b" | "ae" => FC,
        "e" | "ne" => FZ,
        "be" | "a" => FC | FZ,
        "s" | "ns" => FS,
        "p" | "np" => FP,
        "l" | "ge" => FS | FO,
        "le" | "g" => FZ | FS | FO,
        _ => panic!("no condition code {}", cc),
    }
}

pub fn cc_num(cc: &str) -> usize {
    CC_NAMES.iter().position(|c| *c == cc).unwrap_or_else(|| panic!("no condition code {}", cc))
}

fn cc_alias(c: &str) -> &str {
    match c {
        "c" | "nae" => "b",
        "nc" | "nb" => "ae",
        "z" => "e",
        "nz" => "ne",
        "na" => "be",
        "nbe" => "a",
        "pe" => "p",
        "po" => "np",
        "nge" => "l",
        "nl" => "ge",
        "ng" => "le",
        "nle" => "g",
        _ => c,
    }
}

/// The condition code of a jcc, setcc or cmovcc (after `prefix`).
pub fn cc_of<'a>(mnemonic: &'a str, prefix: &str) -> &'a str {
    cc_alias(&mnemonic[prefix.len()..])
}

/// Flag kinds, as aot_rt.h numbers them.
pub fn kind(m: &str) -> Option<u8> {
    Some(match m {
        "add" => 0,
        "sub" | "cmp" | "repe cmpsb" | "repne cmpsb" | "repe cmpsw" | "repne cmpsw" | "repe cmpsd" | "repne cmpsd" => 1,
        "and" | "or" | "xor" | "test" | "bsf" | "bsr" => 2,
        "inc" => 3,
        "dec" => 4,
        "neg" => 5,
        "shl" | "sal" => 6,
        "shr" => 7,
        "sar" => 8,
        "imul" | "mul" => 9,
        "adc" => 10,
        "sbb" => 11,
        // ZF alone, as a logic result is read: 0 where it stored (emit.rs).
        "cmpxchg8b" | "lock cmpxchg8b" => 2,
        // AOT_EFL: the flags as a word of eflags' bits (aot_rt.h).
        "sahf" | "bt" | "bts" | "btr" | "btc" | "ucomisd" | "comisd" => 12,
        _ => return None,
    })
}

/// The full registers an instruction reads and writes (a partial write
/// reads the rest of its register).
pub fn regs_rw(i: &Insn) -> (Regs, Regs) {
    let (mut reads, mut writes) = (0u8, 0u8);
    for &r in &i.regs_read {
        if let Some(s) = reg_full(r) {
            reads |= bit(s.0);
        }
    }
    for &r in &i.regs_write {
        if let Some(s) = reg_full(r) {
            writes |= bit(s.0);
            if s.1 < 32 {
                reads |= bit(s.0);
            }
        }
    }
    let m = i.mnemonic.as_str();
    let ops = &i.ops;
    if (m == "xor" || m == "sub") && ops.len() == 2 && ops[0].is_reg() && ops[1].is_reg() && ops[0].reg() == ops[1].reg()
    {
        if let Some(s) = reg_full(ops[0].reg()) {
            if s.1 == 32 {
                reads &= !bit(s.0);
            }
        }
    }
    (reads, writes)
}

/// Half registers: bit 2r the low 16 bits of r, 2r+1 the high.
pub type Parts = u16;

pub fn p_of(s: Regs) -> Parts {
    let mut out = 0u16;
    for r in 0..8 {
        if has(s, r) {
            out |= 3 << (2 * r);
        }
    }
    out
}

pub fn r_of(p: Parts) -> Regs {
    let mut out = 0u8;
    for q in 0..16 {
        if p & (1 << q) != 0 {
            out |= bit((q / 2) as u8);
        }
    }
    out
}

/// The half registers an instruction reads and writes: a 16-bit write
/// leaves the high half alone, an 8-bit write reads the rest of the low.
pub fn parts_rw(i: &Insn) -> (Parts, Parts) {
    let (mut reads, mut writes) = (0u16, 0u16);
    for &r in &i.regs_read {
        if let Some(s) = reg_full(r) {
            reads |= if s.1 == 32 { p_of(bit(s.0)) } else { 1 << (2 * s.0) };
        }
    }
    for &r in &i.regs_write {
        if let Some(s) = reg_full(r) {
            if s.1 == 32 {
                writes |= p_of(bit(s.0));
            } else {
                writes |= 1 << (2 * s.0);
                if s.1 == 8 {
                    reads |= 1 << (2 * s.0);
                }
            }
        }
    }
    let m = i.mnemonic.as_str();
    let ops = &i.ops;
    if (m == "xor" || m == "sub") && ops.len() == 2 && ops[0].is_reg() && ops[1].is_reg() && ops[0].reg() == ops[1].reg()
    {
        if let Some(s) = reg_full(ops[0].reg()) {
            if s.1 == 32 {
                reads &= !p_of(bit(s.0));
            }
        }
    }
    (reads, writes)
}

/// The flags an instruction writes and reads; Capstone 5 leaves out what
/// `test` with a memory operand writes, so by mnemonic too.
pub fn flags_rw(i: &Insn) -> (u8, u8) {
    let m = i.mnemonic.as_str();
    // x87 leaves eflags alone but for fcomi and its kin; Capstone's field is
    // its fpu_flags there, another set of bits (issue #138: 4144c0's cmp,
    // fld, fld1, jnc was refused as reading fld1's flags).
    if crate::emit::x87_op(i).is_some() {
        if m.starts_with("fcomi") || m.starts_with("fucomi") {
            return (FALL, 0);
        }
        if m.starts_with("fcmov") {
            return (0, FC | FP | FZ);
        }
        return (0, 0);
    }
    let e = i.eflags;
    let (mut d, mut u) = (0u8, 0u8);
    for (ours, m, t) in EFL {
        if e & m != 0 {
            d |= ours;
        }
        if e & t != 0 {
            u |= ours;
        }
    }
    d |= match m {
        "add" | "sub" | "cmp" | "and" | "or" | "xor" | "test" | "neg" | "adc" | "sbb" | "imul" | "mul" | "shl" | "sal"
        | "shr" | "sar" => FALL,
        "inc" | "dec" => FALL & !FC,
        _ => 0,
    };
    // bt: the carry alone (the rest undefined, ZF left: an earlier
    // instruction's still). sahf: all but the overflow.
    if ["bt", "bts", "btr", "btc"].contains(&m) {
        d = FC;
    }
    if m == "sahf" {
        d = FALL & !FO;
    }
    if m == "ucomisd" || m == "comisd" {
        d = FALL;
    }
    if m == "rcr" || m == "rcl" {
        u |= FC;   // through the carry (Capstone gives it as modified only)
    }
    if m.starts_with('j') && m != "jmp" && m != "jecxz" && m != "jcxz" {
        u = cc_flags(cc_of(m, "j"));
    } else if m.starts_with("set") {
        u = cc_flags(cc_of(m, "set"));
    } else if m.starts_with("cmov") {
        u = cc_flags(cc_of(m, "cmov"));
    }
    (d, u)
}

const X87_STORES: [&str; 10] = ["fst", "fstp", "fist", "fistp", "fisttp", "fnstsw", "fnstcw", "fbstp", "fnsave", "fnstenv"];

fn x87_mem_size(m: &str) -> Option<u32> {
    match m {
        "fnsave" | "frstor" => Some(108),
        "fnstenv" | "fldenv" => Some(28),
        _ => None,
    }
}

/// (operand, bytes) for each memory operand the instruction writes: x87's
/// by mnemonic (Capstone 5's access is wrong for most of it).
pub fn mem_written(i: &Insn) -> Vec<(Op, u32)> {
    let m = i.mnemonic.as_str();
    let mems = i.ops.iter().filter(|o| matches!(o.kind, OpKind::Mem(_)));
    if m.starts_with('f') {
        if X87_STORES.contains(&m) {
            return mems.map(|o| (*o, x87_mem_size(m).unwrap_or(o.size as u32))).collect();
        }
        return Vec::new();
    }
    mems.filter(|o| o.access & AC_WRITE != 0).map(|o| (*o, o.size as u32)).collect()
}
