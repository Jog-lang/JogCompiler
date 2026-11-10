# Jog v2 linux x86_64 runtime helpers (SysV).
# Linked into every program. Entry (_start) is in linux_x64_start.s for freestanding builds.

.intel_syntax noprefix

.bss
.align 16
jogrt_heap:
  .zero 65536
jogrt_heap_ptr:
  .zero 8

.text

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
  mov eax, 60
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
  add rsi, 65536
  cmp rax, rsi
  ja 2f
  mov qword ptr [rcx], rax
  mov rax, rdx
  ret
2:
  lea rdi, [rip + jogrt_oom_msg]
  mov esi, jogrt_oom_msg_len
  call jogrt_panic

.globl jogrt_dispatcher_stub
.type jogrt_dispatcher_stub, @function
jogrt_dispatcher_stub:
  ret

.section .rodata
jogrt_oom_msg:
  .ascii "jogrt: out of memory\n"
jogrt_oom_msg_len = . - jogrt_oom_msg
