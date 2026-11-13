const std = @import("std");
const ir = @import("../ir/ir.zig");
const asm_emit = @import("asm.zig");
const c_emit = @import("c.zig");

pub const Target = enum {
    native,
    wasm32_emscripten,
};

pub fn buildExecutable(
    allocator: std.mem.Allocator,
    program: *const ir.Program,
    out_path: []const u8,
    keep_asm: bool,
    writer: anytype,
) !void {
    try buildExecutableTargeted(allocator, program, out_path, keep_asm, false, .native, writer);
}

pub fn buildExecutableTargeted(
    allocator: std.mem.Allocator,
    program: *const ir.Program,
    out_path: []const u8,
    keep_asm: bool,
    keep_c: bool,
    target: Target,
    writer: anytype,
) !void {
    switch (target) {
        .native => try buildNative(allocator, program, out_path, keep_asm, writer),
        .wasm32_emscripten => try buildEmscripten(allocator, program, out_path, keep_c, writer),
    }
}

fn buildNative(
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

    var owned_flags: std.ArrayListUnmanaged([]u8) = .{};
    defer {
        for (owned_flags.items) |s| allocator.free(s);
        owned_flags.deinit(allocator);
    }

    for (program.libdirs) |dir| {
        const abs = try absPath(allocator, dir);
        defer allocator.free(abs);
        const lflag = try std.fmt.allocPrint(allocator, "-L{s}", .{abs});
        try owned_flags.append(allocator, lflag);
        try argv.append(allocator, lflag);
        const rflag = try std.fmt.allocPrint(allocator, "-Wl,-rpath,{s}", .{abs});
        try owned_flags.append(allocator, rflag);
        try argv.append(allocator, rflag);
    }
    for (program.link_libs) |lib| {
        const flag = try std.fmt.allocPrint(allocator, "-l{s}", .{lib});
        try owned_flags.append(allocator, flag);
        try argv.append(allocator, flag);
    }

    try runTool(allocator, argv.items, writer);
    if (!keep_asm) {
        std.fs.cwd().deleteFile(asm_path) catch {};
    } else {
        try writer.print("kept asm {s}\n", .{asm_path});
    }

    const mode: []const u8 = if (program.needs_libc) "asm+runtime+libc (C ABI extern)" else "asm+runtime freestanding";
    try writer.print("wrote {s} ({s}, no C intermediate)\n", .{ out_path, mode });
}

fn buildEmscripten(
    allocator: std.mem.Allocator,
    program: *const ir.Program,
    out_path: []const u8,
    keep_c: bool,
    writer: anytype,
) !void {
    // Normalize output to .html / .js (emcc default shell).
    const final_out = try normalizeEmscriptenOut(allocator, out_path);
    defer allocator.free(final_out);

    const c_path = try std.fmt.allocPrint(allocator, "{s}.c", .{stripExt(final_out)});
    defer allocator.free(c_path);

    {
        if (std.fs.path.dirname(c_path)) |dir| {
            std.fs.cwd().makePath(dir) catch {};
        }
        const f = try std.fs.cwd().createFile(c_path, .{});
        defer f.close();
        try c_emit.emit(allocator, program, f.writer());
    }

    const rt = try findRuntimeDir(allocator);
    defer allocator.free(rt);
    const em_dir = try std.fs.path.join(allocator, &.{ rt, "emscripten" });
    defer allocator.free(em_dir);
    const jogrt_c = try std.fs.path.join(allocator, &.{ em_dir, "jogrt.c" });
    defer allocator.free(jogrt_c);
    const jogrt_ml = try std.fs.path.join(allocator, &.{ em_dir, "jogrt_mainloop.c" });
    defer allocator.free(jogrt_ml);
    const jogrt_h_dir = em_dir;

    const emcc = try findEmcc(allocator);
    defer allocator.free(emcc);

    var argv: std.ArrayListUnmanaged([]const u8) = .{};
    defer argv.deinit(allocator);
    try argv.append(allocator, emcc);
    try argv.appendSlice(allocator, &.{
        "-O2",
        "-o",
        final_out,
        c_path,
        jogrt_c,
        jogrt_ml,
        "-I",
        jogrt_h_dir,
        "-sALLOW_MEMORY_GROWTH=1",
        "-sFULL_ES2=1",
        "-sUSE_WEBGL2=0",
    });

    var owned_flags: std.ArrayListUnmanaged([]u8) = .{};
    defer {
        for (owned_flags.items) |s| allocator.free(s);
        owned_flags.deinit(allocator);
    }

    for (program.libdirs) |dir| {
        const abs = try absPath(allocator, dir);
        defer allocator.free(abs);
        const lflag = try std.fmt.allocPrint(allocator, "-L{s}", .{abs});
        try owned_flags.append(allocator, lflag);
        try argv.append(allocator, lflag);
    }
    for (program.link_libs) |lib| {
        const flag = try std.fmt.allocPrint(allocator, "-l{s}", .{lib});
        try owned_flags.append(allocator, flag);
        try argv.append(allocator, flag);
    }

    try runTool(allocator, argv.items, writer);

    if (!keep_c) {
        std.fs.cwd().deleteFile(c_path) catch {};
    } else {
        try writer.print("kept c {s}\n", .{c_path});
    }
    try writer.print("wrote {s} (IR→C→emcc, wasm32-emscripten)\n", .{final_out});
}

fn normalizeEmscriptenOut(allocator: std.mem.Allocator, out_path: []const u8) ![]u8 {
    if (std.mem.endsWith(u8, out_path, ".html") or std.mem.endsWith(u8, out_path, ".js")) {
        return try allocator.dupe(u8, out_path);
    }
    if (std.mem.endsWith(u8, out_path, ".wasm")) {
        return try std.fmt.allocPrint(allocator, "{s}.html", .{out_path[0 .. out_path.len - 5]});
    }
    return try std.fmt.allocPrint(allocator, "{s}.html", .{out_path});
}

fn stripExt(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '.')) |dot| {
        // Keep directory dots; only strip final extension.
        if (std.mem.lastIndexOfScalar(u8, path, '/')) |slash| {
            if (dot > slash) return path[0..dot];
            return path;
        }
        return path[0..dot];
    }
    return path;
}

fn findEmcc(allocator: std.mem.Allocator) ![]u8 {
    if (std.posix.getenv("EMCC")) |e| {
        if (e.len > 0) return try allocator.dupe(u8, e);
    }
    // Common local install on this machine.
    const candidates = [_][]const u8{
        "/home/plague_doc/emsdk/upstream/emscripten/emcc",
        "emcc",
    };
    for (candidates) |c| {
        if (std.mem.eql(u8, c, "emcc")) continue;
        std.fs.accessAbsolute(c, .{}) catch continue;
        return try allocator.dupe(u8, c);
    }
    return try allocator.dupe(u8, "emcc");
}

fn runTool(allocator: std.mem.Allocator, argv: []const []const u8, writer: anytype) !void {
    var child = std.process.Child.init(argv, allocator);
    child.stdout_behavior = .Pipe;
    child.stderr_behavior = .Pipe;
    child.spawn() catch |err| {
        try writer.print("cannot run '{s}': {s}\n", .{ argv[0], @errorName(err) });
        if (std.mem.indexOf(u8, argv[0], "emcc") != null) {
            try writer.writeAll("emcc not found; install emsdk and put emcc on PATH (or set EMCC)\n");
        }
        std.process.exit(1);
    };
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
}

fn absPath(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    if (std.fs.path.isAbsolute(path)) return try allocator.dupe(u8, path);
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const cwd = try std.fs.cwd().realpath(".", &buf);
    return try std.fs.path.join(allocator, &.{ cwd, path });
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
