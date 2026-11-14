//! `jog ide` — JSON position queries for JogLSP (hover / definition / complete /
//! references / workspace-ish symbols). Mirrors the retired jogc `hover_at` /
//! `definition_at` / `complete_at` pattern: load the same graph as `check`
//! (including `.jog-lsp-tmp` overrides), then hit-test at 1-based line/column.
const std = @import("std");
const ast = @import("../ast/ast.zig");
const lex = @import("../lex/lex.zig");
const driver = @import("driver.zig");

pub const Mode = enum { hover, definition, complete, references, symbols };

pub fn run(
    allocator: std.mem.Allocator,
    mode: Mode,
    path: []const u8,
    line: u32,
    column: u32,
    writer: anytype,
    opts: driver.LoadOptions,
) !void {
    var unit = try driver.loadPathOpts(allocator, path, writer, opts);
    defer unit.deinit();

    const query_path = driver_strip_tmp(path);
    switch (mode) {
        .symbols => try emitSymbols(allocator, &unit, writer),
        .hover => try emitHover(allocator, &unit, query_path, line, column, writer),
        .definition => try emitDefinition(allocator, &unit, query_path, line, column, writer),
        .complete => try emitComplete(allocator, &unit, query_path, line, column, writer),
        .references => try emitReferences(allocator, &unit, query_path, line, column, writer),
    }
}

fn driver_strip_tmp(path: []const u8) []const u8 {
    const suf = ".jog-lsp-tmp";
    if (std.mem.endsWith(u8, path, suf)) return path[0 .. path.len - suf.len];
    return path;
}

const Loc = struct {
    file: []const u8,
    line: u32,
    column: u32,
    end_line: u32,
    end_column: u32,
};

const Hit = struct {
    name: []const u8,
    line: u32,
    column: u32,
    /// True when the cursor sits on `base.name` after a `.`.
    is_member: bool,
    /// Identifier immediately before `.` when `is_member` (simple `x.y` only).
    base_name: []const u8 = "",
};

fn emitNullOk(writer: anytype) !void {
    try writer.writeAll("{\"ok\":false}\n");
}

/// Tiny JSON string escape into a threadlocal buffer (paths are short).
/// Not safe to call twice in the same expression — use `jsonEscapeAlloc` then.
threadlocal var json_path_buf: [4096]u8 = undefined;

fn try_json_string_static(s: []const u8) []const u8 {
    var fbs = std.io.fixedBufferStream(&json_path_buf);
    const w = fbs.writer();
    w.writeByte('"') catch return "\"\"";
    for (s) |c| {
        switch (c) {
            '"' => w.writeAll("\\\"") catch return "\"\"",
            '\\' => w.writeAll("\\\\") catch return "\"\"",
            '\n' => w.writeAll("\\n") catch return "\"\"",
            '\r' => w.writeAll("\\r") catch return "\"\"",
            '\t' => w.writeAll("\\t") catch return "\"\"",
            else => w.writeByte(c) catch return "\"\"",
        }
    }
    w.writeByte('"') catch return "\"\"";
    return fbs.getWritten();
}

fn jsonEscapeAlloc(allocator: std.mem.Allocator, s: []const u8) ![]u8 {
    var buf: std.ArrayListUnmanaged(u8) = .{};
    errdefer buf.deinit(allocator);
    try buf.append(allocator, '"');
    for (s) |c| {
        switch (c) {
            '"' => try buf.appendSlice(allocator, "\\\""),
            '\\' => try buf.appendSlice(allocator, "\\\\"),
            '\n' => try buf.appendSlice(allocator, "\\n"),
            '\r' => try buf.appendSlice(allocator, "\\r"),
            '\t' => try buf.appendSlice(allocator, "\\t"),
            else => try buf.append(allocator, c),
        }
    }
    try buf.append(allocator, '"');
    return try buf.toOwnedSlice(allocator);
}

fn pathEq(a: []const u8, b: []const u8) bool {
    if (std.mem.eql(u8, a, b)) return true;
    // Basename fallback when relative vs absolute.
    return std.mem.eql(u8, std.fs.path.basename(a), std.fs.path.basename(b));
}

fn findFileIndex(unit: *const driver.Unit, query_path: []const u8) ?usize {
    for (unit.paths, 0..) |p, i| {
        if (pathEq(p, query_path)) return i;
    }
    // Single-file units still have paths[0].
    if (unit.paths.len == 1) return 0;
    return null;
}

fn fileAst(unit: *const driver.Unit, idx: usize) *const ast.File {
    if (unit.parts.len > 0) return &unit.parts[idx];
    return &unit.file;
}

fn fileSrc(unit: *const driver.Unit, idx: usize) []const u8 {
    return unit.srcs[idx];
}

fn filePath(unit: *const driver.Unit, idx: usize) []const u8 {
    return unit.paths[idx];
}

/// Ident under / at / just before 1-based line:column, plus member-dot context.
fn hitAt(src: []const u8, line: u32, column: u32) ?Hit {
    var lx = lex.Lexer.init(src);
    var best: ?Hit = null;
    var prev_ident: []const u8 = "";
    var last_was_dot = false;
    var last_dot_line: u32 = 0;
    var last_dot_col: u32 = 0;

    while (true) {
        const t = lx.next();
        if (t.kind == .eof) break;
        if (t.kind == .dot or t.kind == .dot_q) {
            last_was_dot = true;
            last_dot_line = t.line;
            last_dot_col = t.col;
            continue;
        }
        if (t.kind != .ident) {
            last_was_dot = false;
            continue;
        }
        const end_col = t.col + @as(u32, @intCast(t.text.len));
        const contains = t.line == line and t.col <= column and column <= end_col;
        const at_or_before = t.line < line or (t.line == line and t.col <= column);
        const member = last_was_dot and last_dot_line == t.line and last_dot_col < t.col and prev_ident.len > 0;

        if (contains or at_or_before) {
            best = .{
                .name = t.text,
                .line = t.line,
                .column = t.col,
                .is_member = member,
                .base_name = if (member) prev_ident else "",
            };
            if (contains) return best;
        }
        prev_ident = t.text;
        last_was_dot = false;
    }

    // Cursor after `.` with optional incomplete prefix: `x.` or `x.ab|`.
    if (best == null) {
        const dot = endsWithDotPrefix(src, line, column);
        if (dot.is_member) {
            return .{
                .name = dot.prefix,
                .line = line,
                .column = column,
                .is_member = true,
                .base_name = dot.base,
            };
        }
    } else if (best) |b| {
        // If sitting right after a trailing `.` past the last ident, treat as member complete.
        const dot = endsWithDotPrefix(src, line, column);
        if (dot.is_member and dot.prefix.len == 0) {
            return .{
                .name = "",
                .line = line,
                .column = column,
                .is_member = true,
                .base_name = if (dot.base.len > 0) dot.base else b.name,
            };
        }
    }
    return best;
}

const DotCtx = struct {
    is_member: bool,
    prefix: []const u8,
    base: []const u8,
};

fn endsWithDotPrefix(src: []const u8, line: u32, column: u32) DotCtx {
    var cur_line: u32 = 1;
    var i: usize = 0;
    while (i < src.len and cur_line < line) : (i += 1) {
        if (src[i] == '\n') cur_line += 1;
    }
    if (cur_line != line) return .{ .is_member = false, .prefix = "", .base = "" };
    const col: usize = if (column == 0) 0 else column - 1;
    if (col > src.len - i) return .{ .is_member = false, .prefix = "", .base = "" };
    var pos = i + col;
    if (pos > src.len) pos = src.len;

    // Skip back over incomplete identifier.
    var end = pos;
    while (end > i) {
        const c = src[end - 1];
        if ((c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or c == '_') {
            end -= 1;
            continue;
        }
        break;
    }
    const prefix = src[end..pos];
    if (end > i and src[end - 1] == '.') {
        // Walk back base ident.
        var b_end = end - 1;
        while (b_end > i and (src[b_end - 1] == '?' )) : (b_end -= 1) {} // ?.
        var b_start = b_end;
        while (b_start > i) {
            const c = src[b_start - 1];
            if ((c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or c == '_') {
                b_start -= 1;
                continue;
            }
            break;
        }
        return .{ .is_member = true, .prefix = prefix, .base = src[b_start..b_end] };
    }
    return .{ .is_member = false, .prefix = prefix, .base = "" };
}

const SymKind = enum { function, struct_ty, object, enum_ty, variable, constant, type_alias, interface, field, method, param };

const Sym = struct {
    name: []const u8,
    kind: SymKind,
    file: []const u8,
    line: u32,
    column: u32,
    /// Hover / signature text (owned by arena or static/slice into AST).
    detail: []const u8 = "",
    /// Struct/type this field or method belongs to.
    parent: []const u8 = "",
};

fn kindStr(k: SymKind) []const u8 {
    return switch (k) {
        .function => "function",
        .struct_ty => "struct",
        .object => "object",
        .enum_ty => "enum",
        .variable => "variable",
        .constant => "constant",
        .type_alias => "type",
        .interface => "interface",
        .field => "field",
        .method => "method",
        .param => "variable",
    };
}

fn typeExprString(allocator: std.mem.Allocator, te: *const ast.TypeExpr) ![]u8 {
    switch (te.*) {
        .name => |n| return try allocator.dupe(u8, n),
        .pointer => |inner| {
            const s = try typeExprString(allocator, inner);
            defer allocator.free(s);
            return try std.fmt.allocPrint(allocator, "*{s}", .{s});
        },
        .pointer_const => |inner| {
            const s = try typeExprString(allocator, inner);
            defer allocator.free(s);
            return try std.fmt.allocPrint(allocator, "*const {s}", .{s});
        },
        .optional => |inner| {
            const s = try typeExprString(allocator, inner);
            defer allocator.free(s);
            return try std.fmt.allocPrint(allocator, "{s}?", .{s});
        },
        .array => |arr| {
            const s = try typeExprString(allocator, arr.elem);
            defer allocator.free(s);
            return try std.fmt.allocPrint(allocator, "[{d}]{s}", .{ arr.len, s });
        },
        .named_app => |na| {
            var buf: std.ArrayListUnmanaged(u8) = .{};
            errdefer buf.deinit(allocator);
            try buf.appendSlice(allocator, na.name);
            try buf.append(allocator, '<');
            for (na.args, 0..) |arg, i| {
                if (i > 0) try buf.append(allocator, ',');
                const s = try typeExprString(allocator, arg);
                defer allocator.free(s);
                try buf.appendSlice(allocator, s);
            }
            try buf.append(allocator, '>');
            return try buf.toOwnedSlice(allocator);
        },
        .slice_view => |sv| {
            const s = try typeExprString(allocator, sv.elem);
            defer allocator.free(s);
            if (sv.is_const) return try std.fmt.allocPrint(allocator, "[]const {s}", .{s});
            return try std.fmt.allocPrint(allocator, "[]{s}", .{s});
        },
        .tuple => |elems| {
            var buf: std.ArrayListUnmanaged(u8) = .{};
            errdefer buf.deinit(allocator);
            try buf.append(allocator, '(');
            for (elems, 0..) |el, i| {
                if (i > 0) try buf.appendSlice(allocator, ", ");
                const s = try typeExprString(allocator, el);
                defer allocator.free(s);
                try buf.appendSlice(allocator, s);
            }
            try buf.append(allocator, ')');
            return try buf.toOwnedSlice(allocator);
        },
        .iface_fn => return try allocator.dupe(u8, "interface fn"),
        .bad => return try allocator.dupe(u8, "?"),
    }
}

fn formatFnDetail(allocator: std.mem.Allocator, name: []const u8, params: []ast.Param, ret: ?*ast.TypeExpr) ![]u8 {
    var buf: std.ArrayListUnmanaged(u8) = .{};
    errdefer buf.deinit(allocator);
    try buf.appendSlice(allocator, "fn ");
    try buf.appendSlice(allocator, name);
    try buf.append(allocator, '(');
    for (params, 0..) |p, i| {
        if (i > 0) try buf.appendSlice(allocator, ", ");
        try buf.appendSlice(allocator, p.name);
        try buf.appendSlice(allocator, ": ");
        const ts = try typeExprString(allocator, p.ty);
        defer allocator.free(ts);
        try buf.appendSlice(allocator, ts);
    }
    try buf.append(allocator, ')');
    if (ret) |r| {
        try buf.appendSlice(allocator, ": ");
        const ts = try typeExprString(allocator, r);
        defer allocator.free(ts);
        try buf.appendSlice(allocator, ts);
    }
    return try buf.toOwnedSlice(allocator);
}

const Index = struct {
    allocator: std.mem.Allocator,
    /// Arena for detail strings.
    arena: std.heap.ArenaAllocator,
    tops: std.ArrayListUnmanaged(Sym) = .{},
    /// name → first top-level sym index
    by_name: std.StringHashMapUnmanaged(usize) = .{},
    structs: std.StringHashMapUnmanaged(*const ast.StructDecl) = .{},
    enums: std.StringHashMapUnmanaged(*const ast.EnumDecl) = .{},

    fn deinit(self: *Index) void {
        self.tops.deinit(self.allocator);
        self.by_name.deinit(self.allocator);
        self.structs.deinit(self.allocator);
        self.enums.deinit(self.allocator);
        self.arena.deinit();
    }

    fn add(self: *Index, sym: Sym) !void {
        const idx = self.tops.items.len;
        try self.tops.append(self.allocator, sym);
        const gop = try self.by_name.getOrPut(self.allocator, sym.name);
        if (!gop.found_existing) gop.value_ptr.* = idx;
    }
};

fn buildIndex(allocator: std.mem.Allocator, unit: *const driver.Unit) !Index {
    var idx: Index = .{
        .allocator = allocator,
        .arena = std.heap.ArenaAllocator.init(allocator),
    };
    errdefer idx.deinit();
    const a = idx.arena.allocator();

    const n = unit.paths.len;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        const path = filePath(unit, i);
        const file = fileAst(unit, i);
        for (file.decls) |*d| {
            switch (d.*) {
                .fn_decl, .actual_fn => |*fd| {
                    const detail = try formatFnDetail(a, fd.name, fd.params, fd.ret);
                    try idx.add(.{
                        .name = fd.name,
                        .kind = .function,
                        .file = path,
                        .line = fd.line,
                        .column = fd.col,
                        .detail = detail,
                    });
                },
                .struct_decl => |*sd| {
                    try idx.structs.put(allocator, sd.name, sd);
                    try idx.add(.{
                        .name = sd.name,
                        .kind = if (sd.is_object) .object else .struct_ty,
                        .file = path,
                        .line = sd.line,
                        .column = sd.col,
                        .detail = if (sd.is_object) "object" else "struct",
                    });
                    for (sd.fields) |fld| {
                        const ts = try typeExprString(a, fld.ty);
                        const detail = try std.fmt.allocPrint(a, "{s}: {s}", .{ fld.name, ts });
                        try idx.add(.{
                            .name = fld.name,
                            .kind = .field,
                            .file = path,
                            .line = sd.line,
                            .column = sd.col,
                            .detail = detail,
                            .parent = sd.name,
                        });
                    }
                },
                .enum_decl => |*ed| {
                    try idx.enums.put(allocator, ed.name, ed);
                    try idx.add(.{
                        .name = ed.name,
                        .kind = .enum_ty,
                        .file = path,
                        .line = ed.line,
                        .column = ed.col,
                        .detail = "enum",
                    });
                },
                .global_decl => |*gd| {
                    var detail: []const u8 = if (gd.mutable) "var" else "val";
                    if (gd.ty) |te| {
                        const ts = try typeExprString(a, te);
                        detail = try std.fmt.allocPrint(a, "{s} {s}: {s}", .{ if (gd.mutable) "var" else "val", gd.name, ts });
                    } else {
                        detail = try std.fmt.allocPrint(a, "{s} {s}", .{ if (gd.mutable) "var" else "val", gd.name });
                    }
                    try idx.add(.{
                        .name = gd.name,
                        .kind = if (gd.mutable) .variable else .constant,
                        .file = path,
                        .line = gd.line,
                        .column = gd.col,
                        .detail = detail,
                    });
                },
                .type_alias => |*ta| {
                    const ts = try typeExprString(a, ta.ty);
                    const detail = try std.fmt.allocPrint(a, "type {s} = {s}", .{ ta.name, ts });
                    try idx.add(.{
                        .name = ta.name,
                        .kind = .type_alias,
                        .file = path,
                        .line = 1,
                        .column = 1,
                        .detail = detail,
                    });
                },
                .interface_fn => |*inf| {
                    const detail = try formatFnDetail(a, inf.name, inf.params, inf.ret);
                    try idx.add(.{
                        .name = inf.name,
                        .kind = .interface,
                        .file = path,
                        .line = 1,
                        .column = 1,
                        .detail = detail,
                    });
                },
                .interface_struct => |*ins| {
                    try idx.add(.{
                        .name = ins.name,
                        .kind = .interface,
                        .file = path,
                        .line = 1,
                        .column = 1,
                        .detail = "interface struct",
                    });
                },
                .method => |*md| {
                    const detail = try formatFnDetail(a, md.name, md.params, md.ret);
                    const full = try std.fmt.allocPrint(a, "{s}.{s}", .{ md.type_name, detail });
                    try idx.add(.{
                        .name = md.name,
                        .kind = .method,
                        .file = path,
                        .line = 1,
                        .column = 1,
                        .detail = full,
                        .parent = md.type_name,
                    });
                },
                .extern_block => |*eb| {
                    for (eb.fns) |*ef| {
                        const detail = try formatFnDetail(a, ef.name, ef.params, ef.ret);
                        // Extern names: locate via lexer on this file.
                        const loc = findNameInSrc(fileSrc(unit, i), ef.name) orelse Loc{
                            .file = path,
                            .line = 1,
                            .column = 1,
                            .end_line = 1,
                            .end_column = 1 + @as(u32, @intCast(ef.name.len)),
                        };
                        try idx.add(.{
                            .name = ef.name,
                            .kind = .function,
                            .file = path,
                            .line = loc.line,
                            .column = loc.column,
                            .detail = detail,
                        });
                    }
                },
                .expect_fn => |*ef| {
                    const detail = try formatFnDetail(a, ef.name, ef.params, ef.ret);
                    const loc = findNameInSrc(fileSrc(unit, i), ef.name) orelse Loc{
                        .file = path,
                        .line = 1,
                        .column = 1,
                        .end_line = 1,
                        .end_column = 1 + @as(u32, @intCast(ef.name.len)),
                    };
                    try idx.add(.{
                        .name = ef.name,
                        .kind = .function,
                        .file = path,
                        .line = loc.line,
                        .column = loc.column,
                        .detail = detail,
                    });
                },
                .import_decl, .require_decl => {},
            }
        }
    }
    return idx;
}

fn findNameInSrc(src: []const u8, name: []const u8) ?Loc {
    var lx = lex.Lexer.init(src);
    while (true) {
        const t = lx.next();
        if (t.kind == .eof) return null;
        if (t.kind == .ident and std.mem.eql(u8, t.text, name)) {
            return .{
                .file = "",
                .line = t.line,
                .column = t.col,
                .end_line = t.line,
                .end_column = t.col + @as(u32, @intCast(t.text.len)),
            };
        }
    }
}

fn locFromSym(s: Sym) Loc {
    const len: u32 = @intCast(s.name.len);
    return .{
        .file = s.file,
        .line = s.line,
        .column = s.column,
        .end_line = s.line,
        .end_column = s.column + len,
    };
}

fn lookupTop(idx: *const Index, name: []const u8) ?Sym {
    if (idx.by_name.get(name)) |i| return idx.tops.items[i];
    return null;
}

fn lookupMember(idx: *const Index, type_name: []const u8, member: []const u8) ?Sym {
    for (idx.tops.items) |s| {
        if (s.kind == .field or s.kind == .method) {
            if (std.mem.eql(u8, s.parent, type_name) and std.mem.eql(u8, s.name, member)) return s;
        }
    }
    // Enum variant: Type.Variant — variants aren't separate tops; scan enum.
    if (idx.enums.get(type_name)) |ed| {
        const parent = lookupTop(idx, type_name) orelse return null;
        for (ed.variants) |v| {
            if (std.mem.eql(u8, v.name, member)) {
                return .{
                    .name = v.name,
                    .kind = .constant,
                    .file = parent.file,
                    .line = parent.line,
                    .column = parent.column,
                    .detail = "enum variant",
                    .parent = type_name,
                };
            }
        }
    }
    return null;
}

fn resolveTypeNameOfIdent(idx: *const Index, unit: *const driver.Unit, file_i: usize, name: []const u8, at_line: u32, at_col: u32) ?[]const u8 {
    // Locals / params with explicit types in the enclosing function.
    if (localTypeAt(unit, file_i, name, at_line, at_col)) |t| return t;
    if (lookupTop(idx, name)) |s| {
        switch (s.kind) {
            .struct_ty, .object, .enum_ty, .type_alias, .interface => return s.name,
            .variable, .constant => {
                // detail like `val x: Foo` — extract after `: `.
                if (std.mem.indexOf(u8, s.detail, ": ")) |p| {
                    return s.detail[p + 2 ..];
                }
            },
            else => {},
        }
    }
    return null;
}

fn localTypeAt(unit: *const driver.Unit, file_i: usize, name: []const u8, at_line: u32, at_col: u32) ?[]const u8 {
    _ = at_col;
    const file = fileAst(unit, file_i);
    for (file.decls) |d| {
        const fd: ?*const ast.FnDecl = switch (d) {
            .fn_decl, .actual_fn => |*f| f,
            else => null,
        };
        const f = fd orelse continue;
        // Rough: function name line ≤ cursor (no end line). Prefer innermost later.
        if (f.line > at_line) continue;
        for (f.params) |p| {
            if (std.mem.eql(u8, p.name, name)) {
                return typeNameLeaf(p.ty);
            }
        }
        if (f.body) |body| {
            if (findLocalInBlock(body, name)) |ty| return ty;
        }
    }
    return null;
}

fn typeNameLeaf(te: *ast.TypeExpr) ?[]const u8 {
    return switch (te.*) {
        .name => |n| n,
        .pointer => |i| typeNameLeaf(i),
        .pointer_const => |i| typeNameLeaf(i),
        .optional => |i| typeNameLeaf(i),
        .named_app => |na| na.name,
        else => null,
    };
}

fn findLocalInBlock(block: *ast.Block, name: []const u8) ?[]const u8 {
    for (block.stmts) |stmt| {
        switch (stmt) {
            .var_decl => |vd| {
                if (std.mem.eql(u8, vd.name, name)) {
                    if (vd.ty) |t| return typeNameLeaf(t);
                    return "i64"; // inferred fallback
                }
            },
            .if_stmt => |ifs| {
                if (findLocalInBlock(ifs.then_block, name)) |t| return t;
                if (ifs.else_block) |eb| if (findLocalInBlock(eb, name)) |t| return t;
            },
            .while_stmt => |ws| {
                if (findLocalInBlock(ws.body, name)) |t| return t;
            },
            .for_in_stmt => |fs| {
                if (std.mem.eql(u8, fs.name, name)) return "i64";
                if (findLocalInBlock(fs.body, name)) |t| return t;
            },
            .with_stmt => |ws| {
                if (findLocalInBlock(ws.body, name)) |t| return t;
            },
            else => {},
        }
    }
    return null;
}

fn findLocalDeclLoc(unit: *const driver.Unit, file_i: usize, name: []const u8, at_line: u32) ?Loc {
    const file = fileAst(unit, file_i);
    const path = filePath(unit, file_i);
    for (file.decls) |d| {
        const fd: ?*const ast.FnDecl = switch (d) {
            .fn_decl, .actual_fn => |*f| f,
            else => null,
        };
        const f = fd orelse continue;
        if (f.line > at_line) continue;
        for (f.params) |p| {
            if (std.mem.eql(u8, p.name, name)) {
                // Approximate: search name in function source region via lexer from fn line.
                if (findNameAfterLine(fileSrc(unit, file_i), name, f.line)) |loc| {
                    return .{
                        .file = path,
                        .line = loc.line,
                        .column = loc.column,
                        .end_line = loc.end_line,
                        .end_column = loc.end_column,
                    };
                }
            }
        }
        if (f.body) |body| {
            if (findVarDeclInBlock(body, name)) |vd| {
                return .{
                    .file = path,
                    .line = vd.line,
                    .column = vd.col,
                    .end_line = vd.line,
                    .end_column = vd.col + @as(u32, @intCast(name.len)),
                };
            }
        }
    }
    return null;
}

fn findNameAfterLine(src: []const u8, name: []const u8, min_line: u32) ?Loc {
    var lx = lex.Lexer.init(src);
    while (true) {
        const t = lx.next();
        if (t.kind == .eof) return null;
        if (t.line < min_line) continue;
        if (t.kind == .ident and std.mem.eql(u8, t.text, name)) {
            return .{
                .file = "",
                .line = t.line,
                .column = t.col,
                .end_line = t.line,
                .end_column = t.col + @as(u32, @intCast(t.text.len)),
            };
        }
    }
}

fn findVarDeclInBlock(block: *ast.Block, name: []const u8) ?ast.VarDecl {
    for (block.stmts) |stmt| {
        switch (stmt) {
            .var_decl => |vd| {
                if (std.mem.eql(u8, vd.name, name)) return vd;
            },
            .if_stmt => |ifs| {
                if (findVarDeclInBlock(ifs.then_block, name)) |v| return v;
                if (ifs.else_block) |eb| if (findVarDeclInBlock(eb, name)) |v| return v;
            },
            .while_stmt => |ws| {
                if (findVarDeclInBlock(ws.body, name)) |v| return v;
            },
            .for_in_stmt => |fs| {
                if (findVarDeclInBlock(fs.body, name)) |v| return v;
            },
            .with_stmt => |ws| {
                if (findVarDeclInBlock(ws.body, name)) |v| return v;
            },
            else => {},
        }
    }
    return null;
}

fn resolveHit(idx: *const Index, unit: *const driver.Unit, file_i: usize, hit: Hit) ?Sym {
    if (hit.is_member) {
        const tname = resolveTypeNameOfIdent(idx, unit, file_i, hit.base_name, hit.line, hit.column) orelse hit.base_name;
        if (hit.name.len == 0) return null;
        if (lookupMember(idx, tname, hit.name)) |s| return s;
        // Fall back: treat as top-level (e.g. std fn called unqualified after import merge).
    }
    if (lookupTop(idx, hit.name)) |s| {
        // Prefer non-field tops for bare names.
        if (s.kind != .field and s.kind != .method) return s;
        // If the first entry is a field (name collision), search for a better top.
        for (idx.tops.items) |cand| {
            if (std.mem.eql(u8, cand.name, hit.name) and cand.kind != .field and cand.kind != .method) return cand;
        }
        return s;
    }
    // Local variable / param — synthesize a Sym.
    if (findLocalDeclLoc(unit, file_i, hit.name, hit.line)) |loc| {
        const ty = localTypeAt(unit, file_i, hit.name, hit.line, hit.column) orelse "";
        return .{
            .name = hit.name,
            .kind = .variable,
            .file = loc.file,
            .line = loc.line,
            .column = loc.column,
            .detail = ty,
        };
    }
    return null;
}

fn emitHover(allocator: std.mem.Allocator, unit: *const driver.Unit, query_path: []const u8, line: u32, column: u32, writer: anytype) !void {
    const fi = findFileIndex(unit, query_path) orelse {
        try emitNullOk(writer);
        return;
    };
    const hit = hitAt(fileSrc(unit, fi), line, column) orelse {
        try emitNullOk(writer);
        return;
    };
    var idx = try buildIndex(allocator, unit);
    defer idx.deinit();
    const sym = resolveHit(&idx, unit, fi, hit) orelse {
        try emitNullOk(writer);
        return;
    };
    const loc = locFromSym(sym);
    const contents = if (sym.detail.len > 0) sym.detail else kindStr(sym.kind);
    const esc = try jsonEscapeAlloc(allocator, contents);
    defer allocator.free(esc);
    try writer.writeAll("{\"ok\":true,\"contents\":");
    try writer.writeAll(esc);
    try writer.writeAll(",");
    try writer.print(
        \\"file":{s},"line":{d},"column":{d},"end_line":{d},"end_column":{d},"kind":"{s}"
    , .{
        try_json_string_static(loc.file),
        loc.line,
        loc.column,
        loc.end_line,
        loc.end_column,
        kindStr(sym.kind),
    });
    try writer.writeAll("}\n");
}

fn emitDefinition(allocator: std.mem.Allocator, unit: *const driver.Unit, query_path: []const u8, line: u32, column: u32, writer: anytype) !void {
    const fi = findFileIndex(unit, query_path) orelse {
        try emitNullOk(writer);
        return;
    };
    const hit = hitAt(fileSrc(unit, fi), line, column) orelse {
        try emitNullOk(writer);
        return;
    };
    var idx = try buildIndex(allocator, unit);
    defer idx.deinit();
    const sym = resolveHit(&idx, unit, fi, hit) orelse {
        try emitNullOk(writer);
        return;
    };
    const loc = locFromSym(sym);
    const file_j = try jsonEscapeAlloc(allocator, loc.file);
    defer allocator.free(file_j);
    const name_j = try jsonEscapeAlloc(allocator, sym.name);
    defer allocator.free(name_j);
    try writer.print(
        "{{\"ok\":true,\"file\":{s},\"line\":{d},\"column\":{d},\"end_line\":{d},\"end_column\":{d},\"name\":{s}}}\n",
        .{ file_j, loc.line, loc.column, loc.end_line, loc.end_column, name_j },
    );
}

fn emitComplete(allocator: std.mem.Allocator, unit: *const driver.Unit, query_path: []const u8, line: u32, column: u32, writer: anytype) !void {
    const fi = findFileIndex(unit, query_path) orelse {
        try writer.writeAll("{\"ok\":true,\"items\":[]}\n");
        return;
    };
    const src = fileSrc(unit, fi);
    const dot = endsWithDotPrefix(src, line, column);
    var idx = try buildIndex(allocator, unit);
    defer idx.deinit();

    var items: std.ArrayListUnmanaged(Sym) = .{};
    defer items.deinit(allocator);

    if (dot.is_member) {
        const tname = resolveTypeNameOfIdent(&idx, unit, fi, dot.base, line, column) orelse dot.base;
        for (idx.tops.items) |s| {
            if ((s.kind == .field or s.kind == .method) and std.mem.eql(u8, s.parent, tname)) {
                if (dot.prefix.len == 0 or std.mem.startsWith(u8, s.name, dot.prefix)) {
                    try items.append(allocator, s);
                }
            }
        }
        // Enum variants.
        if (idx.enums.get(tname)) |ed| {
            for (ed.variants) |v| {
                if (dot.prefix.len == 0 or std.mem.startsWith(u8, v.name, dot.prefix)) {
                    try items.append(allocator, .{
                        .name = v.name,
                        .kind = .constant,
                        .file = "",
                        .line = 1,
                        .column = 1,
                        .detail = "enum variant",
                        .parent = tname,
                    });
                }
            }
        }
    } else {
        const prefix = blk: {
            if (hitAt(src, line, column)) |h| break :blk h.name;
            break :blk "";
        };
        // Dedup by name for scope completion.
        var seen: std.StringHashMapUnmanaged(void) = .{};
        defer seen.deinit(allocator);
        for (idx.tops.items) |s| {
            if (s.kind == .field) continue; // fields only after `.`
            if (prefix.len > 0 and !std.mem.startsWith(u8, s.name, prefix)) continue;
            if (seen.contains(s.name)) continue;
            try seen.put(allocator, s.name, {});
            try items.append(allocator, s);
        }
    }

    try writer.writeAll("{\"ok\":true,\"items\":[");
    for (items.items, 0..) |s, i| {
        if (i > 0) try writer.writeAll(",");
        try writer.print("{{\"label\":{s},\"kind\":\"{s}\"", .{ try_json_string_static(s.name), kindStr(s.kind) });
        if (s.detail.len > 0) {
            const esc = try jsonEscapeAlloc(allocator, s.detail);
            defer allocator.free(esc);
            try writer.writeAll(",\"detail\":");
            try writer.writeAll(esc);
        }
        try writer.writeAll("}");
    }
    try writer.writeAll("]}\n");
}

fn emitReferences(allocator: std.mem.Allocator, unit: *const driver.Unit, query_path: []const u8, line: u32, column: u32, writer: anytype) !void {
    const fi = findFileIndex(unit, query_path) orelse {
        try writer.writeAll("{\"ok\":true,\"refs\":[]}\n");
        return;
    };
    const hit = hitAt(fileSrc(unit, fi), line, column) orelse {
        try writer.writeAll("{\"ok\":true,\"refs\":[]}\n");
        return;
    };
    var idx = try buildIndex(allocator, unit);
    defer idx.deinit();
    // Resolve to canonical name (member uses member name).
    const name = hit.name;
    if (name.len == 0) {
        try writer.writeAll("{\"ok\":true,\"refs\":[]}\n");
        return;
    }

    try writer.writeAll("{\"ok\":true,\"refs\":[");
    var first = true;
    const n = unit.paths.len;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        const path = filePath(unit, i);
        const src = fileSrc(unit, i);
        var lx = lex.Lexer.init(src);
        while (true) {
            const t = lx.next();
            if (t.kind == .eof) break;
            if (t.kind != .ident) continue;
            if (!std.mem.eql(u8, t.text, name)) continue;
            if (!first) try writer.writeAll(",");
            first = false;
            try writer.print(
                "{{\"file\":{s},\"line\":{d},\"column\":{d},\"end_line\":{d},\"end_column\":{d}}}",
                .{
                    try_json_string_static(path),
                    t.line,
                    t.col,
                    t.line,
                    t.col + @as(u32, @intCast(t.text.len)),
                },
            );
        }
    }
    try writer.writeAll("]}\n");
}

fn emitSymbols(allocator: std.mem.Allocator, unit: *const driver.Unit, writer: anytype) !void {
    var idx = try buildIndex(allocator, unit);
    defer idx.deinit();
    for (idx.tops.items) |s| {
        // Workspace/symbol: top-level only (skip bare fields; methods stay).
        if (s.kind == .field) continue;
        const loc = locFromSym(s);
        var buf: std.ArrayListUnmanaged(u8) = .{};
        defer buf.deinit(allocator);
        try std.json.stringify(.{
            .name = s.name,
            .kind = kindStr(s.kind),
            .file = loc.file,
            .line = loc.line,
            .column = loc.column,
            .end_line = loc.end_line,
            .end_column = loc.end_column,
        }, .{}, buf.writer(allocator));
        try writer.writeAll(buf.items);
        try writer.writeAll("\n");
    }
}
