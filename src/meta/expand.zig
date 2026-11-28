//! User-annotation expand (JOG-228).
//!
//! No comptime interpreter yet. After parse/merge:
//! - resolve user `@Name` attrs against `@Annotation` defs
//! - marker annotations (empty body) generate nothing
//! - bodies that are only `c.add(quote { … })` are expanded by rendering the
//!   quote (splices: `target.name`, string/int literals) and re-parsing

const std = @import("std");
const ast = @import("../ast/ast.zig");
const parse = @import("../parse/parse.zig");

pub const ExpandError = error{
    OutOfMemory,
    UnexpectedToken,
    Overflow,
    InvalidCharacter,
};

pub const ExpandDiag = struct {
    line: u32 = 1,
    col: u32 = 1,
    message: []const u8,
};

pub const ExpandResult = struct {
    diagnostics: []ExpandDiag,
    generated: bool = false,
};

/// Mutates `file.decls` in place. New nodes are allocated in `file.arena`.
pub fn expandAnnotations(file: *ast.File) ExpandError!ExpandResult {
    const a = file.arena.allocator();
    var diags: std.ArrayListUnmanaged(ExpandDiag) = .{};

    var ann_defs = std.StringHashMapUnmanaged(*ast.FnDecl){};
    for (file.decls) |*d| {
        switch (d.*) {
            .fn_decl, .actual_fn => |*fd| {
                if (fd.is_annotation) try ann_defs.put(a, fd.name, fd);
            },
            else => {},
        }
    }

    var generated: std.ArrayListUnmanaged(ast.Decl) = .{};
    const n_orig = file.decls.len;
    var i: usize = 0;
    while (i < n_orig) : (i += 1) {
        const d = &file.decls[i];
        const attrs = declAttrs(d);
        if (attrs.len == 0) continue;

        var seen = std.StringHashMapUnmanaged(void){};
        for (attrs) |attr| {
            if (isBuiltinAttr(attr.name)) continue;
            const def = ann_defs.get(attr.name) orelse {
                try diags.append(a, .{
                    .line = attr.line,
                    .col = attr.col,
                    .message = try std.fmt.allocPrint(a, "unknown annotation '@{s}'", .{attr.name}),
                });
                continue;
            };

            if (!def.annotation_repeatable) {
                if (seen.contains(attr.name)) {
                    try diags.append(a, .{
                        .line = attr.line,
                        .col = attr.col,
                        .message = try std.fmt.allocPrint(a, "annotation '@{s}' is not repeatable", .{attr.name}),
                    });
                    continue;
                }
                try seen.put(a, attr.name, {});
            }

            if (!placementOk(def, d.*)) {
                try diags.append(a, .{
                    .line = attr.line,
                    .col = attr.col,
                    .message = try std.fmt.allocPrint(a, "annotation '@{s}' cannot be used here", .{attr.name}),
                });
                continue;
            }

            if (def.body == null or def.body.?.stmts.len == 0) continue;

            var quotes: std.ArrayListUnmanaged(*ast.Quote) = .{};
            if (!collectAddQuotes(def, &quotes, a)) {
                try diags.append(a, .{
                    .line = def.line,
                    .col = def.col,
                    .message = try std.fmt.allocPrint(
                        a,
                        "annotation '@{s}': only empty bodies or `c.add(quote {{ … }})` are supported without a comptime interpreter",
                        .{attr.name},
                    ),
                });
                continue;
            }

            const target_name = declName(d.*) orelse "";
            for (quotes.items) |q| {
                const src = renderQuote(a, q, target_name) catch |err| switch (err) {
                    error.UnsupportedSplice => {
                        try diags.append(a, .{
                            .line = attr.line,
                            .col = attr.col,
                            .message = try std.fmt.allocPrint(
                                a,
                                "annotation '@{s}': unsupported quote splice (only target.name and literals in this slice)",
                                .{attr.name},
                            ),
                        });
                        continue;
                    },
                    else => |e| return e,
                };
                const owned_src = try a.dupe(u8, src);
                a.free(src);
                const keep = try a.create(ast.File);
                keep.* = parse.Parser.parseFile(a, owned_src) catch {
                    try diags.append(a, .{
                        .line = attr.line,
                        .col = attr.col,
                        .message = try std.fmt.allocPrint(
                            a,
                            "annotation '@{s}': generated code failed to parse:\n{s}",
                            .{ attr.name, owned_src },
                        ),
                    });
                    continue;
                };
                try generated.appendSlice(a, keep.decls);
            }
        }
    }

    const did_gen = generated.items.len > 0;
    if (did_gen) {
        var all: std.ArrayListUnmanaged(ast.Decl) = .{};
        try all.appendSlice(a, file.decls);
        try all.appendSlice(a, generated.items);
        file.decls = try all.toOwnedSlice(a);
    }

    return .{
        .diagnostics = try diags.toOwnedSlice(a),
        .generated = did_gen,
    };
}

fn isBuiltinAttr(name: []const u8) bool {
    const builtins = [_][]const u8{
        "link",     "export",     "test",      "packed",   "intrinsic", "opaque",
        "union",    "align",      "unchecked", "extern",   "systemStack", "blocking",
        "bench",    "fuzz",       "Annotation", "comptime", "embed",
    };
    for (builtins) |b| if (std.mem.eql(u8, name, b)) return true;
    return false;
}

fn declAttrs(d: *ast.Decl) []ast.Attr {
    return switch (d.*) {
        .fn_decl, .actual_fn => |*fd| fd.attrs,
        .struct_decl => |*sd| sd.attrs,
        .enum_decl => |*ed| ed.attrs,
        .global_decl => |*gd| gd.attrs,
        else => &.{},
    };
}

fn declName(d: ast.Decl) ?[]const u8 {
    return switch (d) {
        .fn_decl, .actual_fn => |fd| fd.name,
        .struct_decl => |sd| sd.name,
        .enum_decl => |ed| ed.name,
        .global_decl => |gd| gd.name,
        .method => |md| md.name,
        else => null,
    };
}

fn placementOk(def: *const ast.FnDecl, d: ast.Decl) bool {
    if (def.params.len == 0) return true;
    return switch (targetKindFromType(def.params[0].ty)) {
        .any_decl => switch (d) {
            .fn_decl, .actual_fn, .struct_decl, .enum_decl, .global_decl, .method => true,
            else => false,
        },
        .fn_only => d == .fn_decl or d == .actual_fn,
        .struct_only => d == .struct_decl,
        .enum_only => d == .enum_decl,
        .field_only => false,
        .unknown => true,
    };
}

const TargetKind = enum { any_decl, fn_only, struct_only, enum_only, field_only, unknown };

fn targetKindFromType(ty: *ast.TypeExpr) TargetKind {
    const leaf = typeLeafName(ty) orelse return .unknown;
    // Qualified names are stored as `meta.Fn` (see parseTypePrimary).
    const base = if (std.mem.lastIndexOfScalar(u8, leaf, '.')) |dot| leaf[dot + 1 ..] else leaf;
    if (std.mem.eql(u8, base, "Fn")) return .fn_only;
    if (std.mem.eql(u8, base, "Struct")) return .struct_only;
    if (std.mem.eql(u8, base, "Enum")) return .enum_only;
    if (std.mem.eql(u8, base, "Field")) return .field_only;
    if (std.mem.eql(u8, base, "Decl")) return .any_decl;
    return .unknown;
}

fn typeLeafName(ty: *ast.TypeExpr) ?[]const u8 {
    return switch (ty.*) {
        .name => |n| n,
        .pointer, .pointer_const, .optional => |inner| typeLeafName(inner),
        .named_app => |na| na.name,
        else => null,
    };
}

/// Collect `c.add(quote{…})` quote nodes. Returns false if the body has other statements.
fn collectAddQuotes(def: *const ast.FnDecl, out: *std.ArrayListUnmanaged(*ast.Quote), a: std.mem.Allocator) bool {
    const body = def.body orelse return true;
    if (body.stmts.len == 0) return true;
    for (body.stmts) |st| {
        const e = switch (st) {
            .expr => |ex| ex,
            else => return false,
        };
        const q = exprIsAddQuote(e) orelse return false;
        out.append(a, q) catch return false;
    }
    return true;
}

fn exprIsAddQuote(e: *ast.Expr) ?*ast.Quote {
    const call_ptr = switch (e.*) {
        .call => |*c| c,
        else => return null,
    };
    if (call_ptr.args.len != 1) return null;
    if (call_ptr.callee.* != .field) return null;
    if (!std.mem.eql(u8, call_ptr.callee.field.name, "add")) return null;
    return switch (call_ptr.args[0].*) {
        .quote => |*q| q,
        else => null,
    };
}

const RenderError = ExpandError || error{UnsupportedSplice};

fn renderQuote(a: std.mem.Allocator, q: *const ast.Quote, target_name: []const u8) RenderError![]u8 {
    var sb: std.ArrayListUnmanaged(u8) = .{};
    errdefer sb.deinit(a);
    for (q.parts) |part| {
        switch (part) {
            .text => |t| try sb.appendSlice(a, t),
            .splice => |ex| {
                const rendered = try renderSplice(a, ex, target_name);
                defer a.free(rendered);
                try sb.appendSlice(a, rendered);
            },
        }
    }
    return try sb.toOwnedSlice(a);
}

fn renderSplice(a: std.mem.Allocator, e: *ast.Expr, target_name: []const u8) RenderError![]u8 {
    if (e.* == .field) {
        const f = e.field;
        if (f.base.* == .ident and std.mem.eql(u8, f.base.ident, "target") and std.mem.eql(u8, f.name, "name")) {
            return try a.dupe(u8, target_name);
        }
    }
    if (e.* == .ident and std.mem.eql(u8, e.ident, "target")) {
        return try a.dupe(u8, target_name);
    }
    if (e.* == .string) {
        var sb: std.ArrayListUnmanaged(u8) = .{};
        try sb.append(a, '"');
        for (e.string) |c| {
            if (c == '"' or c == '\\') try sb.append(a, '\\');
            try sb.append(a, c);
        }
        try sb.append(a, '"');
        return try sb.toOwnedSlice(a);
    }
    if (e.* == .int) {
        return try std.fmt.allocPrint(a, "{d}", .{e.int});
    }
    return error.UnsupportedSplice;
}

test "placement rejects struct for meta.Fn" {
    const src =
        \\@Annotation
        \\fn OnlyFn(target: *const meta.Fn) {}
        \\@OnlyFn
        \\struct S { x: i64 }
        \\
    ;
    var file = try @import("../parse/parse.zig").Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    // Move decls into file.arena ownership — parseFile already did.
    const r = try expandAnnotations(&file);
    try std.testing.expect(r.diagnostics.len >= 1);
    try std.testing.expect(std.mem.indexOf(u8, r.diagnostics[0].message, "cannot be used") != null);
}
