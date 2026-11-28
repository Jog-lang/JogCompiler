//! `jog-bindgen` — separate tool so the `jog` compiler does not link libclang.
//! Invoked as `jog bindgen …` (driver re-execs this binary) or directly.

const std = @import("std");
const model = @import("model.zig");
const collect = @import("collect.zig");
const emit = @import("emit.zig");

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    var arena = std.heap.ArenaAllocator.init(gpa.allocator());
    defer arena.deinit();
    // Arena backs the Unit graph (including fn-ptr param slices); GPA for argv only.
    const allocator = arena.allocator();
    const gpa_all = gpa.allocator();

    var args = try std.process.argsWithAllocator(gpa_all);
    defer args.deinit();
    _ = args.skip(); // argv0

    const stderr = std.io.getStdErr().writer();
    const stdout = std.io.getStdOut().writer();

    var header: ?[]const u8 = null;
    var output: ?[]const u8 = null;
    var prefix: []const u8 = "";
    var link: ?[]const u8 = null;
    var includes: std.ArrayListUnmanaged([]const u8) = .{};
    defer includes.deinit(gpa_all);
    var defines: std.ArrayListUnmanaged([]const u8) = .{};
    defer defines.deinit(gpa_all);

    while (args.next()) |a| {
        if (std.mem.eql(u8, a, "-h") or std.mem.eql(u8, a, "--help")) {
            try usage(stdout);
            return;
        } else if (std.mem.eql(u8, a, "-o") or std.mem.eql(u8, a, "--output")) {
            output = args.next() orelse {
                try stderr.writeAll("bindgen: -o needs a path\n");
                std.process.exit(2);
            };
        } else if (std.mem.startsWith(u8, a, "-o") and a.len > 2 and a[2] != '-') {
            output = a[2..];
        } else if (std.mem.startsWith(u8, a, "--output=")) {
            output = a["--output=".len..];
        } else if (std.mem.eql(u8, a, "--prefix")) {
            prefix = args.next() orelse {
                try stderr.writeAll("bindgen: --prefix needs a value\n");
                std.process.exit(2);
            };
        } else if (std.mem.startsWith(u8, a, "--prefix=")) {
            prefix = a["--prefix=".len..];
        } else if (std.mem.eql(u8, a, "--link")) {
            link = args.next() orelse {
                try stderr.writeAll("bindgen: --link needs a library name\n");
                std.process.exit(2);
            };
        } else if (std.mem.startsWith(u8, a, "--link=")) {
            link = a["--link=".len..];
        } else if (std.mem.eql(u8, a, "-I") or std.mem.eql(u8, a, "--include")) {
            const dir = args.next() orelse {
                try stderr.writeAll("bindgen: -I needs a directory\n");
                std.process.exit(2);
            };
            try includes.append(gpa_all, dir);
        } else if (std.mem.startsWith(u8, a, "-I") and a.len > 2) {
            try includes.append(gpa_all, a[2..]);
        } else if (std.mem.eql(u8, a, "-D") or std.mem.eql(u8, a, "--define")) {
            const d = args.next() orelse {
                try stderr.writeAll("bindgen: -D needs NAME or NAME=value\n");
                std.process.exit(2);
            };
            try defines.append(gpa_all, d);
        } else if (std.mem.startsWith(u8, a, "-D") and a.len > 2) {
            try defines.append(gpa_all, a[2..]);
        } else if (std.mem.startsWith(u8, a, "-")) {
            try stderr.print("bindgen: unknown flag {s}\n", .{a});
            std.process.exit(2);
        } else if (header == null) {
            header = a;
        } else {
            try stderr.print("bindgen: unexpected arg {s}\n", .{a});
            std.process.exit(2);
        }
    }

    const hdr = header orelse {
        try usage(stderr);
        std.process.exit(2);
    };

    const opts = model.Options{
        .header = hdr,
        .output = output,
        .prefix = prefix,
        .link = link,
        .includes = includes.items,
        .defines = defines.items,
    };

    var unit = collect.collectFromHeader(allocator, opts) catch |err| {
        try stderr.print("bindgen: failed to parse {s}: {s}\n", .{ hdr, @errorName(err) });
        std.process.exit(1);
    };
    defer unit.deinit();

    try emit.writeSkipped(stderr, &unit);

    if (output) |path| {
        const file = try std.fs.cwd().createFile(path, .{});
        defer file.close();
        try emit.writeUnit(file.writer(), &unit, opts);
        try stderr.print("bindgen: wrote {s}\n", .{path});
    } else {
        try emit.writeUnit(stdout, &unit, opts);
    }
}

fn usage(writer: anytype) !void {
    try writer.writeAll(
        \\usage: jog bindgen <header> [options]
        \\
        \\Generate an ordinary Jog extern binding file from a C header (libclang).
        \\Does not implement import c("…"); that is a later design pass.
        \\
        \\options:
        \\  -o, --output PATH     write bindings to PATH (default: stdout)
        \\  --prefix PREFIX       only emit decls whose names start with PREFIX
        \\  --link LIB            emit @link("LIB") on the extern block
        \\  -I, --include DIR     add an include search path (repeatable)
        \\  -D, --define MACRO    add a preprocessor macro (repeatable)
        \\  -h, --help            this message
        \\
        \\example:
        \\  jog bindgen SDL3/SDL.h --prefix SDL_ --link SDL3 -o vendor/sdl3/sdl3.jog
        \\
    );
}
