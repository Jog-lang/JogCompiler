const std = @import("std");
const ast = @import("../ast/ast.zig");
const check = @import("../check/check.zig");

pub const Op = enum {
    nop,
    /// write(fd, ptr, len) then exit(code) — program entry for hello-style
    sys_write_exit,
};

pub const Program = struct {
    ops: []Inst,
    /// rodata payload for the message, if any
    message: []const u8,
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *Program) void {
        self.arena.deinit();
    }
};

pub const Inst = struct {
    op: Op,
    /// for sys_write_exit: message is in Program.message
    exit_code: u8 = 0,
};

pub fn lower(parent: std.mem.Allocator, file: *const ast.File, cr: *const check.CheckResult) !Program {
    var program: Program = .{
        .ops = &[_]Inst{},
        .message = "",
        .arena = std.heap.ArenaAllocator.init(parent),
    };
    errdefer program.deinit();
    const a = program.arena.allocator();

    var ops: std.ArrayListUnmanaged(Inst) = .{};
    _ = file;

    if (cr.main_prints_string) |msg| {
        try ops.append(a, .{ .op = .sys_write_exit, .exit_code = 0 });
        program.ops = try ops.toOwnedSlice(a);
        program.message = try a.dupe(u8, msg);
        return program;
    }

    // Fallback: empty main → exit 0
    if (cr.has_main) {
        try ops.append(a, .{ .op = .sys_write_exit, .exit_code = 0 });
        program.ops = try ops.toOwnedSlice(a);
        program.message = try a.dupe(u8, "");
        return program;
    }

    try ops.append(a, .{ .op = .nop });
    program.ops = try ops.toOwnedSlice(a);
    return program;
}
