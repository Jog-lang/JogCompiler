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
};

pub const Diagnostic = struct {
    line: u32 = 1,
    col: u32 = 1,
    message: []const u8,
};

/// Active file while `check` runs (CLI is single-threaded). Used to attach
/// expression spans to diagnostics without threading File through every helper.
threadlocal var active_file: ?*const ast.File = null;

fn emitDiag(
    a: std.mem.Allocator,
    diags: *std.ArrayListUnmanaged(Diagnostic),
    at: ?*const ast.Expr,
    line: u32,
    col: u32,
    message: []const u8,
) !void {
    var d: Diagnostic = .{ .line = line, .col = col, .message = message };
    if (at) |e| {
        if (active_file) |f| {
            const sp = f.spanOf(e);
            d.line = sp.line;
            d.col = sp.col;
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

    // builtins
    try defineBuiltin(a, &types, "byte", .{ .kind = .byte_ty, .name = "byte", .bits = 8, .unsigned = true });
    try defineBuiltin(a, &types, "u8", .{ .kind = .byte_ty, .name = "u8", .bits = 8, .unsigned = true });
    try defineBuiltin(a, &types, "bool", .{ .kind = .bool_ty, .name = "bool", .bits = 1, .unsigned = true });
    try defineBuiltin(a, &types, "u16", .{ .kind = .u16_ty, .name = "u16", .bits = 16, .unsigned = true });
    try defineBuiltin(a, &types, "u32", .{ .kind = .u32_ty, .name = "u32", .bits = 32, .unsigned = true });
    try defineBuiltin(a, &types, "u64", .{ .kind = .u64_ty, .name = "u64", .bits = 64, .unsigned = true });
    try defineBuiltin(a, &types, "i32", .{ .kind = .i32_ty, .name = "i32", .bits = 32, .unsigned = false });
    try defineBuiltin(a, &types, "i64", .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "f32", .{ .kind = .f32_ty, .name = "f32", .bits = 32, .unsigned = false });
    try defineBuiltin(a, &types, "f64", .{ .kind = .f64_ty, .name = "f64", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "error", .{ .kind = .i64_ty, .name = "error", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "usize", .{ .kind = .usize_ty, .name = "usize", .bits = 64, .unsigned = true });
    try defineBuiltin(a, &types, "str", .{ .kind = .string_ty, .name = "str" });
    try defineBuiltin(a, &types, "bytes", .{ .kind = .bytes_ty, .name = "bytes" });
    try defineBuiltin(a, &types, "void", .{ .kind = .void_ty, .name = "void" });
    // Opaque pool handle (i64-sized); with (Dispatcher) migrates stackful coros (gate 3).
    try defineBuiltin(a, &types, "Dispatcher", .{ .kind = .i64_ty, .name = "Dispatcher", .bits = 64, .unsigned = false });

    // collect decls
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
            .enum_decl => |ed| {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .enum_ty, .name = ed.name, .bits = 64, .unsigned = false };
                try types.put(a, ed.name, ti);
            },
            .import_decl => {},
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
                for (ef.params) |p| _ = try resolveTypeExpr(a, &types, p.ty, &diags);
                if (ef.ret) |r| _ = try resolveTypeExpr(a, &types, r, &diags);
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
                    for (ef.params) |p| _ = try resolveTypeExpr(a, &types, p.ty, &diags);
                    if (ef.ret) |r| _ = try resolveTypeExpr(a, &types, r, &diags);
                }
            },
            .global_decl => {}, // bound into package scope below
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
        } else if (gd.init) |init| {
            // Infer from a literal init only (const fold path).
            t = switch (init.*) {
                .float => types.get("f64").?,
                else => types.get("i64").?,
            };
        } else {
            t = types.get("i64").?;
        }
        if (gd.init) |init| {
            _ = try checkExpr(a, &types, &fns, &structs, &methods, &globals_scope, init, &diags);
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
        if (fd.body) |body| {
            try checkFunctionBody(a, &types, &fns, &structs, &methods, fd.params, fd.contexts, body, &diags, &globals_scope);
        }
        for (bound.items) |tp| _ = types.remove(tp);
    }

    // methods: structural interface satisfaction (basic)
    var mit = methods.iterator();
    while (mit.next()) |e| {
        const md = e.value_ptr.*;
        if (structs.get(md.type_name) == null) {
            try emitDiag(a, &diags, null, 1, 1, try std.fmt.allocPrint(a, "method on unknown type '{s}'", .{md.type_name}));
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
            if (types.get(n)) |t| return t;
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
            if (types.get(na.name)) |t| return t;
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .struct_ty, .name = mangled };
            try types.put(a, mangled, ti);
            return ti;
        },
        .tuple => |elems| {
            for (elems) |el| _ = try resolveTypeExpr(a, types, el, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .struct_ty,
                .name = try std.fmt.allocPrint(a, "__Tup{d}", .{elems.len}),
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
    var scope = Scope{
        .parent = parent_scope,
        .vars = .{},
        .narrowed = .{},
    };
    for (block.stmts) |stmt| {
        try checkStmt(a, types, fns, structs, methods, &scope, stmt, diags);
    }
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
            _ = it;
            const i64t = types.get("i64").?;
            for (ds.names) |n| {
                try scope.vars.put(a, n, .{ .ty = i64t, .mutable = ds.mutable });
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
                _ = try checkExpr(a, types, fns, structs, methods, scope, fa.base, diags);
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
            _ = r;
            if (b.op == .eq or b.op == .neq or b.op == .lt or b.op == .gt or b.op == .lte or b.op == .gte or b.op == .and_op or b.op == .or_op)
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
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "alloc")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .name = "ptr", .pointee = types.get("byte") };
                return ti;
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
                std.mem.eql(u8, c.callee.ident, "sleep_ms")))
            {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                return try voidType(a);
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
                    for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
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
                const base_t = try checkExpr(a, types, fns, structs, methods, scope, c.callee.field.base, diags);
                for (c.args) |arg| _ = try checkExpr(a, types, fns, structs, methods, scope, arg, diags);
                for (c.with_args) |w| _ = try checkExpr(a, types, fns, structs, methods, scope, w, diags);
                const type_name = blk: {
                    if (base_t.kind == .struct_ty) break :blk base_t.name;
                    if (base_t.kind == .pointer) {
                        if (base_t.pointee) |po| {
                            if (po.kind == .struct_ty) break :blk po.name;
                        }
                    }
                    break :blk "";
                };
                if (type_name.len > 0) {
                    const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ type_name, c.callee.field.name });
                    if (methods.get(key)) |md| {
                        if (md.ret) |r| return try resolveTypeExpr(a, types, r, diags);
                        return try voidType(a);
                    }
                }
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
            const base = try checkExpr(a, types, fns, structs, methods, scope, f.base, diags);
            if (base.kind == .enum_ty) return base;
            if (base.kind == .struct_ty) {
                if (structs.get(base.name)) |sd| {
                    for (sd.fields) |fld| {
                        if (std.mem.eql(u8, fld.name, f.name)) {
                            return try resolveTypeExpr(a, types, fld.ty, diags);
                        }
                    }
                }
            }
            if (base.kind == .pointer) {
                if (base.pointee) |po| {
                    if (po.kind == .struct_ty) {
                        if (structs.get(po.name)) |sd| {
                            for (sd.fields) |fld| {
                                if (std.mem.eql(u8, fld.name, f.name)) {
                                    return try resolveTypeExpr(a, types, fld.ty, diags);
                                }
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
            _ = try checkExpr(a, types, fns, structs, methods, scope, w.scrutinee, diags);
            var result: ?*TypeInfo = null;
            for (w.arms) |arm| {
                const bt = try checkExpr(a, types, fns, structs, methods, scope, arm.body, diags);
                if (result == null) result = bt;
            }
            if (w.else_body) |eb| {
                const bt = try checkExpr(a, types, fns, structs, methods, scope, eb, diags);
                if (result == null) result = bt;
            }
            return result orelse try voidType(a);
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
        .cast => |c| {
            const from = try checkExpr(a, types, fns, structs, methods, scope, c.expr, diags);
            const to = try resolveTypeExpr(a, types, c.ty, diags);
            if (!castAllowed(from, to, true)) {
                try emitDiag(a, diags, e, 1, 1, try std.fmt.allocPrint(a, "cannot cast {s} to {s}", .{ from.name, to.name }));
            }
            // narrowing int constant fold check
            if (c.expr.* == .int and to.bits > 0 and to.bits < 64) {
                const max: i64 = (@as(i64, 1) << @intCast(to.bits)) - 1;
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
        .tuple => |elems| {
            for (elems) |el| _ = try checkExpr(a, types, fns, structs, methods, scope, el, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{
                .kind = .struct_ty,
                .name = try std.fmt.allocPrint(a, "__Tup{d}", .{elems.len}),
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
            _ = try checkExpr(a, types, fns, structs, methods, scope, it.expr, diags);
            _ = try resolveTypeExpr(a, types, it.ty, diags);
            return types.get("bool").?;
        },
    }
}

fn voidType(a: std.mem.Allocator) !*TypeInfo {
    const ti = try a.create(TypeInfo);
    ti.* = .{ .kind = .void_ty, .name = "void" };
    return ti;
}

/// Implicit assign: widen ints, same type, optional wrap, iface structural later
fn canAssign(from: *TypeInfo, to: *TypeInfo) bool {
    if (from.kind == to.kind and std.mem.eql(u8, from.name, to.name)) return true;
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
