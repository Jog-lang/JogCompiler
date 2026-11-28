//! libclang C API via @cImport. Linked only into `jog-bindgen`, not the compiler.

const std = @import("std");

pub const c = @cImport({
    @cInclude("clang-c/Index.h");
});

pub fn cString(s: c.CXString) []const u8 {
    const p = c.clang_getCString(s);
    if (p == null) return "";
    return std.mem.span(p);
}

pub fn disposeString(s: c.CXString) void {
    c.clang_disposeString(s);
}

pub fn spellingOwned(allocator: std.mem.Allocator, s: c.CXString) ![]u8 {
    defer disposeString(s);
    return try allocator.dupe(u8, cString(s));
}
