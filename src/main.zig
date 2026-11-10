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
        var path: ?[]const u8 = null;
        var out_path: ?[]const u8 = null;
        var keep_asm = false;
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "-o")) {
                out_path = args.next() orelse {
                    try stderr.writeAll("build: -o needs a path\n");
                    std.process.exit(2);
                };
            } else if (std.mem.eql(u8, a, "--keep-asm")) {
                keep_asm = true;
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
        try driver.cmdBuild(allocator, path orelse {
            try stderr.writeAll("build: missing file\n");
            std.process.exit(2);
        }, out_path orelse "a.out", keep_asm, stdout);
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
}
