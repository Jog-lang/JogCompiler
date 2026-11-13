const std = @import("std");

pub const TypeExpr = union(enum) {
    name: []const u8,
    pointer: *TypeExpr,
    pointer_const: *TypeExpr,
    optional: *TypeExpr,
    /// Fixed array `[N]T` (elements stored as 8-byte slots for now).
    array: ArrayType,
    iface_fn: *IfaceFnType,
    bad,
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
};

pub const ImportDecl = struct {
    path: []const u8,
    alias: ?[]const u8 = null,
};

pub const EnumDecl = struct {
    name: []const u8,
    variants: []EnumVariant,
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
    fields: []Field,
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
    params: []Param,
    contexts: []ContextParam,
    ret: ?*TypeExpr,
    body: ?*Block,
    /// C-callable export (SysV). Jog callers still use the Jog path; export adds .globl for C.
    exported: bool = false,
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
    assign: Assign,
    expr: *Expr,
    return_stmt: ?*Expr,
    if_stmt: IfStmt,
    while_stmt: WhileStmt,
    break_stmt,
    continue_stmt,
    with_stmt: WithStmt,
    defer_stmt: DeferStmt,
};

pub const DeferStmt = union(enum) {
    expr: *Expr,
    block: *Block,
};

pub const WhileStmt = struct {
    cond: *Expr,
    body: *Block,
};

pub const VarDecl = struct {
    mutable: bool,
    name: []const u8,
    ty: ?*TypeExpr,
    init: ?*Expr,
};

pub const Assign = struct {
    /// Simple `name = value`, field store (`base.name = value`), or index store (`base[i] = value`).
    name: []const u8,
    value: *Expr,
    field: ?FieldAssign = null,
    index: ?IndexAssign = null,
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
    string: []const u8,
    null_lit,
    binary: Binary,
    unary: Unary,
    call: Call,
    field: FieldAccess,
    index: IndexAccess,
    cast: Cast,
    optional_call: OptionalCall,
    struct_lit: StructLit,
    panic_call: PanicCall,
    when_expr: WhenExpr,
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
    fields: []StructLitField,
};

pub const StructLitField = struct {
    name: []const u8,
    value: *Expr,
};

pub const File = struct {
    decls: []Decl,
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *File) void {
        self.arena.deinit();
    }
};
