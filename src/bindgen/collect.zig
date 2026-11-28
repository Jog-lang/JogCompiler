//! Walk a libclang translation unit into a bindgen Unit.

const std = @import("std");
const clang = @import("clang.zig");
const model = @import("model.zig");
const c = clang.c;

const Collector = struct {
    unit: *model.Unit,
    opts: model.Options,
    /// Type names already emitted (structs/enums/typedefs).
    seen: std.StringHashMapUnmanaged(void) = .{},
    /// Record USRs / names we should force-emit when referenced.
    wanted_types: std.StringHashMapUnmanaged(void) = .{},

    fn matchesPrefix(self: *Collector, name: []const u8) bool {
        if (self.opts.prefix.len == 0) return true;
        return std.mem.startsWith(u8, name, self.opts.prefix);
    }

    fn markSeen(self: *Collector, name: []const u8) !bool {
        // Own the key — callers often pass temporaries.
        if (self.seen.contains(name)) return false;
        const key = try self.unit.allocator.dupe(u8, name);
        errdefer self.unit.allocator.free(key);
        try self.seen.put(self.unit.allocator, key, {});
        try self.unit.owned_strings.append(self.unit.allocator, key);
        return true;
    }

    fn skip(self: *Collector, name: []const u8, reason: []const u8) !void {
        const n = try self.unit.dupe(name);
        const r = try self.unit.dupe(reason);
        try self.unit.skipped.append(self.unit.allocator, .{ .name = n, .reason = r });
    }
};

pub fn collectFromHeader(allocator: std.mem.Allocator, opts: model.Options) !model.Unit {
    var unit: model.Unit = .{ .allocator = allocator };
    errdefer unit.deinit();

    var coll: Collector = .{ .unit = &unit, .opts = opts };
    defer {
        coll.seen.deinit(allocator);
        coll.wanted_types.deinit(allocator);
    }

    const index = c.clang_createIndex(0, 0);
    defer c.clang_disposeIndex(index);

    var argv: std.ArrayListUnmanaged([*:0]const u8) = .{};
    defer argv.deinit(allocator);

    // Parse as C.
    try argv.append(allocator, "-x");
    try argv.append(allocator, "c");
    try argv.append(allocator, "-std=c17");

    for (opts.includes) |inc| {
        const flag = try std.fmt.allocPrintZ(allocator, "-I{s}", .{inc});
        try unit.owned_strings.append(allocator, flag);
        try argv.append(allocator, flag.ptr);
    }
    for (opts.defines) |d| {
        const flag = try std.fmt.allocPrintZ(allocator, "-D{s}", .{d});
        try unit.owned_strings.append(allocator, flag);
        try argv.append(allocator, flag.ptr);
    }

    // Build a tiny wrapper so the user's header is the primary include.
    // Using the header path directly also works; prefer absolute when possible.
    const header_z = try allocator.dupeZ(u8, opts.header);
    defer allocator.free(header_z);

    const tu_flags: c_uint = c.CXTranslationUnit_DetailedPreprocessingRecord |
        c.CXTranslationUnit_SkipFunctionBodies |
        c.CXTranslationUnit_KeepGoing;

    var tu: c.CXTranslationUnit = undefined;
    const rc = c.clang_parseTranslationUnit2(
        index,
        header_z.ptr,
        if (argv.items.len > 0) @ptrCast(argv.items.ptr) else null,
        @intCast(argv.items.len),
        null,
        0,
        tu_flags,
        &tu,
    );
    if (rc != c.CXError_Success) {
        return error.ParseFailed;
    }
    defer c.clang_disposeTranslationUnit(tu);

    // Surface fatal diagnostics on stderr; keep going for warnings.
    const n_diag = c.clang_getNumDiagnostics(tu);
    var i: c_uint = 0;
    while (i < n_diag) : (i += 1) {
        const diag = c.clang_getDiagnostic(tu, i);
        defer c.clang_disposeDiagnostic(diag);
        const sev = c.clang_getDiagnosticSeverity(diag);
        if (sev < c.CXDiagnostic_Error) continue;
        const msg = c.clang_formatDiagnostic(diag, c.clang_defaultDiagnosticDisplayOptions());
        defer clang.disposeString(msg);
        std.debug.print("bindgen: {s}\n", .{clang.cString(msg)});
    }

    const cursor = c.clang_getTranslationUnitCursor(tu);
    _ = c.clang_visitChildren(cursor, visitTop, &coll);

    return unit;
}

fn visitTop(cursor: c.CXCursor, parent: c.CXCursor, client: c.CXClientData) callconv(.C) c.CXChildVisitResult {
    _ = parent;
    const coll: *Collector = @ptrCast(@alignCast(client));
    visitTopInner(coll, cursor) catch {};
    return c.CXChildVisit_Continue;
}

fn visitTopInner(coll: *Collector, cursor: c.CXCursor) !void {
    if (c.clang_Location_isInSystemHeader(c.clang_getCursorLocation(cursor)) != 0) {
        return;
    }

    const kind = c.clang_getCursorKind(cursor);
    switch (kind) {
        c.CXCursor_FunctionDecl => try visitFunction(coll, cursor),
        c.CXCursor_StructDecl, c.CXCursor_UnionDecl => try visitRecord(coll, cursor),
        c.CXCursor_EnumDecl => try visitEnum(coll, cursor),
        c.CXCursor_TypedefDecl => try visitTypedef(coll, cursor),
        c.CXCursor_MacroDefinition => try visitMacro(coll, cursor),
        else => {},
    }
}

fn cursorName(coll: *Collector, cursor: c.CXCursor) ![]const u8 {
    const s = c.clang_getCursorSpelling(cursor);
    return clang.spellingOwned(coll.unit.allocator, s);
}

fn visitFunction(coll: *Collector, cursor: c.CXCursor) !void {
    // Only definitions/declarations with external linkage.
    const linkage = c.clang_getCursorLinkage(cursor);
    if (linkage == c.CXLinkage_Internal) {
        const name = try cursorName(coll, cursor);
        defer coll.unit.allocator.free(name);
        if (coll.matchesPrefix(name)) {
            try coll.skip(name, "static / internal linkage");
        }
        return;
    }

    // Inline functions cannot be translated into extern decls.
    if (c.clang_Cursor_isFunctionInlined(cursor) != 0) {
        const name = try cursorName(coll, cursor);
        defer coll.unit.allocator.free(name);
        if (name.len > 0 and coll.matchesPrefix(name)) {
            try coll.skip(name, "inline function");
        }
        return;
    }

    const name_owned = try cursorName(coll, cursor);
    if (name_owned.len == 0 or !coll.matchesPrefix(name_owned)) {
        coll.unit.allocator.free(name_owned);
        return;
    }
    const name = try coll.unit.dupe(name_owned);
    coll.unit.allocator.free(name_owned);

    if (!try coll.markSeen(name)) return;

    const ty = c.clang_getCursorType(cursor);
    const n_args = c.clang_Cursor_getNumArguments(cursor);
    var params: std.ArrayListUnmanaged(model.Param) = .{};
    errdefer params.deinit(coll.unit.allocator);

    var ai: c_uint = 0;
    while (ai < n_args) : (ai += 1) {
        const arg = c.clang_Cursor_getArgument(cursor, ai);
        var pname = try cursorName(coll, arg);
        if (pname.len == 0) {
            coll.unit.allocator.free(pname);
            pname = try std.fmt.allocPrint(coll.unit.allocator, "arg{d}", .{ai});
        }
        const pname_owned = try coll.unit.dupe(pname);
        coll.unit.allocator.free(pname);
        const aty = c.clang_getCursorType(arg);
        const jog_ty = try typeToJog(coll, aty, false);
        try params.append(coll.unit.allocator, .{ .name = pname_owned, .ty = jog_ty });
    }

    const variadic = c.clang_isFunctionTypeVariadic(ty) != 0;
    // Prefer the type as written on the function (keeps size_t typedefs).
    const result = c.clang_getResultType(ty);
    const ret: ?model.JogType = if (result.kind == c.CXType_Void) null else try typeToJog(coll, result, true);

    try coll.unit.fns.append(coll.unit.allocator, .{
        .name = name,
        .params = try params.toOwnedSlice(coll.unit.allocator),
        .ret = ret,
        .variadic = variadic,
    });
}

fn visitRecord(coll: *Collector, cursor: c.CXCursor) !void {
    if (c.clang_isCursorDefinition(cursor) == 0) {
        // Forward declaration — emit opaque if named and matching.
        const name_tmp = try cursorName(coll, cursor);
        defer coll.unit.allocator.free(name_tmp);
        if (name_tmp.len == 0 or !coll.matchesPrefix(name_tmp)) return;
        if (!try coll.markSeen(name_tmp)) return;
        const name = try coll.unit.dupe(name_tmp);
        try coll.unit.structs.append(coll.unit.allocator, .{
            .name = name,
            .fields = &.{},
            .opaque_only = true,
            .is_union = c.clang_getCursorKind(cursor) == c.CXCursor_UnionDecl,
        });
        return;
    }

    const name_tmp = try cursorName(coll, cursor);
    defer coll.unit.allocator.free(name_tmp);
    if (name_tmp.len == 0) {
        // Anonymous record — skip at top level; fields may appear via typedef.
        return;
    }
    if (!coll.matchesPrefix(name_tmp)) return;
    if (!try coll.markSeen(name_tmp)) return;
    const name = try coll.unit.dupe(name_tmp);

    var fields: std.ArrayListUnmanaged(model.Field) = .{};
    errdefer fields.deinit(coll.unit.allocator);

    const FieldCtx = struct {
        coll: *Collector,
        fields: *std.ArrayListUnmanaged(model.Field),
    };
    var fctx: FieldCtx = .{ .coll = coll, .fields = &fields };

    const FieldVisitor = struct {
        fn visit(cur: c.CXCursor, parent: c.CXCursor, client: c.CXClientData) callconv(.C) c.CXChildVisitResult {
            _ = parent;
            const ctx: *FieldCtx = @ptrCast(@alignCast(client));
            if (c.clang_getCursorKind(cur) != c.CXCursor_FieldDecl) return c.CXChildVisit_Continue;
            visitField(ctx.coll, ctx.fields, cur) catch {};
            return c.CXChildVisit_Continue;
        }
    };
    _ = c.clang_visitChildren(cursor, FieldVisitor.visit, &fctx);

    const is_union = c.clang_getCursorKind(cursor) == c.CXCursor_UnionDecl;
    const opaque_only = fields.items.len == 0;
    try coll.unit.structs.append(coll.unit.allocator, .{
        .name = name,
        .fields = try fields.toOwnedSlice(coll.unit.allocator),
        .opaque_only = opaque_only,
        .is_union = is_union,
    });
}

fn visitField(coll: *Collector, fields: *std.ArrayListUnmanaged(model.Field), cursor: c.CXCursor) !void {
    const name_tmp = try cursorName(coll, cursor);
    defer coll.unit.allocator.free(name_tmp);
    if (name_tmp.len == 0) return; // anonymous bitfield / padding
    const name = try coll.unit.dupe(name_tmp);
    const ty = try typeToJog(coll, c.clang_getCursorType(cursor), false);
    try fields.append(coll.unit.allocator, .{ .name = name, .ty = ty });
}

fn visitEnum(coll: *Collector, cursor: c.CXCursor) !void {
    if (c.clang_isCursorDefinition(cursor) == 0) return;
    const name_tmp = try cursorName(coll, cursor);
    defer coll.unit.allocator.free(name_tmp);
    if (name_tmp.len == 0 or !coll.matchesPrefix(name_tmp)) return;
    if (!try coll.markSeen(name_tmp)) return;
    const name = try coll.unit.dupe(name_tmp);

    var variants: std.ArrayListUnmanaged(model.EnumVariant) = .{};
    errdefer variants.deinit(coll.unit.allocator);

    const Ctx = struct {
        coll: *Collector,
        variants: *std.ArrayListUnmanaged(model.EnumVariant),
    };
    var ctx: Ctx = .{ .coll = coll, .variants = &variants };
    const V = struct {
        fn visit(cur: c.CXCursor, parent: c.CXCursor, client: c.CXClientData) callconv(.C) c.CXChildVisitResult {
            _ = parent;
            const cctx: *Ctx = @ptrCast(@alignCast(client));
            if (c.clang_getCursorKind(cur) != c.CXCursor_EnumConstantDecl) return c.CXChildVisit_Continue;
            const n = clang.spellingOwned(cctx.coll.unit.allocator, c.clang_getCursorSpelling(cur)) catch return c.CXChildVisit_Continue;
            defer cctx.coll.unit.allocator.free(n);
            const owned = cctx.coll.unit.dupe(n) catch return c.CXChildVisit_Continue;
            const val = c.clang_getEnumConstantDeclValue(cur);
            cctx.variants.append(cctx.coll.unit.allocator, .{ .name = owned, .value = val }) catch {};
            return c.CXChildVisit_Continue;
        }
    };
    _ = c.clang_visitChildren(cursor, V.visit, &ctx);

    try coll.unit.enums.append(coll.unit.allocator, .{
        .name = name,
        .variants = try variants.toOwnedSlice(coll.unit.allocator),
    });
}

fn visitTypedef(coll: *Collector, cursor: c.CXCursor) !void {
    const name_tmp = try cursorName(coll, cursor);
    defer coll.unit.allocator.free(name_tmp);
    if (name_tmp.len == 0 or !coll.matchesPrefix(name_tmp)) return;

    const under = c.clang_getTypedefDeclUnderlyingType(cursor);
    const canon = c.clang_getCanonicalType(under);

    // typedef struct Foo Foo — if the struct was already emitted under Foo, skip alias.
    // typedef struct { … } Name — emit the anonymous struct as Name.
    if (canon.kind == c.CXType_Record) {
        const decl = c.clang_getTypeDeclaration(canon);
        const tag_name = try cursorName(coll, decl);
        defer coll.unit.allocator.free(tag_name);
        if (tag_name.len == 0) {
            // Anonymous struct/union typedef — emit as named struct.
            if (!try coll.markSeen(name_tmp)) return;
            const name = try coll.unit.dupe(name_tmp);
            var fields: std.ArrayListUnmanaged(model.Field) = .{};
            errdefer fields.deinit(coll.unit.allocator);
            const FieldCtx = struct {
                coll: *Collector,
                fields: *std.ArrayListUnmanaged(model.Field),
            };
            var fctx: FieldCtx = .{ .coll = coll, .fields = &fields };
            const FieldVisitor = struct {
                fn visit(cur: c.CXCursor, parent: c.CXCursor, client: c.CXClientData) callconv(.C) c.CXChildVisitResult {
                    _ = parent;
                    const ctx: *FieldCtx = @ptrCast(@alignCast(client));
                    if (c.clang_getCursorKind(cur) != c.CXCursor_FieldDecl) return c.CXChildVisit_Continue;
                    visitField(ctx.coll, ctx.fields, cur) catch {};
                    return c.CXChildVisit_Continue;
                }
            };
            _ = c.clang_visitChildren(decl, FieldVisitor.visit, &fctx);
            try coll.unit.structs.append(coll.unit.allocator, .{
                .name = name,
                .fields = try fields.toOwnedSlice(coll.unit.allocator),
                .opaque_only = fields.items.len == 0,
                .is_union = c.clang_getCursorKind(decl) == c.CXCursor_UnionDecl,
            });
            return;
        }
        if (std.mem.eql(u8, tag_name, name_tmp)) {
            // typedef struct Name Name — struct visitor handles it.
            return;
        }
    }

    if (!try coll.markSeen(name_tmp)) return;
    const name = try coll.unit.dupe(name_tmp);
    const jog_ty = try typeToJog(coll, under, false);
    try coll.unit.typedefs.append(coll.unit.allocator, .{ .name = name, .ty = jog_ty });
}

fn visitMacro(coll: *Collector, cursor: c.CXCursor) !void {
    const name_tmp = try cursorName(coll, cursor);
    defer coll.unit.allocator.free(name_tmp);
    if (name_tmp.len == 0 or !coll.matchesPrefix(name_tmp)) return;
    // Skip compiler / system reserved macros and typical include guards with empty bodies.
    if (name_tmp[0] == '_') return;

    if (c.clang_Cursor_isMacroFunctionLike(cursor) != 0) {
        try coll.skip(name_tmp, "function-like macro");
        return;
    }

    if (!try coll.markSeen(name_tmp)) return;
    const name = try coll.unit.dupe(name_tmp);

    // Prefer Evaluate when it works; otherwise tokenize the definition body.
    const eval = c.clang_Cursor_Evaluate(cursor);
    if (eval != null) {
        defer c.clang_EvalResult_dispose(eval);
        const kind = c.clang_EvalResult_getKind(eval);
        switch (kind) {
            c.CXEval_Int => {
                const val = c.clang_EvalResult_getAsLongLong(eval);
                const unsigned = c.clang_EvalResult_isUnsignedInt(eval) != 0;
                const lit = if (unsigned)
                    try std.fmt.allocPrint(coll.unit.allocator, "{d}", .{@as(u64, @bitCast(val))})
                else
                    try std.fmt.allocPrint(coll.unit.allocator, "{d}", .{val});
                const lit_owned = try coll.unit.dupe(lit);
                coll.unit.allocator.free(lit);
                try coll.unit.consts.append(coll.unit.allocator, .{
                    .name = name,
                    .value = lit_owned,
                    .ty = if (unsigned) "u32" else "i32",
                });
                return;
            },
            c.CXEval_Float => {
                const val = c.clang_EvalResult_getAsDouble(eval);
                const lit = try std.fmt.allocPrint(coll.unit.allocator, "{d}", .{val});
                const lit_owned = try coll.unit.dupe(lit);
                coll.unit.allocator.free(lit);
                try coll.unit.consts.append(coll.unit.allocator, .{
                    .name = name,
                    .value = lit_owned,
                    .ty = "f64",
                });
                return;
            },
            c.CXEval_StrLiteral => {
                const p = c.clang_EvalResult_getAsStr(eval);
                if (p != null) {
                    const text = std.mem.span(p);
                    if (std.mem.indexOfAny(u8, text, "\"\\\n\r") == null) {
                        try coll.unit.consts.append(coll.unit.allocator, .{
                            .name = name,
                            .value = try coll.unit.dupe(text),
                            .ty = "cstr",
                        });
                        return;
                    }
                }
            },
            else => {},
        }
    }

    if (try macroBodyLiteral(coll, cursor)) |body| {
        try coll.unit.consts.append(coll.unit.allocator, .{
            .name = name,
            .value = body.value,
            .ty = body.ty,
        });
        return;
    }
    // Empty `#define GUARD` include guards — ignore silently.
    if (try macroTokenCount(cursor) <= 1) return;
    try coll.skip(name, "unevaluable object-like macro");
}

fn macroTokenCount(cursor: c.CXCursor) !c_uint {
    const tu = c.clang_Cursor_getTranslationUnit(cursor);
    const range = c.clang_getCursorExtent(cursor);
    var tokens: [*c]c.CXToken = undefined;
    var n: c_uint = 0;
    c.clang_tokenize(tu, range, &tokens, &n);
    defer c.clang_disposeTokens(tu, tokens, n);
    return n;
}

const MacroBody = struct { value: []const u8, ty: []const u8 };

/// Tokenize `#define NAME body` and accept a single int / hex / string literal body.
fn macroBodyLiteral(coll: *Collector, cursor: c.CXCursor) !?MacroBody {
    const tu = c.clang_Cursor_getTranslationUnit(cursor);
    const range = c.clang_getCursorExtent(cursor);
    var tokens: [*c]c.CXToken = undefined;
    var n: c_uint = 0;
    c.clang_tokenize(tu, range, &tokens, &n);
    defer c.clang_disposeTokens(tu, tokens, n);
    if (n < 2) return null;

    // tokens[0] is the macro name; body follows.
    const first_kind = c.clang_getTokenKind(tokens[1]);
    if (first_kind == c.CXToken_Punctuation) {
        const sp = c.clang_getTokenSpelling(tu, tokens[1]);
        defer clang.disposeString(sp);
        if (std.mem.eql(u8, clang.cString(sp), "(")) return null;
    }

    // Include guard: `#define FOO` with no body.
    if (n == 1) return null;
    if (n != 2) return null;
    const kind = c.clang_getTokenKind(tokens[1]);
    const sp = c.clang_getTokenSpelling(tu, tokens[1]);
    defer clang.disposeString(sp);
    const text = clang.cString(sp);
    if (kind != c.CXToken_Literal) return null;

    if (text.len >= 2 and text[0] == '"' and text[text.len - 1] == '"') {
        const inner = text[1 .. text.len - 1];
        if (std.mem.indexOfAny(u8, inner, "\"\\\n\r") != null) return null;
        return .{ .value = try coll.unit.dupe(inner), .ty = "cstr" };
    }
    // Integer / hex literal — keep spelling, strip trailing width suffixes.
    var lit = text;
    while (lit.len > 0) {
        const last = lit[lit.len - 1];
        if (last == 'u' or last == 'U' or last == 'l' or last == 'L') {
            lit = lit[0 .. lit.len - 1];
            continue;
        }
        break;
    }
    const unsigned = std.mem.indexOfScalar(u8, text, 'u') != null or std.mem.indexOfScalar(u8, text, 'U') != null;
    return .{
        .value = try coll.unit.dupe(lit),
        .ty = if (unsigned) "u32" else "i32",
    };
}

fn typeToJog(coll: *Collector, ty_in: c.CXType, as_return: bool) anyerror!model.JogType {
    _ = as_return;

    // Spelling often still says `size_t` / `uint32_t` even when kind is a builtin.
    {
        const sp = c.clang_getTypeSpelling(ty_in);
        defer clang.disposeString(sp);
        const text = clang.cString(sp);
        // Strip a trailing trailing "const " / leading qualifiers for the lookup key.
        var key = text;
        if (std.mem.startsWith(u8, key, "const ")) key = key["const ".len..];
        if (mapLibcTypedef(key)) |mapped| {
            return .{ .named = mapped };
        }
    }

    // Preserve well-known libc typedef names (size_t, int32_t, …) before canonicalizing
    // away the typedef to a bare builtin.
    var walk = ty_in;
    while (walk.kind == c.CXType_Typedef) {
        const td_name_s = c.clang_getTypedefName(walk);
        defer clang.disposeString(td_name_s);
        const td_name = clang.cString(td_name_s);
        if (mapLibcTypedef(td_name)) |mapped| {
            return .{ .named = mapped };
        }
        const decl = c.clang_getTypeDeclaration(walk);
        walk = c.clang_getTypedefDeclUnderlyingType(decl);
    }
    while (walk.kind == c.CXType_Elaborated) {
        walk = c.clang_Type_getNamedType(walk);
    }

    var ty = c.clang_getCanonicalType(walk);
    while (ty.kind == c.CXType_Elaborated) {
        ty = c.clang_Type_getNamedType(ty);
        ty = c.clang_getCanonicalType(ty);
    }

    const is_const = c.clang_isConstQualifiedType(ty_in) != 0 or c.clang_isConstQualifiedType(ty) != 0;

    switch (ty.kind) {
        c.CXType_Void => return .{ .named = "void" },
        c.CXType_Bool => return .{ .named = "bool" },
        c.CXType_Char_S, c.CXType_SChar => return .{ .named = "c.char" },
        c.CXType_Char_U, c.CXType_UChar => return .{ .named = "c.unsigned_char" },
        c.CXType_Short => return .{ .named = "c.short" },
        c.CXType_UShort => return .{ .named = "c.unsigned_short" },
        c.CXType_Int => return .{ .named = "c.int" },
        c.CXType_UInt => return .{ .named = "c.unsigned_int" },
        c.CXType_Long => return .{ .named = "c.long" },
        c.CXType_ULong => return .{ .named = "c.unsigned_long" },
        c.CXType_LongLong => return .{ .named = "c.long_long" },
        c.CXType_ULongLong => return .{ .named = "c.unsigned_long_long" },
        c.CXType_Float => return .{ .named = "c.float" },
        c.CXType_Double => return .{ .named = "c.double" },
        c.CXType_Pointer => {
            const pointee = c.clang_getPointeeType(ty);
            const pointee_canon = c.clang_getCanonicalType(pointee);
            if (pointee_canon.kind == c.CXType_Void) {
                const elem = try coll.unit.allocType();
                elem.* = .{ .named = "void" };
                const pc = c.clang_isConstQualifiedType(pointee) != 0;
                // void* is often nullable at the C boundary.
                return .{ .optional_pointer = .{ .elem = elem, .is_const = pc } };
            }
            if (pointee_canon.kind == c.CXType_FunctionProto or pointee_canon.kind == c.CXType_FunctionNoProto) {
                return try functionTypeToJog(coll, pointee_canon);
            }
            const elem = try coll.unit.allocType();
            elem.* = try typeToJog(coll, pointee, false);
            const pc = c.clang_isConstQualifiedType(pointee) != 0;
            // Heuristic: pointer returns and params stay non-optional; opaque record pointers
            // that are commonly nullable get `?` when the record is incomplete.
            if (pointee_canon.kind == c.CXType_Record) {
                const decl = c.clang_getTypeDeclaration(pointee_canon);
                if (c.clang_isCursorDefinition(decl) == 0) {
                    return .{ .optional_pointer = .{ .elem = elem, .is_const = pc } };
                }
            }
            return .{ .pointer = .{ .elem = elem, .is_const = pc } };
        },
        c.CXType_Record, c.CXType_Enum => {
            const decl = c.clang_getTypeDeclaration(ty);
            const name = try cursorName(coll, decl);
            defer coll.unit.allocator.free(name);
            if (name.len == 0) return .{ .opaque_ptr = {} };
            const owned = try coll.unit.dupe(name);
            return .{ .named = owned };
        },
        c.CXType_Typedef => {
            // Prefer std.c aliases for well-known libc typedefs.
            const td_name_s = c.clang_getTypedefName(ty);
            defer clang.disposeString(td_name_s);
            const td_name = clang.cString(td_name_s);
            if (mapLibcTypedef(td_name)) |mapped| {
                return .{ .named = mapped };
            }
            // Prefer the typedef's own name when present (e.g. SampleCallback).
            if (td_name.len > 0) {
                const owned = try coll.unit.dupe(td_name);
                return .{ .named = owned };
            }
            const under = c.clang_getCanonicalType(c.clang_getTypedefDeclUnderlyingType(c.clang_getTypeDeclaration(ty)));
            return try typeToJog(coll, under, false);
        },
        c.CXType_ConstantArray => {
            const len = c.clang_getArraySize(ty);
            const elem = try coll.unit.allocType();
            elem.* = try typeToJog(coll, c.clang_getArrayElementType(ty), false);
            return .{ .array = .{ .len = @intCast(len), .elem = elem } };
        },
        c.CXType_IncompleteArray => {
            // Decay to pointer.
            const elem = try coll.unit.allocType();
            elem.* = try typeToJog(coll, c.clang_getArrayElementType(ty), false);
            return .{ .pointer = .{ .elem = elem, .is_const = is_const } };
        },
        c.CXType_FunctionProto, c.CXType_FunctionNoProto => return try functionTypeToJog(coll, ty),
        else => {
            // Fallback: use clang's spelling when it looks like a simple name.
            const sp = c.clang_getTypeSpelling(ty);
            defer clang.disposeString(sp);
            const text = clang.cString(sp);
            if (mapBuiltinSpelling(text)) |mapped| return .{ .named = mapped };
            return .{ .opaque_ptr = {} };
        },
    }
}

fn functionTypeToJog(coll: *Collector, ty: c.CXType) anyerror!model.JogType {
    const n = c.clang_getNumArgTypes(ty);
    var params: std.ArrayListUnmanaged(model.JogType) = .{};
    errdefer params.deinit(coll.unit.allocator);
    var i: c_uint = 0;
    while (i < n) : (i += 1) {
        try params.append(coll.unit.allocator, try typeToJog(coll, c.clang_getArgType(ty, i), false));
    }
    const ret = try coll.unit.allocType();
    const result = c.clang_getResultType(ty);
    if (result.kind == c.CXType_Void) {
        ret.* = .{ .named = "()" };
    } else {
        ret.* = try typeToJog(coll, result, true);
    }
    // Param slices are owned by the unit allocator; jog-bindgen is short-lived.
    const slice = try params.toOwnedSlice(coll.unit.allocator);
    return .{ .fn_ptr = .{ .params = slice, .ret = ret } };
}

fn mapLibcTypedef(name: []const u8) ?[]const u8 {
    const pairs = [_]struct { []const u8, []const u8 }{
        .{ "size_t", "c.size_t" },
        .{ "ptrdiff_t", "c.ptrdiff_t" },
        .{ "intptr_t", "c.intptr_t" },
        .{ "uintptr_t", "c.uintptr_t" },
        .{ "int8_t", "i8" },
        .{ "uint8_t", "u8" },
        .{ "int16_t", "i16" },
        .{ "uint16_t", "u16" },
        .{ "int32_t", "i32" },
        .{ "uint32_t", "u32" },
        .{ "int64_t", "i64" },
        .{ "uint64_t", "u64" },
        .{ "FILE", "c.FILE" },
    };
    for (pairs) |p| {
        if (std.mem.eql(u8, name, p[0])) return p[1];
    }
    return null;
}

fn mapBuiltinSpelling(text: []const u8) ?[]const u8 {
    if (std.mem.eql(u8, text, "int")) return "c.int";
    if (std.mem.eql(u8, text, "unsigned int")) return "c.unsigned_int";
    if (std.mem.eql(u8, text, "long")) return "c.long";
    if (std.mem.eql(u8, text, "unsigned long")) return "c.unsigned_long";
    if (std.mem.eql(u8, text, "char")) return "c.char";
    if (std.mem.eql(u8, text, "void")) return "void";
    if (std.mem.eql(u8, text, "_Bool") or std.mem.eql(u8, text, "bool")) return "bool";
    if (std.mem.eql(u8, text, "float")) return "c.float";
    if (std.mem.eql(u8, text, "double")) return "c.double";
    return null;
}
