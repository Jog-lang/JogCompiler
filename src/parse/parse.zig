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
        var is_private = false;
        if (self.tok.kind == .kw_private) {
            is_private = true;
            self.advance();
            // private(package) — accept and ignore scope for now
            if (self.tok.kind == .lparen) {
                self.advance();
                if (self.tok.kind == .ident) self.advance();
                try self.expect(.rparen);
            }
        }
        // @link / @export / @test / @packed / @intrinsic
        if (self.tok.kind == .at) {
            self.advance();
            // `export` is a keyword; other attrs are bare idents.
            const attr = blk: {
                if (self.tok.kind == .ident) break :blk self.tok.text;
                if (self.tok.kind == .kw_export) break :blk @as([]const u8, "export");
                return error.UnexpectedToken;
            };
            self.advance();
            if (std.mem.eql(u8, attr, "link")) {
                try self.expect(.lparen);
                if (self.tok.kind != .string) return error.UnexpectedToken;
                const raw = self.tok.text;
                self.advance();
                try self.expect(.rparen);
                const lib = try unescape(self.allocator, if (raw.len >= 2) raw[1 .. raw.len - 1] else raw);
                var eb = try self.parseExternBlock();
                eb.link_lib = lib;
                return .{ .extern_block = eb };
            } else if (std.mem.eql(u8, attr, "export")) {
                var fd = try self.parseFn();
                fd.exported = true;
                fd.is_private = is_private;
                return .{ .fn_decl = fd };
            } else if (std.mem.eql(u8, attr, "test")) {
                var fd = try self.parseFn();
                fd.is_test = true;
                fd.is_private = is_private;
                return .{ .fn_decl = fd };
            } else if (std.mem.eql(u8, attr, "packed")) {
                var sd = try self.parseStruct();
                sd.packed_attr = true;
                return .{ .struct_decl = sd };
            } else if (std.mem.eql(u8, attr, "intrinsic")) {
                // @intrinsic(...) type Name: …  — skip args, parse type alias
                if (self.tok.kind == .lparen) {
                    self.advance();
                    var depth: u32 = 1;
                    while (self.tok.kind != .eof and depth > 0) {
                        if (self.tok.kind == .lparen) depth += 1;
                        if (self.tok.kind == .rparen) depth -= 1;
                        self.advance();
                    }
                }
                return .{ .type_alias = try self.parseTypeAlias() };
            } else {
                return error.UnexpectedToken;
            }
        }
        switch (self.tok.kind) {
            .kw_type => return .{ .type_alias = try self.parseTypeAlias() },
            .kw_struct => return .{ .struct_decl = try self.parseStruct() },
            .kw_enum => return .{ .enum_decl = try self.parseEnum() },
            .kw_import => return .{ .import_decl = try self.parseImport() },
            .kw_require => {
                self.advance();
                if (self.tok.kind == .string) {
                    const raw = self.tok.text;
                    self.advance();
                    const name = try unescape(self.allocator, if (raw.len >= 2) raw[1 .. raw.len - 1] else raw);
                    return .{ .require_decl = .{ .name = name } };
                }
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                return .{ .require_decl = .{ .name = name } };
            },
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
                fd.is_private = is_private;
                return .{ .fn_decl = fd };
            },
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
                return .{ .global_decl = .{ .mutable = mutable, .name = name, .ty = ty, .init = init_e } };
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
                var fd = try self.parseFn();
                fd.is_private = is_private;
                return .{ .fn_decl = fd };
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
        return .{ .fns = try fns.toOwnedSlice(self.allocator), .link_lib = null };
    }

    fn parseImport(self: *Parser) ParseError!ast.ImportDecl {
        try self.expect(.kw_import);
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        var path = self.tok.text;
        self.advance();
        while (self.tok.kind == .dot) {
            self.advance();
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            path = try std.fmt.allocPrint(self.allocator, "{s}.{s}", .{ path, self.tok.text });
            self.advance();
        }
        var alias: ?[]const u8 = null;
        if (self.tok.kind == .kw_as) {
            self.advance();
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            alias = self.tok.text;
            self.advance();
        }
        return .{ .path = path, .alias = alias };
    }

    fn parseEnum(self: *Parser) ParseError!ast.EnumDecl {
        try self.expect(.kw_enum);
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        if (self.tok.kind == .colon) {
            self.advance();
            _ = try self.parseTypeExpr();
        }
        try self.expect(.lbrace);
        var variants: std.ArrayListUnmanaged(ast.EnumVariant) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            const vname = self.tok.text;
            self.advance();
            var value: ?i64 = null;
            if (self.tok.kind == .eq) {
                self.advance();
                if (self.tok.kind != .int) return error.UnexpectedToken;
                value = try std.fmt.parseInt(i64, self.tok.text, 10);
                self.advance();
            }
            if (self.tok.kind == .lparen) return error.UnexpectedToken;
            try variants.append(self.allocator, .{ .name = vname, .value = value });
            if (self.tok.kind == .comma) self.advance();
        }
        try self.expect(.rbrace);
        return .{ .name = name, .variants = try variants.toOwnedSlice(self.allocator) };
    }

    fn parseTypeAlias(self: *Parser) ParseError!ast.TypeAlias {
        try self.expect(.kw_type);
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        // `type Name = T` or jogc `type Name: T`
        if (self.tok.kind == .eq or self.tok.kind == .colon) {
            self.advance();
        } else return error.UnexpectedToken;
        const ty = try self.parseTypeExpr();
        return .{ .name = name, .ty = ty };
    }

    fn parseStruct(self: *Parser) ParseError!ast.StructDecl {
        try self.expect(.kw_struct);
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        const type_params = try self.parseTypeParamList();
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
        return .{ .name = name, .type_params = type_params, .fields = try fields.toOwnedSlice(self.allocator) };
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
        const type_params = try self.parseTypeParamList();
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
            .type_params = type_params,
            .params = params,
            .contexts = contexts,
            .ret = ret,
            .body = body,
            .exported = false,
        };
    }

    fn parseMethodRest(self: *Parser, pointer_recv: bool, type_name: []const u8) ParseError!ast.MethodDecl {
        const name = try self.parseMethodName();
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

    /// Method name: ident, or `op` + operator (`op+`, `op==`, …).
    fn parseMethodName(self: *Parser) ParseError![]const u8 {
        if (self.tok.kind == .ident and std.mem.eql(u8, self.tok.text, "op")) {
            self.advance();
            const op_text: []const u8 = switch (self.tok.kind) {
                .plus => "+",
                .minus => "-",
                .star => "*",
                .slash => "/",
                .percent => "%",
                .eqeq => "==",
                .neq => "!=",
                .lt => "<",
                .gt => ">",
                .lte => "<=",
                .gte => ">=",
                .amp => "&",
                .pipe => "|",
                .caret => "^",
                .shl => "<<",
                .shr => ">>",
                .tilde => "~",
                else => return error.UnexpectedToken,
            };
            self.advance();
            return try std.fmt.allocPrint(self.allocator, "op{s}", .{op_text});
        }
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        return name;
    }

    fn parseTypeParamList(self: *Parser) ParseError![][]const u8 {
        if (self.tok.kind != .lt) return &.{};
        self.advance();
        var params: std.ArrayListUnmanaged([]const u8) = .{};
        while (true) {
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            try params.append(self.allocator, self.tok.text);
            self.advance();
            // Optional constraint `T: Iface`
            if (self.tok.kind == .colon) {
                self.advance();
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                self.advance(); // constraint name recorded lightly on FnDecl later via parseFn
            }
            if (self.tok.kind == .comma) {
                self.advance();
                continue;
            }
            break;
        }
        try self.expect(.gt);
        return try params.toOwnedSlice(self.allocator);
    }

    fn parseTypeArgList(self: *Parser) ParseError![]*ast.TypeExpr {
        if (self.tok.kind != .lt) return &.{};
        self.advance();
        var args: std.ArrayListUnmanaged(*ast.TypeExpr) = .{};
        while (true) {
            try args.append(self.allocator, try self.parseTypeExpr());
            if (self.tok.kind == .comma) {
                self.advance();
                continue;
            }
            break;
        }
        try self.expect(.gt);
        return try args.toOwnedSlice(self.allocator);
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
        if (self.tok.kind == .lbracket) {
            self.advance();
            // `[]T` / `[]const T` view, or `[N]T` fixed array
            if (self.tok.kind == .rbracket) {
                self.advance();
                var is_const = false;
                if (self.tok.kind == .ident and std.mem.eql(u8, self.tok.text, "const")) {
                    is_const = true;
                    self.advance();
                }
                const elem = try self.parseTypePrimary();
                const ty = try self.allocator.create(ast.TypeExpr);
                ty.* = .{ .slice_view = .{ .elem = elem, .is_const = is_const } };
                return ty;
            }
            if (self.tok.kind != .int) return error.UnexpectedToken;
            const len = try std.fmt.parseInt(u64, self.tok.text, 10);
            self.advance();
            try self.expect(.rbracket);
            const elem = try self.parseTypePrimary();
            const ty = try self.allocator.create(ast.TypeExpr);
            ty.* = .{ .array = .{ .len = len, .elem = elem } };
            return ty;
        }
        if (self.tok.kind == .kw_interface) {
            // inline interface fn type: interface fn (params): Ret — rare; skip for subset
            return error.UnexpectedToken;
        }
        if (self.tok.kind == .lparen) {
            self.advance();
            var elems: std.ArrayListUnmanaged(*ast.TypeExpr) = .{};
            try elems.append(self.allocator, try self.parseTypeExpr());
            if (self.tok.kind != .comma) {
                // Single `(T)` is just grouping.
                try self.expect(.rparen);
                return elems.items[0];
            }
            while (self.tok.kind == .comma) {
                self.advance();
                try elems.append(self.allocator, try self.parseTypeExpr());
            }
            try self.expect(.rparen);
            const ty = try self.allocator.create(ast.TypeExpr);
            ty.* = .{ .tuple = try elems.toOwnedSlice(self.allocator) };
            return ty;
        }
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        const targs = try self.parseTypeArgList();
        const ty = try self.allocator.create(ast.TypeExpr);
        if (targs.len == 0) {
            ty.* = .{ .name = name };
        } else {
            ty.* = .{ .named_app = .{ .name = name, .args = targs } };
        }
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
                if (self.tok.kind == .lparen) {
                    self.advance();
                    var names: std.ArrayListUnmanaged([]const u8) = .{};
                    while (true) {
                        if (self.tok.kind != .ident) return error.UnexpectedToken;
                        try names.append(self.allocator, self.tok.text);
                        self.advance();
                        if (self.tok.kind == .comma) {
                            self.advance();
                            continue;
                        }
                        break;
                    }
                    try self.expect(.rparen);
                    try self.expect(.eq);
                    return .{ .destructure = .{
                        .mutable = mutable,
                        .names = try names.toOwnedSlice(self.allocator),
                        .init = try self.parseExpr(),
                    } };
                }
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
                // Allow `if (cond)` or `if cond` (bang/ident).
                var cond: *ast.Expr = undefined;
                if (self.tok.kind == .lparen) {
                    self.advance();
                    cond = try self.parseExpr();
                    try self.expect(.rparen);
                } else {
                    cond = try self.parseExpr();
                }
                const then_b = try self.parseBlock();
                var else_b: ?*ast.Block = null;
                if (self.tok.kind == .kw_else) {
                    self.advance();
                    else_b = try self.parseBlock();
                }
                return .{ .if_stmt = .{ .cond = cond, .then_block = then_b, .else_block = else_b } };
            },
            .kw_while => {
                self.advance();
                var cond: *ast.Expr = undefined;
                if (self.tok.kind == .lparen) {
                    self.advance();
                    cond = try self.parseExpr();
                    try self.expect(.rparen);
                } else {
                    cond = try self.parseExpr();
                }
                const body = try self.parseBlock();
                return .{ .while_stmt = .{ .cond = cond, .body = body } };
            },
            .kw_for => {
                self.advance();
                // `for { }` infinite; `for x in iter { }`; `for cond { }`
                if (self.tok.kind == .lbrace) {
                    const one = try self.allocator.create(ast.Expr);
                    one.* = .{ .int = 1 };
                    return .{ .while_stmt = .{ .cond = one, .body = try self.parseBlock() } };
                }
                // Lookahead: ident `in` → for-in
                if (self.tok.kind == .ident) {
                    const save = self.snapshot();
                    const name = self.tok.text;
                    self.advance();
                    if (self.tok.kind == .kw_in) {
                        self.advance();
                        const iter = try self.parseExpr();
                        const body = try self.parseBlock();
                        return .{ .for_in_stmt = .{ .name = name, .iter = iter, .body = body } };
                    }
                    self.restore(save);
                }
                var cond: *ast.Expr = undefined;
                if (self.tok.kind == .lparen) {
                    self.advance();
                    cond = try self.parseExpr();
                    try self.expect(.rparen);
                } else {
                    cond = try self.parseExpr();
                }
                const body = try self.parseBlock();
                return .{ .while_stmt = .{ .cond = cond, .body = body } };
            },
            .kw_break => {
                self.advance();
                return .break_stmt;
            },
            .kw_continue => {
                self.advance();
                return .continue_stmt;
            },
            .kw_defer => {
                self.advance();
                if (self.tok.kind == .lbrace) {
                    return .{ .defer_stmt = .{ .block = try self.parseBlock() } };
                }
                return .{ .defer_stmt = .{ .expr = try self.parseExpr() } };
            },
            .kw_errdefer => {
                self.advance();
                if (self.tok.kind == .lbrace) {
                    return .{ .errdefer_stmt = .{ .block = try self.parseBlock() } };
                }
                return .{ .errdefer_stmt = .{ .expr = try self.parseExpr() } };
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
                const compound: ?ast.BinaryOp = switch (self.tok.kind) {
                    .eq => null,
                    .pluseq => .add,
                    .minuseq => .sub,
                    .stareq => .mul,
                    .slasheq => .div,
                    .percenteq => .rem,
                    else => {
                        return .{ .expr = e };
                    },
                };
                self.advance();
                const v = try self.parseExpr();
                if (e.* == .ident) {
                    return .{ .assign = .{ .name = e.ident, .value = v, .compound = compound } };
                }
                if (e.* == .field) {
                    return .{ .assign = .{
                        .name = e.field.name,
                        .value = v,
                        .field = .{ .base = e.field.base, .name = e.field.name },
                        .compound = compound,
                    } };
                }
                if (e.* == .index) {
                    return .{ .assign = .{
                        .name = "",
                        .value = v,
                        .index = .{ .base = e.index.base, .index = e.index.index },
                        .compound = compound,
                    } };
                }
                return error.UnexpectedToken;
            },
        }
    }

    fn parseExpr(self: *Parser) ParseError!*ast.Expr {
        return try self.parseRange();
    }

    fn parseRange(self: *Parser) ParseError!*ast.Expr {
        const left = try self.parseCoalesce();
        if (self.tok.kind == .dotdot) {
            self.advance();
            const right = try self.parseCoalesce();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .range = .{ .start = left, .end = right } };
            return e;
        }
        return left;
    }

    fn parseCoalesce(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseOr();
        while (self.tok.kind == .question) {
            const save = self.snapshot();
            self.advance();
            if (self.tok.kind != .colon) {
                self.restore(save);
                break;
            }
            self.advance();
            const right = try self.parseOr();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = .coalesce, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseOr(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseAnd();
        while (self.tok.kind == .pipepipe) {
            self.advance();
            const right = try self.parseAnd();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = .or_op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseAnd(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseBitOr();
        while (self.tok.kind == .ampamp) {
            self.advance();
            const right = try self.parseBitOr();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = .and_op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    // C/jogc-shaped: || && | ^ & == != < <= > >= << >> + - * / %
    fn parseBitOr(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseBitXor();
        while (self.tok.kind == .pipe) {
            self.advance();
            const right = try self.parseBitXor();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = .bit_or, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseBitXor(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseBitAnd();
        while (self.tok.kind == .caret) {
            self.advance();
            const right = try self.parseBitAnd();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = .bit_xor, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseBitAnd(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseEquality();
        while (self.tok.kind == .amp) {
            self.advance();
            const right = try self.parseEquality();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = .bit_and, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseEquality(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseCmp();
        while (self.tok.kind == .eqeq or self.tok.kind == .neq) {
            const op: ast.BinaryOp = if (self.tok.kind == .eqeq) .eq else .neq;
            self.advance();
            const right = try self.parseCmp();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseCmp(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseShift();
        while (self.tok.kind == .lt or self.tok.kind == .gt or self.tok.kind == .lte or self.tok.kind == .gte) {
            const op: ast.BinaryOp = switch (self.tok.kind) {
                .lt => .lt,
                .gt => .gt,
                .lte => .lte,
                .gte => .gte,
                else => unreachable,
            };
            self.advance();
            const right = try self.parseShift();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseShift(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseAdd();
        while (self.tok.kind == .shl or self.tok.kind == .shr) {
            const op: ast.BinaryOp = if (self.tok.kind == .shl) .shl else .shr;
            self.advance();
            const right = try self.parseAdd();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseAdd(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseMul();
        while (self.tok.kind == .plus or self.tok.kind == .minus) {
            const op: ast.BinaryOp = if (self.tok.kind == .plus) .add else .sub;
            self.advance();
            const right = try self.parseMul();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseMul(self: *Parser) ParseError!*ast.Expr {
        var left = try self.parseUnary();
        while (self.tok.kind == .star or self.tok.kind == .slash or self.tok.kind == .percent) {
            const op: ast.BinaryOp = switch (self.tok.kind) {
                .star => .mul,
                .slash => .div,
                .percent => .rem,
                else => unreachable,
            };
            self.advance();
            const right = try self.parseUnary();
            const e = try self.allocator.create(ast.Expr);
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseUnary(self: *Parser) ParseError!*ast.Expr {
        if (self.tok.kind == .minus or self.tok.kind == .bang or self.tok.kind == .tilde or self.tok.kind == .amp or self.tok.kind == .star) {
            const op: ast.UnaryOp = switch (self.tok.kind) {
                .minus => .neg,
                .bang => .not,
                .tilde => .bit_not,
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
            // Generic call: `id<i64>(…)` — `<` after ident before `(`.
            var pending_targs: []*ast.TypeExpr = &.{};
            if (e.* == .ident and self.tok.kind == .lt) {
                const save = self.snapshot();
                const targs = self.parseTypeArgList() catch {
                    self.restore(save);
                    break;
                };
                if (self.tok.kind != .lparen) {
                    self.restore(save);
                    break;
                }
                pending_targs = targs;
            }
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
                    .type_args = pending_targs,
                } };
                e = call;
                continue;
            }
            if (pending_targs.len != 0) return error.UnexpectedToken;
            if (self.tok.kind == .lbracket) {
                self.advance();
                const idx = try self.parseExpr();
                try self.expect(.rbracket);
                const ix = try self.allocator.create(ast.Expr);
                ix.* = .{ .index = .{ .base = e, .index = idx } };
                e = ix;
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
                const try_cast = self.tok.kind == .question;
                if (try_cast) self.advance();
                const ty = try self.parseTypeExpr();
                const c = try self.allocator.create(ast.Expr);
                if (try_cast) {
                    c.* = .{ .try_cast = .{ .expr = e, .ty = ty } };
                } else {
                    c.* = .{ .cast = .{ .expr = e, .ty = ty } };
                }
                e = c;
                continue;
            }
            if (self.tok.kind == .kw_is) {
                self.advance();
                const ty = try self.parseTypeExpr();
                const c = try self.allocator.create(ast.Expr);
                c.* = .{ .is_type = .{ .expr = e, .ty = ty } };
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
            .kw_when => {
                self.advance();
                const scrutinee = try self.parseExpr();
                try self.expect(.lbrace);
                var arms: std.ArrayListUnmanaged(ast.WhenArm) = .{};
                var else_body: ?*ast.Expr = null;
                while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
                    if (self.tok.kind == .kw_else) {
                        self.advance();
                        try self.expect(.arrow);
                        else_body = try self.parseExpr();
                        continue;
                    }
                    if (self.tok.kind != .ident) return error.UnexpectedToken;
                    const pat = self.tok.text;
                    self.advance();
                    try self.expect(.arrow);
                    const body = try self.parseExpr();
                    try arms.append(self.allocator, .{ .pattern = pat, .body = body });
                }
                try self.expect(.rbrace);
                e.* = .{ .when_expr = .{
                    .scrutinee = scrutinee,
                    .arms = try arms.toOwnedSlice(self.allocator),
                    .else_body = else_body,
                } };
                return e;
            },
            .ident => {
                const name = self.tok.text;
                self.advance();
                // Optional type args before struct lit / call: Name<…>(…)
                var type_args: []*ast.TypeExpr = &.{};
                if (self.tok.kind == .lt) {
                    const save_ta = self.snapshot();
                    if (self.parseTypeArgList()) |ta| {
                        type_args = ta;
                    } else |_| {
                        self.restore(save_ta);
                        type_args = &.{};
                    }
                }
                // StructLit: Name(field: expr, ...) or Name<T>(field: …)
                // Or generic call: Name<T>(args) — build call here so type_args are kept.
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
                            e.* = .{ .struct_lit = .{
                                .type_name = name,
                                .type_args = type_args,
                                .fields = try fields.toOwnedSlice(self.allocator),
                            } };
                            return e;
                        }
                    }
                    self.restore(save);
                    if (type_args.len != 0) {
                        // Generic call Name<T>(…) — parse args now (parsePostfix would drop type_args).
                        self.advance(); // (
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
                        const callee = try self.allocator.create(ast.Expr);
                        callee.* = .{ .ident = name };
                        e.* = .{ .call = .{
                            .callee = callee,
                            .args = try args.toOwnedSlice(self.allocator),
                            .type_args = type_args,
                        } };
                        return e;
                    }
                } else if (type_args.len != 0) {
                    // Bare Name<T> is not a value expr.
                    return error.UnexpectedToken;
                }
                e.* = .{ .ident = name };
                return e;
            },
            .int => {
                const text = self.tok.text;
                self.advance();
                const n: i64 = if (text.len >= 2 and text[0] == '0' and (text[1] == 'x' or text[1] == 'X'))
                    try std.fmt.parseInt(i64, text[2..], 16)
                else
                    try std.fmt.parseInt(i64, text, 10);
                e.* = .{ .int = n };
                return e;
            },
            .float => {
                const n = try std.fmt.parseFloat(f64, self.tok.text);
                self.advance();
                e.* = .{ .float = n };
                return e;
            },
            .char => {
                const raw = self.tok.text;
                self.advance();
                // 'x' or '\n'
                var val: i64 = 0;
                if (raw.len >= 3 and raw[0] == '\'') {
                    if (raw[1] == '\\' and raw.len >= 4) {
                        val = switch (raw[2]) {
                            'n' => '\n',
                            't' => '\t',
                            'r' => '\r',
                            '0' => 0,
                            '\\' => '\\',
                            '\'' => '\'',
                            else => raw[2],
                        };
                    } else {
                        val = raw[1];
                    }
                }
                e.* = .{ .int = val };
                return e;
            },
            .cstring => {
                const raw = self.tok.text;
                self.advance();
                // c"…"
                const inner = if (raw.len >= 3) raw[2 .. raw.len - 1] else "";
                const s = try unescape(self.allocator, inner);
                // Append NUL for C interop
                var buf = try self.allocator.alloc(u8, s.len + 1);
                @memcpy(buf[0..s.len], s);
                buf[s.len] = 0;
                e.* = .{ .cstring = buf };
                return e;
            },
            .string => {
                const raw = self.tok.text;
                self.advance();
                // strip quotes; support ${expr} interpolation
                const inner = if (raw.len >= 2) raw[1 .. raw.len - 1] else raw;
                if (std.mem.indexOf(u8, inner, "${")) |_| {
                    e.* = .{ .interp = try self.parseInterpParts(inner) };
                    return e;
                }
                e.* = .{ .string = try unescape(self.allocator, inner) };
                return e;
            },
            .lbracket => {
                // Array literal `[a, b, c]`
                self.advance();
                var elems: std.ArrayListUnmanaged(*ast.Expr) = .{};
                if (self.tok.kind != .rbracket) {
                    while (true) {
                        try elems.append(self.allocator, try self.parseExpr());
                        if (self.tok.kind == .comma) {
                            self.advance();
                            continue;
                        }
                        break;
                    }
                }
                try self.expect(.rbracket);
                e.* = .{ .array_lit = .{ .elems = try elems.toOwnedSlice(self.allocator) } };
                return e;
            },
            .kw_null => {
                self.advance();
                e.* = .null_lit;
                return e;
            },
            .kw_true => {
                self.advance();
                e.* = .{ .int = 1 };
                return e;
            },
            .kw_false => {
                self.advance();
                e.* = .{ .int = 0 };
                return e;
            },
            .lparen => {
                self.advance();
                if (self.tok.kind == .rparen) {
                    self.advance();
                    e.* = .{ .tuple = &[_]*ast.Expr{} };
                    return e;
                }
                const first = try self.parseExpr();
                if (self.tok.kind != .comma) {
                    try self.expect(.rparen);
                    return first;
                }
                var elems: std.ArrayListUnmanaged(*ast.Expr) = .{};
                try elems.append(self.allocator, first);
                while (self.tok.kind == .comma) {
                    self.advance();
                    if (self.tok.kind == .rparen) break; // trailing comma
                    try elems.append(self.allocator, try self.parseExpr());
                }
                try self.expect(.rparen);
                e.* = .{ .tuple = try elems.toOwnedSlice(self.allocator) };
                return e;
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

    fn parseInterpParts(self: *Parser, inner: []const u8) ParseError!ast.Interp {
        var parts: std.ArrayListUnmanaged(ast.InterpPart) = .{};
        var i: usize = 0;
        var text_start: usize = 0;
        while (i < inner.len) {
            if (i + 1 < inner.len and inner[i] == '$' and inner[i + 1] == '{') {
                if (i > text_start) {
                    const chunk = try unescape(self.allocator, inner[text_start..i]);
                    try parts.append(self.allocator, .{ .text = chunk });
                }
                i += 2;
                const expr_start = i;
                var depth: u32 = 1;
                while (i < inner.len and depth > 0) : (i += 1) {
                    if (inner[i] == '{') depth += 1;
                    if (inner[i] == '}') {
                        depth -= 1;
                        if (depth == 0) break;
                    }
                }
                if (depth != 0) return error.UnexpectedToken;
                const expr_src = inner[expr_start..i];
                i += 1; // }
                text_start = i;
                var nested = Parser{
                    .allocator = self.allocator,
                    .lx = lex.Lexer.init(expr_src),
                    .tok = undefined,
                };
                nested.tok = nested.lx.next();
                const ex = try nested.parseExpr();
                try parts.append(self.allocator, .{ .expr = ex });
            } else {
                i += 1;
            }
        }
        if (text_start < inner.len) {
            try parts.append(self.allocator, .{ .text = try unescape(self.allocator, inner[text_start..]) });
        }
        return .{ .parts = try parts.toOwnedSlice(self.allocator) };
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
