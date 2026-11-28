//! Intermediate model for C → Jog binding generation (JOG-230).
//! Kept free of libclang so emit/unit tests do not need Clang.

const std = @import("std");

pub const Options = struct {
    /// C header path (or include name such as `SDL3/SDL.h`).
    header: []const u8,
    /// Output path; null → stdout.
    output: ?[]const u8 = null,
    /// Only emit decls whose name starts with this prefix (empty = all non-system).
    prefix: []const u8 = "",
    /// Optional `@link("…")` library name for the extern block.
    link: ?[]const u8 = null,
    /// Extra `-I` include directories.
    includes: []const []const u8 = &.{},
    /// Extra `-D` macros (`NAME` or `NAME=value`).
    defines: []const []const u8 = &.{},
};

pub const JogType = union(enum) {
    named: []const u8,
    pointer: struct { elem: *JogType, is_const: bool },
    optional_pointer: struct { elem: *JogType, is_const: bool },
    array: struct { len: u64, elem: *JogType },
    fn_ptr: struct { params: []JogType, ret: *JogType },
    /// Untranslatable — emit as comment + `*void`.
    opaque_ptr,
};

pub const Field = struct {
    name: []const u8,
    ty: JogType,
};

pub const Param = struct {
    name: []const u8,
    ty: JogType,
};

pub const FnDecl = struct {
    name: []const u8,
    params: []Param,
    ret: ?JogType, // null = void
    variadic: bool = false,
};

pub const StructDecl = struct {
    name: []const u8,
    fields: []Field,
    opaque_only: bool = false,
    is_union: bool = false,
};

pub const EnumVariant = struct {
    name: []const u8,
    value: i64,
};

pub const EnumDecl = struct {
    name: []const u8,
    variants: []EnumVariant,
};

pub const TypedefDecl = struct {
    name: []const u8,
    ty: JogType,
};

pub const ConstDecl = struct {
    name: []const u8,
    /// Rendered Jog literal / expression text (e.g. `42`, `0x20`, `"hi"`).
    value: []const u8,
    /// Jog type spelling (`u32`, `i32`, `str`/`*const c.char` via c"…").
    ty: []const u8,
};

pub const Skipped = struct {
    name: []const u8,
    reason: []const u8,
};

pub const Unit = struct {
    allocator: std.mem.Allocator,
    structs: std.ArrayListUnmanaged(StructDecl) = .{},
    enums: std.ArrayListUnmanaged(EnumDecl) = .{},
    typedefs: std.ArrayListUnmanaged(TypedefDecl) = .{},
    consts: std.ArrayListUnmanaged(ConstDecl) = .{},
    fns: std.ArrayListUnmanaged(FnDecl) = .{},
    skipped: std.ArrayListUnmanaged(Skipped) = .{},
    /// Arena-ish owned strings / JogType nodes.
    owned_strings: std.ArrayListUnmanaged([]const u8) = .{},
    owned_types: std.ArrayListUnmanaged(*JogType) = .{},

    pub fn deinit(self: *Unit) void {
        for (self.structs.items) |*s| {
            self.allocator.free(s.fields);
        }
        self.structs.deinit(self.allocator);
        for (self.enums.items) |*e| {
            self.allocator.free(e.variants);
        }
        self.enums.deinit(self.allocator);
        self.typedefs.deinit(self.allocator);
        self.consts.deinit(self.allocator);
        for (self.fns.items) |*f| {
            self.allocator.free(f.params);
        }
        self.fns.deinit(self.allocator);
        self.skipped.deinit(self.allocator);
        for (self.owned_strings.items) |s| self.allocator.free(s);
        self.owned_strings.deinit(self.allocator);
        for (self.owned_types.items) |t| self.allocator.destroy(t);
        self.owned_types.deinit(self.allocator);
    }

    pub fn dupe(self: *Unit, s: []const u8) ![]const u8 {
        const copy = try self.allocator.dupe(u8, s);
        try self.owned_strings.append(self.allocator, copy);
        return copy;
    }

    pub fn allocType(self: *Unit) !*JogType {
        const t = try self.allocator.create(JogType);
        try self.owned_types.append(self.allocator, t);
        return t;
    }
};
