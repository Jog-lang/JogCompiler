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
    eq,
    ne,
    lt,
    neg,
    call,
    ret,
    jmp,
    jz,
    panic,
};

pub const none: u32 = std.math.maxInt(u32);

pub const Inst = struct {
    op: Op,
    dst: u32 = none,
    src0: u32 = none,
    src1: u32 = none,
    aux: i64 = 0,
    arity: u8 = 0,
    args: [6]u32 = .{none} ** 6,
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
    needs_libc: bool,
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *Program) void {
        self.arena.deinit();
    }
};

const LowerError = error{ UnknownName, OutOfMemory, TooManyArgs };

pub fn lower(parent: std.mem.Allocator, file: *const ast.File, cr: *const check.CheckResult) !Program {
    _ = cr;
    var program: Program = .{
        .strings = &[_][]const u8{},
        .callees = &[_]Callee{},
        .funcs = &[_]Func{},
        .needs_libc = false,
        .arena = std.heap.ArenaAllocator.init(parent),
    };
    errdefer program.deinit();
    const a = program.arena.allocator();

    var strings: std.ArrayListUnmanaged([]const u8) = .{};
    var callees: std.ArrayListUnmanaged(Callee) = .{};
    var callee_map: std.StringHashMapUnmanaged(u32) = .{};
    var extern_names: std.StringHashMapUnmanaged(void) = .{};

    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_print" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_panic" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_alloc" });

    for (file.decls) |d| {
        switch (d) {
            .extern_block => |eb| {
                for (eb.fns) |ef| {
                    try extern_names.put(a, ef.name, {});
                    try internCallee(a, &callees, &callee_map, .{ .kind = .extern_c, .name = ef.name });
                    program.needs_libc = true;
                }
            },
            .fn_decl => |fd| {
                if (fd.body != null) {
                    try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = fd.name });
                }
            },
            else => {},
        }
    }

    var funcs: std.ArrayListUnmanaged(Func) = .{};
    for (file.decls) |d| {
        if (d == .fn_decl and d.fn_decl.body != null) {
            const fd = d.fn_decl;
            const f = try lowerFn(a, fd, fd.body.?, &strings, &callee_map);
            try funcs.append(a, f);
        }
    }

    program.strings = try strings.toOwnedSlice(a);
    program.callees = try callees.toOwnedSlice(a);
    program.funcs = try funcs.toOwnedSlice(a);
    return program;
}

fn internCallee(a: std.mem.Allocator, list: *std.ArrayListUnmanaged(Callee), map: *std.StringHashMapUnmanaged(u32), c: Callee) !void {
    if (map.contains(c.name)) return;
    try list.append(a, c);
    try map.put(a, c.name, @intCast(list.items.len - 1));
}

const Ctx = struct {
    a: std.mem.Allocator,
    strings: *std.ArrayListUnmanaged([]const u8),
    callee_map: *std.StringHashMapUnmanaged(u32),
    slots: *std.StringHashMapUnmanaged(u32),
    next_slot: *u32,
    insts: *std.ArrayListUnmanaged(Inst),

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
    fd: ast.FnDecl,
    body: *ast.Block,
    strings: *std.ArrayListUnmanaged([]const u8),
    callee_map: *std.StringHashMapUnmanaged(u32),
) !Func {
    var slots: std.StringHashMapUnmanaged(u32) = .{};
    var next_slot: u32 = 0;
    for (fd.params) |p| {
        try slots.put(a, p.name, next_slot);
        next_slot += 1;
    }
    const nparams = next_slot;
    var insts: std.ArrayListUnmanaged(Inst) = .{};
    var ctx = Ctx{
        .a = a,
        .strings = strings,
        .callee_map = callee_map,
        .slots = &slots,
        .next_slot = &next_slot,
        .insts = &insts,
    };
    try lowerBlock(&ctx, body);
    if (insts.items.len == 0 or insts.items[insts.items.len - 1].op != .ret) {
        try ctx.emit(.{ .op = .ret, .src0 = none });
    }
    return .{
        .name = fd.name,
        .nparams = nparams,
        .nlocals = next_slot,
        .exported = fd.exported or std.mem.eql(u8, fd.name, "main"),
        .insts = try insts.toOwnedSlice(a),
    };
}

fn lowerBlock(ctx: *Ctx, block: *ast.Block) LowerError!void {
    for (block.stmts) |stmt| try lowerStmt(ctx, stmt);
}

fn lowerStmt(ctx: *Ctx, stmt: ast.Stmt) LowerError!void {
    switch (stmt) {
        .var_decl => |vd| {
            const slot = try ctx.fresh();
            try ctx.slots.put(ctx.a, vd.name, slot);
            if (vd.init) |init| {
                const v = try lowerExpr(ctx, init);
                try ctx.emit(.{ .op = .copy, .dst = slot, .src0 = v });
            } else {
                try ctx.emit(.{ .op = .iconst, .dst = slot, .aux = 0 });
            }
        },
        .assign => |as| {
            const slot = ctx.slots.get(as.name) orelse return error.UnknownName;
            const v = try lowerExpr(ctx, as.value);
            try ctx.emit(.{ .op = .copy, .dst = slot, .src0 = v });
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
            if (ifs.else_block) |eb| {
                ctx.insts.items[jz_i].aux = else_pos;
                try lowerBlock(ctx, eb);
            } else {
                ctx.insts.items[jz_i].aux = else_pos;
            }
            const end_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jmp_i].aux = end_pos;
            if (ifs.else_block == null) {
                ctx.insts.items[jz_i].aux = end_pos;
            }
        },
        .with_stmt => |ws| {
            for (ws.values) |v| _ = try lowerExpr(ctx, v);
            try lowerBlock(ctx, ws.body);
        },
    }
}

fn lowerExpr(ctx: *Ctx, e: *ast.Expr) LowerError!u32 {
    switch (e.*) {
        .ident => |name| return ctx.slots.get(name) orelse error.UnknownName,
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
            const l = try lowerExpr(ctx, b.left);
            const r = try lowerExpr(ctx, b.right);
            const d = try ctx.fresh();
            switch (b.op) {
                .add => try ctx.emit(.{ .op = .add, .dst = d, .src0 = l, .src1 = r }),
                .sub => try ctx.emit(.{ .op = .sub, .dst = d, .src0 = l, .src1 = r }),
                .eq => try ctx.emit(.{ .op = .eq, .dst = d, .src0 = l, .src1 = r }),
                .neq => try ctx.emit(.{ .op = .ne, .dst = d, .src0 = l, .src1 = r }),
                .lt => try ctx.emit(.{ .op = .lt, .dst = d, .src0 = l, .src1 = r }),
                .gt => try ctx.emit(.{ .op = .lt, .dst = d, .src0 = r, .src1 = l }),
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
                .addr, .deref => try ctx.emit(.{ .op = .copy, .dst = d, .src0 = v }),
            }
            return d;
        },
        .call => |c| {
            if (c.callee.* != .ident) return error.UnknownName;
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
            const cid = ctx.callee_map.get(name) orelse return error.UnknownName;
            if (c.args.len > 6) return error.TooManyArgs;
            var arg_slots: [6]u32 = .{none} ** 6;
            for (c.args, 0..) |arg, i| arg_slots[i] = try lowerExpr(ctx, arg);
            const d = try ctx.fresh();
            try ctx.emit(.{
                .op = .call,
                .dst = d,
                .aux = @intCast(cid),
                .arity = @intCast(c.args.len),
                .args = arg_slots,
            });
            return d;
        },
        .cast => |c| return try lowerExpr(ctx, c.expr),
        .panic_call => |p| {
            const ptr, const len = try lowerPrintArgs(ctx, p.message);
            try ctx.emit(.{ .op = .panic, .src0 = ptr, .src1 = len });
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        },
        .field, .optional_call, .struct_lit => {
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        },
    }
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
