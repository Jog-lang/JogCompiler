const std = @import("std");
const parse = @import("../parse/parse.zig");
const check_mod = @import("../check/check.zig");
const ir = @import("../ir/ir.zig");
const link = @import("../codegen/link.zig");

pub const version_string = "jog 0.2.2-v2 (zig host, ir→asm→native)";

pub fn cmdVersion(writer: anytype) !void {
    try writer.print("{s}\n", .{version_string});
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
    const src = try std.fs.cwd().readFileAlloc(allocator, path, 16 * 1024 * 1024);
    defer allocator.free(src);
    var file = parse.Parser.parseFile(allocator, src) catch |err| {
        try writer.print("parse error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer file.deinit();
    try writer.print("ok: {d} declaration(s)\n", .{file.decls.len});
    for (file.decls) |d| {
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
        }
    }
}

pub fn cmdCheck(allocator: std.mem.Allocator, path: []const u8, diag_json: bool, writer: anytype) !void {
    const src = try std.fs.cwd().readFileAlloc(allocator, path, 16 * 1024 * 1024);
    defer allocator.free(src);
    var file = parse.Parser.parseFile(allocator, src) catch |err| {
        if (diag_json) {
            try writeDiag(allocator, writer, path, 1, 1, "error", try std.fmt.allocPrint(allocator, "parse error: {s}", .{@errorName(err)}));
        } else {
            try writer.print("parse error: {s}\n", .{@errorName(err)});
        }
        std.process.exit(1);
    };
    defer file.deinit();
    var result = try check_mod.check(allocator, &file);
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

pub fn cmdBuild(allocator: std.mem.Allocator, path: []const u8, out_path: []const u8, keep_asm: bool, writer: anytype) !void {
    const src = try std.fs.cwd().readFileAlloc(allocator, path, 16 * 1024 * 1024);
    defer allocator.free(src);
    var file = parse.Parser.parseFile(allocator, src) catch |err| {
        try writer.print("parse error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer file.deinit();
    var result = try check_mod.check(allocator, &file);
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

    var program = ir.lower(allocator, &file, &result) catch |err| {
        try writer.print("lower error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer program.deinit();

    if (std.fs.path.dirname(out_path)) |dir| {
        std.fs.cwd().makePath(dir) catch {};
    }

    try link.buildExecutable(allocator, &program, out_path, keep_asm, writer);
}

pub fn usage(writer: anytype) !void {
    try writer.writeAll(
        \\usage: jog <command> [args]
        \\
        \\commands:
        \\  version                 print version
        \\  parse <file>            parse and list declarations
        \\  check [--diag=json] <file>
        \\                          typecheck subset (JSON diags for LSP)
        \\  build <file> -o OUT [--keep-asm]
        \\                          IR → asm → native link (no C intermediate)
        \\  help                    this message
        \\
    );
}
