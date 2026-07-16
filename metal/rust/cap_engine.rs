// ============================================================================
// OUTRUN OS — rust/cap_engine.rs   (compiled by rustc, linked into the kernel)
// ============================================================================
// A real Rust translation unit in the bootable image. It cannot use `core`
// (no precompiled core for x86_64-unknown-none here), so it is #![no_core] and
// declares the handful of lang items it needs itself. Everything below is
// genuine rustc-emitted machine code — the FNV multiplies become `imul`, the
// capability test becomes `and`/`cmp` — callable from C as plain extern "C".
//
// Role in the OS: content-addressing (the CAS filesystem's block hash) and the
// capability bitset check that gates hardware passthrough — the two invariants
// the manifesto insists live in memory-safe code.
// ============================================================================
#![no_std]
#![no_core]
#![feature(no_core, lang_items, auto_traits)]
#![allow(internal_features)]

/* rustc >= 1.89 splits `Sized` into a hierarchy and demands all three lang
 * items even in #![no_core]; `receiver` was likewise renamed to
 * `legacy_receiver`. Nothing about codegen changes — these remain marker
 * traits the compiler wants declared. */
#[lang = "pointee_sized"] pub trait PointeeSized {}
#[lang = "meta_sized"]    pub trait MetaSized: PointeeSized {}
#[lang = "sized"]         pub trait Sized: MetaSized {}
#[lang = "copy"]     pub trait Copy {}
#[lang = "freeze"]   pub unsafe auto trait Freeze {}
#[lang = "legacy_receiver"] pub trait Receiver {}
impl<T: ?Sized> Receiver for &T {}
impl Copy for u8 {} impl Copy for u32 {} impl Copy for u64 {} impl Copy for usize {}

#[lang = "add"]    pub trait Add<R = Self>    { type Output; fn add(self, r: R) -> Self::Output; }
#[lang = "mul"]    pub trait Mul<R = Self>    { type Output; fn mul(self, r: R) -> Self::Output; }
#[lang = "bitxor"] pub trait BitXor<R = Self> { type Output; fn bitxor(self, r: R) -> Self::Output; }
#[lang = "bitand"] pub trait BitAnd<R = Self> { type Output; fn bitand(self, r: R) -> Self::Output; }
#[lang = "eq"]     pub trait PartialEq<R: ?Sized = Self> { fn eq(&self, o: &R) -> bool; fn ne(&self, o: &R) -> bool; }

impl Add    for usize { type Output = usize; fn add(self, r: usize) -> usize { self + r } }
impl Add    for u64   { type Output = u64;   fn add(self, r: u64) -> u64 { self + r } }
impl Mul    for u64   { type Output = u64;   fn mul(self, r: u64) -> u64 { self * r } }
impl BitXor for u64   { type Output = u64;   fn bitxor(self, r: u64) -> u64 { self ^ r } }
impl BitAnd for u64   { type Output = u64;   fn bitand(self, r: u64) -> u64 { self & r } }
impl PartialEq for usize { fn eq(&self, o: &usize) -> bool { *self == *o } fn ne(&self, o: &usize) -> bool { *self != *o } }
impl PartialEq for u64   { fn eq(&self, o: &u64)   -> bool { *self == *o } fn ne(&self, o: &u64)   -> bool { *self != *o } }

/// FNV-1a over a byte range addressed purely by integer arithmetic (no pointer
/// helper methods, which live in core). This is the CAS block hash.
#[no_mangle]
pub extern "C" fn rust_cas_hash(base: usize, len: usize) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    let mut i: usize = 0;
    while i != len {
        let b: u8 = unsafe { *((base + i) as *const u8) };
        h = (h ^ (b as u64)).mul(0x100000001b3);
        i = i + 1;
    }
    h
}

/// Capability gate: returns 1 iff `caps` holds every bit in `required`.
/// This is the exact test the kernel's hardware passthrough relies on.
#[no_mangle]
pub extern "C" fn rust_cap_check(caps: u64, required: u64) -> u32 {
    if (caps & required) == required { 1 } else { 0 }
}
