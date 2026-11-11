const std = @import("std");
const ir = @import("../ir/ir.zig");
const asm_emit = @import("asm.zig");

pub fn buildExecutable(
    allocator: std.mem.Allocator,
    program: *const ir.Program,
    out_path: []const u8,
    keep_asm: bool,
    writer: anytype,
) !void {
    const asm_path = try std.fmt.allocPrint(allocator, "{s}.s", .{out_path});
    defer allocator.free(asm_path);

    {
        const f = try std.fs.cwd().createFile(asm_path, .{});
        defer f.close();
        try asm_emit.emit(allocator, program, f.writer());
    }

    const rt = try findRuntimeDir(allocator);
    defer allocator.free(rt);
    const rt_main = try std.fs.path.join(allocator, &.{ rt, "linux_x64.s" });
    defer allocator.free(rt_main);
    const rt_coro = try std.fs.path.join(allocator, &.{ rt, "linux_x64_coro.s" });
    defer allocator.free(rt_coro);
    const rt_start = try std.fs.path.join(allocator, &.{ rt, "linux_x64_start.s" });
    defer allocator.free(rt_start);

    var argv: std.ArrayListUnmanaged([]const u8) = .{};
    defer argv.deinit(allocator);
    try argv.appendSlice(allocator, &.{ "zig", "cc", "-no-pie", "-o", out_path, asm_path, rt_main, rt_coro });
    if (!program.needs_libc) {
        try argv.appendSlice(allocator, &.{ rt_start, "-nostdlib", "-fno-stack-protector" });
    }

    var child = std.process.Child.init(argv.items, allocator);
    child.stdout_behavior = .Pipe;
    child.stderr_behavior = .Pipe;
    try child.spawn();
    const out = try child.stdout.?.readToEndAlloc(allocator, 1024 * 1024);
    defer allocator.free(out);
    const err = try child.stderr.?.readToEndAlloc(allocator, 1024 * 1024);
    defer allocator.free(err);
    const term = try child.wait();
    if (out.len > 0) try writer.writeAll(out);
    if (err.len > 0) try writer.writeAll(err);
    const code: u8 = switch (term) {
        .Exited => |c| c,
        else => 1,
    };
    if (code != 0) {
        try writer.print("link failed (exit {d})\n", .{code});
        std.process.exit(1);
    }

    if (!keep_asm) {
        std.fs.cwd().deleteFile(asm_path) catch {};
    } else {
        try writer.print("kept asm {s}\n", .{asm_path});
    }

    const mode: []const u8 = if (program.needs_libc) "asm+runtime+libc (C ABI extern)" else "asm+runtime freestanding";
    try writer.print("wrote {s} ({s}, no C intermediate)\n", .{ out_path, mode });
}

fn findRuntimeDir(allocator: std.mem.Allocator) ![]u8 {
    const candidates = [_][]const u8{
        "runtime",
        "../runtime",
        "/home/plague_doc/Documents/Projects/Jog/JogCompiler-v2/runtime",
    };
    for (candidates) |c| {
        var dir = std.fs.cwd().openDir(c, .{}) catch continue;
        dir.close();
        const main_s = try std.fs.path.join(allocator, &.{ c, "linux_x64.s" });
        defer allocator.free(main_s);
        std.fs.cwd().access(main_s, .{}) catch continue;
        return try allocator.dupe(u8, c);
    }
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const exe = std.fs.selfExePath(&buf) catch {
        std.debug.print("jog: runtime/ not found (cwd has no runtime/linux_x64.s)\n", .{});
        std.process.exit(1);
    };
    const dir = std.fs.path.dirname(exe) orelse ".";
    // zig-out/bin → ../../runtime
    const near = try std.fs.path.join(allocator, &.{ dir, "../../runtime" });
    defer allocator.free(near);
    const near_s = try std.fs.path.join(allocator, &.{ near, "linux_x64.s" });
    defer allocator.free(near_s);
    std.fs.cwd().access(near_s, .{}) catch {
        const near2 = try std.fs.path.join(allocator, &.{ dir, "../runtime" });
        return near2;
    };
    return try allocator.dupe(u8, near);
}
