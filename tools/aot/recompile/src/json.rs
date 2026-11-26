//! JSON as Python's json.dump(..., indent=1, sort_keys=True) writes it.

#[derive(Clone, Debug, PartialEq)]
pub enum J {
    Int(i64),
    Float(f64),
    Str(String),
    List(Vec<J>),
    Obj(Vec<(String, J)>),
}

impl J {
    pub fn obj() -> J {
        J::Obj(Vec::new())
    }

    /// Sets key k (replacing it where it's there).
    pub fn set(&mut self, k: &str, v: J) {
        if let J::Obj(items) = self {
            if let Some(e) = items.iter_mut().find(|(kk, _)| kk == k) {
                e.1 = v;
            } else {
                items.push((k.to_string(), v));
            }
        } else {
            panic!("J::set on a non-object");
        }
    }

    pub fn get(&self, k: &str) -> Option<&J> {
        match self {
            J::Obj(items) => items.iter().find(|(kk, _)| kk == k).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn dump(&self) -> String {
        let mut s = String::new();
        self.write(&mut s, 0);
        s
    }

    fn write(&self, out: &mut String, level: usize) {
        match self {
            J::Int(v) => out.push_str(&v.to_string()),
            J::Float(v) => out.push_str(&py_float(*v)),
            J::Str(s) => py_str(s, out),
            J::List(v) => {
                if v.is_empty() {
                    out.push_str("[]");
                    return;
                }
                out.push('[');
                for (k, x) in v.iter().enumerate() {
                    if k > 0 {
                        out.push(',');
                    }
                    out.push('\n');
                    out.push_str(&" ".repeat(level + 1));
                    x.write(out, level + 1);
                }
                out.push('\n');
                out.push_str(&" ".repeat(level));
                out.push(']');
            }
            J::Obj(items) => {
                if items.is_empty() {
                    out.push_str("{}");
                    return;
                }
                let mut sorted: Vec<&(String, J)> = items.iter().collect();
                sorted.sort_by(|a, b| a.0.cmp(&b.0));
                out.push('{');
                for (k, (key, x)) in sorted.into_iter().enumerate() {
                    if k > 0 {
                        out.push(',');
                    }
                    out.push('\n');
                    out.push_str(&" ".repeat(level + 1));
                    py_str(key, out);
                    out.push_str(": ");
                    x.write(out, level + 1);
                }
                out.push('\n');
                out.push_str(&" ".repeat(level));
                out.push('}');
            }
        }
    }
}

/// Python's repr of a float (the shortest round trip; 5.0, 1e+16).
pub fn py_float(v: f64) -> String {
    if v.is_nan() {
        return "NaN".to_string();
    }
    if v.is_infinite() {
        return if v > 0.0 { "Infinity" } else { "-Infinity" }.to_string();
    }
    let a = v.abs();
    if a != 0.0 && !(1e-4..1e16).contains(&a) {
        let s = format!("{:e}", v);
        let (m, e) = s.split_once('e').unwrap();
        let e: i32 = e.parse().unwrap();
        return format!("{}e{}{:02}", m, if e < 0 { "-" } else { "+" }, e.abs());
    }
    let s = format!("{}", v);
    if s.contains('.') {
        s
    } else {
        s + ".0"
    }
}

/// A JSON string with ensure_ascii, as Python's json writes it.
fn py_str(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 || (c as u32) > 0x7e => {
                let mut buf = [0u16; 2];
                for u in c.encode_utf16(&mut buf) {
                    out.push_str(&format!("\\u{:04x}", u));
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}
