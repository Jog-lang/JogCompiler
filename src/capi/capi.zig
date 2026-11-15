//! Minimal C ABI façade for Jog / foreign callers (JOG-97 Phase C).
//! One exported symbol proves Jog `@link` → Zig `.so` end-to-end.
//! Not a full frontend API — see project-store `docs/jog-zig-libs-ffi.md`.

/// Stable ABI version for the façade. Bump when exported symbols change.
pub export fn jogc_abi_version() callconv(.C) u32 {
    return 1;
}
