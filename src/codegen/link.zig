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
    const rt_scope = try std.fs.path.join(allocator, &.{ rt, "jogrt_scope.c" });
    defer allocator.free(rt_scope);
    const rt_chan = try std.fs.path.join(allocator, &.{ rt, "jogrt_chan.c" });
    defer allocator.free(rt_chan);
    const rt_race = try std.fs.path.join(allocator, &.{ rt, "jogrt_race.c" });
    defer allocator.free(rt_race);
    const rt_io = try std.fs.path.join(allocator, &.{ rt, "jogrt_io.c" });
    defer allocator.free(rt_io);
    const rt_fuzz = try std.fs.path.join(allocator, &.{ rt, "jogrt_fuzz.c" });
    defer allocator.free(rt_fuzz);
    const rt_start = try std.fs.path.join(allocator, &.{ rt, "linux_x64_start.s" });
    defer allocator.free(rt_start);

    var argv: std.ArrayListUnmanaged([]const u8) = .{};
    defer argv.deinit(allocator);
    // -fno-sanitize=all keeps jogrt_scope/chan/race.c freestanding (no ubsan→getauxval).
    try argv.appendSlice(allocator, &.{ "zig", "cc", "-no-pie", "-fno-sanitize=all", "-o", out_path, asm_path, rt_main, rt_coro, rt_scope, rt_chan, rt_race });
    // std.io / std.fs need errno + stdin helpers; only with libc (fopen path).
    if (program.needs_libc) {
        try argv.append(allocator, rt_io);
    }
    // Coverage-guided fuzz runtime (fopen + printf); only when instrumenting.
    if (program.fuzz_instrument) {
        try argv.append(allocator, rt_fuzz);
    }
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

fn runtimeLooksValid(allocator: std.mem.Allocator, rt: []const u8) !bool {
    const main_s = try std.fs.path.join(allocator, &.{ rt, "linux_x64.s" });
    defer allocator.free(main_s);
    std.fs.cwd().access(main_s, .{}) catch {
        // Absolute / non-cwd paths: try absolute access.
        std.fs.accessAbsolute(main_s, .{}) catch return false;
        return true;
    };
    return true;
}

fn findRuntimeDir(allocator: std.mem.Allocator) ![]u8 {
    // Prefer the runtime next to this jog binary (worktree-safe), then cwd.
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    if (std.fs.selfExePath(&buf)) |exe| {
        const dir = std.fs.path.dirname(exe) orelse ".";
        const near_paths = [_][]const u8{ "../../runtime", "../runtime" };
        for (near_paths) |rel| {
            const near = try std.fs.path.join(allocator, &.{ dir, rel });
            errdefer allocator.free(near);
            if (try runtimeLooksValid(allocator, near)) return near;
            allocator.free(near);
        }
    } else |_| {}

    const cwd_candidates = [_][]const u8{ "runtime", "../runtime" };
    for (cwd_candidates) |c| {
        if (try runtimeLooksValid(allocator, c)) return try allocator.dupe(u8, c);
    }

    std.debug.print("jog: runtime/ not found (checked next to jog and cwd)\n", .{});
    std.process.exit(1);
}
