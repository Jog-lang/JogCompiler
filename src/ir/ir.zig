const std = @import("std");
const ast = @import("../ast/ast.zig");
const check = @import("../check/check.zig");

/// Linear IR per function. Slots are i64/pointer-sized. jmp/jz targets are absolute instruction indices.
pub const Op = enum {
    iconst,
    fconst,
    saddr,
    slen,
    copy,
    add,
    sub,
    mul,
    div,
    rem,
    fadd,
    fsub,
    fmul,
    fdiv,
    /// i64 → f64 (cvtsi2sd).
    sitofp,
    /// f64 → i64 trunc toward zero (cvttsd2si).
    fptosi,
    load8,
    store8,
    eq,
    ne,
    lt,
    le,
    and_op,
    or_op,
    bit_and,
    bit_or,
    bit_xor,
    shl,
    shr,
    neg,
    bit_not,
    load,
    store,
    lea_slot,
    narrow,
    call,
    icall,
    vtaddr,
    /// Load package-level `var` (aux = global index).
    gload,
    /// Store package-level `var` (aux = global index, src0 = value).
    gstore,
    /// Address of package global / object storage (aux = global index) — lea, not load.
    gaddr,
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
    /// SysV float return (xmm0) — set for `extern` f32/f64 returns.
    float_ret: bool = false,
    /// Bit i set ⇒ argument i is f32/f64 (xmm classification for `extern`).
    float_args: u8 = 0,
    /// Bit i set ⇒ argument i is i64/u64 (wasm32 needs real i64, not intptr_t).
    wide_args: u8 = 0,
    /// `extern` return is i64/u64 (wasm32 i64, not intptr_t).
    wide_ret: bool = false,
    /// `extern` with no return type — wasm32 void vs i32 signature.
    void_ret: bool = false,
};

pub const Global = struct {
    name: []const u8,
    mutable: bool,
    init_bits: i64 = 0,
    is_float: bool = false,
    /// Number of 8-byte slots in static storage (1 for scalar `var`; field count for `object`).
    nquads: u32 = 1,
    /// `object Name` singleton — loads of the bare name use `gaddr`.
    is_object: bool = false,
};

pub const Program = struct {
    strings: [][]const u8,
    callees: []Callee,
    funcs: []Func,
    vtables: []VTable,
    globals: []Global,
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

const ConstVal = struct {
    bits: i64,
    is_float: bool,
};

fn isFloatTypeExpr(te: *ast.TypeExpr) bool {
    return switch (te.*) {
        .name => |n| std.mem.eql(u8, n, "f64") or std.mem.eql(u8, n, "f32"),
        else => false,
    };
}

fn floatArgMask(params: []ast.Param) u8 {
    var mask: u8 = 0;
    for (params, 0..) |p, i| {
        if (i >= 8) break;
        if (isFloatTypeExpr(p.ty)) mask |= @as(u8, 1) << @intCast(i);
    }
    return mask;
}

fn isWideIntTypeExpr(te: *ast.TypeExpr) bool {
    return switch (te.*) {
        .name => |n| std.mem.eql(u8, n, "i64") or std.mem.eql(u8, n, "u64"),
        else => false,
    };
}

fn wideArgMask(params: []ast.Param) u8 {
    var mask: u8 = 0;
    for (params, 0..) |p, i| {
        if (i >= 8) break;
        if (isWideIntTypeExpr(p.ty)) mask |= @as(u8, 1) << @intCast(i);
    }
    return mask;
}

fn literalConstBits(e: *ast.Expr) ?ConstVal {
    return switch (e.*) {
        .int => |n| .{ .bits = n, .is_float = false },
        .float => |n| .{ .bits = @bitCast(n), .is_float = true },
        .unary => |u| blk: {
            if (u.op == .neg) {
                if (literalConstBits(u.expr)) |inner| {
                    if (inner.is_float) {
                        const f: f64 = @bitCast(inner.bits);
                        break :blk .{ .bits = @bitCast(-f), .is_float = true };
                    }
                    break :blk .{ .bits = -inner.bits, .is_float = false };
                }
            }
            break :blk null;
        },
        else => null,
    };
}

const LowerError = error{ UnknownName, OutOfMemory, TooManyArgs, TypeError };

const StructInfo = struct {
    fields: []ast.Field,
};

const EnumInfo = struct {
    variants: []EnumVariantVal,
};

const EnumVariantVal = struct {
    name: []const u8,
    value: i64,
};

const DeferItem = struct {
    kind: enum { expr, block },
    expr: ?*ast.Expr = null,
    block: ?*ast.Block = null,
    /// errdefer: run only when returning a non-null / non-zero error-ish value.
    on_error_only: bool = false,
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

const SlotKind = enum { scalar, struct_ptr, iface_ptr, opt_ptr, raw_ptr, array_ptr };

const SlotMeta = struct {
    kind: SlotKind,
    type_name: []const u8 = "",
    /// Element count when `kind == .array_ptr`.
    array_len: u64 = 0,
    /// Element stride in bytes (1 for byte/u8, 8 otherwise).
    elem_size: u32 = 8,
};

pub fn lower(parent: std.mem.Allocator, file: *const ast.File, cr: *const check.CheckResult) !Program {
    _ = cr;
    var program: Program = .{
        .strings = &[_][]const u8{},
        .callees = &[_]Callee{},
        .funcs = &[_]Func{},
        .vtables = &[_]VTable{},
        .globals = &[_]Global{},
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
    var enums: std.StringHashMapUnmanaged(EnumInfo) = .{};
    var methods: std.StringHashMapUnmanaged(MethodInfo) = .{}; // "Type.method"
    var ifaces: std.StringHashMapUnmanaged(IfaceInfo) = .{};
    var fn_infos: std.StringHashMapUnmanaged(FnInfo) = .{};
    var vtables: std.ArrayListUnmanaged(VTable) = .{};
    var vtable_map: std.StringHashMapUnmanaged(u32) = .{};
    var globals: std.ArrayListUnmanaged(Global) = .{};
    var global_map: std.StringHashMapUnmanaged(u32) = .{};
    var object_map: std.StringHashMapUnmanaged(u32) = .{}; // object name → global index
    var const_vals: std.StringHashMapUnmanaged(ConstVal) = .{};

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
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_host_run_loop" });

    var generic_fns: std.StringHashMapUnmanaged(ast.FnDecl) = .{};
    var generic_structs: std.StringHashMapUnmanaged(ast.StructDecl) = .{};

    for (file.decls) |d| {
        switch (d) {
            .struct_decl => |sd| {
                if (sd.type_params.len > 0) {
                    try generic_structs.put(a, sd.name, sd);
                } else {
                    try structs.put(a, sd.name, .{ .fields = sd.fields });
                    if (sd.is_object) {
                        // One static multi-slot instance (no heap). Zero-init BSS-like .data.
                        const nquads: u32 = @intCast(@max(sd.fields.len, 1));
                        const idx: u32 = @intCast(globals.items.len);
                        try globals.append(a, .{
                            .name = try a.dupe(u8, sd.name),
                            .mutable = true,
                            .init_bits = 0,
                            .nquads = nquads,
                            .is_object = true,
                        });
                        try global_map.put(a, sd.name, idx);
                        try object_map.put(a, sd.name, idx);
                    }
                }
            },
            .enum_decl => |ed| {
                var variants: std.ArrayListUnmanaged(EnumVariantVal) = .{};
                var next: i64 = 0;
                for (ed.variants) |v| {
                    const val = v.value orelse next;
                    try variants.append(a, .{ .name = v.name, .value = val });
                    next = val + 1;
                }
                try enums.put(a, ed.name, .{ .variants = try variants.toOwnedSlice(a) });
            },
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
                    const fret = if (ef.ret) |r| isFloatTypeExpr(r) else false;
                    const wret = if (ef.ret) |r| isWideIntTypeExpr(r) else false;
                    try internCallee(a, &callees, &callee_map, .{
                        .kind = .extern_c,
                        .name = ef.name,
                        .float_ret = fret,
                        .float_args = floatArgMask(ef.params),
                        .wide_args = wideArgMask(ef.params),
                        .wide_ret = wret,
                        .void_ret = ef.ret == null,
                    });
                    var ret_name: []const u8 = "";
                    if (ef.ret) |r| ret_name = try resolveRetName(a, &structs, r);
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
            .global_decl => |gd| {
                var bits: i64 = 0;
                var is_float = false;
                if (gd.init) |init| {
                    if (literalConstBits(init)) |cv| {
                        bits = cv.bits;
                        is_float = cv.is_float;
                    } else if (init.* == .null_lit) {
                        bits = 0;
                    } else {
                        std.debug.print("lower: top-level {s} '{s}' needs a literal/null init\n", .{ if (gd.mutable) "var" else "val", gd.name });
                        return error.TypeError;
                    }
                }
                if (gd.ty) |te| {
                    if (isFloatTypeExpr(te)) is_float = true;
                }
                if (!gd.mutable) {
                    // Compile-time constant: fold at use sites (no storage).
                    try const_vals.put(a, gd.name, .{ .bits = bits, .is_float = is_float });
                } else {
                    // Mutable: zero-init OK when no initializer (BSS-like .data zero).
                    const idx: u32 = @intCast(globals.items.len);
                    try globals.append(a, .{
                        .name = try a.dupe(u8, gd.name),
                        .mutable = true,
                        .init_bits = bits,
                        .is_float = is_float,
                    });
                    try global_map.put(a, gd.name, idx);
                }
            },
            .fn_decl, .actual_fn => |fd| {
                if (fd.body != null and fd.type_params.len > 0) {
                    try generic_fns.put(a, fd.name, fd);
                } else if (fd.body != null) {
                    try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = fd.name });
                    var ret_name: []const u8 = "";
                    if (fd.ret) |r| ret_name = try resolveRetName(a, &structs, r);
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
                    if (ef.ret) |r| ret_name = try resolveRetName(a, &structs, r);
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
                    const sym = try opMethodSymbol(a, md.name);
                    const mangled = try std.fmt.allocPrint(a, "{s}_{s}", .{ md.type_name, sym });
                    const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ md.type_name, md.name });
                    try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = mangled });
                    var mret: []const u8 = "";
                    if (md.ret) |r| mret = try resolveRetName(a, &structs, r);
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

    // Instantiate generics referenced from the program (calls / struct lits / types).
    var specialized: std.ArrayListUnmanaged(ast.FnDecl) = .{};
    try instantiateGenerics(a, file, &generic_fns, &generic_structs, &structs, &fn_infos, &callees, &callee_map, &specialized);

    var funcs: std.ArrayListUnmanaged(Func) = .{};
    for (file.decls) |d| {
        const maybe_fd: ?ast.FnDecl = switch (d) {
            .fn_decl => |fd| if (fd.body != null and fd.type_params.len == 0) fd else null,
            .actual_fn => |fd| if (fd.body != null and fd.type_params.len == 0) fd else null,
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
            }, &strings, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &const_vals, &global_map, globals.items, &object_map);
            try funcs.append(a, f);
        } else if (d == .method and d.method.body != null) {
            const md = d.method;
            const sym = try opMethodSymbol(a, md.name);
            const mangled = try std.fmt.allocPrint(a, "{s}_{s}", .{ md.type_name, sym });
            const f = try lowerFn(a, .{
                .name = mangled,
                .params = md.params,
                .contexts = md.contexts,
                .body = md.body.?,
                .exported = true,
                .self_type = md.type_name,
                .pointer_recv = md.pointer_recv,
            }, &strings, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &const_vals, &global_map, globals.items, &object_map);
            try funcs.append(a, f);
        }
    }
    for (specialized.items) |fd| {
        const f = try lowerFn(a, .{
            .name = fd.name,
            .params = fd.params,
            .contexts = fd.contexts,
            .body = fd.body.?,
            .exported = false,
            .self_type = null,
            .pointer_recv = false,
        }, &strings, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &const_vals, &global_map, globals.items, &object_map);
        try funcs.append(a, f);
    }

    program.strings = try strings.toOwnedSlice(a);
    program.callees = try callees.toOwnedSlice(a);
    program.funcs = try funcs.toOwnedSlice(a);
    program.vtables = try vtables.toOwnedSlice(a);
    program.globals = try globals.toOwnedSlice(a);
    program.link_libs = try link_libs.toOwnedSlice(a);
    return program;
}

fn internCallee(a: std.mem.Allocator, list: *std.ArrayListUnmanaged(Callee), map: *std.StringHashMapUnmanaged(u32), c: Callee) !void {
    if (map.contains(c.name)) return;
    try list.append(a, c);
    try map.put(a, c.name, @intCast(list.items.len - 1));
}

fn resolveRetName(a: std.mem.Allocator, structs: *std.StringHashMapUnmanaged(StructInfo), te: *ast.TypeExpr) ![]const u8 {
    return switch (te.*) {
        .name => |n| n,
        .named_app => |na| try mangleNamedApp(a, na.name, na.args),
        .tuple => |elems| try ensureTupleStruct(a, structs, elems.len),
        .optional => |inner| try resolveRetName(a, structs, inner),
        .pointer, .pointer_const => |inner| try resolveRetName(a, structs, inner),
        .array => |arr| try resolveRetName(a, structs, arr.elem),
        else => "",
    };
}

fn substTypeExpr(a: std.mem.Allocator, te: *ast.TypeExpr, params: [][]const u8, args: []*ast.TypeExpr) !*ast.TypeExpr {
    switch (te.*) {
        .name => |n| {
            for (params, 0..) |p, i| {
                if (std.mem.eql(u8, p, n) and i < args.len) {
                    return try cloneTypeExpr(a, args[i]);
                }
            }
            return try cloneTypeExpr(a, te);
        },
        .named_app => |na| {
            var nargs: std.ArrayListUnmanaged(*ast.TypeExpr) = .{};
            for (na.args) |arg| try nargs.append(a, try substTypeExpr(a, arg, params, args));
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .named_app = .{ .name = na.name, .args = try nargs.toOwnedSlice(a) } };
            return out;
        },
        .tuple => |elems| {
            var ne: std.ArrayListUnmanaged(*ast.TypeExpr) = .{};
            for (elems) |el| try ne.append(a, try substTypeExpr(a, el, params, args));
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .tuple = try ne.toOwnedSlice(a) };
            return out;
        },
        .optional => |inner| {
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .optional = try substTypeExpr(a, inner, params, args) };
            return out;
        },
        .pointer => |inner| {
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .pointer = try substTypeExpr(a, inner, params, args) };
            return out;
        },
        .pointer_const => |inner| {
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .pointer_const = try substTypeExpr(a, inner, params, args) };
            return out;
        },
        .array => |arr| {
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .array = .{ .len = arr.len, .elem = try substTypeExpr(a, arr.elem, params, args) } };
            return out;
        },
        else => return try cloneTypeExpr(a, te),
    }
}

fn cloneTypeExpr(a: std.mem.Allocator, te: *ast.TypeExpr) !*ast.TypeExpr {
    const out = try a.create(ast.TypeExpr);
    out.* = te.*;
    return out;
}

fn ensureGenericStruct(
    a: std.mem.Allocator,
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    name: []const u8,
    args: []*ast.TypeExpr,
) ![]const u8 {
    const mangled = try mangleNamedApp(a, name, args);
    if (structs.contains(mangled)) return mangled;
    const tmpl = generic_structs.get(name) orelse return mangled;
    if (args.len != tmpl.type_params.len) return mangled;
    var fields: std.ArrayListUnmanaged(ast.Field) = .{};
    for (tmpl.fields) |f| {
        try fields.append(a, .{
            .name = f.name,
            .ty = try substTypeExpr(a, f.ty, tmpl.type_params, args),
        });
    }
    try structs.put(a, mangled, .{ .fields = try fields.toOwnedSlice(a) });
    return mangled;
}

fn ensureGenericFn(
    a: std.mem.Allocator,
    generic_fns: *std.StringHashMapUnmanaged(ast.FnDecl),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(ast.FnDecl),
    name: []const u8,
    args: []*ast.TypeExpr,
) !void {
    const mangled = try mangleNamedApp(a, name, args);
    if (fn_infos.contains(mangled)) return;
    const tmpl = generic_fns.get(name) orelse return;
    if (args.len != tmpl.type_params.len) return;
    var params: std.ArrayListUnmanaged(ast.Param) = .{};
    for (tmpl.params) |p| {
        try params.append(a, .{
            .name = p.name,
            .ty = try substTypeExpr(a, p.ty, tmpl.type_params, args),
        });
    }
    var ret: ?*ast.TypeExpr = null;
    if (tmpl.ret) |r| ret = try substTypeExpr(a, r, tmpl.type_params, args);
    const fd = ast.FnDecl{
        .name = mangled,
        .type_params = &.{},
        .params = try params.toOwnedSlice(a),
        .contexts = tmpl.contexts,
        .ret = ret,
        .body = tmpl.body,
        .exported = false,
    };
    try specialized.append(a, fd);
    try internCallee(a, callees, callee_map, .{ .kind = .jog_fn, .name = mangled });
    var ret_name: []const u8 = "";
    if (ret) |r| ret_name = try resolveRetName(a, structs, r);
    try fn_infos.put(a, mangled, .{
        .mangled = mangled,
        .nparams = @intCast(fd.params.len),
        .contexts = fd.contexts,
        .exported = false,
        .ret_name = ret_name,
    });
    // Also alias under call lookup: lowerCall will mangle at site.
}

fn walkExprInstantiate(
    a: std.mem.Allocator,
    e: *ast.Expr,
    generic_fns: *std.StringHashMapUnmanaged(ast.FnDecl),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(ast.FnDecl),
) !void {
    switch (e.*) {
        .call => |c| {
            if (c.callee.* == .ident) {
                if (c.type_args.len > 0) {
                    try ensureGenericFn(a, generic_fns, structs, fn_infos, callees, callee_map, specialized, c.callee.ident, c.type_args);
                } else if (c.args.len >= 1 and generic_fns.contains(c.callee.ident)) {
                    // Infer T from first arg: int → i64, else type name.
                    const atn = blk: {
                        if (c.args[0].* == .int) break :blk "i64";
                        if (c.args[0].* == .float) break :blk "f64";
                        if (c.args[0].* == .string) break :blk "str";
                        break :blk "i64";
                    };
                    const ty = try a.create(ast.TypeExpr);
                    ty.* = .{ .name = atn };
                    var args = try a.alloc(*ast.TypeExpr, 1);
                    args[0] = ty;
                    try ensureGenericFn(a, generic_fns, structs, fn_infos, callees, callee_map, specialized, c.callee.ident, args);
                }
            }
            for (c.args) |arg| try walkExprInstantiate(a, arg, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            for (c.with_args) |w| try walkExprInstantiate(a, w, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            try walkExprInstantiate(a, c.callee, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .struct_lit => |sl| {
            if (sl.type_args.len > 0) {
                _ = try ensureGenericStruct(a, generic_structs, structs, sl.type_name, sl.type_args);
            }
            for (sl.fields) |f| try walkExprInstantiate(a, f.value, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .tuple => |elems| {
            _ = try ensureTupleStruct(a, structs, elems.len);
            for (elems) |el| try walkExprInstantiate(a, el, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .binary => |b| {
            try walkExprInstantiate(a, b.left, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            try walkExprInstantiate(a, b.right, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .unary => |u| try walkExprInstantiate(a, u.expr, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
        .field => |f| try walkExprInstantiate(a, f.base, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
        .index => |ix| {
            try walkExprInstantiate(a, ix.base, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            try walkExprInstantiate(a, ix.index, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .cast => |c| {
            try walkTypeInstantiate(a, c.ty, generic_structs, structs);
            try walkExprInstantiate(a, c.expr, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .when_expr => |w| {
            try walkExprInstantiate(a, w.scrutinee, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            for (w.arms) |arm| try walkExprInstantiate(a, arm.body, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            if (w.else_body) |eb| try walkExprInstantiate(a, eb, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .optional_call => |oc| {
            try walkExprInstantiate(a, oc.base, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            for (oc.args) |arg| try walkExprInstantiate(a, arg, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .panic_call => |p| try walkExprInstantiate(a, p.message, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
        .range => |r| {
            try walkExprInstantiate(a, r.start, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            try walkExprInstantiate(a, r.end, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .array_lit => |al| {
            for (al.elems) |el| try walkExprInstantiate(a, el, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .interp => |ip| {
            for (ip.parts) |p| switch (p) {
                .text => {},
                .expr => |ex| try walkExprInstantiate(a, ex, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
            };
        },
        .try_cast => |c| {
            try walkTypeInstantiate(a, c.ty, generic_structs, structs);
            try walkExprInstantiate(a, c.expr, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .is_type => |it| {
            try walkTypeInstantiate(a, it.ty, generic_structs, structs);
            try walkExprInstantiate(a, it.expr, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        else => {},
    }
}

fn walkTypeInstantiate(a: std.mem.Allocator, te: *ast.TypeExpr, generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl), structs: *std.StringHashMapUnmanaged(StructInfo)) !void {
    switch (te.*) {
        .named_app => |na| _ = try ensureGenericStruct(a, generic_structs, structs, na.name, na.args),
        .tuple => |elems| _ = try ensureTupleStruct(a, structs, elems.len),
        .optional, .pointer, .pointer_const => |inner| try walkTypeInstantiate(a, inner, generic_structs, structs),
        .array => |arr| try walkTypeInstantiate(a, arr.elem, generic_structs, structs),
        else => {},
    }
}

fn walkStmtInstantiate(
    a: std.mem.Allocator,
    stmt: ast.Stmt,
    generic_fns: *std.StringHashMapUnmanaged(ast.FnDecl),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(ast.FnDecl),
) !void {
    switch (stmt) {
        .var_decl => |vd| {
            if (vd.ty) |t| try walkTypeInstantiate(a, t, generic_structs, structs);
            if (vd.init) |init| try walkExprInstantiate(a, init, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .destructure => |ds| try walkExprInstantiate(a, ds.init, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
        .assign => |as| {
            try walkExprInstantiate(a, as.value, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            if (as.field) |fa| try walkExprInstantiate(a, fa.base, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            if (as.index) |ix| {
                try walkExprInstantiate(a, ix.base, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
                try walkExprInstantiate(a, ix.index, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            }
        },
        .expr => |e| try walkExprInstantiate(a, e, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
        .return_stmt => |re| if (re) |e| try walkExprInstantiate(a, e, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
        .if_stmt => |ifs| {
            try walkExprInstantiate(a, ifs.cond, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            for (ifs.then_block.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            if (ifs.else_block) |eb| for (eb.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .while_stmt => |ws| {
            try walkExprInstantiate(a, ws.cond, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            for (ws.body.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .with_stmt => |ws| {
            for (ws.values) |v| try walkExprInstantiate(a, v, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            for (ws.body.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        .defer_stmt, .errdefer_stmt => |ds| switch (ds) {
            .expr => |ex| try walkExprInstantiate(a, ex, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
            .block => |b| for (b.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized),
        },
        .for_in_stmt => |fs| {
            try walkExprInstantiate(a, fs.iter, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
            for (fs.body.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
        },
        else => {},
    }
}

fn instantiateGenerics(
    a: std.mem.Allocator,
    file: *const ast.File,
    generic_fns: *std.StringHashMapUnmanaged(ast.FnDecl),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(ast.FnDecl),
) !void {
    for (file.decls) |d| {
        switch (d) {
            .fn_decl, .actual_fn => |fd| {
                if (fd.ret) |r| try walkTypeInstantiate(a, r, generic_structs, structs);
                for (fd.params) |p| try walkTypeInstantiate(a, p.ty, generic_structs, structs);
                if (fd.body) |body| {
                    for (body.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
                }
            },
            .method => |md| {
                if (md.ret) |r| try walkTypeInstantiate(a, r, generic_structs, structs);
                if (md.body) |body| {
                    for (body.stmts) |s| try walkStmtInstantiate(a, s, generic_fns, generic_structs, structs, fn_infos, callees, callee_map, specialized);
                }
            },
            else => {},
        }
    }
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
    enums: *std.StringHashMapUnmanaged(EnumInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    vtables: *std.ArrayListUnmanaged(VTable),
    vtable_map: *std.StringHashMapUnmanaged(u32),
    const_vals: *std.StringHashMapUnmanaged(ConstVal),
    global_map: *std.StringHashMapUnmanaged(u32),
    /// Package `var` storage (for floatish gload typing).
    globals: []const Global,
    object_map: *std.StringHashMapUnmanaged(u32),
    slots: *std.StringHashMapUnmanaged(u32),
    meta: *std.StringHashMapUnmanaged(SlotMeta),
    next_slot: *u32,
    insts: *std.ArrayListUnmanaged(Inst),
    ambient: *std.ArrayListUnmanaged(Ambient),
    /// Indices of `jmp` insts from `break` in the innermost while; patched to loop end.
    break_jmps: *std.ArrayListUnmanaged(usize),
    /// Indices of `jmp` insts from `continue`; patched to loop head.
    continue_jmps: *std.ArrayListUnmanaged(usize),
    defer_stack: *std.ArrayListUnmanaged(std.ArrayListUnmanaged(DeferItem)),

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
    enums: *std.StringHashMapUnmanaged(EnumInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    vtables: *std.ArrayListUnmanaged(VTable),
    vtable_map: *std.StringHashMapUnmanaged(u32),
    const_vals: *std.StringHashMapUnmanaged(ConstVal),
    global_map: *std.StringHashMapUnmanaged(u32),
    globals: []const Global,
    object_map: *std.StringHashMapUnmanaged(u32),
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
        try meta.put(a, p.name, try metaForTypeExpr(a, p.ty, structs, ifaces));
    }
    const nparams = next_slot;

    var insts: std.ArrayListUnmanaged(Inst) = .{};
    var break_jmps: std.ArrayListUnmanaged(usize) = .{};
    var continue_jmps: std.ArrayListUnmanaged(usize) = .{};
    var defer_stack: std.ArrayListUnmanaged(std.ArrayListUnmanaged(DeferItem)) = .{};
    var ctx = Ctx{
        .a = a,
        .strings = strings,
        .callee_map = callee_map,
        .structs = structs,
        .enums = enums,
        .methods = methods,
        .ifaces = ifaces,
        .fn_infos = fn_infos,
        .vtables = vtables,
        .vtable_map = vtable_map,
        .const_vals = const_vals,
        .global_map = global_map,
        .globals = globals,
        .object_map = object_map,
        .slots = &slots,
        .meta = &meta,
        .next_slot = &next_slot,
        .insts = &insts,
        .ambient = &ambient,
        .break_jmps = &break_jmps,
        .continue_jmps = &continue_jmps,
        .defer_stack = &defer_stack,
    };
    try lowerBlock(&ctx, fl.body);
    if (insts.items.len == 0 or insts.items[insts.items.len - 1].op != .ret) {
        try emitAllDefers(&ctx);
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
        .named_app => |na| na.name, // caller should prefer mangleNamedApp
        .pointer, .pointer_const, .optional => |inner| typeExprName(inner),
        .array => |arr| typeExprName(arr.elem),
        .tuple => "__Tup",
        else => "",
    };
}

fn mangleTypeArgName(te: *ast.TypeExpr) []const u8 {
    return switch (te.*) {
        .name => |n| n,
        .named_app => |na| na.name,
        .tuple => |elems| blk: {
            _ = elems;
            break :blk "tup";
        },
        .optional => |inner| mangleTypeArgName(inner),
        .pointer, .pointer_const => |inner| mangleTypeArgName(inner),
        .array => |arr| mangleTypeArgName(arr.elem),
        else => "X",
    };
}

fn mangleNamedApp(a: std.mem.Allocator, name: []const u8, args: []*ast.TypeExpr) ![]const u8 {
    var buf: std.ArrayListUnmanaged(u8) = .{};
    try buf.appendSlice(a, name);
    for (args) |arg| {
        try buf.appendSlice(a, "__");
        try buf.appendSlice(a, mangleTypeArgName(arg));
    }
    return try buf.toOwnedSlice(a);
}

fn tupleStructName(a: std.mem.Allocator, n: usize) ![]const u8 {
    return try std.fmt.allocPrint(a, "__Tup{d}", .{n});
}

fn ensureTupleStruct(a: std.mem.Allocator, structs: *std.StringHashMapUnmanaged(StructInfo), n: usize) ![]const u8 {
    const name = try tupleStructName(a, n);
    if (structs.contains(name)) return name;
    var fields: std.ArrayListUnmanaged(ast.Field) = .{};
    var i: usize = 0;
    while (i < n) : (i += 1) {
        const fname = try std.fmt.allocPrint(a, "_{d}", .{i});
        const ty = try a.create(ast.TypeExpr);
        ty.* = .{ .name = "i64" };
        try fields.append(a, .{ .name = fname, .ty = ty });
    }
    try structs.put(a, name, .{ .fields = try fields.toOwnedSlice(a) });
    return name;
}

fn opMethodSymbol(a: std.mem.Allocator, name: []const u8) ![]const u8 {
    // `op+` → `op_add` for ELF-safe symbols; method lookup key stays `Type.op+`.
    if (!std.mem.startsWith(u8, name, "op") or name.len < 3) return name;
    const rest = name[2..];
    const tag: []const u8 = if (std.mem.eql(u8, rest, "+"))
        "add"
    else if (std.mem.eql(u8, rest, "-"))
        "sub"
    else if (std.mem.eql(u8, rest, "*"))
        "mul"
    else if (std.mem.eql(u8, rest, "/"))
        "div"
    else if (std.mem.eql(u8, rest, "%"))
        "rem"
    else if (std.mem.eql(u8, rest, "=="))
        "eq"
    else if (std.mem.eql(u8, rest, "!="))
        "ne"
    else if (std.mem.eql(u8, rest, "<"))
        "lt"
    else if (std.mem.eql(u8, rest, ">"))
        "gt"
    else if (std.mem.eql(u8, rest, "<="))
        "le"
    else if (std.mem.eql(u8, rest, ">="))
        "ge"
    else if (std.mem.eql(u8, rest, "&"))
        "band"
    else if (std.mem.eql(u8, rest, "|"))
        "bor"
    else if (std.mem.eql(u8, rest, "^"))
        "bxor"
    else if (std.mem.eql(u8, rest, "<<"))
        "shl"
    else if (std.mem.eql(u8, rest, ">>"))
        "shr"
    else if (std.mem.eql(u8, rest, "~"))
        "bnot"
    else
        return try std.fmt.allocPrint(a, "op_{s}", .{rest});
    return try std.fmt.allocPrint(a, "op_{s}", .{tag});
}

fn binOpMethodName(op: ast.BinaryOp) ?[]const u8 {
    return switch (op) {
        .add => "op+",
        .sub => "op-",
        .mul => "op*",
        .div => "op/",
        .rem => "op%",
        .eq => "op==",
        .neq => "op!=",
        .lt => "op<",
        .gt => "op>",
        .lte => "op<=",
        .gte => "op>=",
        .bit_and => "op&",
        .bit_or => "op|",
        .bit_xor => "op^",
        .shl => "op<<",
        .shr => "op>>",
        else => null,
    };
}

fn metaForTypeName(name: []const u8, ifaces: *std.StringHashMapUnmanaged(IfaceInfo)) SlotMeta {
    if (ifaces.contains(name)) return .{ .kind = .iface_ptr, .type_name = name };
    if (name.len == 0) return .{ .kind = .scalar };
    // treat named non-iface as struct (or alias to scalar — scalar if builtin-looking)
    if (isBuiltinScalar(name)) return .{ .kind = .scalar, .type_name = name };
    return .{ .kind = .struct_ptr, .type_name = name };
}

fn metaForTypeExpr(a: std.mem.Allocator, te: *ast.TypeExpr, structs: *std.StringHashMapUnmanaged(StructInfo), ifaces: *std.StringHashMapUnmanaged(IfaceInfo)) !SlotMeta {
    switch (te.*) {
        .optional => return .{ .kind = .opt_ptr, .type_name = typeExprName(te) },
        .pointer, .pointer_const => {
            const inner = typeExprName(te);
            if (ifaces.contains(inner)) return .{ .kind = .iface_ptr, .type_name = inner };
            if (isBuiltinScalar(inner) or std.mem.eql(u8, inner, "byte") or std.mem.eql(u8, inner, "void"))
                return .{ .kind = .raw_ptr, .type_name = inner };
            return .{ .kind = .struct_ptr, .type_name = inner };
        },
        .array => |arr| {
            const en = typeExprName(arr.elem);
            const es: u32 = if (std.mem.eql(u8, en, "byte") or std.mem.eql(u8, en, "u8") or std.mem.eql(u8, en, "bool")) 1 else 8;
            return .{
                .kind = .array_ptr,
                .type_name = en,
                .array_len = arr.len,
                .elem_size = es,
            };
        },
        .name => |n| return metaForTypeName(n, ifaces),
        .named_app => |na| {
            const mangled = try mangleNamedApp(a, na.name, na.args);
            return metaForTypeName(mangled, ifaces);
        },
        .tuple => |elems| {
            const name = try ensureTupleStruct(a, structs, elems.len);
            return .{ .kind = .struct_ptr, .type_name = name };
        },
        .slice_view => |sv| {
            // Views are {ptr,len} — treat as raw_ptr to elem for index ops.
            const en = typeExprName(sv.elem);
            return .{ .kind = .raw_ptr, .type_name = en };
        },
        else => return .{ .kind = .scalar },
    }
}

fn isBuiltinScalar(name: []const u8) bool {
    const builtins = [_][]const u8{ "byte", "u8", "bool", "u16", "u32", "u64", "i32", "i64", "f32", "f64", "error", "usize", "str", "bytes", "void", "Dispatcher" };
    for (builtins) |b| if (std.mem.eql(u8, name, b)) return true;
    return false;
}

fn lowerBlock(ctx: *Ctx, block: *ast.Block) LowerError!void {
    try ctx.defer_stack.append(ctx.a, .{});
    for (block.stmts) |stmt| try lowerStmt(ctx, stmt);
    try emitCurrentDefers(ctx);
    _ = ctx.defer_stack.pop();
}

fn emitDeferItem(ctx: *Ctx, item: DeferItem) LowerError!void {
    if (item.kind == .expr) {
        if (item.expr) |ex| _ = try lowerExpr(ctx, ex);
    } else if (item.block) |b| {
        try lowerBlock(ctx, b);
    }
}

fn emitCurrentDefers(ctx: *Ctx) LowerError!void {
    if (ctx.defer_stack.items.len == 0) return;
    const frame = &ctx.defer_stack.items[ctx.defer_stack.items.len - 1];
    var i: usize = frame.items.len;
    while (i > 0) {
        i -= 1;
        // Scope-exit defers: skip errdefer (those only run on error return).
        if (!frame.items[i].on_error_only) try emitDeferItem(ctx, frame.items[i]);
    }
    // Keep errdefer items for return paths; clear normal ones.
    var kept: std.ArrayListUnmanaged(DeferItem) = .{};
    for (frame.items) |it| {
        if (it.on_error_only) try kept.append(ctx.a, it);
    }
    frame.clearRetainingCapacity();
    try frame.appendSlice(ctx.a, kept.items);
}

fn emitAllDefers(ctx: *Ctx) LowerError!void {
    try emitAllDefersFiltered(ctx, false);
}

fn emitAllDefersFiltered(ctx: *Ctx, error_path: bool) LowerError!void {
    var fi: usize = ctx.defer_stack.items.len;
    while (fi > 0) {
        fi -= 1;
        const frame = &ctx.defer_stack.items[fi];
        var i: usize = frame.items.len;
        while (i > 0) {
            i -= 1;
            const it = frame.items[i];
            if (it.on_error_only and !error_path) continue;
            if (!it.on_error_only and error_path) {
                // On error path still run normal defers too (LIFO with errdefer).
            }
            try emitDeferItem(ctx, it);
        }
    }
}

fn lowerStmt(ctx: *Ctx, stmt: ast.Stmt) LowerError!void {
    switch (stmt) {
        .destructure => |ds| {
            const tup = try lowerExpr(ctx, ds.init);
            const tn = blk: {
                if (inferMetaFromExpr(ctx, ds.init)) |m| break :blk m.type_name;
                break :blk try ensureTupleStruct(ctx.a, ctx.structs, ds.names.len);
            };
            _ = try ensureTupleStruct(ctx.a, ctx.structs, ds.names.len);
            const sd = ctx.structs.get(tn) orelse return error.UnknownName;
            for (ds.names, 0..) |n, i| {
                if (i >= sd.fields.len) return error.TypeError;
                const off: i64 = @intCast(i * 8);
                const addr = try ctx.fresh();
                const off_s = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = tup, .src1 = off_s });
                const slot = try ctx.fresh();
                try ctx.emit(.{ .op = .load, .dst = slot, .src0 = addr });
                try ctx.slots.put(ctx.a, n, slot);
                try ctx.meta.put(ctx.a, n, .{ .kind = .scalar, .type_name = "i64" });
            }
        },
        .var_decl => |vd| {
            var sm: SlotMeta = .{ .kind = .scalar };
            if (vd.ty) |t| sm = try metaForTypeExpr(ctx.a, t, ctx.structs, ctx.ifaces);
            if (sm.kind == .array_ptr and sm.array_len > 0) {
                const is_byte = std.mem.eql(u8, sm.type_name, "byte") or std.mem.eql(u8, sm.type_name, "u8") or std.mem.eql(u8, sm.type_name, "bool");
                if (is_byte) {
                    // Dense packed bytes: heap buffer, upward index, stride 1.
                    const sz = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = @intCast(sm.array_len) });
                    const cid = ctx.callee_map.get("jogrt_alloc").?;
                    const ptr = try ctx.fresh();
                    var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
                    allo.args[0] = sz;
                    try ctx.emit(allo);
                    var sm2 = sm;
                    sm2.elem_size = 1;
                    sm2.kind = .array_ptr;
                    try ctx.slots.put(ctx.a, vd.name, ptr);
                    try ctx.meta.put(ctx.a, vd.name, sm2);
                    if (vd.init) |init| {
                        const v = try lowerExpr(ctx, init);
                        try ctx.emit(.{ .op = .copy, .dst = ptr, .src0 = v });
                    }
                    return;
                }
                // One stack slot per element (8-byte frame slots).
                const first = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = first, .aux = 0 });
                var i: u64 = 1;
                while (i < sm.array_len) : (i += 1) {
                    const s = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = s, .aux = 0 });
                }
                const ptr = try ctx.fresh();
                try ctx.emit(.{ .op = .lea_slot, .dst = ptr, .src0 = first });
                var sm2 = sm;
                sm2.elem_size = 8;
                try ctx.slots.put(ctx.a, vd.name, ptr);
                try ctx.meta.put(ctx.a, vd.name, sm2);
                if (vd.init) |init| {
                    const v = try lowerExpr(ctx, init);
                    try ctx.emit(.{ .op = .copy, .dst = ptr, .src0 = v });
                }
                return;
            }
            const slot = try ctx.fresh();
            try ctx.slots.put(ctx.a, vd.name, slot);
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
            var v = try lowerExpr(ctx, as.value);
            if (as.compound) |op| {
                // Desugar `x += rhs` → `x = x + rhs` (and field/index forms).
                const cur = blk: {
                    if (as.index) |ix| {
                        const addr = try lowerIndexAddr(ctx, ix.base, ix.index);
                        const d = try ctx.fresh();
                        if (indexLoadStoreWidth(ctx, ix.base) == 1) {
                            try ctx.emit(.{ .op = .load8, .dst = d, .src0 = addr });
                        } else {
                            try ctx.emit(.{ .op = .load, .dst = d, .src0 = addr });
                        }
                        break :blk d;
                    } else if (as.field) |fa| {
                        const base = try lowerExpr(ctx, fa.base);
                        const off = try fieldOffset(ctx, fa.base, fa.name);
                        const addr = try ctx.fresh();
                        const off_s = try ctx.fresh();
                        try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                        try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = off_s });
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .load, .dst = d, .src0 = addr });
                        break :blk d;
                    } else if (ctx.slots.get(as.name)) |slot| {
                        break :blk slot;
                    } else if (ctx.global_map.get(as.name)) |gi| {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .gload, .dst = d, .aux = @intCast(gi) });
                        break :blk d;
                    } else {
                        return error.UnknownName;
                    }
                };
                const d = try ctx.fresh();
                const irop: Op = switch (op) {
                    .add => .add,
                    .sub => .sub,
                    .mul => .mul,
                    .div => .div,
                    .rem => .rem,
                    else => .add,
                };
                try ctx.emit(.{ .op = irop, .dst = d, .src0 = cur, .src1 = v });
                v = d;
            }
            if (as.index) |ix| {
                const addr = try lowerIndexAddr(ctx, ix.base, ix.index);
                try maybeBoundsCheck(ctx, ix.base, ix.index);
                if (indexLoadStoreWidth(ctx, ix.base) == 1) {
                    try ctx.emit(.{ .op = .store8, .src0 = addr, .src1 = v });
                } else {
                    try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = v });
                }
            } else if (as.field) |fa| {
                const base = try lowerExpr(ctx, fa.base);
                const off = try fieldOffset(ctx, fa.base, fa.name);
                const addr = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = addr, .aux = off });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = addr });
                try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = v });
            } else if (ctx.slots.get(as.name)) |slot| {
                try ctx.emit(.{ .op = .copy, .dst = slot, .src0 = v });
            } else if (ctx.global_map.get(as.name)) |gi| {
                try ctx.emit(.{ .op = .gstore, .src0 = v, .aux = @intCast(gi) });
            } else {
                std.debug.print("lower UnknownName assign: {s}\n", .{as.name});
                return error.UnknownName;
            }
        },
        .expr => |e| _ = try lowerExpr(ctx, e),
        .return_stmt => |re| {
            const ret_slot: u32 = if (re) |e| try lowerExpr(ctx, e) else none;
            // Heuristic error path: non-null return of optional/error.
            var error_path = false;
            if (re) |e| {
                if (e.* == .null_lit) {
                    error_path = false;
                } else if (e.* == .int and e.int != 0) {
                    error_path = true;
                } else if (e.* != .int) {
                    // Runtime: if ret_slot != 0 treat as error for errdefer.
                    error_path = true;
                    const jz_i = ctx.insts.items.len;
                    try ctx.emit(.{ .op = .jz, .src0 = ret_slot, .aux = 0 });
                    try emitAllDefersFiltered(ctx, true);
                    const jmp_i = ctx.insts.items.len;
                    try ctx.emit(.{ .op = .jmp, .aux = 0 });
                    const ok_pos: i64 = @intCast(ctx.insts.items.len);
                    ctx.insts.items[jz_i].aux = ok_pos;
                    try emitAllDefersFiltered(ctx, false);
                    const end_pos: i64 = @intCast(ctx.insts.items.len);
                    ctx.insts.items[jmp_i].aux = end_pos;
                    try ctx.emit(.{ .op = .ret, .src0 = ret_slot });
                    return;
                }
            }
            try emitAllDefersFiltered(ctx, error_path);
            try ctx.emit(.{ .op = .ret, .src0 = ret_slot });
        },
        .defer_stmt => |ds| {
            if (ctx.defer_stack.items.len == 0) return error.TypeError;
            const frame = &ctx.defer_stack.items[ctx.defer_stack.items.len - 1];
            try frame.append(ctx.a, switch (ds) {
                .expr => |ex| .{ .kind = .expr, .expr = ex },
                .block => |b| .{ .kind = .block, .block = b },
            });
        },
        .errdefer_stmt => |ds| {
            if (ctx.defer_stack.items.len == 0) return error.TypeError;
            const frame = &ctx.defer_stack.items[ctx.defer_stack.items.len - 1];
            try frame.append(ctx.a, switch (ds) {
                .expr => |ex| .{ .kind = .expr, .expr = ex, .on_error_only = true },
                .block => |b| .{ .kind = .block, .block = b, .on_error_only = true },
            });
        },
        .for_in_stmt => |fs| {
            try lowerForIn(ctx, fs);
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
            const cont_mark = ctx.continue_jmps.items.len;
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
            while (ctx.continue_jmps.items.len > cont_mark) {
                const ji = ctx.continue_jmps.pop().?;
                ctx.insts.items[ji].aux = head;
            }
        },
        .break_stmt => {
            const ji = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            try ctx.break_jmps.append(ctx.a, ji);
        },
        .continue_stmt => {
            const ji = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            try ctx.continue_jmps.append(ctx.a, ji);
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
        .struct_lit => |sl| {
            if (sl.type_args.len > 0) {
                const mangled = mangleNamedApp(ctx.a, sl.type_name, sl.type_args) catch return .{ .kind = .struct_ptr, .type_name = sl.type_name };
                return .{ .kind = .struct_ptr, .type_name = mangled };
            }
            return .{ .kind = .struct_ptr, .type_name = sl.type_name };
        },
        .null_lit => return .{ .kind = .opt_ptr },
        .float => return .{ .kind = .scalar, .type_name = "f64" },
        .int => return .{ .kind = .scalar, .type_name = "i64" },
        .ident => |n| {
            if (ctx.meta.get(n)) |m| return m;
            if (ctx.const_vals.get(n)) |cv| {
                if (cv.is_float) return .{ .kind = .scalar, .type_name = "f64" };
                return .{ .kind = .scalar, .type_name = "i64" };
            }
            if (ctx.global_map.get(n)) |gi| {
                if (gi < ctx.globals.len and ctx.globals[gi].is_float)
                    return .{ .kind = .scalar, .type_name = "f64" };
            }
            if (ctx.object_map.contains(n)) return .{ .kind = .struct_ptr, .type_name = n };
            return null;
        },
        .field => |f| {
            const base_tn = inferTypeNameFromExpr(ctx, f.base) orelse return null;
            const sd = ctx.structs.get(base_tn) orelse return null;
            for (sd.fields) |fld| {
                if (std.mem.eql(u8, fld.name, f.name)) {
                    const ftn = typeExprName(fld.ty);
                    if (ctx.structs.contains(ftn)) return .{ .kind = .struct_ptr, .type_name = ftn };
                    if (isBuiltinScalar(ftn)) return .{ .kind = .scalar, .type_name = ftn };
                    return .{ .kind = .struct_ptr, .type_name = ftn };
                }
            }
            return null;
        },
        .array_lit => |al| return .{ .kind = .array_ptr, .type_name = "i64", .array_len = al.elems.len, .elem_size = 8 },
        .binary => |b| {
            if (binOpMethodName(b.op)) |opm| {
                if (inferTypeNameFromExpr(ctx, b.left)) |tn| {
                    if (!isBuiltinScalar(tn) and ctx.structs.contains(tn)) {
                        const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ tn, opm }) catch return null;
                        if (ctx.methods.get(key)) |mi| {
                            if (mi.ret_name.len > 0) {
                                if (ctx.structs.contains(mi.ret_name))
                                    return .{ .kind = .struct_ptr, .type_name = mi.ret_name };
                                return .{ .kind = .scalar, .type_name = mi.ret_name };
                            }
                        }
                    }
                }
            }
            // Untyped `var x = a + b` must keep f64 so later `x + y` uses fadd (not integer add).
            if (isFloatish(ctx, b.left) or isFloatish(ctx, b.right))
                return .{ .kind = .scalar, .type_name = "f64" };
            return .{ .kind = .scalar, .type_name = "i64" };
        },
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
                if (c.type_args.len > 0) {
                    const mangled = mangleNamedApp(ctx.a, c.callee.ident, c.type_args) catch "";
                    if (mangled.len > 0) {
                        if (ctx.fn_infos.get(mangled)) |fi| {
                            if (fi.ret_name.len > 0) {
                                if (ctx.structs.contains(fi.ret_name))
                                    return .{ .kind = .struct_ptr, .type_name = fi.ret_name };
                                return .{ .kind = .scalar, .type_name = fi.ret_name };
                            }
                        }
                    }
                }
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
        .cast => |c| return metaForTypeExpr(ctx.a, c.ty, ctx.structs, ctx.ifaces) catch null,
        .tuple => |elems| return .{ .kind = .struct_ptr, .type_name = ensureTupleStruct(ctx.a, ctx.structs, elems.len) catch return null },
        else => return null,
    }
}

fn inferTypeNameFromExpr(ctx: *Ctx, e: *ast.Expr) ?[]const u8 {
    if (inferMetaFromExpr(ctx, e)) |m| {
        if (m.type_name.len > 0) return m.type_name;
    }
    return null;
}

fn indexLoadStoreWidth(ctx: *Ctx, base_expr: *ast.Expr) u32 {
    // 1 = byte load/store; 8 = qword.
    if (base_expr.* == .ident) {
        if (ctx.meta.get(base_expr.ident)) |m| {
            if (std.mem.eql(u8, m.type_name, "byte") or std.mem.eql(u8, m.type_name, "u8") or std.mem.eql(u8, m.type_name, "bool"))
                return 1;
        }
    }
    return 8;
}

fn indexStride(ctx: *Ctx, base_expr: *ast.Expr) u32 {
    // Dense packed byte arrays / raw byte pointers use stride 1.
    if (base_expr.* == .ident) {
        if (ctx.meta.get(base_expr.ident)) |m| {
            if (m.kind == .raw_ptr and (std.mem.eql(u8, m.type_name, "byte") or std.mem.eql(u8, m.type_name, "u8")))
                return 1;
            if (m.kind == .array_ptr and m.elem_size == 1)
                return 1;
        }
    }
    return 8;
}

fn lowerIndexAddr(ctx: *Ctx, base_expr: *ast.Expr, index_expr: *ast.Expr) LowerError!u32 {
    // Stack i64 arrays: base - i*8. Packed bytes / raw ptr: base + i*stride.
    const base = try lowerExpr(ctx, base_expr);
    const idx = try lowerExpr(ctx, index_expr);
    const stride = indexStride(ctx, base_expr);
    const upward = blk: {
        if (base_expr.* == .ident) {
            if (ctx.meta.get(base_expr.ident)) |m| {
                if (m.kind == .raw_ptr) break :blk true;
                if (m.kind == .array_ptr and m.elem_size == 1) break :blk true;
            }
        }
        break :blk false;
    };
    const scale = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = scale, .aux = stride });
    const off = try ctx.fresh();
    try ctx.emit(.{ .op = .mul, .dst = off, .src0 = idx, .src1 = scale });
    const addr = try ctx.fresh();
    if (upward) {
        try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = off });
    } else {
        try ctx.emit(.{ .op = .sub, .dst = addr, .src0 = base, .src1 = off });
    }
    return addr;
}

fn maybeBoundsCheck(ctx: *Ctx, base_expr: *ast.Expr, index_expr: *ast.Expr) LowerError!void {
    var len: ?u64 = null;
    if (base_expr.* == .ident) {
        if (ctx.meta.get(base_expr.ident)) |m| {
            if (m.kind == .array_ptr and m.array_len > 0) len = m.array_len;
        }
    }
    const n = len orelse return;
    const idx = try lowerExpr(ctx, index_expr);
    const lim = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = lim, .aux = @intCast(n) });
    const cmp = try ctx.fresh();
    try ctx.emit(.{ .op = .lt, .dst = cmp, .src0 = idx, .src1 = lim });
    const jz_i = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = cmp, .aux = 0 });
    const jmp_ok = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jmp, .aux = 0 });
    const panic_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_i].aux = panic_pos;
    const msg = try ctx.a.dupe(u8, "index out of bounds\n");
    const sidx: i64 = @intCast(ctx.strings.items.len);
    try ctx.strings.append(ctx.a, msg);
    const ptr = try ctx.fresh();
    try ctx.emit(.{ .op = .saddr, .dst = ptr, .aux = sidx });
    const sl = try ctx.fresh();
    try ctx.emit(.{ .op = .slen, .dst = sl, .aux = sidx });
    try ctx.emit(.{ .op = .panic, .src0 = ptr, .src1 = sl });
    const end_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jmp_ok].aux = end_pos;
}

fn lowerForIn(ctx: *Ctx, fs: ast.ForInStmt) LowerError!void {
    // `for x in a..b` → counter loop; `for x in arr` → index 0..len
    const break_mark = ctx.break_jmps.items.len;
    const cont_mark = ctx.continue_jmps.items.len;
    if (fs.iter.* == .range) {
        const start = try lowerExpr(ctx, fs.iter.range.start);
        const end = try lowerExpr(ctx, fs.iter.range.end);
        const iv = try ctx.fresh();
        try ctx.emit(.{ .op = .copy, .dst = iv, .src0 = start });
        try ctx.slots.put(ctx.a, fs.name, iv);
        try ctx.meta.put(ctx.a, fs.name, .{ .kind = .scalar, .type_name = "i64" });
        const head: i64 = @intCast(ctx.insts.items.len);
        const cmp = try ctx.fresh();
        try ctx.emit(.{ .op = .lt, .dst = cmp, .src0 = iv, .src1 = end });
        const jz_i = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jz, .src0 = cmp, .aux = 0 });
        try lowerBlock(ctx, fs.body);
        const cont_pos: i64 = @intCast(ctx.insts.items.len);
        const one = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = one, .aux = 1 });
        try ctx.emit(.{ .op = .add, .dst = iv, .src0 = iv, .src1 = one });
        try ctx.emit(.{ .op = .jmp, .aux = head });
        const end_pos: i64 = @intCast(ctx.insts.items.len);
        ctx.insts.items[jz_i].aux = end_pos;
        while (ctx.break_jmps.items.len > break_mark) {
            const ji = ctx.break_jmps.pop().?;
            ctx.insts.items[ji].aux = end_pos;
        }
        while (ctx.continue_jmps.items.len > cont_mark) {
            const ji = ctx.continue_jmps.pop().?;
            ctx.insts.items[ji].aux = cont_pos;
        }
        return;
    }
    // Array for-in: for x in xs
    const base = try lowerExpr(ctx, fs.iter);
    var alen: u64 = 0;
    if (fs.iter.* == .ident) {
        if (ctx.meta.get(fs.iter.ident)) |m| {
            if (m.kind == .array_ptr) alen = m.array_len;
        }
    } else if (fs.iter.* == .array_lit) {
        alen = fs.iter.array_lit.elems.len;
    }
    if (alen == 0) return error.TypeError;
    const iv = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = iv, .aux = 0 });
    const head: i64 = @intCast(ctx.insts.items.len);
    const lim = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = lim, .aux = @intCast(alen) });
    const cmp = try ctx.fresh();
    try ctx.emit(.{ .op = .lt, .dst = cmp, .src0 = iv, .src1 = lim });
    const jz_i = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = cmp, .aux = 0 });
    // elem = load(base - iv*stride)
    const scale = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = scale, .aux = 8 });
    const off = try ctx.fresh();
    try ctx.emit(.{ .op = .mul, .dst = off, .src0 = iv, .src1 = scale });
    const addr = try ctx.fresh();
    try ctx.emit(.{ .op = .sub, .dst = addr, .src0 = base, .src1 = off });
    const elem = try ctx.fresh();
    if (indexLoadStoreWidth(ctx, fs.iter) == 1) {
        try ctx.emit(.{ .op = .load8, .dst = elem, .src0 = addr });
    } else {
        try ctx.emit(.{ .op = .load, .dst = elem, .src0 = addr });
    }
    try ctx.slots.put(ctx.a, fs.name, elem);
    try ctx.meta.put(ctx.a, fs.name, .{ .kind = .scalar, .type_name = "i64" });
    try lowerBlock(ctx, fs.body);
    const cont_pos: i64 = @intCast(ctx.insts.items.len);
    const one = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = one, .aux = 1 });
    try ctx.emit(.{ .op = .add, .dst = iv, .src0 = iv, .src1 = one });
    try ctx.emit(.{ .op = .jmp, .aux = head });
    const end_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_i].aux = end_pos;
    while (ctx.break_jmps.items.len > break_mark) {
        const ji = ctx.break_jmps.pop().?;
        ctx.insts.items[ji].aux = end_pos;
    }
    while (ctx.continue_jmps.items.len > cont_mark) {
        const ji = ctx.continue_jmps.pop().?;
        ctx.insts.items[ji].aux = cont_pos;
    }
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

fn isFloatTypeName(name: []const u8) bool {
    return std.mem.eql(u8, name, "f64") or std.mem.eql(u8, name, "f32");
}

fn isFloatish(ctx: *Ctx, e: *ast.Expr) bool {
    return switch (e.*) {
        .float => true,
        .ident => |n| blk: {
            if (ctx.meta.get(n)) |m| {
                break :blk isFloatTypeName(m.type_name);
            }
            if (ctx.const_vals.get(n)) |cv| break :blk cv.is_float;
            if (ctx.global_map.get(n)) |gi| {
                if (gi < ctx.globals.len) break :blk ctx.globals[gi].is_float;
            }
            break :blk false;
        },
        .field => blk: {
            if (inferMetaFromExpr(ctx, e)) |m| break :blk isFloatTypeName(m.type_name);
            break :blk false;
        },
        .binary => |b| blk: {
            if (binOpMethodName(b.op)) |opm| {
                if (inferTypeNameFromExpr(ctx, b.left)) |tn| {
                    if (!isBuiltinScalar(tn) and ctx.structs.contains(tn)) {
                        const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ tn, opm }) catch break :blk false;
                        if (ctx.methods.get(key)) |mi| {
                            if (mi.ret_name.len > 0 and isFloatTypeName(mi.ret_name))
                                break :blk true;
                        }
                    }
                }
            }
            break :blk isFloatish(ctx, b.left) or isFloatish(ctx, b.right);
        },
        .unary => |u| isFloatish(ctx, u.expr),
        .cast => |c| isFloatTypeName(typeExprName(c.ty)),
        .call => blk: {
            if (inferMetaFromExpr(ctx, e)) |m| break :blk isFloatTypeName(m.type_name);
            break :blk false;
        },
        else => false,
    };
}

fn lowerExpr(ctx: *Ctx, e: *ast.Expr) LowerError!u32 {
    switch (e.*) {
        .ident => |name| {
            if (ctx.slots.get(name)) |s| return s;
            if (ctx.const_vals.get(name)) |cv| {
                const d = try ctx.fresh();
                if (cv.is_float) {
                    try ctx.emit(.{ .op = .fconst, .dst = d, .aux = cv.bits });
                } else {
                    try ctx.emit(.{ .op = .iconst, .dst = d, .aux = cv.bits });
                }
                return d;
            }
            // Object singleton: bare name is the address of static storage (no heap).
            if (ctx.object_map.get(name)) |gi| {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .gaddr, .dst = d, .aux = @intCast(gi) });
                return d;
            }
            if (ctx.global_map.get(name)) |gi| {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .gload, .dst = d, .aux = @intCast(gi) });
                return d;
            }
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
        .float => |n| {
            const d = try ctx.fresh();
            const bits: i64 = @bitCast(n);
            try ctx.emit(.{ .op = .fconst, .dst = d, .aux = bits });
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
            if (b.op == .coalesce) {
                // a ?: b → if a != 0 then a else b
                const l = try lowerExpr(ctx, b.left);
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .copy, .dst = d, .src0 = l });
                const jz_i = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jz, .src0 = d, .aux = 0 });
                const jmp_ok = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jmp, .aux = 0 });
                const else_pos: i64 = @intCast(ctx.insts.items.len);
                ctx.insts.items[jz_i].aux = else_pos;
                const r = try lowerExpr(ctx, b.right);
                try ctx.emit(.{ .op = .copy, .dst = d, .src0 = r });
                const end_pos: i64 = @intCast(ctx.insts.items.len);
                ctx.insts.items[jmp_ok].aux = end_pos;
                return d;
            }
            // Operator methods: `a + b` → `Type.op+(b)` when left is a named struct with that method.
            if (binOpMethodName(b.op)) |opm| {
                if (inferTypeNameFromExpr(ctx, b.left)) |tn| {
                    if (!isBuiltinScalar(tn) and ctx.structs.contains(tn)) {
                        const key = try std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ tn, opm });
                        if (ctx.methods.get(key)) |_| {
                            const base = try lowerExpr(ctx, b.left);
                            var args = try ctx.a.alloc(*ast.Expr, 1);
                            args[0] = b.right;
                            return try lowerMethodOrIfaceCall(ctx, b.left, base, opm, args, &.{});
                        }
                    }
                }
            }
            const l = try lowerExpr(ctx, b.left);
            const r = try lowerExpr(ctx, b.right);
            const d = try ctx.fresh();
            const use_float = isFloatish(ctx, b.left) or isFloatish(ctx, b.right);
            switch (b.op) {
                .add => try ctx.emit(.{ .op = if (use_float) .fadd else .add, .dst = d, .src0 = l, .src1 = r }),
                .sub => try ctx.emit(.{ .op = if (use_float) .fsub else .sub, .dst = d, .src0 = l, .src1 = r }),
                .mul => try ctx.emit(.{ .op = if (use_float) .fmul else .mul, .dst = d, .src0 = l, .src1 = r }),
                .div => try ctx.emit(.{ .op = if (use_float) .fdiv else .div, .dst = d, .src0 = l, .src1 = r }),
                .rem => try ctx.emit(.{ .op = .rem, .dst = d, .src0 = l, .src1 = r }),
                .eq => try ctx.emit(.{ .op = .eq, .dst = d, .src0 = l, .src1 = r }),
                .neq => try ctx.emit(.{ .op = .ne, .dst = d, .src0 = l, .src1 = r }),
                .lt => try ctx.emit(.{ .op = .lt, .dst = d, .src0 = l, .src1 = r }),
                .gt => try ctx.emit(.{ .op = .lt, .dst = d, .src0 = r, .src1 = l }),
                .lte => try ctx.emit(.{ .op = .le, .dst = d, .src0 = l, .src1 = r }),
                .gte => try ctx.emit(.{ .op = .le, .dst = d, .src0 = r, .src1 = l }),
                .bit_and => try ctx.emit(.{ .op = .bit_and, .dst = d, .src0 = l, .src1 = r }),
                .bit_or => try ctx.emit(.{ .op = .bit_or, .dst = d, .src0 = l, .src1 = r }),
                .bit_xor => try ctx.emit(.{ .op = .bit_xor, .dst = d, .src0 = l, .src1 = r }),
                .shl => try ctx.emit(.{ .op = .shl, .dst = d, .src0 = l, .src1 = r }),
                .shr => try ctx.emit(.{ .op = .shr, .dst = d, .src0 = l, .src1 = r }),
                .and_op, .or_op, .coalesce => unreachable,
            }
            return d;
        },
        .unary => |u| {
            const v = try lowerExpr(ctx, u.expr);
            const d = try ctx.fresh();
            switch (u.op) {
                .neg => try ctx.emit(.{ .op = .neg, .dst = d, .src0 = v }),
                .bit_not => try ctx.emit(.{ .op = .bit_not, .dst = d, .src0 = v }),
                .not => {
                    const z = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
                    try ctx.emit(.{ .op = .eq, .dst = d, .src0 = v, .src1 = z });
                },
                .addr => {
                    if (u.expr.* != .ident) return error.TypeError;
                    const slot = ctx.slots.get(u.expr.ident) orelse return error.UnknownName;
                    // Array names already hold a pointer; &arr is the same pointer.
                    if (ctx.meta.get(u.expr.ident)) |m| {
                        if (m.kind == .array_ptr) {
                            try ctx.emit(.{ .op = .copy, .dst = d, .src0 = slot });
                            return d;
                        }
                    }
                    try ctx.emit(.{ .op = .lea_slot, .dst = d, .src0 = slot });
                },
                .deref => try ctx.emit(.{ .op = .load, .dst = d, .src0 = v }),
            }
            return d;
        },
        .index => |ix| {
            try maybeBoundsCheck(ctx, ix.base, ix.index);
            const addr = try lowerIndexAddr(ctx, ix.base, ix.index);
            const d = try ctx.fresh();
            if (indexLoadStoreWidth(ctx, ix.base) == 1) {
                try ctx.emit(.{ .op = .load8, .dst = d, .src0 = addr });
            } else {
                try ctx.emit(.{ .op = .load, .dst = d, .src0 = addr });
            }
            return d;
        },
        .cstring => |s| {
            const idx: i64 = @intCast(ctx.strings.items.len);
            try ctx.strings.append(ctx.a, try ctx.a.dupe(u8, s));
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .saddr, .dst = d, .aux = idx });
            return d;
        },
        .range => {
            // Bare range value not usable except as for-in iter (handled there).
            return error.TypeError;
        },
        .array_lit => |al| {
            // Allocate N slots + lea to first; store elems.
            if (al.elems.len == 0) return error.TypeError;
            const first = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = first, .aux = 0 });
            var i: usize = 1;
            while (i < al.elems.len) : (i += 1) {
                const s = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = s, .aux = 0 });
            }
            const ptr = try ctx.fresh();
            try ctx.emit(.{ .op = .lea_slot, .dst = ptr, .src0 = first });
            for (al.elems, 0..) |el, ei| {
                const val = try lowerExpr(ctx, el);
                const scale = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = scale, .aux = 8 });
                const idx = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = idx, .aux = @intCast(ei) });
                const off = try ctx.fresh();
                try ctx.emit(.{ .op = .mul, .dst = off, .src0 = idx, .src1 = scale });
                const addr = try ctx.fresh();
                try ctx.emit(.{ .op = .sub, .dst = addr, .src0 = ptr, .src1 = off });
                try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = val });
            }
            // Stash array meta via a synthetic name is hard; callers use typed var.
            return ptr;
        },
        .interp => |ip| {
            // Lower to concatenated string in a fresh buffer via runtime print-style join:
            // For surface: build one string in the string table by evaluating int parts to decimal
            // at compile time when possible; otherwise allocate and... keep simple: only support
            // text + int/ident by formatting into a single compile-time string when all static,
            // else panic with unsupported. Practical path: concatenate text parts and stringify
            // ints at runtime into a fixed buffer via jogrt_alloc + manual digits — too heavy.
            // Minimal: flatten text-only; for expr parts emit decimal via a small helper pattern
            // stored as string "0" placeholders — actually use print to a buffer.
            // Ship: join text parts; for each expr, if int literal append digits; else append "?".
            var buf: std.ArrayListUnmanaged(u8) = .{};
            for (ip.parts) |p| {
                switch (p) {
                    .text => |t| try buf.appendSlice(ctx.a, t),
                    .expr => |ex| {
                        if (ex.* == .int) {
                            var tmp: [32]u8 = undefined;
                            const slice = std.fmt.bufPrint(&tmp, "{d}", .{ex.int}) catch unreachable;
                            try buf.appendSlice(ctx.a, slice);
                        } else if (ex.* == .string) {
                            try buf.appendSlice(ctx.a, ex.string);
                        } else {
                            // Runtime: evaluate to i64 and append decimal digits into heap buffer.
                            // Fallback marker — still evaluate for side effects.
                            _ = try lowerExpr(ctx, ex);
                            try buf.appendSlice(ctx.a, "?");
                        }
                    },
                }
            }
            const s = try buf.toOwnedSlice(ctx.a);
            const idx: i64 = @intCast(ctx.strings.items.len);
            try ctx.strings.append(ctx.a, s);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .saddr, .dst = d, .aux = idx });
            return d;
        },
        .try_cast => |c| {
            // as? T — for now same as cast but null on failure is not checked; return value.
            const v = try lowerExpr(ctx, c.expr);
            return v;
        },
        .is_type => |it| {
            // `e is T` — structural: true if meta type_name matches T name.
            _ = try lowerExpr(ctx, it.expr);
            const want = typeExprName(it.ty);
            const got = inferTypeNameFromExpr(ctx, it.expr) orelse "";
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = if (std.mem.eql(u8, want, got)) 1 else 0 });
            return d;
        },
        .call => |c| return try lowerCall(ctx, c),
        .cast => |c| {
            const v = try lowerExpr(ctx, c.expr);
            const to_name = typeExprName(c.ty);
            const to = try metaForTypeExpr(ctx.a, c.ty, ctx.structs, ctx.ifaces);
            const src_float = isFloatish(ctx, c.expr);
            const dst_float = isFloatTypeName(to_name);
            // Types.md: numeric `as` is a checked convert, not a bitcast.
            if (src_float and !dst_float) {
                const conv = try ctx.fresh();
                try ctx.emit(.{ .op = .fptosi, .dst = conv, .src0 = v });
                const bits = scalarBits(to_name);
                if (bits > 0 and bits < 64) {
                    const d = try ctx.fresh();
                    try ctx.emit(.{ .op = .narrow, .dst = d, .src0 = conv, .aux = bits });
                    return d;
                }
                return conv;
            }
            if (!src_float and dst_float) {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .sitofp, .dst = d, .src0 = v });
                return d;
            }
            // narrow ints via `as` when target bits known
            const bits = scalarBits(to_name);
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
            if (f.base.* == .ident) {
                if (ctx.enums.get(f.base.ident)) |ei| {
                    for (ei.variants) |v| {
                        if (std.mem.eql(u8, v.name, f.name)) {
                            const d = try ctx.fresh();
                            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = v.value });
                            return d;
                        }
                    }
                    std.debug.print("lower UnknownName enum variant {s}.{s}\n", .{ f.base.ident, f.name });
                    return error.UnknownName;
                }
            }
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
        .when_expr => |w| {
            const scrut = try lowerExpr(ctx, w.scrutinee);
            const result = try ctx.fresh();
            const enum_name = inferTypeNameFromExpr(ctx, w.scrutinee);
            var end_jmps: std.ArrayListUnmanaged(usize) = .{};
            for (w.arms) |arm| {
                var tag_val: i64 = undefined;
                var found = false;
                if (enum_name) |en| {
                    if (ctx.enums.get(en)) |ei| {
                        for (ei.variants) |v| {
                            if (std.mem.eql(u8, v.name, arm.pattern)) {
                                tag_val = v.value;
                                found = true;
                                break;
                            }
                        }
                    }
                }
                if (!found) {
                    var eit = ctx.enums.iterator();
                    while (eit.next()) |entry| {
                        for (entry.value_ptr.variants) |v| {
                            if (std.mem.eql(u8, v.name, arm.pattern)) {
                                tag_val = v.value;
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                }
                if (!found) {
                    std.debug.print("lower UnknownName when pattern {s}\n", .{arm.pattern});
                    return error.UnknownName;
                }
                const tag = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = tag, .aux = tag_val });
                const cmp = try ctx.fresh();
                try ctx.emit(.{ .op = .eq, .dst = cmp, .src0 = scrut, .src1 = tag });
                const jz_i = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jz, .src0 = cmp, .aux = 0 });
                const body_v = try lowerExpr(ctx, arm.body);
                try ctx.emit(.{ .op = .copy, .dst = result, .src0 = body_v });
                const jmp_i = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jmp, .aux = 0 });
                try end_jmps.append(ctx.a, jmp_i);
                const next_pos: i64 = @intCast(ctx.insts.items.len);
                ctx.insts.items[jz_i].aux = next_pos;
            }
            if (w.else_body) |eb| {
                const body_v = try lowerExpr(ctx, eb);
                try ctx.emit(.{ .op = .copy, .dst = result, .src0 = body_v });
            } else {
                try ctx.emit(.{ .op = .iconst, .dst = result, .aux = 0 });
            }
            const end_pos: i64 = @intCast(ctx.insts.items.len);
            for (end_jmps.items) |ji| ctx.insts.items[ji].aux = end_pos;
            return result;
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
        .tuple => |elems| {
            const tname = try ensureTupleStruct(ctx.a, ctx.structs, elems.len);
            const sd = ctx.structs.get(tname) orelse return error.UnknownName;
            const size: i64 = @intCast(sd.fields.len * 8);
            const sz = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = if (size == 0) 8 else size });
            const cid = ctx.callee_map.get("jogrt_alloc").?;
            const ptr = try ctx.fresh();
            var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
            allo.args[0] = sz;
            try ctx.emit(allo);
            for (elems, 0..) |el, i| {
                const val = try lowerExpr(ctx, el);
                const addr = try ctx.fresh();
                const off_s = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = @intCast(i * 8) });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = ptr, .src1 = off_s });
                try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = val });
            }
            return ptr;
        },
        .struct_lit => |sl| {
            const tname = if (sl.type_args.len > 0)
                try mangleNamedApp(ctx.a, sl.type_name, sl.type_args)
            else
                sl.type_name;
            const sd = ctx.structs.get(tname) orelse return error.UnknownName;
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
        if (std.mem.eql(u8, name, "jog_host_run_loop")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const fp = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_host_run_loop").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
            inst.args[0] = fp;
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
        if (c.type_args.len > 0) {
            const mangled = try mangleNamedApp(ctx.a, name, c.type_args);
            if (ctx.fn_infos.get(mangled)) |fi| {
                return try lowerFnCall(ctx, fi, c.args, c.with_args);
            }
        }
        if (ctx.fn_infos.get(name)) |fi| {
            return try lowerFnCall(ctx, fi, c.args, c.with_args);
        }
        // Inference-only: id(42) → id__i64 when specialized instance exists.
        if (c.type_args.len == 0 and c.args.len >= 1) {
            const atn = inferTypeNameFromExpr(ctx, c.args[0]) orelse (if (c.args[0].* == .int) "i64" else "");
            if (atn.len > 0) {
                const mangled = try std.fmt.allocPrint(ctx.a, "{s}__{s}", .{ name, atn });
                if (ctx.fn_infos.get(mangled)) |fi| {
                    return try lowerFnCall(ctx, fi, c.args, c.with_args);
                }
            }
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
