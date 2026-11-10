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
        const path = args.next() orelse {
            try stderr.writeAll("check: missing file\n");
            std.process.exit(2);
        };
        try driver.cmdCheck(allocator, path, stdout);
        return;
    }
    if (std.mem.eql(u8, cmd, "build")) {
        const path = args.next() orelse {
            try stderr.writeAll("build: missing file\n");
            std.process.exit(2);
        };
        var out_path: ?[]const u8 = null;
        while (args.next()) |a| {
            if (std.mem.eql(u8, a, "-o")) {
                out_path = args.next() orelse {
                    try stderr.writeAll("build: -o needs a path\n");
                    std.process.exit(2);
                };
            } else {
                try stderr.print("build: unknown arg {s}\n", .{a});
                std.process.exit(2);
            }
        }
        try driver.cmdBuild(allocator, path, out_path orelse "a.out", stdout);
        return;
    }

    try stderr.print("unknown command: {s}\n", .{cmd});
    try driver.usage(stderr);
    std.process.exit(2);
}

test {
    _ = @import("lex/lex.zig");
    _ = @import("check/check.zig");
    _ = @import("codegen/elf.zig");
}
