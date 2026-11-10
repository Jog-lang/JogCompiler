const std = @import("std");
const parse = @import("../parse/parse.zig");
const check_mod = @import("../check/check.zig");
const ir = @import("../ir/ir.zig");
const elf = @import("../codegen/elf.zig");

pub const version_string = "jog 0.2.0-v2 (zig host, linux-x64 native)";

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
        const name = switch (d) {
            .type_alias => |t| t.name,
            .struct_decl => |s| s.name,
            .interface_fn => |i| i.name,
            .interface_struct => |i| i.name,
            .fn_decl => |f| f.name,
            .method => |m| m.name,
        };
        try writer.print("  {s}\n", .{name});
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

pub fn cmdBuild(allocator: std.mem.Allocator, path: []const u8, out_path: []const u8, writer: anytype) !void {
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

    var program = try ir.lower(allocator, &file, &result);
    defer program.deinit();

    if (std.fs.path.dirname(out_path)) |dir| {
        std.fs.cwd().makePath(dir) catch {};
    }

    const out_file = try std.fs.cwd().createFile(out_path, .{ .mode = 0o755 });
    defer out_file.close();

    try elf.emitLinuxX64(allocator, &program, out_file.writer());
    try writer.print("wrote {s} (native ELF, no C intermediate)\n", .{out_path});
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
        \\  build <file> -o OUT     emit linux x64 ELF
        \\  help                    this message
        \\
    );
}
