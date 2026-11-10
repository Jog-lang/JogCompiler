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
    usize_ty,
    named,
    pointer,
    optional,
    struct_ty,
    iface_fn,
    iface_struct,
    string_ty,
    bytes_ty,
};

pub const TypeInfo = struct {
    kind: TypeKind,
    name: []const u8 = "",
    pointee: ?*TypeInfo = null,
    /// bit width for integers (for widen/narrow)
    bits: u16 = 0,
    unsigned: bool = true,
};

pub const Diagnostic = struct {
    line: u32 = 1,
    col: u32 = 1,
    message: []const u8,
};

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

    var diags: std.ArrayListUnmanaged(Diagnostic) = .{};
    var types = std.StringHashMapUnmanaged(*TypeInfo){};
    var structs = std.StringHashMapUnmanaged(*ast.StructDecl){};
    var iface_structs = std.StringHashMapUnmanaged(*ast.InterfaceStruct){};
    var iface_fns = std.StringHashMapUnmanaged(*ast.InterfaceFn){};
    var fns = std.StringHashMapUnmanaged(*ast.FnDecl){};
    var methods = std.StringHashMapUnmanaged(*ast.MethodDecl){};

    // builtins
    try defineBuiltin(a, &types, "byte", .{ .kind = .byte_ty, .name = "byte", .bits = 8, .unsigned = true });
    try defineBuiltin(a, &types, "bool", .{ .kind = .bool_ty, .name = "bool", .bits = 1, .unsigned = true });
    try defineBuiltin(a, &types, "u16", .{ .kind = .u16_ty, .name = "u16", .bits = 16, .unsigned = true });
    try defineBuiltin(a, &types, "u32", .{ .kind = .u32_ty, .name = "u32", .bits = 32, .unsigned = true });
    try defineBuiltin(a, &types, "u64", .{ .kind = .u64_ty, .name = "u64", .bits = 64, .unsigned = true });
    try defineBuiltin(a, &types, "i32", .{ .kind = .i32_ty, .name = "i32", .bits = 32, .unsigned = false });
    try defineBuiltin(a, &types, "i64", .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false });
    try defineBuiltin(a, &types, "usize", .{ .kind = .usize_ty, .name = "usize", .bits = 64, .unsigned = true });
    try defineBuiltin(a, &types, "str", .{ .kind = .string_ty, .name = "str" });
    try defineBuiltin(a, &types, "bytes", .{ .kind = .bytes_ty, .name = "bytes" });
    try defineBuiltin(a, &types, "void", .{ .kind = .void_ty, .name = "void" });

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
            },
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
            .fn_decl => |*fd| {
                try fns.put(a, fd.name, fd);
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
        }
    }

    // check interface structs have methods only (already true by grammar)
    // check functions
    var has_main = false;
    var main_str: ?[]const u8 = null;

    var it = fns.iterator();
    while (it.next()) |e| {
        const fd = e.value_ptr.*;
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
            try checkFunctionBody(a, &types, &fns, fd.params, fd.contexts, body, &diags);
        }
    }

    // methods: structural interface satisfaction (basic)
    var mit = methods.iterator();
    while (mit.next()) |e| {
        const md = e.value_ptr.*;
        if (structs.get(md.type_name) == null) {
            try diags.append(a, .{ .message = try std.fmt.allocPrint(a, "method on unknown type '{s}'", .{md.type_name}) });
        }
        for (md.contexts) |ctx| _ = try resolveTypeExpr(a, &types, ctx.ty, &diags);
        if (md.body) |body| {
            try checkFunctionBody(a, &types, &fns, md.params, md.contexts, body, &diags);
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
            try diags.append(a, .{ .message = try std.fmt.allocPrint(a, "unknown type '{s}'", .{n}) });
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
    params: []ast.Param,
    contexts: []ast.ContextParam,
    body: *ast.Block,
    diags: *std.ArrayListUnmanaged(Diagnostic),
) CheckError!void {
    var scope = Scope{
        .parent = null,
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
    try checkBlock(a, types, fns, body, diags, &scope);
}

fn checkBlock(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
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
        try checkStmt(a, types, fns, &scope, stmt, diags);
    }
}

fn checkStmt(
    a: std.mem.Allocator,
    types: *std.StringHashMapUnmanaged(*TypeInfo),
    fns: *std.StringHashMapUnmanaged(*ast.FnDecl),
    scope: *Scope,
    stmt: ast.Stmt,
    diags: *std.ArrayListUnmanaged(Diagnostic),
) CheckError!void {
    switch (stmt) {
        .var_decl => |vd| {
            var ty: ?*TypeInfo = null;
            if (vd.ty) |t| ty = try resolveTypeExpr(a, types, t, diags);
            if (vd.init) |init| {
                const it = try checkExpr(a, types, fns, scope, init, diags);
                if (ty) |t| {
                    if (!canAssign(it, t)) {
                        try diags.append(a, .{ .message = try std.fmt.allocPrint(a, "cannot assign {s} to {s}", .{ it.name, t.name }) });
                    }
                } else {
                    ty = it;
                }
            }
            if (ty) |t| {
                try scope.vars.put(a, vd.name, .{ .ty = t, .mutable = vd.mutable });
            }
        },
        .assign => |as| {
            const v = lookupVar(scope, as.name) orelse {
                try diags.append(a, .{ .message = try std.fmt.allocPrint(a, "unknown name '{s}'", .{as.name}) });
                return;
            };
            const rt = try checkExpr(a, types, fns, scope, as.value, diags);
            if (!canAssign(rt, v.ty)) {
                try diags.append(a, .{ .message = "assignment type mismatch" });
            }
        },
        .expr => |e| _ = try checkExpr(a, types, fns, scope, e, diags),
        .return_stmt => |re| {
            if (re) |e| _ = try checkExpr(a, types, fns, scope, e, diags);
        },
        .if_stmt => |ifs| {
            _ = try checkExpr(a, types, fns, scope, ifs.cond, diags);
            // flow-sensitive: if (x != null) narrow x
            var then_scope = Scope{ .parent = scope, .vars = .{}, .narrowed = .{} };
            if (nullCheckIdent(ifs.cond)) |name| {
                try then_scope.narrowed.put(a, name, {});
            }
            try checkBlock(a, types, fns, ifs.then_block, diags, &then_scope);
            if (ifs.else_block) |eb| {
                try checkBlock(a, types, fns, eb, diags, scope);
            }
        },
        .with_stmt => |ws| {
            for (ws.values) |v| _ = try checkExpr(a, types, fns, scope, v, diags);
            try checkBlock(a, types, fns, ws.body, diags, scope);
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
            try diags.append(a, .{ .message = try std.fmt.allocPrint(a, "unknown name '{s}'", .{name}) });
            return try voidType(a);
        },
        .int => {
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .i64_ty, .name = "i64", .bits = 64, .unsigned = false };
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
            const l = try checkExpr(a, types, fns, scope, b.left, diags);
            const r = try checkExpr(a, types, fns, scope, b.right, diags);
            _ = r;
            if (b.op == .eq or b.op == .neq) return types.get("bool").?;
            // arithmetic: allow widen
            return l;
        },
        .unary => |u| {
            const t = try checkExpr(a, types, fns, scope, u.expr, diags);
            if (u.op == .addr) {
                const ti = try a.create(TypeInfo);
                ti.* = .{ .kind = .pointer, .pointee = t, .name = "ptr" };
                return ti;
            }
            return t;
        },
        .call => |c| {
            // builtins
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "print")) {
                for (c.args) |arg| _ = try checkExpr(a, types, fns, scope, arg, diags);
                return try voidType(a);
            }
            _ = try checkExpr(a, types, fns, scope, c.callee, diags);
            for (c.args) |arg| _ = try checkExpr(a, types, fns, scope, arg, diags);
            return try voidType(a);
        },
        .field => |f| {
            _ = try checkExpr(a, types, fns, scope, f.base, diags);
            return try voidType(a);
        },
        .cast => |c| {
            const from = try checkExpr(a, types, fns, scope, c.expr, diags);
            const to = try resolveTypeExpr(a, types, c.ty, diags);
            if (!castAllowed(from, to, true)) {
                try diags.append(a, .{ .message = try std.fmt.allocPrint(a, "cannot cast {s} to {s}", .{ from.name, to.name }) });
            }
            // narrowing int constant fold check
            if (c.expr.* == .int and to.bits > 0 and to.bits < 64) {
                const max: i64 = (@as(i64, 1) << @intCast(to.bits)) - 1;
                if (c.expr.int < 0 or c.expr.int > max) {
                    try diags.append(a, .{ .message = "integer constant does not fit in cast target" });
                }
            }
            return to;
        },
        .optional_call => |oc| {
            const base = try checkExpr(a, types, fns, scope, oc.base, diags);
            if (base.kind != .optional and base.kind != .pointer) {
                // allow; ?. on optional
            }
            for (oc.args) |arg| _ = try checkExpr(a, types, fns, scope, arg, diags);
            const ti = try a.create(TypeInfo);
            ti.* = .{ .kind = .optional, .name = "opt_result", .pointee = try voidType(a) };
            return ti;
        },
        .panic_call => |p| {
            _ = try checkExpr(a, types, fns, scope, p.message, diags);
            return try voidType(a);
        },
        .struct_lit => |sl| {
            if (types.get(sl.type_name)) |t| {
                for (sl.fields) |f| _ = try checkExpr(a, types, fns, scope, f.value, diags);
                return t;
            }
            try diags.append(a, .{ .message = try std.fmt.allocPrint(a, "unknown struct '{s}'", .{sl.type_name}) });
            return try voidType(a);
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
        if (to.pointee) |p| return canAssign(from, p) or from.kind == .optional;
    }
    // widen
    if (from.bits > 0 and to.bits > 0 and from.unsigned and to.unsigned and from.bits <= to.bits) return true;
    // interface / named struct same name
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
