const std = @import("std");
const ast = @import("../ast/ast.zig");

pub const FuzzKind = enum {
    /// Single `bytes` / `const bytes` / `[]const byte` / … — raw corpus bytes.
    raw_bytes,
    /// Multi-param structured decode (Go-style stream).
    structured,
};

pub const ParamKind = enum {
    bytes,
    str,
    bool_ty,
    i8,
    u8,
    i16,
    u16,
    i32,
    u32,
    i64,
    u64,
};

pub const FuzzTarget = struct {
    name: []const u8,
    ncontexts: usize,
    kind: FuzzKind,
    params: []const ParamKind,
    /// Directory of the `*.test.jog` that declared this target (for testdata/).
    test_dir: []const u8,
};

pub fn typeExprParamKind(te: *const ast.TypeExpr) ?ParamKind {
    return switch (te.*) {
        .name => |n| blk: {
            if (std.mem.eql(u8, n, "bytes")) break :blk .bytes;
            if (std.mem.eql(u8, n, "str")) break :blk .str;
            if (std.mem.eql(u8, n, "bool")) break :blk .bool_ty;
            if (std.mem.eql(u8, n, "byte") or std.mem.eql(u8, n, "u8")) break :blk .u8;
            if (std.mem.eql(u8, n, "i8")) break :blk .i8;
            if (std.mem.eql(u8, n, "i16")) break :blk .i16;
            if (std.mem.eql(u8, n, "u16")) break :blk .u16;
            if (std.mem.eql(u8, n, "i32")) break :blk .i32;
            if (std.mem.eql(u8, n, "u32")) break :blk .u32;
            if (std.mem.eql(u8, n, "i64") or std.mem.eql(u8, n, "isize")) break :blk .i64;
            if (std.mem.eql(u8, n, "u64") or std.mem.eql(u8, n, "usize")) break :blk .u64;
            break :blk null;
        },
        .slice_view => |sv| blk: {
            const elem = switch (sv.elem.*) {
                .name => |n| n,
                else => break :blk null,
            };
            if (std.mem.eql(u8, elem, "byte") or std.mem.eql(u8, elem, "u8")) break :blk .bytes;
            break :blk null;
        },
        else => null,
    };
}

pub fn classifyFuzzFn(fd: *const ast.FnDecl) ?struct { FuzzKind, []const ParamKind } {
    if (fd.params.len == 0) return null;
    var kinds_buf: [8]ParamKind = undefined;
    if (fd.params.len > kinds_buf.len) return null;
    for (fd.params, 0..) |p, i| {
        kinds_buf[i] = typeExprParamKind(p.ty) orelse return null;
    }
    const kinds = kinds_buf[0..fd.params.len];
    if (kinds.len == 1 and kinds[0] == .bytes) {
        return .{ .raw_bytes, kinds };
    }
    // Structured: one or more supported scalar/str/bytes params (not a lone raw bytes — already handled).
    return .{ .structured, kinds };
}

pub fn findCorpusDir(allocator: std.mem.Allocator, test_dir: []const u8, name: []const u8) ![]u8 {
    return try std.fs.path.join(allocator, &.{ test_dir, "testdata", "fuzz", name });
}

pub fn loadCorpus(allocator: std.mem.Allocator, corpus_dir: []const u8) ![][]u8 {
    var out: std.ArrayListUnmanaged([]u8) = .{};
    errdefer {
        for (out.items) |s| allocator.free(s);
        out.deinit(allocator);
    }
    var dir = std.fs.cwd().openDir(corpus_dir, .{ .iterate = true }) catch {
        return try out.toOwnedSlice(allocator);
    };
    defer dir.close();
    var it = dir.iterate();
    while (try it.next()) |entry| {
        if (entry.kind != .file) continue;
        // Skip Jog sources if any sneak in; corpus is raw files.
        if (std.mem.endsWith(u8, entry.name, ".jog")) continue;
        const data = dir.readFileAlloc(allocator, entry.name, 1 << 20) catch continue;
        try out.append(allocator, data);
    }
    return try out.toOwnedSlice(allocator);
}

pub fn escapeJogString(allocator: std.mem.Allocator, bytes: []const u8) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .{};
    errdefer out.deinit(allocator);
    try out.append(allocator, '"');
    for (bytes) |b| {
        switch (b) {
            '\n' => try out.appendSlice(allocator, "\\n"),
            '\t' => try out.appendSlice(allocator, "\\t"),
            '\r' => try out.appendSlice(allocator, "\\r"),
            0 => try out.appendSlice(allocator, "\\0"),
            '\\' => try out.appendSlice(allocator, "\\\\"),
            '"' => try out.appendSlice(allocator, "\\\""),
            else => {
                if (b >= 32 and b < 127) {
                    try out.append(allocator, b);
                } else {
                    // Non-printable: emit as raw byte via \0-style fallback — use octal-ish by
                    // repeating a safe ASCII and document binary seeds prefer --fuzz file path.
                    // Keep deterministic: encode as \\x is unsupported; drop to '?'.
                    try out.append(allocator, '?');
                }
            },
        }
    }
    try out.append(allocator, '"');
    return try out.toOwnedSlice(allocator);
}

/// Mutate `buf` in place (must be growable capacity via ArrayList).
pub fn mutate(buf: *std.ArrayListUnmanaged(u8), allocator: std.mem.Allocator, rng: *std.Random) !void {
    if (buf.items.len == 0) {
        try buf.append(allocator, @truncate(rng.int(u8)));
        return;
    }
    const op = rng.intRangeLessThan(u8, 0, 5);
    switch (op) {
        0 => { // bit flip
            const i = rng.intRangeLessThan(usize, 0, buf.items.len);
            buf.items[i] ^= @as(u8, 1) << @as(u3, @truncate(rng.intRangeLessThan(u4, 0, 8)));
        },
        1 => { // byte set
            const i = rng.intRangeLessThan(usize, 0, buf.items.len);
            buf.items[i] = rng.int(u8);
        },
        2 => { // insert
            if (buf.items.len >= 4096) return;
            const i = rng.intRangeAtMost(usize, 0, buf.items.len);
            try buf.insert(allocator, i, rng.int(u8));
        },
        3 => { // delete
            if (buf.items.len <= 1) return;
            const i = rng.intRangeLessThan(usize, 0, buf.items.len);
            _ = buf.orderedRemove(i);
        },
        else => { // append
            if (buf.items.len >= 4096) return;
            try buf.append(allocator, rng.int(u8));
        },
    }
}

pub fn appendDecodeAndCall(
    allocator: std.mem.Allocator,
    harness: *std.ArrayListUnmanaged(u8),
    target: FuzzTarget,
    raw_expr: []const u8,
) !void {
    // `raw_expr` is a Jog expression of type str (string literal or view).
    const w = harness.writer(allocator);
    if (target.kind == .raw_bytes) {
        // str → bytes view via bytes() + ptr/len assign (std.c.fromCStr pattern).
        try w.print("  var __fb = bytes()\n", .{});
        try w.print("  __fb.ptr = ({s}).ptr as *u8\n", .{raw_expr});
        try w.print("  __fb.len = ({s}).len\n", .{raw_expr});
        if (target.ncontexts == 0) {
            try w.print("  {s}(__fb)\n", .{target.name});
        } else {
            try w.print("  {s}(__fb) with (testing())\n", .{target.name});
        }
        return;
    }
    // Structured: consume little-endian integers / length-prefixed str|bytes from a stream.
    try w.print("  val __raw = {s}\n", .{raw_expr});
    try w.print("  var __at: usize = 0\n", .{});
    for (target.params, 0..) |pk, i| {
        try emitDecodeParam(w, pk, i);
    }
    try w.writeAll("  ");
    try w.print("{s}(", .{target.name});
    for (target.params, 0..) |_, i| {
        if (i > 0) try w.writeAll(", ");
        try w.print("__p{d}", .{i});
    }
    try w.writeAll(")");
    if (target.ncontexts > 0) try w.writeAll(" with (testing())");
    try w.writeAll("\n");
}

fn emitDecodeParam(w: anytype, pk: ParamKind, i: usize) !void {
    switch (pk) {
        .bool_ty => {
            try w.print("  var __p{d}: bool = false\n", .{i});
            try w.print("  if __at < __raw.len {{\n", .{});
            try w.print("    __p{d} = __raw.ptr[__at] != 0\n", .{i});
            try w.print("    __at = __at + 1\n", .{});
            try w.print("  }}\n", .{});
        },
        .i8, .u8 => {
            const ty: []const u8 = if (pk == .i8) "i8" else "u8";
            try w.print("  var __p{d}: {s} = 0\n", .{ i, ty });
            try w.print("  if __at < __raw.len {{\n", .{});
            try w.print("    __p{d} = __raw.ptr[__at] as {s}\n", .{ i, ty });
            try w.print("    __at = __at + 1\n", .{});
            try w.print("  }}\n", .{});
        },
        .i16, .u16 => {
            const ty: []const u8 = if (pk == .i16) "i16" else "u16";
            try w.print("  var __p{d}: {s} = 0\n", .{ i, ty });
            try w.print("  if __at < __raw.len {{ __p{d} = __p{d} | ((__raw.ptr[__at] as {s}) << 0); __at = __at + 1 }}\n", .{ i, i, ty });
            try w.print("  if __at < __raw.len {{ __p{d} = __p{d} | ((__raw.ptr[__at] as {s}) << 8); __at = __at + 1 }}\n", .{ i, i, ty });
        },
        .i32, .u32 => {
            const ty: []const u8 = if (pk == .i32) "i32" else "u32";
            try w.print("  var __p{d}: {s} = 0\n", .{ i, ty });
            try w.print("  if __at < __raw.len {{ __p{d} = __p{d} | ((__raw.ptr[__at] as {s}) << 0); __at = __at + 1 }}\n", .{ i, i, ty });
            try w.print("  if __at < __raw.len {{ __p{d} = __p{d} | ((__raw.ptr[__at] as {s}) << 8); __at = __at + 1 }}\n", .{ i, i, ty });
            try w.print("  if __at < __raw.len {{ __p{d} = __p{d} | ((__raw.ptr[__at] as {s}) << 16); __at = __at + 1 }}\n", .{ i, i, ty });
            try w.print("  if __at < __raw.len {{ __p{d} = __p{d} | ((__raw.ptr[__at] as {s}) << 24); __at = __at + 1 }}\n", .{ i, i, ty });
        },
        .i64, .u64 => {
            const ty: []const u8 = if (pk == .i64) "i64" else "u64";
            try w.print("  var __p{d}: {s} = 0\n", .{ i, ty });
            var sh: u6 = 0;
            while (sh < 64) : (sh += 8) {
                try w.print("  if __at < __raw.len {{ __p{d} = __p{d} | ((__raw.ptr[__at] as {s}) << {d}); __at = __at + 1 }}\n", .{ i, i, ty, sh });
            }
        },
        .str, .bytes => {
            // Rest of buffer as the value (simple structured form; length-prefix later).
            if (pk == .str) {
                try w.print("  var __p{d}: str = \"\"\n", .{i});
                try w.print("  if __at <= __raw.len {{\n", .{});
                try w.print("    var __rest = bytes()\n", .{});
                try w.print("    __rest.ptr = (__raw.ptr + __at) as *u8\n", .{});
                try w.print("    __rest.len = __raw.len - __at\n", .{});
                try w.print("    __p{d} = __rest\n", .{i});
                try w.print("    __at = __raw.len\n", .{});
                try w.print("  }}\n", .{});
            } else {
                try w.print("  var __p{d} = bytes()\n", .{i});
                try w.print("  if __at <= __raw.len {{\n", .{});
                try w.print("    __p{d}.ptr = (__raw.ptr + __at) as *u8\n", .{i});
                try w.print("    __p{d}.len = __raw.len - __at\n", .{i});
                try w.print("    __at = __raw.len\n", .{});
                try w.print("  }}\n", .{});
            }
        },
    }
}

/// Emit a one-shot file-input harness body for `--fuzz` (uses jogrt_fuzz_*).
pub fn appendFileInputCall(
    allocator: std.mem.Allocator,
    harness: *std.ArrayListUnmanaged(u8),
    target: FuzzTarget,
) !void {
    const w = harness.writer(allocator);
    try w.writeAll("  val __n = jogrt_fuzz_prepare()\n");
    try w.writeAll("  if __n < 0 {\n");
    try w.writeAll("    panic(\"fuzz: failed to load input\")\n");
    try w.writeAll("  }\n");
    try w.writeAll("  var __view = bytes()\n");
    try w.writeAll("  if __n > 0 {\n");
    try w.writeAll("    __view.ptr = jogrt_fuzz_ptr() as *u8\n");
    try w.writeAll("    __view.len = __n as usize\n");
    try w.writeAll("  }\n");
    // Treat the view as str for decode (same header).
    try appendDecodeAndCall(allocator, harness, target, "__view");
    try w.writeAll("  jogrt_fuzz_report()\n");
    // Flush Testing failures (expect/fail) so the driver sees a nonzero exit.
    try w.writeAll("  finish()\n");
}

test "classify raw bytes" {
    // smoke: typeExprParamKind for name bytes
    var te = ast.TypeExpr{ .name = "bytes" };
    try std.testing.expect(typeExprParamKind(&te) == .bytes);
}

test "mutate grows empty" {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const a = gpa.allocator();
    var buf: std.ArrayListUnmanaged(u8) = .{};
    defer buf.deinit(a);
    var prng = std.Random.DefaultPrng.init(1);
    var rng = prng.random();
    try mutate(&buf, a, &rng);
    try std.testing.expect(buf.items.len >= 1);
}
