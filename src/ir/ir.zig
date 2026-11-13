const std = @import("std");
const ast = @import("../ast/ast.zig");
const check = @import("../check/check.zig");

/// Linear IR per function. Slots are i64/pointer-sized. jmp/jz targets are absolute instruction indices.
pub const Op = enum {
    iconst,
    saddr,
    slen,
    copy,
    add,
    sub,
    mul,
    div,
    eq,
    ne,
    lt,
    and_op,
    or_op,
    neg,
    load,
    store,
    lea_slot,
    narrow,
    call,
    icall,
    vtaddr,
    ret,
    jmp,
    jz,
    panic,
};

pub const VTable = struct {
    symbol: []const u8,
    methods: [][]const u8,
};

pub const none: u32 = std.math.maxInt(u32);

pub const Inst = struct {
    op: Op,
    dst: u32 = none,
    src0: u32 = none,
    src1: u32 = none,
    aux: i64 = 0,
    arity: u8 = 0,
    args: [8]u32 = .{none} ** 8,
};

pub const Func = struct {
    name: []const u8,
    nparams: u32,
    nlocals: u32,
    exported: bool,
    insts: []Inst,
};

pub const CalleeKind = enum { jog_fn, extern_c, runtime };

pub const Callee = struct {
    kind: CalleeKind,
    name: []const u8,
};

pub const Program = struct {
    strings: [][]const u8,
    callees: []Callee,
    funcs: []Func,
    vtables: []VTable,
    needs_libc: bool,
    /// `-l` names from `@link` / jog.mod link.* lines.
    link_libs: [][]const u8,
    /// `-L` / rpath dirs from CLI `--libdir` / jog.mod libdir.* lines.
    libdirs: [][]const u8,
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *Program) void {
        self.arena.deinit();
    }
};

const LowerError = error{ UnknownName, OutOfMemory, TooManyArgs, TypeError };

const StructInfo = struct {
    fields: []ast.Field,
};

const MethodInfo = struct {
    mangled: []const u8,
    pointer_recv: bool,
    type_name: []const u8,
    name: []const u8,
    nparams: u32, // excluding self
    ncontexts: u32,
    contexts: []ast.ContextParam,
    ret_name: []const u8 = "",
};

const IfaceInfo = struct {
    methods: []ast.MethodSig,
};

const FnInfo = struct {
    mangled: []const u8,
    nparams: u32,
    contexts: []ast.ContextParam,
    exported: bool,
    /// Return type name when a named struct/scalar; empty if void/unknown.
    ret_name: []const u8 = "",
};

const Ambient = struct {
    slot: u32,
    type_name: []const u8, // concrete struct or iface name
};

const SlotKind = enum { scalar, struct_ptr, iface_ptr, opt_ptr, raw_ptr };

const SlotMeta = struct {
    kind: SlotKind,
    type_name: []const u8 = "",
};

pub fn lower(parent: std.mem.Allocator, file: *const ast.File, cr: *const check.CheckResult) !Program {
    _ = cr;
    var program: Program = .{
        .strings = &[_][]const u8{},
        .callees = &[_]Callee{},
        .funcs = &[_]Func{},
        .vtables = &[_]VTable{},
        .needs_libc = false,
        .link_libs = &[_][]const u8{},
        .libdirs = &[_][]const u8{},
        .arena = std.heap.ArenaAllocator.init(parent),
    };
    errdefer program.deinit();
    const a = program.arena.allocator();

    var strings: std.ArrayListUnmanaged([]const u8) = .{};
    var link_libs: std.ArrayListUnmanaged([]const u8) = .{};
    var callees: std.ArrayListUnmanaged(Callee) = .{};
    var callee_map: std.StringHashMapUnmanaged(u32) = .{};
    var structs: std.StringHashMapUnmanaged(StructInfo) = .{};
    var methods: std.StringHashMapUnmanaged(MethodInfo) = .{}; // "Type.method"
    var ifaces: std.StringHashMapUnmanaged(IfaceInfo) = .{};
    var fn_infos: std.StringHashMapUnmanaged(FnInfo) = .{};
    var vtables: std.ArrayListUnmanaged(VTable) = .{};
    var vtable_map: std.StringHashMapUnmanaged(u32) = .{};

    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_print" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_panic" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_alloc" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_spawn" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_stub" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_go" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_yield" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_coro_drain" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_coro_alive" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_coro_id" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_coro_park" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_coro_unpark" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_single" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_pool" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_new" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_get" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_io" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_enter" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_dispatcher_leave" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_sleep_ms" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_print_tid" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_worker_id" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_gettid" });

    for (file.decls) |d| {
        switch (d) {
            .struct_decl => |sd| try structs.put(a, sd.name, .{ .fields = sd.fields }),
            .interface_struct => |ins| try ifaces.put(a, ins.name, .{ .methods = ins.methods }),
            .extern_block => |eb| {
                if (eb.link_lib) |lib| {
                    var dup = false;
                    for (link_libs.items) |existing| {
                        if (std.mem.eql(u8, existing, lib)) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) try link_libs.append(a, try a.dupe(u8, lib));
                }
                for (eb.fns) |ef| {
                    try internCallee(a, &callees, &callee_map, .{ .kind = .extern_c, .name = ef.name });
                    var ret_name: []const u8 = "";
                    if (ef.ret) |r| ret_name = typeExprName(r);
                    try fn_infos.put(a, ef.name, .{
                        .mangled = ef.name,
                        .nparams = @intCast(ef.params.len),
                        .contexts = &[_]ast.ContextParam{},
                        .exported = false,
                        .ret_name = ret_name,
                    });
                    program.needs_libc = true;
                }
            },
            .fn_decl, .actual_fn => |fd| {
                if (fd.body != null) {
                    try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = fd.name });
                    var ret_name: []const u8 = "";
                    if (fd.ret) |r| ret_name = typeExprName(r);
                    try fn_infos.put(a, fd.name, .{
                        .mangled = fd.name,
                        .nparams = @intCast(fd.params.len),
                        .contexts = fd.contexts,
                        .exported = fd.exported or std.mem.eql(u8, fd.name, "main"),
                        .ret_name = ret_name,
                    });
                }
            },
            .expect_fn => |ef| {
                // Placeholder until an actual_fn / runtime symbol is seen; do not pull libc yet.
                if (!callee_map.contains(ef.name)) {
                    const kind: CalleeKind = if (std.mem.startsWith(u8, ef.name, "jogrt_")) .runtime else .jog_fn;
                    try internCallee(a, &callees, &callee_map, .{ .kind = kind, .name = ef.name });
                    var ret_name: []const u8 = "";
                    if (ef.ret) |r| ret_name = typeExprName(r);
                    try fn_infos.put(a, ef.name, .{
                        .mangled = ef.name,
                        .nparams = @intCast(ef.params.len),
                        .contexts = &[_]ast.ContextParam{},
                        .exported = false,
                        .ret_name = ret_name,
                    });
                }
            },
            .method => |md| {
                if (md.body != null) {
                    const mangled = try std.fmt.allocPrint(a, "{s}_{s}", .{ md.type_name, md.name });
                    const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ md.type_name, md.name });
                    try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = mangled });
                    var mret: []const u8 = "";
                    if (md.ret) |r| mret = typeExprName(r);
                    try methods.put(a, key, .{
                        .mangled = mangled,
                        .pointer_recv = md.pointer_recv,
                        .type_name = md.type_name,
                        .name = md.name,
                        .nparams = @intCast(md.params.len),
                        .ncontexts = @intCast(md.contexts.len),
                        .contexts = md.contexts,
                        .ret_name = mret,
                    });
                }
            },
            else => {},
        }
    }

    var funcs: std.ArrayListUnmanaged(Func) = .{};
    for (file.decls) |d| {
        const maybe_fd: ?ast.FnDecl = switch (d) {
            .fn_decl => |fd| if (fd.body != null) fd else null,
            .actual_fn => |fd| if (fd.body != null) fd else null,
            else => null,
        };
        if (maybe_fd) |fd| {
            try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = fd.name });
            const f = try lowerFn(a, .{
                .name = fd.name,
                .params = fd.params,
                .contexts = fd.contexts,
                .body = fd.body.?,
                .exported = fd.exported or std.mem.eql(u8, fd.name, "main"),
                .self_type = null,
                .pointer_recv = false,
            }, &strings, &callee_map, &structs, &methods, &ifaces, &fn_infos, &vtables, &vtable_map);
            try funcs.append(a, f);
        } else if (d == .method and d.method.body != null) {
            const md = d.method;
            const mangled = try std.fmt.allocPrint(a, "{s}_{s}", .{ md.type_name, md.name });
            const f = try lowerFn(a, .{
                .name = mangled,
                .params = md.params,
                .contexts = md.contexts,
                .body = md.body.?,
                .exported = true,
                .self_type = md.type_name,
                .pointer_recv = md.pointer_recv,
            }, &strings, &callee_map, &structs, &methods, &ifaces, &fn_infos, &vtables, &vtable_map);
            try funcs.append(a, f);
        }
    }

    program.strings = try strings.toOwnedSlice(a);
    program.callees = try callees.toOwnedSlice(a);
    program.funcs = try funcs.toOwnedSlice(a);
    program.vtables = try vtables.toOwnedSlice(a);
    program.link_libs = try link_libs.toOwnedSlice(a);
    return program;
}

fn internCallee(a: std.mem.Allocator, list: *std.ArrayListUnmanaged(Callee), map: *std.StringHashMapUnmanaged(u32), c: Callee) !void {
    if (map.contains(c.name)) return;
    try list.append(a, c);
    try map.put(a, c.name, @intCast(list.items.len - 1));
}

const FnLower = struct {
    name: []const u8,
    params: []ast.Param,
    contexts: []ast.ContextParam,
    body: *ast.Block,
    exported: bool,
    self_type: ?[]const u8,
    pointer_recv: bool,
};

const Ctx = struct {
    a: std.mem.Allocator,
    strings: *std.ArrayListUnmanaged([]const u8),
    callee_map: *std.StringHashMapUnmanaged(u32),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    vtables: *std.ArrayListUnmanaged(VTable),
    vtable_map: *std.StringHashMapUnmanaged(u32),
    slots: *std.StringHashMapUnmanaged(u32),
    meta: *std.StringHashMapUnmanaged(SlotMeta),
    next_slot: *u32,
    insts: *std.ArrayListUnmanaged(Inst),
    ambient: *std.ArrayListUnmanaged(Ambient),
    /// Indices of `jmp` insts from `break` in the innermost while; patched to loop end.
    break_jmps: *std.ArrayListUnmanaged(usize),

    fn fresh(self: *Ctx) !u32 {
        const s = self.next_slot.*;
        self.next_slot.* += 1;
        return s;
    }
    fn emit(self: *Ctx, inst: Inst) !void {
        try self.insts.append(self.a, inst);
    }
};

fn lowerFn(
    a: std.mem.Allocator,
    fl: FnLower,
    strings: *std.ArrayListUnmanaged([]const u8),
    callee_map: *std.StringHashMapUnmanaged(u32),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    vtables: *std.ArrayListUnmanaged(VTable),
    vtable_map: *std.StringHashMapUnmanaged(u32),
) !Func {
    var slots: std.StringHashMapUnmanaged(u32) = .{};
    var meta: std.StringHashMapUnmanaged(SlotMeta) = .{};
    var next_slot: u32 = 0;
    var ambient: std.ArrayListUnmanaged(Ambient) = .{};

    // contexts first (hidden args), then self, then params
    for (fl.contexts) |ctx| {
        const slot = next_slot;
        next_slot += 1;
        const ty_name = typeExprName(ctx.ty);
        if (ctx.name) |n| {
            try slots.put(a, n, slot);
            try meta.put(a, n, metaForTypeName(ty_name, ifaces));
        }
        try ambient.append(a, .{ .slot = slot, .type_name = ty_name });
    }
    if (fl.self_type) |st| {
        const slot = next_slot;
        next_slot += 1;
        try slots.put(a, "self", slot);
        try meta.put(a, "self", .{
            .kind = if (fl.pointer_recv) .struct_ptr else .struct_ptr,
            .type_name = st,
        });
    }
    for (fl.params) |p| {
        const slot = next_slot;
        next_slot += 1;
        try slots.put(a, p.name, slot);
        try meta.put(a, p.name, metaForTypeExpr(p.ty, ifaces));
    }
    const nparams = next_slot;

    var insts: std.ArrayListUnmanaged(Inst) = .{};
    var break_jmps: std.ArrayListUnmanaged(usize) = .{};
    var ctx = Ctx{
        .a = a,
        .strings = strings,
        .callee_map = callee_map,
        .structs = structs,
        .methods = methods,
        .ifaces = ifaces,
        .fn_infos = fn_infos,
        .vtables = vtables,
        .vtable_map = vtable_map,
        .slots = &slots,
        .meta = &meta,
        .next_slot = &next_slot,
        .insts = &insts,
        .ambient = &ambient,
        .break_jmps = &break_jmps,
    };
    try lowerBlock(&ctx, fl.body);
    if (insts.items.len == 0 or insts.items[insts.items.len - 1].op != .ret) {
        try ctx.emit(.{ .op = .ret, .src0 = none });
    }
    return .{
        .name = fl.name,
        .nparams = nparams,
        .nlocals = next_slot,
        .exported = fl.exported,
        .insts = try insts.toOwnedSlice(a),
    };
}

fn typeExprName(te: *ast.TypeExpr) []const u8 {
    return switch (te.*) {
        .name => |n| n,
        .pointer, .pointer_const, .optional => |inner| typeExprName(inner),
        else => "",
    };
}

fn metaForTypeName(name: []const u8, ifaces: *std.StringHashMapUnmanaged(IfaceInfo)) SlotMeta {
    if (ifaces.contains(name)) return .{ .kind = .iface_ptr, .type_name = name };
    if (name.len == 0) return .{ .kind = .scalar };
    // treat named non-iface as struct (or alias to scalar — scalar if builtin-looking)
    if (isBuiltinScalar(name)) return .{ .kind = .scalar, .type_name = name };
    return .{ .kind = .struct_ptr, .type_name = name };
}

fn metaForTypeExpr(te: *ast.TypeExpr, ifaces: *std.StringHashMapUnmanaged(IfaceInfo)) SlotMeta {
    switch (te.*) {
        .optional => return .{ .kind = .opt_ptr, .type_name = typeExprName(te) },
        .pointer, .pointer_const => {
            const inner = typeExprName(te);
            if (ifaces.contains(inner)) return .{ .kind = .iface_ptr, .type_name = inner };
            if (isBuiltinScalar(inner) or std.mem.eql(u8, inner, "byte") or std.mem.eql(u8, inner, "void"))
                return .{ .kind = .raw_ptr, .type_name = inner };
            return .{ .kind = .struct_ptr, .type_name = inner };
        },
        .name => |n| return metaForTypeName(n, ifaces),
        else => return .{ .kind = .scalar },
    }
}

fn isBuiltinScalar(name: []const u8) bool {
    const builtins = [_][]const u8{ "byte", "u8", "bool", "u16", "u32", "u64", "i32", "i64", "usize", "str", "bytes", "void", "Dispatcher" };
    for (builtins) |b| if (std.mem.eql(u8, name, b)) return true;
    return false;
}

fn lowerBlock(ctx: *Ctx, block: *ast.Block) LowerError!void {
    for (block.stmts) |stmt| try lowerStmt(ctx, stmt);
}

fn lowerStmt(ctx: *Ctx, stmt: ast.Stmt) LowerError!void {
    switch (stmt) {
        .var_decl => |vd| {
            const slot = try ctx.fresh();
            try ctx.slots.put(ctx.a, vd.name, slot);
            var sm: SlotMeta = .{ .kind = .scalar };
            if (vd.ty) |t| sm = metaForTypeExpr(t, ctx.ifaces);
            if (vd.init) |init| {
                const v = try lowerExpr(ctx, init);
                try ctx.emit(.{ .op = .copy, .dst = slot, .src0 = v });
                if (vd.ty == null) {
                    if (ctx.meta.get(slotNameOf(ctx, v))) |m| sm = m;
                    // struct lit / alloc leave meta on temp — copy from init kind via peek
                    sm = inferMetaFromExpr(ctx, init) orelse sm;
                }
            } else {
                try ctx.emit(.{ .op = .iconst, .dst = slot, .aux = 0 });
            }
            try ctx.meta.put(ctx.a, vd.name, sm);
        },
        .assign => |as| {
            const v = try lowerExpr(ctx, as.value);
            if (as.field) |fa| {
                const base = try lowerExpr(ctx, fa.base);
                const off = try fieldOffset(ctx, fa.base, fa.name);
                const addr = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = addr, .aux = off });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = addr });
                try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = v });
            } else {
                const slot = ctx.slots.get(as.name) orelse return error.UnknownName;
                try ctx.emit(.{ .op = .copy, .dst = slot, .src0 = v });
            }
        },
        .expr => |e| _ = try lowerExpr(ctx, e),
        .return_stmt => |re| {
            if (re) |e| {
                try ctx.emit(.{ .op = .ret, .src0 = try lowerExpr(ctx, e) });
            } else {
                try ctx.emit(.{ .op = .ret, .src0 = none });
            }
        },
        .if_stmt => |ifs| {
            const cond = try lowerExpr(ctx, ifs.cond);
            const jz_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jz, .src0 = cond, .aux = 0 });
            try lowerBlock(ctx, ifs.then_block);
            const jmp_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            const else_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jz_i].aux = else_pos;
            if (ifs.else_block) |eb| {
                try lowerBlock(ctx, eb);
            }
            const end_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jmp_i].aux = end_pos;
            if (ifs.else_block == null) {
                ctx.insts.items[jz_i].aux = end_pos;
            }
        },
        .while_stmt => |ws| {
            const break_mark = ctx.break_jmps.items.len;
            const head: i64 = @intCast(ctx.insts.items.len);
            const cond = try lowerExpr(ctx, ws.cond);
            const jz_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jz, .src0 = cond, .aux = 0 });
            try lowerBlock(ctx, ws.body);
            try ctx.emit(.{ .op = .jmp, .aux = head });
            const end_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jz_i].aux = end_pos;
            while (ctx.break_jmps.items.len > break_mark) {
                const ji = ctx.break_jmps.pop().?;
                ctx.insts.items[ji].aux = end_pos;
            }
        },
        .break_stmt => {
            const ji = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            try ctx.break_jmps.append(ctx.a, ji);
        },
        .with_stmt => |ws| {
            const mark = ctx.ambient.items.len;
            var n_disp_enter: u32 = 0;
            for (ws.values) |v| {
                const slot = try lowerExpr(ctx, v);
                const tn = inferTypeNameFromExpr(ctx, v) orelse "";
                try ctx.ambient.append(ctx.a, .{ .slot = slot, .type_name = tn });
                // Gate 3: Dispatcher with → park/resume onto that pool (Kotlin withContext shape).
                if (std.mem.eql(u8, tn, "Dispatcher")) {
                    const cid = ctx.callee_map.get("jogrt_dispatcher_enter").?;
                    var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
                    inst.args[0] = slot;
                    try ctx.emit(inst);
                    n_disp_enter += 1;
                }
            }
            try lowerBlock(ctx, ws.body);
            var i: u32 = 0;
            while (i < n_disp_enter) : (i += 1) {
                const cid = ctx.callee_map.get("jogrt_dispatcher_leave").?;
                try ctx.emit(.{ .op = .call, .dst = none, .aux = cid, .arity = 0 });
            }
            ctx.ambient.shrinkRetainingCapacity(mark);
        },
    }
}

fn slotNameOf(_: *Ctx, _: u32) []const u8 {
    return "";
}

fn inferMetaFromExpr(ctx: *Ctx, e: *ast.Expr) ?SlotMeta {
    switch (e.*) {
        .struct_lit => |sl| return .{ .kind = .struct_ptr, .type_name = sl.type_name },
        .null_lit => return .{ .kind = .opt_ptr },
        .ident => |n| return ctx.meta.get(n),
        .call => |c| {
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "alloc"))
                return .{ .kind = .raw_ptr, .type_name = "byte" };
            if (c.callee.* == .ident and (std.mem.eql(u8, c.callee.ident, "dispatcher_pool") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_single") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_new") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_get") or
                std.mem.eql(u8, c.callee.ident, "dispatcher_io")))
                return .{ .kind = .scalar, .type_name = "Dispatcher" };
            if (c.callee.* == .ident) {
                if (ctx.fn_infos.get(c.callee.ident)) |fi| {
                    if (fi.ret_name.len > 0) {
                        if (ctx.structs.contains(fi.ret_name))
                            return .{ .kind = .struct_ptr, .type_name = fi.ret_name };
                        return .{ .kind = .scalar, .type_name = fi.ret_name };
                    }
                }
            }
            if (c.callee.* == .field) {
                const base_tn = inferTypeNameFromExpr(ctx, c.callee.field.base) orelse "";
                const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ base_tn, c.callee.field.name }) catch return .{ .kind = .raw_ptr };
                if (ctx.methods.get(key)) |mi| {
                    if (mi.ret_name.len > 0) {
                        if (ctx.structs.contains(mi.ret_name))
                            return .{ .kind = .struct_ptr, .type_name = mi.ret_name };
                        return .{ .kind = .scalar, .type_name = mi.ret_name };
                    }
                }
            }
            return .{ .kind = .raw_ptr };
        },
        .cast => |c| return metaForTypeExpr(c.ty, ctx.ifaces),
        else => return null,
    }
}

fn inferTypeNameFromExpr(ctx: *Ctx, e: *ast.Expr) ?[]const u8 {
    if (inferMetaFromExpr(ctx, e)) |m| {
        if (m.type_name.len > 0) return m.type_name;
    }
    return null;
}

fn fieldOffset(ctx: *Ctx, base_expr: *ast.Expr, field: []const u8) LowerError!i64 {
    const tn = blk: {
        if (base_expr.* == .ident) {
            if (ctx.meta.get(base_expr.ident)) |m| break :blk m.type_name;
        }
        if (inferTypeNameFromExpr(ctx, base_expr)) |t| break :blk t;
        std.debug.print("lower UnknownName fieldOffset type for field {s}\n", .{field});
        return error.UnknownName;
    };
    const sd = ctx.structs.get(tn) orelse {
        std.debug.print("lower UnknownName struct {s} field {s}\n", .{ tn, field });
        return error.UnknownName;
    };
    for (sd.fields, 0..) |f, i| {
        if (std.mem.eql(u8, f.name, field)) return @intCast(i * 8);
    }
    std.debug.print("lower UnknownName missing field {s}.{s}\n", .{ tn, field });
    return error.UnknownName;
}

fn lowerExpr(ctx: *Ctx, e: *ast.Expr) LowerError!u32 {
    switch (e.*) {
        .ident => |name| {
            if (ctx.slots.get(name)) |s| return s;
            // function address
            if (ctx.callee_map.get(name)) |cid| {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .saddr, .dst = d, .aux = -1 - @as(i64, @intCast(cid)) });
                return d;
            }
            std.debug.print("lower UnknownName ident: {s}\n", .{name});
            return error.UnknownName;
        },
        .int => |n| {
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = n });
            return d;
        },
        .string => |s| {
            const idx: i64 = @intCast(ctx.strings.items.len);
            try ctx.strings.append(ctx.a, try ctx.a.dupe(u8, s));
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .saddr, .dst = d, .aux = idx });
            return d;
        },
        .null_lit => {
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        },
        .binary => |b| {
            // Short-circuit && / ||
            if (b.op == .and_op) {
                const l = try lowerExpr(ctx, b.left);
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .copy, .dst = d, .src0 = l });
                const jz_i = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jz, .src0 = d, .aux = 0 });
                const r = try lowerExpr(ctx, b.right);
                try ctx.emit(.{ .op = .copy, .dst = d, .src0 = r });
                const end_pos: i64 = @intCast(ctx.insts.items.len);
                ctx.insts.items[jz_i].aux = end_pos;
                return d;
            }
            if (b.op == .or_op) {
                const l = try lowerExpr(ctx, b.left);
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .copy, .dst = d, .src0 = l });
                const jz_i = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jz, .src0 = d, .aux = 0 });
                // left was true: skip right
                const jmp_true = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jmp, .aux = 0 });
                const right_pos: i64 = @intCast(ctx.insts.items.len);
                ctx.insts.items[jz_i].aux = right_pos;
                const r = try lowerExpr(ctx, b.right);
                try ctx.emit(.{ .op = .copy, .dst = d, .src0 = r });
                const end_pos: i64 = @intCast(ctx.insts.items.len);
                ctx.insts.items[jmp_true].aux = end_pos;
                return d;
            }
            const l = try lowerExpr(ctx, b.left);
            const r = try lowerExpr(ctx, b.right);
            const d = try ctx.fresh();
            switch (b.op) {
                .add => try ctx.emit(.{ .op = .add, .dst = d, .src0 = l, .src1 = r }),
                .sub => try ctx.emit(.{ .op = .sub, .dst = d, .src0 = l, .src1 = r }),
                .mul => try ctx.emit(.{ .op = .mul, .dst = d, .src0 = l, .src1 = r }),
                .div => try ctx.emit(.{ .op = .div, .dst = d, .src0 = l, .src1 = r }),
                .eq => try ctx.emit(.{ .op = .eq, .dst = d, .src0 = l, .src1 = r }),
                .neq => try ctx.emit(.{ .op = .ne, .dst = d, .src0 = l, .src1 = r }),
                .lt => try ctx.emit(.{ .op = .lt, .dst = d, .src0 = l, .src1 = r }),
                .gt => try ctx.emit(.{ .op = .lt, .dst = d, .src0 = r, .src1 = l }),
                .and_op, .or_op => unreachable,
            }
            return d;
        },
        .unary => |u| {
            const v = try lowerExpr(ctx, u.expr);
            const d = try ctx.fresh();
            switch (u.op) {
                .neg => try ctx.emit(.{ .op = .neg, .dst = d, .src0 = v }),
                .not => {
                    const z = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
                    try ctx.emit(.{ .op = .eq, .dst = d, .src0 = v, .src1 = z });
                },
                .addr => {
                    if (u.expr.* != .ident) return error.TypeError;
                    const slot = ctx.slots.get(u.expr.ident) orelse return error.UnknownName;
                    try ctx.emit(.{ .op = .lea_slot, .dst = d, .src0 = slot });
                },
                .deref => try ctx.emit(.{ .op = .load, .dst = d, .src0 = v }),
            }
            return d;
        },
        .call => |c| return try lowerCall(ctx, c),
        .cast => |c| {
            const v = try lowerExpr(ctx, c.expr);
            const to = metaForTypeExpr(c.ty, ctx.ifaces);
            // narrow ints via `as` when target bits known
            const bits = scalarBits(typeExprName(c.ty));
            if (bits > 0 and bits < 64) {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .narrow, .dst = d, .src0 = v, .aux = bits });
                return d;
            }
            // iface coerce from concrete struct ptr
            if (to.kind == .iface_ptr) {
                return try coerceToIface(ctx, v, inferTypeNameFromExpr(ctx, c.expr) orelse "", to.type_name);
            }
            return v;
        },
        .panic_call => |p| {
            const ptr, const len = try lowerPrintArgs(ctx, p.message);
            try ctx.emit(.{ .op = .panic, .src0 = ptr, .src1 = len });
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        },
        .field => |f| {
            const base = try lowerExpr(ctx, f.base);
            const off = try fieldOffset(ctx, f.base, f.name);
            const addr = try ctx.fresh();
            const off_s = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
            try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = off_s });
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = d, .src0 = addr });
            return d;
        },
        .optional_call => |oc| {
            const base = try lowerExpr(ctx, oc.base);
            // if base == 0, result = 0; else call method
            const cond = try ctx.fresh();
            try ctx.emit(.{ .op = .copy, .dst = cond, .src0 = base });
            const jz_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jz, .src0 = cond, .aux = 0 });
            const call_slot = try lowerMethodOrIfaceCall(ctx, oc.base, base, oc.name, oc.args, &.{});
            const result = try ctx.fresh();
            try ctx.emit(.{ .op = .copy, .dst = result, .src0 = call_slot });
            const jmp_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            const zero_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jz_i].aux = zero_pos;
            try ctx.emit(.{ .op = .iconst, .dst = result, .aux = 0 });
            const end_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jmp_i].aux = end_pos;
            return result;
        },
        .struct_lit => |sl| {
            const sd = ctx.structs.get(sl.type_name) orelse return error.UnknownName;
            const size: i64 = @intCast(sd.fields.len * 8);
            if (size == 0) {
                // empty-ish: still alloc 8
            }
            const sz = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = if (size == 0) 8 else size });
            const cid = ctx.callee_map.get("jogrt_alloc").?;
            const ptr = try ctx.fresh();
            var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
            allo.args[0] = sz;
            try ctx.emit(allo);
            for (sl.fields) |flit| {
                var off: i64 = -1;
                for (sd.fields, 0..) |f, i| {
                    if (std.mem.eql(u8, f.name, flit.name)) {
                        off = @intCast(i * 8);
                        break;
                    }
                }
                if (off < 0) return error.UnknownName;
                const val = try lowerExpr(ctx, flit.value);
                const addr = try ctx.fresh();
                const off_s = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = ptr, .src1 = off_s });
                try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = val });
            }
            return ptr;
        },
    }
}

fn scalarBits(name: []const u8) i64 {
    if (std.mem.eql(u8, name, "u16") or std.mem.eql(u8, name, "i16")) return 16;
    if (std.mem.eql(u8, name, "u32") or std.mem.eql(u8, name, "i32")) return 32;
    if (std.mem.eql(u8, name, "byte") or std.mem.eql(u8, name, "u8")) return 8;
    if (std.mem.eql(u8, name, "bool")) return 1;
    return 0;
}

fn lowerCall(ctx: *Ctx, c: ast.Call) LowerError!u32 {
    if (c.callee.* == .ident) {
        const name = c.callee.ident;
        if (std.mem.eql(u8, name, "print")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const ptr, const len = try lowerPrintArgs(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_print").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 2 };
            inst.args[0] = ptr;
            inst.args[1] = len;
            try ctx.emit(inst);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "alloc")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const sz = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_alloc").?;
            const d = try ctx.fresh();
            var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
            inst.args[0] = sz;
            try ctx.emit(inst);
            return d;
        }
        if (std.mem.eql(u8, name, "spawn")) {
            if (c.args.len != 2) return error.TooManyArgs;
            const fp = try lowerExpr(ctx, c.args[0]);
            const arg = try lowerExpr(ctx, c.args[1]);
            const cid = ctx.callee_map.get("jogrt_spawn").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 2 };
            inst.args[0] = fp;
            inst.args[1] = arg;
            try ctx.emit(inst);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        // Stackful coro (gate step 1): go enqueues; yield round-robins on one OS thread.
        if (std.mem.eql(u8, name, "go")) {
            if (c.args.len != 2) return error.TooManyArgs;
            const fp = try lowerExpr(ctx, c.args[0]);
            const arg = try lowerExpr(ctx, c.args[1]);
            const cid = ctx.callee_map.get("jogrt_go").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 2 };
            inst.args[0] = fp;
            inst.args[1] = arg;
            try ctx.emit(inst);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "yield")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_yield").?;
            try ctx.emit(.{ .op = .call, .dst = none, .aux = cid, .arity = 0 });
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "coro_drain")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_coro_drain").?;
            try ctx.emit(.{ .op = .call, .dst = none, .aux = cid, .arity = 0 });
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "coro_alive")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_coro_alive").?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "coro_id")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_coro_id").?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "coro_park")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_coro_park").?;
            try ctx.emit(.{ .op = .call, .dst = none, .aux = cid, .arity = 0 });
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "coro_unpark")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const idx = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_coro_unpark").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
            inst.args[0] = idx;
            try ctx.emit(inst);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "dispatcher_single")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_dispatcher_single").?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "dispatcher_pool")) {
            const cid = ctx.callee_map.get("jogrt_dispatcher_pool").?;
            const d = try ctx.fresh();
            if (c.args.len == 0) {
                const z = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
                var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
                inst.args[0] = z;
                try ctx.emit(inst);
            } else if (c.args.len == 1) {
                const n = try lowerExpr(ctx, c.args[0]);
                var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
                inst.args[0] = n;
                try ctx.emit(inst);
            } else return error.TooManyArgs;
            return d;
        }
        if (std.mem.eql(u8, name, "dispatcher_new")) {
            const cid = ctx.callee_map.get("jogrt_dispatcher_new").?;
            const d = try ctx.fresh();
            if (c.args.len == 0) {
                const z = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
                var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
                inst.args[0] = z;
                try ctx.emit(inst);
            } else if (c.args.len == 1) {
                const n = try lowerExpr(ctx, c.args[0]);
                var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
                inst.args[0] = n;
                try ctx.emit(inst);
            } else return error.TooManyArgs;
            return d;
        }
        if (std.mem.eql(u8, name, "dispatcher_get")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const id = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_dispatcher_get").?;
            const d = try ctx.fresh();
            var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
            inst.args[0] = id;
            try ctx.emit(inst);
            return d;
        }
        if (std.mem.eql(u8, name, "dispatcher_io")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_dispatcher_io").?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "sleep_ms")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const ms = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_sleep_ms").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
            inst.args[0] = ms;
            try ctx.emit(inst);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "print_tid")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_print_tid").?;
            try ctx.emit(.{ .op = .call, .dst = none, .aux = cid, .arity = 0 });
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "worker_id") or std.mem.eql(u8, name, "gettid")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const rt = if (std.mem.eql(u8, name, "worker_id")) "jogrt_worker_id" else "jogrt_gettid";
            const cid = ctx.callee_map.get(rt).?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
            return d;
        }
        if (ctx.fn_infos.get(name)) |fi| {
            return try lowerFnCall(ctx, fi, c.args, c.with_args);
        }
    }
    if (c.callee.* == .field) {
        const base = try lowerExpr(ctx, c.callee.field.base);
        return try lowerMethodOrIfaceCall(ctx, c.callee.field.base, base, c.callee.field.name, c.args, c.with_args);
    }
    return error.UnknownName;
}

fn lowerFnCall(ctx: *Ctx, fi: FnInfo, args: []*ast.Expr, with_args: []*ast.Expr) LowerError!u32 {
    var arg_slots: [8]u32 = .{none} ** 8;
    var arity: u8 = 0;

    // resolve contexts: explicit with_args first, else ambient
    if (fi.contexts.len > 0) {
        if (with_args.len == fi.contexts.len) {
            for (fi.contexts, 0..) |cx, i| {
                const want = typeExprName(cx.ty);
                const raw = try lowerExpr(ctx, with_args[i]);
                const concrete = inferTypeNameFromExpr(ctx, with_args[i]) orelse "";
                arg_slots[arity] = try maybeCoerceCtx(ctx, raw, concrete, want);
                arity += 1;
            }
        } else if (with_args.len == 0) {
            for (fi.contexts) |cx| {
                const want = typeExprName(cx.ty);
                const amb = findAmbient(ctx, want) orelse return error.UnknownName;
                arg_slots[arity] = try maybeCoerceCtx(ctx, amb.slot, amb.type_name, want);
                arity += 1;
            }
        } else return error.TooManyArgs;
    }

    if (args.len + arity > 8) return error.TooManyArgs;
    for (args) |arg| {
        arg_slots[arity] = try lowerExpr(ctx, arg);
        arity += 1;
    }

    const cid = ctx.callee_map.get(fi.mangled) orelse return error.UnknownName;
    const d = try ctx.fresh();
    try ctx.emit(.{
        .op = .call,
        .dst = d,
        .aux = @intCast(cid),
        .arity = arity,
        .args = arg_slots,
    });
    return d;
}

fn findAmbient(ctx: *Ctx, want_iface_or_type: []const u8) ?Ambient {
    var i = ctx.ambient.items.len;
    while (i > 0) {
        i -= 1;
        const amb = ctx.ambient.items[i];
        if (std.mem.eql(u8, amb.type_name, want_iface_or_type)) return amb;
        // structural: ambient concrete satisfies iface
        if (ctx.ifaces.get(want_iface_or_type)) |iface| {
            if (satisfiesIface(ctx, amb.type_name, iface)) return amb;
        }
    }
    return null;
}

fn satisfiesIface(ctx: *Ctx, concrete: []const u8, iface: IfaceInfo) bool {
    for (iface.methods) |ms| {
        const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ concrete, ms.name }) catch return false;
        if (!ctx.methods.contains(key)) return false;
    }
    return iface.methods.len > 0 or true;
}

fn maybeCoerceCtx(ctx: *Ctx, slot: u32, concrete: []const u8, want: []const u8) LowerError!u32 {
    if (std.mem.eql(u8, concrete, want)) return slot;
    if (ctx.ifaces.contains(want)) {
        return try coerceToIface(ctx, slot, concrete, want);
    }
    return slot;
}

fn coerceToIface(ctx: *Ctx, data_slot: u32, concrete: []const u8, iface_name: []const u8) LowerError!u32 {
    const iface = ctx.ifaces.get(iface_name) orelse return error.UnknownName;
    if (iface.methods.len == 0) return data_slot;

    const map_key = try std.fmt.allocPrint(ctx.a, "{s}>{s}", .{ concrete, iface_name });
    const vt_index: u32 = blk: {
        if (ctx.vtable_map.get(map_key)) |i| break :blk i;
        var mnames: std.ArrayListUnmanaged([]const u8) = .{};
        for (iface.methods) |ms| {
            const key = try std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ concrete, ms.name });
            const mi = ctx.methods.get(key) orelse return error.UnknownName;
            try mnames.append(ctx.a, mi.mangled);
        }
        const idx: u32 = @intCast(ctx.vtables.items.len);
        try ctx.vtables.append(ctx.a, .{
            .symbol = try std.fmt.allocPrint(ctx.a, "jog_vt_{d}", .{idx}),
            .methods = try mnames.toOwnedSlice(ctx.a),
        });
        try ctx.vtable_map.put(ctx.a, map_key, idx);
        break :blk idx;
    };

    const sz = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = 16 });
    const alloc_id = ctx.callee_map.get("jogrt_alloc").?;
    const fat = try ctx.fresh();
    var allo = Inst{ .op = .call, .dst = fat, .aux = alloc_id, .arity = 1 };
    allo.args[0] = sz;
    try ctx.emit(allo);

    try ctx.emit(.{ .op = .store, .src0 = fat, .src1 = data_slot });
    const vt = try ctx.fresh();
    try ctx.emit(.{ .op = .vtaddr, .dst = vt, .aux = vt_index });
    const eight = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
    const slot1 = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = slot1, .src0 = fat, .src1 = eight });
    try ctx.emit(.{ .op = .store, .src0 = slot1, .src1 = vt });
    return fat;
}

fn lowerMethodOrIfaceCall(
    ctx: *Ctx,
    base_expr: *ast.Expr,
    base_slot: u32,
    method: []const u8,
    args: []*ast.Expr,
    with_args: []*ast.Expr,
) LowerError!u32 {
    _ = with_args;
    const concrete = inferTypeNameFromExpr(ctx, base_expr) orelse
        (if (base_expr.* == .ident) (ctx.meta.get(base_expr.ident) orelse SlotMeta{ .kind = .scalar }).type_name else "");

    // iface value?
    if (base_expr.* == .ident) {
        if (ctx.meta.get(base_expr.ident)) |m| {
            if (m.kind == .iface_ptr) {
                return try lowerIfaceMethod(ctx, base_slot, method, args);
            }
        }
    }
    if (ctx.ifaces.contains(concrete)) {
        return try lowerIfaceMethod(ctx, base_slot, method, args);
    }

    const key = try std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ concrete, method });
    const mi = ctx.methods.get(key) orelse return error.UnknownName;
    const cid = ctx.callee_map.get(mi.mangled) orelse return error.UnknownName;

    var arg_slots: [8]u32 = .{none} ** 8;
    var arity: u8 = 0;
    // contexts on methods (rare in demos)
    for (mi.contexts) |cx| {
        const want = typeExprName(cx.ty);
        const amb = findAmbient(ctx, want) orelse return error.UnknownName;
        arg_slots[arity] = try maybeCoerceCtx(ctx, amb.slot, amb.type_name, want);
        arity += 1;
    }
    arg_slots[arity] = base_slot;
    arity += 1;
    if (args.len + arity > 8) return error.TooManyArgs;
    for (args) |arg| {
        arg_slots[arity] = try lowerExpr(ctx, arg);
        arity += 1;
    }
    const d = try ctx.fresh();
    try ctx.emit(.{
        .op = .call,
        .dst = d,
        .aux = @intCast(cid),
        .arity = arity,
        .args = arg_slots,
    });
    return d;
}

fn lowerIfaceMethod(ctx: *Ctx, fat: u32, method: []const u8, args: []*ast.Expr) LowerError!u32 {
    var method_index: i64 = -1;
    var it = ctx.ifaces.iterator();
    while (it.next()) |e| {
        for (e.value_ptr.methods, 0..) |ms, i| {
            if (std.mem.eql(u8, ms.name, method)) {
                method_index = @intCast(i);
                break;
            }
        }
        if (method_index >= 0) break;
    }
    if (method_index < 0) return error.UnknownName;

    const data = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = data, .src0 = fat });
    const eight = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
    const vt_slot = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = vt_slot, .src0 = fat, .src1 = eight });
    const vt = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = vt, .src0 = vt_slot });
    const off = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = off, .aux = method_index * 8 });
    const fpp = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = fpp, .src0 = vt, .src1 = off });
    const fnptr = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = fnptr, .src0 = fpp });

    var arg_slots: [8]u32 = .{none} ** 8;
    arg_slots[0] = data;
    var arity: u8 = 1;
    if (args.len + 1 > 8) return error.TooManyArgs;
    for (args) |arg| {
        arg_slots[arity] = try lowerExpr(ctx, arg);
        arity += 1;
    }
    const d = try ctx.fresh();
    try ctx.emit(.{
        .op = .icall,
        .dst = d,
        .src0 = fnptr,
        .arity = arity,
        .args = arg_slots,
    });
    return d;
}

fn lowerPrintArgs(ctx: *Ctx, e: *ast.Expr) LowerError!struct { u32, u32 } {
    if (e.* == .string) {
        const idx: i64 = @intCast(ctx.strings.items.len);
        try ctx.strings.append(ctx.a, try ctx.a.dupe(u8, e.string));
        const ptr = try ctx.fresh();
        const len = try ctx.fresh();
        try ctx.emit(.{ .op = .saddr, .dst = ptr, .aux = idx });
        try ctx.emit(.{ .op = .slen, .dst = len, .aux = idx });
        return .{ ptr, len };
    }
    const ptr = try lowerExpr(ctx, e);
    const len = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = len, .aux = 0 });
    return .{ ptr, len };
}
