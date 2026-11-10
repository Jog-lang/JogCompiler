const std = @import("std");
const ir = @import("../ir/ir.zig");

/// Emit a static linux x86_64 ELF that performs write(1, msg, len) and exit(code)
/// with raw syscalls — no libc, no C intermediate.
pub fn emitLinuxX64(allocator: std.mem.Allocator, program: *const ir.Program, writer: anytype) !void {
    const message: []const u8 = program.message;
    var exit_code: u8 = 0;
    for (program.ops) |inst| {
        if (inst.op == .sys_write_exit) {
            exit_code = inst.exit_code;
            break;
        }
    }

    // Machine code: lea rsi, [rip+msg]; mov rdx, len; mov rdi, 1; mov rax, 1; syscall;
    // mov rdi, exit; mov rax, 60; syscall
    // We place code at virtual address 0x400000, message right after code in same segment.

    var code: std.ArrayListUnmanaged(u8) = .{};
    defer code.deinit(allocator);

    // Placeholder: we'll patch RIP-relative displacement after we know msg offset from next insn
    // Sequence:
    //   lea rsi, [rip + disp32]     48 8d 35 xx xx xx xx
    //   mov edx, imm32              ba xx xx xx xx         (len)
    //   mov edi, 1                  bf 01 00 00 00
    //   mov eax, 1                  b8 01 00 00 00
    //   syscall                     0f 05
    //   mov edi, exit               bf xx 00 00 00
    //   mov eax, 60                 b8 3c 00 00 00
    //   syscall                     0f 05

    const lea_pos = code.items.len;
    try code.appendSlice(allocator, &[_]u8{ 0x48, 0x8d, 0x35, 0, 0, 0, 0 }); // disp patched
    const after_lea = code.items.len;

    var len_buf: [4]u8 = undefined;
    std.mem.writeInt(u32, &len_buf, @intCast(message.len), .little);
    try code.append(allocator, 0xba);
    try code.appendSlice(allocator, &len_buf);

    try code.appendSlice(allocator, &[_]u8{ 0xbf, 0x01, 0x00, 0x00, 0x00 }); // rdi = 1
    try code.appendSlice(allocator, &[_]u8{ 0xb8, 0x01, 0x00, 0x00, 0x00 }); // rax = 1 write
    try code.appendSlice(allocator, &[_]u8{ 0x0f, 0x05 });

    try code.append(allocator, 0xbf);
    try code.append(allocator, exit_code);
    try code.appendSlice(allocator, &[_]u8{ 0x00, 0x00, 0x00 });
    try code.appendSlice(allocator, &[_]u8{ 0xb8, 0x3c, 0x00, 0x00, 0x00 }); // exit
    try code.appendSlice(allocator, &[_]u8{ 0x0f, 0x05 });

    // Layout in file:
    // [Ehdr][Phdr][code][message]
    const ehdr_size: u64 = 64;
    const phdr_size: u64 = 56;
    const phdr_off: u64 = ehdr_size;
    const code_off: u64 = ehdr_size + phdr_size;
    const msg_off: u64 = code_off + code.items.len;

    // Patch lea disp: disp = msg_addr - addr_after_lea
    // vaddr base 0x400000, file offset code_off maps to vaddr 0x400000 + 0? 
    // Load: p_vaddr = 0x400000, p_offset = 0, so file offset X → vaddr 0x400000+X
    const msg_vaddr: i64 = @intCast(0x400000 + msg_off);
    const after_lea_vaddr: i64 = @intCast(0x400000 + code_off + after_lea);
    const disp: i32 = @intCast(msg_vaddr - after_lea_vaddr);
    std.mem.writeInt(i32, code.items[lea_pos + 3 ..][0..4], disp, .little);

    const file_size = msg_off + message.len;
    // Align memsz
    const load_size = file_size;

    var out: std.ArrayListUnmanaged(u8) = .{};
    defer out.deinit(allocator);
    try out.resize(allocator, file_size);
    @memset(out.items, 0);

    // ELF header
    const ehdr = out.items[0..64];
    ehdr[0..4].* = "\x7fELF".*;
    ehdr[4] = 2; // 64-bit
    ehdr[5] = 1; // little
    ehdr[6] = 1; // version
    ehdr[7] = 0; // System V
    std.mem.writeInt(u16, ehdr[16..18], 2, .little); // ET_EXEC
    std.mem.writeInt(u16, ehdr[18..20], 62, .little); // EM_X86_64
    std.mem.writeInt(u32, ehdr[20..24], 1, .little);
    std.mem.writeInt(u64, ehdr[24..32], 0x400000 + code_off, .little); // entry
    std.mem.writeInt(u64, ehdr[32..40], phdr_off, .little);
    std.mem.writeInt(u64, ehdr[40..48], 0, .little); // shoff
    std.mem.writeInt(u32, ehdr[48..52], 0, .little);
    std.mem.writeInt(u16, ehdr[52..54], 64, .little); // ehsize
    std.mem.writeInt(u16, ehdr[54..56], 56, .little); // phentsize
    std.mem.writeInt(u16, ehdr[56..58], 1, .little); // phnum
    std.mem.writeInt(u16, ehdr[58..60], 0, .little);
    std.mem.writeInt(u16, ehdr[60..62], 0, .little);
    std.mem.writeInt(u16, ehdr[62..64], 0, .little);

    // Program header PT_LOAD
    const ph = out.items[phdr_off..][0..56];
    std.mem.writeInt(u32, ph[0..4], 1, .little); // PT_LOAD
    std.mem.writeInt(u32, ph[4..8], 5, .little); // PF_R|PF_X
    std.mem.writeInt(u64, ph[8..16], 0, .little); // offset
    std.mem.writeInt(u64, ph[16..24], 0x400000, .little); // vaddr
    std.mem.writeInt(u64, ph[24..32], 0x400000, .little); // paddr
    std.mem.writeInt(u64, ph[32..40], load_size, .little); // filesz
    std.mem.writeInt(u64, ph[40..48], load_size, .little); // memsz
    std.mem.writeInt(u64, ph[48..56], 0x1000, .little); // align

    @memcpy(out.items[code_off..][0..code.items.len], code.items);
    if (message.len > 0) {
        @memcpy(out.items[msg_off..][0..message.len], message);
    }

    try writer.writeAll(out.items);
}

test "elf header magic" {
    var ops = [_]ir.Inst{.{ .op = .sys_write_exit }};
    var prog = ir.Program{
        .ops = ops[0..],
        .message = "x",
        .arena = undefined,
    };
    var list: std.ArrayListUnmanaged(u8) = .{};
    defer list.deinit(std.testing.allocator);
    try emitLinuxX64(std.testing.allocator, &prog, list.writer(std.testing.allocator));
    try std.testing.expect(list.items.len > 64);
    try std.testing.expectEqualStrings("\x7fELF", list.items[0..4]);
}
