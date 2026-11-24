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
    /// Unsigned 64-bit division (for u64/usize/…).
    udiv,
    /// Unsigned 64-bit remainder.
    urem,
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
    /// Unsigned comparisons (u64 magnitudes ≥ 2^63).
    ult,
    ule,
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
    /// Up to 16 call args (SysV: 6 GPRs + stack). Was 8; SDL and packs need more.
    args: [16]u32 = .{none} ** 16,
};

pub const max_call_args: usize = 16;

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
    float_args: u16 = 0,
    /// Bit i set ⇒ argument i is i64/u64 (wasm32 needs real i64, not intptr_t).
    wide_args: u16 = 0,
    /// Bit i set ⇒ argument i is a pointer / `*byte` (C emit needs void* cast under prototypes).
    ptr_args: u16 = 0,
    /// `extern` return is i64/u64 (wasm32 i64, not intptr_t).
    wide_ret: bool = false,
    /// `extern` with no return type — wasm32 void vs i32 signature.
    void_ret: bool = false,
    /// Declared arity for C prototype emit (`extern` only; 0 ⇒ `(void)`).
    nparams: u8 = 0,
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
    /// Package `var x: *byte` (or `*u8`) — index with stride 1 / upward addr like locals.
    is_raw_byte_ptr: bool = false,
    /// Package `var x: SomeIface` — one-word ptr-to-fat; recover `iface_ptr` meta at use sites.
    iface_type_name: []const u8 = "",
    /// Declared type leaf name when known (`Malloc`, `i64`, …) for `&global` iface coerce.
    type_name: []const u8 = "",
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

fn floatArgMask(params: []ast.Param) u16 {
    var mask: u16 = 0;
    for (params, 0..) |p, i| {
        if (i >= max_call_args) break;
        if (isFloatTypeExpr(p.ty)) mask |= @as(u16, 1) << @intCast(i);
    }
    return mask;
}

fn isWideIntTypeExpr(te: *ast.TypeExpr) bool {
    return switch (te.*) {
        .name => |n| std.mem.eql(u8, n, "i64") or std.mem.eql(u8, n, "u64"),
        else => false,
    };
}

fn wideArgMask(params: []ast.Param) u16 {
    var mask: u16 = 0;
    for (params, 0..) |p, i| {
        if (i >= max_call_args) break;
        if (isWideIntTypeExpr(p.ty)) mask |= @as(u16, 1) << @intCast(i);
    }
    return mask;
}

fn isPtrTypeExpr(te: *ast.TypeExpr) bool {
    return switch (te.*) {
        .pointer, .pointer_const => true,
        .name => |n| std.mem.eql(u8, n, "raw_ptr") or std.mem.eql(u8, n, "str") or std.mem.eql(u8, n, "bytes"),
        else => false,
    };
}

fn ptrArgMask(params: []ast.Param) u16 {
    var mask: u16 = 0;
    for (params, 0..) |p, i| {
        if (i >= max_call_args) break;
        if (isPtrTypeExpr(p.ty)) mask |= @as(u16, 1) << @intCast(i);
    }
    return mask;
}

/// Pack an integer literal into the 64-bit `iconst` bit pattern.
/// Signed values use two's complement; u64-only magnitudes (2^63..2^64-1)
/// use the same bits as the unsigned value.
fn intLiteralBits(n: i128) ?i64 {
    if (n >= std.math.minInt(i64) and n <= std.math.maxInt(i64)) {
        return @intCast(n);
    }
    if (n >= 0 and n <= std.math.maxInt(u64)) {
        return @bitCast(@as(u64, @intCast(n)));
    }
    return null;
}

fn literalConstBits(e: *ast.Expr) ?ConstVal {
    return switch (e.*) {
        .int => |n| blk: {
            const bits = intLiteralBits(n) orelse break :blk null;
            break :blk .{ .bits = bits, .is_float = false };
        },
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
    nparams: u32, // excluding self (pack counts as one logical param; ABI may be ptr+len)
    ncontexts: u32,
    contexts: []ast.ContextParam,
    ret_name: []const u8 = "",
    /// Full return slot meta (preserves `*T` as raw_ptr/struct_ptr); null if void/unknown.
    ret_meta: ?SlotMeta = null,
    /// Index of `...T: name` among value params (not counting contexts/self), or null.
    pack_index: ?u8 = null,
};

/// Specialized free function with type-param substitution for body lowering (`size(T)`).
const SpecializedFn = struct {
    fd: ast.FnDecl,
    type_subst: std.StringHashMapUnmanaged([]const u8),
};

/// Specialized method instance for a monomorphized receiver (`List__i64.push`).
const SpecializedMethod = struct {
    mangled: []const u8,
    method_name: []const u8,
    pointer_recv: bool,
    type_name: []const u8,
    params: []ast.Param,
    contexts: []ast.ContextParam,
    ret: ?*ast.TypeExpr,
    body: *ast.Block,
    type_subst: std.StringHashMapUnmanaged([]const u8),
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
    /// For pointers this is the pointee name (see `ret_meta` for pointer-ness).
    ret_name: []const u8 = "",
    /// Full return slot meta (preserves `*T` as raw_ptr/struct_ptr); null if void/unknown.
    ret_meta: ?SlotMeta = null,
    /// Index of `...T: name` among value params (not counting contexts/self), or null.
    pack_index: ?u8 = null,
};

const Ambient = struct {
    slot: u32,
    type_name: []const u8, // concrete struct or iface name
};

const SlotKind = enum { scalar, struct_ptr, iface_ptr, opt_ptr, raw_ptr, array_ptr };

const SlotMeta = struct {
    kind: SlotKind,
    type_name: []const u8 = "",
    /// Element count when `kind == .array_ptr` and length is compile-time.
    array_len: u64 = 0,
    /// Element stride in bytes (1 for byte/u8, 8 otherwise).
    elem_size: u32 = 8,
    /// When set, runtime length for pack/`[]T` for-in (companion slot holding usize).
    len_slot: u32 = none,
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
    var addr_consts: std.StringHashMapUnmanaged(u32) = .{}; // val Name = &global
    var import_binds: std.StringHashMapUnmanaged(void) = .{};

    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_print" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_panic" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_alloc" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_ret_alloc" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_alloc_reset" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_alloc_checkpoint" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_f32_bits" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_f32_from_bits" });
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

    // Name → overloads by arity (flat merge: list.empty<T> and map.empty<K,V>).
    var generic_fns: std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)) = .{};
    var generic_structs: std.StringHashMapUnmanaged(ast.StructDecl) = .{};
    // Receiver type name → template methods (`List` → [*List<T>.push, …]).
    var generic_methods: std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.MethodDecl)) = .{};

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
                        .ptr_args = ptrArgMask(ef.params),
                        .wide_ret = wret,
                        .void_ret = ef.ret == null,
                        .nparams = @intCast(@min(ef.params.len, max_call_args)),
                    });
                    var ret_name: []const u8 = "";
                    var ret_meta: ?SlotMeta = null;
                    if (ef.ret) |r| {
                        ret_name = try resolveRetName(a, &structs, r);
                        ret_meta = try metaForTypeExpr(a, r, &structs, &ifaces);
                    }
                    try fn_infos.put(a, ef.name, .{
                        .mangled = ef.name,
                        .nparams = @intCast(ef.params.len),
                        .contexts = &[_]ast.ContextParam{},
                        .exported = false,
                        .ret_name = ret_name,
                        .ret_meta = ret_meta,
                    });
                    program.needs_libc = true;
                }
            },
            .import_decl => |imp| {
                const bind = if (imp.alias) |al|
                    al
                else if (std.mem.lastIndexOfScalar(u8, imp.path, '.')) |dot|
                    imp.path[dot + 1 ..]
                else
                    imp.path;
                try import_binds.put(a, bind, {});
            },
            .global_decl => |gd| {
                var bits: i64 = 0;
                var is_float = false;
                var is_raw_byte_ptr = false;
                var iface_type_name: []const u8 = "";
                var type_name: []const u8 = "";
                var addr_of_global: ?u32 = null;
                if (gd.init) |init| {
                    if (literalConstBits(init)) |cv| {
                        bits = cv.bits;
                        is_float = cv.is_float;
                    } else if (init.* == .null_lit) {
                        bits = 0;
                    } else if (!gd.mutable and init.* == .unary and init.unary.op == .addr and init.unary.expr.* == .ident) {
                        // `val ErrX = &global` / `&object` — sentinel address constants (std.error).
                        const target = init.unary.expr.ident;
                        if (global_map.get(target)) |gi| {
                            addr_of_global = gi;
                        } else if (object_map.get(target)) |gi| {
                            addr_of_global = gi;
                        } else {
                            std.debug.print("lower: top-level val '{s}' address-of unknown '{s}'\n", .{ gd.name, target });
                            return error.TypeError;
                        }
                    } else {
                        std.debug.print("lower: top-level {s} '{s}' needs a literal/null init\n", .{ if (gd.mutable) "var" else "val", gd.name });
                        return error.TypeError;
                    }
                }
                if (gd.ty) |te| {
                    if (isFloatTypeExpr(te)) is_float = true;
                    // Preserve `*byte` / `*u8` so `pkg[i]` uses stride-1 upward addressing
                    // (locals already get this via ctx.meta; package vars need Global flags).
                    const sm = try metaForTypeExpr(a, te, &structs, &ifaces);
                    if (sm.kind == .raw_ptr and (std.mem.eql(u8, sm.type_name, "byte") or std.mem.eql(u8, sm.type_name, "u8"))) {
                        is_raw_byte_ptr = true;
                    }
                    if (sm.kind == .iface_ptr and sm.type_name.len > 0) {
                        iface_type_name = try a.dupe(u8, sm.type_name);
                    }
                    const leaf = typeExprName(te);
                    if (leaf.len > 0) type_name = try a.dupe(u8, leaf);
                }
                if (!gd.mutable) {
                    if (addr_of_global) |gi| {
                        try addr_consts.put(a, gd.name, gi);
                    } else {
                        // Compile-time constant: fold at use sites (no storage).
                        try const_vals.put(a, gd.name, .{ .bits = bits, .is_float = is_float });
                    }
                } else {
                    // Mutable: zero-init OK when no initializer (BSS-like .data zero).
                    const idx: u32 = @intCast(globals.items.len);
                    try globals.append(a, .{
                        .name = try a.dupe(u8, gd.name),
                        .mutable = true,
                        .init_bits = bits,
                        .is_float = is_float,
                        .is_raw_byte_ptr = is_raw_byte_ptr,
                        .iface_type_name = iface_type_name,
                        .type_name = type_name,
                    });
                    try global_map.put(a, gd.name, idx);
                }
            },
            .fn_decl, .actual_fn => |fd| {
                if (fd.body != null and fd.type_params.len > 0) {
                    const gop = try generic_fns.getOrPut(a, fd.name);
                    if (!gop.found_existing) gop.value_ptr.* = .{};
                    try gop.value_ptr.append(a, fd);
                } else if (fd.body != null) {
                    try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = fd.name });
                    var ret_name: []const u8 = "";
                    var ret_meta: ?SlotMeta = null;
                    if (fd.ret) |r| {
                        ret_name = try resolveRetName(a, &structs, r);
                        ret_meta = try metaForTypeExpr(a, r, &structs, &ifaces);
                    }
                    try fn_infos.put(a, fd.name, .{
                        .mangled = fd.name,
                        .nparams = @intCast(fd.params.len + (if (packParamIndex(fd.params) != null) @as(usize, 1) else 0)),
                        .contexts = fd.contexts,
                        .exported = fd.exported or std.mem.eql(u8, fd.name, "main"),
                        .ret_name = ret_name,
                        .ret_meta = ret_meta,
                        .pack_index = packParamIndex(fd.params),
                    });
                }
            },
            .expect_fn => |ef| {
                // Placeholder until an actual_fn / runtime symbol is seen; do not pull libc yet.
                if (!callee_map.contains(ef.name)) {
                    const kind: CalleeKind = if (std.mem.startsWith(u8, ef.name, "jogrt_")) .runtime else .jog_fn;
                    try internCallee(a, &callees, &callee_map, .{ .kind = kind, .name = ef.name });
                    var ret_name: []const u8 = "";
                    var ret_meta: ?SlotMeta = null;
                    if (ef.ret) |r| {
                        ret_name = try resolveRetName(a, &structs, r);
                        ret_meta = try metaForTypeExpr(a, r, &structs, &ifaces);
                    }
                    try fn_infos.put(a, ef.name, .{
                        .mangled = ef.name,
                        .nparams = @intCast(ef.params.len),
                        .contexts = &[_]ast.ContextParam{},
                        .exported = false,
                        .ret_name = ret_name,
                        .ret_meta = ret_meta,
                    });
                }
            },
            .method => |md| {
                if (md.body == null) continue;
                if (md.type_params.len > 0) {
                    // Template method on List<T> / Map<K,V> — specialize per concrete receiver.
                    const gop = try generic_methods.getOrPut(a, md.type_name);
                    if (!gop.found_existing) gop.value_ptr.* = .{};
                    try gop.value_ptr.append(a, md);
                    continue;
                }
                const sym = try opMethodSymbol(a, md.name);
                const mangled = try std.fmt.allocPrint(a, "{s}_{s}", .{ md.type_name, sym });
                const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ md.type_name, md.name });
                try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = mangled });
                var mret: []const u8 = "";
                var mret_meta: ?SlotMeta = null;
                if (md.ret) |r| {
                    mret = try resolveRetName(a, &structs, r);
                    mret_meta = try metaForTypeExpr(a, r, &structs, &ifaces);
                }
                try methods.put(a, key, .{
                    .mangled = mangled,
                    .pointer_recv = md.pointer_recv,
                    .type_name = md.type_name,
                    .name = md.name,
                    .nparams = @intCast(md.params.len + (if (packParamIndex(md.params) != null) @as(usize, 1) else 0)),
                    .ncontexts = @intCast(md.contexts.len),
                    .contexts = md.contexts,
                    .ret_name = mret,
                    .ret_meta = mret_meta,
                    .pack_index = packParamIndex(md.params),
                });
            },
            else => {},
        }
    }

    // Instantiate generics referenced from the program (calls / struct lits / types).
    var specialized: std.ArrayListUnmanaged(SpecializedFn) = .{};
    var specialized_methods: std.ArrayListUnmanaged(SpecializedMethod) = .{};
    try instantiateGenerics(a, file, &generic_fns, &generic_structs, &generic_methods, &structs, &methods, &ifaces, &fn_infos, &callees, &callee_map, &specialized, &specialized_methods);

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
                .ret_iface = retIfaceName(&ifaces, fd.ret),
            }, &strings, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds);
            try funcs.append(a, f);
        } else if (d == .method and d.method.body != null and d.method.type_params.len == 0) {
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
                .ret_iface = retIfaceName(&ifaces, md.ret),
            }, &strings, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds);
            try funcs.append(a, f);
        }
    }
    for (specialized.items) |*sf| {
        const fd = sf.fd;
        const f = try lowerFn(a, .{
            .name = fd.name,
            .params = fd.params,
            .contexts = fd.contexts,
            .body = fd.body.?,
            .exported = false,
            .self_type = null,
            .pointer_recv = false,
            .ret_iface = retIfaceName(&ifaces, fd.ret),
            .type_subst = &sf.type_subst,
        }, &strings, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds);
        try funcs.append(a, f);
    }
    for (specialized_methods.items) |*sm| {
        const f = lowerFn(a, .{
            .name = sm.mangled,
            .params = sm.params,
            .contexts = sm.contexts,
            .body = sm.body,
            .exported = true,
            .self_type = sm.type_name,
            .pointer_recv = sm.pointer_recv,
            .ret_iface = retIfaceName(&ifaces, sm.ret),
            .type_subst = &sm.type_subst,
        }, &strings, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds) catch |err| {
            // Callback / unsupported bodies (each, map, fold, …) — drop so callers error clearly.
            std.debug.print("skip specialized method {s}: {s}\n", .{ sm.mangled, @errorName(err) });
            const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ sm.type_name, sm.method_name });
            _ = methods.remove(key);
            continue;
        };
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
        .tuple => |elems| try ensureTupleStructFromTypes(a, structs, elems),
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
        .slice_view => |sv| {
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .slice_view = .{
                .elem = try substTypeExpr(a, sv.elem, params, args),
                .is_const = sv.is_const,
            } };
            return out;
        },
        .iface_fn => |ift| {
            var nparams: std.ArrayListUnmanaged(ast.Param) = .{};
            for (ift.params) |p| {
                try nparams.append(a, .{
                    .name = p.name,
                    .ty = try substTypeExpr(a, p.ty, params, args),
                    .is_pack = p.is_pack,
                });
            }
            var nret: ?*ast.TypeExpr = null;
            if (ift.ret) |r| nret = try substTypeExpr(a, r, params, args);
            const nift = try a.create(ast.IfaceFnType);
            nift.* = .{ .params = try nparams.toOwnedSlice(a), .ret = nret };
            const out = try a.create(ast.TypeExpr);
            out.* = .{ .iface_fn = nift };
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

fn findGenericFn(
    generic_fns: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)),
    name: []const u8,
    nargs: usize,
) ?ast.FnDecl {
    const list = generic_fns.get(name) orelse return null;
    for (list.items) |fd| {
        if (fd.type_params.len == nargs) return fd;
    }
    // Fallback: single overload of any arity (inference sites).
    if (list.items.len == 1) return list.items[0];
    return null;
}

fn typeArgConcreteName(a: std.mem.Allocator, te: *ast.TypeExpr) ![]const u8 {
    return switch (te.*) {
        .name => |n| n,
        .named_app => |na| try mangleNamedApp(a, na.name, na.args),
        .optional => |inner| try std.fmt.allocPrint(a, "Opt{s}", .{try typeArgConcreteName(a, inner)}),
        // Keep pointer identity so `size(T)` / mangling for `*Value` ≠ `Value`.
        .pointer, .pointer_const => |inner| try std.fmt.allocPrint(a, "P{s}", .{try typeArgConcreteName(a, inner)}),
        else => "",
    };
}

fn buildTypeSubst(
    a: std.mem.Allocator,
    params: [][]const u8,
    args: []*ast.TypeExpr,
) !std.StringHashMapUnmanaged([]const u8) {
    var subst: std.StringHashMapUnmanaged([]const u8) = .{};
    for (params, 0..) |p, i| {
        if (i >= args.len) break;
        const cname = try typeArgConcreteName(a, args[i]);
        if (cname.len == 0) continue;
        try subst.put(a, p, cname);
    }
    return subst;
}

fn ensureGenericStruct(
    a: std.mem.Allocator,
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    generic_methods: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.MethodDecl)),
    generic_fns: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(SpecializedFn),
    specialized_methods: *std.ArrayListUnmanaged(SpecializedMethod),
    name: []const u8,
    args: []*ast.TypeExpr,
) ![]const u8 {
    const leaf = typeNameLeaf(name);
    const mangled = try mangleNamedApp(a, leaf, args);
    if (structs.contains(mangled)) return mangled;
    const tmpl = generic_structs.get(leaf) orelse generic_structs.get(name) orelse return mangled;
    if (args.len != tmpl.type_params.len) return mangled;
    var fields: std.ArrayListUnmanaged(ast.Field) = .{};
    for (tmpl.fields) |f| {
        try fields.append(a, .{
            .name = f.name,
            .ty = try substTypeExpr(a, f.ty, tmpl.type_params, args),
        });
    }
    try structs.put(a, mangled, .{ .fields = try fields.toOwnedSlice(a) });
    // Specialize receiver methods for this concrete type.
    if (generic_methods.get(leaf) orelse generic_methods.get(name)) |mlist| {
        for (mlist.items) |md| {
            try ensureGenericMethod(a, generic_fns, generic_structs, generic_methods, structs, methods, ifaces, fn_infos, callees, callee_map, specialized, specialized_methods, md, tmpl.type_params, args, mangled);
        }
    }
    return mangled;
}

fn ensureGenericMethod(
    a: std.mem.Allocator,
    generic_fns: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    generic_methods: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.MethodDecl)),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(SpecializedFn),
    specialized_methods: *std.ArrayListUnmanaged(SpecializedMethod),
    md: ast.MethodDecl,
    recv_params: [][]const u8,
    recv_args: []*ast.TypeExpr,
    mangled_recv: []const u8,
) !void {
    _ = recv_params;
    const sym = try opMethodSymbol(a, md.name);
    const mangled = try std.fmt.allocPrint(a, "{s}_{s}", .{ mangled_recv, sym });
    const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ mangled_recv, md.name });
    if (methods.contains(key)) return;

    // Full subst: receiver args, then method-level params (map<R> / fold<R>).
    // Unbound method params default to the first receiver arg (R = T) so callback
    // methods lower; call sites with matching explicit args (map<i64> on List<i64>) work.
    var all_args: std.ArrayListUnmanaged(*ast.TypeExpr) = .{};
    try all_args.appendSlice(a, recv_args);
    while (all_args.items.len < md.type_params.len) {
        if (recv_args.len == 0) break;
        try all_args.append(a, recv_args[0]);
    }
    if (all_args.items.len < md.type_params.len) return;
    const subst_args = all_args.items;
    const subst = try buildTypeSubst(a, md.type_params, subst_args);

    var params: std.ArrayListUnmanaged(ast.Param) = .{};
    for (md.params) |p| {
        try params.append(a, .{
            .name = p.name,
            .ty = try substTypeExpr(a, p.ty, md.type_params, subst_args),
            .is_pack = p.is_pack,
        });
    }
    var ret: ?*ast.TypeExpr = null;
    if (md.ret) |r| ret = try substTypeExpr(a, r, md.type_params, subst_args);

    var mret: []const u8 = "";
    var mret_meta: ?SlotMeta = null;
    if (ret) |r| {
        mret = try resolveRetName(a, structs, r);
        mret_meta = try metaForTypeExpr(a, r, structs, ifaces);
    }
    try internCallee(a, callees, callee_map, .{ .kind = .jog_fn, .name = mangled });
    try methods.put(a, key, .{
        .mangled = mangled,
        .pointer_recv = md.pointer_recv,
        .type_name = mangled_recv,
        .name = md.name,
        .nparams = @intCast(md.params.len + (if (packParamIndex(md.params) != null) @as(usize, 1) else 0)),
        .ncontexts = @intCast(md.contexts.len),
        .contexts = md.contexts,
        .ret_name = mret,
        .ret_meta = mret_meta,
        .pack_index = packParamIndex(md.params),
    });
    try specialized_methods.append(a, .{
        .mangled = mangled,
        .method_name = md.name,
        .pointer_recv = md.pointer_recv,
        .type_name = mangled_recv,
        .params = try params.toOwnedSlice(a),
        .contexts = md.contexts,
        .ret = ret,
        .body = md.body.?,
        .type_subst = subst,
    });

    // Instantiate nested generic free fns used in the body (store_elem<T>, List<R>(), …).
    if (md.body) |body| {
        for (body.stmts) |s| {
            try walkStmtInstantiateCtx(a, s, generic_fns, generic_structs, generic_methods, structs, methods, ifaces, fn_infos, callees, callee_map, specialized, specialized_methods, md.type_params, subst_args);
        }
    }
}

fn ensureGenericFn(
    a: std.mem.Allocator,
    generic_fns: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    generic_methods: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.MethodDecl)),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(SpecializedFn),
    specialized_methods: *std.ArrayListUnmanaged(SpecializedMethod),
    name: []const u8,
    args: []*ast.TypeExpr,
) !void {
    const mangled = try mangleNamedApp(a, name, args);
    if (fn_infos.contains(mangled)) return;
    const tmpl = findGenericFn(generic_fns, name, args.len) orelse return;
    if (args.len != tmpl.type_params.len) return;
    var params: std.ArrayListUnmanaged(ast.Param) = .{};
    for (tmpl.params) |p| {
        try params.append(a, .{
            .name = p.name,
            .ty = try substTypeExpr(a, p.ty, tmpl.type_params, args),
            .is_pack = p.is_pack,
        });
    }
    var ret: ?*ast.TypeExpr = null;
    if (tmpl.ret) |r| ret = try substTypeExpr(a, r, tmpl.type_params, args);
    // Also instantiate return/param named apps (List<T>).
    {
        var ic = InstCtx{
            .a = a,
            .generic_fns = generic_fns,
            .generic_structs = generic_structs,
            .generic_methods = generic_methods,
            .structs = structs,
            .methods = methods,
            .ifaces = ifaces,
            .fn_infos = fn_infos,
            .callees = callees,
            .callee_map = callee_map,
            .specialized = specialized,
            .specialized_methods = specialized_methods,
            .subst_params = tmpl.type_params,
            .subst_args = args,
        };
        if (ret) |r| try walkTypeInstantiateFull(&ic, r);
        for (params.items) |p| try walkTypeInstantiateFull(&ic, p.ty);
    }

    const subst = try buildTypeSubst(a, tmpl.type_params, args);
    const fd = ast.FnDecl{
        .name = mangled,
        .type_params = &.{},
        .params = try params.toOwnedSlice(a),
        .contexts = tmpl.contexts,
        .ret = ret,
        .body = tmpl.body,
        .exported = false,
    };
    try specialized.append(a, .{ .fd = fd, .type_subst = subst });
    try internCallee(a, callees, callee_map, .{ .kind = .jog_fn, .name = mangled });
    var ret_name: []const u8 = "";
    var ret_meta: ?SlotMeta = null;
    if (ret) |r| {
        ret_name = try resolveRetName(a, structs, r);
        ret_meta = try metaForTypeExpr(a, r, structs, ifaces);
    }
    try fn_infos.put(a, mangled, .{
        .mangled = mangled,
        .nparams = @intCast(fd.params.len + (if (packParamIndex(fd.params) != null) @as(usize, 1) else 0)),
        .contexts = fd.contexts,
        .exported = false,
        .ret_name = ret_name,
        .ret_meta = ret_meta,
        .pack_index = packParamIndex(fd.params),
    });
    // Walk body so nested List<T>() / size uses get specialized structs.
    if (tmpl.body) |body| {
        for (body.stmts) |s| {
            try walkStmtInstantiateCtx(a, s, generic_fns, generic_structs, generic_methods, structs, methods, ifaces, fn_infos, callees, callee_map, specialized, specialized_methods, tmpl.type_params, args);
        }
    }
}

const InstCtx = struct {
    a: std.mem.Allocator,
    generic_fns: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    generic_methods: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.MethodDecl)),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(SpecializedFn),
    specialized_methods: *std.ArrayListUnmanaged(SpecializedMethod),
    /// When walking a specialized body, substitute these into bare type-arg holes.
    subst_params: [][]const u8 = &.{},
    subst_args: []*ast.TypeExpr = &.{},
};

fn substTypeArgs(ic: *InstCtx, targs: []*ast.TypeExpr) ![]*ast.TypeExpr {
    if (ic.subst_params.len == 0) return targs;
    var out = try ic.a.alloc(*ast.TypeExpr, targs.len);
    for (targs, 0..) |ta, i| {
        out[i] = try substTypeExpr(ic.a, ta, ic.subst_params, ic.subst_args);
    }
    return out;
}

fn ensureFn(ic: *InstCtx, name: []const u8, args: []*ast.TypeExpr) !void {
    try ensureGenericFn(ic.a, ic.generic_fns, ic.generic_structs, ic.generic_methods, ic.structs, ic.methods, ic.ifaces, ic.fn_infos, ic.callees, ic.callee_map, ic.specialized, ic.specialized_methods, name, args);
}

fn ensureStruct(ic: *InstCtx, name: []const u8, args: []*ast.TypeExpr) ![]const u8 {
    return try ensureGenericStruct(ic.a, ic.generic_structs, ic.generic_methods, ic.generic_fns, ic.structs, ic.methods, ic.ifaces, ic.fn_infos, ic.callees, ic.callee_map, ic.specialized, ic.specialized_methods, name, args);
}

fn walkExprInstantiate(ic: *InstCtx, e: *ast.Expr) anyerror!void {
    switch (e.*) {
        .call => |c| {
            const fname: ?[]const u8 = blk: {
                if (c.callee.* == .ident) break :blk c.callee.ident;
                // `list.empty<T>()` / `pkg.fn<T>(…)` after import flatten.
                if (c.callee.* == .field) break :blk c.callee.field.name;
                break :blk null;
            };
            if (fname) |name| {
                if (c.type_args.len > 0) {
                    const targs = try substTypeArgs(ic, c.type_args);
                    try ensureFn(ic, name, targs);
                } else if (ic.generic_fns.contains(name)) {
                    if (ic.subst_args.len > 0) {
                        // Inside specialized body: store_elem(…) inherits receiver T.
                        if (findGenericFn(ic.generic_fns, name, ic.subst_args.len)) |tmpl| {
                            if (tmpl.type_params.len <= ic.subst_args.len) {
                                try ensureFn(ic, name, ic.subst_args[0..tmpl.type_params.len]);
                            }
                        } else if (findGenericFn(ic.generic_fns, name, 1)) |tmpl| {
                            if (tmpl.type_params.len <= ic.subst_args.len) {
                                try ensureFn(ic, name, ic.subst_args[0..tmpl.type_params.len]);
                            }
                        }
                    } else if (c.args.len >= 1) {
                        const atn = blk: {
                            if (c.args[0].* == .int) break :blk "i64";
                            if (c.args[0].* == .float) break :blk "f64";
                            if (c.args[0].* == .string) break :blk "str";
                            break :blk "i64";
                        };
                        const ty = try ic.a.create(ast.TypeExpr);
                        ty.* = .{ .name = atn };
                        var args = try ic.a.alloc(*ast.TypeExpr, 1);
                        args[0] = ty;
                        try ensureFn(ic, name, args);
                    }
                }
            }
            for (c.args) |arg| try walkExprInstantiate(ic, arg);
            for (c.with_args) |w| try walkExprInstantiate(ic, w);
            try walkExprInstantiate(ic, c.callee);
        },
        .struct_lit => |sl| {
            if (sl.type_args.len > 0) {
                const targs = try substTypeArgs(ic, sl.type_args);
                _ = try ensureStruct(ic, sl.type_name, targs);
            }
            for (sl.fields) |f| try walkExprInstantiate(ic, f.value);
        },
        .tuple => |elems| {
            _ = try ensureTupleStruct(ic.a, ic.structs, elems.len);
            for (elems) |el| try walkExprInstantiate(ic, el);
        },
        .binary => |b| {
            try walkExprInstantiate(ic, b.left);
            try walkExprInstantiate(ic, b.right);
        },
        .unary => |u| try walkExprInstantiate(ic, u.expr),
        .field => |f| try walkExprInstantiate(ic, f.base),
        .index => |ix| {
            try walkExprInstantiate(ic, ix.base);
            try walkExprInstantiate(ic, ix.index);
        },
        .slice => |sl| {
            try walkExprInstantiate(ic, sl.base);
            if (sl.start) |s| try walkExprInstantiate(ic, s);
            if (sl.end) |en| try walkExprInstantiate(ic, en);
        },
        .cast => |c| {
            try walkTypeInstantiateFull(ic, c.ty);
            try walkExprInstantiate(ic, c.expr);
        },
        .when_expr => |w| {
            try walkExprInstantiate(ic, w.scrutinee);
            for (w.arms) |arm| try walkExprInstantiate(ic, arm.body);
            if (w.else_body) |eb| try walkExprInstantiate(ic, eb);
        },
        .optional_call => |oc| {
            try walkExprInstantiate(ic, oc.base);
            for (oc.args) |arg| try walkExprInstantiate(ic, arg);
        },
        .panic_call => |p| try walkExprInstantiate(ic, p.message),
        .range => |r| {
            try walkExprInstantiate(ic, r.start);
            try walkExprInstantiate(ic, r.end);
        },
        .array_lit => |al| {
            for (al.elems) |el| try walkExprInstantiate(ic, el);
        },
        .interp => |ip| {
            for (ip.parts) |p| switch (p) {
                .text => {},
                .expr => |ex| try walkExprInstantiate(ic, ex),
            };
        },
        .try_cast => |c| {
            try walkTypeInstantiateFull(ic, c.ty);
            try walkExprInstantiate(ic, c.expr);
        },
        .is_type => |it| {
            try walkTypeInstantiateFull(ic, it.ty);
            try walkExprInstantiate(ic, it.expr);
        },
        .force_unwrap => |inner| try walkExprInstantiate(ic, inner),
        else => {},
    }
}

fn walkTypeInstantiateFull(ic: *InstCtx, te: *ast.TypeExpr) anyerror!void {
    switch (te.*) {
        .named_app => |na| {
            const targs = try substTypeArgs(ic, na.args);
            _ = try ensureStruct(ic, na.name, targs);
        },
        .tuple => |elems| _ = try ensureTupleStructFromTypes(ic.a, ic.structs, elems),
        .optional, .pointer, .pointer_const => |inner| try walkTypeInstantiateFull(ic, inner),
        .array => |arr| try walkTypeInstantiateFull(ic, arr.elem),
        .slice_view => |sv| try walkTypeInstantiateFull(ic, sv.elem),
        else => {},
    }
}

fn walkStmtInstantiate(ic: *InstCtx, stmt: ast.Stmt) anyerror!void {
    switch (stmt) {
        .var_decl => |vd| {
            if (vd.ty) |t| try walkTypeInstantiateFull(ic, t);
            if (vd.init) |init| try walkExprInstantiate(ic, init);
        },
        .destructure => |ds| try walkExprInstantiate(ic, ds.init),
        .assign => |as| {
            try walkExprInstantiate(ic, as.value);
            if (as.field) |fa| try walkExprInstantiate(ic, fa.base);
            if (as.index) |ix| {
                try walkExprInstantiate(ic, ix.base);
                try walkExprInstantiate(ic, ix.index);
            }
        },
        .expr => |e| try walkExprInstantiate(ic, e),
        .return_stmt => |re| if (re) |e| try walkExprInstantiate(ic, e),
        .if_stmt => |ifs| {
            try walkExprInstantiate(ic, ifs.cond);
            for (ifs.then_block.stmts) |s| try walkStmtInstantiate(ic, s);
            if (ifs.else_block) |eb| for (eb.stmts) |s| try walkStmtInstantiate(ic, s);
        },
        .while_stmt => |ws| {
            try walkExprInstantiate(ic, ws.cond);
            for (ws.body.stmts) |s| try walkStmtInstantiate(ic, s);
        },
        .with_stmt => |ws| {
            for (ws.values) |v| try walkExprInstantiate(ic, v);
            for (ws.body.stmts) |s| try walkStmtInstantiate(ic, s);
        },
        .defer_stmt, .errdefer_stmt => |ds| switch (ds) {
            .expr => |ex| try walkExprInstantiate(ic, ex),
            .block => |b| for (b.stmts) |s| try walkStmtInstantiate(ic, s),
        },
        .for_in_stmt => |fs| {
            try walkExprInstantiate(ic, fs.iter);
            for (fs.body.stmts) |s| try walkStmtInstantiate(ic, s);
        },
        else => {},
    }
}

fn walkStmtInstantiateCtx(
    a: std.mem.Allocator,
    stmt: ast.Stmt,
    generic_fns: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    generic_methods: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.MethodDecl)),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(SpecializedFn),
    specialized_methods: *std.ArrayListUnmanaged(SpecializedMethod),
    subst_params: [][]const u8,
    subst_args: []*ast.TypeExpr,
) !void {
    var ic = InstCtx{
        .a = a,
        .generic_fns = generic_fns,
        .generic_structs = generic_structs,
        .generic_methods = generic_methods,
        .structs = structs,
        .methods = methods,
        .ifaces = ifaces,
        .fn_infos = fn_infos,
        .callees = callees,
        .callee_map = callee_map,
        .specialized = specialized,
        .specialized_methods = specialized_methods,
        .subst_params = subst_params,
        .subst_args = subst_args,
    };
    try walkStmtInstantiate(&ic, stmt);
}

fn instantiateGenerics(
    a: std.mem.Allocator,
    file: *const ast.File,
    generic_fns: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.FnDecl)),
    generic_structs: *std.StringHashMapUnmanaged(ast.StructDecl),
    generic_methods: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(ast.MethodDecl)),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    specialized: *std.ArrayListUnmanaged(SpecializedFn),
    specialized_methods: *std.ArrayListUnmanaged(SpecializedMethod),
) !void {
    var ic = InstCtx{
        .a = a,
        .generic_fns = generic_fns,
        .generic_structs = generic_structs,
        .generic_methods = generic_methods,
        .structs = structs,
        .methods = methods,
        .ifaces = ifaces,
        .fn_infos = fn_infos,
        .callees = callees,
        .callee_map = callee_map,
        .specialized = specialized,
        .specialized_methods = specialized_methods,
    };
    for (file.decls) |d| {
        switch (d) {
            .fn_decl, .actual_fn => |fd| {
                if (fd.type_params.len > 0) continue; // templates specialized on demand
                if (fd.ret) |r| try walkTypeInstantiateFull(&ic, r);
                for (fd.params) |p| try walkTypeInstantiateFull(&ic, p.ty);
                if (fd.body) |body| {
                    for (body.stmts) |s| try walkStmtInstantiate(&ic, s);
                }
            },
            .method => |md| {
                if (md.type_params.len > 0) continue;
                if (md.ret) |r| try walkTypeInstantiateFull(&ic, r);
                if (md.body) |body| {
                    for (body.stmts) |s| try walkStmtInstantiate(&ic, s);
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
    /// When set, `return` values are coerced to this interface fat pointer.
    ret_iface: ?[]const u8 = null,
    /// Borrowed map for this specialization (`T` → concrete); null = empty.
    type_subst: ?*const std.StringHashMapUnmanaged([]const u8) = null,
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
    /// `val Name = &global` → emit gaddr of the mapped global index.
    addr_consts: *std.StringHashMapUnmanaged(u32),
    global_map: *std.StringHashMapUnmanaged(u32),
    /// Package `var` storage (for floatish gload typing).
    globals: []const Global,
    object_map: *std.StringHashMapUnmanaged(u32),
    /// Import bind names (`as` alias or last path segment) for `pkg.Name` under flatten.
    import_binds: *std.StringHashMapUnmanaged(void),
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
    /// When set, `return` values are coerced to this interface fat pointer.
    ret_iface: ?[]const u8 = null,
    /// Generic method/fn type-param → concrete name (`T` → `i64`). Empty for non-generic.
    type_subst: *const std.StringHashMapUnmanaged([]const u8),

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
    addr_consts: *std.StringHashMapUnmanaged(u32),
    global_map: *std.StringHashMapUnmanaged(u32),
    globals: []const Global,
    object_map: *std.StringHashMapUnmanaged(u32),
    import_binds: *std.StringHashMapUnmanaged(void),
) !Func {
    std.debug.print("lowerFn:{s}\n", .{fl.name});
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
        if (p.is_pack) {
            // Pack arrives as (ptr, len); local is array_ptr with runtime len_slot.
            const ptr_slot = next_slot;
            next_slot += 1;
            const len_slot = next_slot;
            next_slot += 1;
            try slots.put(a, p.name, ptr_slot);
            const elem = blk: {
                if (p.ty.* == .slice_view) break :blk typeExprName(p.ty.slice_view.elem);
                break :blk typeExprName(p.ty);
            };
            // Placeholder elem_size; patched after Ctx exists via type_subst + layoutSizeOf.
            try meta.put(a, p.name, .{
                .kind = .array_ptr,
                .type_name = elem,
                .elem_size = 8,
                .len_slot = len_slot,
            });
            continue;
        }
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
    var empty_subst: std.StringHashMapUnmanaged([]const u8) = .{};
    const subst_ptr: *const std.StringHashMapUnmanaged([]const u8) = fl.type_subst orelse &empty_subst;
    // Resolve type-param names in param meta (T → i64) and fix pack elem sizes.
    {
        var it = meta.iterator();
        while (it.next()) |e| {
            const leaf = typeNameLeaf(e.value_ptr.type_name);
            if (subst_ptr.get(leaf)) |r| e.value_ptr.type_name = r;
            if (e.value_ptr.kind == .array_ptr) {
                const es = layoutSizeOfName(e.value_ptr.type_name);
                e.value_ptr.elem_size = @intCast(@max(es, 1));
            }
        }
    }
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
        .addr_consts = addr_consts,
        .global_map = global_map,
        .globals = globals,
        .object_map = object_map,
        .import_binds = import_binds,
        .slots = &slots,
        .meta = &meta,
        .next_slot = &next_slot,
        .insts = &insts,
        .ambient = &ambient,
        .break_jmps = &break_jmps,
        .continue_jmps = &continue_jmps,
        .defer_stack = &defer_stack,
        .ret_iface = fl.ret_iface,
        .type_subst = subst_ptr,
    };
    lowerBlock(&ctx, fl.body) catch |err| {
        std.debug.print("lowerFn body fail {s}: {s}\n", .{ fl.name, @errorName(err) });
        return err;
    };
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
        .name => |n| typeNameLeaf(n),
        .named_app => |na| typeNameLeaf(na.name), // caller should prefer mangleNamedApp
        .pointer, .pointer_const, .optional => |inner| typeExprName(inner),
        .array => |arr| typeExprName(arr.elem),
        .tuple => "__Tup",
        else => "",
    };
}

fn retIfaceName(ifaces: *std.StringHashMapUnmanaged(IfaceInfo), ret: ?*ast.TypeExpr) ?[]const u8 {
    const te = ret orelse return null;
    const n = typeExprName(te);
    if (n.len == 0) return null;
    if (!ifaces.contains(n)) return null;
    return n;
}

/// `pkg.Name` → `Name` for flattened import merge / ambient matching.
fn typeNameLeaf(n: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, n, '.')) |dot| return n[dot + 1 ..];
    return n;
}

fn mangleTypeArgName(te: *ast.TypeExpr) []const u8 {
    // Allocator-free leaf used only for non-pointer args; pointers go through
    // mangleTypeArgNameAlloc from mangleNamedApp.
    return switch (te.*) {
        .name => |n| typeNameLeaf(n),
        .named_app => |na| typeNameLeaf(na.name),
        .tuple => |_| "tup",
        .optional => |inner| mangleTypeArgName(inner),
        .pointer, .pointer_const => |inner| mangleTypeArgName(inner),
        .array => |arr| mangleTypeArgName(arr.elem),
        else => "X",
    };
}

fn mangleTypeArgNameAlloc(a: std.mem.Allocator, te: *ast.TypeExpr) std.mem.Allocator.Error![]const u8 {
    return switch (te.*) {
        .name => |n| try a.dupe(u8, typeNameLeaf(n)),
        .named_app => |na| try mangleNamedApp(a, na.name, na.args),
        .tuple => |_| try a.dupe(u8, "tup"),
        .optional => |inner| try std.fmt.allocPrint(a, "Opt{s}", .{try mangleTypeArgNameAlloc(a, inner)}),
        .pointer, .pointer_const => |inner| try std.fmt.allocPrint(a, "P{s}", .{try mangleTypeArgNameAlloc(a, inner)}),
        .array => |arr| try mangleTypeArgNameAlloc(a, arr.elem),
        else => try a.dupe(u8, "X"),
    };
}

fn mangleNamedApp(a: std.mem.Allocator, name: []const u8, args: []*ast.TypeExpr) std.mem.Allocator.Error![]const u8 {
    // `list.List<*Value>` and bare `List<*Value>` must share one mangled name so
    // field-typed receivers find methods (templates are keyed by leaf).
    var buf: std.ArrayListUnmanaged(u8) = .{};
    try buf.appendSlice(a, typeNameLeaf(name));
    for (args) |arg| {
        try buf.appendSlice(a, "__");
        try buf.appendSlice(a, try mangleTypeArgNameAlloc(a, arg));
    }
    return try buf.toOwnedSlice(a);
}

fn tupleStructName(a: std.mem.Allocator, n: usize) ![]const u8 {
    return try std.fmt.allocPrint(a, "__Tup{d}", .{n});
}

fn mangleTupleElem(a: std.mem.Allocator, te: *ast.TypeExpr) ![]const u8 {
    return switch (te.*) {
        .name => |n| try a.dupe(u8, typeNameLeaf(n)),
        .named_app => |na| try mangleNamedApp(a, na.name, na.args),
        .optional => |inner| try std.fmt.allocPrint(a, "Opt{s}", .{try mangleTupleElem(a, inner)}),
        .pointer, .pointer_const => |inner| try std.fmt.allocPrint(a, "P{s}", .{try mangleTupleElem(a, inner)}),
        .array => |arr| try std.fmt.allocPrint(a, "A{s}", .{try mangleTupleElem(a, arr.elem)}),
        .tuple => |elems| blk: {
            var buf: std.ArrayListUnmanaged(u8) = .{};
            try buf.appendSlice(a, "Tup");
            for (elems) |e| {
                try buf.append(a, '_');
                try buf.appendSlice(a, try mangleTupleElem(a, e));
            }
            break :blk try buf.toOwnedSlice(a);
        },
        else => try a.dupe(u8, "X"),
    };
}

/// Typed product `(T, U, …)` → `__Tup_T_OptU` with real field types (not all-i64).
fn ensureTupleStructFromTypes(
    a: std.mem.Allocator,
    structs: ?*std.StringHashMapUnmanaged(StructInfo),
    elems: []*ast.TypeExpr,
) ![]const u8 {
    var buf: std.ArrayListUnmanaged(u8) = .{};
    try buf.appendSlice(a, "__Tup");
    for (elems) |e| {
        try buf.append(a, '_');
        try buf.appendSlice(a, try mangleTupleElem(a, e));
    }
    const name = try buf.toOwnedSlice(a);
    if (structs) |st| {
        if (st.contains(name)) return name;
        var fields: std.ArrayListUnmanaged(ast.Field) = .{};
        for (elems, 0..) |e, i| {
            const fname = try std.fmt.allocPrint(a, "_{d}", .{i});
            try fields.append(a, .{ .name = fname, .ty = try cloneTypeExpr(a, e) });
        }
        try st.put(a, name, .{ .fields = try fields.toOwnedSlice(a) });
    }
    return name;
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

/// `[]T` / `[]const T` → synthetic `__Slice_T` with `{ ptr: *T, len: i64 }`.
fn ensureSliceStruct(
    a: std.mem.Allocator,
    structs: *std.StringHashMapUnmanaged(StructInfo),
    elem: *ast.TypeExpr,
) ![]const u8 {
    const en = try mangleTupleElem(a, elem);
    const name = try std.fmt.allocPrint(a, "__Slice_{s}", .{en});
    if (structs.contains(name)) return name;
    const ptr_ty = try a.create(ast.TypeExpr);
    ptr_ty.* = .{ .pointer = try cloneTypeExpr(a, elem) };
    const len_ty = try a.create(ast.TypeExpr);
    len_ty.* = .{ .name = "i64" };
    var fields: std.ArrayListUnmanaged(ast.Field) = .{};
    try fields.append(a, .{ .name = "ptr", .ty = ptr_ty });
    try fields.append(a, .{ .name = "len", .ty = len_ty });
    try structs.put(a, name, .{ .fields = try fields.toOwnedSlice(a) });
    return name;
}

fn ensureSliceStructFromElemName(
    a: std.mem.Allocator,
    structs: *std.StringHashMapUnmanaged(StructInfo),
    elem_name: []const u8,
) ![]const u8 {
    const elem = try a.create(ast.TypeExpr);
    elem.* = .{ .name = try a.dupe(u8, elem_name) };
    return ensureSliceStruct(a, structs, elem);
}

/// Element type name for slicing `base` (`[N]T` → `T`, `__Slice_T` / `View_T` → `T`).
fn sliceElemTypeName(ctx: *Ctx, base: *ast.Expr) ?[]const u8 {
    if (inferMetaFromExpr(ctx, base)) |bm| {
        if (bm.kind == .array_ptr and bm.type_name.len > 0) return bm.type_name;
        if (bm.kind == .struct_ptr and bm.type_name.len > 0) {
            if (std.mem.startsWith(u8, bm.type_name, "__Slice_"))
                return bm.type_name["__Slice_".len..];
            if (std.mem.startsWith(u8, bm.type_name, "View_"))
                return bm.type_name["View_".len..];
            // Named slice aliases (`bytes` / `str`) share `{ptr,len}` layout.
            if (std.mem.eql(u8, bm.type_name, "bytes") or std.mem.eql(u8, bm.type_name, "str"))
                return "byte";
        }
        if (bm.kind == .scalar and (std.mem.eql(u8, bm.type_name, "bytes") or std.mem.eql(u8, bm.type_name, "str")))
            return "byte";
    }
    return null;
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
        .optional => |inner| {
            // `*T?` → unwrap to `*T` with stride size(T). Nested pointers
            // (`List<*Value>` holds `*(*Value)?`) must not report sizeof(Value).
            if (inner.* == .pointer or inner.* == .pointer_const) {
                const pointee = switch (inner.*) {
                    .pointer, .pointer_const => |p| p,
                    else => unreachable,
                };
                const tn = try resolveRetName(a, structs, pointee);
                const es: u32 = switch (pointee.*) {
                    .pointer, .pointer_const, .optional => 8,
                    .name => |n| blk: {
                        if (std.mem.eql(u8, n, "byte") or std.mem.eql(u8, n, "u8") or std.mem.eql(u8, n, "bool"))
                            break :blk 1;
                        if (std.mem.eql(u8, n, "str") or std.mem.eql(u8, n, "bytes")) break :blk 16;
                        // Struct / scalar: leave 0 so force_unwrap uses layoutSizeOf(tn).
                        break :blk 0;
                    },
                    else => 0,
                };
                return .{ .kind = .opt_ptr, .type_name = tn, .elem_size = es };
            }
            const tn = try resolveRetName(a, structs, inner);
            return .{ .kind = .opt_ptr, .type_name = tn };
        },
        .pointer, .pointer_const => |inner| {
            // `**T` / `*(*T)` is a machine word. resolveRetName strips all pointer
            // layers, so without this `out: *V` with V=`*Box` looks like `*Box` and
            // `out[0]=` word-copies the whole Box (map.get for pointer values).
            switch (inner.*) {
                .pointer, .pointer_const, .optional => {
                    const leaf = try resolveRetName(a, structs, inner);
                    return .{ .kind = .raw_ptr, .type_name = leaf, .elem_size = 8 };
                },
                else => {},
            }
            const pointee = try resolveRetName(a, structs, inner);
            if (ifaces.contains(pointee)) return .{ .kind = .iface_ptr, .type_name = pointee };
            // All data pointers are raw_ptr so `&p` LEAs the local. struct_ptr is
            // reserved for values that already hold a durable header address
            // (`var b: Box = Box()`), where `&b` is the header pointer itself.
            // Byte pointers must keep elem_size 1 so `s.ptr + i` is not scaled by 8.
            const es: u32 = if (std.mem.eql(u8, pointee, "byte") or std.mem.eql(u8, pointee, "u8") or std.mem.eql(u8, pointee, "bool") or std.mem.eql(u8, pointee, "void"))
                1
            else
                8;
            return .{ .kind = .raw_ptr, .type_name = pointee, .elem_size = es };
        },
        .array => |arr| {
            const en = try resolveRetName(a, structs, arr.elem);
            const es: u32 = if (std.mem.eql(u8, en, "byte") or std.mem.eql(u8, en, "u8") or std.mem.eql(u8, en, "bool")) 1 else 8;
            return .{
                .kind = .array_ptr,
                .type_name = en,
                .array_len = arr.len,
                .elem_size = es,
            };
        },
        .name => |n| {
            // Named `str` / `bytes` are slice headers (`{ ptr, len }`), not scalars.
            // Keep the nominal type_name so `str.method` lookup works; fieldOffset
            // maps the name to `__Slice_byte` for `.ptr` / `.len`.
            if (std.mem.eql(u8, n, "str") or std.mem.eql(u8, n, "bytes")) {
                const elem = try a.create(ast.TypeExpr);
                elem.* = .{ .name = "byte" };
                _ = try ensureSliceStruct(a, structs, elem);
                return .{ .kind = .struct_ptr, .type_name = n };
            }
            return metaForTypeName(n, ifaces);
        },
        .named_app => |na| {
            const mangled = try mangleNamedApp(a, na.name, na.args);
            return metaForTypeName(mangled, ifaces);
        },
        .tuple => |elems| {
            const name = try ensureTupleStructFromTypes(a, structs, elems);
            return .{ .kind = .struct_ptr, .type_name = name };
        },
        .slice_view => |sv| {
            // Views are `{ ptr, len }` — synthetic struct so `.ptr` / `.len` lower.
            const name = try ensureSliceStruct(a, structs, sv.elem);
            return .{ .kind = .struct_ptr, .type_name = name };
        },
        // Function values / callbacks — one word (code address), called via `icall`.
        .iface_fn => return .{ .kind = .scalar, .type_name = "iface_fn" },
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
                if (inferMetaFromExpr(ctx, ds.init)) |m| {
                    if (m.type_name.len > 0) break :blk m.type_name;
                }
                break :blk try ensureTupleStruct(ctx.a, ctx.structs, ds.names.len);
            };
            const sd = ctx.structs.get(tn) orelse return error.UnknownName;
            for (ds.names, 0..) |n, i| {
                if (i >= sd.fields.len) return error.TypeError;
                const off: i64 = @intCast(i * 8);
                const addr = try ctx.fresh();
                const off_s = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = tup, .src1 = off_s });
                const loaded = try ctx.fresh();
                try ctx.emit(.{ .op = .load, .dst = loaded, .src0 = addr });
                const sm = try metaForTypeExpr(ctx.a, sd.fields[i].ty, ctx.structs, ctx.ifaces);
                // Struct tuple elems point at ret_alloc — durable-copy like `var x = …`
                // so later calls do not clobber `val (v, ok) = parse(…)`.
                if (sm.kind == .struct_ptr and sm.type_name.len > 0) {
                    if (ctx.structs.get(sm.type_name)) |vsd| {
                        var nbytes: i64 = 0;
                        for (vsd.fields) |fld| nbytes += typeExprSizeOf(ctx, fld.ty);
                        if (nbytes == 0) nbytes = 8;
                        const sz = try ctx.fresh();
                        try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = nbytes });
                        const cid = ctx.callee_map.get("jogrt_alloc").?;
                        const ptr = try ctx.fresh();
                        var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
                        allo.args[0] = sz;
                        try ctx.emit(allo);
                        try emitWordCopy(ctx, ptr, loaded, nbytes);
                        try ctx.slots.put(ctx.a, n, ptr);
                        try ctx.meta.put(ctx.a, n, sm);
                        continue;
                    }
                }
                try ctx.slots.put(ctx.a, n, loaded);
                try ctx.meta.put(ctx.a, n, sm);
            }
        },
        .var_decl => |vd| {
            var sm: SlotMeta = .{ .kind = .scalar };
            if (vd.ty) |t| sm = try metaForTypeExpr(ctx.a, t, ctx.structs, ctx.ifaces);
            // `var s: []T` — stack-allocate `{ptr,len}` and take its address (like `[2]i64`).
            if (vd.ty) |t| {
                if (t.* == .slice_view) {
                    const first = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = first, .aux = 0 });
                    const second = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = second, .aux = 0 });
                    const ptr = try ctx.fresh();
                    try ctx.emit(.{ .op = .lea_slot, .dst = ptr, .src0 = first });
                    try ctx.slots.put(ctx.a, vd.name, ptr);
                    try ctx.meta.put(ctx.a, vd.name, sm);
                    if (vd.init) |init| {
                        const v = try lowerExpr(ctx, init);
                        try ctx.emit(.{ .op = .copy, .dst = ptr, .src0 = v });
                    }
                    return;
                }
            }
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
            if (vd.init) |init| {
                const v = try lowerExpr(ctx, init);
                if (vd.ty == null) {
                    if (ctx.meta.get(slotNameOf(ctx, v))) |m| sm = m;
                    // struct lit / alloc leave meta on temp — copy from init kind via peek
                    sm = inferMetaFromExpr(ctx, init) orelse sm;
                }
                // `var p: *Box = null` is a machine-word pointer. Do not treat it like a
                // durable `Box` header (that path memcpy's from the initializer address —
                // null → segfault).
                const ty_is_ptr = if (vd.ty) |t| switch (t.*) {
                    .pointer, .pointer_const, .optional => true,
                    else => false,
                } else false;
                // Struct / slice headers from `jogrt_ret_alloc` are ephemeral.
                // Copy into a durable heap block (upward layout matches fieldOffset).
                if (!ty_is_ptr and sm.kind == .struct_ptr and sm.type_name.len > 0) {
                    if (ctx.structs.get(sm.type_name)) |sd| {
                        var nbytes: i64 = 0;
                        for (sd.fields) |fld| nbytes += typeExprSizeOf(ctx, fld.ty);
                        if (nbytes == 0) nbytes = 8;
                        const sz = try ctx.fresh();
                        try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = nbytes });
                        const cid = ctx.callee_map.get("jogrt_alloc").?;
                        const ptr = try ctx.fresh();
                        var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
                        allo.args[0] = sz;
                        try ctx.emit(allo);
                        try emitWordCopy(ctx, ptr, v, nbytes);
                        try ctx.slots.put(ctx.a, vd.name, ptr);
                        try ctx.meta.put(ctx.a, vd.name, sm);
                        return;
                    }
                }
                const slot = try ctx.fresh();
                try ctx.slots.put(ctx.a, vd.name, slot);
                try ctx.emit(.{ .op = .copy, .dst = slot, .src0 = v });
                try ctx.meta.put(ctx.a, vd.name, sm);
                return;
            }
            const slot = try ctx.fresh();
            try ctx.slots.put(ctx.a, vd.name, slot);
            try ctx.emit(.{ .op = .iconst, .dst = slot, .aux = 0 });
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
                if (indexElemBytes(ctx, ix.base)) |nbytes| {
                    // `v` is a pointer to a multi-word value — copy into the slot.
                    try emitWordCopy(ctx, addr, v, nbytes);
                } else if (indexLoadStoreWidth(ctx, ix.base) == 1) {
                    try ctx.emit(.{ .op = .store8, .src0 = addr, .src1 = v });
                } else {
                    try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = v });
                }
            } else if (as.field) |fa| {
                const base = try lowerExpr(ctx, fa.base);
                const off = try fieldOffset(ctx, fa.base, fa.name);
                const addr = try ctx.fresh();
                const off_s = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = off_s });
                const nbytes = try fieldSizeBytes(ctx, fa.base, fa.name);
                if (nbytes > 8) {
                    // `v` is a pointer to a wide value (str/bytes/struct) — copy words in.
                    try emitWordCopy(ctx, addr, v, nbytes);
                } else {
                    try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = v });
                }
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
            var ret_slot: u32 = if (re) |e| try lowerExpr(ctx, e) else none;
            if (re != null and ret_slot != none) {
                if (ctx.ret_iface) |iface| {
                    const concrete = if (re) |e| (inferTypeNameFromExpr(ctx, e) orelse "") else "";
                    // `&heap_value` / concrete value returned as iface — build fat pointer.
                    if (concrete.len > 0 and !std.mem.eql(u8, concrete, iface)) {
                        ret_slot = try coerceToIface(ctx, ret_slot, concrete, iface);
                    } else if (concrete.len == 0 or std.mem.eql(u8, concrete, iface)) {
                        // `return &heap_value` — recover concrete from the global's declared type.
                        if (re) |e| {
                            if (e.* == .unary and e.unary.op == .addr and e.unary.expr.* == .ident) {
                                const gname = e.unary.expr.ident;
                                const inner = blk: {
                                    if (inferTypeNameFromExpr(ctx, e.unary.expr)) |tn| break :blk tn;
                                    if (ctx.global_map.get(gname)) |gi| {
                                        if (gi < ctx.globals.len and ctx.globals[gi].type_name.len > 0)
                                            break :blk ctx.globals[gi].type_name;
                                    }
                                    break :blk "";
                                };
                                if (inner.len > 0 and !std.mem.eql(u8, inner, iface)) {
                                    ret_slot = try coerceToIface(ctx, ret_slot, inner, iface);
                                }
                            }
                        }
                    }
                }
            }
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

/// Prefer stored return meta (keeps `*T` as raw_ptr); fall back to name-only heuristic.
fn metaFromCallRet(ctx: *Ctx, ret_meta: ?SlotMeta, ret_name: []const u8) ?SlotMeta {
    if (ret_meta) |m| return m;
    if (ret_name.len == 0) return null;
    if (ctx.structs.contains(ret_name))
        return .{ .kind = .struct_ptr, .type_name = ret_name };
    return .{ .kind = .scalar, .type_name = ret_name };
}

fn inferMetaFromExpr(ctx: *Ctx, e: *ast.Expr) ?SlotMeta {
    switch (e.*) {
        .struct_lit => |sl| {
            if (sl.type_args.len > 0) {
                var resolved = ctx.a.alloc(*ast.TypeExpr, sl.type_args.len) catch return .{ .kind = .struct_ptr, .type_name = sl.type_name };
                for (sl.type_args, 0..) |ta, i| {
                    const rn = resolveTypeExprName(ctx, ta) catch typeNameLeaf(mangleTypeArgName(ta));
                    const te = ctx.a.create(ast.TypeExpr) catch return .{ .kind = .struct_ptr, .type_name = sl.type_name };
                    te.* = .{ .name = rn };
                    resolved[i] = te;
                }
                const mangled = mangleNamedApp(ctx.a, sl.type_name, resolved) catch return .{ .kind = .struct_ptr, .type_name = sl.type_name };
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
                if (gi < ctx.globals.len) {
                    if (ctx.globals[gi].is_raw_byte_ptr)
                        return .{ .kind = .raw_ptr, .type_name = "byte", .elem_size = 1 };
                    if (ctx.globals[gi].iface_type_name.len > 0)
                        return .{ .kind = .iface_ptr, .type_name = ctx.globals[gi].iface_type_name };
                    if (ctx.globals[gi].is_float)
                        return .{ .kind = .scalar, .type_name = "f64" };
                }
            }
            if (ctx.object_map.contains(n)) return .{ .kind = .struct_ptr, .type_name = n };
            return null;
        },
        .field => |f| {
            // Pack/array_ptr: `.ptr` keeps array_ptr meta (downward `+` scale); `.len` is scalar.
            if (f.base.* == .ident) {
                if (ctx.meta.get(f.base.ident)) |bm| {
                    if (bm.kind == .array_ptr) {
                        if (std.mem.eql(u8, f.name, "ptr"))
                            return .{ .kind = .array_ptr, .type_name = bm.type_name, .elem_size = bm.elem_size, .len_slot = bm.len_slot };
                        if (std.mem.eql(u8, f.name, "len"))
                            return .{ .kind = .scalar, .type_name = "usize" };
                    }
                }
            }
            var base_tn = inferTypeNameFromExpr(ctx, f.base) orelse return null;
            // Nominal `str`/`bytes` → `__Slice_byte` for field types (`.ptr` is `*byte`).
            if (std.mem.eql(u8, base_tn, "str") or std.mem.eql(u8, base_tn, "bytes")) {
                const elem = ctx.a.create(ast.TypeExpr) catch return null;
                elem.* = .{ .name = "byte" };
                base_tn = ensureSliceStruct(ctx.a, ctx.structs, elem) catch return null;
            }
            const sd = ctx.structs.get(base_tn) orelse return null;
            for (sd.fields) |fld| {
                if (std.mem.eql(u8, fld.name, f.name)) {
                    return metaForTypeExpr(ctx.a, fld.ty, ctx.structs, ctx.ifaces) catch null;
                }
            }
            return null;
        },
        .index => |ix| {
            // Element of `[N]Iface` / `[N]Struct` / `*T` — recover meta for method calls.
            if (inferMetaFromExpr(ctx, ix.base)) |bm| {
                if ((bm.kind == .array_ptr or bm.kind == .raw_ptr) and bm.type_name.len > 0) {
                    if (ctx.ifaces.contains(bm.type_name))
                        return .{ .kind = .iface_ptr, .type_name = bm.type_name };
                    if (ctx.structs.contains(bm.type_name))
                        return .{ .kind = .struct_ptr, .type_name = bm.type_name };
                    return .{ .kind = .scalar, .type_name = bm.type_name, .elem_size = bm.elem_size };
                }
            }
            return null;
        },
        .slice => |sl| {
            const elem = sliceElemTypeName(ctx, sl.base) orelse return null;
            const name = ensureSliceStructFromElemName(ctx.a, ctx.structs, elem) catch return null;
            return .{ .kind = .struct_ptr, .type_name = name };
        },
        .array_lit => |al| return .{ .kind = .array_ptr, .type_name = "i64", .array_len = al.elems.len, .elem_size = 8 },
        .binary => |b| {
            if (binOpMethodName(b.op)) |opm| {
                if (inferTypeNameFromExpr(ctx, b.left)) |tn| {
                    if (!isBuiltinScalar(tn) and ctx.structs.contains(tn)) {
                        const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ tn, opm }) catch return null;
                        if (ctx.methods.get(key)) |mi| {
                            if (metaFromCallRet(ctx, mi.ret_meta, mi.ret_name)) |m| return m;
                        }
                    }
                }
            }
            // `p + i` / `p - i` keep pointer meta so nested `store_elem(p+i, …)` scales.
            if (b.op == .add or b.op == .sub) {
                if (inferMetaFromExpr(ctx, b.left)) |lm| {
                    if (lm.kind == .raw_ptr or lm.kind == .opt_ptr or lm.kind == .array_ptr)
                        return lm;
                }
            }
            // Untyped `var x = a + b` must keep f64 so later `x + y` uses fadd (not integer add).
            if (isFloatish(ctx, b.left) or isFloatish(ctx, b.right))
                return .{ .kind = .scalar, .type_name = "f64" };
            return .{ .kind = .scalar, .type_name = "i64" };
        },
        .call => |c| {
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "bytes")) {
                const elem = ctx.a.create(ast.TypeExpr) catch return null;
                elem.* = .{ .name = "byte" };
                const sn = ensureSliceStruct(ctx.a, ctx.structs, elem) catch return null;
                return .{ .kind = .struct_ptr, .type_name = sn };
            }
            if (c.callee.* == .ident and c.args.len == 0 and c.type_args.len == 0 and ctx.structs.contains(c.callee.ident)) {
                return .{ .kind = .struct_ptr, .type_name = c.callee.ident };
            }
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
                    // Resolve through type_subst so `List<T>()` inside List__i64 methods
                    // yields struct_ptr meta for `List__i64` (clone / map / filter).
                    var resolved = ctx.a.alloc(*ast.TypeExpr, c.type_args.len) catch return null;
                    for (c.type_args, 0..) |ta, i| {
                        const rn = resolveTypeExprName(ctx, ta) catch return null;
                        const te = ctx.a.create(ast.TypeExpr) catch return null;
                        te.* = .{ .name = rn };
                        resolved[i] = te;
                    }
                    const mangled = mangleNamedApp(ctx.a, c.callee.ident, resolved) catch "";
                    if (mangled.len > 0) {
                        if (ctx.structs.contains(mangled) and c.args.len == 0)
                            return .{ .kind = .struct_ptr, .type_name = mangled };
                        if (ctx.fn_infos.get(mangled)) |fi| {
                            if (metaFromCallRet(ctx, fi.ret_meta, fi.ret_name)) |m| return m;
                        }
                    }
                }
                if (ctx.fn_infos.get(c.callee.ident)) |fi| {
                    if (metaFromCallRet(ctx, fi.ret_meta, fi.ret_name)) |m| return m;
                }
            }
            if (c.callee.* == .field) {
                // `pkg.fn(...)` after import — same flatten rule as lowerCall.
                if (c.callee.field.base.* == .ident) {
                    const base_name = c.callee.field.base.ident;
                    const is_local = ctx.meta.contains(base_name) or ctx.global_map.contains(base_name) or
                        ctx.object_map.contains(base_name) or ctx.const_vals.contains(base_name);
                    if (!is_local) {
                        if (c.type_args.len > 0) {
                            const mangled = mangleNamedApp(ctx.a, c.callee.field.name, c.type_args) catch "";
                            if (mangled.len > 0) {
                                if (ctx.fn_infos.get(mangled)) |fi| {
                                    if (metaFromCallRet(ctx, fi.ret_meta, fi.ret_name)) |m| return m;
                                }
                            }
                        }
                        if (ctx.fn_infos.get(c.callee.field.name)) |fi| {
                            if (metaFromCallRet(ctx, fi.ret_meta, fi.ret_name)) |m| return m;
                        }
                    }
                }
                var base_tn = inferTypeNameFromExpr(ctx, c.callee.field.base) orelse "";
                if (std.mem.eql(u8, base_tn, "__Slice_byte")) base_tn = "str";
                const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ base_tn, c.callee.field.name }) catch return .{ .kind = .raw_ptr };
                if (ctx.methods.get(key)) |mi| {
                    if (metaFromCallRet(ctx, mi.ret_meta, mi.ret_name)) |m| return m;
                }
                if (std.mem.eql(u8, base_tn, "str")) {
                    const alt = std.fmt.allocPrint(ctx.a, "bytes.{s}", .{c.callee.field.name}) catch return .{ .kind = .raw_ptr };
                    if (ctx.methods.get(alt)) |mi| {
                        if (metaFromCallRet(ctx, mi.ret_meta, mi.ret_name)) |m| return m;
                    }
                }
                // Interface method: ret type lives on the iface sig, not methods map.
                if (base_tn.len > 0) {
                    if (ctx.ifaces.get(base_tn)) |iface| {
                        for (iface.methods) |ms| {
                            if (std.mem.eql(u8, ms.name, c.callee.field.name)) {
                                if (ms.ret) |r| {
                                    return metaForTypeExpr(ctx.a, r, ctx.structs, ctx.ifaces) catch null;
                                }
                                return null;
                            }
                        }
                    }
                }
            }
            return .{ .kind = .raw_ptr };
        },
        .cast => |c| return metaForTypeExpr(ctx.a, c.ty, ctx.structs, ctx.ifaces) catch null,
        .tuple => |elems| return .{ .kind = .struct_ptr, .type_name = ensureTupleStruct(ctx.a, ctx.structs, elems.len) catch return null },
        .string => {
            const elem = ctx.a.create(ast.TypeExpr) catch return null;
            elem.* = .{ .name = "byte" };
            _ = ensureSliceStruct(ctx.a, ctx.structs, elem) catch return null;
            // Nominal `str` so method calls (`s.starts_with`) resolve.
            return .{ .kind = .struct_ptr, .type_name = "str" };
        },
        .force_unwrap => |inner| {
            // `p!!` on `*T?` → `*T` meta so `p!![i]` / `p!! + i` scale by size(T).
            if (inferMetaFromExpr(ctx, inner)) |m| {
                if (m.kind == .opt_ptr) {
                    const tn = resolveTypeSubst(ctx, m.type_name);
                    // Prefer elem_size from `*(*T)?` optional meta (pointer elements → 8).
                    const es: u32 = if (m.elem_size > 0)
                        m.elem_size
                    else
                        @intCast(@max(layoutSizeOf(ctx, tn), 1));
                    if (ctx.ifaces.contains(tn))
                        return .{ .kind = .iface_ptr, .type_name = tn };
                    if (isBuiltinScalar(tn) or std.mem.eql(u8, tn, "void"))
                        return .{ .kind = .raw_ptr, .type_name = tn, .elem_size = es };
                    // Pointer to struct value: still raw address arithmetic by struct size.
                    return .{ .kind = .raw_ptr, .type_name = tn, .elem_size = es };
                }
                return m;
            }
            return null;
        },
        else => return null,
    }
}

fn inferTypeNameFromExpr(ctx: *Ctx, e: *ast.Expr) ?[]const u8 {
    if (inferMetaFromExpr(ctx, e)) |m| {
        if (m.type_name.len > 0) return m.type_name;
    }
    return null;
}

fn indexBaseMeta(ctx: *Ctx, base_expr: *ast.Expr) ?SlotMeta {
    if (base_expr.* == .ident) {
        const name = base_expr.ident;
        if (ctx.meta.get(name)) |m| return m;
        // Package `var x: *byte` — type lives on Global, not local ctx.meta.
        if (ctx.global_map.get(name)) |gi| {
            if (gi < ctx.globals.len and ctx.globals[gi].is_raw_byte_ptr) {
                return .{ .kind = .raw_ptr, .type_name = "byte", .elem_size = 1 };
            }
        }
    }
    // `s.ptr[i]` / any expr whose meta is a byte pointer — stride 1 upward.
    if (inferMetaFromExpr(ctx, base_expr)) |m| {
        if (m.kind == .raw_ptr or m.kind == .array_ptr) return m;
    }
    return null;
}

fn indexLoadStoreWidth(ctx: *Ctx, base_expr: *ast.Expr) u32 {
    // 1 = byte load/store; 8 = qword.
    if (indexBaseMeta(ctx, base_expr)) |m| {
        if (std.mem.eql(u8, m.type_name, "byte") or std.mem.eql(u8, m.type_name, "u8") or std.mem.eql(u8, m.type_name, "bool"))
            return 1;
    }
    return 8;
}

fn indexElemBytes(ctx: *Ctx, base_expr: *ast.Expr) ?i64 {
    if (indexBaseMeta(ctx, base_expr)) |bm| {
        // `ptr!![i]` carries elem_size from force_unwrap (Slot stride). Word-sized
        // raw pointers (`**T`, `*i64`) must not fall through to `sizeof(T)`.
        if (bm.kind == .raw_ptr) {
            if (bm.elem_size > 8) return bm.elem_size;
            return null;
        }
        if (std.mem.eql(u8, bm.type_name, "str") or std.mem.eql(u8, bm.type_name, "bytes"))
            return 16;
        if (ctx.structs.contains(bm.type_name)) {
            const sz = layoutSizeOf(ctx, bm.type_name);
            if (sz > 8) return sz;
        }
    }
    return null;
}

fn emitWordCopy(ctx: *Ctx, dst: u32, src: u32, nbytes: i64) LowerError!void {
    var off: i64 = 0;
    while (off < nbytes) : (off += 8) {
        const o = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = o, .aux = off });
        const sa = try ctx.fresh();
        try ctx.emit(.{ .op = .add, .dst = sa, .src0 = src, .src1 = o });
        const word = try ctx.fresh();
        try ctx.emit(.{ .op = .load, .dst = word, .src0 = sa });
        const da = try ctx.fresh();
        try ctx.emit(.{ .op = .add, .dst = da, .src0 = dst, .src1 = o });
        try ctx.emit(.{ .op = .store, .src0 = da, .src1 = word });
    }
}

fn indexStride(ctx: *Ctx, base_expr: *ast.Expr) u32 {
    // Dense packed byte arrays / raw byte pointers use stride 1.
    if (indexBaseMeta(ctx, base_expr)) |m| {
        if (m.kind == .raw_ptr and (std.mem.eql(u8, m.type_name, "byte") or std.mem.eql(u8, m.type_name, "u8")))
            return 1;
        if (m.kind == .array_ptr and m.elem_size == 1)
            return 1;
        if (m.kind == .raw_ptr or m.kind == .array_ptr) {
            if (std.mem.eql(u8, m.type_name, "str") or std.mem.eql(u8, m.type_name, "bytes"))
                return 16;
            // Prefer explicit elem_size (`*(*Value)?` → 8) before sizeof(type_name):
            // type_name may be the peeled struct (`Value`) while elements are pointers.
            if (m.elem_size > 0) return m.elem_size;
            if (ctx.structs.contains(m.type_name)) {
                const sz = layoutSizeOf(ctx, m.type_name);
                if (sz > 0) return @intCast(sz);
            }
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
        if (indexBaseMeta(ctx, base_expr)) |m| {
            if (m.kind == .raw_ptr) break :blk true;
            if (m.kind == .array_ptr and m.elem_size == 1) break :blk true;
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

/// `a[..]` / `a[i..j]` → stack `{ ptr, len }` header (same layout as `[]T`).
fn lowerSliceAccess(ctx: *Ctx, sl: ast.SliceAccess) LowerError!u32 {
    const elem = sliceElemTypeName(ctx, sl.base) orelse {
        std.debug.print("lower TypeError slice: unknown elem type\n", .{});
        return error.TypeError;
    };
    _ = try ensureSliceStructFromElemName(ctx.a, ctx.structs, elem);

    const base_meta = inferMetaFromExpr(ctx, sl.base);
    const is_byte = std.mem.eql(u8, elem, "byte") or std.mem.eql(u8, elem, "u8") or std.mem.eql(u8, elem, "bool");
    const stride: i64 = if (is_byte) 1 else 8;

    // Data pointer and full length of the sliced value.
    var data_base: u32 = undefined;
    var full_len: u32 = undefined;
    var upward = is_byte;

    if (base_meta) |bm| {
        if (bm.kind == .array_ptr) {
            data_base = try lowerExpr(ctx, sl.base);
            if (bm.array_len > 0) {
                full_len = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = full_len, .aux = @intCast(bm.array_len) });
            } else if (bm.len_slot != none) {
                full_len = bm.len_slot;
            } else {
                return error.TypeError;
            }
            upward = bm.elem_size == 1 or is_byte;
        } else {
            // Existing slice / `{ptr,len}` header — load fields.
            const hdr = try lowerExpr(ctx, sl.base);
            data_base = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = data_base, .src0 = hdr });
            const len_addr = try ctx.fresh();
            const eight = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
            try ctx.emit(.{ .op = .add, .dst = len_addr, .src0 = hdr, .src1 = eight });
            full_len = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = full_len, .src0 = len_addr });
            upward = true;
        }
    } else {
        return error.TypeError;
    }

    const start_v: u32 = if (sl.start) |s|
        try lowerExpr(ctx, s)
    else blk: {
        const z = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
        break :blk z;
    };
    const end_v: u32 = if (sl.end) |en|
        try lowerExpr(ctx, en)
    else
        full_len;

    // ptr = data_base ± start * stride
    const scale = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = scale, .aux = stride });
    const off = try ctx.fresh();
    try ctx.emit(.{ .op = .mul, .dst = off, .src0 = start_v, .src1 = scale });
    const data_ptr = try ctx.fresh();
    if (upward) {
        try ctx.emit(.{ .op = .add, .dst = data_ptr, .src0 = data_base, .src1 = off });
    } else {
        try ctx.emit(.{ .op = .sub, .dst = data_ptr, .src0 = data_base, .src1 = off });
    }

    // len = end - start
    const len_v = try ctx.fresh();
    try ctx.emit(.{ .op = .sub, .dst = len_v, .src0 = end_v, .src1 = start_v });

    // Heap `{ ptr, len }` via ret arena (same as struct_lit). Stack lea_slot + field
    // offsets grow opposite directions on the frame, so a stack header is unsafe
    // when `len == 8` (the byte offset of `.len`) collides with nearby slots.
    const sz = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = 16 });
    const cid = ctx.callee_map.get("jogrt_ret_alloc").?;
    const hdr = try ctx.fresh();
    var allo = Inst{ .op = .call, .dst = hdr, .aux = cid, .arity = 1 };
    allo.args[0] = sz;
    try ctx.emit(allo);
    try ctx.emit(.{ .op = .store, .src0 = hdr, .src1 = data_ptr });
    const len_addr = try ctx.fresh();
    const eight = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
    try ctx.emit(.{ .op = .add, .dst = len_addr, .src0 = hdr, .src1 = eight });
    try ctx.emit(.{ .op = .store, .src0 = len_addr, .src1 = len_v });
    return hdr;
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
    // Array / pack for-in: for x in xs
    const base = try lowerExpr(ctx, fs.iter);
    var alen: u64 = 0;
    var runtime_len: ?u32 = null;
    if (fs.iter.* == .ident) {
        if (ctx.meta.get(fs.iter.ident)) |m| {
            if (m.kind == .array_ptr) {
                if (m.array_len > 0) alen = m.array_len;
                if (m.len_slot != none) runtime_len = m.len_slot;
            }
        }
    } else if (fs.iter.* == .array_lit) {
        alen = fs.iter.array_lit.elems.len;
    }
    if (alen == 0 and runtime_len == null) return error.TypeError;
    const iv = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = iv, .aux = 0 });
    const head: i64 = @intCast(ctx.insts.items.len);
    const lim = try ctx.fresh();
    if (runtime_len) |ls| {
        try ctx.emit(.{ .op = .copy, .dst = lim, .src0 = ls });
    } else {
        try ctx.emit(.{ .op = .iconst, .dst = lim, .aux = @intCast(alen) });
    }
    const cmp = try ctx.fresh();
    try ctx.emit(.{ .op = .lt, .dst = cmp, .src0 = iv, .src1 = lim });
    const jz_i = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = cmp, .aux = 0 });
    // elem = load(base - iv*stride) — stack arrays / pack buffers grow downward from lea
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

fn packParamIndex(params: []ast.Param) ?u8 {
    for (params, 0..) |p, i| {
        if (p.is_pack) return @intCast(i);
    }
    return null;
}

fn fieldStructName(ctx: *Ctx, base_expr: *ast.Expr) LowerError![]const u8 {
    var tn = blk: {
        if (base_expr.* == .ident) {
            if (ctx.meta.get(base_expr.ident)) |m| break :blk m.type_name;
        }
        if (inferTypeNameFromExpr(ctx, base_expr)) |t| break :blk t;
        return error.UnknownName;
    };
    tn = resolveTypeSubst(ctx, tn);
    if (std.mem.eql(u8, tn, "str") or std.mem.eql(u8, tn, "bytes")) {
        const elem = try ctx.a.create(ast.TypeExpr);
        elem.* = .{ .name = "byte" };
        tn = try ensureSliceStruct(ctx.a, ctx.structs, elem);
    }
    return tn;
}

fn fieldOffset(ctx: *Ctx, base_expr: *ast.Expr, field: []const u8) LowerError!i64 {
    const tn = fieldStructName(ctx, base_expr) catch {
        std.debug.print("lower UnknownName fieldOffset type for field {s}\n", .{field});
        return error.UnknownName;
    };
    const sd = ctx.structs.get(tn) orelse {
        std.debug.print("lower UnknownName struct {s} field {s}\n", .{ tn, field });
        return error.UnknownName;
    };
    var off: i64 = 0;
    for (sd.fields) |f| {
        if (std.mem.eql(u8, f.name, field)) return off;
        off += fieldSlotSizeOf(ctx, f.ty);
    }
    std.debug.print("lower UnknownName missing field {s}.{s}\n", .{ tn, field });
    return error.UnknownName;
}

fn fieldSizeBytes(ctx: *Ctx, base_expr: *ast.Expr, field: []const u8) LowerError!i64 {
    const tn = try fieldStructName(ctx, base_expr);
    const sd = ctx.structs.get(tn) orelse return error.UnknownName;
    for (sd.fields) |f| {
        if (std.mem.eql(u8, f.name, field)) return fieldSlotSizeOf(ctx, f.ty);
    }
    return error.UnknownName;
}

fn isFloatTypeName(name: []const u8) bool {
    return std.mem.eql(u8, name, "f64") or std.mem.eql(u8, name, "f32");
}

fn isUnsignedTypeName(name: []const u8) bool {
    return std.mem.eql(u8, name, "u64") or
        std.mem.eql(u8, name, "u32") or
        std.mem.eql(u8, name, "u16") or
        std.mem.eql(u8, name, "u8") or
        std.mem.eql(u8, name, "byte") or
        std.mem.eql(u8, name, "usize");
}

/// True when this expr should use unsigned div/rem/cmp (uN locals, casts, or
/// integer literals whose magnitude does not fit in i64).
fn isUnsignedish(ctx: *Ctx, e: *ast.Expr) bool {
    if (inferTypeNameFromExpr(ctx, e)) |tn| {
        if (isUnsignedTypeName(tn)) return true;
    }
    return switch (e.*) {
        .int => |n| n > std.math.maxInt(i64),
        .binary => |b| isUnsignedish(ctx, b.left) or isUnsignedish(ctx, b.right),
        .unary => |u| isUnsignedish(ctx, u.expr),
        .cast => |c| isUnsignedTypeName(typeExprName(c.ty)),
        .ident => |n| blk: {
            if (ctx.meta.get(n)) |m| break :blk isUnsignedTypeName(m.type_name);
            break :blk false;
        },
        else => false,
    };
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
            // `val ErrX = &global` — address of the referenced package storage.
            if (ctx.addr_consts.get(name)) |gi| {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .gaddr, .dst = d, .aux = @intCast(gi) });
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
            const bits = intLiteralBits(n) orelse return error.TypeError;
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = bits });
            return d;
        },
        .float => |n| {
            const d = try ctx.fresh();
            const bits: i64 = @bitCast(n);
            try ctx.emit(.{ .op = .fconst, .dst = d, .aux = bits });
            return d;
        },
        .string => |s| {
            // `str` / `bytes` are `{ ptr, len }` headers — not a bare data pointer.
            return try lowerStringSlice(ctx, s);
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
            // Also `str.op==` / `bytes.op==` (content equality) even though str is a builtin scalar name.
            if (binOpMethodName(b.op)) |opm| {
                if (inferTypeNameFromExpr(ctx, b.left)) |tn| {
                    const recv = if (std.mem.eql(u8, tn, "__Slice_byte")) "str" else tn;
                    const allow = (!isBuiltinScalar(recv) and ctx.structs.contains(recv)) or
                        std.mem.eql(u8, recv, "str") or std.mem.eql(u8, recv, "bytes");
                    if (allow) {
                        const key = try std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ recv, opm });
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
            // Typed pointer ± int: scale by pointee size (`p + i` → p + i*size(T)).
            // Pack/array_ptr buffers grow downward from lea_slot — `p + i` subtracts.
            if ((b.op == .add or b.op == .sub) and !isFloatish(ctx, b.left)) {
                if (inferMetaFromExpr(ctx, b.left)) |lm| {
                    if (lm.kind == .raw_ptr or lm.kind == .opt_ptr or lm.kind == .array_ptr) {
                        const es: i64 = blk: {
                            if (lm.elem_size > 0) break :blk lm.elem_size;
                            break :blk layoutSizeOf(ctx, resolveTypeSubst(ctx, lm.type_name));
                        };
                        if (es >= 1 and (es != 1 or lm.kind == .array_ptr)) {
                            const scaled = try ctx.fresh();
                            const scale = try ctx.fresh();
                            try ctx.emit(.{ .op = .iconst, .dst = scale, .aux = @max(es, 1) });
                            try ctx.emit(.{ .op = .mul, .dst = scaled, .src0 = r, .src1 = scale });
                            const downward = lm.kind == .array_ptr;
                            const op_arith: Op = if (downward)
                                (if (b.op == .add) .sub else .add)
                            else
                                (if (b.op == .add) .add else .sub);
                            try ctx.emit(.{ .op = op_arith, .dst = d, .src0 = l, .src1 = scaled });
                            return d;
                        }
                    }
                }
            }
            const use_float = isFloatish(ctx, b.left) or isFloatish(ctx, b.right);
            const use_unsigned = !use_float and (isUnsignedish(ctx, b.left) or isUnsignedish(ctx, b.right));
            switch (b.op) {
                .add => try ctx.emit(.{ .op = if (use_float) .fadd else .add, .dst = d, .src0 = l, .src1 = r }),
                .sub => try ctx.emit(.{ .op = if (use_float) .fsub else .sub, .dst = d, .src0 = l, .src1 = r }),
                .mul => try ctx.emit(.{ .op = if (use_float) .fmul else .mul, .dst = d, .src0 = l, .src1 = r }),
                .div => try ctx.emit(.{ .op = if (use_float) .fdiv else if (use_unsigned) .udiv else .div, .dst = d, .src0 = l, .src1 = r }),
                .rem => try ctx.emit(.{ .op = if (use_unsigned) .urem else .rem, .dst = d, .src0 = l, .src1 = r }),
                .eq => try ctx.emit(.{ .op = .eq, .dst = d, .src0 = l, .src1 = r }),
                .neq => try ctx.emit(.{ .op = .ne, .dst = d, .src0 = l, .src1 = r }),
                .lt => try ctx.emit(.{ .op = if (use_unsigned) .ult else .lt, .dst = d, .src0 = l, .src1 = r }),
                .gt => try ctx.emit(.{ .op = if (use_unsigned) .ult else .lt, .dst = d, .src0 = r, .src1 = l }),
                .lte => try ctx.emit(.{ .op = if (use_unsigned) .ule else .le, .dst = d, .src0 = l, .src1 = r }),
                .gte => try ctx.emit(.{ .op = if (use_unsigned) .ule else .le, .dst = d, .src0 = r, .src1 = l }),
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
            // Address-of: resolve lvalue before evaluating operand (package &ident → gaddr).
            if (u.op == .addr) {
                if (u.expr.* != .ident) return error.TypeError;
                const name = u.expr.ident;
                const d = try ctx.fresh();
                if (ctx.slots.get(name)) |slot| {
                    // Array / struct / iface slots already hold a data pointer; &x is that pointer
                    // (LEA of the slot would pass a **T to C and break SDL createinfo / FRect FFI).
                    if (ctx.meta.get(name)) |m| {
                        if (m.kind == .array_ptr or m.kind == .struct_ptr or m.kind == .iface_ptr) {
                            try ctx.emit(.{ .op = .copy, .dst = d, .src0 = slot });
                            return d;
                        }
                    }
                    try ctx.emit(.{ .op = .lea_slot, .dst = d, .src0 = slot });
                    return d;
                }
                if (ctx.global_map.get(name)) |gi| {
                    try ctx.emit(.{ .op = .gaddr, .dst = d, .aux = @intCast(gi) });
                    return d;
                }
                if (ctx.object_map.get(name)) |gi| {
                    try ctx.emit(.{ .op = .gaddr, .dst = d, .aux = @intCast(gi) });
                    return d;
                }
                std.debug.print("lower UnknownName addr: {s}\n", .{name});
                return error.UnknownName;
            }
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
                .addr => unreachable,
                .deref => try ctx.emit(.{ .op = .load, .dst = d, .src0 = v }),
            }
            return d;
        },
        .force_unwrap => |inner| {
            // Optional is a nullable pointer/value; !! panics on null then yields the bits.
            const v = try lowerExpr(ctx, inner);
            const ok = try ctx.fresh();
            try ctx.emit(.{ .op = .copy, .dst = ok, .src0 = v });
            const jz_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jz, .src0 = ok, .aux = 0 });
            const jmp_ok = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            const panic_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jz_i].aux = panic_pos;
            const msg = try ctx.a.dupe(u8, "force unwrap of null\n");
            const sidx: i64 = @intCast(ctx.strings.items.len);
            try ctx.strings.append(ctx.a, msg);
            const ptr = try ctx.fresh();
            try ctx.emit(.{ .op = .saddr, .dst = ptr, .aux = sidx });
            const sl = try ctx.fresh();
            try ctx.emit(.{ .op = .slen, .dst = sl, .aux = sidx });
            try ctx.emit(.{ .op = .panic, .src0 = ptr, .src1 = sl });
            const end_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jmp_ok].aux = end_pos;
            return v;
        },
        .index => |ix| {
            try maybeBoundsCheck(ctx, ix.base, ix.index);
            const addr = try lowerIndexAddr(ctx, ix.base, ix.index);
            // Struct / str / bytes elements are multi-word — copy into a fresh header.
            if (indexElemBytes(ctx, ix.base)) |nbytes| {
                const sz = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = nbytes });
                const cid = ctx.callee_map.get("jogrt_ret_alloc").?;
                const dst = try ctx.fresh();
                var allo = Inst{ .op = .call, .dst = dst, .aux = cid, .arity = 1 };
                allo.args[0] = sz;
                try ctx.emit(allo);
                try emitWordCopy(ctx, dst, addr, nbytes);
                return dst;
            }
            const d = try ctx.fresh();
            if (indexLoadStoreWidth(ctx, ix.base) == 1) {
                try ctx.emit(.{ .op = .load8, .dst = d, .src0 = addr });
            } else {
                try ctx.emit(.{ .op = .load, .dst = d, .src0 = addr });
            }
            return d;
        },
        .slice => |sl| return try lowerSliceAccess(ctx, sl),
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
            // Pointer / optional / iface casts are bitcasts of the address word — never narrow.
            const to_is_ptr = c.ty.* == .pointer or c.ty.* == .pointer_const or c.ty.* == .optional or
                to.kind == .raw_ptr or to.kind == .struct_ptr or to.kind == .iface_ptr or to.kind == .opt_ptr;
            // Types.md: numeric `as` is a checked convert, not a bitcast.
            if (!to_is_ptr and src_float and !dst_float) {
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
            if (!to_is_ptr and !src_float and dst_float) {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .sitofp, .dst = d, .src0 = v });
                return d;
            }
            // narrow ints via `as` when target bits known (not pointer targets)
            if (!to_is_ptr) {
                const bits = scalarBits(to_name);
                if (bits > 0 and bits < 64) {
                    const d = try ctx.fresh();
                    try ctx.emit(.{ .op = .narrow, .dst = d, .src0 = v, .aux = bits });
                    return d;
                }
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
                // Pack / array_ptr: `elements.len` / `elements.ptr` (ABI is separate slots).
                if (ctx.meta.get(f.base.ident)) |m| {
                    if (m.kind == .array_ptr) {
                        if (std.mem.eql(u8, f.name, "len")) {
                            if (m.len_slot != none) {
                                const d = try ctx.fresh();
                                try ctx.emit(.{ .op = .copy, .dst = d, .src0 = m.len_slot });
                                return d;
                            }
                            if (m.array_len > 0) {
                                const d = try ctx.fresh();
                                try ctx.emit(.{ .op = .iconst, .dst = d, .aux = @intCast(m.array_len) });
                                return d;
                            }
                        }
                        if (std.mem.eql(u8, f.name, "ptr")) {
                            // The pack local slot itself holds the element pointer.
                            return try lowerExpr(ctx, f.base);
                        }
                    }
                }
                // `pkg.Name` / `alias.Name` after import — flattened merge exposes the leaf.
                const base_name = f.base.ident;
                const is_value = ctx.meta.contains(base_name) or ctx.global_map.contains(base_name) or ctx.object_map.contains(base_name) or ctx.const_vals.contains(base_name) or ctx.addr_consts.contains(base_name) or ctx.slots.contains(base_name);
                if (!is_value and ctx.import_binds.contains(base_name)) {
                    // Synthesize leaf lookup (const / addr_const / global / fn).
                    if (ctx.const_vals.get(f.name)) |cv| {
                        const d = try ctx.fresh();
                        if (cv.is_float) {
                            try ctx.emit(.{ .op = .fconst, .dst = d, .aux = cv.bits });
                        } else {
                            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = cv.bits });
                        }
                        return d;
                    }
                    if (ctx.addr_consts.get(f.name)) |gi| {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .gaddr, .dst = d, .aux = @intCast(gi) });
                        return d;
                    }
                    if (ctx.object_map.get(f.name)) |gi| {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .gaddr, .dst = d, .aux = @intCast(gi) });
                        return d;
                    }
                    if (ctx.global_map.get(f.name)) |gi| {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .gload, .dst = d, .aux = @intCast(gi) });
                        return d;
                    }
                    if (ctx.callee_map.get(f.name)) |cid| {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .saddr, .dst = d, .aux = -1 - @as(i64, @intCast(cid)) });
                        return d;
                    }
                    std.debug.print("lower UnknownName import field: {s}.{s}\n", .{ base_name, f.name });
                    return error.UnknownName;
                }
            }
            const base = try lowerExpr(ctx, f.base);
            const off = try fieldOffset(ctx, f.base, f.name);
            const addr = try ctx.fresh();
            const off_s = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
            try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = off_s });
            const nbytes = try fieldSizeBytes(ctx, f.base, f.name);
            if (nbytes > 8) {
                // Wide field (str/bytes/struct): return a fresh header copy.
                const sz = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = nbytes });
                const cid = ctx.callee_map.get("jogrt_ret_alloc").?;
                const d = try ctx.fresh();
                var allo = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
                allo.args[0] = sz;
                try ctx.emit(allo);
                try emitWordCopy(ctx, d, addr, nbytes);
                return d;
            }
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
            const cid = ctx.callee_map.get("jogrt_ret_alloc").?;
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
            const tname = if (sl.type_args.len > 0) blk: {
                var resolved = try ctx.a.alloc(*ast.TypeExpr, sl.type_args.len);
                for (sl.type_args, 0..) |ta, i| {
                    const rn = try resolveTypeExprName(ctx, ta);
                    const te = try ctx.a.create(ast.TypeExpr);
                    te.* = .{ .name = rn };
                    resolved[i] = te;
                }
                break :blk try mangleNamedApp(ctx.a, sl.type_name, resolved);
            }
            else
                sl.type_name;
            const sd = ctx.structs.get(tname) orelse {
                std.debug.print("struct_lit missing {s}\n", .{tname});
                return error.UnknownName;
            };
            var size: i64 = 0;
            for (sd.fields) |f| size += fieldSlotSizeOf(ctx, f.ty);
            if (size == 0) size = 8;
            const sz = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = size });
            // Circular ret arena — hot-path struct returns no longer OOM the 4 MiB bump.
            const cid = ctx.callee_map.get("jogrt_ret_alloc").?;
            const ptr = try ctx.fresh();
            var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
            allo.args[0] = sz;
            try ctx.emit(allo);
            for (sl.fields) |flit| {
                var off: i64 = 0;
                var found = false;
                var fty: ?*ast.TypeExpr = null;
                for (sd.fields) |f| {
                    if (std.mem.eql(u8, f.name, flit.name)) {
                        found = true;
                        fty = f.ty;
                        break;
                    }
                    off += fieldSlotSizeOf(ctx, f.ty);
                }
                if (!found) return error.UnknownName;
                const val = try lowerExpr(ctx, flit.value);
                const addr = try ctx.fresh();
                const off_s = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                try ctx.emit(.{ .op = .add, .dst = addr, .src0 = ptr, .src1 = off_s });
                const nbytes = fieldSlotSizeOf(ctx, fty.?);
                if (nbytes > 8) {
                    try emitWordCopy(ctx, addr, val, nbytes);
                } else {
                    try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = val });
                }
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

/// Allocate a zeroed struct/slice header value (used for `Type()` and `bytes()`).
fn lowerEmptyStructValue(ctx: *Ctx, tname: []const u8) LowerError!u32 {
    const sd = ctx.structs.get(tname) orelse {
        std.debug.print("struct_lit missing {s}\n", .{tname});
        return error.UnknownName;
    };
    var size: i64 = 0;
    for (sd.fields) |f| size += fieldSlotSizeOf(ctx, f.ty);
    if (size == 0) size = 8;
    const sz = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = size });
    const cid = ctx.callee_map.get("jogrt_ret_alloc").?;
    const ptr = try ctx.fresh();
    var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
    allo.args[0] = sz;
    try ctx.emit(allo);
    var off: i64 = 0;
    for (sd.fields) |f| {
        const nbytes = fieldSlotSizeOf(ctx, f.ty);
        var b: i64 = 0;
        while (b < nbytes) : (b += 8) {
            const z = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
            const addr = try ctx.fresh();
            const off_s = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off + b });
            try ctx.emit(.{ .op = .add, .dst = addr, .src0 = ptr, .src1 = off_s });
            try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = z });
        }
        off += nbytes;
    }
    return ptr;
}

fn layoutSizeOfName(name: []const u8) i64 {
    const leaf = typeNameLeaf(name);
    // `PValue` / `Opt…` from type_subst / mangling — machine-word / optional pointer.
    if (leaf.len > 0 and (leaf[0] == 'P' or std.mem.startsWith(u8, leaf, "Opt")))
        return 8;
    if (std.mem.eql(u8, leaf, "u8") or std.mem.eql(u8, leaf, "i8") or std.mem.eql(u8, leaf, "byte") or std.mem.eql(u8, leaf, "bool") or std.mem.eql(u8, leaf, "char"))
        return 1;
    if (std.mem.eql(u8, leaf, "u16") or std.mem.eql(u8, leaf, "i16")) return 2;
    if (std.mem.eql(u8, leaf, "u32") or std.mem.eql(u8, leaf, "i32") or std.mem.eql(u8, leaf, "f32") or std.mem.eql(u8, leaf, "int"))
        return 4;
    if (std.mem.eql(u8, leaf, "str") or std.mem.eql(u8, leaf, "bytes"))
        return 16; // `{ ptr, len }` slice header
    // pointers, 64-bit scalars: 8. Struct sizes need the structs map — see layoutSizeOf.
    return 8;
}

fn layoutAlignOfName(name: []const u8) i64 {
    const leaf = typeNameLeaf(name);
    if (std.mem.eql(u8, leaf, "str") or std.mem.eql(u8, leaf, "bytes")) return 8;
    return layoutSizeOfName(name); // power-of-two scalars; pointers 8
}

fn layoutSizeOf(ctx: *Ctx, name: []const u8) i64 {
    const resolved = resolveTypeSubst(ctx, name);
    const leaf = typeNameLeaf(resolved);
    if (leaf.len > 0 and (leaf[0] == 'P' or std.mem.startsWith(u8, leaf, "Opt")))
        return 8;
    if (std.mem.eql(u8, leaf, "str") or std.mem.eql(u8, leaf, "bytes")) return 16;
    if (ctx.structs.get(leaf)) |sd| {
        var total: i64 = 0;
        for (sd.fields) |f| {
            total += fieldSlotSizeOf(ctx, f.ty);
        }
        if (total == 0) return 8;
        return total;
    }
    return layoutSizeOfName(leaf);
}

/// In-struct field stride: scalars widen to 8; str/bytes stay 16; nested structs sum.
fn fieldSlotSizeOf(ctx: *Ctx, te: *ast.TypeExpr) i64 {
    return switch (te.*) {
        .name => |n| blk: {
            const leaf = typeNameLeaf(resolveTypeSubst(ctx, n));
            if (std.mem.eql(u8, leaf, "str") or std.mem.eql(u8, leaf, "bytes")) break :blk 16;
            if (ctx.structs.get(leaf)) |_| break :blk layoutSizeOf(ctx, leaf);
            break :blk 8; // u32/u8/bool/… stored as 8-byte slots
        },
        .named_app => |na| blk: {
            const mangled = mangleNamedApp(ctx.a, na.name, na.args) catch break :blk 8;
            break :blk layoutSizeOf(ctx, mangled);
        },
        .optional, .pointer, .pointer_const => 8,
        .slice_view => 16,
        .tuple => |elems| blk: {
            var t: i64 = 0;
            for (elems) |e| t += fieldSlotSizeOf(ctx, e);
            break :blk if (t == 0) @as(i64, 8) else t;
        },
        else => 8,
    };
}

fn typeExprSizeOf(ctx: *Ctx, te: *ast.TypeExpr) i64 {
    return fieldSlotSizeOf(ctx, te);
}

fn resolveTypeSubst(ctx: *Ctx, name: []const u8) []const u8 {
    const leaf = typeNameLeaf(name);
    if (ctx.type_subst.get(leaf)) |r| return r;
    return name;
}

/// Resolve a type expr to a layout/lookup name with generic subst applied.
fn resolveTypeExprName(ctx: *Ctx, te: *ast.TypeExpr) LowerError![]const u8 {
    return switch (te.*) {
        .name => |n| resolveTypeSubst(ctx, n),
        .named_app => |na| blk: {
            var args = try ctx.a.alloc(*ast.TypeExpr, na.args.len);
            for (na.args, 0..) |arg, i| {
                const rn = try resolveTypeExprName(ctx, arg);
                const te2 = try ctx.a.create(ast.TypeExpr);
                te2.* = .{ .name = rn };
                args[i] = te2;
            }
            break :blk try mangleNamedApp(ctx.a, na.name, args);
        },
        .optional => |inner| try resolveTypeExprName(ctx, inner),
        .pointer, .pointer_const => |inner| try resolveTypeExprName(ctx, inner),
        .array => |arr| try resolveTypeExprName(ctx, arr.elem),
        .tuple => |elems| try ensureTupleStructFromTypes(ctx.a, ctx.structs, elems),
        .slice_view => |sv| blk: {
            const en = try resolveTypeExprName(ctx, sv.elem);
            const elem = try ctx.a.create(ast.TypeExpr);
            elem.* = .{ .name = en };
            break :blk try ensureSliceStruct(ctx.a, ctx.structs, elem);
        },
        else => "",
    };
}

fn lowerCall(ctx: *Ctx, c: ast.Call) LowerError!u32 {
    if (c.callee.* == .ident) {
        const name = c.callee.ident;
        // Empty byte slice header `{ ptr: null, len: 0 }`.
        if (std.mem.eql(u8, name, "bytes")) {
            if (c.args.len != 0) return error.TooManyArgs;
            return try lowerEmptyStructValue(ctx, blk: {
                const elem = try ctx.a.create(ast.TypeExpr);
                elem.* = .{ .name = "byte" };
                break :blk try ensureSliceStruct(ctx.a, ctx.structs, elem);
            });
        }
        // Zero-value struct: `StrBuilder()` / `ByteIter()` when Name is a struct type.
        if (c.args.len == 0 and c.type_args.len == 0 and ctx.structs.contains(name)) {
            return try lowerEmptyStructValue(ctx, name);
        }
        if (std.mem.eql(u8, name, "size") or std.mem.eql(u8, name, "align")) {
            // Value form `size(i64)` or type form `size(Slot<K,V>)` / `size(*Value)`.
            // Pointer type exprs and `P…` subst names are one machine word.
            const n: i64 = blk: {
                if (c.type_args.len == 1) {
                    const te = c.type_args[0];
                    switch (te.*) {
                        .pointer, .pointer_const, .optional => break :blk 8,
                        else => {},
                    }
                    const tname = try resolveTypeExprName(ctx, te);
                    break :blk if (std.mem.eql(u8, name, "size")) layoutSizeOf(ctx, tname) else layoutAlignOfName(tname);
                }
                if (c.args.len != 1) return error.TypeError;
                const arg = c.args[0];
                const tname = blk2: {
                    if (arg.* == .ident) break :blk2 resolveTypeSubst(ctx, arg.ident);
                    if (inferTypeNameFromExpr(ctx, arg)) |tn| break :blk2 resolveTypeSubst(ctx, tn);
                    break :blk2 "i64";
                };
                break :blk if (std.mem.eql(u8, name, "size")) layoutSizeOf(ctx, tname) else layoutAlignOfName(tname);
            };
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = n });
            return d;
        }
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
        // Freeable bump arena (struct-return Friction): checkpoint/reset between frames.
        if (std.mem.eql(u8, name, "alloc_checkpoint")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const cid = ctx.callee_map.get("jogrt_alloc_checkpoint").?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "alloc_reset")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const cp = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_alloc_reset").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
            inst.args[0] = cp;
            try ctx.emit(inst);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = d, .aux = 0 });
            return d;
        }
        // Native f32 bitcast helpers (drop host libf32bits).
        if (std.mem.eql(u8, name, "f32_bits") or std.mem.eql(u8, name, "jog_f32_bits")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const v = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_f32_bits").?;
            const d = try ctx.fresh();
            var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
            inst.args[0] = v;
            try ctx.emit(inst);
            return d;
        }
        if (std.mem.eql(u8, name, "f32_from_bits")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const v = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_f32_from_bits").?;
            const d = try ctx.fresh();
            var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 1 };
            inst.args[0] = v;
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
            var resolved = try ctx.a.alloc(*ast.TypeExpr, c.type_args.len);
            for (c.type_args, 0..) |ta, i| {
                const rn = try resolveTypeExprName(ctx, ta);
                const te = try ctx.a.create(ast.TypeExpr);
                te.* = .{ .name = rn };
                resolved[i] = te;
            }
            const mangled = try mangleNamedApp(ctx.a, name, resolved);
            // Zero-value generic struct: `List<T>()` / `Map<K,V>()`.
            if (c.args.len == 0 and ctx.structs.contains(mangled)) {
                return try lowerEmptyStructValue(ctx, mangled);
            }
            if (ctx.fn_infos.get(mangled)) |fi| {
                return try lowerFnCall(ctx, fi, c.args, c.with_args);
            }
        }
        if (ctx.fn_infos.get(name)) |fi| {
            return try lowerFnCall(ctx, fi, c.args, c.with_args);
        }
        // Inside specialized bodies (`store_elem` in `List<*Value>.push`): bind via type_subst
        // so `store_elem__PValue` is found even when arg meta peels to `Value`.
        if (c.type_args.len == 0 and ctx.type_subst.count() == 1) {
            var it = ctx.type_subst.iterator();
            if (it.next()) |e| {
                const mangled = try std.fmt.allocPrint(ctx.a, "{s}__{s}", .{ name, e.value_ptr.* });
                if (ctx.fn_infos.get(mangled)) |fi| {
                    return try lowerFnCall(ctx, fi, c.args, c.with_args);
                }
            }
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
        // `pkg.fn(...)` after `import std.pkg` — flattened merge exposes `fn` unqualified.
        // When the base is an unresolved ident (import alias), look up the free function.
        if (c.callee.field.base.* == .ident) {
            const base_name = c.callee.field.base.ident;
            const is_local = ctx.meta.contains(base_name) or ctx.global_map.contains(base_name) or ctx.object_map.contains(base_name) or ctx.const_vals.contains(base_name);
            if (!is_local) {
                const fname = c.callee.field.name;
                if (c.type_args.len > 0) {
                    // Resolve type params through current subst, then mangle: list.empty<T>().
                    var resolved_args = try ctx.a.alloc(*ast.TypeExpr, c.type_args.len);
                    for (c.type_args, 0..) |ta, i| {
                        const rn = try resolveTypeExprName(ctx, ta);
                        const te = try ctx.a.create(ast.TypeExpr);
                        te.* = .{ .name = rn };
                        resolved_args[i] = te;
                    }
                    const mangled = try mangleNamedApp(ctx.a, fname, resolved_args);
                    if (ctx.fn_infos.get(mangled)) |fi| {
                        return try lowerFnCall(ctx, fi, c.args, c.with_args);
                    }
                }
                // Infer from current type_subst (mem.slice inside List<T>.items) or first arg.
                if (ctx.type_subst.count() > 0) {
                    var it = ctx.type_subst.iterator();
                    // Prefer single binding T → concrete for unary generics.
                    if (ctx.type_subst.count() == 1) {
                        if (it.next()) |e| {
                            const mangled = try std.fmt.allocPrint(ctx.a, "{s}__{s}", .{ fname, e.value_ptr.* });
                            if (ctx.fn_infos.get(mangled)) |fi| {
                                return try lowerFnCall(ctx, fi, c.args, c.with_args);
                            }
                        }
                    }
                }
                if (c.args.len >= 1) {
                    if (inferTypeNameFromExpr(ctx, c.args[0])) |atn| {
                        const mangled = try std.fmt.allocPrint(ctx.a, "{s}__{s}", .{ fname, atn });
                        if (ctx.fn_infos.get(mangled)) |fi| {
                            return try lowerFnCall(ctx, fi, c.args, c.with_args);
                        }
                    }
                }
                if (ctx.fn_infos.get(fname)) |fi| {
                    return try lowerFnCall(ctx, fi, c.args, c.with_args);
                }
                std.debug.print("lower UnknownName pkg fn {s}.{s}\n", .{ base_name, fname });
                return error.UnknownName;
            }
        }
        const base = try lowerExpr(ctx, c.callee.field.base);
        return try lowerMethodOrIfaceCall(ctx, c.callee.field.base, base, c.callee.field.name, c.args, c.with_args);
    }
    // Call through a function-typed local/param (`body(x)` in List.each).
    if (c.callee.* == .ident) {
        if (ctx.slots.get(c.callee.ident)) |fp| {
            if (ctx.meta.get(c.callee.ident)) |m| {
                if (std.mem.eql(u8, m.type_name, "iface_fn")) {
                    return try lowerFnPtrCall(ctx, fp, c.args);
                }
            }
        }
    }
    return error.UnknownName;
}

fn lowerFnPtrCall(ctx: *Ctx, fnptr: u32, args: []*ast.Expr) LowerError!u32 {
    var arg_slots: [16]u32 = .{none} ** 16;
    var arity: u8 = 0;
    if (args.len > max_call_args) return error.TooManyArgs;
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

fn lowerFnCall(ctx: *Ctx, fi: FnInfo, args: []*ast.Expr, with_args: []*ast.Expr) LowerError!u32 {
    var arg_slots: [16]u32 = .{none} ** 16;
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

    if (fi.pack_index) |pi| {
        const fixed: usize = pi;
        if (args.len < fixed) return error.TooManyArgs;
        if (fixed + arity > max_call_args) return error.TooManyArgs;
        var i: usize = 0;
        while (i < fixed) : (i += 1) {
            arg_slots[arity] = try lowerExpr(ctx, args[i]);
            arity += 1;
        }
        const pack_n = args.len - fixed;
        // Stack buffer of pack_n slots (same layout as `[N]T` locals).
        const first = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = first, .aux = 0 });
        var j: usize = 1;
        while (j < pack_n) : (j += 1) {
            const s = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = s, .aux = 0 });
        }
        const ptr = try ctx.fresh();
        try ctx.emit(.{ .op = .lea_slot, .dst = ptr, .src0 = first });
        var k: usize = 0;
        while (k < pack_n) : (k += 1) {
            const val = try lowerExpr(ctx, args[fixed + k]);
            const addr = try ctx.fresh();
            const off = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = off, .aux = @intCast(k * 8) });
            try ctx.emit(.{ .op = .sub, .dst = addr, .src0 = ptr, .src1 = off });
            try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = val });
        }
        const len = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = len, .aux = @intCast(pack_n) });
        if (arity + 2 > max_call_args) return error.TooManyArgs;
        arg_slots[arity] = ptr;
        arity += 1;
        arg_slots[arity] = len;
        arity += 1;
    } else {
        if (args.len + arity > max_call_args) return error.TooManyArgs;
        for (args) |arg| {
            arg_slots[arity] = try lowerExpr(ctx, arg);
            arity += 1;
        }
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

/// Address of a `*T` method receiver. Ident struct_ptr locals already hold a
/// durable header pointer; field receivers need `parent + field_offset`.
fn lowerPointerReceiver(ctx: *Ctx, base_expr: *ast.Expr, base_slot: u32) LowerError!u32 {
    switch (base_expr.*) {
        .field => |f| {
            const parent = try lowerExpr(ctx, f.base);
            const off = try fieldOffset(ctx, f.base, f.name);
            if (off == 0) return parent;
            const addr = try ctx.fresh();
            const off_s = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
            try ctx.emit(.{ .op = .add, .dst = addr, .src0 = parent, .src1 = off_s });
            return addr;
        },
        .ident => |name| {
            if (ctx.meta.get(name)) |m| {
                // Durable struct / array / iface locals already store a data pointer.
                if (m.kind == .struct_ptr or m.kind == .array_ptr or m.kind == .iface_ptr)
                    return base_slot;
            }
            // Scalar / raw_ptr local: method wants `&local`.
            if (ctx.slots.get(name)) |slot| {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .lea_slot, .dst = d, .src0 = slot });
                return d;
            }
            return base_slot;
        },
        else => return base_slot,
    }
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

    // iface value? (local meta, package global, field/index via inferMetaFromExpr)
    if (inferMetaFromExpr(ctx, base_expr)) |m| {
        if (m.kind == .iface_ptr) {
            return try lowerIfaceMethod(ctx, base_slot, method, args);
        }
    } else if (base_expr.* == .ident) {
        if (ctx.meta.get(base_expr.ident)) |m| {
            if (m.kind == .iface_ptr) {
                return try lowerIfaceMethod(ctx, base_slot, method, args);
            }
        }
    }
    if (ctx.ifaces.contains(concrete)) {
        return try lowerIfaceMethod(ctx, base_slot, method, args);
    }

    // `__Slice_byte` is the layout name for `str`/`bytes`; methods attach to `str`.
    const recv = if (std.mem.eql(u8, concrete, "__Slice_byte")) "str" else concrete;
    const recv_leaf = typeNameLeaf(recv);
    const key = try std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ recv_leaf, method });
    const mi = ctx.methods.get(key) orelse blk: {
        if (std.mem.eql(u8, recv_leaf, "str")) {
            const alt = try std.fmt.allocPrint(ctx.a, "bytes.{s}", .{method});
            break :blk ctx.methods.get(alt) orelse return error.UnknownName;
        }
        return error.UnknownName;
    };
    const cid = ctx.callee_map.get(mi.mangled) orelse return error.UnknownName;

    var arg_slots: [16]u32 = .{none} ** 16;
    var arity: u8 = 0;
    // contexts on methods (rare in demos)
    for (mi.contexts) |cx| {
        const want = typeExprName(cx.ty);
        const amb = findAmbient(ctx, want) orelse return error.UnknownName;
        arg_slots[arity] = try maybeCoerceCtx(ctx, amb.slot, amb.type_name, want);
        arity += 1;
    }
    // `*T` methods need the address of the receiver storage. Field loads of wide
    // structs (`h.fields`) return a ret_alloc copy — mutating that leaves `h` unchanged.
    const self_slot = if (mi.pointer_recv)
        try lowerPointerReceiver(ctx, base_expr, base_slot)
    else
        base_slot;
    arg_slots[arity] = self_slot;
    arity += 1;
    if (mi.pack_index) |pi| {
        const fixed: usize = pi;
        if (args.len < fixed) return error.TooManyArgs;
        var i: usize = 0;
        while (i < fixed) : (i += 1) {
            if (arity >= max_call_args) return error.TooManyArgs;
            arg_slots[arity] = try lowerExpr(ctx, args[i]);
            arity += 1;
        }
        const pack_n = args.len - fixed;
        const first = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = first, .aux = 0 });
        var j: usize = 1;
        while (j < pack_n) : (j += 1) {
            const s = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = s, .aux = 0 });
        }
        const ptr = try ctx.fresh();
        try ctx.emit(.{ .op = .lea_slot, .dst = ptr, .src0 = first });
        var k: usize = 0;
        while (k < pack_n) : (k += 1) {
            const val = try lowerExpr(ctx, args[fixed + k]);
            const addr = try ctx.fresh();
            const off = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = off, .aux = @intCast(k * 8) });
            try ctx.emit(.{ .op = .sub, .dst = addr, .src0 = ptr, .src1 = off });
            try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = val });
        }
        const len = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = len, .aux = @intCast(pack_n) });
        if (arity + 2 > max_call_args) return error.TooManyArgs;
        arg_slots[arity] = ptr;
        arity += 1;
        arg_slots[arity] = len;
        arity += 1;
    } else {
        if (args.len + arity > max_call_args) return error.TooManyArgs;
        for (args) |arg| {
            arg_slots[arity] = try lowerExpr(ctx, arg);
            arity += 1;
        }
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

    var arg_slots: [16]u32 = .{none} ** 16;
    arg_slots[0] = data;
    var arity: u8 = 1;
    if (args.len + 1 > max_call_args) return error.TooManyArgs;
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

fn lowerStringSlice(ctx: *Ctx, s: []const u8) LowerError!u32 {
    const idx: i64 = @intCast(ctx.strings.items.len);
    try ctx.strings.append(ctx.a, try ctx.a.dupe(u8, s));
    const data = try ctx.fresh();
    const len = try ctx.fresh();
    try ctx.emit(.{ .op = .saddr, .dst = data, .aux = idx });
    try ctx.emit(.{ .op = .slen, .dst = len, .aux = idx });
    const hdr = try lowerEmptyStructValue(ctx, blk: {
        const elem = try ctx.a.create(ast.TypeExpr);
        elem.* = .{ .name = "byte" };
        break :blk try ensureSliceStruct(ctx.a, ctx.structs, elem);
    });
    // ptr at offset 0, len at offset 8
    try ctx.emit(.{ .op = .store, .src0 = hdr, .src1 = data });
    const eight = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
    const len_addr = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = len_addr, .src0 = hdr, .src1 = eight });
    try ctx.emit(.{ .op = .store, .src0 = len_addr, .src1 = len });
    return hdr;
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
    // `str` / `bytes` / slice header: load `.ptr` and `.len`.
    if (inferMetaFromExpr(ctx, e)) |m| {
        if (m.kind == .struct_ptr and (std.mem.startsWith(u8, m.type_name, "__Slice_") or
            std.mem.eql(u8, m.type_name, "str") or std.mem.eql(u8, m.type_name, "bytes")))
        {
            const base = try lowerExpr(ctx, e);
            const ptr = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = ptr, .src0 = base });
            const eight = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
            const len_addr = try ctx.fresh();
            try ctx.emit(.{ .op = .add, .dst = len_addr, .src0 = base, .src1 = eight });
            const len = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = len, .src0 = len_addr });
            return .{ ptr, len };
        }
    }
    const ptr = try lowerExpr(ctx, e);
    const len = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = len, .aux = 0 });
    return .{ ptr, len };
}
