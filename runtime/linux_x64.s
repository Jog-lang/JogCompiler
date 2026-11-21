# Jog v2 linux x86_64 runtime helpers (SysV).
# Linked into every program. Entry (_start) is in linux_x64_start.s for freestanding builds.

.intel_syntax noprefix

.bss
.align 16
# Bump heap (JOG-89): 4 MiB so software framebuffers fit without libc malloc.
.set JOGRT_HEAP_SIZE, 4194304
jogrt_heap:
  .zero JOGRT_HEAP_SIZE
jogrt_heap_ptr:
  .zero 8
# Circular arena for by-value struct/tuple temps + returns (avoids bump OOM on hot paths).
# Pointers are only valid until the ring wraps; fine for immediate field use / sret-style temps.
.set JOGRT_RET_ARENA_SIZE, 1048576
jogrt_ret_arena:
  .zero JOGRT_RET_ARENA_SIZE
jogrt_ret_ptr:
  .zero 8
# Process args (captured at main entry from CRT or freestanding _start).
.globl jogrt_argc_val
jogrt_argc_val:
  .zero 8
.globl jogrt_argv_base
jogrt_argv_base:
  .zero 8

.text

.globl jogrt_argc
.type jogrt_argc, @function
jogrt_argc:
  mov rax, qword ptr [rip + jogrt_argc_val]
  ret

.globl jogrt_arg_ptr
.type jogrt_arg_ptr, @function
jogrt_arg_ptr:
  # rdi = index; return argv[i] or null
  mov rax, qword ptr [rip + jogrt_argc_val]
  cmp rdi, rax
  jae 1f
  test rdi, rdi
  js 1f
  mov rax, qword ptr [rip + jogrt_argv_base]
  mov rax, qword ptr [rax + rdi * 8]
  ret
1:
  xor eax, eax
  ret

.globl jogrt_print
.type jogrt_print, @function
jogrt_print:
  mov rdx, rsi
  mov rsi, rdi
  mov edi, 1
  mov eax, 1
  syscall
  ret

.globl jogrt_panic
.type jogrt_panic, @function
jogrt_panic:
  mov rdx, rsi
  mov rsi, rdi
  mov edi, 2
  mov eax, 1
  syscall
  mov edi, 1
  mov eax, 231
  syscall

.globl jogrt_alloc
.type jogrt_alloc, @function
jogrt_alloc:
  lea rcx, [rip + jogrt_heap_ptr]
  mov rax, qword ptr [rcx]
  test rax, rax
  jnz 1f
  lea rax, [rip + jogrt_heap]
1:
  add rdi, 7
  and rdi, -8
  mov rdx, rax
  add rax, rdi
  lea rsi, [rip + jogrt_heap]
  add rsi, JOGRT_HEAP_SIZE
  cmp rax, rsi
  ja 2f
  mov qword ptr [rcx], rax
  mov rax, rdx
  ret
2:
  lea rdi, [rip + jogrt_oom_msg]
  mov esi, jogrt_oom_msg_len
  call jogrt_panic

# Freeable bump: return current watermark (pass to jogrt_alloc_reset).
.globl jogrt_alloc_checkpoint
.type jogrt_alloc_checkpoint, @function
jogrt_alloc_checkpoint:
  lea rcx, [rip + jogrt_heap_ptr]
  mov rax, qword ptr [rcx]
  test rax, rax
  jnz 1f
  lea rax, [rip + jogrt_heap]
1:
  ret

# Reset bump to checkpoint (0 / null ⇒ base of heap). LIFO only; dangling ptrs after reset.
.globl jogrt_alloc_reset
.type jogrt_alloc_reset, @function
jogrt_alloc_reset:
  lea rcx, [rip + jogrt_heap_ptr]
  test rdi, rdi
  jnz 1f
  lea rdi, [rip + jogrt_heap]
1:
  mov qword ptr [rcx], rdi
  xor eax, eax
  ret

# f64 → IEEE-754 binary32 bits. C/SysV extern: arg in xmm0.
.globl jog_f32_bits
.type jog_f32_bits, @function
jog_f32_bits:
  cvtsd2ss xmm0, xmm0
  movd eax, xmm0
  ret

# Same convert when the f64 bit pattern arrives in a Jog GPR slot (rdi).
.globl jogrt_f32_bits
.type jogrt_f32_bits, @function
jogrt_f32_bits:
  movq xmm0, rdi
  cvtsd2ss xmm0, xmm0
  movd eax, xmm0
  ret

# IEEE-754 binary32 bits in edi → f64 bits in rax (Jog float slot).
.globl jogrt_f32_from_bits
.type jogrt_f32_from_bits, @function
jogrt_f32_from_bits:
  movd xmm0, edi
  cvtss2sd xmm0, xmm0
  movq rax, xmm0
  ret

# Allocate from the circular struct-return arena (wraps; no OOM panic).
.globl jogrt_ret_alloc
.type jogrt_ret_alloc, @function
jogrt_ret_alloc:
  lea rcx, [rip + jogrt_ret_ptr]
  mov rax, qword ptr [rcx]
  test rax, rax
  jnz 1f
  lea rax, [rip + jogrt_ret_arena]
1:
  add rdi, 7
  and rdi, -8
  mov rdx, rax
  add rax, rdi
  lea rsi, [rip + jogrt_ret_arena]
  add rsi, JOGRT_RET_ARENA_SIZE
  cmp rax, rsi
  jbe 2f
  # wrap
  lea rax, [rip + jogrt_ret_arena]
  mov rdx, rax
  add rax, rdi
2:
  mov qword ptr [rcx], rax
  mov rax, rdx
  ret

.globl jogrt_dispatcher_stub
.type jogrt_dispatcher_stub, @function
jogrt_dispatcher_stub:
  ret

# Synchronous "spawn": call fn(arg) on this thread (Dispatcher stub).
# For cooperative stackful launch use jogrt_go (linux_x64_coro.s).
# rdi = function pointer, rsi = arg
.globl jogrt_spawn
.type jogrt_spawn, @function
jogrt_spawn:
  mov rax, rdi
  mov rdi, rsi
  call rax
  ret

# Returning mainloop (desktop): rdi = tick fn () -> i64/i32; stop when tick returns 0.
# Emscripten/wasm is a separate target (JOG-90); this is the native while-loop actual.
.globl jog_host_run_loop
.type jog_host_run_loop, @function
.globl jogrt_host_run_loop
.type jogrt_host_run_loop, @function
jog_host_run_loop:
jogrt_host_run_loop:
  push rbp
  mov rbp, rsp
  push rbx
  mov rbx, rdi
  test rbx, rbx
  jz 2f
1:
  xor eax, eax
  call rbx
  test rax, rax
  jnz 1b
2:
  pop rbx
  pop rbp
  ret

.section .rodata
jogrt_oom_msg:
  .ascii "jogrt: out of memory\n"
jogrt_oom_msg_len = . - jogrt_oom_msg
