const std = @import("std");
const driver = @import("driver/driver.zig");

fn parseModuleFlag(allocator: std.mem.Allocator, raw: []const u8) !driver.ModuleMapping {
    const eq = std.mem.indexOfScalar(u8, raw, '=') orelse {
        return error.BadModuleFlag;
    };
    const name = raw[0..eq];
    const dir = raw[eq + 1 ..];
    if (name.len == 0 or dir.len == 0) return error.BadModuleFlag;
    return .{
        .name = try allocator.dupe(u8, name),
        .dir = try allocator.dupe(u8, dir),
    };
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var args = try std.process.argsWithAllocator(allocator);
    defer args.deinit();
    _ = args.skip(); // argv0

    const stdout = std.io.getStdOut().writer();
    const stderr = std.io.getStdErr().writer();

    const cmd = args.next() orelse {
        try driver.usage(stderr);
        std.process.exit(2);
    };

    if (std.mem.eql(u8, cmd, "version") or std.mem.eql(u8, cmd, "--version")) {
        try driver.cmdVersion(stdout);
        return;
    }
    if (std.mem.eql(u8, cmd, "help") or std.mem.eql(u8, cmd, "-h") or std.mem.eql(u8, cmd, "--help")) {
        try driver.usage(stdout);
        return;
    }
    if (std.mem.eql(u8, cmd, "parse")) {
        var path: ?[]const u8 = null;
        var modules: std.ArrayListUnmanaged(driver.ModuleMapping) = .{};
        defer {
            for (modules.items) |m| {
                allocator.free(m.name);
                allocator.free(m.dir);
            }
            modules.deinit(allocator);
        }
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "--module")) {
                const v = args.next() orelse {
                    try stderr.writeAll("parse: --module needs name=dir\n");
                    std.process.exit(2);
                };
                const m = parseModuleFlag(allocator, v) catch {
                    try stderr.writeAll("parse: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "--module=")) {
                const m = parseModuleFlag(allocator, a["--module=".len..]) catch {
                    try stderr.writeAll("parse: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "-")) {
                try stderr.print("parse: unknown flag {s}\n", .{a});
                std.process.exit(2);
            } else if (path == null) {
                path = a;
            } else {
                try stderr.print("parse: unexpected arg {s}\n", .{a});
                std.process.exit(2);
            }
        }
        try driver.cmdParse(allocator, path orelse {
            try stderr.writeAll("parse: missing file\n");
            std.process.exit(2);
        }, stdout, .{ .modules = modules.items });
        return;
    }
    if (std.mem.eql(u8, cmd, "symbols")) {
        var path: ?[]const u8 = null;
        var json = false;
        var modules: std.ArrayListUnmanaged(driver.ModuleMapping) = .{};
        defer {
            for (modules.items) |m| {
                allocator.free(m.name);
                allocator.free(m.dir);
            }
            modules.deinit(allocator);
        }
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "--json")) {
                json = true;
            } else if (std.mem.eql(u8, a, "--module")) {
                const v = args.next() orelse {
                    try stderr.writeAll("symbols: --module needs name=dir\n");
                    std.process.exit(2);
                };
                const m = parseModuleFlag(allocator, v) catch {
                    try stderr.writeAll("symbols: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "--module=")) {
                const m = parseModuleFlag(allocator, a["--module=".len..]) catch {
                    try stderr.writeAll("symbols: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "-")) {
                try stderr.print("symbols: unknown flag {s}\n", .{a});
                std.process.exit(2);
            } else if (path == null) {
                path = a;
            } else {
                try stderr.print("symbols: unexpected arg {s}\n", .{a});
                std.process.exit(2);
            }
        }
        try driver.cmdSymbols(allocator, path orelse {
            try stderr.writeAll("symbols: missing file\n");
            std.process.exit(2);
        }, json, stdout, .{ .modules = modules.items });
        return;
    }
    if (std.mem.eql(u8, cmd, "check")) {
        var path: ?[]const u8 = null;
        var diag_json = false;
        var include_test = false;
        var modules: std.ArrayListUnmanaged(driver.ModuleMapping) = .{};
        defer {
            for (modules.items) |m| {
                allocator.free(m.name);
                allocator.free(m.dir);
            }
            modules.deinit(allocator);
        }
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "--diag=json")) {
                diag_json = true;
            } else if (std.mem.eql(u8, a, "--test")) {
                include_test = true;
            } else if (std.mem.eql(u8, a, "--module")) {
                const v = args.next() orelse {
                    try stderr.writeAll("check: --module needs name=dir\n");
                    std.process.exit(2);
                };
                const m = parseModuleFlag(allocator, v) catch {
                    try stderr.writeAll("check: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "--module=")) {
                const m = parseModuleFlag(allocator, a["--module=".len..]) catch {
                    try stderr.writeAll("check: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "-")) {
                try stderr.print("check: unknown flag {s}\n", .{a});
                std.process.exit(2);
            } else if (path == null) {
                path = a;
            } else {
                try stderr.print("check: unexpected arg {s}\n", .{a});
                std.process.exit(2);
            }
        }
        try driver.cmdCheck(allocator, path orelse {
            try stderr.writeAll("check: missing file\n");
            std.process.exit(2);
        }, diag_json, stdout, .{
            .modules = modules.items,
            .include_test_requires = include_test,
        });
        return;
    }
    if (std.mem.eql(u8, cmd, "build")) {
        const link = @import("codegen/link.zig");
        var path: ?[]const u8 = null;
        var out_path: ?[]const u8 = null;
        var keep_asm = false;
        var keep_c = false;
        var target: link.Target = .native;
        var libdirs: std.ArrayListUnmanaged([]const u8) = .{};
        defer libdirs.deinit(allocator);
        var modules: std.ArrayListUnmanaged(driver.ModuleMapping) = .{};
        defer {
            for (modules.items) |m| {
                allocator.free(m.name);
                allocator.free(m.dir);
            }
            modules.deinit(allocator);
        }
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "-o")) {
                out_path = args.next() orelse {
                    try stderr.writeAll("build: -o needs a path\n");
                    std.process.exit(2);
                };
            } else if (std.mem.eql(u8, a, "--keep-asm")) {
                keep_asm = true;
            } else if (std.mem.eql(u8, a, "--keep-c")) {
                keep_c = true;
            } else if (std.mem.eql(u8, a, "--target")) {
                const t = args.next() orelse {
                    try stderr.writeAll("build: --target needs wasm32-emscripten\n");
                    std.process.exit(2);
                };
                if (std.mem.eql(u8, t, "wasm32-emscripten")) {
                    target = .wasm32_emscripten;
                } else {
                    try stderr.print("build: unsupported --target {s} (v2: wasm32-emscripten)\n", .{t});
                    std.process.exit(2);
                }
            } else if (std.mem.startsWith(u8, a, "--target=")) {
                const t = a["--target=".len..];
                if (std.mem.eql(u8, t, "wasm32-emscripten")) {
                    target = .wasm32_emscripten;
                } else {
                    try stderr.print("build: unsupported --target {s} (v2: wasm32-emscripten)\n", .{t});
                    std.process.exit(2);
                }
            } else if (std.mem.eql(u8, a, "--libdir") or std.mem.eql(u8, a, "-L")) {
                const dir = args.next() orelse {
                    try stderr.writeAll("build: --libdir needs a directory\n");
                    std.process.exit(2);
                };
                try libdirs.append(allocator, dir);
            } else if (std.mem.startsWith(u8, a, "--libdir=")) {
                try libdirs.append(allocator, a["--libdir=".len..]);
            } else if (std.mem.startsWith(u8, a, "-L") and a.len > 2) {
                try libdirs.append(allocator, a[2..]);
            } else if (std.mem.eql(u8, a, "--module")) {
                const v = args.next() orelse {
                    try stderr.writeAll("build: --module needs name=dir\n");
                    std.process.exit(2);
                };
                const m = parseModuleFlag(allocator, v) catch {
                    try stderr.writeAll("build: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "--module=")) {
                const m = parseModuleFlag(allocator, a["--module=".len..]) catch {
                    try stderr.writeAll("build: --module needs name=dir\n");
                    std.process.exit(2);
                };
                try modules.append(allocator, m);
            } else if (std.mem.startsWith(u8, a, "-")) {
                try stderr.print("build: unknown arg {s}\n", .{a});
                std.process.exit(2);
            } else if (path == null) {
                path = a;
            } else {
                try stderr.print("build: unexpected arg {s}\n", .{a});
                std.process.exit(2);
            }
        }
        const default_out: []const u8 = if (target == .wasm32_emscripten) "a.out.html" else "a.out";
        try driver.cmdBuildOpts(allocator, path orelse {
            try stderr.writeAll("build: missing file\n");
            std.process.exit(2);
        }, out_path orelse default_out, .{
            .keep_asm = keep_asm,
            .keep_c = keep_c,
            .target = target,
            .cli_libdirs = libdirs.items,
            .modules = modules.items,
        }, stdout);
        return;
    }
    if (std.mem.eql(u8, cmd, "deps")) {
        var path: []const u8 = ".";
        var flags = false;
        var include_test = false;
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "--flags")) {
                flags = true;
            } else if (std.mem.eql(u8, a, "--test")) {
                include_test = true;
            } else if (std.mem.startsWith(u8, a, "-")) {
                try stderr.print("deps: unknown flag {s}\n", .{a});
                std.process.exit(2);
            } else {
                path = a;
            }
        }
        try driver.cmdDepsOpts(allocator, path, stdout, flags, include_test);
        return;
    }

    try stderr.print("unknown command: {s}\n", .{cmd});
    try driver.usage(stderr);
    std.process.exit(2);
}

test {
    _ = @import("lex/lex.zig");
    _ = @import("check/check.zig");
    _ = @import("codegen/asm.zig");
    _ = @import("mod/mod.zig");
}
