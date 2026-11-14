const std = @import("std");

/// Located library roots (owned strings when non-empty). Caller frees via `deinit`.
pub const Roots = struct {
    /// Directory containing `std/` packages (may be empty).
    std_root: []u8 = &.{},
    /// Directory containing `core/scalar.jog` (may be empty).
    core_root: []u8 = &.{},
    /// How the base was chosen (for `jog roots` / diagnostics).
    source: []const u8 = "none",

    pub fn deinit(self: *Roots, allocator: std.mem.Allocator) void {
        if (self.std_root.len > 0) allocator.free(self.std_root);
        if (self.core_root.len > 0) allocator.free(self.core_root);
        self.* = .{};
    }
};

fn isDir(path: []const u8) bool {
    const st = std.fs.cwd().statFile(path) catch return false;
    return st.kind == .directory;
}

fn joinOwned(allocator: std.mem.Allocator, a: []const u8, b: []const u8) ![]u8 {
    return try std.fs.path.join(allocator, &.{ a, b });
}

/// Spec / seed layout: `$JOG_ROOT` exact, else parent of the directory holding `jog`,
/// then one directory higher (zig-out/bin → repo root), then cwd.
pub fn locate(allocator: std.mem.Allocator) !Roots {
    var roots: Roots = .{};

    var base: ?[]u8 = null;
    defer if (base) |b| allocator.free(b);

    if (std.posix.getenv("JOG_ROOT")) |env| {
        if (env.len > 0) {
            if (!isDir(env)) return roots;
            const abs = std.fs.cwd().realpathAlloc(allocator, env) catch try allocator.dupe(u8, env);
            base = abs;
            roots.source = "JOG_ROOT";
        }
    }

    if (base == null) {
        var exe_buf: [std.fs.max_path_bytes]u8 = undefined;
        if (std.fs.selfExePath(&exe_buf)) |exe| {
            if (std.fs.path.dirname(exe)) |exe_dir| {
                if (std.fs.path.dirname(exe_dir)) |parent| {
                    const cand1 = try allocator.dupe(u8, parent);
                    const std1 = try joinOwned(allocator, cand1, "std");
                    defer allocator.free(std1);
                    const core1 = try joinOwned(allocator, cand1, "core");
                    defer allocator.free(core1);
                    if (isDir(std1) or isDir(core1)) {
                        base = cand1;
                        roots.source = "exe-parent";
                    } else {
                        allocator.free(cand1);
                        if (std.fs.path.dirname(parent)) |up| {
                            const cand2 = try allocator.dupe(u8, up);
                            const std2 = try joinOwned(allocator, cand2, "std");
                            defer allocator.free(std2);
                            const core2 = try joinOwned(allocator, cand2, "core");
                            defer allocator.free(core2);
                            if (isDir(std2) or isDir(core2)) {
                                base = cand2;
                                roots.source = "exe-grandparent";
                            } else {
                                allocator.free(cand2);
                            }
                        }
                    }
                }
            }
        } else |_| {}
    }

    if (base == null) {
        if (isDir("std") or isDir("core")) {
            const abs = std.fs.cwd().realpathAlloc(allocator, ".") catch try allocator.dupe(u8, ".");
            base = abs;
            roots.source = "cwd";
        }
    }

    const b = base orelse return roots;

    const std_dir = try joinOwned(allocator, b, "std");
    if (isDir(std_dir)) {
        roots.std_root = std_dir;
    } else {
        allocator.free(std_dir);
    }

    const core_dir = try joinOwned(allocator, b, "core");
    if (isDir(core_dir)) {
        roots.core_root = core_dir;
    } else {
        allocator.free(core_dir);
    }

    return roots;
}

/// Resolve `import std.pkg` / `std.pkg.leaf` to a `.jog` path. Caller frees.
pub fn resolveStdImport(allocator: std.mem.Allocator, std_root: []const u8, import_path: []const u8) !?[]u8 {
    if (std_root.len == 0) return null;
    if (!std.mem.startsWith(u8, import_path, "std.")) return null;

    const rest = import_path["std.".len..];
    var parts: std.ArrayListUnmanaged([]const u8) = .{};
    defer parts.deinit(allocator);
    var it = std.mem.splitScalar(u8, rest, '.');
    while (it.next()) |p| {
        if (p.len == 0) continue;
        try parts.append(allocator, p);
    }
    if (parts.items.len == 0) return null;

    const pkg = parts.items[0];
    if (parts.items.len == 1) {
        const file = try std.fmt.allocPrint(allocator, "{s}.jog", .{pkg});
        defer allocator.free(file);
        const joined = try std.fs.path.join(allocator, &.{ std_root, pkg, file });
        if (std.fs.cwd().access(joined, .{})) |_| return joined else |_| {
            allocator.free(joined);
        }
        return null;
    }

    // std.foo.bar → try std/foo/bar.jog then std/foo/bar/bar.jog
    var path_buf: std.ArrayListUnmanaged(u8) = .{};
    defer path_buf.deinit(allocator);
    try path_buf.appendSlice(allocator, std_root);
    for (parts.items) |p| {
        try path_buf.append(allocator, std.fs.path.sep);
        try path_buf.appendSlice(allocator, p);
    }
    try path_buf.appendSlice(allocator, ".jog");
    const flat = try allocator.dupe(u8, path_buf.items);
    if (std.fs.cwd().access(flat, .{})) |_| return flat else |_| {
        allocator.free(flat);
    }

    const leaf = parts.items[parts.items.len - 1];
    const nested_file = try std.fmt.allocPrint(allocator, "{s}.jog", .{leaf});
    defer allocator.free(nested_file);
    var nest: std.ArrayListUnmanaged(u8) = .{};
    defer nest.deinit(allocator);
    try nest.appendSlice(allocator, std_root);
    for (parts.items) |p| {
        try nest.append(allocator, std.fs.path.sep);
        try nest.appendSlice(allocator, p);
    }
    try nest.append(allocator, std.fs.path.sep);
    try nest.appendSlice(allocator, nested_file);
    const nested = try allocator.dupe(u8, nest.items);
    if (std.fs.cwd().access(nested, .{})) |_| return nested else |_| {
        allocator.free(nested);
    }
    return null;
}

/// Path to `core/scalar.jog` when present. Caller frees.
pub fn resolveCoreScalar(allocator: std.mem.Allocator, core_root: []const u8) !?[]u8 {
    if (core_root.len == 0) return null;
    const joined = try std.fs.path.join(allocator, &.{ core_root, "scalar.jog" });
    if (std.fs.cwd().access(joined, .{})) |_| return joined else |_| {
        allocator.free(joined);
        return null;
    }
}
