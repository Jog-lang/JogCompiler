const std = @import("std");

pub const Error = error{
    MissingJogMod,
    MissingModule,
    MissingRoot,
    DuplicateModule,
    DuplicateVersion,
    DuplicateRoot,
    EmptyDirective,
    UnknownDirective,
    OutOfMemory,
};

/// Link/libdir qualifier from `link.<target>` / `libdir.<target>`.
pub const LinkTarget = enum { any, linux, macos, windows, emscripten };

pub const LinkEntry = struct {
    lib: []const u8,
    target: LinkTarget,
};

pub const LibdirEntry = struct {
    path: []const u8,
    target: LinkTarget,
};

/// Minimal jog.mod: module, version, root, optional source / link / libdir lines.
pub const Manifest = struct {
    name: []const u8,
    version: []const u8,
    /// Paths relative to the module directory; root first, then source lines (deduped).
    sources: []const []const u8,
    /// From `link` / `link.<os|emscripten>` — filter with `linksFor`.
    links: []const LinkEntry,
    /// From `libdir` / `libdir.<os|emscripten>` — filter with `libdirsFor`.
    libdir_entries: []const LibdirEntry,
    /// Convenience: libs after filtering for the last `parse`/`load` target hint.
    /// Prefer `linksFor` / `libdirsFor` when the build target is known.
    link_libs: []const []const u8,
    libdirs: []const []const u8,
    /// Directory that contains jog.mod (as given / relative to cwd).
    dir: []const u8,
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *Manifest) void {
        self.arena.deinit();
    }

    /// Keep untargeted + matching `link.<target>` for `build_target`
    /// (`"linux"` / `"emscripten"` / …). Freestanding wasm is not modeled here.
    pub fn linksFor(self: *const Manifest, a: std.mem.Allocator, build_target: []const u8) Error![]const []const u8 {
        var out: std.ArrayListUnmanaged([]const u8) = .{};
        errdefer out.deinit(a);
        for (self.links) |e| {
            if (!linkApplies(e.target, build_target)) continue;
            try appendUnique(&out, a, e.lib);
        }
        return try out.toOwnedSlice(a);
    }

    pub fn libdirsFor(self: *const Manifest, a: std.mem.Allocator, build_target: []const u8) Error![]const []const u8 {
        var out: std.ArrayListUnmanaged([]const u8) = .{};
        errdefer {
            for (out.items) |p| a.free(p);
            out.deinit(a);
        }
        for (self.libdir_entries) |e| {
            if (!linkApplies(e.target, build_target)) continue;
            if (std.fs.path.isAbsolute(e.path)) {
                try appendUniqueOwned(&out, a, e.path);
            } else {
                const joined = try std.fs.path.join(a, &.{ self.dir, e.path });
                defer a.free(joined);
                try appendUniqueOwned(&out, a, joined);
            }
        }
        return try out.toOwnedSlice(a);
    }
};

fn linkApplies(t: LinkTarget, build_target: []const u8) bool {
    return switch (t) {
        .any => true,
        .linux => std.mem.eql(u8, build_target, "linux"),
        .macos => std.mem.eql(u8, build_target, "macos"),
        .windows => std.mem.eql(u8, build_target, "windows"),
        .emscripten => std.mem.eql(u8, build_target, "emscripten"),
    };
}

fn parseLinkTarget(name: []const u8) ?LinkTarget {
    if (std.mem.eql(u8, name, "linux")) return .linux;
    if (std.mem.eql(u8, name, "macos")) return .macos;
    if (std.mem.eql(u8, name, "windows")) return .windows;
    if (std.mem.eql(u8, name, "emscripten")) return .emscripten;
    return null;
}

/// True when `path` is a directory or a `jog.mod` file.
pub fn isModulePath(path: []const u8) bool {
    if (std.mem.eql(u8, std.fs.path.basename(path), "jog.mod")) return true;
    const st = std.fs.cwd().statFile(path) catch return false;
    return st.kind == .directory;
}

/// Load and parse `dir/jog.mod` (or `path` when it is a jog.mod file).
pub fn loadFromPath(parent: std.mem.Allocator, path: []const u8) Error!Manifest {
    const st = std.fs.cwd().statFile(path) catch return error.MissingJogMod;

    const dir: []const u8 = if (st.kind == .directory)
        path
    else if (std.mem.eql(u8, std.fs.path.basename(path), "jog.mod"))
        (std.fs.path.dirname(path) orelse ".")
    else
        return error.MissingJogMod;

    const mod_path = if (st.kind == .directory)
        try std.fs.path.join(parent, &.{ path, "jog.mod" })
    else
        try parent.dupe(u8, path);
    defer parent.free(mod_path);

    const text = std.fs.cwd().readFileAlloc(parent, mod_path, 1024 * 1024) catch return error.MissingJogMod;
    defer parent.free(text);

    return parse(parent, text, dir);
}

pub fn parse(parent: std.mem.Allocator, text: []const u8, dir: []const u8) Error!Manifest {
    var arena = std.heap.ArenaAllocator.init(parent);
    errdefer arena.deinit();
    const a = arena.allocator();

    var name: ?[]const u8 = null;
    var version: ?[]const u8 = null;
    var root: ?[]const u8 = null;
    var extras: std.ArrayListUnmanaged([]const u8) = .{};
    var links: std.ArrayListUnmanaged(LinkEntry) = .{};
    var libdir_entries: std.ArrayListUnmanaged(LibdirEntry) = .{};

    var line_it = std.mem.splitScalar(u8, text, '\n');
    while (line_it.next()) |raw_line| {
        const line = trimComment(std.mem.trim(u8, raw_line, " \t\r"));
        if (line.len == 0) continue;

        var tok_it = std.mem.tokenizeAny(u8, line, " \t");
        const key = tok_it.next() orelse continue;
        const value = std.mem.trim(u8, line[key.len..], " \t");
        if (value.len == 0) return error.EmptyDirective;

        if (std.mem.eql(u8, key, "module")) {
            if (name != null) return error.DuplicateModule;
            name = try a.dupe(u8, firstWord(value));
        } else if (std.mem.eql(u8, key, "version")) {
            if (version != null) return error.DuplicateVersion;
            version = try a.dupe(u8, firstWord(value));
        } else if (std.mem.eql(u8, key, "root")) {
            if (root != null) return error.DuplicateRoot;
            root = try a.dupe(u8, firstWord(value));
        } else if (std.mem.eql(u8, key, "source")) {
            try extras.append(a, try a.dupe(u8, firstWord(value)));
        } else if (std.mem.eql(u8, key, "link")) {
            try links.append(a, .{ .lib = try a.dupe(u8, firstWord(value)), .target = .any });
        } else if (std.mem.startsWith(u8, key, "link.")) {
            const tname = key["link.".len..];
            const t = parseLinkTarget(tname) orelse return error.UnknownDirective;
            try links.append(a, .{ .lib = try a.dupe(u8, firstWord(value)), .target = t });
        } else if (std.mem.eql(u8, key, "libdir")) {
            try libdir_entries.append(a, .{ .path = try a.dupe(u8, firstWord(value)), .target = .any });
        } else if (std.mem.startsWith(u8, key, "libdir.")) {
            const tname = key["libdir.".len..];
            const t = parseLinkTarget(tname) orelse return error.UnknownDirective;
            try libdir_entries.append(a, .{ .path = try a.dupe(u8, firstWord(value)), .target = t });
        } else if (std.mem.eql(u8, key, "export") or std.mem.eql(u8, key, "kind") or std.mem.eql(u8, key, "jog") or std.mem.eql(u8, key, "require") or std.mem.eql(u8, key, "require.test")) {
            // Recognized v1 directives ignored on v2 (sources listed explicitly).
            continue;
        } else {
            return error.UnknownDirective;
        }
    }

    const mod_name = name orelse return error.MissingModule;
    const mod_root = root orelse return error.MissingRoot;
    const mod_version = version orelse try a.dupe(u8, "0.0.0");

    var sources: std.ArrayListUnmanaged([]const u8) = .{};
    try sources.append(a, mod_root);
    for (extras.items) |s| {
        if (containsPath(sources.items, s)) continue;
        try sources.append(a, s);
    }

    const links_slice = try links.toOwnedSlice(a);
    const libdirs_slice = try libdir_entries.toOwnedSlice(a);

    // Default convenience slices: native linux (matches prior v2 behavior).
    var default_libs: std.ArrayListUnmanaged([]const u8) = .{};
    for (links_slice) |e| {
        if (linkApplies(e.target, "linux")) try appendUnique(&default_libs, a, e.lib);
    }
    var default_dirs: std.ArrayListUnmanaged([]const u8) = .{};
    for (libdirs_slice) |e| {
        if (linkApplies(e.target, "linux")) try appendUnique(&default_dirs, a, e.path);
    }

    return .{
        .name = mod_name,
        .version = mod_version,
        .sources = try sources.toOwnedSlice(a),
        .links = links_slice,
        .libdir_entries = libdirs_slice,
        .link_libs = try default_libs.toOwnedSlice(a),
        .libdirs = try default_dirs.toOwnedSlice(a),
        .dir = try a.dupe(u8, dir),
        .arena = arena,
    };
}

/// Join module dir + relative sources. Caller frees each path and the slice.
pub fn resolveSourcePaths(allocator: std.mem.Allocator, m: *const Manifest) Error![][]const u8 {
    var out: std.ArrayListUnmanaged([]const u8) = .{};
    errdefer {
        for (out.items) |p| allocator.free(p);
        out.deinit(allocator);
    }
    for (m.sources) |rel| {
        const joined = try std.fs.path.join(allocator, &.{ m.dir, rel });
        try out.append(allocator, joined);
    }
    return try out.toOwnedSlice(allocator);
}

/// Resolve libdir entries relative to the module directory. Caller frees.
pub fn resolveLibdirs(allocator: std.mem.Allocator, m: *const Manifest) Error![][]const u8 {
    var out: std.ArrayListUnmanaged([]const u8) = .{};
    errdefer {
        for (out.items) |p| allocator.free(p);
        out.deinit(allocator);
    }
    for (m.libdirs) |rel| {
        if (std.fs.path.isAbsolute(rel)) {
            try out.append(allocator, try allocator.dupe(u8, rel));
        } else {
            try out.append(allocator, try std.fs.path.join(allocator, &.{ m.dir, rel }));
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn appendUnique(list: *std.ArrayListUnmanaged([]const u8), a: std.mem.Allocator, word: []const u8) !void {
    for (list.items) |e| {
        if (std.mem.eql(u8, e, word)) return;
    }
    try list.append(a, try a.dupe(u8, word));
}

fn appendUniqueOwned(list: *std.ArrayListUnmanaged([]const u8), a: std.mem.Allocator, word: []const u8) !void {
    for (list.items) |e| {
        if (std.mem.eql(u8, e, word)) return;
    }
    try list.append(a, try a.dupe(u8, word));
}

fn containsPath(hay: []const []const u8, needle: []const u8) bool {
    for (hay) |p| {
        if (std.mem.eql(u8, p, needle)) return true;
    }
    return false;
}

fn firstWord(s: []const u8) []const u8 {
    var it = std.mem.tokenizeAny(u8, s, " \t");
    return it.next() orelse s;
}

fn trimComment(line: []const u8) []const u8 {
    if (std.mem.indexOfScalar(u8, line, '#')) |i| {
        return std.mem.trim(u8, line[0..i], " \t");
    }
    return line;
}

test "parse minimal jog.mod" {
    const text =
        \\module demo
        \\version 0.1.0
        \\root main.jog
        \\source util.jog
        \\
    ;
    var m = try parse(std.testing.allocator, text, ".");
    defer m.deinit();
    try std.testing.expectEqualStrings("demo", m.name);
    try std.testing.expectEqualStrings("0.1.0", m.version);
    try std.testing.expectEqual(@as(usize, 2), m.sources.len);
    try std.testing.expectEqualStrings("main.jog", m.sources[0]);
    try std.testing.expectEqualStrings("util.jog", m.sources[1]);
}

test "root listed again in source is deduped" {
    const text =
        \\module demo
        \\root main.jog
        \\source main.jog
        \\
    ;
    var m = try parse(std.testing.allocator, text, ".");
    defer m.deinit();
    try std.testing.expectEqual(@as(usize, 1), m.sources.len);
}

test "link and libdir lines" {
    const text =
        \\module jog_sdl
        \\root sdl.jog
        \\link.linux SDL3
        \\link.emscripten SDL3
        \\libdir.linux ./prefix/lib64
        \\libdir.emscripten ./prefix-emscripten/lib
        \\export sdl
        \\
    ;
    var m = try parse(std.testing.allocator, text, "/tmp/jog_sdl");
    defer m.deinit();
    try std.testing.expectEqual(@as(usize, 1), m.link_libs.len);
    try std.testing.expectEqualStrings("SDL3", m.link_libs[0]);
    try std.testing.expectEqual(@as(usize, 1), m.libdirs.len);
    try std.testing.expectEqualStrings("./prefix/lib64", m.libdirs[0]);

    const em_libs = try m.linksFor(std.testing.allocator, "emscripten");
    defer std.testing.allocator.free(em_libs);
    try std.testing.expectEqual(@as(usize, 1), em_libs.len);
    try std.testing.expectEqualStrings("SDL3", em_libs[0]);

    const em_dirs = try m.libdirsFor(std.testing.allocator, "emscripten");
    defer {
        for (em_dirs) |p| std.testing.allocator.free(p);
        std.testing.allocator.free(em_dirs);
    }
    try std.testing.expectEqual(@as(usize, 1), em_dirs.len);
    try std.testing.expectEqualStrings("/tmp/jog_sdl/prefix-emscripten/lib", em_dirs[0]);
}
