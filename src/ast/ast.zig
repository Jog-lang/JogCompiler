const std = @import("std");

pub const TypeExpr = union(enum) {
    name: []const u8,
    /// Applied generics: `Box<i64>` / `id` type args live on Call separately.
    named_app: NamedApp,
    pointer: *TypeExpr,
    pointer_const: *TypeExpr,
    optional: *TypeExpr,
    /// Fixed array `[N]T` (elements stored as 8-byte slots for now).
    array: ArrayType,
    /// View `[]T` / `[]const T` → `{ ptr, len }` (bytes-first; not jogc slice).
    slice_view: SliceView,
    /// Product type `(T, U, …)` — lowered as synthetic `__TupN` struct.
    tuple: []*TypeExpr,
    iface_fn: *IfaceFnType,
    bad,
};

pub const SliceView = struct {
    elem: *TypeExpr,
    is_const: bool,
};

pub const NamedApp = struct {
    name: []const u8,
    args: []*TypeExpr,
};

pub const ArrayType = struct {
    len: u64,
    elem: *TypeExpr,
};

pub const IfaceFnType = struct {
    params: []Param,
    ret: ?*TypeExpr,
};

pub const Param = struct {
    name: []const u8,
    ty: *TypeExpr,
};

pub const ContextParam = struct {
    name: ?[]const u8,
    ty: *TypeExpr,
};

pub const Decl = union(enum) {
    type_alias: TypeAlias,
    struct_decl: StructDecl,
    enum_decl: EnumDecl,
    interface_fn: InterfaceFn,
    interface_struct: InterfaceStruct,
    fn_decl: FnDecl,
    method: MethodDecl,
    extern_block: ExternBlock,
    expect_fn: ExternFn,
    actual_fn: FnDecl,
    import_decl: ImportDecl,
    require_decl: RequireDecl,
    /// Package-level `val` / `var` (compile-time const or mutable global).
    global_decl: GlobalDecl,
};

pub const GlobalDecl = struct {
    mutable: bool,
    name: []const u8,
    ty: ?*TypeExpr,
    init: ?*Expr,
    line: u32 = 1,
    col: u32 = 1,
};

pub const ImportDecl = struct {
    path: []const u8,
    alias: ?[]const u8 = null,
};

pub const RequireDecl = struct {
    /// Module name as in `require foo` / `require "path"`.
    name: []const u8,
};

pub const EnumDecl = struct {
    name: []const u8,
    variants: []EnumVariant,
    line: u32 = 1,
    col: u32 = 1,
};

pub const EnumVariant = struct {
    name: []const u8,
    value: ?i64 = null,
};

pub const ExternBlock = struct {
    /// C ABI SysV (linux x64) for now.
    fns: []ExternFn,
    /// From `@link("name")` — host `-lname` at link time.
    link_lib: ?[]const u8 = null,
};

pub const ExternFn = struct {
    name: []const u8,
    params: []Param,
    ret: ?*TypeExpr,
};

pub const TypeAlias = struct {
    name: []const u8,
    ty: *TypeExpr,
};

pub const Field = struct {
    name: []const u8,
    ty: *TypeExpr,
};

pub const StructDecl = struct {
    name: []const u8,
    line: u32 = 1,
    col: u32 = 1,
    /// Generic params e.g. `struct Box<T>`.
    type_params: [][]const u8 = &.{},
    fields: []Field,
    /// `@packed` — request dense layout (byte-packed when fields are byte-sized).
    packed_attr: bool = false,
    /// `object Name { … }` — struct body + one static singleton (JOG-96).
    is_object: bool = false,
};

pub const InterfaceFn = struct {
    name: []const u8,
    params: []Param,
    ret: ?*TypeExpr,
};

pub const InterfaceStruct = struct {
    name: []const u8,
    methods: []MethodSig,
};

pub const MethodSig = struct {
    pointer_recv: bool,
    name: []const u8,
    params: []Param,
    contexts: []ContextParam,
    ret: ?*TypeExpr,
};

pub const FnDecl = struct {
    name: []const u8,
    /// Generic params e.g. `fn id<T>(…)`.
    type_params: [][]const u8 = &.{},
    /// Optional constraints parallel to type_params (`T: Iface`).
    type_constraints: []?[]const u8 = &.{},
    params: []Param,
    contexts: []ContextParam,
    ret: ?*TypeExpr,
    body: ?*Block,
    /// C-callable export (SysV). Jog callers still use the Jog path; export adds .globl for C.
    exported: bool = false,
    /// `@test` — test entry; not linked as main.
    is_test: bool = false,
    /// `private` — file-local (enforced when multi-file).
    is_private: bool = false,
    /// Name token location (for `jog symbols` / LSP).
    line: u32 = 1,
    col: u32 = 1,
};

pub const MethodDecl = struct {
    pointer_recv: bool,
    type_name: []const u8,
    name: []const u8,
    params: []Param,
    contexts: []ContextParam,
    ret: ?*TypeExpr,
    body: ?*Block,
};

pub const Block = struct {
    stmts: []Stmt,
};

pub const Stmt = union(enum) {
    var_decl: VarDecl,
    destructure: Destructure,
    assign: Assign,
    expr: *Expr,
    return_stmt: ?*Expr,
    if_stmt: IfStmt,
    while_stmt: WhileStmt,
    for_in_stmt: ForInStmt,
    break_stmt,
    continue_stmt,
    with_stmt: WithStmt,
    defer_stmt: DeferStmt,
    errdefer_stmt: DeferStmt,
};

pub const DeferStmt = union(enum) {
    expr: *Expr,
    block: *Block,
};

pub const WhileStmt = struct {
    cond: *Expr,
    body: *Block,
};

/// `for x in iter { }` — iter is range `a..b` or array.
pub const ForInStmt = struct {
    name: []const u8,
    iter: *Expr,
    body: *Block,
};

pub const VarDecl = struct {
    mutable: bool,
    name: []const u8,
    ty: ?*TypeExpr,
    init: ?*Expr,
    line: u32 = 1,
    col: u32 = 1,
};

/// `val (a, b) = expr` — bind tuple fields by index.
pub const Destructure = struct {
    mutable: bool,
    names: [][]const u8,
    init: *Expr,
};

pub const Assign = struct {
    /// Simple `name = value`, field store (`base.name = value`), or index store (`base[i] = value`).
    name: []const u8,
    value: *Expr,
    field: ?FieldAssign = null,
    index: ?IndexAssign = null,
    /// Compound `+=` etc.; null means plain `=`.
    compound: ?BinaryOp = null,
};

pub const FieldAssign = struct {
    base: *Expr,
    name: []const u8,
};

pub const IndexAssign = struct {
    base: *Expr,
    index: *Expr,
};

pub const IfStmt = struct {
    cond: *Expr,
    then_block: *Block,
    else_block: ?*Block,
};

pub const WithStmt = struct {
    contexts: []ContextParam,
    values: []*Expr,
    body: *Block,
};

pub const Expr = union(enum) {
    ident: []const u8,
    int: i64,
    float: f64,
    string: []const u8,
    /// `c"…"` — string bytes with trailing NUL for FFI.
    cstring: []const u8,
    null_lit,
    binary: Binary,
    unary: Unary,
    call: Call,
    field: FieldAccess,
    index: IndexAccess,
    cast: Cast,
    /// `e as? T` — optional cast (null on failure).
    try_cast: Cast,
    /// `e is T` — type test → bool.
    is_type: IsType,
    optional_call: OptionalCall,
    struct_lit: StructLit,
    panic_call: PanicCall,
    when_expr: WhenExpr,
    /// `(a, b, …)` product value.
    tuple: []*Expr,
    /// `a..b` half-open range (start inclusive, end exclusive).
    range: Range,
    /// `[a, b, c]` array literal (fixed len).
    array_lit: ArrayLit,
    /// `"a ${x} b"` interpolation pieces (string/expr alternating).
    interp: Interp,
};

pub const Range = struct {
    start: *Expr,
    end: *Expr,
};

pub const ArrayLit = struct {
    elems: []*Expr,
};

pub const Interp = struct {
    /// Alternating string literal chunks and expression values.
    parts: []InterpPart,
};

pub const InterpPart = union(enum) {
    text: []const u8,
    expr: *Expr,
};

pub const IsType = struct {
    expr: *Expr,
    ty: *TypeExpr,
};

pub const WhenExpr = struct {
    scrutinee: *Expr,
    arms: []WhenArm,
    else_body: ?*Expr,
};

pub const WhenArm = struct {
    pattern: []const u8,
    body: *Expr,
};

pub const IndexAccess = struct {
    base: *Expr,
    index: *Expr,
};

pub const PanicCall = struct {
    message: *Expr,
};

pub const Binary = struct {
    op: BinaryOp,
    left: *Expr,
    right: *Expr,
};

pub const BinaryOp = enum {
    eq,
    neq,
    add,
    sub,
    mul,
    div,
    rem,
    lt,
    gt,
    lte,
    gte,
    and_op,
    or_op,
    bit_and,
    bit_or,
    bit_xor,
    shl,
    shr,
    coalesce, // ?:
};

pub const Unary = struct {
    op: UnaryOp,
    expr: *Expr,
};

pub const UnaryOp = enum { neg, not, bit_not, addr, deref };

pub const Call = struct {
    callee: *Expr,
    args: []*Expr,
    /// Call-site contexts: `f(args) with (e1, e2)`.
    with_args: []*Expr = &.{},
    /// Explicit generic args: `id<i64>(x)`.
    type_args: []*TypeExpr = &.{},
};

pub const FieldAccess = struct {
    base: *Expr,
    name: []const u8,
};

pub const Cast = struct {
    expr: *Expr,
    ty: *TypeExpr,
};

pub const OptionalCall = struct {
    base: *Expr,
    name: []const u8,
    args: []*Expr,
};

pub const StructLit = struct {
    type_name: []const u8,
    /// `Box<i64>(…)` concrete args; empty for non-generic.
    type_args: []*TypeExpr = &.{},
    fields: []StructLitField,
};

pub const StructLitField = struct {
    name: []const u8,
    value: *Expr,
};

/// 1-based source location (LSP / `--diag=json` contract).
pub const Span = struct {
    line: u32 = 1,
    col: u32 = 1,
};

pub const File = struct {
    decls: []Decl,
    arena: std.heap.ArenaAllocator,
    /// Expression node → span. Key is `@intFromPtr(*Expr)`. Arena-backed.
    expr_spans: std.AutoHashMapUnmanaged(usize, Span) = .{},

    pub fn deinit(self: *File) void {
        self.arena.deinit();
    }

    pub fn spanOf(self: *const File, e: *const Expr) Span {
        return self.expr_spans.get(@intFromPtr(e)) orelse .{};
    }
};
