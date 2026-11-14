const std = @import("std");
const ast = @import("../ast/ast.zig");
const parse = @import("../parse/parse.zig");
const check_mod = @import("../check/check.zig");
const ir = @import("../ir/ir.zig");
const link = @import("../codegen/link.zig");
const mod = @import("../mod/mod.zig");
const roots_mod = @import("roots.zig");

pub const version_string = "jog 0.2.9-v2 (zig host, std/core roots; ir→asm→native | ir→c→emcc; LSP surfaces)";

pub fn cmdVersion(writer: anytype) !void {
    try writer.print("{s}\n", .{version_string});
}

pub fn cmdRoots(allocator: std.mem.Allocator, writer: anytype) !void {
    var roots = try roots_mod.locate(allocator);
    defer roots.deinit(allocator);
    try writer.print("source: {s}\n", .{roots.source});
    try writer.print("std: {s}\n", .{if (roots.std_root.len > 0) roots.std_root else "(missing)"});
    try writer.print("core: {s}\n", .{if (roots.core_root.len > 0) roots.core_root else "(missing)"});
    if (try roots_mod.resolveCoreScalar(allocator, roots.core_root)) |scalar| {
        defer allocator.free(scalar);
        try writer.print("core/scalar.jog: {s}\n", .{scalar});
    }
}

/// `--module name=dir` mapping (absolute or cwd-relative).
pub const ModuleMapping = struct {
    name: []const u8,
    dir: []const u8,
};

pub const LoadOptions = struct {
    modules: []const ModuleMapping = &.{},
    /// When true, also pull `require.test` path/module deps (LSP `*.test.jog`).
    include_test_requires: bool = false,
};

/// JogLSP writes unsaved buffers as `<realpath>.jog-lsp-tmp` next to the file.
fn stripLspTmpSuffix(path: []const u8) []const u8 {
    const suf = ".jog-lsp-tmp";
    if (std.mem.endsWith(u8, path, suf)) return path[0 .. path.len - suf.len];
    return path;
}

fn findModuleDirUp(allocator: std.mem.Allocator, start_file: []const u8) !?[]u8 {
    var dir = std.fs.path.dirname(start_file) orelse return null;
    while (true) {
        const mod_path = try std.fs.path.join(allocator, &.{ dir, "jog.mod" });
        defer allocator.free(mod_path);
        if (std.fs.cwd().access(mod_path, .{})) |_| {
            return try allocator.dupe(u8, dir);
        } else |_| {}
        const parent = std.fs.path.dirname(dir) orelse return null;
        if (std.mem.eql(u8, parent, dir)) return null;
        dir = parent;
    }
}

/// Parsed Jog unit: one file or a merged module. Keeps source buffers alive
/// because the parser stores string slices into them.
pub const Unit = struct {
    file: ast.File,
    /// Owned source buffers (one per input file).
    srcs: [][]u8,
    /// Per-file ASTs when multi-file; empty for single-file.
    parts: []ast.File,
    /// Merged decls slice owned by `allocator` when parts.len > 0.
    merged_decls: ?[]ast.Decl,
    allocator: std.mem.Allocator,

    pub fn deinit(self: *Unit) void {
        if (self.merged_decls) |d| self.allocator.free(d);
        for (self.parts) |*p| p.deinit();
        if (self.parts.len > 0) {
            self.allocator.free(self.parts);
            // file.arena was never used for nodes
            self.file.arena.deinit();
        } else {
            self.file.deinit();
        }
        for (self.srcs) |s| self.allocator.free(s);
        self.allocator.free(self.srcs);
    }
};

fn loadSingle(allocator: std.mem.Allocator, path: []const u8, modules: []const ModuleMapping) !Unit {
    // Follow `import` decls to sibling `.jog` files and `std.*` via library roots (BFS).
    var lib_roots = try roots_mod.locate(allocator);
    defer lib_roots.deinit(allocator);

    var pending: std.ArrayListUnmanaged([]const u8) = .{};
    defer {
        for (pending.items) |x| allocator.free(x);
        pending.deinit(allocator);
    }
    var seen: std.StringHashMapUnmanaged(void) = .{};
    defer seen.deinit(allocator);

    const root = try allocator.dupe(u8, path);
    try pending.append(allocator, root);
    try seen.put(allocator, root, {});

    var srcs: std.ArrayListUnmanaged([]u8) = .{};
    errdefer {
        for (srcs.items) |s| allocator.free(s);
        srcs.deinit(allocator);
    }
    var parts: std.ArrayListUnmanaged(ast.File) = .{};
    errdefer {
        for (parts.items) |*pf| pf.deinit();
        parts.deinit(allocator);
    }
    var decls: std.ArrayListUnmanaged(ast.Decl) = .{};
    errdefer decls.deinit(allocator);

    var qi: usize = 0;
    while (qi < pending.items.len) : (qi += 1) {
        const cur = pending.items[qi];
        const src = try std.fs.cwd().readFileAlloc(allocator, cur, 16 * 1024 * 1024);
        try srcs.append(allocator, src);
        const file = try parse.Parser.parseFile(allocator, src);
        try decls.appendSlice(allocator, file.decls);
        for (file.decls) |d| {
            if (d == .import_decl) {
                const imp = d.import_decl;
                if (std.mem.eql(u8, imp.path, "core") or std.mem.eql(u8, imp.path, "std")) continue;
                const resolved = (try resolveImportPath(allocator, cur, imp.path, modules, &lib_roots)) orelse continue;
                defer allocator.free(resolved);
                if (seen.contains(resolved)) continue;
                std.fs.cwd().access(resolved, .{}) catch continue;
                const owned = try allocator.dupe(u8, resolved);
                try seen.put(allocator, owned, {});
                try pending.append(allocator, owned);
                try enqueuePlatformSiblings(allocator, &pending, &seen, owned);
            } else if (d == .require_decl) {
                const req = d.require_decl;
                var resolved: ?[]u8 = null;
                for (modules) |m| {
                    if (!std.mem.eql(u8, m.name, req.name)) continue;
                    resolved = try resolveModuleEntry(allocator, m.dir, req.name);
                    break;
                }
                if (resolved == null) resolved = try resolveRequirePath(allocator, cur, req.name);
                defer if (resolved) |r| allocator.free(r);
                const res = resolved orelse continue;
                if (seen.contains(res)) continue;
                std.fs.cwd().access(res, .{}) catch continue;
                const owned = try allocator.dupe(u8, res);
                try seen.put(allocator, owned, {});
                try pending.append(allocator, owned);
            }
        }
        try parts.append(allocator, file);
    }

    if (parts.items.len == 1) {
        // Single file, no imports pulled — same shape as before.
        decls.deinit(allocator);
        const srcs_slice = try srcs.toOwnedSlice(allocator);
        const parts_owned = try parts.toOwnedSlice(allocator);
        const file = parts_owned[0];
        allocator.free(parts_owned);
        return .{
            .file = file,
            .srcs = srcs_slice,
            .parts = &[_]ast.File{},
            .merged_decls = null,
            .allocator = allocator,
        };
    }

    return finishMergedUnit(allocator, &srcs, &parts, &decls);
}

fn finishMergedUnit(
    allocator: std.mem.Allocator,
    srcs: *std.ArrayListUnmanaged([]u8),
    parts: *std.ArrayListUnmanaged(ast.File),
    decls: *std.ArrayListUnmanaged(ast.Decl),
) !Unit {
    const merged = try decls.toOwnedSlice(allocator);
    const parts_slice = try parts.toOwnedSlice(allocator);
    var file: ast.File = .{
        .decls = merged,
        .arena = std.heap.ArenaAllocator.init(allocator),
    };
    // Preserve expression spans from each part for diagnostics.
    const fa = file.arena.allocator();
    for (parts_slice) |*pf| {
        var it = pf.expr_spans.iterator();
        while (it.next()) |e| {
            try file.expr_spans.put(fa, e.key_ptr.*, e.value_ptr.*);
        }
    }
    return .{
        .file = file,
        .srcs = try srcs.toOwnedSlice(allocator),
        .parts = parts_slice,
        .merged_decls = merged,
        .allocator = allocator,
    };
}

fn resolveImportPath(
    allocator: std.mem.Allocator,
    from_file: []const u8,
    import_path: []const u8,
    modules: []const ModuleMapping,
    lib_roots: *const roots_mod.Roots,
) !?[]u8 {
    // Library: `import std.math` → `$JOG_ROOT/std/math/math.jog` (etc.).
    if (std.mem.startsWith(u8, import_path, "std.")) {
        if (try roots_mod.resolveStdImport(allocator, lib_roots.std_root, import_path)) |p| return p;
    }

    // `mod.leaf` → look in mapped module dir first when `mod` is a mapping name.
    if (std.mem.indexOfScalar(u8, import_path, '.')) |dot| {
        const head = import_path[0..dot];
        const leaf = import_path[dot + 1 ..];
        for (modules) |m| {
            if (std.mem.eql(u8, m.name, head)) {
                // Prefer `leaf.jog`, then replace remaining dots with `/`.
                const simple = try std.fmt.allocPrint(allocator, "{s}.jog", .{leaf});
                defer allocator.free(simple);
                const joined = try std.fs.path.join(allocator, &.{ m.dir, simple });
                if (std.fs.cwd().access(joined, .{})) |_| return joined else |_| {
                    allocator.free(joined);
                }
                var path_buf: std.ArrayListUnmanaged(u8) = .{};
                defer path_buf.deinit(allocator);
                var it = std.mem.splitScalar(u8, leaf, '.');
                var first = true;
                while (it.next()) |part| {
                    if (!first) try path_buf.append(allocator, '/');
                    first = false;
                    try path_buf.appendSlice(allocator, part);
                }
                try path_buf.appendSlice(allocator, ".jog");
                return try std.fs.path.join(allocator, &.{ m.dir, path_buf.items });
            }
        }
    }
    const dir = std.fs.path.dirname(from_file) orelse ".";
    var leaf = import_path;
    if (std.mem.lastIndexOfScalar(u8, import_path, '.')) |dot| {
        leaf = import_path[dot + 1 ..];
    }
    const name = try std.fmt.allocPrint(allocator, "{s}.jog", .{leaf});
    defer allocator.free(name);
    const joined = try std.fs.path.join(allocator, &.{ dir, name });
    return joined;
}

/// When loading `foo.jog`, also queue `foo.posix.jog` / `foo.linux.jog` if present (std platform files).
fn enqueuePlatformSiblings(
    allocator: std.mem.Allocator,
    pending: *std.ArrayListUnmanaged([]const u8),
    seen: *std.StringHashMapUnmanaged(void),
    path: []const u8,
) !void {
    if (!std.mem.endsWith(u8, path, ".jog")) return;
    const base = path[0 .. path.len - ".jog".len];
    const plats = [_][]const u8{ "posix", "linux" };
    for (plats) |plat| {
        const cand = try std.fmt.allocPrint(allocator, "{s}.{s}.jog", .{ base, plat });
        defer allocator.free(cand);
        std.fs.cwd().access(cand, .{}) catch continue;
        if (seen.contains(cand)) continue;
        const owned = try allocator.dupe(u8, cand);
        try seen.put(allocator, owned, {});
        try pending.append(allocator, owned);
    }
}

/// `require foo` → sibling `foo.jog`, `foo/root.jog`, or `foo/main.jog`.
fn resolveRequirePath(allocator: std.mem.Allocator, from_file: []const u8, name: []const u8) !?[]u8 {
    const dir = std.fs.path.dirname(from_file) orelse ".";
    return resolveModuleEntry(allocator, dir, name);
}

/// Entry file for a module directory or a same-folder `name.jog`.
fn resolveModuleEntry(allocator: std.mem.Allocator, dir: []const u8, name: []const u8) !?[]u8 {
    const a = try std.fmt.allocPrint(allocator, "{s}/{s}.jog", .{ dir, name });
    if (std.fs.cwd().access(a, .{})) |_| return a else |_| allocator.free(a);
    const b = try std.fs.path.join(allocator, &.{ dir, "root.jog" });
    if (std.fs.cwd().access(b, .{})) |_| return b else |_| allocator.free(b);
    const c = try std.fs.path.join(allocator, &.{ dir, "main.jog" });
    if (std.fs.cwd().access(c, .{})) |_| return c else |_| allocator.free(c);
    const d = try std.fmt.allocPrint(allocator, "{s}/{s}/root.jog", .{ dir, name });
    if (std.fs.cwd().access(d, .{})) |_| return d else |_| allocator.free(d);
    const e = try std.fmt.allocPrint(allocator, "{s}/{s}/main.jog", .{ dir, name });
    if (std.fs.cwd().access(e, .{})) |_| return e else |_| allocator.free(e);
    return null;
}

fn appendUniquePath(list: *std.ArrayListUnmanaged([]const u8), allocator: std.mem.Allocator, path: []const u8) !void {
    for (list.items) |p| {
        if (std.mem.eql(u8, p, path)) return;
    }
    try list.append(allocator, try allocator.dupe(u8, path));
}

fn loadModule(allocator: std.mem.Allocator, path: []const u8, writer: anytype, opts: LoadOptions) !Unit {
    return loadModuleWithOverride(allocator, path, writer, opts, null, null);
}

/// `override_real` / `override_read`: when set, read `override_read` in place of source `override_real`
/// (JogLSP temp buffer sitting next to the real file).
fn loadModuleWithOverride(
    allocator: std.mem.Allocator,
    path: []const u8,
    writer: anytype,
    opts: LoadOptions,
    override_real: ?[]const u8,
    override_read: ?[]const u8,
) !Unit {
    var manifest = mod.loadFromPath(allocator, path) catch |err| {
        try writer.print("jog.mod error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer manifest.deinit();

    var path_list: std.ArrayListUnmanaged([]const u8) = .{};
    defer {
        for (path_list.items) |p| allocator.free(p);
        path_list.deinit(allocator);
    }

    const own_paths = mod.resolveSourcePaths(allocator, &manifest) catch |err| {
        try writer.print("jog.mod error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer {
        for (own_paths) |p| allocator.free(p);
        allocator.free(own_paths);
    }
    for (own_paths) |p| try appendUniquePath(&path_list, allocator, p);

    // Build effective module map: CLI `--module` plus path requires from jog.mod.
    // `owned_dirs` keeps path-require directories alive for `map_buf` pointers.
    var owned_dirs: std.ArrayListUnmanaged([]u8) = .{};
    defer {
        for (owned_dirs.items) |d| allocator.free(d);
        owned_dirs.deinit(allocator);
    }
    var map_buf: std.ArrayListUnmanaged(ModuleMapping) = .{};
    defer map_buf.deinit(allocator);
    try map_buf.appendSlice(allocator, opts.modules);

    for (manifest.requires) |req| {
        if (req.test_only and !opts.include_test_requires) continue;
        var dep_dir: []const u8 = undefined;
        if (mod.isPathSpec(req.spec)) {
            const resolved = mod.resolveRequireDir(allocator, &manifest, req.spec) catch |err| {
                try writer.print("require {s}: {s}\n", .{ req.name, @errorName(err) });
                std.process.exit(1);
            };
            try owned_dirs.append(allocator, resolved);
            dep_dir = resolved;
        } else {
            var found: ?[]const u8 = null;
            for (opts.modules) |m| {
                if (std.mem.eql(u8, m.name, req.name)) {
                    found = m.dir;
                    break;
                }
            }
            if (found == null) {
                try writer.print("require {s}: missing --module {s}=<dir> (version requires need a mapping)\n", .{ req.name, req.name });
                std.process.exit(1);
            }
            dep_dir = found.?;
        }
        // Record mapping for import resolution.
        var already = false;
        for (map_buf.items) |m| {
            if (std.mem.eql(u8, m.name, req.name)) {
                already = true;
                break;
            }
        }
        if (!already) {
            try map_buf.append(allocator, .{ .name = req.name, .dir = dep_dir });
        }
        // Merge dependency sources (path require or mapped module).
        if (mod.isModulePath(dep_dir)) {
            var dep_man = mod.loadFromPath(allocator, dep_dir) catch |err| {
                try writer.print("require {s}: jog.mod error: {s}\n", .{ req.name, @errorName(err) });
                std.process.exit(1);
            };
            defer dep_man.deinit();
            const dep_paths = mod.resolveSourcePaths(allocator, &dep_man) catch |err| {
                try writer.print("require {s}: {s}\n", .{ req.name, @errorName(err) });
                std.process.exit(1);
            };
            defer {
                for (dep_paths) |p| allocator.free(p);
                allocator.free(dep_paths);
            }
            for (dep_paths) |p| try appendUniquePath(&path_list, allocator, p);
        } else if (try resolveModuleEntry(allocator, dep_dir, req.name)) |entry| {
            defer allocator.free(entry);
            try appendUniquePath(&path_list, allocator, entry);
        }
    }

    const modules = map_buf.items;
    if (path_list.items.len == 0) {
        try writer.writeAll("jog.mod error: no sources\n");
        std.process.exit(1);
    }
    if (path_list.items.len == 1) {
        return loadSingle(allocator, path_list.items[0], modules) catch |err| {
            try writer.print("parse error ({s}): {s}\n", .{ path_list.items[0], @errorName(err) });
            std.process.exit(1);
        };
    }

    var srcs: std.ArrayListUnmanaged([]u8) = .{};
    errdefer {
        for (srcs.items) |s| allocator.free(s);
        srcs.deinit(allocator);
    }
    var parts: std.ArrayListUnmanaged(ast.File) = .{};
    errdefer {
        for (parts.items) |*p| p.deinit();
        parts.deinit(allocator);
    }
    var decls: std.ArrayListUnmanaged(ast.Decl) = .{};
    errdefer decls.deinit(allocator);

    var lib_roots = try roots_mod.locate(allocator);
    defer lib_roots.deinit(allocator);

    var pi: usize = 0;
    while (pi < path_list.items.len) : (pi += 1) {
        const p = path_list.items[pi];
        const read_path = blk: {
            if (override_real) |real| {
                if (override_read) |alt| {
                    if (std.mem.eql(u8, p, real)) break :blk alt;
                    // Also match by basename when paths differ only by relative/absolute form.
                    if (std.mem.eql(u8, std.fs.path.basename(p), std.fs.path.basename(real))) break :blk alt;
                }
            }
            break :blk p;
        };
        const src = std.fs.cwd().readFileAlloc(allocator, read_path, 16 * 1024 * 1024) catch |err| {
            try writer.print("read error ({s}): {s}\n", .{ read_path, @errorName(err) });
            std.process.exit(1);
        };
        try srcs.append(allocator, src);
        const file = parse.Parser.parseFile(allocator, src) catch |err| {
            try writer.print("parse error ({s}): {s}\n", .{ read_path, @errorName(err) });
            std.process.exit(1);
        };
        try decls.appendSlice(allocator, file.decls);
        // Also BFS imports from each file against the module map / std roots.
        for (file.decls) |d| {
            if (d != .import_decl) continue;
            const imp = d.import_decl;
            if (std.mem.eql(u8, imp.path, "core") or std.mem.eql(u8, imp.path, "std")) continue;
            const resolved = (try resolveImportPath(allocator, p, imp.path, modules, &lib_roots)) orelse continue;
            defer allocator.free(resolved);
            std.fs.cwd().access(resolved, .{}) catch continue;
            try appendUniquePath(&path_list, allocator, resolved);
            // Platform siblings for std packages (c.posix.jog, …).
            if (!std.mem.endsWith(u8, resolved, ".jog")) continue;
            const base = resolved[0 .. resolved.len - ".jog".len];
            for ([_][]const u8{ "posix", "linux" }) |plat| {
                const cand = try std.fmt.allocPrint(allocator, "{s}.{s}.jog", .{ base, plat });
                defer allocator.free(cand);
                std.fs.cwd().access(cand, .{}) catch continue;
                try appendUniquePath(&path_list, allocator, cand);
            }
        }
        // Platform siblings of each listed source (std.c → c.posix.jog).
        if (std.mem.endsWith(u8, p, ".jog")) {
            const base = p[0 .. p.len - ".jog".len];
            for ([_][]const u8{ "posix", "linux" }) |plat| {
                const cand = try std.fmt.allocPrint(allocator, "{s}.{s}.jog", .{ base, plat });
                defer allocator.free(cand);
                std.fs.cwd().access(cand, .{}) catch continue;
                try appendUniquePath(&path_list, allocator, cand);
            }
        }
        try parts.append(allocator, file);
    }

    return finishMergedUnit(allocator, &srcs, &parts, &decls);
}

pub fn loadPath(allocator: std.mem.Allocator, path: []const u8, writer: anytype) !Unit {
    return loadPathOpts(allocator, path, writer, .{});
}

pub fn loadPathOpts(allocator: std.mem.Allocator, path: []const u8, writer: anytype, opts: LoadOptions) !Unit {
    if (mod.isModulePath(path)) {
        return loadModule(allocator, path, writer, opts);
    }
    // JogLSP temp buffer or plain file under a module: load the whole module
    // (with path requires / --module) so cross-module names resolve.
    const logical = stripLspTmpSuffix(path);
    if (try findModuleDirUp(allocator, logical)) |mod_dir| {
        defer allocator.free(mod_dir);
        const is_tmp = !std.mem.eql(u8, logical, path);
        return loadModuleWithOverride(
            allocator,
            mod_dir,
            writer,
            opts,
            if (is_tmp) logical else null,
            if (is_tmp) path else null,
        );
    }
    return loadSingle(allocator, path, opts.modules) catch |err| {
        try writer.print("parse error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
}

/// One JSON object per line (v1 LSP contract).
fn writeDiag(allocator: std.mem.Allocator, writer: anytype, file: []const u8, line: u32, column: u32, severity: []const u8, message: []const u8) !void {
    var buf: std.ArrayListUnmanaged(u8) = .{};
    defer buf.deinit(allocator);
    try std.json.stringify(.{
        .file = file,
        .line = line,
        .column = column,
        .severity = severity,
        .message = message,
    }, .{}, buf.writer(allocator));
    try writer.writeAll(buf.items);
    try writer.writeAll("\n");
}

pub fn cmdParse(allocator: std.mem.Allocator, path: []const u8, writer: anytype, opts: LoadOptions) !void {
    var unit = try loadPathOpts(allocator, path, writer, opts);
    defer unit.deinit();
    try writer.print("ok: {d} declaration(s)\n", .{unit.file.decls.len});
    for (unit.file.decls) |d| {
        switch (d) {
            .type_alias => |t| try writer.print("  type {s}\n", .{t.name}),
            .struct_decl => |s| try writer.print("  struct {s}\n", .{s.name}),
            .enum_decl => |e| try writer.print("  enum {s}\n", .{e.name}),
            .import_decl => |i| try writer.print("  import {s}\n", .{i.path}),
            .require_decl => |r| try writer.print("  require {s}\n", .{r.name}),
            .interface_fn => |i| try writer.print("  interface fn {s}\n", .{i.name}),
            .interface_struct => |i| try writer.print("  interface struct {s}\n", .{i.name}),
            .fn_decl => |f| try writer.print("  fn {s}\n", .{f.name}),
            .method => |m| try writer.print("  method {s}.{s}\n", .{ m.type_name, m.name }),
            .extern_block => |eb| {
                for (eb.fns) |ef| try writer.print("  extern fn {s}\n", .{ef.name});
            },
            .expect_fn => |e| try writer.print("  expect fn {s}\n", .{e.name}),
            .actual_fn => |f| try writer.print("  actual fn {s}\n", .{f.name}),
            .global_decl => |g| try writer.print("  {s} {s}\n", .{ if (g.mutable) "var" else "val", g.name }),
        }
    }
}

/// Document symbols for LSP (`jog symbols --json <path>`).
pub fn cmdSymbols(allocator: std.mem.Allocator, path: []const u8, json: bool, writer: anytype, opts: LoadOptions) !void {
    var unit = try loadPathOpts(allocator, path, writer, opts);
    defer unit.deinit();
    for (unit.file.decls) |d| {
        const name: []const u8 = switch (d) {
            .fn_decl => |f| f.name,
            .actual_fn => |f| f.name,
            .struct_decl => |s| s.name,
            .enum_decl => |e| e.name,
            .global_decl => |g| g.name,
            .type_alias => |t| t.name,
            .interface_fn => |i| i.name,
            .interface_struct => |i| i.name,
            .method => |m| m.name,
            else => continue,
        };
        const kind: []const u8 = switch (d) {
            .fn_decl, .actual_fn, .method => "function",
            .struct_decl => "struct",
            .enum_decl => "enum",
            .global_decl => |g| if (g.mutable) "variable" else "constant",
            .type_alias => "type",
            .interface_fn, .interface_struct => "interface",
            else => "variable",
        };
        const line: u32 = switch (d) {
            .fn_decl => |f| f.line,
            .actual_fn => |f| f.line,
            .struct_decl => |s| s.line,
            .enum_decl => |e| e.line,
            .global_decl => |g| g.line,
            else => 1,
        };
        const col: u32 = switch (d) {
            .fn_decl => |f| f.col,
            .actual_fn => |f| f.col,
            .struct_decl => |s| s.col,
            .enum_decl => |e| e.col,
            .global_decl => |g| g.col,
            else => 1,
        };
        if (json) {
            var buf: std.ArrayListUnmanaged(u8) = .{};
            defer buf.deinit(allocator);
            try std.json.stringify(.{
                .name = name,
                .kind = kind,
                .line = line,
                .column = col,
                .end_line = line,
                .end_column = col + @as(u32, @intCast(name.len)),
            }, .{}, buf.writer(allocator));
            try writer.writeAll(buf.items);
            try writer.writeAll("\n");
        } else {
            try writer.print("{s}\t{s}\t{d}:{d}\n", .{ kind, name, line, col });
        }
    }
}

pub fn cmdCheck(allocator: std.mem.Allocator, path: []const u8, diag_json: bool, writer: anytype, opts: LoadOptions) !void {
    var unit = try loadPathOpts(allocator, path, writer, opts);
    defer unit.deinit();
    var result = try check_mod.check(allocator, &unit.file);
    defer result.deinit();
    if (!result.ok) {
        for (result.diagnostics) |d| {
            if (diag_json) {
                try writeDiag(allocator, writer, path, d.line, d.col, "error", d.message);
            } else {
                try writer.print("error: {s}\n", .{d.message});
            }
        }
        std.process.exit(1);
    }
    if (!diag_json) {
        try writer.print("check: ok\n", .{});
    }
}

pub const BuildOptions = struct {
    keep_asm: bool = false,
    keep_c: bool = false,
    target: link.Target = .native,
    cli_libdirs: []const []const u8 = &.{},
    modules: []const ModuleMapping = &.{},
    include_test_requires: bool = false,
};

pub fn cmdBuild(allocator: std.mem.Allocator, path: []const u8, out_path: []const u8, keep_asm: bool, cli_libdirs: []const []const u8, writer: anytype) !void {
    try cmdBuildOpts(allocator, path, out_path, .{
        .keep_asm = keep_asm,
        .cli_libdirs = cli_libdirs,
    }, writer);
}

pub fn cmdBuildOpts(allocator: std.mem.Allocator, path: []const u8, out_path: []const u8, opts: BuildOptions, writer: anytype) !void {
    var unit = try loadPathOpts(allocator, path, writer, .{
        .modules = opts.modules,
        .include_test_requires = opts.include_test_requires,
    });
    defer unit.deinit();
    var result = try check_mod.check(allocator, &unit.file);
    defer result.deinit();
    if (!result.ok) {
        for (result.diagnostics) |d| {
            try writer.print("error: {s}\n", .{d.message});
        }
        std.process.exit(1);
    }
    if (!result.has_main) {
        try writer.print("error: no fn main\n", .{});
        std.process.exit(1);
    }

    var program = ir.lower(allocator, &unit.file, &result) catch |err| {
        try writer.print("lower error: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer program.deinit();

    const mod_target: []const u8 = switch (opts.target) {
        .native => "linux",
        .wasm32_emscripten => "emscripten",
    };
    try mergeLinkMeta(allocator, &program, path, opts.cli_libdirs, mod_target);

    if (std.fs.path.dirname(out_path)) |dir| {
        std.fs.cwd().makePath(dir) catch {};
    }

    try link.buildExecutableTargeted(allocator, &program, out_path, opts.keep_asm, opts.keep_c, opts.target, writer);
}

fn mergeLinkMeta(allocator: std.mem.Allocator, program: *ir.Program, path: []const u8, cli_libdirs: []const []const u8, build_target: []const u8) !void {
    const a = program.arena.allocator();
    var libs: std.ArrayListUnmanaged([]const u8) = .{};
    var dirs: std.ArrayListUnmanaged([]const u8) = .{};
    for (program.link_libs) |l| try libs.append(a, l);
    for (program.libdirs) |d| try dirs.append(a, d);

    if (mod.isModulePath(path)) {
        if (mod.loadFromPath(allocator, path)) |manifest_val| {
            var m = manifest_val;
            defer m.deinit();
            if (m.linksFor(allocator, build_target)) |filtered| {
                defer {
                    for (filtered) |l| allocator.free(l);
                    allocator.free(filtered);
                }
                for (filtered) |l| {
                    if (!containsStr(libs.items, l)) try libs.append(a, try a.dupe(u8, l));
                }
            } else |_| {}
            if (m.libdirsFor(allocator, build_target)) |resolved| {
                defer {
                    for (resolved) |p| allocator.free(p);
                    allocator.free(resolved);
                }
                for (resolved) |d| {
                    if (!containsStr(dirs.items, d)) try dirs.append(a, try a.dupe(u8, d));
                }
            } else |_| {}
        } else |_| {}
    }
    for (cli_libdirs) |d| {
        if (!containsStr(dirs.items, d)) try dirs.append(a, try a.dupe(u8, d));
    }
    program.link_libs = try libs.toOwnedSlice(a);
    program.libdirs = try dirs.toOwnedSlice(a);
}

fn containsStr(hay: []const []const u8, needle: []const u8) bool {
    for (hay) |e| {
        if (std.mem.eql(u8, e, needle)) return true;
    }
    return false;
}

pub fn cmdDeps(allocator: std.mem.Allocator, path: []const u8, writer: anytype) !void {
    try cmdDepsOpts(allocator, path, writer, false, false);
}

/// `flags`: emit `--module=name=absdir` lines (LSP / jog-build deps --flags).
/// `include_test`: also emit mappings for `require.test`.
pub fn cmdDepsOpts(allocator: std.mem.Allocator, path: []const u8, writer: anytype, flags: bool, include_test: bool) !void {
    var manifest = mod.loadFromPath(allocator, path) catch |err| {
        try writer.print("deps: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
    defer manifest.deinit();

    if (flags) {
        var cwd_buf: [std.fs.max_path_bytes]u8 = undefined;
        const cwd = std.fs.cwd().realpath(".", &cwd_buf) catch ".";
        for (manifest.requires) |req| {
            if (req.test_only and !include_test) continue;
            if (!mod.isPathSpec(req.spec)) continue;
            const rel = mod.resolveRequireDir(allocator, &manifest, req.spec) catch continue;
            defer allocator.free(rel);
            const abs = std.fs.cwd().realpathAlloc(allocator, rel) catch try allocator.dupe(u8, rel);
            defer allocator.free(abs);
            _ = cwd;
            try writer.print("--module={s}={s}\n", .{ req.name, abs });
        }
        return;
    }

    const paths = try mod.resolveSourcePaths(allocator, &manifest);
    defer {
        for (paths) |p| allocator.free(p);
        allocator.free(paths);
    }
    for (paths) |p| {
        try writer.print("{s}\n", .{p});
    }
}

pub fn usage(writer: anytype) !void {
    try writer.writeAll(
        \\usage: jog <command> [args]
        \\
        \\commands:
        \\  version                 print version
        \\  parse <path>            parse and list declarations
        \\  symbols [--json] <path> document symbols (LSP)
        \\  check [--diag=json] [--test] [--module name=dir]... <path>
        \\                          typecheck subset (JSON diags for LSP)
        \\  build <path> -o OUT [--keep-asm] [--keep-c]
        \\                          [--target wasm32-emscripten]
        \\                          [--libdir DIR | -L DIR]...
        \\                          [--module name=dir]...
        \\                          native: IR → asm → zig cc
        \\                          wasm32-emscripten: IR → C → emcc (.html/.js)
        \\                          @link / jog.mod link.* → -l; libdir → -L (+ rpath native)
        \\                          jog.mod require path| --module for deps
        \\  deps [--flags] [--test] [path]
        \\                          source paths, or --module flags for LSP
        \\  roots                   print std/core discovery ($JOG_ROOT or next to jog)
        \\  help                    this message
        \\
        \\path may be a .jog file, a directory with jog.mod, or a jog.mod file.
        \\std/core: $JOG_ROOT/{std,core} or <jog>/../{std,core} (see `jog roots`).
        \\
    );
}
