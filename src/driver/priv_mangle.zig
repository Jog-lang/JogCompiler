const std = @import("std");
const ast = @import("../ast/ast.zig");

/// Flattened multi-file merge keeps bare top-level names. Bare `private` is
/// file-local (JogDocs Syntax / Modules), so rewrite private fn decls and
/// same-file call sites to a path-stable asm-safe symbol before merge.
pub fn mangleFilePrivateFns(file: *ast.File, path: []const u8) !void {
    const arena = file.arena.allocator();
    var renames: std.StringHashMapUnmanaged([]const u8) = .{};
    defer renames.deinit(arena);

    for (file.decls) |*d| {
        switch (d.*) {
            .fn_decl, .actual_fn => |*fd| {
                if (!fd.is_private) continue;
                if (fd.body == null) continue;
                const mangled = try mangleName(arena, path, fd.name);
                try renames.put(arena, fd.name, mangled);
                fd.name = mangled;
            },
            else => {},
        }
    }
    if (renames.count() == 0) return;

    for (file.decls) |*d| {
        switch (d.*) {
            .fn_decl, .actual_fn => |*fd| {
                if (fd.body) |body| rewriteBlock(body, &renames);
            },
            .method => |*md| {
                if (md.body) |body| rewriteBlock(body, &renames);
            },
            else => {},
        }
    }
}

fn mangleName(arena: std.mem.Allocator, path: []const u8, name: []const u8) ![]u8 {
    var h: u64 = 14695981039346656037;
    for (path) |c| {
        h ^= c;
        h *%= 1099511628211;
    }
    const base = std.fs.path.basename(path);
    const stem = if (std.mem.endsWith(u8, base, ".jog"))
        base[0 .. base.len - ".jog".len]
    else
        base;
    var stem_buf: [96]u8 = undefined;
    const stem_safe = sanitizeIdent(&stem_buf, stem);
    // Short hash keeps labels unique across packages that share a leaf name.
    const tag: u32 = @truncate(h);
    return try std.fmt.allocPrint(arena, "jogp_{s}_{x}_{s}", .{ stem_safe, tag, name });
}

fn sanitizeIdent(buf: []u8, s: []const u8) []const u8 {
    const n = @min(buf.len, s.len);
    var i: usize = 0;
    while (i < n) : (i += 1) {
        const c = s[i];
        buf[i] = if ((c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or c == '_')
            c
        else
            '_';
    }
    if (n == 0) {
        if (buf.len == 0) return "";
        buf[0] = 'x';
        return buf[0..1];
    }
    return buf[0..n];
}

fn rewriteBlock(block: *ast.Block, renames: *const std.StringHashMapUnmanaged([]const u8)) void {
    for (block.stmts) |*stmt| rewriteStmt(stmt, renames);
}

fn rewriteStmt(stmt: *ast.Stmt, renames: *const std.StringHashMapUnmanaged([]const u8)) void {
    switch (stmt.*) {
        .var_decl => |*vd| {
            if (vd.init) |init| rewriteExpr(init, renames);
        },
        .destructure => |*ds| rewriteExpr(ds.init, renames),
        .assign => |*as| {
            rewriteExpr(as.value, renames);
            if (as.field) |*fa| rewriteExpr(fa.base, renames);
            if (as.index) |*ix| {
                rewriteExpr(ix.base, renames);
                rewriteExpr(ix.index, renames);
            }
        },
        .expr => |e| rewriteExpr(e, renames),
        .return_stmt => |re| if (re) |e| rewriteExpr(e, renames),
        .if_stmt => |*ifs| {
            rewriteExpr(ifs.cond, renames);
            rewriteBlock(ifs.then_block, renames);
            if (ifs.else_block) |eb| rewriteBlock(eb, renames);
        },
        .while_stmt => |*ws| {
            rewriteExpr(ws.cond, renames);
            rewriteBlock(ws.body, renames);
        },
        .for_in_stmt => |*fs| {
            rewriteExpr(fs.iter, renames);
            rewriteBlock(fs.body, renames);
        },
        .with_stmt => |*ws| {
            for (ws.values) |v| rewriteExpr(v, renames);
            rewriteBlock(ws.body, renames);
        },
        .defer_stmt, .errdefer_stmt => |*ds| {
            switch (ds.*) {
                .expr => |e| rewriteExpr(e, renames),
                .block => |b| rewriteBlock(b, renames),
            }
        },
        .break_stmt, .continue_stmt => {},
    }
}

fn rewriteExpr(e: *ast.Expr, renames: *const std.StringHashMapUnmanaged([]const u8)) void {
    switch (e.*) {
        .ident => {},
        .int, .float, .string, .cstring, .null_lit => {},
        .binary => |*b| {
            rewriteExpr(b.left, renames);
            rewriteExpr(b.right, renames);
        },
        .unary => |*u| rewriteExpr(u.expr, renames),
        .call => |*c| {
            // Bare `helper(...)` — rewrite callee ident to the mangled private symbol.
            if (c.callee.* == .ident) {
                if (renames.get(c.callee.ident)) |mangled| {
                    c.callee.* = .{ .ident = mangled };
                }
            } else {
                rewriteExpr(c.callee, renames);
            }
            for (c.args) |arg| rewriteExpr(arg, renames);
            for (c.with_args) |w| rewriteExpr(w, renames);
        },
        .field => |*f| rewriteExpr(f.base, renames),
        .index => |*ix| {
            rewriteExpr(ix.base, renames);
            rewriteExpr(ix.index, renames);
        },
        .slice => |*sl| {
            rewriteExpr(sl.base, renames);
            if (sl.start) |s| rewriteExpr(s, renames);
            if (sl.end) |en| rewriteExpr(en, renames);
        },
        .cast, .try_cast => |*c| rewriteExpr(c.expr, renames),
        .is_type => |*it| rewriteExpr(it.expr, renames),
        .optional_call => |*oc| {
            rewriteExpr(oc.base, renames);
            for (oc.args) |arg| rewriteExpr(arg, renames);
        },
        .struct_lit => |*sl| {
            for (sl.fields) |*f| rewriteExpr(f.value, renames);
        },
        .panic_call => |*p| rewriteExpr(p.message, renames),
        .when_expr => |*w| {
            rewriteExpr(w.scrutinee, renames);
            for (w.arms) |*arm| rewriteExpr(arm.body, renames);
            if (w.else_body) |eb| rewriteExpr(eb, renames);
        },
        .tuple => |elems| {
            for (elems) |el| rewriteExpr(el, renames);
        },
        .range => |*r| {
            rewriteExpr(r.start, renames);
            rewriteExpr(r.end, renames);
        },
        .array_lit => |*al| {
            for (al.elems) |el| rewriteExpr(el, renames);
        },
        .interp => |*ip| {
            for (ip.parts) |*part| {
                switch (part.*) {
                    .text => {},
                    .expr => |ex| rewriteExpr(ex, renames),
                }
            }
        },
        .force_unwrap => |inner| rewriteExpr(inner, renames),
    }
}
