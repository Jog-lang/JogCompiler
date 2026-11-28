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
    file: *ast.File,
    /// Grammar: no trailing lambda in the head of `if` / `for` / `while` / `with`.
    allow_trailing: bool = true,

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
            .file = &file,
        };
        p.tok = p.lx.next();
        var decls: std.ArrayListUnmanaged(ast.Decl) = .{};
        p.skipSeps();
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
            p.skipSeps();
        }
        file.decls = try decls.toOwnedSlice(p.allocator);
        return file;
    }

    fn advance(self: *Parser) void {
        self.tok = self.lx.next();
    }

    /// Allocate an expression and record the current token's span for diagnostics.
    fn allocExpr(self: *Parser) ParseError!*ast.Expr {
        return self.allocExprAt(self.tok.line, self.tok.col);
    }

    fn allocExprAt(self: *Parser, line: u32, col: u32) ParseError!*ast.Expr {
        const e = try self.allocator.create(ast.Expr);
        try self.file.expr_spans.put(self.allocator, @intFromPtr(e), .{ .line = line, .col = col });
        return e;
    }

    fn expect(self: *Parser, kind: lex.TokenKind) ParseError!void {
        if (self.tok.kind != kind) return error.UnexpectedToken;
        self.advance();
    }

    /// Grammar `SEP`: inserted newline terminator or explicit `;`.
    fn skipSeps(self: *Parser) void {
        while (self.tok.kind == .semicolon) self.advance();
    }

    fn parseDecl(self: *Parser) ParseError!ast.Decl {
        var is_private = false;
        if (self.tok.kind == .kw_private) {
            self.advance();
            // Bare `private` / `private(file)` are file-local (mangled before flatten).
            // `private(package)` is package-visible — keep the source name.
            if (self.tok.kind == .lparen) {
                self.advance();
                var file_local = true;
                if (self.tok.kind == .ident) {
                    if (std.mem.eql(u8, self.tok.text, "package")) file_local = false;
                    self.advance();
                }
                try self.expect(.rparen);
                is_private = file_local;
            } else {
                is_private = true;
            }
        }
        // Grammar: `{ attribute }` before the declaration.
        var attrs: std.ArrayListUnmanaged(ast.Attr) = .{};
        while (self.tok.kind == .at) {
            try attrs.append(self.allocator, try self.parseOneAttr());
            self.skipSeps();
        }
        const attr_slice = try attrs.toOwnedSlice(self.allocator);

        // `@link("…") extern { … }` — link is not a general user attr.
        if (attr_slice.len == 1 and std.mem.eql(u8, attr_slice[0].name, "link")) {
            if (attr_slice[0].args.len != 1 or attr_slice[0].args[0].* != .string)
                return error.UnexpectedToken;
            const lib = attr_slice[0].args[0].string;
            var eb = try self.parseExternBlock();
            eb.link_lib = lib;
            return .{ .extern_block = eb };
        }
        if (hasAttrNamed(attr_slice, "intrinsic")) {
            self.skipSeps();
            return .{ .type_alias = try self.parseTypeAlias() };
        }

        var is_export = hasAttrNamed(attr_slice, "export");
        const is_test = hasAttrNamed(attr_slice, "test");
        const is_fuzz = hasAttrNamed(attr_slice, "fuzz");
        const is_packed = hasAttrNamed(attr_slice, "packed");
        const is_thread_local = hasAttrNamed(attr_slice, "threadLocal");
        const is_annotation = hasAttrNamed(attr_slice, "Annotation");
        var annotation_repeatable = false;
        if (is_annotation) {
            if (findAttr(attr_slice, "Annotation")) |ann| {
                annotation_repeatable = attrBoolOpt(ann, "repeatable", false);
            }
        }
        const user_attrs = try filterUserAttrs(self.allocator, attr_slice);

        switch (self.tok.kind) {
            .kw_type => return .{ .type_alias = try self.parseTypeAlias() },
            .kw_struct => {
                var sd = try self.parseStruct();
                sd.packed_attr = is_packed;
                sd.attrs = user_attrs;
                return .{ .struct_decl = sd };
            },
            .kw_object => {
                var sd = try self.parseObject();
                sd.attrs = user_attrs;
                return .{ .struct_decl = sd };
            },
            .kw_enum => {
                var ed = try self.parseEnum();
                ed.attrs = user_attrs;
                return .{ .enum_decl = ed };
            },
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
                var fd = try self.parseFn();
                fd.attrs = user_attrs;
                return .{ .actual_fn = fd };
            },
            .kw_export => {
                self.advance();
                is_export = true;
                var fd = try self.parseFn();
                fd.exported = true;
                fd.is_private = is_private;
                fd.attrs = user_attrs;
                return .{ .fn_decl = fd };
            },
            .kw_val, .kw_var => {
                const mutable = self.tok.kind == .kw_var;
                // `@threadLocal` only on package `var` (JOG-227).
                if (is_thread_local and !mutable) return error.UnexpectedToken;
                self.advance();
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                const name_line = self.tok.line;
                const name_col = self.tok.col;
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
                return .{ .global_decl = .{
                    .mutable = mutable,
                    .name = name,
                    .ty = ty,
                    .init = init_e,
                    .thread_local = is_thread_local,
                    .line = name_line,
                    .col = name_col,
                    .attrs = user_attrs,
                } };
            },
            .kw_fn => {
                // method: fn [*]? Type [ <T,…> ] . name  (Grammar receiver)
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
                    const recv_tparams = try self.parseTypeParamList();
                    if (self.tok.kind == .dot) {
                        self.advance();
                        return .{ .method = try self.parseMethodRest(pointer_recv, type_name, recv_tparams) };
                    }
                }
                // rewind to parse as plain fn
                self.lx.i = save_i;
                self.lx.line = save_line;
                self.lx.col = save_col;
                self.tok = save_tok;
                var fd = try self.parseFn();
                fd.is_private = is_private;
                fd.exported = is_export;
                fd.is_test = is_test;
                fd.is_fuzz = is_fuzz;
                fd.is_annotation = is_annotation;
                fd.annotation_repeatable = annotation_repeatable;
                fd.attrs = user_attrs;
                return .{ .fn_decl = fd };
            },
            else => return error.UnexpectedToken,
        }
    }

    fn parseOneAttr(self: *Parser) ParseError!ast.Attr {
        const line = self.tok.line;
        const col = self.tok.col;
        try self.expect(.at);
        const name = blk: {
            if (self.tok.kind == .ident) break :blk self.tok.text;
            if (self.tok.kind == .kw_export) break :blk @as([]const u8, "export");
            if (self.tok.kind == .kw_extern) break :blk @as([]const u8, "extern");
            return error.UnexpectedToken;
        };
        self.advance();
        var args: []*ast.Expr = &.{};
        var arg_names: []const ?[]const u8 = &.{};
        if (self.tok.kind == .lparen) {
            self.advance();
            // `@intrinsic(...)` — args are not real Jog exprs; skip balanced parens.
            if (std.mem.eql(u8, name, "intrinsic")) {
                var depth: u32 = 1;
                while (self.tok.kind != .eof and depth > 0) {
                    if (self.tok.kind == .lparen) depth += 1;
                    if (self.tok.kind == .rparen) depth -= 1;
                    self.advance();
                }
            } else {
                const parsed = try self.parseArgList(.rparen);
                args = parsed.args;
                arg_names = parsed.names;
                try self.expect(.rparen);
            }
        }
        return .{ .name = name, .args = args, .arg_names = arg_names, .line = line, .col = col };
    }

    fn hasAttrNamed(attrs: []const ast.Attr, name: []const u8) bool {
        for (attrs) |a| if (std.mem.eql(u8, a.name, name)) return true;
        return false;
    }

    fn findAttr(attrs: []const ast.Attr, name: []const u8) ?ast.Attr {
        for (attrs) |a| if (std.mem.eql(u8, a.name, name)) return a;
        return null;
    }

    fn attrBoolOpt(attr: ast.Attr, key: []const u8, default: bool) bool {
        if (attr.arg_names.len != attr.args.len) return default;
        for (attr.arg_names, attr.args) |n, v| {
            if (n) |name| {
                if (std.mem.eql(u8, name, key)) {
                    if (v.* == .int) return v.int != 0;
                    return default;
                }
            }
        }
        return default;
    }

    fn filterUserAttrs(allocator: std.mem.Allocator, attrs: []const ast.Attr) ParseError![]ast.Attr {
        const skip = [_][]const u8{
            "link",     "export", "test",      "packed",    "intrinsic", "opaque",
            "Annotation", "union", "align",    "unchecked", "extern",    "systemStack",
            "blocking", "bench",  "fuzz",      "threadLocal",
        };
        var out: std.ArrayListUnmanaged(ast.Attr) = .{};
        for (attrs) |a| {
            var is_skip = false;
            for (skip) |s| {
                if (std.mem.eql(u8, a.name, s)) {
                    is_skip = true;
                    break;
                }
            }
            if (!is_skip) try out.append(allocator, a);
        }
        return try out.toOwnedSlice(allocator);
    }

    fn parseExternBlock(self: *Parser) ParseError!ast.ExternBlock {
        try self.expect(.kw_extern);
        try self.expect(.lbrace);
        self.skipSeps();
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
            self.skipSeps();
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
        const line = self.tok.line;
        const col = self.tok.col;
        self.advance();
        if (self.tok.kind == .colon) {
            self.advance();
            _ = try self.parseTypeExpr();
        }
        try self.expect(.lbrace);
        self.skipSeps();
        var variants: std.ArrayListUnmanaged(ast.EnumVariant) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            const vname = self.tok.text;
            self.advance();
            var value: ?i64 = null;
            var fields: []ast.Field = &.{};
            // Payload `Circle(r: f64)` or explicit tag `Red = 1` — never both (Grammar).
            if (self.tok.kind == .lparen) {
                fields = try self.parseEnumPayloadFields();
            } else if (self.tok.kind == .eq) {
                self.advance();
                if (self.tok.kind != .int) return error.UnexpectedToken;
                value = try std.fmt.parseInt(i64, self.tok.text, 10);
                self.advance();
            }
            try variants.append(self.allocator, .{ .name = vname, .value = value, .fields = fields });
            self.skipSeps();
            if (self.tok.kind == .comma) {
                self.advance();
                self.skipSeps();
            }
        }
        try self.expect(.rbrace);
        return .{
            .name = name,
            .variants = try variants.toOwnedSlice(self.allocator),
            .line = line,
            .col = col,
        };
    }

    /// Payload field list inside `Variant(name: T, ...)`.
    fn parseEnumPayloadFields(self: *Parser) ParseError![]ast.Field {
        try self.expect(.lparen);
        var fields: std.ArrayListUnmanaged(ast.Field) = .{};
        if (self.tok.kind != .rparen) {
            while (true) {
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const fname = self.tok.text;
                self.advance();
                try self.expect(.colon);
                const ty = try self.parseTypeExpr();
                try fields.append(self.allocator, .{ .name = fname, .ty = ty });
                if (self.tok.kind == .comma) {
                    self.advance();
                    if (self.tok.kind == .rparen) break;
                    continue;
                }
                break;
            }
        }
        try self.expect(.rparen);
        return try fields.toOwnedSlice(self.allocator);
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
        return try self.parseStructBody(false);
    }

    fn parseObject(self: *Parser) ParseError!ast.StructDecl {
        try self.expect(.kw_object);
        // `object` has no type params in the spike.
        return try self.parseStructBody(true);
    }

    fn parseStructBody(self: *Parser, is_object: bool) ParseError!ast.StructDecl {
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        const line = self.tok.line;
        const col = self.tok.col;
        self.advance();
        const type_params: [][]const u8 = if (is_object) &.{} else try self.parseTypeParamList();
        // `@opaque struct FILE` — empty body, no braces.
        if (self.tok.kind != .lbrace) {
            if (is_object) return error.UnexpectedToken;
            return .{
                .name = name,
                .line = line,
                .col = col,
                .type_params = type_params,
                .fields = &.{},
                .is_object = false,
            };
        }
        try self.expect(.lbrace);
        self.skipSeps();
        var fields: std.ArrayListUnmanaged(ast.Field) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            var is_private = false;
            if (self.tok.kind == .kw_private) {
                is_private = true;
                self.advance();
            }
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            const fname = self.tok.text;
            self.advance();
            if (self.tok.kind == .colon) {
                self.advance();
                const ty = try self.parseTypeExpr();
                try fields.append(self.allocator, .{ .name = fname, .ty = ty, .is_private = is_private });
            } else {
                // Embedding: bare type name, field name is the type leaf.
                // Spec: `struct Player { Entity  name: str }` — see Structs.md.
                const ty = try self.allocator.create(ast.TypeExpr);
                ty.* = .{ .name = fname };
                try fields.append(self.allocator, .{
                    .name = fname,
                    .ty = ty,
                    .is_private = is_private,
                    .is_embedded = true,
                });
            }
            self.skipSeps();
            if (self.tok.kind == .comma) {
                self.advance();
                self.skipSeps();
            }
        }
        try self.expect(.rbrace);
        return .{
            .name = name,
            .line = line,
            .col = col,
            .type_params = type_params,
            .fields = try fields.toOwnedSlice(self.allocator),
            .is_object = is_object,
        };
    }

    /// `= expr` → `{ return expr }` for expression-bodied fn/method.
    fn parseExprBodyAsBlock(self: *Parser) ParseError!*ast.Block {
        try self.expect(.eq);
        const expr = try self.parseExpr();
        const stmts = try self.allocator.alloc(ast.Stmt, 1);
        stmts[0] = .{ .return_stmt = expr };
        const block = try self.allocator.create(ast.Block);
        block.* = .{ .stmts = stmts };
        return block;
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
        // Spec form `interface Name { … }` and legacy `interface struct Name { … }`.
        if (self.tok.kind == .kw_struct) {
            self.advance();
        }
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        const name = self.tok.text;
        self.advance();
        try self.expect(.lbrace);
        self.skipSeps();
        var methods: std.ArrayListUnmanaged(ast.MethodSig) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            // Spec also allows `val`/`var` property requirements and embeds — accept `fn` methods for now.
            if (self.tok.kind == .kw_fn) {
                try methods.append(self.allocator, try self.parseMethodSig());
                self.skipSeps();
                continue;
            }
            // Skip unsupported property/embed/type-set lines (`~i8 | ~i16`, …) until `fn` or `}`.
            if (self.tok.kind == .kw_val or self.tok.kind == .kw_var or self.tok.kind == .ident or
                self.tok.kind == .tilde or self.tok.kind == .pipe)
            {
                while (self.tok.kind != .kw_fn and self.tok.kind != .rbrace and self.tok.kind != .eof) {
                    self.advance();
                }
                self.skipSeps();
                continue;
            }
            return error.UnexpectedToken;
        }
        try self.expect(.rbrace);
        return .{ .interface_struct = .{ .name = name, .methods = try methods.toOwnedSlice(self.allocator) } };
    }

    fn parseMethodSig(self: *Parser) ParseError!ast.MethodSig {
        try self.expect(.kw_fn);
        var pointer_recv = false;
        if (self.tok.kind == .star) {
            pointer_recv = true;
            self.advance();
        }
        const name = try self.parseMethodName();
        const params = try self.parseParamList();
        // Grammar: [ ":" type ] [ contextClause ]
        var ret: ?*ast.TypeExpr = null;
        if (self.tok.kind == .colon) {
            self.advance();
            ret = try self.parseTypeExpr();
        }
        const contexts = try self.parseContextClause();
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
        const line = self.tok.line;
        const col = self.tok.col;
        self.advance();
        const type_params = try self.parseTypeParamList();
        const params = try self.parseParamList();
        // Grammar: [ ":" type ] [ contextClause ] fnBody — also accept legacy
        // `with` before `: type` for existing v2 examples.
        var ret: ?*ast.TypeExpr = null;
        var contexts: []ast.ContextParam = &.{};
        if (self.tok.kind == .kw_with) {
            contexts = try self.parseContextClause();
            if (self.tok.kind == .colon) {
                self.advance();
                ret = try self.parseTypeExpr();
            }
        } else {
            if (self.tok.kind == .colon) {
                self.advance();
                ret = try self.parseTypeExpr();
            }
            contexts = try self.parseContextClause();
        }
        var body: ?*ast.Block = null;
        if (self.tok.kind == .lbrace) {
            body = try self.parseBlock();
        } else if (self.tok.kind == .eq) {
            body = try self.parseExprBodyAsBlock();
        }
        return .{
            .name = name,
            .type_params = type_params,
            .params = params,
            .contexts = contexts,
            .ret = ret,
            .body = body,
            .exported = false,
            .line = line,
            .col = col,
        };
    }

    fn parseMethodRest(self: *Parser, pointer_recv: bool, type_name: []const u8, recv_tparams: [][]const u8) ParseError!ast.MethodDecl {
        const name = try self.parseMethodName();
        // Grammar: fnName [ typeParams ] — e.g. List<T>.map<R>
        const method_tparams = try self.parseTypeParamList();
        var all_tp: std.ArrayListUnmanaged([]const u8) = .{};
        try all_tp.appendSlice(self.allocator, recv_tparams);
        try all_tp.appendSlice(self.allocator, method_tparams);
        const params = try self.parseParamList();
        var ret: ?*ast.TypeExpr = null;
        var contexts: []ast.ContextParam = &.{};
        if (self.tok.kind == .kw_with) {
            contexts = try self.parseContextClause();
            if (self.tok.kind == .colon) {
                self.advance();
                ret = try self.parseTypeExpr();
            }
        } else {
            if (self.tok.kind == .colon) {
                self.advance();
                ret = try self.parseTypeExpr();
            }
            contexts = try self.parseContextClause();
        }
        var body: ?*ast.Block = null;
        if (self.tok.kind == .lbrace) {
            body = try self.parseBlock();
        } else if (self.tok.kind == .eq) {
            body = try self.parseExprBodyAsBlock();
        }
        return .{
            .pointer_recv = pointer_recv,
            .type_name = type_name,
            .type_params = try all_tp.toOwnedSlice(self.allocator),
            .name = name,
            .params = params,
            .contexts = contexts,
            .ret = ret,
            .body = body,
        };
    }

    /// Method / field name: ident, contextual `expect`/`actual`, or `op` + operator.
    /// Spec: `expect`/`actual` are keywords only before top-level `fn`; elsewhere they are names.
    fn parseNameToken(self: *Parser) ParseError![]const u8 {
        if (self.tok.kind == .ident or self.tok.kind == .kw_expect or self.tok.kind == .kw_actual) {
            const name = self.tok.text;
            self.advance();
            return name;
        }
        return error.UnexpectedToken;
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
                .spaceship => "<=>",
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
        return try self.parseNameToken();
    }

    fn parseTypeParamList(self: *Parser) ParseError![][]const u8 {
        if (self.tok.kind != .lt) return &.{};
        self.advance();
        var params: std.ArrayListUnmanaged([]const u8) = .{};
        while (true) {
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            try params.append(self.allocator, self.tok.text);
            self.advance();
            // Optional constraint `T: Iface` or `T: pkg.Iface`
            if (self.tok.kind == .colon) {
                self.advance();
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                self.advance();
                while (self.tok.kind == .dot) {
                    self.advance();
                    if (self.tok.kind != .ident) return error.UnexpectedToken;
                    self.advance();
                }
            }
            if (self.tok.kind == .comma) {
                self.advance();
                // Grammar: typeParam { "," typeParam } with optional trailing comma.
                if (self.tok.kind == .gt) break;
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
                // Grammar typeArgs allows trailing comma before `>`.
                if (self.tok.kind == .gt) break;
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
                // Pack: `...T: name` (must be last). Bare `...` is C variadic in extern only.
                if (self.tok.kind == .ellipsis) {
                    self.advance();
                    if (self.tok.kind == .rparen or self.tok.kind == .comma) {
                        // Bare `...` — C variadic; leave as sentinel empty-name pack for extern.
                        try params.append(self.allocator, .{
                            .name = "...",
                            .ty = blk: {
                                const t = try self.allocator.create(ast.TypeExpr);
                                t.* = .{ .name = "void" };
                                break :blk t;
                            },
                            .is_pack = true,
                        });
                        if (self.tok.kind == .comma) self.advance();
                        break;
                    }
                    const elem_ty = try self.parseTypeExpr();
                    try self.expect(.colon);
                    if (self.tok.kind != .ident) return error.UnexpectedToken;
                    const name = self.tok.text;
                    self.advance();
                    // Local type is []elem — wrap as slice_view.
                    const slice_ty = try self.allocator.create(ast.TypeExpr);
                    slice_ty.* = .{ .slice_view = .{ .elem = elem_ty, .is_const = false } };
                    try params.append(self.allocator, .{ .name = name, .ty = slice_ty, .is_pack = true });
                    if (self.tok.kind == .comma) {
                        self.advance();
                        // Pack must be last — anything after is an error at check time; reject here too.
                        if (self.tok.kind != .rparen) return error.UnexpectedToken;
                    }
                    break;
                }
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                try self.expect(.colon);
                const ty = try self.parseTypeExpr();
                var default: ?*ast.Expr = null;
                if (self.tok.kind == .eq) {
                    self.advance();
                    default = try self.parseExpr();
                }
                try params.append(self.allocator, .{ .name = name, .ty = ty, .default = default });
                if (self.tok.kind == .comma) {
                    self.advance();
                    // Grammar paramList / fnParams: optional trailing comma before `)`.
                    if (self.tok.kind == .rparen) break;
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
                    // Grammar contextClause: optional trailing comma before `)`.
                    if (self.tok.kind == .rparen) break;
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
            if (self.tok.kind != .rparen) {
                try elems.append(self.allocator, try self.parseTypeExpr());
                while (self.tok.kind == .comma) {
                    self.advance();
                    // Trailing comma before `)` in tuple / fn-type params.
                    if (self.tok.kind == .rparen) break;
                    try elems.append(self.allocator, try self.parseTypeExpr());
                }
            }
            try self.expect(.rparen);
            // Grammar fnType: (…) -> type
            if (self.tok.kind == .arrow) {
                self.advance();
                const ret = try self.parseTypeExpr();
                var params: std.ArrayListUnmanaged(ast.Param) = .{};
                for (elems.items) |et| {
                    try params.append(self.allocator, .{ .name = "", .ty = et });
                }
                const ift = try self.allocator.create(ast.IfaceFnType);
                ift.* = .{ .params = try params.toOwnedSlice(self.allocator), .ret = ret };
                const ty = try self.allocator.create(ast.TypeExpr);
                ty.* = .{ .iface_fn = ift };
                return ty;
            }
            if (elems.items.len == 0) {
                // Unit type `()`.
                const ty = try self.allocator.create(ast.TypeExpr);
                ty.* = .{ .tuple = &.{} };
                return ty;
            }
            if (elems.items.len == 1) {
                // Single `(T)` is just grouping.
                return elems.items[0];
            }
            const ty = try self.allocator.create(ast.TypeExpr);
            ty.* = .{ .tuple = try elems.toOwnedSlice(self.allocator) };
            return ty;
        }
        // Spec sugar: `const bytes` → `[]const byte` (Syntax fuzz / Types).
        if (self.tok.kind == .ident and std.mem.eql(u8, self.tok.text, "const")) {
            self.advance();
            if (self.tok.kind != .ident or !std.mem.eql(u8, self.tok.text, "bytes")) return error.UnexpectedToken;
            self.advance();
            const elem = try self.allocator.create(ast.TypeExpr);
            elem.* = .{ .name = "byte" };
            const ty = try self.allocator.create(ast.TypeExpr);
            ty.* = .{ .slice_view = .{ .elem = elem, .is_const = true } };
            return ty;
        }
        if (self.tok.kind != .ident) return error.UnexpectedToken;
        // Grammar: qualifiedName = IDENT { "." IDENT } [ typeArgs ]
        var name = self.tok.text;
        self.advance();
        while (self.tok.kind == .dot) {
            self.advance();
            if (self.tok.kind != .ident) return error.UnexpectedToken;
            name = try std.fmt.allocPrint(self.allocator, "{s}.{s}", .{ name, self.tok.text });
            self.advance();
        }
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
        self.skipSeps();
        var stmts: std.ArrayListUnmanaged(ast.Stmt) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            try stmts.append(self.allocator, try self.parseStmt());
            self.skipSeps();
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
                const name_line = self.tok.line;
                const name_col = self.tok.col;
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
                return .{ .var_decl = .{
                    .mutable = mutable,
                    .name = name,
                    .ty = ty,
                    .init = init_e,
                    .line = name_line,
                    .col = name_col,
                } };
            },
            .kw_return => {
                self.advance();
                if (self.tok.kind == .rbrace or self.tok.kind == .semicolon) return .{ .return_stmt = null };
                // Grammar: return [ expr { "," expr } ] — bare commas build a tuple.
                const first = try self.parseExpr();
                if (self.tok.kind != .comma) return .{ .return_stmt = first };
                var elems: std.ArrayListUnmanaged(*ast.Expr) = .{};
                try elems.append(self.allocator, first);
                while (self.tok.kind == .comma) {
                    self.advance();
                    try elems.append(self.allocator, try self.parseExpr());
                }
                const tup = try self.allocExpr();
                tup.* = .{ .tuple = try elems.toOwnedSlice(self.allocator) };
                return .{ .return_stmt = tup };
            },
            .kw_panic => {
                self.advance();
                try self.expect(.lparen);
                const msg = try self.parseExpr();
                try self.expect(.rparen);
                const e = try self.allocExpr();
                e.* = .{ .panic_call = .{ .message = msg } };
                return .{ .expr = e };
            },
            .kw_if => {
                self.advance();
                // Allow `if (cond)` or `if cond` (bang/ident).
                // Heads do not take a trailing lambda (Grammar disambiguation).
                const saved_trail = self.allow_trailing;
                self.allow_trailing = false;
                var cond: *ast.Expr = undefined;
                if (self.tok.kind == .lparen) {
                    self.advance();
                    cond = try self.parseExpr();
                    try self.expect(.rparen);
                } else {
                    cond = try self.parseExpr();
                }
                self.allow_trailing = saved_trail;
                const then_b = try self.parseBlock();
                var else_b: ?*ast.Block = null;
                // Grammar: ignore SEP right before `else`.
                self.skipSeps();
                if (self.tok.kind == .kw_else) {
                    self.advance();
                    // Grammar ifExpr: else ( ifExpr | block ). `else if` → one-stmt block.
                    if (self.tok.kind == .kw_if) {
                        const inner = try self.parseStmt();
                        const wrap = try self.allocator.create(ast.Block);
                        const stmts = try self.allocator.alloc(ast.Stmt, 1);
                        stmts[0] = inner;
                        wrap.* = .{ .stmts = stmts };
                        else_b = wrap;
                    } else {
                        else_b = try self.parseBlock();
                    }
                }
                return .{ .if_stmt = .{ .cond = cond, .then_block = then_b, .else_block = else_b } };
            },
            .kw_while => {
                self.advance();
                const saved_trail = self.allow_trailing;
                self.allow_trailing = false;
                var cond: *ast.Expr = undefined;
                if (self.tok.kind == .lparen) {
                    self.advance();
                    cond = try self.parseExpr();
                    try self.expect(.rparen);
                } else {
                    cond = try self.parseExpr();
                }
                self.allow_trailing = saved_trail;
                const body = try self.parseBlock();
                return .{ .while_stmt = .{ .cond = cond, .body = body } };
            },
            .kw_for => {
                self.advance();
                // `for { }` infinite; `for x in iter { }`; `for cond { }`
                if (self.tok.kind == .lbrace) {
                    const one = try self.allocExpr();
                    one.* = .{ .int = 1 };
                    return .{ .while_stmt = .{ .cond = one, .body = try self.parseBlock() } };
                }
                // Lookahead: ident `in` → for-in
                const saved_trail = self.allow_trailing;
                self.allow_trailing = false;
                if (self.tok.kind == .ident) {
                    const save = self.snapshot();
                    const name = self.tok.text;
                    self.advance();
                    if (self.tok.kind == .kw_in) {
                        self.advance();
                        const iter = try self.parseExpr();
                        self.allow_trailing = saved_trail;
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
                self.allow_trailing = saved_trail;
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
                // Grammar: defer ( block | stmt ). Wrap a bare stmt in a one-stmt block
                // so assignment (`defer n = n + 1`) and other stmts lower cleanly.
                if (self.tok.kind == .lbrace) {
                    return .{ .defer_stmt = .{ .block = try self.parseBlock() } };
                }
                const inner = try self.parseStmt();
                if (inner == .expr) {
                    return .{ .defer_stmt = .{ .expr = inner.expr } };
                }
                const wrap = try self.allocator.create(ast.Block);
                const stmts = try self.allocator.alloc(ast.Stmt, 1);
                stmts[0] = inner;
                wrap.* = .{ .stmts = stmts };
                return .{ .defer_stmt = .{ .block = wrap } };
            },
            .kw_errdefer => {
                self.advance();
                if (self.tok.kind == .lbrace) {
                    return .{ .errdefer_stmt = .{ .block = try self.parseBlock() } };
                }
                const inner = try self.parseStmt();
                if (inner == .expr) {
                    return .{ .errdefer_stmt = .{ .expr = inner.expr } };
                }
                const wrap = try self.allocator.create(ast.Block);
                const stmts = try self.allocator.alloc(ast.Stmt, 1);
                stmts[0] = inner;
                wrap.* = .{ .stmts = stmts };
                return .{ .errdefer_stmt = .{ .block = wrap } };
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
                switch (e.*) {
                    .ident => |name| {
                        return .{ .assign = .{ .name = name, .value = v, .compound = compound } };
                    },
                    .field => |fa| {
                        return .{ .assign = .{
                            .name = fa.name,
                            .value = v,
                            .field = .{ .base = fa.base, .name = fa.name },
                            .compound = compound,
                        } };
                    },
                    .index => |ix| {
                        return .{ .assign = .{
                            .name = "",
                            .value = v,
                            .index = .{ .base = ix.base, .index = ix.index },
                            .compound = compound,
                        } };
                    },
                    .unary => |u| {
                        if (u.op == .deref) {
                            return .{ .assign = .{
                                .name = "",
                                .value = v,
                                .deref = u.expr,
                                .compound = compound,
                            } };
                        }
                        return error.UnexpectedToken;
                    },
                    else => return error.UnexpectedToken,
                }
            },
        }
    }

    fn parseExpr(self: *Parser) ParseError!*ast.Expr {
        // Grammar: expr = ifExpr | whenExpr | goExpr | lambda | orExpr
        if (self.tok.kind == .kw_if) return try self.parseIfExpr();
        return try self.parseRange();
    }

    /// `if cond { … } [ else ( ifExpr | block ) ]` as a value-producing expression.
    fn parseIfExpr(self: *Parser) ParseError!*ast.Expr {
        try self.expect(.kw_if);
        // Heads do not take a trailing lambda (same disambiguation as if-stmt).
        const saved_trail = self.allow_trailing;
        self.allow_trailing = false;
        var cond: *ast.Expr = undefined;
        if (self.tok.kind == .lparen) {
            self.advance();
            cond = try self.parseExpr();
            try self.expect(.rparen);
        } else {
            cond = try self.parseExpr();
        }
        self.allow_trailing = saved_trail;
        const then_b = try self.parseBlock();
        var else_b: ?*ast.Block = null;
        if (self.tok.kind == .kw_else) {
            self.advance();
            if (self.tok.kind == .kw_if) {
                // else if → one-stmt block whose value is the nested if_expr.
                const inner = try self.parseIfExpr();
                const wrap = try self.allocator.create(ast.Block);
                const stmts = try self.allocator.alloc(ast.Stmt, 1);
                stmts[0] = .{ .expr = inner };
                wrap.* = .{ .stmts = stmts };
                else_b = wrap;
            } else {
                else_b = try self.parseBlock();
            }
        }
        const e = try self.allocExpr();
        e.* = .{ .if_expr = .{ .cond = cond, .then_block = then_b, .else_block = else_b } };
        return e;
    }

    fn parseRange(self: *Parser) ParseError!*ast.Expr {
        const left = try self.parseCoalesce();
        if (self.tok.kind == .dotdot) {
            self.advance();
            const right = try self.parseCoalesce();
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
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
            const e = try self.allocExpr();
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    fn parseMul(self: *Parser) ParseError!*ast.Expr {
        // Grammar: mulExpr = castExpr { ("*"|"/"|"%") castExpr }
        var left = try self.parseCast();
        while (self.tok.kind == .star or self.tok.kind == .slash or self.tok.kind == .percent) {
            const op: ast.BinaryOp = switch (self.tok.kind) {
                .star => .mul,
                .slash => .div,
                .percent => .rem,
                else => unreachable,
            };
            self.advance();
            const right = try self.parseCast();
            const e = try self.allocExpr();
            e.* = .{ .binary = .{ .op = op, .left = left, .right = right } };
            left = e;
        }
        return left;
    }

    /// Grammar castExpr = unaryExpr { ("as"|"as?") type }.
    /// Prefix `&` binds tighter than `as`, so `&x as *T` is `(&x) as *T` (not `&(x as *T)`).
    fn parseCast(self: *Parser) ParseError!*ast.Expr {
        var e = try self.parseUnary();
        while (self.tok.kind == .kw_as) {
            self.advance();
            const try_cast = self.tok.kind == .question;
            if (try_cast) self.advance();
            const ty = try self.parseTypeExpr();
            const c = try self.allocExpr();
            if (try_cast) {
                c.* = .{ .try_cast = .{ .expr = e, .ty = ty } };
            } else {
                c.* = .{ .cast = .{ .expr = e, .ty = ty } };
            }
            e = c;
        }
        return e;
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
            // Fold `-9223372036854775808` (magnitude does not fit in i64 alone).
            if (op == .neg and self.tok.kind == .int) {
                const text = self.tok.text;
                const is_hex = text.len >= 2 and text[0] == '0' and (text[1] == 'x' or text[1] == 'X');
                if (!is_hex) {
                    if (std.fmt.parseInt(u64, text, 10)) |mag| {
                        if (mag == 1 << 63) {
                            self.advance();
                            const e = try self.allocExpr();
                            e.* = .{ .int = std.math.minInt(i64) };
                            return e;
                        }
                    } else |_| {}
                }
            }
            const inner = try self.parseUnary();
            const e = try self.allocExpr();
            e.* = .{ .unary = .{ .op = op, .expr = inner } };
            return e;
        }
        return try self.parsePostfix();
    }

    fn parsePostfix(self: *Parser) ParseError!*ast.Expr {
        var e = try self.parsePrimary();
        while (true) {
            // Generic call: `id<i64>(…)` or `pkg.fn<i64>(…)` — `<` after ident/field before `(`.
            var pending_targs: []*ast.TypeExpr = &.{};
            if ((e.* == .ident or e.* == .field) and self.tok.kind == .lt) {
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
                // `size(T)` / `align(T)` take a type, including `Slot<K,V>` — not a value expr
                // (`Slot < K` would parse as comparison). Encode the type as call type_args.
                const size_align = e.* == .ident and (std.mem.eql(u8, e.ident, "size") or std.mem.eql(u8, e.ident, "align") or std.mem.eql(u8, e.ident, "typeInfo"));
                if (size_align and pending_targs.len == 0) {
                    self.advance();
                    const ty = try self.parseTypeExpr();
                    try self.expect(.rparen);
                    var targs: std.ArrayListUnmanaged(*ast.TypeExpr) = .{};
                    try targs.append(self.allocator, ty);
                    const call = try self.allocExpr();
                    call.* = .{ .call = .{
                        .callee = e,
                        .args = &.{},
                        .type_args = try targs.toOwnedSlice(self.allocator),
                    } };
                    e = call;
                    continue;
                }
                // `Shape.Circle(r: 2.0)` — field access + named fields → enum variant lit.
                // Empty `()` must stay a call (`n.greet()`); tag-only variants use
                // `Shape.Empty` / `.Empty` without parens (see field / primary paths).
                if (e.* == .field and e.field.base.* == .ident and pending_targs.len == 0) {
                    const save_enum = self.snapshot();
                    self.advance(); // (
                    if (self.tok.kind == .ident) {
                        const fname = self.tok.text;
                        self.advance();
                        if (self.tok.kind == .colon) {
                            self.advance();
                            var fields: std.ArrayListUnmanaged(ast.StructLitField) = .{};
                            try fields.append(self.allocator, .{ .name = fname, .value = try self.parseExpr() });
                            while (self.tok.kind == .comma) {
                                self.advance();
                                if (self.tok.kind == .rparen) break;
                                if (self.tok.kind != .ident) return error.UnexpectedToken;
                                const n = self.tok.text;
                                self.advance();
                                try self.expect(.colon);
                                try fields.append(self.allocator, .{ .name = n, .value = try self.parseExpr() });
                            }
                            try self.expect(.rparen);
                            const el = try self.allocExpr();
                            el.* = .{ .enum_lit = .{
                                .type_name = e.field.base.ident,
                                .variant = e.field.name,
                                .fields = try fields.toOwnedSlice(self.allocator),
                            } };
                            e = el;
                            continue;
                        }
                    }
                    self.restore(save_enum);
                }
                self.advance();
                const parsed_args = try self.parseArgList(.rparen);
                if (self.tok.kind != .rparen) return error.UnexpectedToken;
                const rparen_line = self.tok.line;
                self.advance();
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
                            if (self.tok.kind == .rparen) break;
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
                const call = try self.allocExpr();
                call.* = .{ .call = .{
                    .callee = e,
                    .args = parsed_args.args,
                    .arg_names = parsed_args.names,
                    .with_args = with_args,
                    .type_args = pending_targs,
                } };
                e = call;
                // Trailing lambda on the same line as the call's `)`.
                e = try self.maybeAttachTrailing(e, rparen_line);
                continue;
            }
            if (pending_targs.len != 0) return error.UnexpectedToken;
            // Bare trailing lambda: `items.each { … }` / `twiceDo { … }` (same line).
            if (self.allow_trailing and self.tok.kind == .lbrace) {
                const base_line = self.file.spanOf(e).line;
                if (self.tok.line == base_line) {
                    e = try self.attachTrailing(e);
                    continue;
                }
            }
            if (self.tok.kind == .lbracket) {
                // `a[i]` index, or `a[..]` / `a[i..j]` / `a[i..]` / `a[..j]` slice.
                // Bounds use parseCoalesce so `..` is not eaten as a range expr.
                self.advance();
                if (self.tok.kind == .dotdot) {
                    self.advance();
                    var end: ?*ast.Expr = null;
                    if (self.tok.kind != .rbracket) end = try self.parseCoalesce();
                    try self.expect(.rbracket);
                    const sl = try self.allocExpr();
                    sl.* = .{ .slice = .{ .base = e, .start = null, .end = end } };
                    e = sl;
                    continue;
                }
                const mid = try self.parseCoalesce();
                if (self.tok.kind == .dotdot) {
                    self.advance();
                    var end: ?*ast.Expr = null;
                    if (self.tok.kind != .rbracket) end = try self.parseCoalesce();
                    try self.expect(.rbracket);
                    const sl = try self.allocExpr();
                    sl.* = .{ .slice = .{ .base = e, .start = mid, .end = end } };
                    e = sl;
                    continue;
                }
                try self.expect(.rbracket);
                const ix = try self.allocExpr();
                ix.* = .{ .index = .{ .base = e, .index = mid } };
                e = ix;
                continue;
            }
            if (self.tok.kind == .dot) {
                self.advance();
                // Tuple fields: `pair.0` / `pair.1` → synthetic `_0` / `_1`
                // (same names as lowered `__Tup*` structs).
                const name: []const u8 = blk: {
                    if (self.tok.kind == .int) {
                        const n = try std.fmt.allocPrint(self.allocator, "_{s}", .{self.tok.text});
                        self.advance();
                        break :blk n;
                    }
                    break :blk try self.parseNameToken();
                };
                const f = try self.allocExpr();
                f.* = .{ .field = .{ .base = e, .name = name } };
                e = f;
                continue;
            }
            if (self.tok.kind == .dot_q) {
                self.advance();
                const name = try self.parseNameToken();
                try self.expect(.lparen);
                var args: std.ArrayListUnmanaged(*ast.Expr) = .{};
                if (self.tok.kind != .rparen) {
                    while (true) {
                        try args.append(self.allocator, try self.parseExpr());
                        if (self.tok.kind == .comma) {
                            self.advance();
                            if (self.tok.kind == .rparen) break;
                            continue;
                        }
                        break;
                    }
                }
                try self.expect(.rparen);
                const oc = try self.allocExpr();
                oc.* = .{ .optional_call = .{ .base = e, .name = name, .args = try args.toOwnedSlice(self.allocator) } };
                e = oc;
                continue;
            }
            // `as` / `as?` are castExpr (above unary), not postfix — see parseCast.
            if (self.tok.kind == .kw_is) {
                self.advance();
                const ty = try self.parseTypeExpr();
                const c = try self.allocExpr();
                c.* = .{ .is_type = .{ .expr = e, .ty = ty } };
                e = c;
                continue;
            }
            if (self.tok.kind == .bang_bang) {
                self.advance();
                const fu = try self.allocExpr();
                fu.* = .{ .force_unwrap = e };
                e = fu;
                continue;
            }
            break;
        }
        return e;
    }

    fn parsePrimary(self: *Parser) ParseError!*ast.Expr {
        const e = try self.allocExpr();
        switch (self.tok.kind) {
            .kw_quote => {
                return try self.parseQuote(e);
            },
            .kw_when => {
                self.advance();
                // Scrutinee must not steal the arm-list `{` as a trailing lambda.
                const saved_trail = self.allow_trailing;
                self.allow_trailing = false;
                const scrutinee = try self.parseExpr();
                self.allow_trailing = saved_trail;
                try self.expect(.lbrace);
                self.skipSeps();
                var arms: std.ArrayListUnmanaged(ast.WhenArm) = .{};
                var else_body: ?*ast.Expr = null;
                while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
                    if (self.tok.kind == .kw_else) {
                        self.advance();
                        try self.expect(.arrow);
                        else_body = try self.parseExpr();
                        self.skipSeps();
                        continue;
                    }
                    if (self.tok.kind != .ident) return error.UnexpectedToken;
                    const pat = self.tok.text;
                    self.advance();
                    // Payload binds: `Circle(r)` / `Rect(w, h)` (positional; `_` allowed).
                    var bindings: [][]const u8 = &.{};
                    if (self.tok.kind == .lparen) {
                        self.advance();
                        var binds: std.ArrayListUnmanaged([]const u8) = .{};
                        if (self.tok.kind != .rparen) {
                            while (true) {
                                if (self.tok.kind != .ident) return error.UnexpectedToken;
                                try binds.append(self.allocator, self.tok.text);
                                self.advance();
                                if (self.tok.kind == .comma) {
                                    self.advance();
                                    if (self.tok.kind == .rparen) break;
                                    continue;
                                }
                                break;
                            }
                        }
                        try self.expect(.rparen);
                        bindings = try binds.toOwnedSlice(self.allocator);
                    }
                    try self.expect(.arrow);
                    const body = try self.parseExpr();
                    try arms.append(self.allocator, .{ .pattern = pat, .bindings = bindings, .body = body });
                    self.skipSeps();
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
                // JOG-222: `go { … }` / `go with (…) { … }` — `go(fn, arg)` stays a normal call.
                // JOG-242: `go f(x)` expression form (not `go(` which is the fn+arg builtin).
                if (std.mem.eql(u8, name, "go") and (self.tok.kind == .kw_with or self.tok.kind == .lbrace)) {
                    var with_args: []*ast.Expr = &.{};
                    if (self.tok.kind == .kw_with) {
                        self.advance();
                        try self.expect(.lparen);
                        var wargs: std.ArrayListUnmanaged(*ast.Expr) = .{};
                        if (self.tok.kind != .rparen) {
                            while (true) {
                                try wargs.append(self.allocator, try self.parseExpr());
                                if (self.tok.kind == .comma) {
                                    self.advance();
                                    if (self.tok.kind == .rparen) break;
                                    continue;
                                }
                                break;
                            }
                        }
                        try self.expect(.rparen);
                        with_args = try wargs.toOwnedSlice(self.allocator);
                    }
                    const body = try self.parseBlock();
                    e.* = .{ .go_block = .{ .with_args = with_args, .body = body } };
                    return e;
                }
                if (std.mem.eql(u8, name, "go") and self.tok.kind != .lparen and self.tok.kind != .lt and
                    self.tok.kind != .dot and self.tok.kind != .lbracket and
                    (self.tok.kind == .ident or self.tok.kind == .lparen or self.tok.kind == .int or
                        self.tok.kind == .float or self.tok.kind == .string or self.tok.kind == .kw_true or
                        self.tok.kind == .kw_false))
                {
                    const inner = try self.parseUnary();
                    e.* = .{ .go_expr = inner };
                    return e;
                }
                // JOG-222: `scope { … }` (trailing-lambda `scope()` waits on JOG-221).
                if (std.mem.eql(u8, name, "scope") and self.tok.kind == .lbrace) {
                    const body = try self.parseBlock();
                    e.* = .{ .scope_block = .{ .body = body } };
                    return e;
                }
                // JOG-242: `select { … }` — cases are recv/send/timeout/default calls.
                if (std.mem.eql(u8, name, "select") and self.tok.kind == .lbrace) {
                    const body = try self.parseBlock();
                    e.* = .{ .select_block = .{ .body = body } };
                    return e;
                }
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
                    // Name() with no args stays a call; lower/check turn it into a
                    // zero struct value when Name is a struct type (StrBuilder(), etc.).
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
                                    if (self.tok.kind == .rparen) break;
                                    continue;
                                }
                                break;
                            }
                        }
                        try self.expect(.rparen);
                        // `name` was captured before type-args / `(…)` — reuse primary span.
                        const sp = self.file.spanOf(e);
                        const callee = try self.allocExprAt(sp.line, sp.col);
                        callee.* = .{ .ident = name };
                        e.* = .{ .call = .{
                            .callee = callee,
                            .args = try args.toOwnedSlice(self.allocator),
                            .type_args = type_args,
                        } };
                        return e;
                    }
                } else if (type_args.len != 0) {
                    // `Chan<i64>.init(…)` — type value prefix before `.`.
                    if (self.tok.kind == .dot) {
                        e.* = .{ .type_value = .{ .name = name, .args = type_args } };
                        return e;
                    }
                    return error.UnexpectedToken;
                }
                e.* = .{ .ident = name };
                return e;
            },
            .int => {
                const text = self.tok.text;
                self.advance();
                // i128 so u64-range magnitudes (e.g. 2^63 for i64-min) parse.
                // Values outside signed i128 still error.Overflow.
                const n: i128 = if (text.len >= 2 and text[0] == '0' and (text[1] == 'x' or text[1] == 'X'))
                    try std.fmt.parseInt(i128, text[2..], 16)
                else
                    try std.fmt.parseInt(i128, text, 10);
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
            // `.Empty` — enum variant with type filled in by check from context.
            .dot => {
                self.advance();
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const vname = self.tok.text;
                self.advance();
                var fields: []ast.StructLitField = &.{};
                if (self.tok.kind == .lparen) {
                    self.advance();
                    if (self.tok.kind != .rparen) {
                        if (self.tok.kind != .ident) return error.UnexpectedToken;
                        const fname = self.tok.text;
                        self.advance();
                        try self.expect(.colon);
                        var flist: std.ArrayListUnmanaged(ast.StructLitField) = .{};
                        try flist.append(self.allocator, .{ .name = fname, .value = try self.parseExpr() });
                        while (self.tok.kind == .comma) {
                            self.advance();
                            if (self.tok.kind == .rparen) break;
                            if (self.tok.kind != .ident) return error.UnexpectedToken;
                            const n = self.tok.text;
                            self.advance();
                            try self.expect(.colon);
                            try flist.append(self.allocator, .{ .name = n, .value = try self.parseExpr() });
                        }
                        fields = try flist.toOwnedSlice(self.allocator);
                    }
                    try self.expect(.rparen);
                }
                e.* = .{ .enum_lit = .{ .type_name = null, .variant = vname, .fields = fields } };
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
                // Grammar: at `(`, scan to matching `)`; if next is `:` or `->`, parse a lambda.
                if (self.lookaheadLambda()) {
                    return try self.parseLambda(e);
                }
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

    const ParsedArgs = struct {
        args: []*ast.Expr,
        names: []const ?[]const u8,
    };

    /// Parse `arg { "," arg }` until `end_kind` (not consuming it).
    /// `arg = [ IDENT ":" ] expr` — named only when IDENT is followed by `:`.
    fn parseArgList(self: *Parser, end_kind: lex.TokenKind) ParseError!ParsedArgs {
        var args: std.ArrayListUnmanaged(*ast.Expr) = .{};
        var names: std.ArrayListUnmanaged(?[]const u8) = .{};
        var any_named = false;
        if (self.tok.kind != end_kind) {
            while (true) {
                var name: ?[]const u8 = null;
                if (self.tok.kind == .ident) {
                    const save = self.snapshot();
                    const n = self.tok.text;
                    self.advance();
                    if (self.tok.kind == .colon) {
                        self.advance();
                        name = n;
                        any_named = true;
                    } else {
                        self.restore(save);
                    }
                }
                try args.append(self.allocator, try self.parseExpr());
                try names.append(self.allocator, name);
                if (self.tok.kind == .comma) {
                    self.advance();
                    if (self.tok.kind == end_kind) break;
                    continue;
                }
                break;
            }
        }
        const owned_args = try args.toOwnedSlice(self.allocator);
        if (!any_named) {
            return .{ .args = owned_args, .names = &.{} };
        }
        return .{ .args = owned_args, .names = try names.toOwnedSlice(self.allocator) };
    }

    /// `quote { … }` with `${expr}` splices. Body is kept as source text plus splice ASTs
    /// so expand can re-parse after substitution (JOG-228).
    fn parseQuote(self: *Parser, e: *ast.Expr) ParseError!*ast.Expr {
        try self.expect(.kw_quote);
        if (self.tok.kind != .lbrace) return error.UnexpectedToken;
        // `lx.i` is already past `{`. Do not `advance()` first — that would skipTrivia
        // and consume `fn` before the raw scan starts.
        const src = self.lx.src;
        var i = self.lx.i;
        var depth: i32 = 1;
        var parts: std.ArrayListUnmanaged(ast.QuotePart) = .{};
        var text_start = i;
        while (i < src.len and depth > 0) {
            const c = src[i];
            // Strings: skip so braces/`${` inside literals are not special.
            if (c == '"') {
                i += 1;
                while (i < src.len and src[i] != '"') {
                    if (src[i] == '\\' and i + 1 < src.len) i += 1;
                    i += 1;
                }
                if (i < src.len) i += 1;
                continue;
            }
            if (c == '/' and i + 1 < src.len and src[i + 1] == '/') {
                i += 2;
                while (i < src.len and src[i] != '\n') i += 1;
                continue;
            }
            if (c == '/' and i + 1 < src.len and src[i + 1] == '*') {
                i += 2;
                while (i + 1 < src.len and !(src[i] == '*' and src[i + 1] == '/')) i += 1;
                if (i + 1 < src.len) i += 2;
                continue;
            }
            if (c == '$' and i + 1 < src.len and src[i + 1] == '{') {
                if (i > text_start) {
                    try parts.append(self.allocator, .{ .text = src[text_start..i] });
                }
                i += 2; // skip `${`
                // Parse splice expression from a nested parser on the remaining source.
                var nested = Parser{
                    .allocator = self.allocator,
                    .lx = .{
                        .src = src,
                        .i = i,
                        .line = self.lx.line,
                        .col = self.lx.col,
                    },
                    .tok = undefined,
                    .file = self.file,
                    .allow_trailing = self.allow_trailing,
                };
                nested.tok = nested.lx.next();
                const splice = try nested.parseExpr();
                if (nested.tok.kind != .rbrace) return error.UnexpectedToken;
                // Resume raw scan at the byte after this `}` — do not `advance()` or the
                // following ident (e.g. `_gen` in `${target.name}_gen`) is skipped.
                const rbrace_off = @intFromPtr(nested.tok.text.ptr) - @intFromPtr(src.ptr);
                i = rbrace_off + nested.tok.text.len;
                try parts.append(self.allocator, .{ .splice = splice });
                self.lx.line = nested.lx.line;
                self.lx.col = nested.lx.col;
                text_start = i;
                continue;
            }
            if (c == '{') depth += 1;
            if (c == '}') {
                depth -= 1;
                if (depth == 0) {
                    if (i > text_start) {
                        try parts.append(self.allocator, .{ .text = src[text_start..i] });
                    }
                    i += 1; // consume closing `}`
                    break;
                }
            }
            i += 1;
        }
        if (depth != 0) return error.UnexpectedToken;
        // Resync the main lexer past the quote body.
        self.lx.i = i;
        self.tok = self.lx.next();
        e.* = .{ .quote = .{ .parts = try parts.toOwnedSlice(self.allocator) } };
        return e;
    }

    /// True when `(…)` is followed by `:` or `->` (lambda, not grouping/tuple).
    fn lookaheadLambda(self: *Parser) bool {
        if (self.tok.kind != .lparen) return false;
        // `self.lx` is already past the current `(`.
        var tmp = self.lx;
        var depth: i32 = 1;
        while (true) {
            const t = tmp.next();
            if (t.kind == .eof) return false;
            if (t.kind == .lparen) depth += 1;
            if (t.kind == .rparen) {
                depth -= 1;
                if (depth == 0) {
                    const after = tmp.next();
                    return after.kind == .colon or after.kind == .arrow;
                }
            }
        }
    }

    fn parseLambda(self: *Parser, e: *ast.Expr) ParseError!*ast.Expr {
        try self.expect(.lparen);
        var params: std.ArrayListUnmanaged(ast.Param) = .{};
        if (self.tok.kind != .rparen) {
            while (true) {
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                var ty: ?*ast.TypeExpr = null;
                if (self.tok.kind == .colon) {
                    self.advance();
                    ty = try self.parseTypeExpr();
                }
                const pty = ty orelse blk: {
                    const t = try self.allocator.create(ast.TypeExpr);
                    t.* = .{ .name = "_" }; // inferred later
                    break :blk t;
                };
                try params.append(self.allocator, .{ .name = name, .ty = pty });
                if (self.tok.kind == .comma) {
                    self.advance();
                    if (self.tok.kind == .rparen) break;
                    continue;
                }
                break;
            }
        }
        try self.expect(.rparen);
        var ret: ?*ast.TypeExpr = null;
        if (self.tok.kind == .colon) {
            self.advance();
            ret = try self.parseTypeExpr();
        }
        try self.expect(.arrow);
        var expr_body: ?*ast.Expr = null;
        var block: ?*ast.Block = null;
        if (self.tok.kind == .lbrace) {
            block = try self.parseBlock();
        } else {
            expr_body = try self.parseExpr();
        }
        e.* = .{ .lambda = .{
            .params = try params.toOwnedSlice(self.allocator),
            .ret = ret,
            .expr_body = expr_body,
            .block = block,
        } };
        return e;
    }

    fn maybeAttachTrailing(self: *Parser, call_or_expr: *ast.Expr, same_line: u32) ParseError!*ast.Expr {
        if (!self.allow_trailing) return call_or_expr;
        if (self.tok.kind != .lbrace) return call_or_expr;
        if (self.tok.line != same_line) return call_or_expr;
        return try self.attachTrailing(call_or_expr);
    }

    /// Attach `{ [params ->] stmts }` as the last argument of a call (creating a call if needed).
    fn attachTrailing(self: *Parser, node: *ast.Expr) ParseError!*ast.Expr {
        const lam = try self.parseTrailingLambda();
        if (node.* == .call) {
            var args: std.ArrayListUnmanaged(*ast.Expr) = .{};
            try args.appendSlice(self.allocator, node.call.args);
            try args.append(self.allocator, lam);
            var names: std.ArrayListUnmanaged(?[]const u8) = .{};
            if (node.call.arg_names.len > 0) {
                try names.appendSlice(self.allocator, node.call.arg_names);
                try names.append(self.allocator, null);
            }
            node.call.args = try args.toOwnedSlice(self.allocator);
            if (names.items.len > 0) node.call.arg_names = try names.toOwnedSlice(self.allocator);
            return node;
        }
        const call = try self.allocExpr();
        const args = try self.allocator.alloc(*ast.Expr, 1);
        args[0] = lam;
        call.* = .{ .call = .{ .callee = node, .args = args } };
        return call;
    }

    /// `{ body }` / `{ x -> body }` / `{ a, b -> body }` on a trailing-lambda `{`.
    fn parseTrailingLambda(self: *Parser) ParseError!*ast.Expr {
        const e = try self.allocExpr();
        try self.expect(.lbrace);
        var params: std.ArrayListUnmanaged(ast.Param) = .{};
        var implicit_it = true;
        if (self.atTrailingLambdaParams()) {
            implicit_it = false;
            while (true) {
                if (self.tok.kind != .ident) return error.UnexpectedToken;
                const name = self.tok.text;
                self.advance();
                const ty = try self.allocator.create(ast.TypeExpr);
                ty.* = .{ .name = "_" };
                try params.append(self.allocator, .{ .name = name, .ty = ty });
                if (self.tok.kind == .comma) {
                    self.advance();
                    continue;
                }
                break;
            }
            try self.expect(.arrow);
        }
        // Rest of the block body (statements until `}`).
        var stmts: std.ArrayListUnmanaged(ast.Stmt) = .{};
        while (self.tok.kind != .rbrace and self.tok.kind != .eof) {
            try stmts.append(self.allocator, try self.parseStmt());
            if (self.tok.kind == .semicolon) self.advance();
        }
        try self.expect(.rbrace);
        const block = try self.allocator.create(ast.Block);
        block.* = .{ .stmts = try stmts.toOwnedSlice(self.allocator) };
        if (implicit_it) {
            // Placeholder; check may clear this when the expected type is `() -> R`.
            const ty = try self.allocator.create(ast.TypeExpr);
            ty.* = .{ .name = "_" };
            try params.append(self.allocator, .{ .name = "it", .ty = ty });
        }
        e.* = .{ .lambda = .{
            .params = try params.toOwnedSlice(self.allocator),
            .block = block,
            .implicit_it = implicit_it,
        } };
        return e;
    }

    /// `x ->` or `x, y ->` at the start of a trailing-lambda body.
    fn atTrailingLambdaParams(self: *Parser) bool {
        if (self.tok.kind != .ident) return false;
        const save = self.snapshot();
        defer self.restore(save);
        self.advance();
        while (self.tok.kind == .comma) {
            self.advance();
            if (self.tok.kind != .ident) return false;
            self.advance();
        }
        return self.tok.kind == .arrow;
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
                    .file = self.file,
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

test "parse threadLocal var" {
    const src =
        \\@threadLocal var tls_x: i64 = 0
        \\
        \\fn main() {}
        \\
    ;
    var file = try Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    try std.testing.expectEqual(@as(usize, 2), file.decls.len);
    const gd = file.decls[0].global_decl;
    try std.testing.expect(gd.mutable);
    try std.testing.expect(gd.thread_local);
    try std.testing.expectEqualStrings("tls_x", gd.name);
}

test "parse scope and go blocks" {
    const src =
        \\fn main() {
        \\    scope {
        \\        go {
        \\            print("x\n")
        \\        }
        \\        go with (1) {
        \\            print("y\n")
        \\        }
        \\    }
        \\}
    ;
    var file = try Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    try std.testing.expect(file.decls.len == 1);
    const body = file.decls[0].fn_decl.body.?;
    try std.testing.expect(body.stmts.len == 1);
    try std.testing.expect(body.stmts[0] == .expr);
    try std.testing.expect(body.stmts[0].expr.* == .scope_block);
    const sb = body.stmts[0].expr.scope_block;
    try std.testing.expect(sb.body.stmts.len == 2);
    try std.testing.expect(sb.body.stmts[0].expr.* == .go_block);
    try std.testing.expect(sb.body.stmts[1].expr.* == .go_block);
    try std.testing.expect(sb.body.stmts[1].expr.go_block.with_args.len == 1);
}

test "parse enum payload variants" {
    const src =
        \\enum Shape {
        \\    Circle(r: f64)
        \\    Rect(w: f64, h: f64)
        \\    Empty
        \\}
        \\
    ;
    var file = try Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    try std.testing.expectEqual(@as(usize, 1), file.decls.len);
    const ed = file.decls[0].enum_decl;
    try std.testing.expectEqualStrings("Shape", ed.name);
    try std.testing.expectEqual(@as(usize, 3), ed.variants.len);
    try std.testing.expectEqualStrings("Circle", ed.variants[0].name);
    try std.testing.expectEqual(@as(usize, 1), ed.variants[0].fields.len);
    try std.testing.expectEqualStrings("r", ed.variants[0].fields[0].name);
    try std.testing.expectEqual(@as(usize, 2), ed.variants[1].fields.len);
    try std.testing.expectEqual(@as(usize, 0), ed.variants[2].fields.len);
}

test "parse enum lit and when bindings" {
    const src =
        \\enum Shape { Circle(r: i64), Empty }
        \\fn f(s: Shape): i64 {
        \\    val x = Shape.Circle(r: 2)
        \\    val e: Shape = .Empty
        \\    return when s {
        \\        Circle(r) -> r
        \\        Empty -> 0
        \\    }
        \\}
        \\
    ;
    var file = try Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    try std.testing.expect(file.decls.len >= 2);
}

test "parse quote with splice" {
    const src =
        \\fn main() {
        \\    val x = quote {
        \\        fn ${target.name}_gen(): i64 {
        \\            return 42
        \\        }
        \\    }
        \\}
    ;
    var file = try Parser.parseFile(std.testing.allocator, src);
    defer file.deinit();
    const body = file.decls[0].fn_decl.body.?;
    const init = body.stmts[0].var_decl.init.?;
    try std.testing.expect(init.* == .quote);
    const q = init.quote;
    try std.testing.expect(q.parts.len >= 2);
    // First text should contain "fn "
    var found_fn = false;
    var found_suffix = false;
    for (q.parts) |p| {
        switch (p) {
            .text => |t| {
                if (std.mem.indexOf(u8, t, "fn ") != null) found_fn = true;
                if (std.mem.indexOf(u8, t, "_gen") != null) found_suffix = true;
            },
            .splice => {},
        }
    }
    try std.testing.expect(found_fn);
    try std.testing.expect(found_suffix);
}
