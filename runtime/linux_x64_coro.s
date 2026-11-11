# Jog v2 single-thread stackful coroutine scheduler (linux x86_64 SysV).
# Gate step 1: yield / park stubs + rsp swap. No multi-worker pool.
#
# Slot layout (128 bytes):
#   0x00 rbx  0x08 rbp  0x10 r12  0x18 rsp
#   0x20 r13  0x28 r14  0x30 r15  0x38 rip
#   0x40 rdi  0x48 rsi  0x50 fn   0x58 arg
#   0x60 flags (bit0=alive, bit1=parked)  0x68 stack_base
#
# Stacks: 64 KiB usable + 1 guard page via mmap/mprotect.

.intel_syntax noprefix

.set JOGRT_CORO_MAX, 16
.set JOGRT_CORO_SLOT, 128
.set JOGRT_CORO_STACK, 65536
.set JOGRT_CORO_PAGE, 4096
.set JOGRT_CORO_MAP, (JOGRT_CORO_PAGE + JOGRT_CORO_STACK)

.set CORO_OFF_RSP, 0x18
.set CORO_OFF_RIP, 0x38
.set CORO_OFF_RDI, 0x40
.set CORO_OFF_FN, 0x50
.set CORO_OFF_ARG, 0x58
.set CORO_OFF_FLAGS, 0x60
.set CORO_OFF_STACK, 0x68

.set CORO_ALIVE, 1
.set CORO_PARKED, 2

.bss
.align 16
jogrt_coro_slots:
  .zero JOGRT_CORO_MAX * JOGRT_CORO_SLOT
jogrt_coro_current:
  .zero 8
jogrt_coro_count:
  .zero 8
jogrt_coro_ready:
  .zero 8

.text

# rdi = save ctx, rsi = restore ctx
.globl jogrt_coro_switch
.type jogrt_coro_switch, @function
jogrt_coro_switch:
  mov qword ptr [rdi + 0x00], rbx
  mov qword ptr [rdi + 0x08], rbp
  mov qword ptr [rdi + 0x10], r12
  mov qword ptr [rdi + 0x18], rsp
  mov qword ptr [rdi + 0x20], r13
  mov qword ptr [rdi + 0x28], r14
  mov qword ptr [rdi + 0x30], r15
  lea rax, [rip + .Lcoro_switch_resume]
  mov qword ptr [rdi + 0x38], rax
  mov qword ptr [rdi + 0x40], rdi
  mov qword ptr [rdi + 0x48], rsi

  mov rbx, qword ptr [rsi + 0x00]
  mov rbp, qword ptr [rsi + 0x08]
  mov r12, qword ptr [rsi + 0x10]
  mov r13, qword ptr [rsi + 0x20]
  mov r14, qword ptr [rsi + 0x28]
  mov r15, qword ptr [rsi + 0x30]
  mov rdi, qword ptr [rsi + 0x40]
  mov rax, qword ptr [rsi + 0x38]
  mov rsp, qword ptr [rsi + 0x18]
  jmp rax
.Lcoro_switch_resume:
  ret

.globl jogrt_coro_entry_stub
.type jogrt_coro_entry_stub, @function
jogrt_coro_entry_stub:
  and rsp, -16
  call jogrt_coro_trampoline
.Lcoro_entry_hang:
  jmp .Lcoro_entry_hang

.type jogrt_coro_trampoline, @function
jogrt_coro_trampoline:
  mov rax, qword ptr [rip + jogrt_coro_current]
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  mov rax, qword ptr [rcx + CORO_OFF_FN]
  mov rdi, qword ptr [rcx + CORO_OFF_ARG]
  call rax
  mov rax, qword ptr [rip + jogrt_coro_current]
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  mov qword ptr [rcx + CORO_OFF_FLAGS], 0
.Lcoro_tramp_yield:
  call jogrt_yield
  jmp .Lcoro_tramp_yield

.type jogrt_coro_init, @function
jogrt_coro_init:
  mov rax, qword ptr [rip + jogrt_coro_ready]
  test rax, rax
  jnz .Lcoro_init_done
  lea rdi, [rip + jogrt_coro_slots]
  mov ecx, (JOGRT_CORO_MAX * JOGRT_CORO_SLOT) / 8
  xor eax, eax
  rep stosq
  mov qword ptr [rip + jogrt_coro_slots + CORO_OFF_FLAGS], CORO_ALIVE
  mov qword ptr [rip + jogrt_coro_current], 0
  mov qword ptr [rip + jogrt_coro_count], 1
  mov qword ptr [rip + jogrt_coro_ready], 1
.Lcoro_init_done:
  ret

# Returns stack base in rax. Clobbers per SysV caller-saved; preserves rbx.
.type jogrt_coro_stack_alloc, @function
jogrt_coro_stack_alloc:
  push rbx
  mov eax, 9
  xor edi, edi
  mov esi, JOGRT_CORO_MAP
  mov edx, 3
  mov r10d, 0x22
  mov r8, -1
  xor r9d, r9d
  syscall
  cmp rax, -4095
  jae .Lcoro_stack_fail
  mov rbx, rax
  mov eax, 10
  mov rdi, rbx
  mov esi, JOGRT_CORO_PAGE
  xor edx, edx
  syscall
  test rax, rax
  jnz .Lcoro_stack_fail
  mov rax, rbx
  pop rbx
  ret
.Lcoro_stack_fail:
  lea rdi, [rip + jogrt_coro_oom_msg]
  mov esi, jogrt_coro_oom_msg_len
  call jogrt_panic

# rdi = slot, rsi = fn, rdx = arg
.type jogrt_coro_arm, @function
jogrt_coro_arm:
  push rbx
  push r12
  push r13
  mov r12, rdi
  mov r13, rsi
  mov rbx, rdx
  mov rax, qword ptr [r12 + CORO_OFF_STACK]
  test rax, rax
  jnz .Lcoro_arm_have
  call jogrt_coro_stack_alloc
  mov qword ptr [r12 + CORO_OFF_STACK], rax
.Lcoro_arm_have:
  mov rax, qword ptr [r12 + CORO_OFF_STACK]
  lea rcx, [rax + JOGRT_CORO_MAP]
  and rcx, -16
  xor eax, eax
  mov qword ptr [r12 + 0x00], rax
  mov qword ptr [r12 + 0x08], rax
  mov qword ptr [r12 + 0x10], rax
  mov qword ptr [r12 + 0x20], rax
  mov qword ptr [r12 + 0x28], rax
  mov qword ptr [r12 + 0x30], rax
  mov qword ptr [r12 + 0x48], rax
  mov qword ptr [r12 + CORO_OFF_RSP], rcx
  lea rax, [rip + jogrt_coro_entry_stub]
  mov qword ptr [r12 + CORO_OFF_RIP], rax
  mov qword ptr [r12 + CORO_OFF_RDI], 0
  mov qword ptr [r12 + CORO_OFF_FN], r13
  mov qword ptr [r12 + CORO_OFF_ARG], rbx
  mov qword ptr [r12 + CORO_OFF_FLAGS], CORO_ALIVE
  pop r13
  pop r12
  pop rbx
  ret

# rdi = destination index. Saves current, restores dest.
.type jogrt_coro_switch_to, @function
jogrt_coro_switch_to:
  mov rsi, qword ptr [rip + jogrt_coro_current]
  cmp rdi, rsi
  je .Lcoro_sti_ret
  mov qword ptr [rip + jogrt_coro_current], rdi
  # rsi = save index, compute save ptr into r8; dest ptr into rsi; then switch(save, restore)
  mov r8, rsi
  imul r8, r8, JOGRT_CORO_SLOT
  lea rax, [rip + jogrt_coro_slots]
  add r8, rax
  imul rdi, rdi, JOGRT_CORO_SLOT
  lea rax, [rip + jogrt_coro_slots]
  add rdi, rax
  mov rsi, rdi
  mov rdi, r8
  jmp jogrt_coro_switch
.Lcoro_sti_ret:
  ret

# rdi = fn, rsi = arg. Enqueue only; does not switch.
.globl jogrt_go
.type jogrt_go, @function
jogrt_go:
  push rbx
  push r12
  push r13
  mov r12, rdi
  mov r13, rsi
  call jogrt_coro_init
  test r12, r12
  jz .Lcoro_go_fail
  mov rcx, qword ptr [rip + jogrt_coro_count]
  mov rdx, qword ptr [rip + jogrt_coro_current]
  mov rbx, 1
.Lcoro_go_scan:
  cmp rbx, rcx
  jae .Lcoro_go_new
  cmp rbx, rdx
  je .Lcoro_go_scan_next
  mov rax, rbx
  imul rax, rax, JOGRT_CORO_SLOT
  lea rdi, [rip + jogrt_coro_slots]
  add rdi, rax
  mov rax, qword ptr [rdi + CORO_OFF_FLAGS]
  test rax, CORO_ALIVE
  jnz .Lcoro_go_scan_next
  mov rsi, r12
  mov rdx, r13
  call jogrt_coro_arm
  xor eax, eax
  pop r13
  pop r12
  pop rbx
  ret
.Lcoro_go_scan_next:
  inc rbx
  jmp .Lcoro_go_scan
.Lcoro_go_new:
  cmp rcx, JOGRT_CORO_MAX
  jae .Lcoro_go_fail
  mov rbx, rcx
  inc rcx
  mov qword ptr [rip + jogrt_coro_count], rcx
  mov rax, rbx
  imul rax, rax, JOGRT_CORO_SLOT
  lea rdi, [rip + jogrt_coro_slots]
  add rdi, rax
  mov rsi, r12
  mov rdx, r13
  call jogrt_coro_arm
  xor eax, eax
  pop r13
  pop r12
  pop rbx
  ret
.Lcoro_go_fail:
  lea rdi, [rip + jogrt_coro_go_fail_msg]
  mov esi, jogrt_coro_go_fail_msg_len
  call jogrt_panic

.globl jogrt_yield
.type jogrt_yield, @function
jogrt_yield:
  push rbx
  push r12
  push r13
  call jogrt_coro_init
  mov r12, qword ptr [rip + jogrt_coro_count]
  test r12, r12
  jz .Lcoro_yield_done
  mov r13, qword ptr [rip + jogrt_coro_current]
  mov rbx, 1
.Lcoro_yield_scan:
  cmp rbx, r12
  jae .Lcoro_yield_done
  mov rax, r13
  add rax, rbx
  xor edx, edx
  div r12
  # rdx = candidate index
  cmp rdx, r13
  je .Lcoro_yield_next
  mov rax, rdx
  imul rax, rax, JOGRT_CORO_SLOT
  lea rdi, [rip + jogrt_coro_slots]
  add rdi, rax
  mov rsi, qword ptr [rdi + CORO_OFF_FLAGS]
  test rsi, CORO_ALIVE
  jz .Lcoro_yield_next
  test rsi, CORO_PARKED
  jnz .Lcoro_yield_next
  mov rdi, rdx
  pop r13
  pop r12
  pop rbx
  jmp jogrt_coro_switch_to
.Lcoro_yield_next:
  inc rbx
  jmp .Lcoro_yield_scan
.Lcoro_yield_done:
  pop r13
  pop r12
  pop rbx
  ret

.globl jogrt_coro_park
.type jogrt_coro_park, @function
jogrt_coro_park:
  call jogrt_coro_init
  mov rax, qword ptr [rip + jogrt_coro_current]
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  mov rax, qword ptr [rcx + CORO_OFF_FLAGS]
  or rax, CORO_PARKED
  mov qword ptr [rcx + CORO_OFF_FLAGS], rax
  jmp jogrt_yield

.globl jogrt_coro_unpark
.type jogrt_coro_unpark, @function
jogrt_coro_unpark:
  call jogrt_coro_init
  cmp rdi, JOGRT_CORO_MAX
  jae .Lcoro_unpark_ret
  imul rdi, rdi, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rdi
  mov rax, qword ptr [rcx + CORO_OFF_FLAGS]
  test rax, CORO_ALIVE
  jz .Lcoro_unpark_ret
  and rax, ~CORO_PARKED
  mov qword ptr [rcx + CORO_OFF_FLAGS], rax
.Lcoro_unpark_ret:
  ret

.globl jogrt_coro_alive
.type jogrt_coro_alive, @function
jogrt_coro_alive:
  call jogrt_coro_init
  xor eax, eax
  mov rcx, qword ptr [rip + jogrt_coro_count]
  xor edx, edx
.Lcoro_alive_loop:
  cmp rdx, rcx
  jae .Lcoro_alive_done
  mov rsi, rdx
  imul rsi, rsi, JOGRT_CORO_SLOT
  lea rdi, [rip + jogrt_coro_slots]
  add rdi, rsi
  mov rsi, qword ptr [rdi + CORO_OFF_FLAGS]
  test rsi, CORO_ALIVE
  jz .Lcoro_alive_next
  inc rax
.Lcoro_alive_next:
  inc rdx
  jmp .Lcoro_alive_loop
.Lcoro_alive_done:
  ret

.globl jogrt_coro_id
.type jogrt_coro_id, @function
jogrt_coro_id:
  call jogrt_coro_init
  mov rax, qword ptr [rip + jogrt_coro_current]
  ret

.globl jogrt_coro_drain
.type jogrt_coro_drain, @function
jogrt_coro_drain:
  push rbx
  push r12
.Lcoro_drain_loop:
  call jogrt_coro_alive
  cmp rax, 1
  jbe .Lcoro_drain_done
  mov r12, qword ptr [rip + jogrt_coro_count]
  mov rdx, qword ptr [rip + jogrt_coro_current]
  xor ebx, ebx
  xor esi, esi
.Lcoro_drain_scan:
  cmp rsi, r12
  jae .Lcoro_drain_check
  cmp rsi, rdx
  je .Lcoro_drain_scan_next
  mov rax, rsi
  imul rax, rax, JOGRT_CORO_SLOT
  lea rdi, [rip + jogrt_coro_slots]
  add rdi, rax
  mov rax, qword ptr [rdi + CORO_OFF_FLAGS]
  test rax, CORO_ALIVE
  jz .Lcoro_drain_scan_next
  test rax, CORO_PARKED
  jnz .Lcoro_drain_scan_next
  mov ebx, 1
  jmp .Lcoro_drain_check
.Lcoro_drain_scan_next:
  inc rsi
  jmp .Lcoro_drain_scan
.Lcoro_drain_check:
  test ebx, ebx
  jz .Lcoro_drain_done
  call jogrt_yield
  jmp .Lcoro_drain_loop
.Lcoro_drain_done:
  pop r12
  pop rbx
  ret

.section .rodata
jogrt_coro_oom_msg:
  .ascii "jogrt: coro stack mmap failed\n"
jogrt_coro_oom_msg_len = . - jogrt_coro_oom_msg
jogrt_coro_go_fail_msg:
  .ascii "jogrt: coro go failed\n"
jogrt_coro_go_fail_msg_len = . - jogrt_coro_go_fail_msg
