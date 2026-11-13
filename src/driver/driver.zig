const std = @import("std");
const ast = @import("../ast/ast.zig");
const parse = @import("../parse/parse.zig");
const check_mod = @import("../check/check.zig");
const ir = @import("../ir/ir.zig");
const link = @import("../codegen/link.zig");
const mod = @import("../mod/mod.zig");

pub const version_string = "jog 0.2.5-v2 (zig host, ir→asm→native)";

pub fn cmdVersion(writer: anytype) !void {
    try writer.print("{s}\n", .{version_string});
}

/// Parsed Jog unit: one file or a merged module. Keeps source buffers alive
/// because the parser stores string slices into them.
pub const Unit = struct {
    file: ast.File,
    /// Owned source buffers (one per input file).
    srcs: [][]u8,
    /// Per-file ASTs when multi-file; empty for single-file.
    parts: []ast.File,
    /// Merged decls slice owned by `allocator` when parts.len > 0.
    merged_decls: ?[]ast.Decl,
    allocator: std.mem.Allocator,

    pub fn deinit(self: *Unit) void {
        if (self.merged_decls) |d| self.allocator.free(d);
        for (self.parts) |*p| p.deinit();
        if (self.parts.len > 0) {
            self.allocator.free(self.parts);
            // file.arena was never used for nodes
            self.file.arena.deinit();
        } else {
            self.file.deinit();
        }
        for (self.srcs) |s| self.allocator.free(s);
        self.allocator.free(self.srcs);
    }
};

fn loadSingle(allocator: std.mem.Allocator, path: []const u8) !Unit {
    const src = try std.fs.cwd().readFileAlloc(allocator, path, 16 * 1024 * 1024);
    errdefer allocator.free(src);
    var file = try parse.Parser.parseFile(allocator, src);
    errdefer file.deinit();
    const srcs = try allocator.alloc([]u8, 1);
    srcs[0] = src;
    return .{
        .file = file,
        .srcs = srcs,
        .parts = &[_]ast.File{},
        .merged_decls = null,
        .allocator = allocator,
    };
}

fn loadModule(allocator: std.mem.Allocator, path: []const u8, writer: anytype) !Unit {
    var manifest = mod.loadFromPath(allocator, path) catch |err| {
        try writer.print("jog.mod error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer manifest.deinit();

    const paths = mod.resolveSourcePaths(allocator, &manifest) catch |err| {
        try writer.print("jog.mod error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer {
        for (paths) |p| allocator.free(p);
        allocator.free(paths);
    }

    if (paths.len == 0) {
        try writer.writeAll("jog.mod error: no sources\n");
        std.process.exit(1);
    }
    if (paths.len == 1) {
        return loadSingle(allocator, paths[0]) catch |err| {
            try writer.print("parse error ({s}): {s}\n", .{ paths[0], @errorName(err) });
            std.process.exit(1);
        };
    }

    var srcs: std.ArrayListUnmanaged([]u8) = .{};
    errdefer {
        for (srcs.items) |s| allocator.free(s);
        srcs.deinit(allocator);
    }
    var parts: std.ArrayListUnmanaged(ast.File) = .{};
    errdefer {
        for (parts.items) |*p| p.deinit();
        parts.deinit(allocator);
    }
    var decls: std.ArrayListUnmanaged(ast.Decl) = .{};
    errdefer decls.deinit(allocator);

    for (paths) |p| {
        const src = std.fs.cwd().readFileAlloc(allocator, p, 16 * 1024 * 1024) catch |err| {
            try writer.print("read error ({s}): {s}\n", .{ p, @errorName(err) });
            std.process.exit(1);
        };
        try srcs.append(allocator, src);
        const file = parse.Parser.parseFile(allocator, src) catch |err| {
            try writer.print("parse error ({s}): {s}\n", .{ p, @errorName(err) });
            std.process.exit(1);
        };
        try decls.appendSlice(allocator, file.decls);
        try parts.append(allocator, file);
    }

    const merged = try decls.toOwnedSlice(allocator);
    const parts_slice = try parts.toOwnedSlice(allocator);
    const srcs_slice = try srcs.toOwnedSlice(allocator);

    return .{
        .file = .{
            .decls = merged,
            .arena = std.heap.ArenaAllocator.init(allocator),
        },
        .srcs = srcs_slice,
        .parts = parts_slice,
        .merged_decls = merged,
        .allocator = allocator,
    };
}

pub fn loadPath(allocator: std.mem.Allocator, path: []const u8, writer: anytype) !Unit {
    if (mod.isModulePath(path)) {
        return loadModule(allocator, path, writer);
    }
    return loadSingle(allocator, path) catch |err| {
        try writer.print("parse error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
}

/// One JSON object per line (v1 LSP contract).
fn writeDiag(allocator: std.mem.Allocator, writer: anytype, file: []const u8, line: u32, column: u32, severity: []const u8, message: []const u8) !void {
    var buf: std.ArrayListUnmanaged(u8) = .{};
    defer buf.deinit(allocator);
    try std.json.stringify(.{
        .file = file,
        .line = line,
        .column = column,
        .severity = severity,
        .message = message,
    }, .{}, buf.writer(allocator));
    try writer.writeAll(buf.items);
    try writer.writeAll("\n");
}

pub fn cmdParse(allocator: std.mem.Allocator, path: []const u8, writer: anytype) !void {
    var unit = try loadPath(allocator, path, writer);
    defer unit.deinit();
    try writer.print("ok: {d} declaration(s)\n", .{unit.file.decls.len});
    for (unit.file.decls) |d| {
        switch (d) {
            .type_alias => |t| try writer.print("  type {s}\n", .{t.name}),
            .struct_decl => |s| try writer.print("  struct {s}\n", .{s.name}),
            .interface_fn => |i| try writer.print("  interface fn {s}\n", .{i.name}),
            .interface_struct => |i| try writer.print("  interface struct {s}\n", .{i.name}),
            .fn_decl => |f| try writer.print("  fn {s}\n", .{f.name}),
            .method => |m| try writer.print("  method {s}.{s}\n", .{ m.type_name, m.name }),
            .extern_block => |eb| {
                for (eb.fns) |ef| try writer.print("  extern fn {s}\n", .{ef.name});
            },
            .expect_fn => |e| try writer.print("  expect fn {s}\n", .{e.name}),
            .actual_fn => |f| try writer.print("  actual fn {s}\n", .{f.name}),
        }
    }
}

pub fn cmdCheck(allocator: std.mem.Allocator, path: []const u8, diag_json: bool, writer: anytype) !void {
    var unit = try loadPath(allocator, path, writer);
    defer unit.deinit();
    var result = try check_mod.check(allocator, &unit.file);
    defer result.deinit();
    if (!result.ok) {
        for (result.diagnostics) |d| {
            if (diag_json) {
                try writeDiag(allocator, writer, path, d.line, d.col, "error", d.message);
            } else {
                try writer.print("error: {s}\n", .{d.message});
            }
        }
        std.process.exit(1);
    }
    if (!diag_json) {
        try writer.print("check: ok\n", .{});
    }
}

pub fn cmdBuild(allocator: std.mem.Allocator, path: []const u8, out_path: []const u8, keep_asm: bool, cli_libdirs: []const []const u8, writer: anytype) !void {
    var unit = try loadPath(allocator, path, writer);
    defer unit.deinit();
    var result = try check_mod.check(allocator, &unit.file);
    defer result.deinit();
    if (!result.ok) {
        for (result.diagnostics) |d| {
            try writer.print("error: {s}\n", .{d.message});
        }
        std.process.exit(1);
    }
    if (!result.has_main) {
        try writer.print("error: no fn main\n", .{});
        std.process.exit(1);
    }

    var program = ir.lower(allocator, &unit.file, &result) catch |err| {
        try writer.print("lower error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer program.deinit();

    // Merge jog.mod link/libdir with CLI --libdir into the program (arena-owned).
    try mergeLinkMeta(allocator, &program, path, cli_libdirs);

    if (std.fs.path.dirname(out_path)) |dir| {
        std.fs.cwd().makePath(dir) catch {};
    }

    try link.buildExecutable(allocator, &program, out_path, keep_asm, writer);
}

fn mergeLinkMeta(allocator: std.mem.Allocator, program: *ir.Program, path: []const u8, cli_libdirs: []const []const u8) !void {
    const a = program.arena.allocator();
    var libs: std.ArrayListUnmanaged([]const u8) = .{};
    var dirs: std.ArrayListUnmanaged([]const u8) = .{};
    for (program.link_libs) |l| try libs.append(a, l);
    for (program.libdirs) |d| try dirs.append(a, d);

    if (mod.isModulePath(path)) {
        if (mod.loadFromPath(allocator, path)) |manifest_val| {
            var m = manifest_val;
            defer m.deinit();
            for (m.link_libs) |l| {
                if (!containsStr(libs.items, l)) try libs.append(a, try a.dupe(u8, l));
            }
            if (mod.resolveLibdirs(allocator, &m)) |resolved| {
                defer {
                    for (resolved) |p| allocator.free(p);
                    allocator.free(resolved);
                }
                for (resolved) |d| {
                    if (!containsStr(dirs.items, d)) try dirs.append(a, try a.dupe(u8, d));
                }
            } else |_| {}
        } else |_| {}
    }
    for (cli_libdirs) |d| {
        if (!containsStr(dirs.items, d)) try dirs.append(a, try a.dupe(u8, d));
    }
    program.link_libs = try libs.toOwnedSlice(a);
    program.libdirs = try dirs.toOwnedSlice(a);
}

fn containsStr(hay: []const []const u8, needle: []const u8) bool {
    for (hay) |e| {
        if (std.mem.eql(u8, e, needle)) return true;
    }
    return false;
}

pub fn cmdDeps(allocator: std.mem.Allocator, path: []const u8, writer: anytype) !void {
    var manifest = mod.loadFromPath(allocator, path) catch |err| {
        try writer.print("deps: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer manifest.deinit();

    const paths = try mod.resolveSourcePaths(allocator, &manifest);
    defer {
        for (paths) |p| allocator.free(p);
        allocator.free(paths);
    }
    for (paths) |p| {
        try writer.print("{s}\n", .{p});
    }
}

pub fn usage(writer: anytype) !void {
    try writer.writeAll(
        \\usage: jog <command> [args]
        \\
        \\commands:
        \\  version                 print version
        \\  parse <path>            parse and list declarations
        \\  check [--diag=json] <path>
        \\                          typecheck subset (JSON diags for LSP)
        \\  build <path> -o OUT [--keep-asm] [--libdir DIR | -L DIR]...
        \\                          IR → asm → native link (no C intermediate)
        \\                          @link / jog.mod link.* → -l; libdir → -L + rpath
        \\  deps [path]             print module source paths (jog.mod)
        \\  help                    this message
        \\
        \\path may be a .jog file, a directory with jog.mod, or a jog.mod file.
        \\
    );
}
