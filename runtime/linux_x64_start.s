# Freestanding process entry. Omitted when linking against libc CRT.
.intel_syntax noprefix
.text
.globl _start
.type _start, @function
_start:
  xor ebp, ebp
  # SysV process stack: [rsp]=argc, [rsp+8]=argv[0], …, NULL, envp[0], …
  # Pass argc/argv/envp like CRT so main can capture them.
  mov rdi, qword ptr [rsp]
  lea rsi, [rsp + 8]
  lea rdx, [rsi + rdi * 8 + 8]
  and rsp, -16
  call main
  mov rdi, rax
  # exit_group (231): CLONE_THREAD workers survive plain exit(60)
  mov eax, 231
  syscall
