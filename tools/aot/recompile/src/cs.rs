//! Capstone 5 through capstone-sys: one x86-32 instruction at a time, with
//! what recompile.py read from Python's binding (operands, the registers
//! cs_regs_access reports, eflags, groups).

use capstone_sys as sys;
use std::ffi::CStr;
use std::sync::OnceLock;

pub const OP_REG: u32 = 1;
pub const OP_IMM: u32 = 2;
pub const OP_MEM: u32 = 3;
pub const AC_WRITE: u8 = 2;
pub const REG_EIP: u16 = 26;
pub const GRP_JUMP: u8 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Mem {
    pub segment: u16,
    pub base: u16,
    pub index: u16,
    pub scale: i32,
    pub disp: i64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OpKind {
    Reg(u16),
    Imm(i64),
    Mem(Mem),
    Invalid,
}

#[derive(Clone, Copy, Debug)]
pub struct Op {
    pub kind: OpKind,
    pub size: u8,
    pub access: u8,
}

impl Op {
    pub fn is_reg(&self) -> bool {
        matches!(self.kind, OpKind::Reg(_))
    }
    pub fn is_imm(&self) -> bool {
        matches!(self.kind, OpKind::Imm(_))
    }
    pub fn is_mem(&self) -> bool {
        matches!(self.kind, OpKind::Mem(_))
    }
    /// The register (Python's op.reg; 0 for another kind).
    pub fn reg(&self) -> u16 {
        match self.kind {
            OpKind::Reg(r) => r,
            _ => 0,
        }
    }
    /// The immediate (Python's op.imm; 0 for another kind).
    pub fn imm(&self) -> i64 {
        match self.kind {
            OpKind::Imm(v) => v,
            _ => 0,
        }
    }
    /// The memory operand (Python's op.mem; zeros for another kind).
    pub fn mem(&self) -> Mem {
        match self.kind {
            OpKind::Mem(m) => m,
            _ => Mem { segment: 0, base: 0, index: 0, scale: 0, disp: 0 },
        }
    }
}

#[derive(Debug)]
pub struct Insn {
    pub address: u32,
    pub size: u32,
    pub bytes: Vec<u8>,
    pub mnemonic: String,
    pub op_str: String,
    pub ops: Vec<Op>,
    pub regs_read: Vec<u16>,
    pub regs_write: Vec<u16>,
    pub eflags: u64,
    pub groups: Vec<u8>,
}

/// (full register 0..8, bits, shift) of a register id, as recompile.py's
/// SUBREG by Capstone's name.
pub type Sub = (u8, u8, u8);

struct Tables {
    names: Vec<String>,
    subs: Vec<Option<Sub>>,
}

static TABLES: OnceLock<Tables> = OnceLock::new();

const REGS: [&str; 8] = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];

fn subreg(name: &str) -> Option<Sub> {
    for (i, n) in REGS.iter().enumerate() {
        if name == *n {
            return Some((i as u8, 32, 0));
        }
        if name == &n[1..] {
            return Some((i as u8, 16, 0));
        }
    }
    for (i, n) in ["al", "cl", "dl", "bl"].iter().enumerate() {
        if name == *n {
            return Some((i as u8, 8, 0));
        }
        if name.len() == 2 && name.as_bytes()[0] == n.as_bytes()[0] && name.as_bytes()[1] == b'h' {
            return Some((i as u8, 8, 8));
        }
    }
    None
}

pub struct Cs {
    h: sys::csh,
}

impl Cs {
    pub fn new() -> Cs {
        let mut h: sys::csh = 0;
        unsafe {
            let e = sys::cs_open(sys::cs_arch::CS_ARCH_X86, sys::cs_mode::CS_MODE_32, &mut h);
            assert_eq!(e, sys::cs_err::CS_ERR_OK, "cs_open");
            let e = sys::cs_option(h, sys::cs_opt_type::CS_OPT_DETAIL, sys::cs_opt_value::CS_OPT_ON as usize);
            assert_eq!(e, sys::cs_err::CS_ERR_OK, "cs_option");
        }
        let cs = Cs { h };
        TABLES.get_or_init(|| {
            let mut names = Vec::new();
            let mut subs = Vec::new();
            for id in 0..(sys::x86_reg::X86_REG_ENDING as u32) {
                let p = unsafe { sys::cs_reg_name(cs.h, id) };
                let n = if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() };
                subs.push(subreg(&n));
                names.push(n);
            }
            Tables { names, subs }
        });
        cs
    }

    /// The first instruction in `code` at `address`, or None.
    pub fn disasm1(&self, code: &[u8], address: u32) -> Option<Insn> {
        unsafe {
            let mut p: *mut sys::cs_insn = std::ptr::null_mut();
            let n = sys::cs_disasm(self.h, code.as_ptr(), code.len(), address as u64, 1, &mut p);
            if n == 0 {
                return None;
            }
            let i = &*p;
            let d = &*i.detail;
            let x = &d.__bindgen_anon_1.x86;
            let mut ops = Vec::new();
            for k in 0..x.op_count as usize {
                let o = &x.operands[k];
                let kind = match o.type_ as u32 {
                    OP_REG => OpKind::Reg(o.__bindgen_anon_1.reg as u16),
                    OP_IMM => OpKind::Imm(o.__bindgen_anon_1.imm),
                    OP_MEM => {
                        let m = o.__bindgen_anon_1.mem;
                        OpKind::Mem(Mem {
                            segment: m.segment as u16,
                            base: m.base as u16,
                            index: m.index as u16,
                            scale: m.scale,
                            disp: m.disp,
                        })
                    }
                    _ => OpKind::Invalid,
                };
                ops.push(Op { kind, size: o.size, access: o.access });
            }
            let mut rr: sys::cs_regs = [0; 64];
            let mut rw: sys::cs_regs = [0; 64];
            let (mut nr, mut nw) = (0u8, 0u8);
            let e = sys::cs_regs_access(self.h, p, &mut rr as *mut _, &mut nr, &mut rw as *mut _, &mut nw);
            let (regs_read, regs_write) = if e == sys::cs_err::CS_ERR_OK {
                (rr[..nr as usize].to_vec(), rw[..nw as usize].to_vec())
            } else {
                (Vec::new(), Vec::new())
            };
            let out = Insn {
                address: i.address as u32,
                size: i.size as u32,
                bytes: i.bytes[..i.size as usize].to_vec(),
                mnemonic: CStr::from_ptr(i.mnemonic.as_ptr()).to_string_lossy().into_owned(),
                op_str: CStr::from_ptr(i.op_str.as_ptr()).to_string_lossy().into_owned(),
                ops,
                regs_read,
                regs_write,
                eflags: x.__bindgen_anon_1.eflags,
                groups: d.groups[..d.groups_count as usize].to_vec(),
            };
            sys::cs_free(p, n);
            Some(out)
        }
    }
}

impl Drop for Cs {
    fn drop(&mut self) {
        unsafe {
            sys::cs_close(&mut self.h);
        }
    }
}

/// Capstone's name of a register id ("" for none).
pub fn reg_name(rid: u16) -> &'static str {
    let t = TABLES.get().expect("Cs::new first");
    t.names.get(rid as usize).map(|s| s.as_str()).unwrap_or("")
}

/// recompile.py's reg_full: (full register, bits, shift), None for a segment
/// or system register.
pub fn reg_full(rid: u16) -> Option<Sub> {
    let t = TABLES.get().expect("Cs::new first");
    t.subs.get(rid as usize).copied().flatten()
}
