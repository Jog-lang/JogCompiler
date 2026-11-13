const std = @import("std");
const ast = @import("../ast/ast.zig");
const lex = @import("../lex/lex.zig");

pub const ParseError = error{
    UnexpectedToken,
    OutOfMemory,
    Overflow,
    InvalidCharacter,
};

pub const Parser = struct {
    allocator: std.mem.Allocator,
    lx: lex.Lexer,
    tok: lex.Token,

    pub fn parseFile(parent: std.mem.Allocator, src: []const u8) ParseError!ast.File {
        var file: ast.File = .{
            .decls = &[_]ast.Decl{},
            .arena = std.heap.ArenaAllocator.init(parent),
        };
        errdefer file.deinit();
        var p = Parser{
            .allocator = file.arena.allocator(),
            .lx = lex.Lexer.init(src),
            .tok = undefined,
        };
        p.tok = p.lx.next();
        var decls: std.ArrayListUnmanaged(ast.Decl) = .{};
        while (p.tok.kind != .eof) {
            const d = p.parseDecl() catch |err| {
                std.debug.print("parse error at {d}:{d} near '{s}' ({s}): {s}\n", .{
                    p.tok.line,
                    p.tok.col,
                    p.tok.text,
                    @tagName(p.tok.kind),
                    @errorName(err),
                });
                return err;
            };
            try decls.append(p.allocator, d);
        }
        file.decls = try decls.toOwnedSlice(p.allocator);
        return file;
    }

    fn advance(self: *Parser) void {
        self.tok = self.lx.next();
    }

    fn expect(self: *Parser, kind: lex.TokenKind) ParseError!void {
        if (self.tok.kind != kind) return error.UnexpectedToken;
        self.advance();
    }

    fn parseDecl(self: *Parser) ParseError!ast.Decl {
        switch (self.tok.kind) {
            .kw_type => return .{ .type_alias = try self.parseTypeAlias() },
            .kw_struct => return .{ .struct_decl = try self.parseStruct() },
            .kw_interface => return try self.parseInterface(),
            .kw_extern => return .{ .extern_block = try self.parseExternBlock() },
            .kw_expect => {
                self.advance();
                try self.expect(.kw_fn);
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                const params = try self.parseParamList();
                var ret: ?*ast.TypeExpr = null;
                if (self.tok.kind == .colon) {
                    self.advance();
                    ret = try self.parseTypeExpr();
                }
                return .{ .expect_fn = .{ .name = name, .params = params, .ret = ret } };
            },
            .kw_actual => {
                self.advance();
                return .{ .actual_fn = try self.parseFn() };
            },
            .kw_export => {
                self.advance();
                var fd = try self.parseFn();
                fd.exported = true;
                return .{ .fn_decl = fd };
            },
            .kw_fn => {
                // method: fn *Type.name or fn Type.name
                const save_i = self.lx.i;
                const save_line = self.lx.line;
                const save_col = self.lx.col;
                const save_tok = self.tok;
                self.advance(); // fn
                var pointer_recv = false;
                if (self.tok.kind == .star) {
                    pointer_recv = true;
                    self.advance();
                }
                if (self.tok.kind == .ident) {
                    const type_name = self.tok.text;
                    self.advance();
                    if (self.tok.kind == .dot) {
                        self.advance();
                        return .{ .method = try self.parseMethodRest(pointer_recv, type_name) };
                    }
                }
                // rewind to parse as plain fn
                self.lx.i = save_i;
                self.lx.line = save_line;
                self.lx.col = save_col;
                self.tok = save_tok;
                return .{ .fn_decl = try self.parseFn() };
            },
            else => return error.UnexpectedToken,
        }
    }

    fn parseExternBlock(self: *Parser) ParseError!ast.ExternBlock {
        try self.expect(.kw_extern);
        try self.expect(.lbrace);
        var fns: std.ArrayListUnmanaged(ast.ExternFn) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            try self.expect(.kw_fn);
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            const name = self.tok.text;
            self.advance();
            const params = try self.parseParamList();
            var ret: ?*ast.TypeExpr = null;
            if (self.tok.kind == .colon) {
                self.advance();
                ret = try self.parseTypeExpr();
            }
            try fns.append(self.allocator, .{ .name = name, .params = params, .ret = ret });
        }
        try self.expect(.rbrace);
        return .{ .fns = try fns.toOwnedSlice(self.allocator) };
    }

    fn parseTypeAlias(self: *Parser) ParseError!ast.TypeAlias {
        try self.expect(.kw_type);
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        try self.expect(.eq);
        const ty = try self.parseTypeExpr();
        return .{ .name = name, .ty = ty };
    }

    fn parseStruct(self: *Parser) ParseError!ast.StructDecl {
        try self.expect(.kw_struct);
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        try self.expect(.lbrace);
        var fields: std.ArrayListUnmanaged(ast.Field) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            const fname = self.tok.text;
            self.advance();
            try self.expect(.colon);
            const ty = try self.parseTypeExpr();
            try fields.append(self.allocator, .{ .name = fname, .ty = ty });
        }
        try self.expect(.rbrace);
        return .{ .name = name, .fields = try fields.toOwnedSlice(self.allocator) };
    }

    fn parseInterface(self: *Parser) ParseError!ast.Decl {
        try self.expect(.kw_interface);
        if (self.tok.kind == .kw_fn) {
            self.advance();
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            const name = self.tok.text;
            self.advance();
            const params = try self.parseParamList();
            var ret: ?*ast.TypeExpr = null;
            if (self.tok.kind == .colon) {
                self.advance();
                ret = try self.parseTypeExpr();
            }
            return .{ .interface_fn = .{ .name = name, .params = params, .ret = ret } };
        }
        if (self.tok.kind == .kw_struct) {
            self.advance();
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            const name = self.tok.text;
            self.advance();
            try self.expect(.lbrace);
            var methods: std.ArrayListUnmanaged(ast.MethodSig) = .{};
            while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
                try methods.append(self.allocator, try self.parseMethodSig());
            }
            try self.expect(.rbrace);
            return .{ .interface_struct = .{ .name = name, .methods = try methods.toOwnedSlice(self.allocator) } };
        }
        return error.UnexpectedToken;
    }

    fn parseMethodSig(self: *Parser) ParseError!ast.MethodSig {
        try self.expect(.kw_fn);
        var pointer_recv = false;
        if (self.tok.kind == .star) {
            pointer_recv = true;
            self.advance();
        }
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        const params = try self.parseParamList();
        const contexts = try self.parseContextClause();
        var ret: ?*ast.TypeExpr = null;
        if (self.tok.kind == .colon) {
            self.advance();
            ret = try self.parseTypeExpr();
        }
        return .{
            .pointer_recv = pointer_recv,
            .name = name,
            .params = params,
            .contexts = contexts,
            .ret = ret,
        };
    }

    fn parseFn(self: *Parser) ParseError!ast.FnDecl {
        try self.expect(.kw_fn);
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        const params = try self.parseParamList();
        const contexts = try self.parseContextClause();
        var ret: ?*ast.TypeExpr = null;
        if (self.tok.kind == .colon) {
            self.advance();
            ret = try self.parseTypeExpr();
        }
        var body: ?*ast.Block = null;
        if (self.tok.kind == .lbrace) {
            body = try self.parseBlock();
        }
        return .{
            .name = name,
            .params = params,
            .contexts = contexts,
            .ret = ret,
            .body = body,
            .exported = false,
        };
    }

    fn parseMethodRest(self: *Parser, pointer_recv: bool, type_name: []const u8) ParseError!ast.MethodDecl {
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        const params = try self.parseParamList();
        const contexts = try self.parseContextClause();
        var ret: ?*ast.TypeExpr = null;
        if (self.tok.kind == .colon) {
            self.advance();
            ret = try self.parseTypeExpr();
        }
        var body: ?*ast.Block = null;
        if (self.tok.kind == .lbrace) {
            body = try self.parseBlock();
        }
        return .{
            .pointer_recv = pointer_recv,
            .type_name = type_name,
            .name = name,
            .params = params,
            .contexts = contexts,
            .ret = ret,
            .body = body,
        };
    }

    fn parseParamList(self: *Parser) ParseError![]ast.Param {
        try self.expect(.lparen);
        var params: std.ArrayListUnmanaged(ast.Param) = .{};
        if (self.tok.kind != .rparen) {
            while (true) {
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                try self.expect(.colon);
                const ty = try self.parseTypeExpr();
                try params.append(self.allocator, .{ .name = name, .ty = ty });
                if (self.tok.kind == .comma) {
                    self.advance();
                    continue;
                }
                break;
            }
        }
        try self.expect(.rparen);
        return try params.toOwnedSlice(self.allocator);
    }

    /// with Bar | with (a: Bar, Baz)  — before return type
    fn parseContextClause(self: *Parser) ParseError![]ast.ContextParam {
        if (self.tok.kind != .kw_with) return &[_]ast.ContextParam{};
        self.advance();
        var contexts: std.ArrayListUnmanaged(ast.ContextParam) = .{};
        if (self.tok.kind == .lparen) {
            self.advance();
            while (true) {
                try contexts.append(self.allocator, try self.parseContextParam());
                if (self.tok.kind == .comma) {
                    self.advance();
                    continue;
                }
                break;
            }
            try self.expect(.rparen);
        } else {
            try contexts.append(self.allocator, try self.parseContextParam());
        }
        return try contexts.toOwnedSlice(self.allocator);
    }

    fn parseContextParam(self: *Parser) ParseError!ast.ContextParam {
        // name: Type  OR  Type
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const first = self.tok.text;
        self.advance();
        if (self.tok.kind == .colon) {
            self.advance();
            const ty = try self.parseTypeExpr();
            return .{ .name = first, .ty = ty };
        }
        const ty = try self.allocator.create(ast.TypeExpr);
        ty.* = .{ .name = first };
        return .{ .name = null, .ty = ty };
    }

    fn parseTypeExpr(self: *Parser) ParseError!*ast.TypeExpr {
        var ty = try self.parseTypePrimary();
        while (self.tok.kind == .question) {
            self.advance();
            const opt = try self.allocator.create(ast.TypeExpr);
            opt.* = .{ .optional = ty };
            ty = opt;
        }
        return ty;
    }

    fn parseTypePrimary(self: *Parser) ParseError!*ast.TypeExpr {
        if (self.tok.kind == .star) {
            self.advance();
            var is_const = false;
            if (self.tok.kind == .ident and std.mem.eql(u8, self.tok.text, "const")) {
                is_const = true;
                self.advance();
            }
            const inner = try self.parseTypePrimary();
            const ty = try self.allocator.create(ast.TypeExpr);
            ty.* = if (is_const) .{ .pointer_const = inner } else .{ .pointer = inner };
            return ty;
        }
        if (self.tok.kind == .kw_interface) {
            // inline interface fn type: interface fn (params): Ret — rare; skip for subset
            return error.UnexpectedToken;
        }
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        const ty = try self.allocator.create(ast.TypeExpr);
        ty.* = .{ .name = name };
        return ty;
    }

    fn parseBlock(self: *Parser) ParseError!*ast.Block {
        try self.expect(.lbrace);
        var stmts: std.ArrayListUnmanaged(ast.Stmt) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            try stmts.append(self.allocator, try self.parseStmt());
        }
        try self.expect(.rbrace);
        const block = try self.allocator.create(ast.Block);
        block.* = .{ .stmts = try stmts.toOwnedSlice(self.allocator) };
        return block;
    }

    fn parseStmt(self: *Parser) ParseError!ast.Stmt {
        switch (self.tok.kind) {
            .kw_val, .kw_var => {
                const mutable = self.tok.kind == .kw_var;
                self.advance();
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                var ty: ?*ast.TypeExpr = null;
                if (self.tok.kind == .colon) {
                    self.advance();
                    ty = try self.parseTypeExpr();
                }
                var init_e: ?*ast.Expr = null;
                if (self.tok.kind == .eq) {
                    self.advance();
                    init_e = try self.parseExpr();
                }
                return .{ .var_decl = .{ .mutable = mutable, .name = name, .ty = ty, .init = init_e } };
            },
            .kw_return => {
                self.advance();
                if (self.tok.kind == .rbrace) return .{ .return_stmt = null };
                return .{ .return_stmt = try self.parseExpr() };
            },
            .kw_panic => {
                self.advance();
                try self.expect(.lparen);
                const msg = try self.parseExpr();
                try self.expect(.rparen);
                const e = try self.allocator.create(ast.Expr);
                e.* = .{ .panic_call = .{ .message = msg } };
                return .{ .expr = e };
            },
            .kw_if => {
                self.advance();
                try self.expect(.lparen);
                const cond = try self.parseExpr();
                try self.expect(.rparen);
                const then_b = try self.parseBlock();
                var else_b: ?*ast.Block = null;
                if (self.tok.kind == .kw_else) {
                    self.advance();
                    else_b = try self.parseBlock();
                }
                return .{ .if_stmt = .{ .cond = cond, .then_block = then_b, .else_block = else_b } };
            },
            .kw_with => {
                self.advance();
                try self.expect(.lparen);
                var contexts: std.ArrayListUnmanaged(ast.ContextParam) = .{};
                var values: std.ArrayListUnmanaged(*ast.Expr) = .{};
                while (true) {
                    // value expression; context type inferred later — for subset allow `expr` or `name: Type = expr`
                    // Simplified: with (expr, expr) { }  — types from check ambient
                    const e = try self.parseExpr();
                    try values.append(self.allocator, e);
                    const dummy_ty = try self.allocator.create(ast.TypeExpr);
                    dummy_ty.* = .{ .name = "_" };
                    try contexts.append(self.allocator, .{ .name = null, .ty = dummy_ty });
                    if (self.tok.kind == .comma) {
                        self.advance();
                        continue;
                    }
                    break;
                }
                try self.expect(.rparen);
                const body = try self.parseBlock();
                return .{ .with_stmt = .{
                    .contexts = try contexts.toOwnedSlice(self.allocator),
                    .values = try values.toOwnedSlice(self.allocator),
                    .body = body,
                } };
            },
            else => {
                const e = try self.parseExpr();
                if (self.tok.kind == .eq) {
                    self.advance();
                    const v = try self.parseExpr();
                    if (e.* == .ident) {
                        return .{ .assign = .{ .name = e.ident, .value = v } };
                    }
                    if (e.* == .field) {
                        return .{ .assign = .{
                            .name = e.field.name,
                            .value = v,
                            .field = .{ .base = e.field.base, .name = e.field.name },
                        } };
                    }
                    return error.UnexpectedToken;
                }
                return .{ .expr = e };
            },
        }
    }

    fn parseExpr(self: *Parser) ParseError!*ast.Expr {
        return try self.parseEquality();
    }

    fn parseEquality(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseAdd();
        while (self.tok.kind == .eqeq or self.tok.kind == .neq) {
            const op: ast.BinaryOp = if (self.tok.kind == .eqeq) .eq else .neq;
            self.advance();
            const right = try self.parseAdd();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseAdd(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseUnary();
        while (self.tok.kind == .plus or self.tok.kind == .minus) {
            const op: ast.BinaryOp = if (self.tok.kind == .plus) .add else .sub;
            self.advance();
            const right = try self.parseUnary();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseUnary(self: *Parser) ParseError!*ast.Expr {
        if (self.tok.kind == .minus or self.tok.kind == .bang or self.tok.kind == .amp or self.tok.kind == .star) {
            const op: ast.UnaryOp = switch (self.tok.kind) {
                .minus => .neg,
                .bang => .not,
                .amp => .addr,
                .star => .deref,
                else => unreachable,
            };
            self.advance();
            const inner = try self.parseUnary();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .unary = .{ .op = op, .expr = inner } };
            return e;
        }
        return try self.parsePostfix();
    }

    fn parsePostfix(self: *Parser) ParseError!*ast.Expr {
        var e = try self.parsePrimary();
        while (true) {
            if (self.tok.kind == .lparen) {
                self.advance();
                var args: std.ArrayListUnmanaged(*ast.Expr) = .{};
                if (self.tok.kind != .rparen) {
                    while (true) {
                        try args.append(self.allocator, try self.parseExpr());
                        if (self.tok.kind == .comma) {
                            self.advance();
                            continue;
                        }
                        break;
                    }
                }
                try self.expect(.rparen);
                var with_args: []*ast.Expr = &.{};
                // Call-site `f() with (ctx)` must not steal statement `with (ctx) { ... }`.
                if (self.tok.kind == .kw_with) {
                    const save_i = self.lx.i;
                    const save_line = self.lx.line;
                    const save_col = self.lx.col;
                    const save_tok = self.tok;
                    self.advance();
                    try self.expect(.lparen);
                    var wargs: std.ArrayListUnmanaged(*ast.Expr) = .{};
                    while (true) {
                        try wargs.append(self.allocator, try self.parseExpr());
                        if (self.tok.kind == .comma) {
                            self.advance();
                            continue;
                        }
                        break;
                    }
                    try self.expect(.rparen);
                    if (self.tok.kind == .lbrace) {
                        // Block with-stmt — rewind so parseStmt handles it.
                        self.lx.i = save_i;
                        self.lx.line = save_line;
                        self.lx.col = save_col;
                        self.tok = save_tok;
                    } else {
                        with_args = try wargs.toOwnedSlice(self.allocator);
                    }
                }
                const call = try self.allocator.create(ast.Expr);
                call.* = .{ .call = .{
                    .callee = e,
                    .args = try args.toOwnedSlice(self.allocator),
                    .with_args = with_args,
                } };
                e = call;
                continue;
            }
            if (self.tok.kind == .dot) {
                self.advance();
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                const f = try self.allocator.create(ast.Expr);
                f.* = .{ .field = .{ .base = e, .name = name } };
                e = f;
                continue;
            }
            if (self.tok.kind == .dot_q) {
                self.advance();
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                try self.expect(.lparen);
                var args: std.ArrayListUnmanaged(*ast.Expr) = .{};
                if (self.tok.kind != .rparen) {
                    while (true) {
                        try args.append(self.allocator, try self.parseExpr());
                        if (self.tok.kind == .comma) {
                            self.advance();
                            continue;
                        }
                        break;
                    }
                }
                try self.expect(.rparen);
                const oc = try self.allocator.create(ast.Expr);
                oc.* = .{ .optional_call = .{ .base = e, .name = name, .args = try args.toOwnedSlice(self.allocator) } };
                e = oc;
                continue;
            }
            if (self.tok.kind == .kw_as) {
                self.advance();
                const ty = try self.parseTypeExpr();
                const c = try self.allocator.create(ast.Expr);
                c.* = .{ .cast = .{ .expr = e, .ty = ty } };
                e = c;
                continue;
            }
            break;
        }
        return e;
    }

    fn parsePrimary(self: *Parser) ParseError!*ast.Expr {
        const e = try self.allocator.create(ast.Expr);
        switch (self.tok.kind) {
            .ident => {
                const name = self.tok.text;
                self.advance();
                // StructLit: Name(field: expr, ...)
                if (self.tok.kind == .lparen) {
                    // Could be call or struct lit — treat as call at expression level;
                    // struct lit uses Name(field: val) with named fields.
                    // Lookahead: after ( if ident then : → struct lit
                    const save = self.snapshot();
                    self.advance();
                    if (self.tok.kind == .ident) {
                        const fname = self.tok.text;
                        self.advance();
                        if (self.tok.kind == .colon) {
                            self.advance();
                            var fields: std.ArrayListUnmanaged(ast.StructLitField) = .{};
                            const v = try self.parseExpr();
                            try fields.append(self.allocator, .{ .name = fname, .value = v });
                            while (self.tok.kind == .comma) {
                                self.advance();
                                if (self.tok.kind != .ident) return error.UnexpectedToken;
                                const n = self.tok.text;
                                self.advance();
                                try self.expect(.colon);
                                try fields.append(self.allocator, .{ .name = n, .value = try self.parseExpr() });
                            }
                            try self.expect(.rparen);
                            e.* = .{ .struct_lit = .{ .type_name = name, .fields = try fields.toOwnedSlice(self.allocator) } };
                            return e;
                        }
                    }
                    self.restore(save);
                }
                e.* = .{ .ident = name };
                return e;
            },
            .int => {
                const n = try std.fmt.parseInt(i64, self.tok.text, 10);
                self.advance();
                e.* = .{ .int = n };
                return e;
            },
            .string => {
                const raw = self.tok.text;
                self.advance();
                // strip quotes; minimal unescape
                const inner = if (raw.len >= 2) raw[1 .. raw.len - 1] else raw;
                e.* = .{ .string = try unescape(self.allocator, inner) };
                return e;
            },
            .kw_null => {
                self.advance();
                e.* = .null_lit;
                return e;
            },
            .lparen => {
                self.advance();
                const inner = try self.parseExpr();
                try self.expect(.rparen);
                return inner;
            },
            else => return error.UnexpectedToken,
        }
    }

    const Snap = struct { i: usize, line: u32, col: u32, tok: lex.Token };

    fn snapshot(self: *Parser) Snap {
        return .{ .i = self.lx.i, .line = self.lx.line, .col = self.lx.col, .tok = self.tok };
    }

    fn restore(self: *Parser, s: Snap) void {
        self.lx.i = s.i;
        self.lx.line = s.line;
        self.lx.col = s.col;
        self.tok = s.tok;
    }
};

fn unescape(allocator: std.mem.Allocator, s: []const u8) ParseError![]const u8 {
    var out: std.ArrayListUnmanaged(u8) = .{};
    errdefer out.deinit(allocator);
    var i: usize = 0;
    while (i < s.len) : (i += 1) {
        if (s[i] == '\\' and i + 1 < s.len) {
            i += 1;
            const c: u8 = switch (s[i]) {
                'n' => '\n',
                't' => '\t',
                'r' => '\r',
                '0' => 0,
                '\\' => '\\',
                '"' => '"',
                else => s[i],
            };
            try out.append(allocator, c);
        } else {
            try out.append(allocator, s[i]);
        }
    }
    return try out.toOwnedSlice(allocator);
}
