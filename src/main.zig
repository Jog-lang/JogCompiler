const std = @import("std");
const driver = @import("driver/driver.zig");

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
        const path = args.next() orelse {
            try stderr.writeAll("parse: missing file\n");
            std.process.exit(2);
        };
        try driver.cmdParse(allocator, path, stdout);
        return;
    }
    if (std.mem.eql(u8, cmd, "check")) {
        var path: ?[]const u8 = null;
        var diag_json = false;
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "--diag=json")) {
                diag_json = true;
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
        }, diag_json, stdout);
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
        }, stdout);
        return;
    }
    if (std.mem.eql(u8, cmd, "deps")) {
        const path = args.next() orelse ".";
        try driver.cmdDeps(allocator, path, stdout);
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
