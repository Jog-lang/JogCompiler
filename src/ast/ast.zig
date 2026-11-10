const std = @import("std");

pub const TypeExpr = union(enum) {
    name: []const u8,
    pointer: *TypeExpr,
    pointer_const: *TypeExpr,
    optional: *TypeExpr,
    iface_fn: *IfaceFnType,
    bad,
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
    interface_fn: InterfaceFn,
    interface_struct: InterfaceStruct,
    fn_decl: FnDecl,
    method: MethodDecl,
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
    with_stmt: WithStmt,
};

pub const VarDecl = struct {
    mutable: bool,
    name: []const u8,
    ty: ?*TypeExpr,
    init: ?*Expr,
};

pub const Assign = struct {
    name: []const u8,
    value: *Expr,
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
    cast: Cast,
    optional_call: OptionalCall,
    struct_lit: StructLit,
};

pub const Binary = struct {
    op: BinaryOp,
    left: *Expr,
    right: *Expr,
};

pub const BinaryOp = enum { eq, neq, add, sub, lt, gt };

pub const Unary = struct {
    op: UnaryOp,
    expr: *Expr,
};

pub const UnaryOp = enum { neg, not, addr, deref };

pub const Call = struct {
    callee: *Expr,
    args: []*Expr,
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
