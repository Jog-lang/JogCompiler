# Freestanding process entry. Omitted when linking against libc CRT.
.intel_syntax noprefix
.text
.globl _start
.type _start, @function
_start:
  xor ebp, ebp
  # SysV process stack: [rsp]=argc, [rsp+8]=argv[0], …
  # Pass through to main so jogrt_argc / jogrt_arg_ptr can capture (same as CRT).
  mov rdi, qword ptr [rsp]
  lea rsi, [rsp + 8]
  and rsp, -16
  call main
  mov rdi, rax
  # exit_group (231): CLONE_THREAD workers survive plain exit(60)
  mov eax, 231
  syscall
