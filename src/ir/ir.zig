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
    /// Address of runtime TypeInfo record (aux = type_infos index).
    tiaddr,
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
    /// Index into `Program.type_infos` for the concrete type (`type_id`).
    type_info_index: u32 = none,
    methods: [][]const u8,
};

/// Runtime TypeInfo record emitted into rodata (JOG-229 / Modules.md).
/// Layout (32 bytes): kind:i64, name.ptr:*byte, name.len:i64, size:i64.
pub const TypeInfoKind = enum(u8) {
    struct_ty = 1,
    enum_ty = 2,
    iface_ty = 3,
};

pub const TypeInfoRec = struct {
    name: []const u8,
    kind: TypeInfoKind,
    size: i64,
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
    /// `@threadLocal` — native asm stores `JOGRT_MAX_WORKERS` copies; access via worker id.
    thread_local: bool = false,
};

pub const Program = struct {
    strings: [][]const u8,
    callees: []Callee,
    funcs: []Func,
    vtables: []VTable,
    type_infos: []TypeInfoRec,
    globals: []Global,
    needs_libc: bool,
    /// JOG-233: `--race` instruments loads/stores and enables the debug detector.
    race: bool = false,
    /// When true, asm emit inserts `jogrt_fuzz_hit` probes (JOG-232 coverage).
    fuzz_instrument: bool = false,
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
    /// True if any variant carries a payload — values are heap ptrs (tag at +0).
    has_payload: bool = false,
    /// Max payload field count (for allocation size).
    max_fields: usize = 0,
};

const EnumVariantVal = struct {
    name: []const u8,
    value: i64,
    fields: []ast.Field = &.{},
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
    /// Value params (AST borrows; used to coerce args to iface params).
    params: []ast.Param = &.{},
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

const SlotKind = enum { scalar, struct_ptr, iface_ptr, opt_ptr, raw_ptr, array_ptr, capture_ptr };

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
        .type_infos = &[_]TypeInfoRec{},
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
    var type_infos: std.ArrayListUnmanaged(TypeInfoRec) = .{};
    var type_info_map: std.StringHashMapUnmanaged(u32) = .{};
    var globals: std.ArrayListUnmanaged(Global) = .{};
    var global_map: std.StringHashMapUnmanaged(u32) = .{};
    var object_map: std.StringHashMapUnmanaged(u32) = .{}; // object name → global index
    var const_vals: std.StringHashMapUnmanaged(ConstVal) = .{};
    var addr_consts: std.StringHashMapUnmanaged(u32) = .{}; // val Name = &global
    var import_binds: std.StringHashMapUnmanaged(void) = .{};
    // `type Name = T` / `type Name: T` → underlying type leaf (for floatish distinct types).
    var type_aliases: std.StringHashMapUnmanaged([]const u8) = .{};

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
    // JOG-222 structured concurrency (scope + go blocks).
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_scope_enter" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_scope_leave" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_scope_current" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_scope_go" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_scope_trampoline" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_scope_cancel" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_scope_cancelled" });
    // JOG-242 Chan / select / Task.
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_chan_init" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_chan_free" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_chan_close" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_chan_send" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_chan_recv" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_select_begin" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_select_add_recv" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_select_add_send" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_select_add_default" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_select_add_timeout" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_select_wait" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_select_status" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_sleep" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_err_cancelled" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_err_closed" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_task_set" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_task_join" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_task_finish" });
    try internCallee(a, &callees, &callee_map, .{ .kind = .runtime, .name = "jogrt_task_begin" });
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
            .type_alias => |ta| {
                // Record leaf→leaf so `type Celsius: f64` stays floatish under `as` / arith.
                const under = typeExprName(ta.ty);
                if (under.len > 0) try type_aliases.put(a, ta.name, under);
            },
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
                var has_payload = false;
                var max_fields: usize = 0;
                for (ed.variants) |v| {
                    const val = v.value orelse next;
                    if (v.fields.len > 0) has_payload = true;
                    if (v.fields.len > max_fields) max_fields = v.fields.len;
                    try variants.append(a, .{ .name = v.name, .value = val, .fields = v.fields });
                    next = val + 1;
                }
                try enums.put(a, ed.name, .{
                    .variants = try variants.toOwnedSlice(a),
                    .has_payload = has_payload,
                    .max_fields = max_fields,
                });
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
                        .params = ef.params,
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
                        .thread_local = gd.thread_local,
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
                        .params = fd.params,
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
                        .params = ef.params,
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
    var pending_gos: std.ArrayListUnmanaged(PendingGoFn) = .{};
    var go_seq: u32 = 0;
    var pending_funcs: std.ArrayListUnmanaged(Func) = .{};
    var lambda_seq: u32 = 0;
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
            }, &strings, &callees, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &type_infos, &type_info_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds, &type_aliases, &pending_gos, &go_seq, &pending_funcs, &lambda_seq);
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
            }, &strings, &callees, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &type_infos, &type_info_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds, &type_aliases, &pending_gos, &go_seq, &pending_funcs, &lambda_seq);
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
        }, &strings, &callees, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &type_infos, &type_info_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds, &type_aliases, &pending_gos, &go_seq, &pending_funcs, &lambda_seq);
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
        }, &strings, &callees, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &type_infos, &type_info_map, &const_vals, &addr_consts, &global_map, globals.items, &object_map, &import_binds, &type_aliases, &pending_gos, &go_seq, &pending_funcs, &lambda_seq) catch |err| {
            // Callback / unsupported bodies (each, map, fold, …) — drop so callers error clearly.
            std.debug.print("skip specialized method {s}: {s}\n", .{ sm.mangled, @errorName(err) });
            const key = try std.fmt.allocPrint(a, "{s}.{s}", .{ sm.type_name, sm.method_name });
            _ = methods.remove(key);
            continue;
        };
        try funcs.append(a, f);
    }
    // Lower synthetic `go { }` children (may enqueue nested go blocks).
    var gi: usize = 0;
    while (gi < pending_gos.items.len) : (gi += 1) {
        const pg = pending_gos.items[gi];
        try internCallee(a, &callees, &callee_map, .{ .kind = .jog_fn, .name = pg.name });
        try fn_infos.put(a, pg.name, .{
            .mangled = pg.name,
            .nparams = 1,
            .contexts = &[_]ast.ContextParam{},
            .exported = false,
        });
        const f = try lowerGoChild(a, pg, &strings, &callees, &callee_map, &structs, &enums, &methods, &ifaces, &fn_infos, &vtables, &vtable_map, &type_infos, &type_info_map, &const_vals, &global_map, globals.items, &object_map, &addr_consts, &import_binds, &type_aliases, &pending_gos, &go_seq, &pending_funcs, &lambda_seq);
        try funcs.append(a, f);
    }
    try funcs.appendSlice(a, pending_funcs.items);

    program.strings = try strings.toOwnedSlice(a);
    program.callees = try callees.toOwnedSlice(a);
    program.funcs = try funcs.toOwnedSlice(a);
    program.vtables = try vtables.toOwnedSlice(a);
    program.type_infos = try type_infos.toOwnedSlice(a);
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
            .is_private = f.is_private,
            .is_embedded = f.is_embedded,
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
        .params = fd.params,
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
        .enum_lit => |el| {
            for (el.fields) |f| try walkExprInstantiate(ic, f.value);
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
        .if_expr => |ifs| {
            try walkExprInstantiate(ic, ifs.cond);
            for (ifs.then_block.stmts) |s| try walkStmtInstantiate(ic, s);
            if (ifs.else_block) |eb| for (eb.stmts) |s| try walkStmtInstantiate(ic, s);
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
        .lambda => |lam| {
            for (lam.params) |p| {
                if (p.default) |d| try walkExprInstantiate(ic, d);
                try walkTypeInstantiateFull(ic, p.ty);
            }
            if (lam.ret) |r| try walkTypeInstantiateFull(ic, r);
            if (lam.expr_body) |eb| try walkExprInstantiate(ic, eb);
            if (lam.block) |b| {
                for (b.stmts) |s| try walkStmtInstantiate(ic, s);
            }
        },
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
            if (as.deref) |d| try walkExprInstantiate(ic, d);
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
    /// JOG-222: go-child capture name → index in the `__env` pointer array.
    go_captures: ?*const std.StringHashMapUnmanaged(u32) = null,
};

/// Synthetic child function for `go { … }` (JOG-222).
const PendingGoFn = struct {
    name: []const u8,
    body: *ast.Block,
    capture_names: [][]const u8,
};

const Ctx = struct {
    a: std.mem.Allocator,
    strings: *std.ArrayListUnmanaged([]const u8),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    enums: *std.StringHashMapUnmanaged(EnumInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    vtables: *std.ArrayListUnmanaged(VTable),
    vtable_map: *std.StringHashMapUnmanaged(u32),
    type_infos: *std.ArrayListUnmanaged(TypeInfoRec),
    type_info_map: *std.StringHashMapUnmanaged(u32),
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
    /// `defer_stack.items.len` before each loop body — break/continue unwind down to this.
    loop_defer_depths: *std.ArrayListUnmanaged(usize),
    /// When set, `return` values are coerced to this interface fat pointer.
    ret_iface: ?[]const u8 = null,
    /// Generic method/fn type-param → concrete name (`T` → `i64`). Empty for non-generic.
    type_subst: *const std.StringHashMapUnmanaged([]const u8),
    /// Queued `go { }` bodies to lower as sibling funcs after the parent.
    pending_gos: *std.ArrayListUnmanaged(PendingGoFn),
    go_seq: *u32,
    /// Active `scope { }` handle slot, if any.
    scope_slot: ?u32 = null,
    /// Inside a go child: env param slot + capture name → index in env pointer array.
    env_slot: ?u32 = null,
    capture_map: ?*const std.StringHashMapUnmanaged(u32) = null,
    /// `type Name: f64` distinct/alias → underlying leaf (floatish resolution).
    type_aliases: *const std.StringHashMapUnmanaged([]const u8),
    /// Synthetic lambdas / adapters lowered during expression lowering.
    pending_funcs: *std.ArrayListUnmanaged(Func),
    lambda_seq: *u32,

    fn fresh(self: *Ctx) !u32 {
        const s = self.next_slot.*;
        self.next_slot.* += 1;
        return s;
    }
    fn emit(self: *Ctx, inst: Inst) !void {
        try self.insts.append(self.a, inst);
    }
    fn intern(self: *Ctx, c: Callee) !void {
        try internCallee(self.a, self.callees, self.callee_map, c);
    }
};

fn lowerFn(
    a: std.mem.Allocator,
    fl: FnLower,
    strings: *std.ArrayListUnmanaged([]const u8),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    enums: *std.StringHashMapUnmanaged(EnumInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    vtables: *std.ArrayListUnmanaged(VTable),
    vtable_map: *std.StringHashMapUnmanaged(u32),
    type_infos: *std.ArrayListUnmanaged(TypeInfoRec),
    type_info_map: *std.StringHashMapUnmanaged(u32),
    const_vals: *std.StringHashMapUnmanaged(ConstVal),
    addr_consts: *std.StringHashMapUnmanaged(u32),
    global_map: *std.StringHashMapUnmanaged(u32),
    globals: []const Global,
    object_map: *std.StringHashMapUnmanaged(u32),
    import_binds: *std.StringHashMapUnmanaged(void),
    type_aliases: *const std.StringHashMapUnmanaged([]const u8),
    pending_gos: *std.ArrayListUnmanaged(PendingGoFn),
    go_seq: *u32,
    pending_funcs: *std.ArrayListUnmanaged(Func),
    lambda_seq: *u32,
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
    var loop_defer_depths: std.ArrayListUnmanaged(usize) = .{};
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
        .callees = callees,
        .callee_map = callee_map,
        .structs = structs,
        .enums = enums,
        .methods = methods,
        .ifaces = ifaces,
        .fn_infos = fn_infos,
        .vtables = vtables,
        .vtable_map = vtable_map,
        .type_infos = type_infos,
        .type_info_map = type_info_map,
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
        .loop_defer_depths = &loop_defer_depths,
        .ret_iface = fl.ret_iface,
        .type_subst = subst_ptr,
        .pending_gos = pending_gos,
        .go_seq = go_seq,
        .capture_map = fl.go_captures,
        .env_slot = if (fl.go_captures != null) slots.get("__env") else null,
        .type_aliases = type_aliases,
        .pending_funcs = pending_funcs,
        .lambda_seq = lambda_seq,
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

fn lowerGoChild(
    a: std.mem.Allocator,
    pg: PendingGoFn,
    strings: *std.ArrayListUnmanaged([]const u8),
    callees: *std.ArrayListUnmanaged(Callee),
    callee_map: *std.StringHashMapUnmanaged(u32),
    structs: *std.StringHashMapUnmanaged(StructInfo),
    enums: *std.StringHashMapUnmanaged(EnumInfo),
    methods: *std.StringHashMapUnmanaged(MethodInfo),
    ifaces: *std.StringHashMapUnmanaged(IfaceInfo),
    fn_infos: *std.StringHashMapUnmanaged(FnInfo),
    vtables: *std.ArrayListUnmanaged(VTable),
    vtable_map: *std.StringHashMapUnmanaged(u32),
    type_infos: *std.ArrayListUnmanaged(TypeInfoRec),
    type_info_map: *std.StringHashMapUnmanaged(u32),
    const_vals: *std.StringHashMapUnmanaged(ConstVal),
    global_map: *std.StringHashMapUnmanaged(u32),
    globals: []const Global,
    object_map: *std.StringHashMapUnmanaged(u32),
    addr_consts: *std.StringHashMapUnmanaged(u32),
    import_binds: *std.StringHashMapUnmanaged(void),
    type_aliases: *const std.StringHashMapUnmanaged([]const u8),
    pending_gos: *std.ArrayListUnmanaged(PendingGoFn),
    go_seq: *u32,
    pending_funcs: *std.ArrayListUnmanaged(Func),
    lambda_seq: *u32,
) !Func {
    var capture_map: std.StringHashMapUnmanaged(u32) = .{};
    for (pg.capture_names, 0..) |n, i| {
        try capture_map.put(a, n, @intCast(i));
    }
    // Synthetic: `fn __go_N(env: *byte)` — env is an array of capture pointers.
    const env_param = try a.alloc(ast.Param, 1);
    const env_ty = try a.create(ast.TypeExpr);
    const byte_ty = try a.create(ast.TypeExpr);
    byte_ty.* = .{ .name = "byte" };
    env_ty.* = .{ .pointer = byte_ty };
    env_param[0] = .{ .name = "__env", .ty = env_ty, .is_pack = false };
    return try lowerFn(a, .{
        .name = pg.name,
        .params = env_param,
        .contexts = &[_]ast.ContextParam{},
        .body = pg.body,
        .exported = false,
        .self_type = null,
        .pointer_recv = false,
        .go_captures = &capture_map,
    }, strings, callees, callee_map, structs, enums, methods, ifaces, fn_infos, vtables, vtable_map, type_infos, type_info_map, const_vals, addr_consts, global_map, globals, object_map, import_binds, type_aliases, pending_gos, go_seq, pending_funcs, lambda_seq);
}

fn loadCapturePtr(ctx: *Ctx, name: []const u8) LowerError!?u32 {
    const cm = ctx.capture_map orelse return null;
    const idx = cm.get(name) orelse return null;
    const env = ctx.env_slot orelse return error.TypeError;
    const off = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = off, .aux = @as(i64, @intCast(idx)) * 8 });
    const addr = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = addr, .src0 = env, .src1 = off });
    const ptr = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = ptr, .src0 = addr });
    return ptr;
}

fn collectGoCaptures(ctx: *Ctx, body: *ast.Block) LowerError![][]const u8 {
    var locals: std.StringHashMapUnmanaged(void) = .{};
    var caps: std.ArrayListUnmanaged([]const u8) = .{};
    var seen: std.StringHashMapUnmanaged(void) = .{};
    try scanBlockCaptures(ctx, body, &locals, &caps, &seen);
    return try caps.toOwnedSlice(ctx.a);
}

fn scanBlockCaptures(
    ctx: *Ctx,
    body: *ast.Block,
    locals: *std.StringHashMapUnmanaged(void),
    caps: *std.ArrayListUnmanaged([]const u8),
    seen: *std.StringHashMapUnmanaged(void),
) LowerError!void {
    for (body.stmts) |st| try scanStmtCaptures(ctx, st, locals, caps, seen);
}

fn scanStmtCaptures(
    ctx: *Ctx,
    stmt: ast.Stmt,
    locals: *std.StringHashMapUnmanaged(void),
    caps: *std.ArrayListUnmanaged([]const u8),
    seen: *std.StringHashMapUnmanaged(void),
) LowerError!void {
    switch (stmt) {
        .var_decl => |vd| {
            try locals.put(ctx.a, vd.name, {});
            if (vd.init) |init| try scanExprCaptures(ctx, init, locals, caps, seen);
        },
        .destructure => |ds| {
            for (ds.names) |n| try locals.put(ctx.a, n, {});
            try scanExprCaptures(ctx, ds.init, locals, caps, seen);
        },
        .assign => |as| {
            if (as.index == null and as.field == null) {
                try noteCapture(ctx, as.name, locals, caps, seen);
            }
            try scanExprCaptures(ctx, as.value, locals, caps, seen);
            if (as.field) |fa| try scanExprCaptures(ctx, fa.base, locals, caps, seen);
            if (as.index) |ix| {
                try scanExprCaptures(ctx, ix.base, locals, caps, seen);
                try scanExprCaptures(ctx, ix.index, locals, caps, seen);
            }
        },
        .expr => |e| try scanExprCaptures(ctx, e, locals, caps, seen),
        .return_stmt => |re| if (re) |e| try scanExprCaptures(ctx, e, locals, caps, seen),
        .if_stmt => |ifs| {
            try scanExprCaptures(ctx, ifs.cond, locals, caps, seen);
            try scanBlockCaptures(ctx, ifs.then_block, locals, caps, seen);
            if (ifs.else_block) |eb| try scanBlockCaptures(ctx, eb, locals, caps, seen);
        },
        .while_stmt => |ws| {
            try scanExprCaptures(ctx, ws.cond, locals, caps, seen);
            try scanBlockCaptures(ctx, ws.body, locals, caps, seen);
        },
        .for_in_stmt => |fs| {
            try locals.put(ctx.a, fs.name, {});
            try scanExprCaptures(ctx, fs.iter, locals, caps, seen);
            try scanBlockCaptures(ctx, fs.body, locals, caps, seen);
        },
        .with_stmt => |ws| {
            for (ws.values) |v| try scanExprCaptures(ctx, v, locals, caps, seen);
            try scanBlockCaptures(ctx, ws.body, locals, caps, seen);
        },
        .defer_stmt, .errdefer_stmt => |ds| switch (ds) {
            .expr => |ex| try scanExprCaptures(ctx, ex, locals, caps, seen),
            .block => |b| try scanBlockCaptures(ctx, b, locals, caps, seen),
        },
        .break_stmt, .continue_stmt => {},
    }
}

fn noteCapture(
    ctx: *Ctx,
    name: []const u8,
    locals: *std.StringHashMapUnmanaged(void),
    caps: *std.ArrayListUnmanaged([]const u8),
    seen: *std.StringHashMapUnmanaged(void),
) LowerError!void {
    if (locals.contains(name)) return;
    if (seen.contains(name)) return;
    // Only parent stack locals — globals/fns/consts resolve without capture.
    if (!ctx.slots.contains(name)) return;
    try seen.put(ctx.a, name, {});
    try caps.append(ctx.a, name);
}

fn scanExprCaptures(
    ctx: *Ctx,
    e: *ast.Expr,
    locals: *std.StringHashMapUnmanaged(void),
    caps: *std.ArrayListUnmanaged([]const u8),
    seen: *std.StringHashMapUnmanaged(void),
) LowerError!void {
    switch (e.*) {
        .ident => |name| try noteCapture(ctx, name, locals, caps, seen),
        .binary => |b| {
            try scanExprCaptures(ctx, b.left, locals, caps, seen);
            try scanExprCaptures(ctx, b.right, locals, caps, seen);
        },
        .unary => |u| try scanExprCaptures(ctx, u.expr, locals, caps, seen),
        .call => |c| {
            try scanExprCaptures(ctx, c.callee, locals, caps, seen);
            for (c.args) |arg| try scanExprCaptures(ctx, arg, locals, caps, seen);
            for (c.with_args) |w| try scanExprCaptures(ctx, w, locals, caps, seen);
        },
        .field => |f| try scanExprCaptures(ctx, f.base, locals, caps, seen),
        .index => |ix| {
            try scanExprCaptures(ctx, ix.base, locals, caps, seen);
            try scanExprCaptures(ctx, ix.index, locals, caps, seen);
        },
        .slice => |sl| {
            try scanExprCaptures(ctx, sl.base, locals, caps, seen);
            if (sl.start) |s| try scanExprCaptures(ctx, s, locals, caps, seen);
            if (sl.end) |en| try scanExprCaptures(ctx, en, locals, caps, seen);
        },
        .cast, .try_cast => |c| try scanExprCaptures(ctx, c.expr, locals, caps, seen),
        .is_type => |it| try scanExprCaptures(ctx, it.expr, locals, caps, seen),
        .optional_call => |oc| {
            try scanExprCaptures(ctx, oc.base, locals, caps, seen);
            for (oc.args) |arg| try scanExprCaptures(ctx, arg, locals, caps, seen);
        },
        .struct_lit => |sl| {
            for (sl.fields) |f| try scanExprCaptures(ctx, f.value, locals, caps, seen);
        },
        .panic_call => |p| try scanExprCaptures(ctx, p.message, locals, caps, seen),
        .when_expr => |w| {
            try scanExprCaptures(ctx, w.scrutinee, locals, caps, seen);
            for (w.arms) |arm| try scanExprCaptures(ctx, arm.body, locals, caps, seen);
            if (w.else_body) |eb| try scanExprCaptures(ctx, eb, locals, caps, seen);
        },
        .tuple => |elems| for (elems) |el| try scanExprCaptures(ctx, el, locals, caps, seen),
        .range => |r| {
            try scanExprCaptures(ctx, r.start, locals, caps, seen);
            try scanExprCaptures(ctx, r.end, locals, caps, seen);
        },
        .array_lit => |al| for (al.elems) |el| try scanExprCaptures(ctx, el, locals, caps, seen),
        .interp => |ip| {
            for (ip.parts) |part| switch (part) {
                .text => {},
                .expr => |ex| try scanExprCaptures(ctx, ex, locals, caps, seen),
            };
        },
        .force_unwrap => |inner| try scanExprCaptures(ctx, inner, locals, caps, seen),
        .go_block => |gb| {
            for (gb.with_args) |w| try scanExprCaptures(ctx, w, locals, caps, seen);
            try scanBlockCaptures(ctx, gb.body, locals, caps, seen);
        },
        .go_expr => |ge| try scanExprCaptures(ctx, ge, locals, caps, seen),
        .scope_block => |sb| try scanBlockCaptures(ctx, sb.body, locals, caps, seen),
        .select_block => |sb| try scanBlockCaptures(ctx, sb.body, locals, caps, seen),
        .type_value => {},
        .enum_lit => |el| {
            for (el.fields) |f| try scanExprCaptures(ctx, f.value, locals, caps, seen);
        },
        .if_expr => |ie| {
            try scanExprCaptures(ctx, ie.cond, locals, caps, seen);
            try scanBlockCaptures(ctx, ie.then_block, locals, caps, seen);
            if (ie.else_block) |eb| try scanBlockCaptures(ctx, eb, locals, caps, seen);
        },
        .lambda => |lam| {
            // Nested lambda: scan defaults and body; params are new locals for the child.
            for (lam.params) |p| {
                if (p.default) |d| try scanExprCaptures(ctx, d, locals, caps, seen);
            }
            if (lam.expr_body) |eb| try scanExprCaptures(ctx, eb, locals, caps, seen);
            if (lam.block) |bb| try scanBlockCaptures(ctx, bb, locals, caps, seen);
        },
        .int, .float, .string, .cstring, .null_lit => {},
    }
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

/// Nominal `str`/`bytes` keep that type_name for method lookup, but layout lives in `__Slice_byte`.
fn layoutStructName(
    a: std.mem.Allocator,
    structs: *std.StringHashMapUnmanaged(StructInfo),
    type_name: []const u8,
) ![]const u8 {
    if (std.mem.eql(u8, type_name, "str") or std.mem.eql(u8, type_name, "bytes")) {
        return try ensureSliceStructFromElemName(a, structs, "byte");
    }
    return type_name;
}

fn layoutStructInfo(
    a: std.mem.Allocator,
    structs: *std.StringHashMapUnmanaged(StructInfo),
    type_name: []const u8,
) !?StructInfo {
    const layout = try layoutStructName(a, structs, type_name);
    return structs.get(layout);
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

/// Lower a block whose value is the last expression statement; copy into `result`.
fn lowerBlockAsValue(ctx: *Ctx, block: *ast.Block, result: u32) LowerError!void {
    try ctx.defer_stack.append(ctx.a, .{});
    if (block.stmts.len == 0) {
        try ctx.emit(.{ .op = .iconst, .dst = result, .aux = 0 });
    } else {
        const last_i = block.stmts.len - 1;
        for (block.stmts[0..last_i]) |stmt| try lowerStmt(ctx, stmt);
        const last = block.stmts[last_i];
        if (last == .expr) {
            const v = try lowerExpr(ctx, last.expr);
            try ctx.emit(.{ .op = .copy, .dst = result, .src0 = v });
        } else {
            try lowerStmt(ctx, last);
            try ctx.emit(.{ .op = .iconst, .dst = result, .aux = 0 });
        }
    }
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
    // Snapshot: emitDeferItem → lowerBlock may reallocate defer_stack.
    const snapshot = try ctx.a.dupe(DeferItem, frame.items);
    defer ctx.a.free(snapshot);
    var i: usize = snapshot.len;
    while (i > 0) {
        i -= 1;
        // Scope-exit defers: skip errdefer (those only run on error return).
        if (!snapshot[i].on_error_only) try emitDeferItem(ctx, snapshot[i]);
    }
    // Keep errdefer items for return paths; clear normal ones.
    var kept: std.ArrayListUnmanaged(DeferItem) = .{};
    for (snapshot) |it| {
        if (it.on_error_only) try kept.append(ctx.a, it);
    }
    // Re-fetch frame after possible realloc.
    const frame2 = &ctx.defer_stack.items[ctx.defer_stack.items.len - 1];
    frame2.clearRetainingCapacity();
    try frame2.appendSlice(ctx.a, kept.items);
}

fn emitAllDefers(ctx: *Ctx) LowerError!void {
    try emitAllDefersFiltered(ctx, false);
}

fn emitAllDefersFiltered(ctx: *Ctx, error_path: bool) LowerError!void {
    try emitDefersDownTo(ctx, 0, error_path);
}

/// Emit deferred work for frames with index >= `keep_frames` (LIFO), without clearing.
/// Used by return / break / continue. Nested `lowerBlock` may grow `defer_stack`, so each
/// frame's items are snapshotted first.
fn emitDefersDownTo(ctx: *Ctx, keep_frames: usize, error_path: bool) LowerError!void {
    var fi: usize = ctx.defer_stack.items.len;
    while (fi > keep_frames) {
        fi -= 1;
        const snapshot = try ctx.a.dupe(DeferItem, ctx.defer_stack.items[fi].items);
        defer ctx.a.free(snapshot);
        var i: usize = snapshot.len;
        while (i > 0) {
            i -= 1;
            const it = snapshot[i];
            if (it.on_error_only and !error_path) continue;
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
                // `str`/`bytes` meta uses the nominal name; layout is `__Slice_byte`.
                if (sm.kind == .struct_ptr and sm.type_name.len > 0) {
                    if (try layoutStructInfo(ctx.a, ctx.structs, sm.type_name)) |vsd| {
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
                var v = try lowerExpr(ctx, init);
                if (vd.ty == null) {
                    if (ctx.meta.get(slotNameOf(ctx, v))) |m| sm = m;
                    // struct lit / alloc leave meta on temp — copy from init kind via peek
                    sm = inferMetaFromExpr(ctx, init) orelse sm;
                }
                // `val n: Named = &d` / `= d` — build a fat iface pointer.
                if (sm.kind == .iface_ptr and sm.type_name.len > 0) {
                    v = try maybeCoerceArg(ctx, v, init, vd.ty);
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
                // Nominal `str`/`bytes` must resolve through `__Slice_byte` or the
                // durable copy is skipped and iface/`print` later segfault on a
                // recycled ret-arena header.
                if (!ty_is_ptr and sm.kind == .struct_ptr and sm.type_name.len > 0) {
                    if (try layoutStructInfo(ctx.a, ctx.structs, sm.type_name)) |sd| {
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
            // `var xs: List<T>` / any struct typed local with no init — durable zeroed header.
            // A null iconst slot is not a valid struct_ptr (push/field access would SEGV).
            if (sm.kind == .struct_ptr and sm.type_name.len > 0) {
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
                    var off: i64 = 0;
                    while (off < nbytes) : (off += 8) {
                        const z = try ctx.fresh();
                        try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
                        const addr = try ctx.fresh();
                        const off_s = try ctx.fresh();
                        try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                        try ctx.emit(.{ .op = .add, .dst = addr, .src0 = ptr, .src1 = off_s });
                        try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = z });
                    }
                    try ctx.slots.put(ctx.a, vd.name, ptr);
                    try ctx.meta.put(ctx.a, vd.name, sm);
                    return;
                }
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
                    } else if (as.deref) |ptr_e| {
                        const addr = try lowerExpr(ctx, ptr_e);
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .load, .dst = d, .src0 = addr });
                        break :blk d;
                    } else if (try loadCapturePtr(ctx, as.name)) |ptr| {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .load, .dst = d, .src0 = ptr });
                        break :blk d;
                    } else if (ctx.slots.get(as.name)) |slot| {
                        if (ctx.meta.get(as.name)) |m| {
                            if (m.kind == .capture_ptr) {
                                const d = try ctx.fresh();
                                try ctx.emit(.{ .op = .load, .dst = d, .src0 = slot });
                                break :blk d;
                            }
                        }
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
            } else if (as.deref) |ptr_e| {
                const addr = try lowerExpr(ctx, ptr_e);
                try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = v });
            } else if (try loadCapturePtr(ctx, as.name)) |ptr| {
                try ctx.emit(.{ .op = .store, .src0 = ptr, .src1 = v });
            } else if (ctx.slots.get(as.name)) |slot| {
                if (ctx.meta.get(as.name)) |m| {
                    if (m.kind == .capture_ptr) {
                        try ctx.emit(.{ .op = .store, .src0 = slot, .src1 = v });
                        return;
                    }
                }
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
                    try emitTaskSetIfGoChild(ctx, ret_slot);
                    try ctx.emit(.{ .op = .ret, .src0 = ret_slot });
                    return;
                }
            }
            try emitAllDefersFiltered(ctx, error_path);
            try emitTaskSetIfGoChild(ctx, ret_slot);
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
            // Depth before body frame: break/continue run body (+ nested) defers.
            try ctx.loop_defer_depths.append(ctx.a, ctx.defer_stack.items.len);
            try lowerBlock(ctx, ws.body);
            _ = ctx.loop_defer_depths.pop();
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
            if (ctx.loop_defer_depths.items.len > 0) {
                const keep = ctx.loop_defer_depths.items[ctx.loop_defer_depths.items.len - 1];
                try emitDefersDownTo(ctx, keep, false);
            }
            const ji = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            try ctx.break_jmps.append(ctx.a, ji);
        },
        .continue_stmt => {
            if (ctx.loop_defer_depths.items.len > 0) {
                const keep = ctx.loop_defer_depths.items[ctx.loop_defer_depths.items.len - 1];
                try emitDefersDownTo(ctx, keep, false);
            }
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
        .lambda => return .{ .kind = .scalar, .type_name = "iface_fn" },
        .go_block, .go_expr => return .{ .kind = .raw_ptr, .type_name = "Task" },
        .type_value => return null,
        .struct_lit => |sl| {
            if (ctx.fn_infos.contains(sl.type_name)) {
                // Named-arg function call — meta from return type.
                if (ctx.fn_infos.get(sl.type_name)) |fi| {
                    if (metaFromCallRet(ctx, fi.ret_meta, fi.ret_name)) |m| return m;
                }
                return null;
            }
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
        .enum_lit => |el| {
            const tn = el.type_name orelse return null;
            if (ctx.enums.get(tn)) |ei| {
                if (ei.has_payload) return .{ .kind = .struct_ptr, .type_name = tn };
                return .{ .kind = .scalar, .type_name = tn };
            }
            return .{ .kind = .struct_ptr, .type_name = tn };
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
            if (ctx.callee_map.contains(n)) return .{ .kind = .scalar, .type_name = "iface_fn" };
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
                // Enum.Variant construction (no args).
                if (ctx.enums.get(f.base.ident)) |ei| {
                    if (ei.has_payload) return .{ .kind = .struct_ptr, .type_name = f.base.ident };
                    return .{ .kind = .scalar, .type_name = f.base.ident };
                }
            }
            var base_tn = inferTypeNameFromExpr(ctx, f.base) orelse return null;
            // Nominal `str`/`bytes` → `__Slice_byte` for field types (`.ptr` is `*byte`).
            if (std.mem.eql(u8, base_tn, "str") or std.mem.eql(u8, base_tn, "bytes")) {
                const elem = ctx.a.create(ast.TypeExpr) catch return null;
                elem.* = .{ .name = "byte" };
                base_tn = ensureSliceStruct(ctx.a, ctx.structs, elem) catch return null;
            }
            if (findFieldInStruct(ctx, base_tn, f.name)) |hit| {
                return metaForTypeExpr(ctx.a, hit.ty, ctx.structs, ctx.ifaces) catch null;
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
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "typeInfo"))
                return .{ .kind = .raw_ptr, .type_name = "byte", .elem_size = 1 };
            if (c.callee.* == .ident and std.mem.eql(u8, c.callee.ident, "typeName"))
                return .{ .kind = .struct_ptr, .type_name = "str" };
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
                // Chan<T>.init → Chan__T opaque pointer.
                if (c.callee.field.base.* == .type_value) {
                    const tv = c.callee.field.base.type_value;
                    if (std.mem.eql(u8, typeNameLeaf(tv.name), "Chan") and std.mem.eql(u8, c.callee.field.name, "init")) {
                        const mangled = mangleNamedApp(ctx.a, "Chan", tv.args) catch "Chan__i64";
                        return .{ .kind = .raw_ptr, .type_name = mangled };
                    }
                }
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
        .unary => |u| {
            if (u.op != .addr) return null;
            // `&x` / `&base[i]` / `&base.field` → raw pointer to the element/value.
            // Critical for `val dest = &buf[0]; dest[n] = 0` (posix open_c_file).
            switch (u.expr.*) {
                .ident => {
                    if (inferMetaFromExpr(ctx, u.expr)) |m| {
                        if (m.kind == .array_ptr)
                            return .{ .kind = .raw_ptr, .type_name = m.type_name, .elem_size = if (m.elem_size > 0) m.elem_size else 8 };
                        if (m.kind == .struct_ptr or m.kind == .iface_ptr)
                            return m;
                        const es: u32 = if (m.elem_size > 0)
                            m.elem_size
                        else if (std.mem.eql(u8, m.type_name, "byte") or std.mem.eql(u8, m.type_name, "u8") or std.mem.eql(u8, m.type_name, "bool"))
                            1
                        else
                            8;
                        return .{ .kind = .raw_ptr, .type_name = m.type_name, .elem_size = es };
                    }
                    return .{ .kind = .raw_ptr };
                },
                .index => |ix| {
                    if (indexBaseMeta(ctx, ix.base)) |bm| {
                        if (isSliceHeaderMeta(bm)) {
                            const stride = sliceHeaderElemStride(ctx, bm);
                            const elem = sliceElemTypeName(ctx, ix.base) orelse "i64";
                            return .{ .kind = .raw_ptr, .type_name = elem, .elem_size = stride };
                        }
                        const es: u32 = if (bm.elem_size > 0)
                            bm.elem_size
                        else if (std.mem.eql(u8, bm.type_name, "byte") or std.mem.eql(u8, bm.type_name, "u8") or std.mem.eql(u8, bm.type_name, "bool"))
                            1
                        else
                            8;
                        return .{ .kind = .raw_ptr, .type_name = bm.type_name, .elem_size = es };
                    }
                    return .{ .kind = .raw_ptr, .elem_size = 8 };
                },
                .field => {
                    if (inferMetaFromExpr(ctx, u.expr)) |fm| {
                        const es: u32 = if (fm.elem_size > 0)
                            fm.elem_size
                        else if (std.mem.eql(u8, fm.type_name, "byte") or std.mem.eql(u8, fm.type_name, "u8") or std.mem.eql(u8, fm.type_name, "bool"))
                            1
                        else
                            8;
                        if (fm.kind == .struct_ptr)
                            return .{ .kind = .raw_ptr, .type_name = fm.type_name, .elem_size = es };
                        if (fm.kind == .iface_ptr)
                            return .{ .kind = .raw_ptr, .type_name = fm.type_name, .elem_size = 8 };
                        return .{ .kind = .raw_ptr, .type_name = fm.type_name, .elem_size = es };
                    }
                    return .{ .kind = .raw_ptr };
                },
                else => return null,
            }
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
        if (isSliceHeaderMeta(m)) {
            const stride = sliceHeaderElemStride(ctx, m);
            return if (stride == 1) 1 else 8;
        }
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
        // `{ptr,len}` slice: element stride — never `sizeof(__Slice_T)` (that is 16).
        if (isSliceHeaderMeta(bm)) {
            const stride = sliceHeaderElemStride(ctx, bm);
            if (stride > 8) return @as(i64, stride);
            return null;
        }
        // Array of str/bytes / wide structs: multi-word element copy.
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

fn isSliceHeaderMeta(m: SlotMeta) bool {
    if (std.mem.eql(u8, m.type_name, "str") or std.mem.eql(u8, m.type_name, "bytes")) return true;
    if (std.mem.startsWith(u8, m.type_name, "__Slice_")) return true;
    return false;
}

fn sliceHeaderElemStride(ctx: *Ctx, m: SlotMeta) u32 {
    if (std.mem.eql(u8, m.type_name, "str") or std.mem.eql(u8, m.type_name, "bytes")) return 1;
    if (std.mem.startsWith(u8, m.type_name, "__Slice_")) {
        const elem = m.type_name["__Slice_".len..];
        if (std.mem.eql(u8, elem, "byte") or std.mem.eql(u8, elem, "u8") or std.mem.eql(u8, elem, "bool"))
            return 1;
        if (ctx.structs.contains(elem)) {
            const sz = layoutSizeOf(ctx, elem);
            if (sz > 0) return @intCast(sz);
        }
        return 8;
    }
    return 8;
}

fn indexStride(ctx: *Ctx, base_expr: *ast.Expr) u32 {
    // Dense packed byte arrays / raw byte pointers use stride 1.
    if (indexBaseMeta(ctx, base_expr)) |m| {
        if (isSliceHeaderMeta(m)) return sliceHeaderElemStride(ctx, m);
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
    // Stack i64 arrays: base - i*8. Packed bytes / raw ptr / slice `.ptr`: base + i*stride.
    const idx = try lowerExpr(ctx, index_expr);
    const stride = indexStride(ctx, base_expr);
    // `xs[i]` on a `{ptr,len}` slice header must index through `.ptr`, not the header.
    var data_base: u32 = undefined;
    var upward = false;
    if (indexBaseMeta(ctx, base_expr)) |m| {
        if (isSliceHeaderMeta(m)) {
            const hdr = try lowerExpr(ctx, base_expr);
            data_base = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = data_base, .src0 = hdr });
            upward = true;
        } else {
            data_base = try lowerExpr(ctx, base_expr);
            if (m.kind == .raw_ptr) upward = true;
            if (m.kind == .array_ptr and m.elem_size == 1) upward = true;
        }
    } else {
        data_base = try lowerExpr(ctx, base_expr);
    }
    const scale = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = scale, .aux = stride });
    const off = try ctx.fresh();
    try ctx.emit(.{ .op = .mul, .dst = off, .src0 = idx, .src1 = scale });
    const addr = try ctx.fresh();
    if (upward) {
        try ctx.emit(.{ .op = .add, .dst = addr, .src0 = data_base, .src1 = off });
    } else {
        try ctx.emit(.{ .op = .sub, .dst = addr, .src0 = data_base, .src1 = off });
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

    // Stack `[N]T` grows downward from lea_slot, but `[]T` indexing is upward
    // (`ptr + i*stride`). Materialize an upward copy so `xs[i]` matches `.ptr[i]`.
    var slice_ptr = data_ptr;
    if (!upward) {
        const nbytes = try ctx.fresh();
        try ctx.emit(.{ .op = .mul, .dst = nbytes, .src0 = len_v, .src1 = scale });
        const acid = ctx.callee_map.get("jogrt_alloc").?;
        const buf = try ctx.fresh();
        var ainst = Inst{ .op = .call, .dst = buf, .aux = acid, .arity = 1 };
        ainst.args[0] = nbytes;
        try ctx.emit(ainst);
        const iv = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = iv, .aux = 0 });
        const loop_head: i64 = @intCast(ctx.insts.items.len);
        const cmp = try ctx.fresh();
        try ctx.emit(.{ .op = .lt, .dst = cmp, .src0 = iv, .src1 = len_v });
        const jz_i = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jz, .src0 = cmp, .aux = 0 });
        const ioff = try ctx.fresh();
        try ctx.emit(.{ .op = .mul, .dst = ioff, .src0 = iv, .src1 = scale });
        const src = try ctx.fresh();
        try ctx.emit(.{ .op = .sub, .dst = src, .src0 = data_ptr, .src1 = ioff });
        const dst = try ctx.fresh();
        try ctx.emit(.{ .op = .add, .dst = dst, .src0 = buf, .src1 = ioff });
        if (stride == 1) {
            const b = try ctx.fresh();
            try ctx.emit(.{ .op = .load8, .dst = b, .src0 = src });
            try ctx.emit(.{ .op = .store8, .src0 = dst, .src1 = b });
        } else if (stride == 8) {
            const w = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = w, .src0 = src });
            try ctx.emit(.{ .op = .store, .src0 = dst, .src1 = w });
        } else {
            try emitWordCopy(ctx, dst, src, stride);
        }
        const one = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = one, .aux = 1 });
        try ctx.emit(.{ .op = .add, .dst = iv, .src0 = iv, .src1 = one });
        try ctx.emit(.{ .op = .jmp, .aux = loop_head });
        const end_pos: i64 = @intCast(ctx.insts.items.len);
        ctx.insts.items[jz_i].aux = end_pos;
        slice_ptr = buf;
    }

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
    try ctx.emit(.{ .op = .store, .src0 = hdr, .src1 = slice_ptr });
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
    // `for x in ch` (Chan) → recv until closed (JOG-242).
    const break_mark = ctx.break_jmps.items.len;
    const cont_mark = ctx.continue_jmps.items.len;
    const iter_tn = inferTypeNameFromExpr(ctx, fs.iter) orelse "";
    if (isChanTypeName(iter_tn)) {
        const chan = try lowerExpr(ctx, fs.iter);
        const esz = chanElemSize(ctx, iter_tn);
        const head: i64 = @intCast(ctx.insts.items.len);
        const dest = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = dest, .aux = 0 });
        const daddr = try ctx.fresh();
        try ctx.emit(.{ .op = .lea_slot, .dst = daddr, .src0 = dest });
        const sz = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = esz });
        const cid = ctx.callee_map.get("jogrt_chan_recv").?;
        const status = try ctx.fresh();
        var inst = Inst{ .op = .call, .dst = status, .aux = cid, .arity = 3 };
        inst.args[0] = chan;
        inst.args[1] = daddr;
        inst.args[2] = sz;
        try ctx.emit(inst);
        // status==2 → closed, break; status==1 → cancel, break; else bind and body
        const two = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = two, .aux = 2 });
        const is_closed = try ctx.fresh();
        try ctx.emit(.{ .op = .eq, .dst = is_closed, .src0 = status, .src1 = two });
        const jz_closed = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jz, .src0 = is_closed, .aux = 0 }); // if not closed, continue
        const jmp_end_closed = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jmp, .aux = 0 });
        const not_closed: i64 = @intCast(ctx.insts.items.len);
        ctx.insts.items[jz_closed].aux = not_closed;
        const one = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = one, .aux = 1 });
        const is_canc = try ctx.fresh();
        try ctx.emit(.{ .op = .eq, .dst = is_canc, .src0 = status, .src1 = one });
        const jz_canc = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jz, .src0 = is_canc, .aux = 0 });
        const jmp_end_canc = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jmp, .aux = 0 });
        const ok_pos: i64 = @intCast(ctx.insts.items.len);
        ctx.insts.items[jz_canc].aux = ok_pos;
        try ctx.slots.put(ctx.a, fs.name, dest);
        try ctx.meta.put(ctx.a, fs.name, .{ .kind = .scalar, .type_name = "i64" });
        try ctx.loop_defer_depths.append(ctx.a, ctx.defer_stack.items.len);
        try lowerBlock(ctx, fs.body);
        _ = ctx.loop_defer_depths.pop();
        const cont_pos: i64 = @intCast(ctx.insts.items.len);
        try ctx.emit(.{ .op = .jmp, .aux = head });
        const end_pos: i64 = @intCast(ctx.insts.items.len);
        ctx.insts.items[jmp_end_closed].aux = end_pos;
        ctx.insts.items[jmp_end_canc].aux = end_pos;
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
        try ctx.loop_defer_depths.append(ctx.a, ctx.defer_stack.items.len);
        try lowerBlock(ctx, fs.body);
        _ = ctx.loop_defer_depths.pop();
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
    try ctx.loop_defer_depths.append(ctx.a, ctx.defer_stack.items.len);
    try lowerBlock(ctx, fs.body);
    _ = ctx.loop_defer_depths.pop();
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

fn typeExprStructLeaf(ctx: *Ctx, te: *ast.TypeExpr) ?[]const u8 {
    return switch (te.*) {
        .name => |n| typeNameLeaf(resolveTypeSubst(ctx, n)),
        .named_app => |na| blk: {
            const mangled = mangleNamedApp(ctx.a, na.name, na.args) catch break :blk null;
            break :blk mangled;
        },
        else => null,
    };
}

const FieldHit = struct {
    offset: i64,
    ty: *ast.TypeExpr,
};

/// Direct field or promoted through `is_embedded` fields (Structs.md).
const PromotedMethod = struct {
    mi: MethodInfo,
    embed_off: i64,
};

fn findPromotedMethod(ctx: *Ctx, tn: []const u8, method: []const u8) ?PromotedMethod {
    const sd = ctx.structs.get(tn) orelse return null;
    var found: ?PromotedMethod = null;
    var off: i64 = 0;
    for (sd.fields) |f| {
        const fsz = fieldSlotSizeOf(ctx, f.ty);
        defer off += fsz;
        if (!f.is_embedded) continue;
        const etn = typeExprStructLeaf(ctx, f.ty) orelse continue;
        const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ typeNameLeaf(etn), method }) catch continue;
        if (ctx.methods.get(key)) |mi| {
            const hit: PromotedMethod = .{ .mi = mi, .embed_off = off };
            if (found != null) return null;
            found = hit;
            continue;
        }
        if (findPromotedMethod(ctx, etn, method)) |inner| {
            const hit: PromotedMethod = .{ .mi = inner.mi, .embed_off = off + inner.embed_off };
            if (found != null) return null;
            found = hit;
        }
    }
    return found;
}

fn findFieldInStruct(ctx: *Ctx, tn: []const u8, field: []const u8) ?FieldHit {
    const sd = ctx.structs.get(tn) orelse return null;
    var off: i64 = 0;
    for (sd.fields) |f| {
        if (std.mem.eql(u8, f.name, field)) return .{ .offset = off, .ty = f.ty };
        off += fieldSlotSizeOf(ctx, f.ty);
    }
    // Promotion: one level of embedding at a time; shallowest wins; ambiguous → null.
    var found: ?FieldHit = null;
    off = 0;
    for (sd.fields) |f| {
        const fsz = fieldSlotSizeOf(ctx, f.ty);
        defer off += fsz;
        if (!f.is_embedded) continue;
        const etn = typeExprStructLeaf(ctx, f.ty) orelse continue;
        if (findFieldInStruct(ctx, etn, field)) |inner| {
            const hit: FieldHit = .{ .offset = off + inner.offset, .ty = inner.ty };
            if (found != null) return null; // ambiguous at this depth
            found = hit;
        }
    }
    return found;
}

fn fieldOffset(ctx: *Ctx, base_expr: *ast.Expr, field: []const u8) LowerError!i64 {
    const tn = fieldStructName(ctx, base_expr) catch {
        std.debug.print("lower UnknownName fieldOffset type for field {s}\n", .{field});
        return error.UnknownName;
    };
    if (findFieldInStruct(ctx, tn, field)) |hit| return hit.offset;
    std.debug.print("lower UnknownName missing field {s}.{s}\n", .{ tn, field });
    return error.UnknownName;
}

fn fieldSizeBytes(ctx: *Ctx, base_expr: *ast.Expr, field: []const u8) LowerError!i64 {
    const tn = try fieldStructName(ctx, base_expr);
    if (findFieldInStruct(ctx, tn, field)) |hit| return fieldSlotSizeOf(ctx, hit.ty);
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

/// True when `name` is f32/f64 or a `type Alias: f64` chain that resolves to one.
fn isFloatTypeNameResolved(ctx: *Ctx, name: []const u8) bool {
    var cur = typeNameLeaf(name);
    var guard: u32 = 0;
    while (guard < 8) : (guard += 1) {
        if (isFloatTypeName(cur)) return true;
        const next = ctx.type_aliases.get(cur) orelse return false;
        const leaf = typeNameLeaf(next);
        if (std.mem.eql(u8, leaf, cur)) return false;
        cur = leaf;
    }
    return false;
}

fn isFloatish(ctx: *Ctx, e: *ast.Expr) bool {
    return switch (e.*) {
        .float => true,
        .ident => |n| blk: {
            if (ctx.meta.get(n)) |m| {
                break :blk isFloatTypeNameResolved(ctx, m.type_name);
            }
            if (ctx.const_vals.get(n)) |cv| break :blk cv.is_float;
            if (ctx.global_map.get(n)) |gi| {
                if (gi < ctx.globals.len) break :blk ctx.globals[gi].is_float;
            }
            break :blk false;
        },
        .field => blk: {
            if (inferMetaFromExpr(ctx, e)) |m| break :blk isFloatTypeNameResolved(ctx, m.type_name);
            break :blk false;
        },
        .binary => |b| blk: {
            if (binOpMethodName(b.op)) |opm| {
                if (inferTypeNameFromExpr(ctx, b.left)) |tn| {
                    if (!isBuiltinScalar(tn) and ctx.structs.contains(tn)) {
                        const key = std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ tn, opm }) catch break :blk false;
                        if (ctx.methods.get(key)) |mi| {
                            if (mi.ret_name.len > 0 and isFloatTypeNameResolved(ctx, mi.ret_name))
                                break :blk true;
                        }
                    }
                }
            }
            break :blk isFloatish(ctx, b.left) or isFloatish(ctx, b.right);
        },
        .unary => |u| isFloatish(ctx, u.expr),
        .cast => |c| isFloatTypeNameResolved(ctx, typeExprName(c.ty)),
        .call => blk: {
            if (inferMetaFromExpr(ctx, e)) |m| break :blk isFloatTypeNameResolved(ctx, m.type_name);
            break :blk false;
        },
        else => false,
    };
}

fn lowerExpr(ctx: *Ctx, e: *ast.Expr) LowerError!u32 {
    switch (e.*) {
        .ident => |name| {
            if (try loadCapturePtr(ctx, name)) |ptr| {
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .load, .dst = d, .src0 = ptr });
                return d;
            }
            if (ctx.slots.get(name)) |s| {
                // Captured local: slot holds the address; load the value.
                if (ctx.meta.get(name)) |m| {
                    if (m.kind == .capture_ptr) {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .load, .dst = d, .src0 = s });
                        return d;
                    }
                }
                return s;
            }
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
            // function address — fat pointer `{code, env=null}` for iface_fn values
            if (ctx.callee_map.contains(name)) {
                return try lowerFnValuePair(ctx, name, none);
            }
            // JOG-242: ErrCancelled / ErrClosed as bare names (pointer sentinels).
            if (std.mem.eql(u8, name, "ErrCancelled")) {
                const cid = ctx.callee_map.get("jogrt_err_cancelled").?;
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
                return d;
            }
            if (std.mem.eql(u8, name, "ErrClosed")) {
                const cid = ctx.callee_map.get("jogrt_err_closed").?;
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
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
            // Builtin `str`/`bytes` content equality (core methods are not merged into
            // every program yet — do not compare header pointers).
            if ((b.op == .eq or b.op == .neq) and isStrLikeExpr(ctx, b.left) and isStrLikeExpr(ctx, b.right)) {
                return try lowerStrContentEq(ctx, b.left, b.right, b.op == .neq);
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
                .eq, .neq => {
                    // Payload enums: deep-compare tag + fields (not pointer identity).
                    if (try lowerEnumEquality(ctx, b.left, b.right, l, r, b.op == .neq)) |eqd| {
                        try ctx.emit(.{ .op = .copy, .dst = d, .src0 = eqd });
                    } else {
                        try ctx.emit(.{ .op = if (b.op == .eq) .eq else .ne, .dst = d, .src0 = l, .src1 = r });
                    }
                },
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
            // Also `&base[i]` / `&base.field` — needed by posix `open_c_file` (`&buf[0]`).
            if (u.op == .addr) {
                switch (u.expr.*) {
                    .ident => |name| {
                        if (try loadCapturePtr(ctx, name)) |ptr| return ptr;
                        const d = try ctx.fresh();
                        if (ctx.slots.get(name)) |slot| {
                            // Array / struct / iface slots already hold a data pointer; &x is that pointer
                            // (LEA of the slot would pass a **T to C and break SDL createinfo / FRect FFI).
                            if (ctx.meta.get(name)) |m| {
                                if (m.kind == .array_ptr or m.kind == .struct_ptr or m.kind == .iface_ptr or m.kind == .capture_ptr) {
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
                    },
                    .index => |ix| return try lowerIndexAddr(ctx, ix.base, ix.index),
                    .field => |f| {
                        const base = try lowerExpr(ctx, f.base);
                        const off = try fieldOffset(ctx, f.base, f.name);
                        const addr = try ctx.fresh();
                        const off_s = try ctx.fresh();
                        try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                        try ctx.emit(.{ .op = .add, .dst = addr, .src0 = base, .src1 = off_s });
                        return addr;
                    },
                    else => {
                        std.debug.print("lower TypeError addr: non-lvalue\n", .{});
                        return error.TypeError;
                    },
                }
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
            const want = typeExprName(it.ty);
            // `e is Variant` — runtime tag test when want names a variant of e's enum.
            if (inferTypeNameFromExpr(ctx, it.expr)) |en| {
                if (ctx.enums.get(en)) |ei| {
                    for (ei.variants) |v| {
                        if (std.mem.eql(u8, v.name, want)) {
                            const scrut = try lowerExpr(ctx, it.expr);
                            const tag_slot = if (ei.has_payload) blk: {
                                const t = try ctx.fresh();
                                try ctx.emit(.{ .op = .load, .dst = t, .src0 = scrut });
                                break :blk t;
                            } else scrut;
                            const expect = try ctx.fresh();
                            try ctx.emit(.{ .op = .iconst, .dst = expect, .aux = v.value });
                            const d = try ctx.fresh();
                            try ctx.emit(.{ .op = .eq, .dst = d, .src0 = tag_slot, .src1 = expect });
                            return d;
                        }
                    }
                }
            }
            // Interface value: compare vtable type_id with TypeInfo for T (Interfaces.md).
            if (inferMetaFromExpr(ctx, it.expr)) |m| {
                if (m.kind == .iface_ptr) {
                    const fat = try lowerExpr(ctx, it.expr);
                    const eight = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
                    const vt_slot = try ctx.fresh();
                    try ctx.emit(.{ .op = .add, .dst = vt_slot, .src0 = fat, .src1 = eight });
                    const vt = try ctx.fresh();
                    try ctx.emit(.{ .op = .load, .dst = vt, .src0 = vt_slot });
                    const got_id = try ctx.fresh();
                    try ctx.emit(.{ .op = .load, .dst = got_id, .src0 = vt });
                    const ti = try ensureTypeInfo(ctx, want);
                    const want_id = try ctx.fresh();
                    try ctx.emit(.{ .op = .tiaddr, .dst = want_id, .aux = ti });
                    const d = try ctx.fresh();
                    try ctx.emit(.{ .op = .eq, .dst = d, .src0 = got_id, .src1 = want_id });
                    return d;
                }
            }
            // Structural: true if meta type_name matches T name.
            _ = try lowerExpr(ctx, it.expr);
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
            const dst_float = isFloatTypeNameResolved(ctx, to_name);
            // Pointer / optional / iface casts are bitcasts of the address word — never narrow.
            const to_is_ptr = c.ty.* == .pointer or c.ty.* == .pointer_const or c.ty.* == .optional or
                to.kind == .raw_ptr or to.kind == .struct_ptr or to.kind == .iface_ptr or to.kind == .opt_ptr;
            // Types.md: numeric `as` is a checked convert, not a bitcast.
            // Distinct float types (`type Celsius: f64`) share the f64 bit pattern — bitcast.
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
            // iface coerce from concrete struct ptr (`d as Named`, `&d as Named`)
            if (to.kind == .iface_ptr) {
                const concrete = concreteTypeForIfaceCoerce(ctx, c.expr);
                return try coerceToIface(ctx, v, concrete, to.type_name);
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
                            // Payload enums: bare `Shape.Empty` allocates tag-only value.
                            if (ei.has_payload) {
                                return try lowerEnumConstruct(ctx, f.base.ident, ei, v, &.{});
                            }
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
        .if_expr => |ifs| {
            const cond = try lowerExpr(ctx, ifs.cond);
            const result = try ctx.fresh();
            const jz_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jz, .src0 = cond, .aux = 0 });
            try lowerBlockAsValue(ctx, ifs.then_block, result);
            const jmp_i = ctx.insts.items.len;
            try ctx.emit(.{ .op = .jmp, .aux = 0 });
            const else_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jz_i].aux = else_pos;
            if (ifs.else_block) |eb| {
                try lowerBlockAsValue(ctx, eb, result);
            } else {
                try ctx.emit(.{ .op = .iconst, .dst = result, .aux = 0 });
            }
            const end_pos: i64 = @intCast(ctx.insts.items.len);
            ctx.insts.items[jmp_i].aux = end_pos;
            return result;
        },
        .when_expr => |w| {
            const scrut = try lowerExpr(ctx, w.scrutinee);
            const result = try ctx.fresh();
            const enum_name = inferTypeNameFromExpr(ctx, w.scrutinee);
            const payload_enum = blk: {
                if (enum_name) |en| {
                    if (ctx.enums.get(en)) |ei| break :blk ei.has_payload;
                }
                break :blk false;
            };
            // Payload enums store the tag at offset 0 of the heap value.
            const scrut_tag = if (payload_enum) blk: {
                const t = try ctx.fresh();
                try ctx.emit(.{ .op = .load, .dst = t, .src0 = scrut });
                break :blk t;
            } else scrut;
            var end_jmps: std.ArrayListUnmanaged(usize) = .{};
            for (w.arms) |arm| {
                var tag_val: i64 = undefined;
                var matched_fields: []ast.Field = &.{};
                var found = false;
                if (enum_name) |en| {
                    if (ctx.enums.get(en)) |ei| {
                        for (ei.variants) |v| {
                            if (std.mem.eql(u8, v.name, arm.pattern)) {
                                tag_val = v.value;
                                matched_fields = v.fields;
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
                                matched_fields = v.fields;
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
                try ctx.emit(.{ .op = .eq, .dst = cmp, .src0 = scrut_tag, .src1 = tag });
                const jz_i = ctx.insts.items.len;
                try ctx.emit(.{ .op = .jz, .src0 = cmp, .aux = 0 });
                // Bind payload locals: field i lives at offset 8*(1+i).
                const nbind = @min(arm.bindings.len, matched_fields.len);
                var bi: usize = 0;
                while (bi < nbind) : (bi += 1) {
                    const bname = arm.bindings[bi];
                    if (std.mem.eql(u8, bname, "_")) continue;
                    const off: i64 = @intCast(8 * (1 + bi));
                    const addr = try ctx.fresh();
                    const off_s = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
                    try ctx.emit(.{ .op = .add, .dst = addr, .src0 = scrut, .src1 = off_s });
                    const slot = try ctx.fresh();
                    try ctx.emit(.{ .op = .load, .dst = slot, .src0 = addr });
                    try ctx.slots.put(ctx.a, bname, slot);
                    if (metaForTypeExpr(ctx.a, matched_fields[bi].ty, ctx.structs, ctx.ifaces)) |m| {
                        try ctx.meta.put(ctx.a, bname, m);
                    } else |_| {}
                }
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
        .enum_lit => |el| {
            const tname = el.type_name orelse {
                std.debug.print("enum_lit missing type for .{s}\n", .{el.variant});
                return error.UnknownName;
            };
            const ei = ctx.enums.get(tname) orelse {
                std.debug.print("enum_lit unknown enum {s}\n", .{tname});
                return error.UnknownName;
            };
            for (ei.variants) |v| {
                if (std.mem.eql(u8, v.name, el.variant)) {
                    if (!ei.has_payload) {
                        const d = try ctx.fresh();
                        try ctx.emit(.{ .op = .iconst, .dst = d, .aux = v.value });
                        return d;
                    }
                    return try lowerEnumConstruct(ctx, tname, ei, v, el.fields);
                }
            }
            std.debug.print("enum_lit unknown variant {s}.{s}\n", .{ tname, el.variant });
            return error.UnknownName;
        },
        .lambda => |lam| return try lowerLambda(ctx, lam),
        .struct_lit => |sl| {
            // Named-arg call parsed as struct_lit: `connect(host: "db")`.
            if (ctx.fn_infos.get(sl.type_name)) |fi| {
                var args = try ctx.a.alloc(*ast.Expr, sl.fields.len);
                var names = try ctx.a.alloc(?[]const u8, sl.fields.len);
                for (sl.fields, 0..) |f, i| {
                    args[i] = f.value;
                    names[i] = f.name;
                }
                return try lowerFnCall(ctx, fi, args, &.{}, names);
            }
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
        .go_block => |gb| {
            for (gb.with_args) |w| _ = try lowerExpr(ctx, w);
            const caps = try collectGoCaptures(ctx, gb.body);
            const seq = ctx.go_seq.*;
            ctx.go_seq.* += 1;
            const name = try std.fmt.allocPrint(ctx.a, "__go_{d}", .{seq});
            try ctx.intern(.{ .kind = .jog_fn, .name = name });
            try ctx.fn_infos.put(ctx.a, name, .{
                .mangled = name,
                .nparams = 1,
                .contexts = &[_]ast.ContextParam{},
                .exported = false,
            });
            try ctx.pending_gos.append(ctx.a, .{
                .name = name,
                .body = gb.body,
                .capture_names = caps,
            });

            // Capture env: array of pointers to parent locals (or null if none).
            var env_ptr: u32 = none;
            if (caps.len > 0) {
                const nbytes: i64 = @intCast(caps.len * 8);
                const sz = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = nbytes });
                // Bump heap: must outlive the child until scope leave joins.
                const acid = ctx.callee_map.get("jogrt_alloc").?;
                env_ptr = try ctx.fresh();
                var allo = Inst{ .op = .call, .dst = env_ptr, .aux = acid, .arity = 1 };
                allo.args[0] = sz;
                try ctx.emit(allo);
                for (caps, 0..) |cname, i| {
                    const slot = ctx.slots.get(cname) orelse return error.UnknownName;
                    const p = try ctx.fresh();
                    // Struct/array slots already hold pointers; scalars need lea.
                    if (ctx.meta.get(cname)) |m| {
                        if (m.kind == .array_ptr or m.kind == .struct_ptr or m.kind == .iface_ptr) {
                            try ctx.emit(.{ .op = .copy, .dst = p, .src0 = slot });
                        } else {
                            try ctx.emit(.{ .op = .lea_slot, .dst = p, .src0 = slot });
                        }
                    } else {
                        try ctx.emit(.{ .op = .lea_slot, .dst = p, .src0 = slot });
                    }
                    const off = try ctx.fresh();
                    try ctx.emit(.{ .op = .iconst, .dst = off, .aux = @as(i64, @intCast(i)) * 8 });
                    const addr = try ctx.fresh();
                    try ctx.emit(.{ .op = .add, .dst = addr, .src0 = env_ptr, .src1 = off });
                    try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = p });
                }
            } else {
                env_ptr = try ctx.fresh();
                try ctx.emit(.{ .op = .iconst, .dst = env_ptr, .aux = 0 });
            }

            const scope = blk: {
                if (ctx.scope_slot) |s| break :blk s;
                const cid = ctx.callee_map.get("jogrt_scope_current").?;
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
                break :blk d;
            };
            const fn_addr = try ctx.fresh();
            const fcid = ctx.callee_map.get(name).?;
            try ctx.emit(.{ .op = .saddr, .dst = fn_addr, .aux = -1 - @as(i64, @intCast(fcid)) });

            // GoWrap { fn, arg, scope } — 24 bytes; bump heap until scope leave.
            const wraps = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = wraps, .aux = 24 });
            const wcid = ctx.callee_map.get("jogrt_alloc").?;
            const wrap = try ctx.fresh();
            var wallo = Inst{ .op = .call, .dst = wrap, .aux = wcid, .arity = 1 };
            wallo.args[0] = wraps;
            try ctx.emit(wallo);

            const gcid = ctx.callee_map.get("jogrt_scope_go").?;
            const task = try ctx.fresh();
            var ginst = Inst{ .op = .call, .dst = task, .aux = gcid, .arity = 4 };
            ginst.args[0] = scope;
            ginst.args[1] = fn_addr;
            ginst.args[2] = env_ptr;
            ginst.args[3] = wrap;
            try ctx.emit(ginst);
            return task;
        },
        .go_expr => |ge| {
            // Lower as go { return <expr> }.
            const stmts = try ctx.a.alloc(ast.Stmt, 1);
            stmts[0] = .{ .return_stmt = ge };
            const body = try ctx.a.create(ast.Block);
            body.* = .{ .stmts = stmts };
            const wrapper = try ctx.a.create(ast.Expr);
            wrapper.* = .{ .go_block = .{ .with_args = &.{}, .body = body } };
            return try lowerExpr(ctx, wrapper);
        },
        .scope_block => |sb| {
            const ecid = ctx.callee_map.get("jogrt_scope_enter").?;
            const entered = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = entered, .aux = ecid, .arity = 0 });
            const saved = ctx.scope_slot;
            ctx.scope_slot = entered;
            defer ctx.scope_slot = saved;
            try lowerBlock(ctx, sb.body);
            const lcid = ctx.callee_map.get("jogrt_scope_leave").?;
            var linst = Inst{ .op = .call, .dst = none, .aux = lcid, .arity = 1 };
            linst.args[0] = entered;
            try ctx.emit(linst);
            const zero = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = zero, .aux = 0 });
            return zero;
        },
        .select_block => |sb| return try lowerSelectBlock(ctx, sb),
        .type_value => return error.TypeError, // only valid as Chan<T>.init receiver
    }
}

fn emitTaskSetIfGoChild(ctx: *Ctx, ret_slot: u32) LowerError!void {
    if (ctx.capture_map == null or ret_slot == none) return;
    const vaddr = try ctx.fresh();
    try ctx.emit(.{ .op = .lea_slot, .dst = vaddr, .src0 = ret_slot });
    const sz = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = 8 });
    const tcid = ctx.callee_map.get("jogrt_task_set").?;
    var tinst = Inst{ .op = .call, .dst = none, .aux = tcid, .arity = 2 };
    tinst.args[0] = vaddr;
    tinst.args[1] = sz;
    try ctx.emit(tinst);
}

fn chanElemSize(ctx: *Ctx, mangled: []const u8) i64 {
    // Chan__i64 → i64 size
    if (std.mem.indexOf(u8, mangled, "__")) |sep| {
        const elem = mangled[sep + 2 ..];
        return layoutSizeOf(ctx, elem);
    }
    return 8;
}

fn isChanTypeName(name: []const u8) bool {
    return std.mem.startsWith(u8, name, "Chan__") or std.mem.eql(u8, name, "Chan");
}

fn isTaskTypeName(name: []const u8) bool {
    return std.mem.startsWith(u8, name, "Task__") or std.mem.eql(u8, name, "Task");
}

fn errorFromStatus(ctx: *Ctx, status: u32) LowerError!u32 {
    // status 0 → null; 1 → ErrCancelled; 2 → ErrClosed (pointer sentinels).
    const err = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = err, .aux = 0 });
    // if status == 0: err=0; else if status == 2: ErrClosed; else ErrCancelled
    const jz_ok = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = status, .aux = 0 }); // patch to end if status==0

    const two = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = two, .aux = 2 });
    const is_closed = try ctx.fresh();
    try ctx.emit(.{ .op = .eq, .dst = is_closed, .src0 = status, .src1 = two });
    const jz_not_closed = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = is_closed, .aux = 0 });

    const cid_closed = ctx.callee_map.get("jogrt_err_closed").?;
    const closed = try ctx.fresh();
    try ctx.emit(.{ .op = .call, .dst = closed, .aux = cid_closed, .arity = 0 });
    try ctx.emit(.{ .op = .copy, .dst = err, .src0 = closed });
    const jmp_end = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jmp, .aux = 0 });

    const cancelled_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_not_closed].aux = cancelled_pos;
    const cid_canc = ctx.callee_map.get("jogrt_err_cancelled").?;
    const canc = try ctx.fresh();
    try ctx.emit(.{ .op = .call, .dst = canc, .aux = cid_canc, .arity = 0 });
    try ctx.emit(.{ .op = .copy, .dst = err, .src0 = canc });

    const end_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_ok].aux = end_pos;
    ctx.insts.items[jmp_end].aux = end_pos;
    return err;
}

fn lowerChanOrTaskCall(ctx: *Ctx, c: ast.Call) LowerError!?u32 {
    if (c.callee.* != .field) return null;
    const fname = c.callee.field.name;
    const base_expr = c.callee.field.base;

    // Chan<T>.init(cap)
    if (base_expr.* == .type_value) {
        const tv = base_expr.type_value;
        if (!std.mem.eql(u8, tv.name, "Chan") and !std.mem.eql(u8, typeNameLeaf(tv.name), "Chan")) return null;
        if (!std.mem.eql(u8, fname, "init")) return null;
        if (c.args.len > 1) return error.TooManyArgs;
        const mangled = try mangleNamedApp(ctx.a, "Chan", tv.args);
        const esz = chanElemSize(ctx, mangled);
        const es = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = es, .aux = esz });
        const cap = if (c.args.len == 1) try lowerExpr(ctx, c.args[0]) else blk: {
            const z = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
            break :blk z;
        };
        const cid = ctx.callee_map.get("jogrt_chan_init").?;
        const d = try ctx.fresh();
        var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 2 };
        inst.args[0] = es;
        inst.args[1] = cap;
        try ctx.emit(inst);
        // Record meta so later ch.send sees Chan__T.
        return d;
    }

    const base_tn = inferTypeNameFromExpr(ctx, base_expr) orelse "";
    // Also accept typed locals whose name starts with Chan__/Task__ after init meta.
    // For untyped `val ch = Chan<i64>.init(…)`, set meta on the var_decl via type_name hack:
    // we can't easily set it here. Infer from a preceding typed decl or force Chan__i64.
    // Workaround: if method is send/recv/close/free/join, treat as Chan/Task when name matches
    // OR when the base was produced by chan_init (raw pointer) — use Chan__i64 default.
    const treat_chan = isChanTypeName(base_tn) or
        (base_tn.len == 0 and (std.mem.eql(u8, fname, "send") or std.mem.eql(u8, fname, "recv") or
            std.mem.eql(u8, fname, "close") or std.mem.eql(u8, fname, "free")));
    const treat_task = isTaskTypeName(base_tn) or
        (base_tn.len == 0 and std.mem.eql(u8, fname, "join"));

    if (treat_task and std.mem.eql(u8, fname, "join")) {
        const task = try lowerExpr(ctx, base_expr);
        // Unit join: size 0. Valued: assume i64 (8) for this slice.
        const out = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = out, .aux = 0 });
        const oaddr = try ctx.fresh();
        try ctx.emit(.{ .op = .lea_slot, .dst = oaddr, .src0 = out });
        const sz = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = 8 });
        const cid = ctx.callee_map.get("jogrt_task_join").?;
        var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 3 };
        inst.args[0] = task;
        inst.args[1] = oaddr;
        inst.args[2] = sz;
        try ctx.emit(inst);
        return out;
    }

    if (!treat_chan) return null;
    const chan = try lowerExpr(ctx, base_expr);
    const tn = if (isChanTypeName(base_tn)) base_tn else "Chan__i64";
    const esz = chanElemSize(ctx, tn);

    if (std.mem.eql(u8, fname, "close")) {
        const cid = ctx.callee_map.get("jogrt_chan_close").?;
        var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
        inst.args[0] = chan;
        try ctx.emit(inst);
        const z = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
        return z;
    }
    if (std.mem.eql(u8, fname, "free")) {
        const cid = ctx.callee_map.get("jogrt_chan_free").?;
        var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
        inst.args[0] = chan;
        try ctx.emit(inst);
        const z = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
        return z;
    }
    if (std.mem.eql(u8, fname, "send")) {
        if (c.args.len != 1) return error.TooManyArgs;
        const val = try lowerExpr(ctx, c.args[0]);
        const vslot = try ctx.fresh();
        try ctx.emit(.{ .op = .copy, .dst = vslot, .src0 = val });
        const vaddr = try ctx.fresh();
        try ctx.emit(.{ .op = .lea_slot, .dst = vaddr, .src0 = vslot });
        const sz = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = esz });
        const cid = ctx.callee_map.get("jogrt_chan_send").?;
        const status = try ctx.fresh();
        var inst = Inst{ .op = .call, .dst = status, .aux = cid, .arity = 3 };
        inst.args[0] = chan;
        inst.args[1] = vaddr;
        inst.args[2] = sz;
        try ctx.emit(inst);
        return try errorFromStatus(ctx, status);
    }
    if (std.mem.eql(u8, fname, "recv")) {
        if (c.args.len != 0) return error.TooManyArgs;
        const dest = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = dest, .aux = 0 });
        const daddr = try ctx.fresh();
        try ctx.emit(.{ .op = .lea_slot, .dst = daddr, .src0 = dest });
        const sz = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = esz });
        const cid = ctx.callee_map.get("jogrt_chan_recv").?;
        const status = try ctx.fresh();
        var inst = Inst{ .op = .call, .dst = status, .aux = cid, .arity = 3 };
        inst.args[0] = chan;
        inst.args[1] = daddr;
        inst.args[2] = sz;
        try ctx.emit(inst);
        const err = try errorFromStatus(ctx, status);
        // Return tuple (T, error?) as __Tup2.
        const tup_name = try ensureTupleStruct(ctx.a, ctx.structs, 2);
        const hdr = try lowerEmptyStructValue(ctx, tup_name);
        // store .0 = dest, .1 = err
        const off0 = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = off0, .aux = 0 });
        const a0 = try ctx.fresh();
        try ctx.emit(.{ .op = .add, .dst = a0, .src0 = hdr, .src1 = off0 });
        try ctx.emit(.{ .op = .store, .src0 = a0, .src1 = dest });
        const off1 = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = off1, .aux = 8 });
        const a1 = try ctx.fresh();
        try ctx.emit(.{ .op = .add, .dst = a1, .src0 = hdr, .src1 = off1 });
        try ctx.emit(.{ .op = .store, .src0 = a1, .src1 = err });
        return hdr;
    }
    return null;
}

fn lowerSelectBlock(ctx: *Ctx, sb: ast.SelectBlock) LowerError!u32 {
    const begin = ctx.callee_map.get("jogrt_select_begin").?;
    try ctx.emit(.{ .op = .call, .dst = none, .aux = begin, .arity = 0 });

    // Each stmt should be an expr call: recv/send/timeout/default with trailing lambda.
    var case_ids: std.ArrayListUnmanaged(u32) = .{};
    var case_lambdas: std.ArrayListUnmanaged(?*ast.Expr) = .{};
    var case_slots: std.ArrayListUnmanaged(?u32) = .{};
    defer case_ids.deinit(ctx.a);
    defer case_lambdas.deinit(ctx.a);
    defer case_slots.deinit(ctx.a);

    for (sb.body.stmts) |st| {
        const ex = switch (st) {
            .expr => |e| e,
            else => return error.TypeError,
        };
        if (ex.* != .call) return error.TypeError;
        const c = ex.call;
        const cname = blk: {
            if (c.callee.* == .ident) break :blk c.callee.ident;
            return error.TypeError;
        };
        // Trailing lambda is the last arg when present.
        var lam: ?*ast.Expr = null;
        var value_args: []*ast.Expr = c.args;
        if (c.args.len > 0 and c.args[c.args.len - 1].* == .lambda) {
            lam = c.args[c.args.len - 1];
            value_args = c.args[0 .. c.args.len - 1];
        }
        const id = try ctx.fresh();
        var slot: ?u32 = null;
        if (std.mem.eql(u8, cname, "default")) {
            const cid = ctx.callee_map.get("jogrt_select_add_default").?;
            try ctx.emit(.{ .op = .call, .dst = id, .aux = cid, .arity = 0 });
        } else if (std.mem.eql(u8, cname, "timeout")) {
            if (value_args.len != 1) return error.TooManyArgs;
            const ms = try lowerExpr(ctx, value_args[0]);
            const cid = ctx.callee_map.get("jogrt_select_add_timeout").?;
            var inst = Inst{ .op = .call, .dst = id, .aux = cid, .arity = 1 };
            inst.args[0] = ms;
            try ctx.emit(inst);
        } else if (std.mem.eql(u8, cname, "recv")) {
            if (value_args.len != 1) return error.TooManyArgs;
            const chan = try lowerExpr(ctx, value_args[0]);
            const tn = inferTypeNameFromExpr(ctx, value_args[0]) orelse "Chan__i64";
            const esz = chanElemSize(ctx, tn);
            const dest = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = dest, .aux = 0 });
            slot = dest;
            const daddr = try ctx.fresh();
            try ctx.emit(.{ .op = .lea_slot, .dst = daddr, .src0 = dest });
            const sz = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = esz });
            const cid = ctx.callee_map.get("jogrt_select_add_recv").?;
            var inst = Inst{ .op = .call, .dst = id, .aux = cid, .arity = 3 };
            inst.args[0] = chan;
            inst.args[1] = daddr;
            inst.args[2] = sz;
            try ctx.emit(inst);
        } else if (std.mem.eql(u8, cname, "send")) {
            if (value_args.len != 2) return error.TooManyArgs;
            const chan = try lowerExpr(ctx, value_args[0]);
            const val = try lowerExpr(ctx, value_args[1]);
            const tn = inferTypeNameFromExpr(ctx, value_args[0]) orelse "Chan__i64";
            const esz = chanElemSize(ctx, tn);
            // Spill value to a slot and pass its address.
            const vslot = try ctx.fresh();
            try ctx.emit(.{ .op = .copy, .dst = vslot, .src0 = val });
            const vaddr = try ctx.fresh();
            try ctx.emit(.{ .op = .lea_slot, .dst = vaddr, .src0 = vslot });
            const sz = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = esz });
            const cid = ctx.callee_map.get("jogrt_select_add_send").?;
            var inst = Inst{ .op = .call, .dst = id, .aux = cid, .arity = 3 };
            inst.args[0] = chan;
            inst.args[1] = vaddr;
            inst.args[2] = sz;
            try ctx.emit(inst);
        } else return error.TypeError;
        try case_ids.append(ctx.a, id);
        try case_lambdas.append(ctx.a, lam);
        try case_slots.append(ctx.a, slot);
    }

    const wait_cid = ctx.callee_map.get("jogrt_select_wait").?;
    const chosen = try ctx.fresh();
    try ctx.emit(.{ .op = .call, .dst = chosen, .aux = wait_cid, .arity = 0 });
    const st_cid = ctx.callee_map.get("jogrt_select_status").?;
    const status = try ctx.fresh();
    try ctx.emit(.{ .op = .call, .dst = status, .aux = st_cid, .arity = 0 });

    // Dispatch case bodies.
    var end_jmps: std.ArrayListUnmanaged(usize) = .{};
    defer end_jmps.deinit(ctx.a);
    for (case_ids.items, 0..) |cid, i| {
        const eq = try ctx.fresh();
        try ctx.emit(.{ .op = .eq, .dst = eq, .src0 = chosen, .src1 = cid });
        const jz = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jz, .src0 = eq, .aux = 0 });
        // status == 0 to run body
        const ok = try ctx.fresh();
        const zero = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = zero, .aux = 0 });
        try ctx.emit(.{ .op = .eq, .dst = ok, .src0 = status, .src1 = zero });
        const jz_bad = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jz, .src0 = ok, .aux = 0 });
        if (case_lambdas.items[i]) |lam_expr| {
            if (lam_expr.* == .lambda) {
                const lam = lam_expr.lambda;
                // Bind first param to recv slot when present.
                if (case_slots.items[i]) |sl| {
                    if (lam.params.len > 0) {
                        try ctx.slots.put(ctx.a, lam.params[0].name, sl);
                        try ctx.meta.put(ctx.a, lam.params[0].name, .{ .kind = .scalar, .type_name = "i64" });
                    }
                }
                if (lam.block) |b| try lowerBlock(ctx, b);
                if (lam.expr_body) |eb| _ = try lowerExpr(ctx, eb);
            }
        }
        const jmp = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jmp, .aux = 0 });
        try end_jmps.append(ctx.a, jmp);
        const next: i64 = @intCast(ctx.insts.items.len);
        ctx.insts.items[jz].aux = next;
        ctx.insts.items[jz_bad].aux = next;
    }
    const end_pos: i64 = @intCast(ctx.insts.items.len);
    for (end_jmps.items) |ji| ctx.insts.items[ji].aux = end_pos;
    return try errorFromStatus(ctx, status);
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

/// Allocate a payload-enum value: `[tag][field0][field1]…` (8-byte slots).
fn lowerEnumConstruct(
    ctx: *Ctx,
    type_name: []const u8,
    ei: EnumInfo,
    variant: EnumVariantVal,
    fields: []ast.StructLitField,
) LowerError!u32 {
    _ = type_name;
    const nfields = ei.max_fields;
    const size: i64 = @intCast(8 * (1 + nfields));
    const sz = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = sz, .aux = if (size < 8) 8 else size });
    // Durable bump alloc — enum values outlive the expression (locals / params).
    const cid = ctx.callee_map.get("jogrt_alloc").?;
    const ptr = try ctx.fresh();
    var allo = Inst{ .op = .call, .dst = ptr, .aux = cid, .arity = 1 };
    allo.args[0] = sz;
    try ctx.emit(allo);
    // Store tag at offset 0.
    const tag_s = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = tag_s, .aux = variant.value });
    try ctx.emit(.{ .op = .store, .src0 = ptr, .src1 = tag_s });
    // Store each payload field by declaration order / named match.
    for (variant.fields, 0..) |vf, i| {
        var val_expr: ?*ast.Expr = null;
        for (fields) |flit| {
            if (std.mem.eql(u8, flit.name, vf.name)) {
                val_expr = flit.value;
                break;
            }
        }
        const val = if (val_expr) |ve|
            try lowerExpr(ctx, ve)
        else blk: {
            const z = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
            break :blk z;
        };
        const off: i64 = @intCast(8 * (1 + i));
        const addr = try ctx.fresh();
        const off_s = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = off });
        try ctx.emit(.{ .op = .add, .dst = addr, .src0 = ptr, .src1 = off_s });
        try ctx.emit(.{ .op = .store, .src0 = addr, .src1 = val });
    }
    return ptr;
}

/// Deep-compare two payload-enum values. Returns null if not payload enums (caller uses scalar eq).
fn lowerEnumEquality(ctx: *Ctx, left_e: *ast.Expr, right_e: *ast.Expr, l: u32, r: u32, negate: bool) LowerError!?u32 {
    const ln = inferTypeNameFromExpr(ctx, left_e) orelse return null;
    const rn = inferTypeNameFromExpr(ctx, right_e) orelse return null;
    if (!std.mem.eql(u8, ln, rn)) return null;
    const ei = ctx.enums.get(ln) orelse return null;
    if (!ei.has_payload) return null;

    // result starts true; clear on mismatch.
    const result = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = result, .aux = 1 });

    const ltag = try ctx.fresh();
    const rtag = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = ltag, .src0 = l });
    try ctx.emit(.{ .op = .load, .dst = rtag, .src0 = r });
    const tags_eq = try ctx.fresh();
    try ctx.emit(.{ .op = .eq, .dst = tags_eq, .src0 = ltag, .src1 = rtag });
    // if tags differ → result = 0; skip field compares
    const jz_tag = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = tags_eq, .aux = 0 });

    // For each variant: if tag matches, compare that variant's fields.
    var end_jmps: std.ArrayListUnmanaged(usize) = .{};
    for (ei.variants) |v| {
        const expect = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = expect, .aux = v.value });
        const is_v = try ctx.fresh();
        try ctx.emit(.{ .op = .eq, .dst = is_v, .src0 = ltag, .src1 = expect });
        const jz_v = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jz, .src0 = is_v, .aux = 0 });
        for (v.fields, 0..) |_, i| {
            const off: i64 = @intCast(8 * (1 + i));
            const loff = try ctx.fresh();
            const roff = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = loff, .aux = off });
            try ctx.emit(.{ .op = .iconst, .dst = roff, .aux = off });
            const laddr = try ctx.fresh();
            const raddr = try ctx.fresh();
            try ctx.emit(.{ .op = .add, .dst = laddr, .src0 = l, .src1 = loff });
            try ctx.emit(.{ .op = .add, .dst = raddr, .src0 = r, .src1 = roff });
            const lv = try ctx.fresh();
            const rv = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = lv, .src0 = laddr });
            try ctx.emit(.{ .op = .load, .dst = rv, .src0 = raddr });
            const feq = try ctx.fresh();
            try ctx.emit(.{ .op = .eq, .dst = feq, .src0 = lv, .src1 = rv });
            // result &= feq (bitwise — and_op is short-circuit-only and lowers to ud2)
            try ctx.emit(.{ .op = .bit_and, .dst = result, .src0 = result, .src1 = feq });
        }
        const jmp_done = ctx.insts.items.len;
        try ctx.emit(.{ .op = .jmp, .aux = 0 });
        try end_jmps.append(ctx.a, jmp_done);
        const next_v: i64 = @intCast(ctx.insts.items.len);
        ctx.insts.items[jz_v].aux = next_v;
    }
    const fields_done: i64 = @intCast(ctx.insts.items.len);
    for (end_jmps.items) |ji| ctx.insts.items[ji].aux = fields_done;

    // tags differed
    const jmp_ok = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jmp, .aux = 0 });
    const tag_fail: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_tag].aux = tag_fail;
    try ctx.emit(.{ .op = .iconst, .dst = result, .aux = 0 });
    const end: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jmp_ok].aux = end;

    if (negate) {
        const out = try ctx.fresh();
        const one = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = one, .aux = 1 });
        try ctx.emit(.{ .op = .bit_xor, .dst = out, .src0 = result, .src1 = one });
        return out;
    }
    return result;
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
    // Real structs first — `Point` / `Player` must not hit the `P…` mangling heuristic.
    if (ctx.structs.get(leaf)) |sd| {
        var total: i64 = 0;
        for (sd.fields) |f| {
            total += fieldSlotSizeOf(ctx, f.ty);
        }
        if (total == 0) return 8;
        return total;
    }
    // `PValue` / `Opt…` from type_subst / mangling — machine-word / optional pointer.
    if (leaf.len > 0 and (leaf[0] == 'P' or std.mem.startsWith(u8, leaf, "Opt")))
        return 8;
    if (std.mem.eql(u8, leaf, "str") or std.mem.eql(u8, leaf, "bytes")) return 16;
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
        // JOG-229: `typeInfo(T)` → *TypeInfo (rodata); `typeName(ti)` → str from record.
        if (std.mem.eql(u8, name, "typeInfo")) {
            const tname = blk: {
                if (c.type_args.len == 1) break :blk try resolveTypeExprName(ctx, c.type_args[0]);
                if (c.args.len == 1 and c.args[0].* == .ident) break :blk resolveTypeSubst(ctx, c.args[0].ident);
                return error.TypeError;
            };
            const ti = try ensureTypeInfo(ctx, tname);
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .tiaddr, .dst = d, .aux = ti });
            return d;
        }
        if (std.mem.eql(u8, name, "typeName")) {
            if (c.args.len != 1) return error.TypeError;
            const ti_ptr = try lowerExpr(ctx, c.args[0]);
            // Layout: kind@0, name.ptr@8, name.len@16, size@24
            const eight = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
            const name_ptr_addr = try ctx.fresh();
            try ctx.emit(.{ .op = .add, .dst = name_ptr_addr, .src0 = ti_ptr, .src1 = eight });
            const name_ptr = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = name_ptr, .src0 = name_ptr_addr });
            const sixteen = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = sixteen, .aux = 16 });
            const name_len_addr = try ctx.fresh();
            try ctx.emit(.{ .op = .add, .dst = name_len_addr, .src0 = ti_ptr, .src1 = sixteen });
            const name_len = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = name_len, .src0 = name_len_addr });
            const hdr = try lowerEmptyStructValue(ctx, blk: {
                const elem = try ctx.a.create(ast.TypeExpr);
                elem.* = .{ .name = "byte" };
                break :blk try ensureSliceStruct(ctx.a, ctx.structs, elem);
            });
            try ctx.emit(.{ .op = .store, .src0 = hdr, .src1 = name_ptr });
            const len_addr = try ctx.fresh();
            try ctx.emit(.{ .op = .add, .dst = len_addr, .src0 = hdr, .src1 = eight });
            try ctx.emit(.{ .op = .store, .src0 = len_addr, .src1 = name_len });
            return hdr;
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
        // JOG-242: jogrt_go returns the coro/task id.
        if (std.mem.eql(u8, name, "go")) {
            if (c.args.len != 2) return error.TooManyArgs;
            const fp = try lowerExpr(ctx, c.args[0]);
            const arg = try lowerExpr(ctx, c.args[1]);
            const cid = ctx.callee_map.get("jogrt_go").?;
            const d = try ctx.fresh();
            var inst = Inst{ .op = .call, .dst = d, .aux = cid, .arity = 2 };
            inst.args[0] = fp;
            inst.args[1] = arg;
            try ctx.emit(inst);
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
        // JOG-242: cooperative sleep (returns error? / null).
        if (std.mem.eql(u8, name, "sleep")) {
            if (c.args.len != 1) return error.TooManyArgs;
            const ms = try lowerExpr(ctx, c.args[0]);
            const cid = ctx.callee_map.get("jogrt_sleep").?;
            const status = try ctx.fresh();
            var inst = Inst{ .op = .call, .dst = status, .aux = cid, .arity = 1 };
            inst.args[0] = ms;
            try ctx.emit(inst);
            return try errorFromStatus(ctx, status);
        }
        if (std.mem.eql(u8, name, "cancel")) {
            if (c.args.len != 0) return error.TooManyArgs;
            const scope = blk: {
                if (ctx.scope_slot) |s| break :blk s;
                const cid = ctx.callee_map.get("jogrt_scope_current").?;
                const d = try ctx.fresh();
                try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
                break :blk d;
            };
            const cid = ctx.callee_map.get("jogrt_scope_cancel").?;
            var inst = Inst{ .op = .call, .dst = none, .aux = cid, .arity = 1 };
            inst.args[0] = scope;
            try ctx.emit(inst);
            const z = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = z, .aux = 0 });
            return z;
        }
        if (std.mem.eql(u8, name, "ErrCancelled")) {
            const cid = ctx.callee_map.get("jogrt_err_cancelled").?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
            return d;
        }
        if (std.mem.eql(u8, name, "ErrClosed")) {
            const cid = ctx.callee_map.get("jogrt_err_closed").?;
            const d = try ctx.fresh();
            try ctx.emit(.{ .op = .call, .dst = d, .aux = cid, .arity = 0 });
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
                return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
            }
        }
        if (ctx.fn_infos.get(name)) |fi| {
            return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
        }
        // Inside specialized bodies (`store_elem` in `List<*Value>.push`): bind via type_subst
        // so `store_elem__PValue` is found even when arg meta peels to `Value`.
        if (c.type_args.len == 0 and ctx.type_subst.count() == 1) {
            var it = ctx.type_subst.iterator();
            if (it.next()) |e| {
                const mangled = try std.fmt.allocPrint(ctx.a, "{s}__{s}", .{ name, e.value_ptr.* });
                if (ctx.fn_infos.get(mangled)) |fi| {
                    return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
                }
            }
        }
        // Inference-only: id(42) → id__i64 when specialized instance exists.
        if (c.type_args.len == 0 and c.args.len >= 1) {
            const atn = inferTypeNameFromExpr(ctx, c.args[0]) orelse (if (c.args[0].* == .int) "i64" else "");
            if (atn.len > 0) {
                const mangled = try std.fmt.allocPrint(ctx.a, "{s}__{s}", .{ name, atn });
                if (ctx.fn_infos.get(mangled)) |fi| {
                    return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
                }
            }
        }
    }
    if (c.callee.* == .field) {
        // `pkg.fn(...)` after `import std.pkg` — flattened merge exposes `fn` unqualified.
        // When the base is an unresolved ident (import alias), look up the free function.
        if (c.callee.field.base.* == .ident) {
            const base_name = c.callee.field.base.ident;
            const is_capture = if (ctx.capture_map) |cm| cm.contains(base_name) else false;
            const is_local = is_capture or ctx.meta.contains(base_name) or ctx.slots.contains(base_name) or
                ctx.global_map.contains(base_name) or ctx.object_map.contains(base_name) or ctx.const_vals.contains(base_name);
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
                        return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
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
                                return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
                            }
                        }
                    }
                }
                if (c.args.len >= 1) {
                    if (inferTypeNameFromExpr(ctx, c.args[0])) |atn| {
                        const mangled = try std.fmt.allocPrint(ctx.a, "{s}__{s}", .{ fname, atn });
                        if (ctx.fn_infos.get(mangled)) |fi| {
                            return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
                        }
                    }
                }
                if (ctx.fn_infos.get(fname)) |fi| {
                    return try lowerFnCall(ctx, fi, c.args, c.with_args, c.arg_names);
                }
                std.debug.print("lower UnknownName pkg fn {s}.{s}\n", .{ base_name, fname });
                return error.UnknownName;
            }
        }
        // JOG-242: Chan<T>.init / ch.send|recv|close|free / task.join
        if (try lowerChanOrTaskCall(ctx, c)) |r| return r;
        const base = try lowerExpr(ctx, c.callee.field.base);
        return try lowerMethodOrIfaceCall(ctx, c.callee.field.base, base, c.callee.field.name, c.args, c.with_args);
    }
    // Call through a function-typed local/param (`body(x)` in List.each).
    if (c.callee.* == .ident) {
        if (ctx.slots.get(c.callee.ident)) |fp| {
            if (ctx.meta.get(c.callee.ident)) |m| {
                if (std.mem.eql(u8, m.type_name, "iface_fn") or std.mem.eql(u8, m.type_name, "closure")) {
                    return try lowerFnPtrCall(ctx, fp, c.args, std.mem.eql(u8, m.type_name, "closure"));
                }
            }
        }
    }
    return error.UnknownName;
}

/// Expand named args + defaults into a full positional list matching `params`.
fn expandArgsForLower(
    a: std.mem.Allocator,
    params: []ast.Param,
    args: []*ast.Expr,
    arg_names: []const ?[]const u8,
) LowerError![]*ast.Expr {
    if (params.len == 0) return args;
    // Fast path: all positional, correct arity, no defaults needed.
    var any_default = false;
    for (params) |p| {
        if (p.default != null) any_default = true;
    }
    const any_named = arg_names.len > 0;
    if (!any_default and !any_named and args.len == params.len) return args;
    if (!any_default and !any_named and args.len != params.len) {
        // Still may be trailing-lambda short form with defaults — fall through if defaults exist.
        if (!any_default) return args;
    }

    const n = params.len;
    var out = try a.alloc(?*ast.Expr, n);
    @memset(out, null);

    var usable_len = args.len;
    if (args.len > 0 and n > 0 and args[args.len - 1].* == .lambda) {
        out[n - 1] = args[args.len - 1];
        usable_len = args.len - 1;
    }

    var next_pos: usize = 0;
    var i: usize = 0;
    while (i < usable_len) : (i += 1) {
        const name: ?[]const u8 = if (arg_names.len == args.len)
            arg_names[i]
        else if (arg_names.len == usable_len)
            arg_names[i]
        else
            null;
        if (name) |nm| {
            for (params, 0..) |p, pi| {
                if (std.mem.eql(u8, p.name, nm)) {
                    out[pi] = args[i];
                    break;
                }
            }
        } else {
            while (next_pos < n and out[next_pos] != null) : (next_pos += 1) {}
            if (next_pos >= n) return error.TooManyArgs;
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
            return error.TooManyArgs; // missing required
        }
    }
    return result;
}

fn collectCaptures(
    ctx: *Ctx,
    e: *ast.Expr,
    param_names: []const []const u8,
    out: *std.ArrayListUnmanaged([]const u8),
) void {
    const isParam = struct {
        fn check(names: []const []const u8, n: []const u8) bool {
            for (names) |p| if (std.mem.eql(u8, p, n)) return true;
            return false;
        }
    }.check;
    const already = struct {
        fn check(list: *std.ArrayListUnmanaged([]const u8), n: []const u8) bool {
            for (list.items) |x| if (std.mem.eql(u8, x, n)) return true;
            return false;
        }
    }.check;
    switch (e.*) {
        .ident => |n| {
            if (isParam(param_names, n)) return;
            if (already(out, n)) return;
            if (ctx.slots.contains(n)) {
                out.append(ctx.a, n) catch {};
            }
        },
        .binary => |b| {
            collectCaptures(ctx, b.left, param_names, out);
            collectCaptures(ctx, b.right, param_names, out);
        },
        .unary => |u| collectCaptures(ctx, u.expr, param_names, out),
        .call => |c| {
            collectCaptures(ctx, c.callee, param_names, out);
            for (c.args) |a| collectCaptures(ctx, a, param_names, out);
            for (c.with_args) |w| collectCaptures(ctx, w, param_names, out);
        },
        .field => |f| collectCaptures(ctx, f.base, param_names, out),
        .index => |ix| {
            collectCaptures(ctx, ix.base, param_names, out);
            collectCaptures(ctx, ix.index, param_names, out);
        },
        .slice => |sl| {
            collectCaptures(ctx, sl.base, param_names, out);
            if (sl.start) |s| collectCaptures(ctx, s, param_names, out);
            if (sl.end) |en| collectCaptures(ctx, en, param_names, out);
        },
        .cast, .try_cast => |c| collectCaptures(ctx, c.expr, param_names, out),
        .force_unwrap => |inner| collectCaptures(ctx, inner, param_names, out),
        .go_block => |gb| {
            for (gb.with_args) |w| collectCaptures(ctx, w, param_names, out);
            collectCapturesBlock(ctx, gb.body, param_names, out);
        },
        .go_expr => |ge| collectCaptures(ctx, ge, param_names, out),
        .scope_block => |sb| collectCapturesBlock(ctx, sb.body, param_names, out),
        .select_block => |sb| collectCapturesBlock(ctx, sb.body, param_names, out),
        .type_value => {},
        .lambda => |lam| {
            // Nested lambda: only scan free vars not bound by the inner params.
            var nested_params: std.ArrayListUnmanaged([]const u8) = .{};
            defer nested_params.deinit(ctx.a);
            nested_params.appendSlice(ctx.a, param_names) catch {};
            for (lam.params) |p| nested_params.append(ctx.a, p.name) catch {};
            if (lam.expr_body) |eb| collectCaptures(ctx, eb, nested_params.items, out);
            if (lam.block) |b| collectCapturesBlock(ctx, b, nested_params.items, out);
        },
        .when_expr => |w| {
            collectCaptures(ctx, w.scrutinee, param_names, out);
            for (w.arms) |arm| collectCaptures(ctx, arm.body, param_names, out);
            if (w.else_body) |eb| collectCaptures(ctx, eb, param_names, out);
        },
        .tuple => |elems| for (elems) |el| collectCaptures(ctx, el, param_names, out),
        .array_lit => |al| for (al.elems) |el| collectCaptures(ctx, el, param_names, out),
        .struct_lit => |sl| for (sl.fields) |f| collectCaptures(ctx, f.value, param_names, out),
        .range => |r| {
            collectCaptures(ctx, r.start, param_names, out);
            collectCaptures(ctx, r.end, param_names, out);
        },
        .interp => |ip| {
            for (ip.parts) |p| switch (p) {
                .text => {},
                .expr => |ex| collectCaptures(ctx, ex, param_names, out),
            };
        },
        .optional_call => |oc| {
            collectCaptures(ctx, oc.base, param_names, out);
            for (oc.args) |a| collectCaptures(ctx, a, param_names, out);
        },
        .panic_call => |p| collectCaptures(ctx, p.message, param_names, out),
        .is_type => |it| collectCaptures(ctx, it.expr, param_names, out),
        else => {},
    }
}

fn collectCapturesStmt(
    ctx: *Ctx,
    stmt: ast.Stmt,
    param_names: []const []const u8,
    out: *std.ArrayListUnmanaged([]const u8),
) void {
    switch (stmt) {
        .var_decl => |vd| if (vd.init) |init| collectCaptures(ctx, init, param_names, out),
        .destructure => |ds| collectCaptures(ctx, ds.init, param_names, out),
        .assign => |as| {
            collectCaptures(ctx, as.value, param_names, out);
            // Assigned name may be a capture (`sum += …`).
            const isParam = struct {
                fn check(names: []const []const u8, n: []const u8) bool {
                    for (names) |p| if (std.mem.eql(u8, p, n)) return true;
                    return false;
                }
            }.check;
            const already = struct {
                fn check(list: *std.ArrayListUnmanaged([]const u8), n: []const u8) bool {
                    for (list.items) |x| if (std.mem.eql(u8, x, n)) return true;
                    return false;
                }
            }.check;
            if (as.field == null and as.index == null and !isParam(param_names, as.name) and !already(out, as.name)) {
                if (ctx.slots.contains(as.name)) out.append(ctx.a, as.name) catch {};
            }
            if (as.field) |fa| collectCaptures(ctx, fa.base, param_names, out);
            if (as.index) |ix| {
                collectCaptures(ctx, ix.base, param_names, out);
                collectCaptures(ctx, ix.index, param_names, out);
            }
        },
        .expr => |e| collectCaptures(ctx, e, param_names, out),
        .return_stmt => |re| if (re) |e| collectCaptures(ctx, e, param_names, out),
        .if_stmt => |ifs| {
            collectCaptures(ctx, ifs.cond, param_names, out);
            collectCapturesBlock(ctx, ifs.then_block, param_names, out);
            if (ifs.else_block) |eb| collectCapturesBlock(ctx, eb, param_names, out);
        },
        .while_stmt => |ws| {
            collectCaptures(ctx, ws.cond, param_names, out);
            collectCapturesBlock(ctx, ws.body, param_names, out);
        },
        .for_in_stmt => |fs| {
            collectCaptures(ctx, fs.iter, param_names, out);
            collectCapturesBlock(ctx, fs.body, param_names, out);
        },
        .with_stmt => |ws| {
            for (ws.values) |v| collectCaptures(ctx, v, param_names, out);
            collectCapturesBlock(ctx, ws.body, param_names, out);
        },
        .defer_stmt, .errdefer_stmt => |ds| switch (ds) {
            .expr => |ex| collectCaptures(ctx, ex, param_names, out),
            .block => |b| collectCapturesBlock(ctx, b, param_names, out),
        },
        else => {},
    }
}

fn collectCapturesBlock(
    ctx: *Ctx,
    block: *ast.Block,
    param_names: []const []const u8,
    out: *std.ArrayListUnmanaged([]const u8),
) void {
    for (block.stmts) |s| collectCapturesStmt(ctx, s, param_names, out);
}

fn lambdaBodyBlock(a: std.mem.Allocator, lam: ast.Lambda) LowerError!*ast.Block {
    if (lam.expr_body) |eb| {
        const stmts = try a.alloc(ast.Stmt, 1);
        stmts[0] = .{ .return_stmt = eb };
        const b = try a.create(ast.Block);
        b.* = .{ .stmts = stmts };
        return b;
    }
    const block = lam.block orelse {
        const b = try a.create(ast.Block);
        b.* = .{ .stmts = &.{} };
        return b;
    };
    // Last expression statement becomes the return value.
    if (block.stmts.len > 0) {
        const last = block.stmts[block.stmts.len - 1];
        if (last == .expr) {
            var stmts = try a.alloc(ast.Stmt, block.stmts.len);
            @memcpy(stmts[0 .. block.stmts.len - 1], block.stmts[0 .. block.stmts.len - 1]);
            stmts[block.stmts.len - 1] = .{ .return_stmt = last.expr };
            const b = try a.create(ast.Block);
            b.* = .{ .stmts = stmts };
            return b;
        }
    }
    return block;
}

fn lowerLambda(ctx: *Ctx, lam: ast.Lambda) LowerError!u32 {
    var param_names: std.ArrayListUnmanaged([]const u8) = .{};
    for (lam.params) |p| try param_names.append(ctx.a, p.name);

    var captures: std.ArrayListUnmanaged([]const u8) = .{};
    if (lam.expr_body) |eb| collectCaptures(ctx, eb, param_names.items, &captures);
    if (lam.block) |b| collectCapturesBlock(ctx, b, param_names.items, &captures);

    const seq = ctx.lambda_seq.*;
    ctx.lambda_seq.* += 1;
    const name = try std.fmt.allocPrint(ctx.a, "jog_lambda_{d}", .{seq});

    const body = try lambdaBodyBlock(ctx.a, lam);
    const has_captures = captures.items.len > 0;

    // Synthetic params: optional env (*byte), then lambda params.
    var synth_params: std.ArrayListUnmanaged(ast.Param) = .{};
    if (has_captures) {
        const env_ty = try ctx.a.create(ast.TypeExpr);
        env_ty.* = .{ .name = "byte" }; // lowered as raw pointer slot
        const env_ptr = try ctx.a.create(ast.TypeExpr);
        env_ptr.* = .{ .pointer = env_ty };
        try synth_params.append(ctx.a, .{ .name = "__env", .ty = env_ptr });
    }
    for (lam.params) |p| try synth_params.append(ctx.a, p);

    const owned_params = try synth_params.toOwnedSlice(ctx.a);
    try internCallee(ctx.a, ctx.callees, ctx.callee_map, .{ .kind = .jog_fn, .name = name });
    try ctx.fn_infos.put(ctx.a, name, .{
        .mangled = name,
        .nparams = @intCast(owned_params.len),
        .contexts = &.{},
        .exported = false,
        .params = owned_params,
    });

    const f = try lowerLambdaFn(ctx, name, if (has_captures) captures.items else &.{}, lam.params, body);
    try ctx.pending_funcs.append(ctx.a, f);

    if (!has_captures) {
        return try lowerFnValuePair(ctx, name, none);
    }

    // Build env array of pointers to captured locals on the parent stack.
    const ncap = captures.items.len;
    const first = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = first, .aux = 0 });
    var ci: usize = 1;
    while (ci < ncap) : (ci += 1) {
        const s = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = s, .aux = 0 });
    }
    const env_ptr = try ctx.fresh();
    try ctx.emit(.{ .op = .lea_slot, .dst = env_ptr, .src0 = first });
    for (captures.items, 0..) |cname, i| {
        const parent_slot = ctx.slots.get(cname) orelse return error.UnknownName;
        // Address of parent local.
        const addr = try ctx.fresh();
        if (ctx.meta.get(cname)) |m| {
            // Already a pointer-ish local (struct/array): capture that pointer.
            if (m.kind == .struct_ptr or m.kind == .array_ptr or m.kind == .iface_ptr or m.kind == .capture_ptr) {
                try ctx.emit(.{ .op = .copy, .dst = addr, .src0 = parent_slot });
            } else {
                try ctx.emit(.{ .op = .lea_slot, .dst = addr, .src0 = parent_slot });
            }
        } else {
            try ctx.emit(.{ .op = .lea_slot, .dst = addr, .src0 = parent_slot });
        }
        const slot_addr = try ctx.fresh();
        const off = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = off, .aux = @intCast(i * 8) });
        // Stack slots grow downward from lea_slot — index with sub.
        try ctx.emit(.{ .op = .sub, .dst = slot_addr, .src0 = env_ptr, .src1 = off });
        try ctx.emit(.{ .op = .store, .src0 = slot_addr, .src1 = addr });
    }
    return try lowerFnValuePair(ctx, name, env_ptr);
}

fn lowerLambdaFn(
    parent_ctx: *Ctx,
    name: []const u8,
    captures: []const []const u8,
    lam_params: []ast.Param,
    body: *ast.Block,
) LowerError!Func {
    var slots: std.StringHashMapUnmanaged(u32) = .{};
    var meta: std.StringHashMapUnmanaged(SlotMeta) = .{};
    var next_slot: u32 = 0;
    var ambient: std.ArrayListUnmanaged(Ambient) = .{};
    var insts: std.ArrayListUnmanaged(Inst) = .{};
    var break_jmps: std.ArrayListUnmanaged(usize) = .{};
    var continue_jmps: std.ArrayListUnmanaged(usize) = .{};
    var defer_stack: std.ArrayListUnmanaged(std.ArrayListUnmanaged(DeferItem)) = .{};
    var empty_subst: std.StringHashMapUnmanaged([]const u8) = .{};

    // Bind params (env first if present).
    if (captures.len > 0) {
        const env_slot = next_slot;
        next_slot += 1;
        try slots.put(parent_ctx.a, "__env", env_slot);
        try meta.put(parent_ctx.a, "__env", .{ .kind = .raw_ptr, .type_name = "byte", .elem_size = 1 });
    }
    for (lam_params) |p| {
        const slot = next_slot;
        next_slot += 1;
        try slots.put(parent_ctx.a, p.name, slot);
        try meta.put(parent_ctx.a, p.name, try metaForTypeExpr(parent_ctx.a, p.ty, parent_ctx.structs, parent_ctx.ifaces));
    }
    const nparams = next_slot;

    var loop_defer_depths: std.ArrayListUnmanaged(usize) = .{};
    var ctx = Ctx{
        .a = parent_ctx.a,
        .strings = parent_ctx.strings,
        .callees = parent_ctx.callees,
        .callee_map = parent_ctx.callee_map,
        .structs = parent_ctx.structs,
        .enums = parent_ctx.enums,
        .methods = parent_ctx.methods,
        .ifaces = parent_ctx.ifaces,
        .fn_infos = parent_ctx.fn_infos,
        .vtables = parent_ctx.vtables,
        .vtable_map = parent_ctx.vtable_map,
        .type_infos = parent_ctx.type_infos,
        .type_info_map = parent_ctx.type_info_map,
        .const_vals = parent_ctx.const_vals,
        .addr_consts = parent_ctx.addr_consts,
        .global_map = parent_ctx.global_map,
        .globals = parent_ctx.globals,
        .object_map = parent_ctx.object_map,
        .import_binds = parent_ctx.import_binds,
        .slots = &slots,
        .meta = &meta,
        .next_slot = &next_slot,
        .insts = &insts,
        .ambient = &ambient,
        .break_jmps = &break_jmps,
        .continue_jmps = &continue_jmps,
        .defer_stack = &defer_stack,
        .loop_defer_depths = &loop_defer_depths,
        .ret_iface = null,
        .type_subst = &empty_subst,
        .pending_gos = parent_ctx.pending_gos,
        .go_seq = parent_ctx.go_seq,
        .type_aliases = parent_ctx.type_aliases,
        .pending_funcs = parent_ctx.pending_funcs,
        .lambda_seq = parent_ctx.lambda_seq,
    };

    // Prologue: bind each capture as capture_ptr loaded from env[i].
    if (captures.len > 0) {
        const env_slot = slots.get("__env").?;
        for (captures, 0..) |cname, i| {
            const off = try ctx.fresh();
            try ctx.emit(.{ .op = .iconst, .dst = off, .aux = @intCast(i * 8) });
            const addr = try ctx.fresh();
            // Env buffer lives on the caller's stack (downward slots).
            try ctx.emit(.{ .op = .sub, .dst = addr, .src0 = env_slot, .src1 = off });
            const ptr = try ctx.fresh();
            try ctx.emit(.{ .op = .load, .dst = ptr, .src0 = addr });
            try slots.put(parent_ctx.a, cname, ptr);
            try meta.put(parent_ctx.a, cname, .{ .kind = .capture_ptr, .type_name = "i64" });
        }
    }

    try lowerBlock(&ctx, body);
    if (insts.items.len == 0 or insts.items[insts.items.len - 1].op != .ret) {
        try emitAllDefers(&ctx);
        try ctx.emit(.{ .op = .ret, .src0 = none });
    }
    return .{
        .name = name,
        .nparams = nparams,
        .nlocals = next_slot,
        .exported = false,
        .insts = try insts.toOwnedSlice(parent_ctx.a),
    };
}

/// Build a stack pair `{code, env}` and return a pointer to it (function value).
/// `env_slot == none` stores a null env (bare / no-capture).
fn lowerFnValuePair(ctx: *Ctx, fn_name: []const u8, env_slot: u32) LowerError!u32 {
    const cid = ctx.callee_map.get(fn_name) orelse return error.UnknownName;
    const code = try ctx.fresh();
    try ctx.emit(.{ .op = .saddr, .dst = code, .aux = -1 - @as(i64, @intCast(cid)) });
    const env = try ctx.fresh();
    if (env_slot == none) {
        try ctx.emit(.{ .op = .iconst, .dst = env, .aux = 0 });
    } else {
        try ctx.emit(.{ .op = .copy, .dst = env, .src0 = env_slot });
    }
    const ptr = try ctx.fresh();
    try ctx.emit(.{ .op = .lea_slot, .dst = ptr, .src0 = code });
    return ptr;
}

fn lowerFnPtrCall(ctx: *Ctx, fnptr: u32, args: []*ast.Expr, force_closure: bool) LowerError!u32 {
    _ = force_closure;
    // Function values are pointers to `{code, env}` on the caller's stack.
    // Slots grow downward: env is at fnptr - 8.
    const code = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = code, .src0 = fnptr });
    const env_addr = try ctx.fresh();
    const eight = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });
    try ctx.emit(.{ .op = .sub, .dst = env_addr, .src0 = fnptr, .src1 = eight });
    const env = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = env, .src0 = env_addr });

    var user: [16]u32 = .{none} ** 16;
    if (args.len + 1 > max_call_args) return error.TooManyArgs;
    for (args, 0..) |arg, i| {
        user[i] = try lowerExpr(ctx, arg);
    }

    const d = try ctx.fresh();
    // if env == 0 → bare; else closure (env first).
    const jz_i = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = env, .aux = 0 });

    var carg: [16]u32 = .{none} ** 16;
    carg[0] = env;
    var carity: u8 = 1;
    for (args, 0..) |_, i| {
        carg[carity] = user[i];
        carity += 1;
    }
    const d_cl = try ctx.fresh();
    try ctx.emit(.{ .op = .icall, .dst = d_cl, .src0 = code, .arity = carity, .args = carg });
    try ctx.emit(.{ .op = .copy, .dst = d, .src0 = d_cl });
    const jmp_i = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jmp, .aux = 0 });

    const bare_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_i].aux = bare_pos;
    var barg: [16]u32 = .{none} ** 16;
    var barity: u8 = 0;
    for (args, 0..) |_, i| {
        barg[barity] = user[i];
        barity += 1;
    }
    const d_b = try ctx.fresh();
    try ctx.emit(.{ .op = .icall, .dst = d_b, .src0 = code, .arity = barity, .args = barg });
    try ctx.emit(.{ .op = .copy, .dst = d, .src0 = d_b });
    const end_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jmp_i].aux = end_pos;
    return d;
}

fn lowerFnCall(ctx: *Ctx, fi: FnInfo, args: []*ast.Expr, with_args: []*ast.Expr, arg_names: []const ?[]const u8) LowerError!u32 {
    var arg_slots: [16]u32 = .{none} ** 16;
    var arity: u8 = 0;

    const expanded = if (fi.params.len > 0)
        try expandArgsForLower(ctx.a, fi.params, args, arg_names)
    else
        args;

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
        if (expanded.len < fixed) return error.TooManyArgs;
        if (fixed + arity > max_call_args) return error.TooManyArgs;
        var i: usize = 0;
        while (i < fixed) : (i += 1) {
            const raw = try lowerExpr(ctx, expanded[i]);
            arg_slots[arity] = try maybeCoerceArg(ctx, raw, expanded[i], if (i < fi.params.len) fi.params[i].ty else null);
            arity += 1;
        }
        const pack_n = expanded.len - fixed;
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
            const val = try lowerExpr(ctx, expanded[fixed + k]);
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
        if (expanded.len + arity > max_call_args) return error.TooManyArgs;
        for (expanded, 0..) |arg, ai| {
            const raw = try lowerExpr(ctx, arg);
            arg_slots[arity] = try maybeCoerceArg(ctx, raw, arg, if (ai < fi.params.len) fi.params[ai].ty else null);
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

/// Concrete type name for iface coercion. `&x` → type of `x`; otherwise expression meta.
fn concreteTypeForIfaceCoerce(ctx: *Ctx, e: *ast.Expr) []const u8 {
    if (e.* == .unary and e.unary.op == .addr) {
        return inferTypeNameFromExpr(ctx, e.unary.expr) orelse "";
    }
    return inferTypeNameFromExpr(ctx, e) orelse "";
}

/// Coerce a call/assign argument to an iface parameter/local when needed.
fn maybeCoerceArg(ctx: *Ctx, slot: u32, arg: *ast.Expr, want_ty: ?*ast.TypeExpr) LowerError!u32 {
    const ty = want_ty orelse return slot;
    const want_meta = metaForTypeExpr(ctx.a, ty, ctx.structs, ctx.ifaces) catch return slot;
    if (want_meta.kind != .iface_ptr or want_meta.type_name.len == 0) return slot;
    // Already an iface value of the right type — pass through.
    if (inferMetaFromExpr(ctx, arg)) |am| {
        if (am.kind == .iface_ptr and std.mem.eql(u8, am.type_name, want_meta.type_name))
            return slot;
    }
    const concrete = concreteTypeForIfaceCoerce(ctx, arg);
    if (concrete.len == 0 or std.mem.eql(u8, concrete, want_meta.type_name)) return slot;
    return try coerceToIface(ctx, slot, concrete, want_meta.type_name);
}


fn ensureTypeInfo(ctx: *Ctx, type_name: []const u8) LowerError!u32 {
    const leaf = typeNameLeaf(resolveTypeSubst(ctx, type_name));
    if (ctx.type_info_map.get(leaf)) |i| return i;
    const kind: TypeInfoKind = blk: {
        if (ctx.structs.contains(leaf)) break :blk .struct_ty;
        if (ctx.enums.contains(leaf)) break :blk .enum_ty;
        if (ctx.ifaces.contains(leaf)) break :blk .iface_ty;
        break :blk .struct_ty;
    };
    const sz = layoutSizeOf(ctx, leaf);
    const idx: u32 = @intCast(ctx.type_infos.items.len);
    try ctx.type_infos.append(ctx.a, .{
        .name = try ctx.a.dupe(u8, leaf),
        .kind = kind,
        .size = sz,
    });
    try ctx.type_info_map.put(ctx.a, leaf, idx);
    return idx;
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
        const ti = try ensureTypeInfo(ctx, concrete);
        const idx: u32 = @intCast(ctx.vtables.items.len);
        try ctx.vtables.append(ctx.a, .{
            .symbol = try std.fmt.allocPrint(ctx.a, "jog_vt_{d}", .{idx}),
            .type_info_index = ti,
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
    const resolved: PromotedMethod = blk: {
        const key = try std.fmt.allocPrint(ctx.a, "{s}.{s}", .{ recv_leaf, method });
        if (ctx.methods.get(key)) |mi| break :blk .{ .mi = mi, .embed_off = 0 };
        if (std.mem.eql(u8, recv_leaf, "str")) {
            const alt = try std.fmt.allocPrint(ctx.a, "bytes.{s}", .{method});
            if (ctx.methods.get(alt)) |mi| break :blk .{ .mi = mi, .embed_off = 0 };
        }
        // Promoted methods through embedded structs (Structs.md).
        if (findPromotedMethod(ctx, recv_leaf, method)) |pm| break :blk pm;
        return error.UnknownName;
    };
    const mi = resolved.mi;
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
    var self_slot = if (mi.pointer_recv)
        try lowerPointerReceiver(ctx, base_expr, base_slot)
    else
        base_slot;
    if (resolved.embed_off != 0) {
        const adj = try ctx.fresh();
        const off_s = try ctx.fresh();
        try ctx.emit(.{ .op = .iconst, .dst = off_s, .aux = resolved.embed_off });
        try ctx.emit(.{ .op = .add, .dst = adj, .src0 = self_slot, .src1 = off_s });
        self_slot = adj;
    }
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
    // Vtable word 0 is type_id (*TypeInfo); methods start at word 1.
    try ctx.emit(.{ .op = .iconst, .dst = off, .aux = (method_index + 1) * 8 });
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

fn isStrLikeName(name: []const u8) bool {
    return std.mem.eql(u8, name, "str") or
        std.mem.eql(u8, name, "bytes") or
        std.mem.eql(u8, name, "__Slice_byte");
}

fn isStrLikeExpr(ctx: *Ctx, e: *ast.Expr) bool {
    if (e.* == .string) return true;
    if (inferMetaFromExpr(ctx, e)) |m| {
        if (m.kind == .struct_ptr and isStrLikeName(m.type_name)) return true;
    }
    if (inferTypeNameFromExpr(ctx, e)) |tn| {
        if (isStrLikeName(tn)) return true;
    }
    return false;
}

/// Content equality for `{ptr,len}` string/bytes headers. `neq` inverts the result.
fn lowerStrContentEq(ctx: *Ctx, left: *ast.Expr, right: *ast.Expr, invert: bool) LowerError!u32 {
    const lh = try lowerExpr(ctx, left);
    const rh = try lowerExpr(ctx, right);
    const eight = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = eight, .aux = 8 });

    const lp = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = lp, .src0 = lh });
    const lplen = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = lplen, .src0 = lh, .src1 = eight });
    const llen = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = llen, .src0 = lplen });

    const rp = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = rp, .src0 = rh });
    const rplen = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = rplen, .src0 = rh, .src1 = eight });
    const rlen = try ctx.fresh();
    try ctx.emit(.{ .op = .load, .dst = rlen, .src0 = rplen });

    const result = try ctx.fresh();
    // Default: equal (or unequal when invert) — empty equal-length strings win immediately.
    try ctx.emit(.{ .op = .iconst, .dst = result, .aux = if (invert) 0 else 1 });

    const len_eq = try ctx.fresh();
    try ctx.emit(.{ .op = .eq, .dst = len_eq, .src0 = llen, .src1 = rlen });
    const jz_len = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = len_eq, .aux = 0 });

    // Byte loop: for i in 0..len
    const i = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = i, .aux = 0 });
    const head: i64 = @intCast(ctx.insts.items.len);
    const more = try ctx.fresh();
    try ctx.emit(.{ .op = .lt, .dst = more, .src0 = i, .src1 = llen });
    const jz_done = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = more, .aux = 0 });

    const ap = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = ap, .src0 = lp, .src1 = i });
    const av = try ctx.fresh();
    try ctx.emit(.{ .op = .load8, .dst = av, .src0 = ap });
    const bp = try ctx.fresh();
    try ctx.emit(.{ .op = .add, .dst = bp, .src0 = rp, .src1 = i });
    const bv = try ctx.fresh();
    try ctx.emit(.{ .op = .load8, .dst = bv, .src0 = bp });
    const beq = try ctx.fresh();
    try ctx.emit(.{ .op = .eq, .dst = beq, .src0 = av, .src1 = bv });
    const jz_diff = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jz, .src0 = beq, .aux = 0 });
    const one = try ctx.fresh();
    try ctx.emit(.{ .op = .iconst, .dst = one, .aux = 1 });
    try ctx.emit(.{ .op = .add, .dst = i, .src0 = i, .src1 = one });
    try ctx.emit(.{ .op = .jmp, .aux = head });

    const diff_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_diff].aux = diff_pos;
    try ctx.emit(.{ .op = .iconst, .dst = result, .aux = if (invert) 1 else 0 });
    const jmp_end1 = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jmp, .aux = 0 });

    const done_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_done].aux = done_pos;
    // lens matched and all bytes matched — result already set to eq/!eq default
    const jmp_end2 = ctx.insts.items.len;
    try ctx.emit(.{ .op = .jmp, .aux = 0 });

    const len_ne_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jz_len].aux = len_ne_pos;
    try ctx.emit(.{ .op = .iconst, .dst = result, .aux = if (invert) 1 else 0 });

    const end_pos: i64 = @intCast(ctx.insts.items.len);
    ctx.insts.items[jmp_end1].aux = end_pos;
    ctx.insts.items[jmp_end2].aux = end_pos;
    return result;
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
