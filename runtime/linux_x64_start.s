# Freestanding process entry. Omitted when linking against libc CRT.
.intel_syntax noprefix
.text
.globl _start
.type _start, @function
_start:
  xor ebp, ebp
  and rsp, -16
  call main
  mov rdi, rax
  # exit_group (231): CLONE_THREAD workers survive plain exit(60)
  mov eax, 231
  syscall
