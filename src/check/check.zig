const std = @import("std");
const ast = @import("../ast/ast.zig");

pub const CheckError = error{
    TypeError,
    UnknownName,
    OutOfMemory,
};

pub const TypeKind = enum {
    void_ty,
    byte_ty,
    bool_ty,
    u16_ty,
    u32_ty,
    u64_ty,
    i32_ty,
    i64_ty,
    f32_ty,
    f64_ty,
    usize_ty,
    named,
    pointer,
    optional,
    array_ty,
    struct_ty,
    enum_ty,
    iface_fn,
    iface_struct,
    string_ty,
    bytes_ty,
};

pub const TypeInfo = struct {
    kind: TypeKind,
    name: []const u8 = "",
    pointee: ?*TypeInfo = null,
    /// Fixed array length when `kind == .array_ty`.
    array_len: u64 = 0,
    /// bit width for integers (for widen/narrow)
    bits: u16 = 0,
    unsigned: bool = true,
    /// Element types for product `(T, U, …)` (`name` is `__TupN`).
    tuple_elems: []*TypeInfo = &.{},
    /// Backing decl when `kind == .enum_ty` (payloads / comparability).
    enum_decl: ?*ast.EnumDecl = null,
};

pub const Diagnostic = struct {
    line: u32 = 1,
    col: u32 = 1,
    /// Exclusive end (1-based). Zero means unset (point diagnostic).
    end_line: u32 = 0,
    end_col: u32 = 0,
    /// `"error"` | `"warning"` | `"info"` | `"hint"` — LSP maps these.
    severity: []const u8 = "error",
    message: []const u8,
};

/// Active file while `check` runs (CLI is single-threaded). Used to attach
/// expression spans to diagnostics without threading File through every helper.
threadlocal var active_file: ?*const ast.File = null;
/// Import bind names (`as` alias, else last path segment) for `pkg.Name` resolution
/// under flattened merge. Set for the duration of `check`.
threadlocal var active_imports: ?*const std.StringHashMapUnmanaged(void) = null;

fn importBindName(path: []const u8, alias: ?[]const u8) []const u8 {
    if (alias) |a| return a;
    if (std.mem.lastIndexOfScalar(u8, path, '.')) |dot| return path[dot + 1 ..];
    return path;
}

fn isImportBind(name: []const u8) bool {
    if (active_imports) |imps| return imps.contains(name);
    return false;
}

fn emitDiag(
    a: std.mem.Allocator,
    diags: *std.ArrayListUnmanaged(Diagnostic),
    at: ?*const ast.Expr,
    line: u32,
    col: u32,
    message: []const u8,
) !void {
    var d: Diagnostic = .{ .line = line, .col = col, .message = message, .severity = "error" };
    if (at) |e| {
        if (active_file) |f| {
            const sp = f.spanOf(e);
            d.line = sp.line;
            d.col = sp.col;
            // Identifiers get a real end column; other exprs get a 1-char range
            // until the parser records full end spans.
            d.end_line = sp.line;
            if (e.* == .ident) {
                d.end_col = sp.col + @as(u32, @intCast(e.ident.len));
            } else {
                d.end_col = sp.col + 1;
            }
        }
    }
    try diags.append(a, d);
}

pub const CheckResult = struct {
    ok: bool,
    diagnostics: []Diagnostic,
    /// true if file defines fn main that can lower to hello-style print
    has_main: bool,
    main_prints_string: ?[]const u8,
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *CheckResult) void {
        self.arena.deinit();
    }
};

const Builtin = struct {
    name: []const u8,
    ty: TypeInfo,
};

pub fn check(parent: std.mem.Allocator, file: *const ast.File) CheckError!CheckResult {
    var result: CheckResult = .{
        .ok = false,
        .diagnostics = &[_]Diagnostic{},
        .has_main = false,
        .main_prints_string = null,
        .arena = std.heap.ArenaAllocator.init(parent),
    };
    errdefer result.deinit();
    const a = result.arena.allocator();
    const prev_file = active_file;
    active_file = file;
    defer active_file = prev_file;

    var diags: std.ArrayListUnmanaged(Diagnostic) = .{};
    var types = std.StringHashMapUnmanaged(*TypeInfo){};
    var structs = std.StringHashMapUnmanaged(*ast.StructDecl){};
    var iface_structs = std.StringHashMapUnmanaged(*ast.InterfaceStruct){};
    var iface_fns = std.StringHashMapUnmanaged(*ast.InterfaceFn){};
    var fns = std.StringHashMapUnmanaged(*ast.FnDecl){};
    var methods = std.StringHashMapUnmanaged(*ast.MethodDecl){};
    var import_binds: std.StringHashMapUnmanaged(void) = .{};

    // Transitional substrate builtins (Spec: load from core/scalar.jog). Keep thin —
    // library APIs belong in std/; see JogDocs Friction 2026-11-14 std/core port.
    try defineBuiltin(a, &types, "byte", .{ .kind = .byte_ty, .name = "byte", .bits = 8, .unsigned = true });
    try defineBuiltin(a, &types, "u8", .{ .kind = .byte_ty, .name = "u8", .bits = 8, .unsigned = true });
    try defineBuiltin(a, &types, "i8", .{ .kind = .byte_ty, .name = "i8", .bits = 8, .unsigned = false });
    try defineBuiltin(a, &types, "bool", .{ .kind = .bool_ty, .name = "bool", .bits = 1, .unsigned = true });
    try defineBuiltin(a, &types, "u16", .{ .kind = .u16_ty, .name = "u16", .bits = 16, .unsigned = true });
    try defineBuiltin(a, &types, "i16", .{ .kind = .u16_ty, .name = "i16", .bits = 16, .unsigned = false });
    try defineBuiltin(a, &types, "u32", .{ .kind = .u32_ty, .name = "u32", .bits = 32, .unsigned = true });
    try defineBuiltin(a, &types, "u64", .{ .kind = .u64_ty, .name = "u64", .bits = 64, .unsigned = true });
    try defineBuiltin(a, &types, "i32", .{ .kind = .i32_ty, .name = "i32", .bits = 32, .unsigned = false });
    try defineBuiltin(a, &types, "i64", .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "f32", .{ .kind = .f32_ty, .name = "f32", .bits = 32, .unsigned = false });
    try defineBuiltin(a, &types, "f64", .{ .kind = .f64_ty, .name = "f64", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "error", .{ .kind = .i64_ty, .name = "error", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "usize", .{ .kind = .usize_ty, .name = "usize", .bits = 64, .unsigned = true });
    try defineBuiltin(a, &types, "isize", .{ .kind = .i64_ty, .name = "isize", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "str", .{ .kind = .string_ty, .name = "str" });
    try defineBuiltin(a, &types, "bytes", .{ .kind = .bytes_ty, .name = "bytes" });
    // Unicode scalar value (UTF-32 code point); used by std.text rune helpers.
    try defineBuiltin(a, &types, "rune", .{ .kind = .u32_ty, .name = "rune", .bits = 32, .unsigned = true });
    try defineBuiltin(a, &types, "void", .{ .kind = .void_ty, .name = "void" });
    // Opaque pool handle (i64-sized); with (Dispatcher) migrates stackful coros (gate 3).
    try defineBuiltin(a, &types, "Dispatcher", .{ .kind = .i64_ty, .name = "Dispatcher", .bits = 64, .unsigned = false });

    // Pass 1: register types, fn names, and import binds. Do not resolve expect/extern
    // signatures yet — flattened merge puts the root file before imported type aliases
    // (`c.char` in an expect fn would fail if resolved here).
    for (file.decls) |*d| {
        switch (d.*) {
            .type_alias => |ta| {
                const ti = try resolveTypeExpr(a, &types, ta.ty, &diags);
                try types.put(a, ta.name, ti);
            },
            .struct_decl => |*sd| {
                try structs.put(a, sd.name, sd);
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .struct_ty, .name = sd.name };
                try types.put(a, sd.name, ti);
                // Type params are placeholders; field types referencing them resolve when used.
                for (sd.type_params) |tp| {
                    if (!types.contains(tp)) {
                        const tpi = try a.create(TypeInfo);
                        tpi.* = .{ .kind = .named, .name = tp };
                        try types.put(a, tp, tpi);
                    }
                }
            },
            .enum_decl => |*ed| {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .enum_ty, .name = ed.name, .bits = 64, .unsigned = false, .enum_decl = ed };
                try types.put(a, ed.name, ti);
            },
            .import_decl => |imp| {
                const bind = importBindName(imp.path, imp.alias);
                try import_binds.put(a, bind, {});
            },
            .require_decl => {},
            .interface_fn => |*inf| {
                try iface_fns.put(a, inf.name, inf);
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .iface_fn, .name = inf.name };
                try types.put(a, inf.name, ti);
            },
            .interface_struct => |*ins| {
                try iface_structs.put(a, ins.name, ins);
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .iface_struct, .name = ins.name };
                try types.put(a, ins.name, ti);
                // Register iface method signatures so `alloc.resize(...)` returns the
                // declared tuple/optional type (needed for destructure binding).
                for (ins.methods) |ms| {
                    const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ ins.name, ms.name });
                    if (methods.contains(key)) continue;
                    const stub = try a.create(ast.MethodDecl);
                    stub.* = .{
                        .pointer_recv = ms.pointer_recv,
                        .type_name = ins.name,
                        .name = ms.name,
                        .params = ms.params,
                        .contexts = ms.contexts,
                        .ret = ms.ret,
                        .body = null,
                    };
                    try methods.put(a, key, stub);
                }
            },
            .fn_decl, .actual_fn => |*fd| {
                try fns.put(a, fd.name, fd);
            },
            .expect_fn => |ef| {
                const stub = try a.create(ast.FnDecl);
                stub.* = .{
                    .name = ef.name,
                    .params = ef.params,
                    .contexts = &[_]ast.ContextParam{},
                    .ret = ef.ret,
                    .body = null,
                    .exported = false,
                };
                try fns.put(a, ef.name, stub);
            },
            .method => |*md| {
                const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ md.type_name, md.name });
                try methods.put(a, key, md);
            },
            .extern_block => |eb| {
                for (eb.fns) |ef| {
                    // Treat as known fn name for calls
                    const stub = try a.create(ast.FnDecl);
                    stub.* = .{
                        .name = ef.name,
                        .params = ef.params,
                        .contexts = &[_]ast.ContextParam{},
                        .ret = ef.ret,
                        .body = null,
                        .exported = false,
                    };
                    try fns.put(a, ef.name, stub);
                }
            },
            .global_decl => {}, // bound into package scope below
        }
    }

    const prev_imports = active_imports;
    active_imports = &import_binds;
    defer active_imports = prev_imports;

    // Pass 2: resolve expect/extern signatures now that imported type aliases exist.
    for (file.decls) |*d| {
        switch (d.*) {
            .expect_fn => |ef| {
                for (ef.params) |p| _ = try resolveTypeExpr(a, &types, p.ty, &diags);
                if (ef.ret) |r| _ = try resolveTypeExpr(a, &types, r, &diags);
            },
            .extern_block => |eb| {
                for (eb.fns) |ef| {
                    for (ef.params) |p| _ = try resolveTypeExpr(a, &types, p.ty, &diags);
                    if (ef.ret) |r| _ = try resolveTypeExpr(a, &types, r, &diags);
                }
            },
            else => {},
        }
    }

    // Package-level val/var — visible as the parent scope of every function body.
    var globals_scope = Scope{ .parent = null, .vars = .{}, .narrowed = .{} };
    for (file.decls) |*d| {
        if (d.* != .global_decl) continue;
        const gd = d.global_decl;
        var t: *TypeInfo = undefined;
        if (gd.ty) |te| {
            t = try resolveTypeExpr(a, &types, te, &diags);
            if (gd.init) |init| {
                _ = try checkExpr(a, &types, &fns, &structs, &methods, &globals_scope, init, &diags);
            }
        } else if (gd.init) |init| {
            // Infer from init (literals, `&global`, null, …).
            t = try checkExpr(a, &types, &fns, &structs, &methods, &globals_scope, init, &diags);
        } else {
            t = types.get("i64").?;
        }
        try globals_scope.vars.put(a, gd.name, .{ .ty = t, .mutable = gd.mutable });
    }

    // check interface structs have methods only (already true by grammar)
    // check functions
    var has_main = false;
    var main_str: ?[]const u8 = null;

    var it = fns.iterator();
    while (it.next()) |e| {
        const fd = e.value_ptr.*;
        // Bind generic type params as opaque named types for the duration of this check.
        var bound: std.ArrayListUnmanaged([]const u8) = .{};
        for (fd.type_params) |tp| {
            if (!types.contains(tp)) {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .named, .name = tp };
                try types.put(a, tp, ti);
                try bound.append(a, tp);
            }
        }
        // contexts: with before return — already parsed; ensure types resolve
        for (fd.contexts) |ctx| {
            _ = try resolveTypeExpr(a, &types, ctx.ty, &diags);
        }
        if (fd.ret) |r| _ = try resolveTypeExpr(a, &types, r, &diags);
        for (fd.params) |p| _ = try resolveTypeExpr(a, &types, p.ty, &diags);

        if (std.mem.eql(u8, fd.name, "main")) {
            has_main = true;
            if (fd.body) |body| {
                main_str = findPrintString(body);
            }
        }
        // `@Annotation` bodies run only at expand/comptime; skip ordinary check (JOG-228).
        if (fd.body) |body| {
            if (!fd.is_annotation) {
                try checkFunctionBody(a, &types, &fns, &structs, &methods, fd.params, fd.contexts, body, &diags, &globals_scope);
            }
        }
        for (bound.items) |tp| _ = types.remove(tp);
    }

    // methods: structural interface satisfaction (basic)
    var mit = methods.iterator();
    while (mit.next()) |e| {
        const md = e.value_ptr.*;
        // Builtins (`str`, `i64`, …) may carry methods; only reject unknown names.
        if (structs.get(md.type_name) == null and types.get(md.type_name) == null) {
            try emitDiag(a, &diags, null, 1, 1, try std.fmt.allocPrint(a, "method on unknown type '{s}'", .{md.type_name}));
        }
        // Bind receiver/struct type params before resolving param/ret types.
        var bound_tp: std.ArrayListUnmanaged([]const u8) = .{};
        for (md.type_params) |tp| {
            if (!types.contains(tp)) {
                const tpi = try a.create(TypeInfo);
                tpi.* = .{ .kind = .named, .name = tp };
                try types.put(a, tp, tpi);
                try bound_tp.append(a, tp);
            }
        }
        if (structs.get(md.type_name)) |sd| {
            for (sd.type_params) |tp| {
                if (!types.contains(tp)) {
                    const tpi = try a.create(TypeInfo);
                    tpi.* = .{ .kind = .named, .name = tp };
                    try types.put(a, tp, tpi);
                    try bound_tp.append(a, tp);
                }
            }
        }
        for (md.contexts) |ctx| _ = try resolveTypeExpr(a, &types, ctx.ty, &diags);
        if (md.ret) |r| _ = try resolveTypeExpr(a, &types, r, &diags);
        for (md.params) |p| _ = try resolveTypeExpr(a, &types, p.ty, &diags);
        if (md.body) |body| {
            // synthesize self param for receiver
            var params_with_self: std.ArrayListUnmanaged(ast.Param) = .{};
            const self_ty = try a.create(ast.TypeExpr);
            if (md.pointer_recv) {
                const inner = try a.create(ast.TypeExpr);
                inner.* = .{ .name = md.type_name };
                self_ty.* = .{ .pointer = inner };
            } else {
                self_ty.* = .{ .name = md.type_name };
            }
            try params_with_self.append(a, .{ .name = "self", .ty = self_ty });
            for (md.params) |p| try params_with_self.append(a, p);
            try checkFunctionBody(a, &types, &fns, &structs, &methods, try params_with_self.toOwnedSlice(a), md.contexts, body, &diags, &globals_scope);
        }
        for (bound_tp.items) |tp| _ = types.remove(tp);
    }

    // structural: each interface struct method must exist on candidate structs (report only)
    var iit = iface_structs.iterator();
    while (iit.next()) |ie| {
        const iface = ie.value_ptr.*;
        var sit = structs.iterator();
        while (sit.next()) |se| {
            const sd = se.value_ptr.*;
            for (iface.methods) |ms| {
                const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ sd.name, ms.name });
                // Structural Go-style satisfaction is checked at use sites; ensure method keys resolve.
                _ = methods.get(key);
            }
        }
    }

    result.ok = diags.items.len == 0;
    result.diagnostics = try diags.toOwnedSlice(a);
    result.has_main = has_main;
    result.main_prints_string = main_str;
    return result;
}

/// Resolve `Name` or `pkg.Name` (import-qualified). Flattened import merge registers
/// the short name; Grammar qualifiedName keeps the package prefix in the AST.
fn lookupTypeName(types: *std.StringHashMapUnmanaged(*TypeInfo), n: []const u8) ?*TypeInfo {
    if (types.get(n)) |t| return t;
    if (std.mem.lastIndexOfScalar(u8, n, '.')) |dot| {
        return types.get(n[dot + 1 ..]);
    }
    return null;
}

/// Look up a field on `sd`, including fields promoted through embeds.
fn findFieldDecl(structs: *std.StringHashMapUnmanaged(*ast.StructDecl), sd: *ast.StructDecl, name: []const u8) ?*ast.TypeExpr {
    for (sd.fields) |fld| {
        if (std.mem.eql(u8, fld.name, name)) return fld.ty;
    }
    var found: ?*ast.TypeExpr = null;
    for (sd.fields) |fld| {
        if (!fld.is_embedded) continue;
        const etn = switch (fld.ty.*) {
            .name => |n| n,
            .named_app => |na| na.name,
            else => continue,
        };
        const leaf = if (std.mem.lastIndexOfScalar(u8, etn, '.')) |dot| etn[dot + 1 ..] else etn;
        const esd = structs.get(leaf) orelse continue;
        if (findFieldDecl(structs, esd, name)) |ty| {
            if (found != null) return null; // ambiguous
            found = ty;
        }
    }
    return found;
}

/// Private fields are only readable/writable as `self.<name>` inside methods.
/// Struct literals may still initialize them (construction).
fn checkPrivateFieldAccess(
    a: std.mem.Allocator,
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    diags: *std.ArrayListUnmanaged(Diagnostic),
    base: *const ast.Expr,
    base_t: *TypeInfo,
    field_name: []const u8,
) !void {
    const sd_name = blk: {
        if (base_t.kind == .struct_ty) break :blk base_t.name;
        if (base_t.kind == .pointer) {
            if (base_t.pointee) |po| {
                if (po.kind == .struct_ty) break :blk po.name;
            }
        }
        return;
    };
    const sd = structs.get(sd_name) orelse return;
    var is_private = false;
    for (sd.fields) |fld| {
        if (std.mem.eql(u8, fld.name, field_name)) {
            is_private = fld.is_private;
            break;
        }
    }
    if (!is_private) return;
    const allowed = base.* == .ident and std.mem.eql(u8, base.ident, "self");
    if (!allowed) {
        try emitDiag(
            a,
            diags,
            base,
            1,
            1,
            try std.fmt.allocPrint(a, "private field '{s}' is only accessible as self.{s}", .{ field_name, field_name }),
        );
    }
}

fn defineBuiltin(a: std.mem.Allocator, types: *std.StringHashMapUnmanaged(*TypeInfo), name: []const u8, info: TypeInfo) !void {
    const ti = try a.create(TypeInfo);
    ti.* = info;
    try types.put(a, name, ti);
}

fn resolveTypeExpr(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    te: *ast.TypeExpr,
    diags: *std.ArrayListUnmanaged(Diagnostic),
) CheckError!*TypeInfo {
    switch (te.*) {
        .name => |n| {
            if (lookupTypeName(types, n)) |t| return t;
            // Pointer TypeInfo uses the display name "ptr"; never a source type spelling.
            if (std.mem.eql(u8, n, "ptr") or std.mem.eql(u8, n, "const_ptr")) {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .name = n, .pointee = types.get("byte") };
                return ti;
            }
            // Lambda param placeholder — inferred from the expected function type.
            if (std.mem.eql(u8, n, "_")) {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .named, .name = "_" };
                return ti;
            }
            try emitDiag(a, diags, null, 1, 1, try std.fmt.allocPrint(a, "unknown type '{s}'", .{n}));
            const bad = try a.create(TypeInfo);
            bad.* = .{ .kind = .named, .name = n };
            return bad;
        },
        .pointer => |inner| {
            const p = try resolveTypeExpr(a, types, inner, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .pointer, .pointee = p, .name = "ptr" };
            return ti;
        },
        .pointer_const => |inner| {
            const p = try resolveTypeExpr(a, types, inner, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .pointer, .pointee = p, .name = "const_ptr" };
            return ti;
        },
        .optional => |inner| {
            const p = try resolveTypeExpr(a, types, inner, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .optional, .pointee = p, .name = "opt" };
            return ti;
        },
        .array => |arr| {
            const elem = try resolveTypeExpr(a, types, arr.elem, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .array_ty,
                .name = try std.fmt.allocPrint(a, "[{d}]{s}", .{ arr.len, elem.name }),
                .pointee = elem,
                .array_len = arr.len,
            };
            return ti;
        },
        .named_app => |na| {
            // Surface accept: resolve args; treat as named struct of mangled form.
            var buf: std.ArrayListUnmanaged(u8) = .{};
            try buf.appendSlice(a, na.name);
            for (na.args) |arg| {
                const at = try resolveTypeExpr(a, types, arg, diags);
                try buf.appendSlice(a, "__");
                try buf.appendSlice(a, at.name);
            }
            const mangled = try buf.toOwnedSlice(a);
            if (types.get(mangled)) |t| return t;
            if (lookupTypeName(types, na.name)) |t| return t;
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .struct_ty, .name = mangled };
            try types.put(a, mangled, ti);
            return ti;
        },
        .tuple => |elems| {
            var elem_tys: std.ArrayListUnmanaged(*TypeInfo) = .{};
            for (elems) |el| try elem_tys.append(a, try resolveTypeExpr(a, types, el, diags));
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .struct_ty,
                .name = try std.fmt.allocPrint(a, "__Tup{d}", .{elems.len}),
                .tuple_elems = try elem_tys.toOwnedSlice(a),
            };
            return ti;
        },
        .slice_view => |sv| {
            const elem = try resolveTypeExpr(a, types, sv.elem, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .struct_ty,
                .name = try std.fmt.allocPrint(a, "View_{s}", .{elem.name}),
                .pointee = elem,
            };
            return ti;
        },
        .iface_fn => {
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .iface_fn, .name = "iface_fn" };
            return ti;
        },
        .bad => {
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .void_ty };
            return ti;
        },
    }
}

const Scope = struct {
    parent: ?*Scope,
    vars: std.StringHashMapUnmanaged(VarInfo),
    /// names narrowed to non-null in this scope
    narrowed: std.StringHashMapUnmanaged(void),
};

const VarInfo = struct {
    ty: *TypeInfo,
    mutable: bool,
};

fn checkFunctionBody(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    methods: *std.StringHashMapUnmanaged(*ast.MethodDecl),
    params: []ast.Param,
    contexts: []ast.ContextParam,
    body: *ast.Block,
    diags: *std.ArrayListUnmanaged(Diagnostic),
    package_scope: ?*Scope,
) CheckError!void {
    var scope = Scope{
        .parent = package_scope,
        .vars = .{},
        .narrowed = .{},
    };
    for (params) |p| {
        const ty = try resolveTypeExpr(a, types, p.ty, diags);
        try scope.vars.put(a, p.name, .{ .ty = ty, .mutable = false });
    }
    for (contexts) |ctx| {
        const ty = try resolveTypeExpr(a, types, ctx.ty, diags);
        if (ctx.name) |n| {
            try scope.vars.put(a, n, .{ .ty = ty, .mutable = false });
        }
    }
    try checkBlock(a, types, fns, structs, methods, body, diags, &scope);
}

fn checkBlock(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    methods: *std.StringHashMapUnmanaged(*ast.MethodDecl),
    block: *ast.Block,
    diags: *std.ArrayListUnmanaged(Diagnostic),
    parent_scope: ?*Scope,
) CheckError!void {
    _ = try checkBlockValue(a, types, fns, structs, methods, block, diags, parent_scope);
}

/// Typecheck a block; return the type of its last expression statement (block value), or void.
fn checkBlockValue(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    methods: *std.StringHashMapUnmanaged(*ast.MethodDecl),
    block: *ast.Block,
    diags: *std.ArrayListUnmanaged(Diagnostic),
    parent_scope: ?*Scope,
) CheckError!*TypeInfo {
    var scope = Scope{
        .parent = parent_scope,
        .vars = .{},
        .narrowed = .{},
    };
    if (block.stmts.len == 0) return try voidType(a);
    const last_i = block.stmts.len - 1;
    for (block.stmts[0..last_i]) |stmt| {
        try checkStmt(a, types, fns, structs, methods, &scope, stmt, diags);
    }
    const last = block.stmts[last_i];
    if (last == .expr) {
        return try checkExpr(a, types, fns, structs, methods, &scope, last.expr, diags);
    }
    try checkStmt(a, types, fns, structs, methods, &scope, last, diags);
    return try voidType(a);
}

fn checkStmt(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    methods: *std.StringHashMapUnmanaged(*ast.MethodDecl),
    scope: *Scope,
    stmt: ast.Stmt,
    diags: *std.ArrayListUnmanaged(Diagnostic),
) CheckError!void {
    switch (stmt) {
        .var_decl => |vd| {
            var ty: ?*TypeInfo = null;
            if (vd.ty) |t| ty = try resolveTypeExpr(a, types, t, diags);
            if (vd.init) |init| {
                // `.Empty` / `.Circle(r: …)` — fill enum type from the annotation.
                if (init.* == .enum_lit and init.enum_lit.type_name == null) {
                    if (ty) |t| {
                        if (t.kind == .enum_ty) init.enum_lit.type_name = t.name;
                    }
                }
                const it = try checkExpr(a, types, fns, structs, methods, scope, init, diags);
                if (ty) |t| {
                    if (!canAssign(it, t)) {
                        try emitDiag(a, diags, init, vd.line, vd.col, try std.fmt.allocPrint(a, "cannot assign {s} to {s}", .{ it.name, t.name }));
                    }
                } else {
                    ty = it;
                }
            }
            if (ty) |t| {
                try scope.vars.put(a, vd.name, .{ .ty = t, .mutable = vd.mutable });
            }
        },
        .destructure => |ds| {
            const it = try checkExpr(a, types, fns, structs, methods, scope, ds.init, diags);
            if (it.tuple_elems.len == ds.names.len and it.tuple_elems.len > 0) {
                for (ds.names, it.tuple_elems) |n, et| {
                    try scope.vars.put(a, n, .{ .ty = et, .mutable = ds.mutable });
                }
            } else {
                // Untyped / non-tuple RHS — soft ints until richer inference lands.
                const i64t = types.get("i64").?;
                for (ds.names) |n| {
                    try scope.vars.put(a, n, .{ .ty = i64t, .mutable = ds.mutable });
                }
            }
        },
        .for_in_stmt => |fs| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, fs.iter, diags);
            const i64t = types.get("i64").?;
            try scope.vars.put(a, fs.name, .{ .ty = i64t, .mutable = false });
            try checkBlock(a, types, fns, structs, methods, fs.body, diags, scope);
        },
        .errdefer_stmt => |ds| switch (ds) {
            .expr => |ex| _ = try checkExpr(a, types, fns, structs, methods, scope, ex, diags),
            .block => |b| try checkBlock(a, types, fns, structs, methods, b, diags, scope),
        },
        .assign => |as| {
            if (as.index) |ix| {
                _ = try checkExpr(a, types, fns, structs, methods, scope, ix.base, diags);
                _ = try checkExpr(a, types, fns, structs, methods, scope, ix.index, diags);
                _ = try checkExpr(a, types, fns, structs, methods, scope, as.value, diags);
            } else if (as.field) |fa| {
                const base_t = try checkExpr(a, types, fns, structs, methods, scope, fa.base, diags);
                try checkPrivateFieldAccess(a, structs, diags, fa.base, base_t, fa.name);
                _ = try checkExpr(a, types, fns, structs, methods, scope, as.value, diags);
            } else if (as.deref) |ptr_e| {
                _ = try checkExpr(a, types, fns, structs, methods, scope, ptr_e, diags);
                _ = try checkExpr(a, types, fns, structs, methods, scope, as.value, diags);
            } else {
                const v = lookupVar(scope, as.name) orelse {
                    try emitDiag(a, diags, as.value, 1, 1, try std.fmt.allocPrint(a, "unknown name '{s}'", .{as.name}));
                    return;
                };
                const rt = try checkExpr(a, types, fns, structs, methods, scope, as.value, diags);
                if (!canAssign(rt, v.ty)) {
                    try emitDiag(a, diags, as.value, 1, 1, "assignment type mismatch");
                }
            }
        },
        .expr => |e| _ = try checkExpr(a, types, fns, structs, methods, scope, e, diags),
        .return_stmt => |re| {
            if (re) |e| _ = try checkExpr(a, types, fns, structs, methods, scope, e, diags);
        },
        .if_stmt => |ifs| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, ifs.cond, diags);
            // flow-sensitive: if (x != null) narrow x
            var then_scope = Scope{ .parent = scope, .vars = .{}, .narrowed = .{} };
            if (nullCheckIdent(ifs.cond)) |name| {
                try then_scope.narrowed.put(a, name, {});
            }
            try checkBlock(a, types, fns, structs, methods, ifs.then_block, diags, &then_scope);
            if (ifs.else_block) |eb| {
                try checkBlock(a, types, fns, structs, methods, eb, diags, scope);
            }
        },
        .while_stmt => |ws| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, ws.cond, diags);
            try checkBlock(a, types, fns, structs, methods, ws.body, diags, scope);
        },
        .break_stmt, .continue_stmt => {},
        .with_stmt => |ws| {
            for (ws.values) |v| _ = try checkExpr(a, types, fns, structs, methods, scope, v, diags);
            try checkBlock(a, types, fns, structs, methods, ws.body, diags, scope);
        },
        .defer_stmt => |ds| {
            switch (ds) {
                .expr => |ex| _ = try checkExpr(a, types, fns, structs, methods, scope, ex, diags),
                .block => |b| try checkBlock(a, types, fns, structs, methods, b, diags, scope),
            }
        },
    }
}

fn nullCheckIdent(e: *ast.Expr) ?[]const u8 {
    if (e.* != .binary) return null;
    const b = e.binary;
    if (b.op != .neq) return null;
    if (b.left.* == .ident and b.right.* == .null_lit) return b.left.ident;
    if (b.right.* == .ident and b.left.* == .null_lit) return b.right.ident;
    return null;
}

fn lookupVar(scope: *Scope, name: []const u8) ?VarInfo {
    var s: ?*Scope = scope;
    while (s) |cur| {
        if (cur.vars.get(name)) |v| return v;
        s = cur.parent;
    }
    return null;
}

fn isNarrowed(scope: *Scope, name: []const u8) bool {
    var s: ?*Scope = scope;
    while (s) |cur| {
        if (cur.narrowed.contains(name)) return true;
        s = cur.parent;
    }
    return false;
}

fn checkExpr(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    methods: *std.StringHashMapUnmanaged(*ast.MethodDecl),
    scope: *Scope,
    e: *ast.Expr,
    diags: *std.ArrayListUnmanaged(Diagnostic),
) CheckError!*TypeInfo {
    switch (e.*) {
        .ident => |name| {
            if (lookupVar(scope, name)) |v| {
                if (v.ty.kind == .optional and isNarrowed(scope, name)) {
                    return v.ty.pointee orelse v.ty;
                }
                return v.ty;
            }
            // JOG-242 lang-item error sentinels (pointer-shaped).
            if (std.mem.eql(u8, name, "ErrCancelled") or std.mem.eql(u8, name, "ErrClosed")) {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .name = name, .pointee = try voidType(a) };
                return ti;
            }
            if (fns.contains(name)) {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .iface_fn, .name = name };
                return ti;
            }
            if (types.get(name)) |t| return t;
            try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "unknown name '{s}'", .{name}));
            return try voidType(a);
        },
        .int => {
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false };
            return ti;
        },
        .float => {
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .f64_ty, .name = "f64", .bits = 64, .unsigned = false };
            return ti;
        },
        .string => {
            return types.get("str").?;
        },
        .null_lit => {
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .optional, .name = "null", .pointee = try voidType(a) };
            return ti;
        },
        .binary => |b| {
            const l = try checkExpr(a, types, fns, structs, methods, scope, b.left, diags);
            const r = try checkExpr(a, types, fns, structs, methods, scope, b.right, diags);
            if (b.op == .eq or b.op == .neq) {
                // Types.md: comparable enums only — reject struct/slice/pointer/option payloads.
                if (l.kind == .enum_ty) {
                    if (!enumIsComparable(types, l)) {
                        try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "operator '==' does not apply to {s}", .{l.name}));
                    }
                } else if (r.kind == .enum_ty) {
                    if (!enumIsComparable(types, r)) {
                        try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "operator '==' does not apply to {s}", .{r.name}));
                    }
                }
                return types.get("bool").?;
            }
            if (b.op == .lt or b.op == .gt or b.op == .lte or b.op == .gte or b.op == .and_op or b.op == .or_op)
                return types.get("bool").?;
            // arithmetic / bitwise: allow widen
            return l;
        },
        .unary => |u| {
            const t = try checkExpr(a, types, fns, structs, methods, scope, u.expr, diags);
            if (u.op == .addr) {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .pointee = t, .name = "ptr" };
                return ti;
            }
            if (u.op == .not) return types.get("bool").?;
            return t;
        },
        .call => |c| {
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "print")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                for (c.with_args) |w| _ = try checkExpr(a, types, fns, structs, methods, scope, w, diags);
                return try voidType(a);
            }
            // Builtin bytes(): empty mutable byte slice header.
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "bytes")) {
                if (c.args.len != 0) {
                    try emitDiag(a, diags, e, 1, 1, "bytes() takes no arguments");
                }
                return types.get("bytes").?;
            }
            // Zero-value struct constructor: `StrBuilder()` when Name is a struct type.
            if (c.callee.* == .ident and c.args.len == 0 and c.type_args.len == 0) {
                if (structs.get(c.callee.ident)) |sd| {
                    _ = sd;
                    if (types.get(c.callee.ident)) |t| return t;
                    const ti = try a.create(TypeInfo);
                    ti.* = .{ .kind = .struct_ty, .name = c.callee.ident };
                    return ti;
                }
            }
            // Builtins size(T) / align(T): type form via type_args (Slot<K,V>) or value form.
            if (c.callee.* == .ident and (std.mem.eql(u8, c.callee.ident, "size") or std.mem.eql(u8, c.callee.ident, "align"))) {
                if (c.type_args.len == 1) {
                    // Type form — nothing to check as a value expr.
                } else if (c.args.len == 1) {
                    _ = try checkExpr(a, types, fns, structs, methods, scope, c.args[0], diags);
                } else {
                    try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "{s} takes one type argument", .{c.callee.ident}));
                }
                return types.get("usize").?;
            }
            // JOG-229: typeInfo(T) → *byte (TypeInfo address); typeName(ti) → str.
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "typeInfo")) {
                if (c.type_args.len == 1) {
                    // Type form.
                } else if (c.args.len == 1) {
                    _ = try checkExpr(a, types, fns, structs, methods, scope, c.args[0], diags);
                } else {
                    try emitDiag(a, diags, e, 1, 1, "typeInfo takes one type argument");
                }
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .name = "ptr", .pointee = types.get("byte") };
                return ti;
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "typeName")) {
                if (c.args.len != 1) {
                    try emitDiag(a, diags, e, 1, 1, "typeName takes one argument");
                } else {
                    _ = try checkExpr(a, types, fns, structs, methods, scope, c.args[0], diags);
                }
                return types.get("str").?;
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "alloc")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .name = "ptr", .pointee = types.get("byte") };
                return ti;
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "alloc_checkpoint")) {
                return types.get("i64").?;
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "alloc_reset")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
            }
            if (c.callee.* == .ident and (std.mem.eql(u8, c.callee.ident, "f32_bits") or
                std.mem.eql(u8, c.callee.ident, "jog_f32_bits")))
            {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return types.get("u32") orelse types.get("i64").?;
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "f32_from_bits")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return types.get("f64").?;
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "spawn")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "jog_host_run_loop")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "go")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
            }
            if (c.callee.* == .ident and (std.mem.eql(u8, c.callee.ident, "yield") or
                std.mem.eql(u8, c.callee.ident, "coro_drain") or
                std.mem.eql(u8, c.callee.ident, "coro_park") or
                std.mem.eql(u8, c.callee.ident, "print_tid") or
                std.mem.eql(u8, c.callee.ident, "sleep_ms") or
                std.mem.eql(u8, c.callee.ident, "cancel")))
            {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
            }
            // JOG-242: select cases + cooperative sleep.
            if (c.callee.* == .ident and (std.mem.eql(u8, c.callee.ident, "recv") or
                std.mem.eql(u8, c.callee.ident, "send") or
                std.mem.eql(u8, c.callee.ident, "timeout") or
                std.mem.eql(u8, c.callee.ident, "default") or
                std.mem.eql(u8, c.callee.ident, "sleep")))
            {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
            }
            // Chan<T>.init / ch.send / task.join — accept; lower special-cases.
            if (c.callee.* == .field) {
                const fname = c.callee.field.name;
                if (c.callee.field.base.* == .type_value or
                    std.mem.eql(u8, fname, "send") or std.mem.eql(u8, fname, "recv") or
                    std.mem.eql(u8, fname, "close") or std.mem.eql(u8, fname, "free") or
                    std.mem.eql(u8, fname, "join") or std.mem.eql(u8, fname, "init") or
                    std.mem.eql(u8, fname, "cancel"))
                {
                    _ = try checkExpr(a, types, fns, structs, methods, scope, c.callee.field.base, diags);
                    for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                    return try voidType(a);
                }
            }
            if (c.callee.* == .ident and (std.mem.eql(u8, c.callee.ident, "dispatcher_single") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_pool") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_new") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_get") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_io")))
            {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .i64_ty, .name = "Dispatcher", .bits = 64, .unsigned = false };
                return ti;
            }
            if (c.callee.* == .ident and (std.mem.eql(u8, c.callee.ident, "coro_alive") or
                std.mem.eql(u8, c.callee.ident, "coro_id") or
                std.mem.eql(u8, c.callee.ident, "worker_id") or
                std.mem.eql(u8, c.callee.ident, "gettid")))
            {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false };
                return ti;
            }
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "coro_unpark")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
            }
            if (c.callee.* == .ident) {
                if (fns.get(c.callee.ident)) |fd| {
                    // Flattened import merge can leave one same-named fn in `fns`
                    // (`mem.slice` vs `text.slice`). Skip arity expand when the
                    // looked-up decl clearly cannot match this call.
                    if (callArgsFitParams(fd.params, c.args, c.arg_names)) {
                        try checkArgsAgainstParams(a, types, fns, structs, methods, scope, fd.params, c.args, c.arg_names, diags, e);
                    } else {
                        for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                    }
                    for (c.with_args) |w| _ = try checkExpr(a, types, fns, structs, methods, scope, w, diags);
                    for (c.type_args) |ta| _ = try resolveTypeExpr(a, types, ta, diags);
                    if (fd.type_params.len > 0) {
                        // Explicit type args, or infer from first arg (int → i64).
                        var targ_buf: [4]*ast.TypeExpr = undefined;
                        var nta: usize = c.type_args.len;
                        if (nta == 0 and c.args.len > 0 and fd.type_params.len == 1) {
                            const te = try a.create(ast.TypeExpr);
                            const at = try checkExpr(a, types, fns, structs, methods, scope, c.args[0], diags);
                            te.* = .{ .name = if (at.name.len > 0) at.name else "i64" };
                            targ_buf[0] = te;
                            nta = 1;
                        }
                        if (nta == fd.type_params.len) {
                            var saved: std.ArrayListUnmanaged(struct { []const u8, ?*TypeInfo }) = .{};
                            var i: usize = 0;
                            while (i < nta) : (i += 1) {
                                const ta = if (c.type_args.len > 0) c.type_args[i] else targ_buf[i];
                                const prev = types.get(fd.type_params[i]);
                                try saved.append(a, .{ fd.type_params[i], prev });
                                const concrete = try resolveTypeExpr(a, types, ta, diags);
                                try types.put(a, fd.type_params[i], concrete);
                            }
                            const result = if (fd.ret) |r| try resolveTypeExpr(a, types, r, diags) else try voidType(a);
                            for (saved.items) |s| {
                                if (s[1]) |p| try types.put(a, s[0], p) else _ = types.remove(s[0]);
                            }
                            return result;
                        }
                    }
                    if (fd.ret) |r| return try resolveTypeExpr(a, types, r, diags);
                    return try voidType(a);
                }
            }
            if (c.callee.* == .field) {
                // `pkg.fn(...)` after import — flattened merge; base is alias, not a value.
                if (c.callee.field.base.* == .ident) {
                    const base_name = c.callee.field.base.ident;
                    if (lookupVar(scope, base_name) == null) {
                        if (fns.get(c.callee.field.name)) |fd| {
                            if (callArgsFitParams(fd.params, c.args, c.arg_names)) {
                                try checkArgsAgainstParams(a, types, fns, structs, methods, scope, fd.params, c.args, c.arg_names, diags, e);
                            } else {
                                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                            }
                            for (c.with_args) |w| _ = try checkExpr(a, types, fns, structs, methods, scope, w, diags);
                            if (fd.ret) |r| return try resolveTypeExpr(a, types, r, diags);
                            return try voidType(a);
                        }
                    }
                }
                const base_t = try checkExpr(a, types, fns, structs, methods, scope, c.callee.field.base, diags);
                for (c.with_args) |w| _ = try checkExpr(a, types, fns, structs, methods, scope, w, diags);
                const type_name = blk: {
                    if (base_t.kind == .struct_ty) break :blk base_t.name;
                    if (base_t.kind == .iface_struct) break :blk base_t.name;
                    if (base_t.kind == .pointer) {
                        if (base_t.pointee) |po| {
                            if (po.kind == .struct_ty or po.kind == .iface_struct) break :blk po.name;
                        }
                    }
                    break :blk "";
                };
                if (type_name.len > 0) {
                    const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ type_name, c.callee.field.name });
                    if (methods.get(key)) |md| {
                        // Bind trailing method type params from call site (`xs.map<i64>(…)` → R).
                        var saved_tp: std.ArrayListUnmanaged(struct { []const u8, ?*TypeInfo }) = .{};
                        if (c.type_args.len > 0 and md.type_params.len >= c.type_args.len) {
                            const off = md.type_params.len - c.type_args.len;
                            for (c.type_args, 0..) |ta, i| {
                                const tp = md.type_params[off + i];
                                const prev = types.get(tp);
                                try saved_tp.append(a, .{ tp, prev });
                                const concrete = try resolveTypeExpr(a, types, ta, diags);
                                try types.put(a, tp, concrete);
                            }
                        }
                        try checkArgsAgainstParams(a, types, fns, structs, methods, scope, md.params, c.args, c.arg_names, diags, e);
                        const result = if (md.ret) |r|
                            try resolveTypeExpr(a, types, r, diags)
                        else
                            try voidType(a);
                        for (saved_tp.items) |s| {
                            if (s[1]) |p| try types.put(a, s[0], p) else _ = types.remove(s[0]);
                        }
                        return result;
                    }
                }
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .name = "ptr", .pointee = types.get("byte") };
                return ti;
            }
            _ = try checkExpr(a, types, fns, structs, methods, scope, c.callee, diags);
            for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
            for (c.with_args) |w| _ = try checkExpr(a, types, fns, structs, methods, scope, w, diags);
            return try voidType(a);
        },
        .field => |f| {
            // `pkg.Name` / `alias.Name` after import — flattened merge; base is a bind, not a value.
            if (f.base.* == .ident) {
                const base_name = f.base.ident;
                if (lookupVar(scope, base_name) == null and isImportBind(base_name)) {
                    if (lookupVar(scope, f.name)) |v| {
                        if (v.ty.kind == .optional and isNarrowed(scope, f.name)) {
                            return v.ty.pointee orelse v.ty;
                        }
                        return v.ty;
                    }
                    if (fns.contains(f.name)) {
                        const ti = try a.create(TypeInfo);
                        ti.* = .{ .kind = .iface_fn, .name = f.name };
                        return ti;
                    }
                    if (lookupTypeName(types, f.name)) |t| return t;
                    try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "unknown name '{s}.{s}'", .{ base_name, f.name }));
                    return try voidType(a);
                }
            }
            const base = try checkExpr(a, types, fns, structs, methods, scope, f.base, diags);
            if (base.kind == .enum_ty) return base;
            // Slice headers: str / bytes expose .ptr and .len (usize), not i64.
            if (base.kind == .string_ty or base.kind == .bytes_ty) {
                if (std.mem.eql(u8, f.name, "len")) {
                    return types.get("usize").?;
                }
                if (std.mem.eql(u8, f.name, "ptr")) {
                    const ti = try a.create(TypeInfo);
                    ti.* = .{ .kind = .pointer, .name = "ptr", .pointee = types.get("u8") };
                    return ti;
                }
            }
            if (base.kind == .struct_ty) {
                if (structs.get(base.name)) |sd| {
                    if (findFieldDecl(structs, sd, f.name)) |fty| {
                        try checkPrivateFieldAccess(a, structs, diags, f.base, base, f.name);
                        return try resolveTypeExpr(a, types, fty, diags);
                    }
                }
            }
            if (base.kind == .pointer) {
                if (base.pointee) |po| {
                    if (po.kind == .struct_ty) {
                        if (structs.get(po.name)) |sd| {
                            if (findFieldDecl(structs, sd, f.name)) |fty| {
                                try checkPrivateFieldAccess(a, structs, diags, f.base, base, f.name);
                                return try resolveTypeExpr(a, types, fty, diags);
                            }
                        }
                    }
                }
            }
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false };
            return ti;
        },
        .when_expr => |w| {
            const scrut_t = try checkExpr(a, types, fns, structs, methods, scope, w.scrutinee, diags);
            var result: ?*TypeInfo = null;
            for (w.arms) |arm| {
                // Bind payload locals for this arm (positional).
                var arm_scope = Scope{
                    .parent = scope,
                    .vars = .{},
                    .narrowed = .{},
                };
                if (scrut_t.kind == .enum_ty) {
                    if (scrut_t.enum_decl) |ed| {
                        for (ed.variants) |v| {
                            if (!std.mem.eql(u8, v.name, arm.pattern)) continue;
                            const nbind = @min(arm.bindings.len, v.fields.len);
                            var bi: usize = 0;
                            while (bi < nbind) : (bi += 1) {
                                const bname = arm.bindings[bi];
                                if (std.mem.eql(u8, bname, "_")) continue;
                                const fty = try resolveTypeExpr(a, types, v.fields[bi].ty, diags);
                                try arm_scope.vars.put(a, bname, .{ .ty = fty, .mutable = false });
                            }
                            break;
                        }
                    }
                }
                const bt = try checkExpr(a, types, fns, structs, methods, &arm_scope, arm.body, diags);
                if (result == null) result = bt;
            }
            if (w.else_body) |eb| {
                const bt = try checkExpr(a, types, fns, structs, methods, scope, eb, diags);
                if (result == null) result = bt;
            }
            return result orelse try voidType(a);
        },
        .if_expr => |ifs| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, ifs.cond, diags);
            // Value `if` requires else; branch value is the last expression in each block.
            var then_scope = Scope{ .parent = scope, .vars = .{}, .narrowed = .{} };
            if (nullCheckIdent(ifs.cond)) |name| {
                try then_scope.narrowed.put(a, name, {});
            }
            const then_t = try checkBlockValue(a, types, fns, structs, methods, ifs.then_block, diags, &then_scope);
            if (ifs.else_block) |eb| {
                const else_t = try checkBlockValue(a, types, fns, structs, methods, eb, diags, scope);
                if (!canAssign(then_t, else_t) and !canAssign(else_t, then_t)) {
                    try emitDiag(a, diags, e, 1, 1, "if expression branches have different types");
                }
                return then_t;
            }
            try emitDiag(a, diags, e, 1, 1, "if expression requires else");
            return then_t;
        },
        .index => |ix| {
            const base = try checkExpr(a, types, fns, structs, methods, scope, ix.base, diags);
            _ = try checkExpr(a, types, fns, structs, methods, scope, ix.index, diags);
            if (base.kind == .array_ty) {
                return base.pointee orelse {
                    const ti = try a.create(TypeInfo);
                    ti.* = .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false };
                    return ti;
                };
            }
            if (base.kind == .pointer) {
                return base.pointee orelse {
                    const ti = try a.create(TypeInfo);
                    ti.* = .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false };
                    return ti;
                };
            }
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false };
            return ti;
        },
        .slice => |sl| {
            const base = try checkExpr(a, types, fns, structs, methods, scope, sl.base, diags);
            if (sl.start) |s| _ = try checkExpr(a, types, fns, structs, methods, scope, s, diags);
            if (sl.end) |en| _ = try checkExpr(a, types, fns, structs, methods, scope, en, diags);
            const elem = blk: {
                if (base.kind == .array_ty) break :blk base.pointee;
                if (base.kind == .struct_ty and base.pointee != null) break :blk base.pointee;
                if (base.kind == .bytes_ty) break :blk types.get("byte");
                break :blk types.get("i64");
            };
            const el = elem orelse types.get("i64").?;
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .struct_ty,
                .name = try std.fmt.allocPrint(a, "View_{s}", .{el.name}),
                .pointee = el,
            };
            return ti;
        },
        .cast => |c| {
            const from = try checkExpr(a, types, fns, structs, methods, scope, c.expr, diags);
            const to = try resolveTypeExpr(a, types, c.ty, diags);
            if (!castAllowed(from, to, true)) {
                try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "cannot cast {s} to {s}", .{ from.name, to.name }));
            }
            // narrowing int constant fold check
            if (c.expr.* == .int and to.bits > 0 and to.bits < 64) {
                const max: i128 = (@as(i128, 1) << @intCast(to.bits)) - 1;
                if (c.expr.int < 0 or c.expr.int > max) {
                    try emitDiag(a, diags, e, 1, 1, "integer constant does not fit in cast target");
                }
            }
            return to;
        },
        .optional_call => |oc| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, oc.base, diags);
            for (oc.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .optional, .name = "opt_result", .pointee = try voidType(a) };
            return ti;
        },
        .panic_call => |p| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, p.message, diags);
            return try voidType(a);
        },
        .struct_lit => |sl| {
            // `connect(host: "db")` parses as struct_lit; if the name is a function, treat as named-arg call.
            if (fns.get(sl.type_name)) |fd| {
                var args = try a.alloc(*ast.Expr, sl.fields.len);
                var names = try a.alloc(?[]const u8, sl.fields.len);
                for (sl.fields, 0..) |f, i| {
                    args[i] = f.value;
                    names[i] = f.name;
                }
                try checkArgsAgainstParams(a, types, fns, structs, methods, scope, fd.params, args, names, diags, e);
                if (fd.ret) |r| return try resolveTypeExpr(a, types, r, diags);
                return try voidType(a);
            }
            for (sl.fields) |f| _ = try checkExpr(a, types, fns, structs, methods, scope, f.value, diags);
            if (sl.type_args.len > 0) {
                const te = try a.create(ast.TypeExpr);
                te.* = .{ .named_app = .{ .name = sl.type_name, .args = sl.type_args } };
                return try resolveTypeExpr(a, types, te, diags);
            }
            if (types.get(sl.type_name)) |t| return t;
            try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "unknown struct '{s}'", .{sl.type_name}));
            return try voidType(a);
        },
        .enum_lit => |*el| {
            // Resolve missing type from a unique matching enum variant, if possible.
            if (el.type_name == null) {
                var found: ?[]const u8 = null;
                var it = types.iterator();
                while (it.next()) |entry| {
                    const ti = entry.value_ptr.*;
                    if (ti.kind != .enum_ty) continue;
                    if (ti.enum_decl) |ed| {
                        for (ed.variants) |v| {
                            if (std.mem.eql(u8, v.name, el.variant)) {
                                if (found != null and !std.mem.eql(u8, found.?, ti.name)) {
                                    found = null;
                                    break;
                                }
                                found = ti.name;
                            }
                        }
                    }
                }
                if (found) |tn| el.type_name = tn;
            }
            const tname = el.type_name orelse {
                try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "enum variant '.{s}' needs a known type", .{el.variant}));
                return try voidType(a);
            };
            const et = types.get(tname) orelse {
                try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "unknown enum '{s}'", .{tname}));
                return try voidType(a);
            };
            if (et.kind != .enum_ty) {
                try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "'{s}' is not an enum", .{tname}));
                return try voidType(a);
            }
            var matched = false;
            if (et.enum_decl) |ed| {
                for (ed.variants) |v| {
                    if (!std.mem.eql(u8, v.name, el.variant)) continue;
                    matched = true;
                    for (el.fields) |f| _ = try checkExpr(a, types, fns, structs, methods, scope, f.value, diags);
                    break;
                }
            }
            if (!matched) {
                try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "unknown variant '{s}.{s}'", .{ tname, el.variant }));
            }
            return et;
        },
        .tuple => |elems| {
            var elem_tys: std.ArrayListUnmanaged(*TypeInfo) = .{};
            for (elems) |el| try elem_tys.append(a, try checkExpr(a, types, fns, structs, methods, scope, el, diags));
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .struct_ty,
                .name = try std.fmt.allocPrint(a, "__Tup{d}", .{elems.len}),
                .tuple_elems = try elem_tys.toOwnedSlice(a),
            };
            return ti;
        },
        .cstring => return types.get("str").?,
        .range => |r| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, r.start, diags);
            _ = try checkExpr(a, types, fns, structs, methods, scope, r.end, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .i64_ty, .name = "range", .bits = 64, .unsigned = false };
            return ti;
        },
        .array_lit => |al| {
            for (al.elems) |el| _ = try checkExpr(a, types, fns, structs, methods, scope, el, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .array_ty,
                .name = "array_lit",
                .array_len = al.elems.len,
                .pointee = types.get("i64"),
            };
            return ti;
        },
        .interp => |ip| {
            for (ip.parts) |p| switch (p) {
                .text => {},
                .expr => |ex| _ = try checkExpr(a, types, fns, structs, methods, scope, ex, diags),
            };
            return types.get("str").?;
        },
        .try_cast => |c| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, c.expr, diags);
            const to = try resolveTypeExpr(a, types, c.ty, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .optional, .name = "opt", .pointee = to };
            return ti;
        },
        .is_type => |it| {
            const scrut = try checkExpr(a, types, fns, structs, methods, scope, it.expr, diags);
            // `e is Variant` — when T is not a known type but names a variant of e's enum.
            if (it.ty.* == .name) {
                const n = it.ty.name;
                if (types.get(n) == null and scrut.kind == .enum_ty) {
                    if (scrut.enum_decl) |ed| {
                        for (ed.variants) |v| {
                            if (std.mem.eql(u8, v.name, n)) return types.get("bool").?;
                        }
                    }
                }
            }
            _ = try resolveTypeExpr(a, types, it.ty, diags);
            return types.get("bool").?;
        },
        .force_unwrap => |inner| {
            const t = try checkExpr(a, types, fns, structs, methods, scope, inner, diags);
            if (t.kind == .optional) {
                return t.pointee orelse try voidType(a);
            }
            // Non-optional: accept (no-op unwrap) for pointer-ish values.
            return t;
        },
        .go_block => |gb| {
            for (gb.with_args) |w| _ = try checkExpr(a, types, fns, structs, methods, scope, w, diags);
            // Body sees parent locals (captures by address at lower time).
            try checkBlock(a, types, fns, structs, methods, gb.body, diags, scope);
            return try voidType(a); // Task handle is opaque i64 at lower
        },
        .go_expr => |ge| {
            _ = try checkExpr(a, types, fns, structs, methods, scope, ge, diags);
            return try voidType(a);
        },
        .scope_block => |sb| {
            try checkBlock(a, types, fns, structs, methods, sb.body, diags, scope);
            return try voidType(a);
        },
        .select_block => |sb| {
            try checkBlock(a, types, fns, structs, methods, sb.body, diags, scope);
            return try voidType(a);
        },
        .type_value => {
            // Only valid as receiver for `.init` etc.; type-check at the call site.
            return try voidType(a);
        },
        .lambda => {
            return try checkLambda(a, types, fns, structs, methods, scope, e, null, diags);
        },
        .quote => {
            // `meta.Code` — only meaningful inside `@Annotation` bodies (expand phase).
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .named, .name = "Code" };
            return ti;
        },
    }
}

/// True when `args` could bind to `params` under positional/default/trailing rules.
/// Used to avoid false "missing argument" when flattened `fns` holds a same-named sibling
/// (`mem.slice` vs `text.slice`) after import merge.
fn callArgsFitParams(params: []ast.Param, args: []*ast.Expr, arg_names: []const ?[]const u8) bool {
    _ = arg_names;
    for (params) |p| {
        if (p.is_pack) return true;
    }
    var provided = args.len;
    if (args.len > 0 and params.len > 0 and args[args.len - 1].* == .lambda) {
        // Trailing lambda always fills the last parameter.
        provided = args.len; // already counted
    }
    if (provided > params.len) return false;
    var required: usize = 0;
    for (params) |p| {
        if (p.default == null) required += 1;
    }
    return provided >= required;
}

/// Expand positional / named / default / trailing-lambda args to match `params` order.
fn expandCallArgs(
    a: std.mem.Allocator,
    params: []ast.Param,
    args: []*ast.Expr,
    arg_names: []const ?[]const u8,
    diags: *std.ArrayListUnmanaged(Diagnostic),
    at: ?*const ast.Expr,
) CheckError![]*ast.Expr {
    const n = params.len;
    var out = try a.alloc(?*ast.Expr, n);
    @memset(out, null);

    var trailing: ?*ast.Expr = null;
    var usable_len = args.len;
    if (args.len > 0 and n > 0 and args[args.len - 1].* == .lambda) {
        // Trailing lambda always fills the last parameter.
        trailing = args[args.len - 1];
        usable_len = args.len - 1;
        out[n - 1] = trailing;
    }

    var next_pos: usize = 0;
    var i: usize = 0;
    while (i < usable_len) : (i += 1) {
        const name: ?[]const u8 = if (arg_names.len == args.len) arg_names[i] else if (arg_names.len == usable_len) arg_names[i] else null;
        if (name) |nm| {
            var found = false;
            for (params, 0..) |p, pi| {
                if (std.mem.eql(u8, p.name, nm)) {
                    if (out[pi] != null) {
                        try emitDiag(a, diags, at, 1, 1, try std.fmt.allocPrint(a, "duplicate argument '{s}'", .{nm}));
                    }
                    out[pi] = args[i];
                    found = true;
                    break;
                }
            }
            if (!found) {
                try emitDiag(a, diags, at, 1, 1, try std.fmt.allocPrint(a, "unknown argument '{s}'", .{nm}));
            }
        } else {
            while (next_pos < n and out[next_pos] != null) : (next_pos += 1) {}
            if (next_pos >= n) {
                try emitDiag(a, diags, at, 1, 1, "too many arguments");
                break;
            }
            out[next_pos] = args[i];
            next_pos += 1;
        }
    }

    var result = try a.alloc(*ast.Expr, n);
    for (params, 0..) |p, pi| {
        if (out[pi]) |e| {
            result[pi] = e;
        } else if (p.default) |d| {
            result[pi] = d;
        } else {
            try emitDiag(a, diags, at, 1, 1, try std.fmt.allocPrint(a, "missing argument '{s}'", .{p.name}));
            const dummy = try a.create(ast.Expr);
            dummy.* = .{ .int = 0 };
            result[pi] = dummy;
        }
    }
    return result;
}

fn isZeroParamIfaceFn(te: *ast.TypeExpr) bool {
    if (te.* != .iface_fn) return false;
    return te.iface_fn.params.len == 0;
}

fn checkLambda(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    methods: *std.StringHashMapUnmanaged(*ast.MethodDecl),
    scope: *Scope,
    lam_expr: *ast.Expr,
    expected: ?*ast.TypeExpr,
    diags: *std.ArrayListUnmanaged(Diagnostic),
) CheckError!*TypeInfo {
    if (lam_expr.* != .lambda) return try voidType(a);
    // `{ body }` with expected `() -> R` is a zero-param lambda (no `it`).
    // Mutate AST so IR sees the same shape.
    if (lam_expr.lambda.implicit_it and expected != null and isZeroParamIfaceFn(expected.?)) {
        lam_expr.lambda.params = &.{};
        lam_expr.lambda.implicit_it = false;
    }
    const lam = lam_expr.lambda;

    var child = Scope{
        .parent = scope,
        .vars = .{},
        .narrowed = .{},
    };

    // Bind params; `_` / missing types inferred from expected iface_fn when present.
    const exp_params: []ast.Param = if (expected) |et|
        (if (et.* == .iface_fn) et.iface_fn.params else &.{})
    else
        &.{};

    for (lam.params, 0..) |p, i| {
        var ty: *TypeInfo = undefined;
        const is_infer = p.ty.* == .name and std.mem.eql(u8, p.ty.name, "_");
        if (is_infer and i < exp_params.len) {
            ty = try resolveTypeExpr(a, types, exp_params[i].ty, diags);
        } else if (is_infer) {
            ty = types.get("i64").?;
        } else {
            ty = try resolveTypeExpr(a, types, p.ty, diags);
        }
        try child.vars.put(a, p.name, .{ .ty = ty, .mutable = false });
    }

    if (lam.expr_body) |eb| {
        _ = try checkExpr(a, types, fns, structs, methods, &child, eb, diags);
    } else if (lam.block) |b| {
        try checkBlock(a, types, fns, structs, methods, b, diags, &child);
    }

    const ti = try a.create(TypeInfo);
    ti.* = .{ .kind = .iface_fn, .name = "iface_fn" };
    return ti;
}

fn checkArgsAgainstParams(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(*ast.StructDecl),
    methods: *std.StringHashMapUnmanaged(*ast.MethodDecl),
    scope: *Scope,
    params: []ast.Param,
    args: []*ast.Expr,
    arg_names: []const ?[]const u8,
    diags: *std.ArrayListUnmanaged(Diagnostic),
    at: *ast.Expr,
) CheckError!void {
    // Pack params (`...T`) accept a variable arg list — skip named/default expansion.
    for (params) |p| {
        if (p.is_pack) {
            for (args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
            return;
        }
    }
    const expanded = try expandCallArgs(a, params, args, arg_names, diags, at);
    for (params, 0..) |p, i| {
        const arg = expanded[i];
        if (arg.* == .lambda) {
            _ = try checkLambda(a, types, fns, structs, methods, scope, arg, p.ty, diags);
        } else {
            _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
        }
    }
}

fn voidType(a: std.mem.Allocator) !*TypeInfo {
    const ti = try a.create(TypeInfo);
    ti.* = .{ .kind = .void_ty, .name = "void" };
    return ti;
}

/// Types.md: comparable when every payload is bool/int/float/bytes/str or nested comparable enum.
fn enumIsComparable(types: *std.StringHashMapUnmanaged(*TypeInfo), ti: *TypeInfo) bool {
    const ed = ti.enum_decl orelse return true; // tag-only / unknown → allow (scalar path)
    for (ed.variants) |v| {
        for (v.fields) |f| {
            if (!payloadTypeComparable(types, f.ty)) return false;
        }
    }
    return true;
}

fn payloadTypeComparable(types: *std.StringHashMapUnmanaged(*TypeInfo), te: *ast.TypeExpr) bool {
    return switch (te.*) {
        .name => |n| blk: {
            const ok = [_][]const u8{ "bool", "byte", "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "usize", "isize", "f32", "f64", "str", "bytes", "rune" };
            for (ok) |k| if (std.mem.eql(u8, n, k)) break :blk true;
            if (types.get(n)) |inner| {
                if (inner.kind == .enum_ty) break :blk enumIsComparable(types, inner);
            }
            break :blk false;
        },
        .pointer, .pointer_const, .optional, .slice_view, .array, .tuple => false,
        else => false,
    };
}

/// Implicit assign: widen ints, same type, optional wrap, iface structural later
fn canAssign(from: *TypeInfo, to: *TypeInfo) bool {
    if (from.kind == to.kind and std.mem.eql(u8, from.name, to.name)) return true;
    // `bytes` / `str` builtins vs same-kind aliases that share the spelling.
    if (from.kind == to.kind and (from.kind == .bytes_ty or from.kind == .string_ty)) return true;
    // Function values are interchangeable at this check depth.
    if (from.kind == .iface_fn and to.kind == .iface_fn) return true;
    // Generic type params are placeholders in the check subset.
    if (from.kind == .named or to.kind == .named) return true;
    if (to.kind == .optional) {
        if (from.kind == .optional) return true;
        if (from.kind == .pointer or from.kind == .struct_ty) return true;
        if (to.pointee) |p| return canAssign(from, p) or from.kind == .pointer;
        return false;
    }
    // untyped int literal (i64) → any integer
    if (from.kind == .i64_ty and to.bits > 0) return true;
    // widen unsigned
    if (from.bits > 0 and to.bits > 0 and from.unsigned and to.unsigned and from.bits <= to.bits) return true;
    // pointer to optional pointer / raw
    if (from.kind == .pointer and to.kind == .pointer) return true;
    // null literal → any pointer (C-nullable *T / **T; package init already allowed this)
    if (from.kind == .optional and std.mem.eql(u8, from.name, "null") and to.kind == .pointer) return true;
    // interface / named struct
    if (from.kind == .struct_ty and to.kind == .iface_struct) return true;
    if (from.kind == .pointer and to.kind == .iface_struct) return true;
    return false;
}

fn castAllowed(from: *TypeInfo, to: *TypeInfo, explicit: bool) bool {
    if (canAssign(from, to)) return true;
    if (explicit and from.bits > 0 and to.bits > 0) return true; // narrow via as
    if (from.kind == .struct_ty and to.kind == .struct_ty) return true; // bitcast-ish
    return explicit;
}

fn findPrintString(block: *ast.Block) ?[]const u8 {
    for (block.stmts) |stmt| {
        switch (stmt) {
            .expr => |e| {
                if (e.* == .call and e.call.callee.* == .ident and std.mem.eql(u8, e.call.callee.ident, "print")) {
                    if (e.call.args.len == 1 and e.call.args[0].* == .string) {
                        return e.call.args[0].string;
                    }
                }
            },
            .if_stmt => |ifs| {
                if (findPrintString(ifs.then_block)) |s| return s;
                if (ifs.else_block) |eb| if (findPrintString(eb)) |s| return s;
            },
            .with_stmt => |ws| {
                if (findPrintString(ws.body)) |s| return s;
            },
            else => {},
        }
    }
    return null;
}

test "widen rule" {
    var from = TypeInfo{ .kind = .u16_ty, .name = "u16", .bits = 16, .unsigned = true };
    var to = TypeInfo{ .kind = .u32_ty, .name = "u32", .bits = 32, .unsigned = true };
    try std.testing.expect(canAssign(&from, &to));
    try std.testing.expect(!canAssign(&to, &from));
}

test "enum payload check and equality" {
    const parse = @import("../parse/parse.zig");
    const src =
        \\enum Shape {
        \\    Circle(r: i64)
        \\    Empty
        \\}
        \\fn main() {
        \\    val s = Shape.Circle(r: 1)
        \\    val e: Shape = .Empty
        \\    if s == e {}
        \\}
        \\
    ;
    var file = try parse.Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    var cr = try check(std.testing.allocator, &file);
    defer cr.deinit();
    try std.testing.expect(cr.ok);
}

test "enum equality rejected for struct payload" {
    const parse = @import("../parse/parse.zig");
    const src =
        \\struct Box { n: i64 }
        \\enum E { Wrap(b: Box) }
        \\fn eq(a: E, b: E): bool {
        \\    return a == b
        \\}
        \\
    ;
    var file = try parse.Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    var cr = try check(std.testing.allocator, &file);
    defer cr.deinit();
    try std.testing.expect(!cr.ok);
    var found = false;
    for (cr.diagnostics) |d| {
        if (std.mem.indexOf(u8, d.message, "does not apply") != null) found = true;
    }
    try std.testing.expect(found);
}

test "iface_fn assign" {
    var a = TypeInfo{ .kind = .iface_fn, .name = "iface_fn" };
    var b = TypeInfo{ .kind = .iface_fn, .name = "iface_fn" };
    try std.testing.expect(canAssign(&a, &b));
}

test "typeInfo and typeName builtins" {
    const parse = @import("../parse/parse.zig");
    const src =
        \\struct Box { n: i64 }
        \\fn main() {
        \\    val ti = typeInfo(Box)
        \\    val n = typeName(ti)
        \\    if (!(n == "Box")) {
        \\        panic("bad\n")
        \\    }
        \\}
        \\
    ;
    var file = try parse.Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    var cr = try check(std.testing.allocator, &file);
    defer cr.deinit();
    try std.testing.expect(cr.ok);
}
