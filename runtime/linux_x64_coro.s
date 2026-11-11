# Jog v2 stackful coroutine scheduler (linux x86_64 SysV).
# Gate step 2: multi-worker pool + work-steal; ambient Dispatcher.
# Gate step 1 (rsp swap, park stubs) preserved; single-thread via dispatcher_single.
#
# Slot layout (128 bytes):
#   0x00 rbx  0x08 rbp  0x10 r12  0x18 rsp
#   0x20 r13  0x28 r14  0x30 r15  0x38 rip
#   0x40 rdi  0x48 rsi  0x50 fn   0x58 arg
#   0x60 flags (bit0=alive, bit1=parked)  0x68 stack_base
#
# TLS (FS base → jogrt_tls[wid], 32 bytes):
#   0x00 wid  0x08 current (0=home/sched)  0x10 home_ctx*  0x18 yield_requeue
# yield always hops to home first; home requeues after the rsp save (no dual-run race).
#
# Per-worker locked FIFO queues; steal = take from another worker's head.

.intel_syntax noprefix

.set JOGRT_CORO_MAX, 32
.set JOGRT_CORO_SLOT, 128
.set JOGRT_CORO_STACK, 65536
.set JOGRT_CORO_PAGE, 4096
.set JOGRT_CORO_MAP, (JOGRT_CORO_PAGE + JOGRT_CORO_STACK)

.set JOGRT_MAX_WORKERS, 8
.set JOGRT_DEFAULT_WORKERS, 4
.set JOGRT_QUEUE_CAP, 64
.set JOGRT_WORKER_STACK, 131072
.set JOGRT_TLS_SIZE, 32
.set JOGRT_HOME_SIZE, 128

.set CORO_OFF_RSP, 0x18
.set CORO_OFF_RIP, 0x38
.set CORO_OFF_RDI, 0x40
.set CORO_OFF_FN, 0x50
.set CORO_OFF_ARG, 0x58
.set CORO_OFF_FLAGS, 0x60
.set CORO_OFF_STACK, 0x68

.set CORO_ALIVE, 1
.set CORO_PARKED, 2

.set TLS_OFF_WID, 0
.set TLS_OFF_CUR, 8
.set TLS_OFF_HOME, 16
.set TLS_OFF_YRQ, 24

.set SYS_WRITE, 1
.set SYS_MMAP, 9
.set SYS_MPROTECT, 10
.set SYS_CLONE, 56
.set SYS_EXIT, 60
.set SYS_FUTEX, 202
.set SYS_SCHED_YIELD, 24
.set SYS_GETTID, 186
.set SYS_ARCH_PRCTL, 158
.set ARCH_SET_FS, 0x1002

.set FUTEX_WAIT, 0
.set FUTEX_WAKE, 1

# clone flags: VM|FS|FILES|SIGHAND|THREAD|SYSVSEM
.set CLONE_FLAGS, 0x50F00

.bss
.align 16
jogrt_coro_slots:
  .zero JOGRT_CORO_MAX * JOGRT_CORO_SLOT
jogrt_coro_count:
  .zero 8
jogrt_coro_ready:
  .zero 8
jogrt_slots_lock:
  .zero 8
jogrt_print_lock:
  .zero 8

jogrt_disp_mode:
  .zero 8          # 0=unset(default pool), 1=single, 2=pool
jogrt_nworkers:
  .zero 8
jogrt_pool_started:
  .zero 8
jogrt_rr:
  .zero 8

.align 16
jogrt_tls:
  .zero JOGRT_MAX_WORKERS * JOGRT_TLS_SIZE
jogrt_home_ctx:
  .zero JOGRT_MAX_WORKERS * JOGRT_HOME_SIZE
jogrt_wq_head:
  .zero JOGRT_MAX_WORKERS * 8   # steal / FIFO end
jogrt_wq_tail:
  .zero JOGRT_MAX_WORKERS * 8   # owner push/pop end
jogrt_wq_lock:
  .zero JOGRT_MAX_WORKERS * 8
jogrt_wq_park:
  .zero JOGRT_MAX_WORKERS * 8   # low 32 bits used as futex word
.align 16
jogrt_wq_data:
  .zero JOGRT_MAX_WORKERS * JOGRT_QUEUE_CAP * 8
jogrt_worker_stack_ptr:
  .zero JOGRT_MAX_WORKERS * 8
jogrt_tid_buf:
  .zero 64

.text

# ---------------------------------------------------------------------------
# Spinlocks
# ---------------------------------------------------------------------------
.type jogrt_lock, @function
jogrt_lock:
  mov eax, 1
.Llock_spin:
  xchg eax, dword ptr [rdi]
  test eax, eax
  jz .Llock_got
  pause
  mov eax, 1
  jmp .Llock_spin
.Llock_got:
  ret

.type jogrt_unlock, @function
jogrt_unlock:
  mov dword ptr [rdi], 0
  ret

# ---------------------------------------------------------------------------
# Coro switch (unchanged SysV callee-saved + rsp/rip)
# rdi = save ctx, rsi = restore ctx
# ---------------------------------------------------------------------------
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
  mov rax, qword ptr fs:[TLS_OFF_CUR]
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  mov rax, qword ptr [rcx + CORO_OFF_FN]
  mov rdi, qword ptr [rcx + CORO_OFF_ARG]
  call rax
  # finished: clear alive under lock
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_lock
  mov rax, qword ptr fs:[TLS_OFF_CUR]
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  mov qword ptr [rcx + CORO_OFF_FLAGS], 0
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_unlock
.Lcoro_tramp_yield:
  call jogrt_yield
  jmp .Lcoro_tramp_yield

# ---------------------------------------------------------------------------
# Init / TLS / ambient Dispatcher
# ---------------------------------------------------------------------------
.type jogrt_tls_install, @function
# rdi = wid
jogrt_tls_install:
  push rbx
  mov rbx, rdi
  mov rax, rbx
  imul rax, rax, JOGRT_TLS_SIZE
  lea rcx, [rip + jogrt_tls]
  add rcx, rax
  mov qword ptr [rcx + TLS_OFF_WID], rbx
  mov qword ptr [rcx + TLS_OFF_CUR], 0
  mov qword ptr [rcx + TLS_OFF_YRQ], 0
  mov rax, rbx
  imul rax, rax, JOGRT_HOME_SIZE
  lea rdx, [rip + jogrt_home_ctx]
  add rdx, rax
  mov qword ptr [rcx + TLS_OFF_HOME], rdx
  # arch_prctl(ARCH_SET_FS, tls)
  mov eax, SYS_ARCH_PRCTL
  mov edi, ARCH_SET_FS
  mov rsi, rcx
  syscall
  pop rbx
  ret

.type jogrt_coro_init, @function
jogrt_coro_init:
  mov rax, qword ptr [rip + jogrt_coro_ready]
  test rax, rax
  jnz .Lcoro_init_done
  push rbx
  lea rdi, [rip + jogrt_coro_slots]
  mov ecx, (JOGRT_CORO_MAX * JOGRT_CORO_SLOT) / 8
  xor eax, eax
  rep stosq
  mov qword ptr [rip + jogrt_coro_slots + CORO_OFF_FLAGS], CORO_ALIVE
  mov qword ptr [rip + jogrt_coro_count], 1
  # default ambient = pool with JOGRT_DEFAULT_WORKERS
  mov rax, qword ptr [rip + jogrt_disp_mode]
  test rax, rax
  jnz .Lcoro_init_mode
  mov qword ptr [rip + jogrt_disp_mode], 2
  mov qword ptr [rip + jogrt_nworkers], JOGRT_DEFAULT_WORKERS
.Lcoro_init_mode:
  mov rax, qword ptr [rip + jogrt_nworkers]
  test rax, rax
  jnz .Lcoro_init_nw
  mov qword ptr [rip + jogrt_nworkers], 1
.Lcoro_init_nw:
  cmp qword ptr [rip + jogrt_nworkers], JOGRT_MAX_WORKERS
  jbe .Lcoro_init_nw_ok
  mov qword ptr [rip + jogrt_nworkers], JOGRT_MAX_WORKERS
.Lcoro_init_nw_ok:
  # install worker 0 TLS (main). Slot 0 is main; home_ctx[0] mirrors sched saves.
  xor edi, edi
  call jogrt_tls_install
  # For worker 0, use slot 0 as home so main's OS stack is saved into the main CCB
  # when switching away — match gate-1 mental model.
  lea rax, [rip + jogrt_coro_slots]
  mov qword ptr fs:[TLS_OFF_HOME], rax
  mov qword ptr fs:[TLS_OFF_CUR], 0
  mov qword ptr [rip + jogrt_coro_ready], 1
  pop rbx
.Lcoro_init_done:
  ret

# dispatcher_single(): ambient single-thread (tests / deterministic yield demos)
.globl jogrt_dispatcher_single
.type jogrt_dispatcher_single, @function
jogrt_dispatcher_single:
  cmp qword ptr [rip + jogrt_pool_started], 0
  jne .Ldisp_single_late
  mov qword ptr [rip + jogrt_disp_mode], 1
  mov qword ptr [rip + jogrt_nworkers], 1
  call jogrt_coro_init
  xor eax, eax
  ret
.Ldisp_single_late:
  lea rdi, [rip + jogrt_disp_late_msg]
  mov esi, jogrt_disp_late_msg_len
  call jogrt_panic

# dispatcher_pool(n): n==0 → default; ambient multi-worker
.globl jogrt_dispatcher_pool
.type jogrt_dispatcher_pool, @function
jogrt_dispatcher_pool:
  push rbx
  mov rbx, rdi
  cmp qword ptr [rip + jogrt_pool_started], 0
  jne .Ldisp_pool_late
  mov qword ptr [rip + jogrt_disp_mode], 2
  test rbx, rbx
  jnz .Ldisp_pool_n
  mov rbx, JOGRT_DEFAULT_WORKERS
.Ldisp_pool_n:
  cmp rbx, 1
  jae .Ldisp_pool_ge1
  mov rbx, 1
.Ldisp_pool_ge1:
  cmp rbx, JOGRT_MAX_WORKERS
  jbe .Ldisp_pool_set
  mov rbx, JOGRT_MAX_WORKERS
.Ldisp_pool_set:
  mov qword ptr [rip + jogrt_nworkers], rbx
  call jogrt_coro_init
  xor eax, eax
  pop rbx
  ret
.Ldisp_pool_late:
  lea rdi, [rip + jogrt_disp_late_msg]
  mov esi, jogrt_disp_late_msg_len
  call jogrt_panic

# ---------------------------------------------------------------------------
# Stack alloc / arm (same as gate 1)
# ---------------------------------------------------------------------------
.type jogrt_coro_stack_alloc, @function
jogrt_coro_stack_alloc:
  push rbx
  mov eax, SYS_MMAP
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
  mov eax, SYS_MPROTECT
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

# ---------------------------------------------------------------------------
# Work queues: locked deque (owner pops tail; thieves steal head)
# ---------------------------------------------------------------------------
.type jogrt_wq_push, @function
# rdi = wid, rsi = coro index
jogrt_wq_push:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12, rsi
  lea rdi, [rip + jogrt_wq_lock]
  lea rdi, [rdi + rbx * 8]
  call jogrt_lock
  mov rax, rbx
  imul rax, rax, 8
  lea rdx, [rip + jogrt_wq_tail]
  mov r8, qword ptr [rdx + rax]          # tail
  lea rdx, [rip + jogrt_wq_head]
  mov r9, qword ptr [rdx + rax]          # head
  mov r10, r8
  inc r10
  mov r13, r10
  and r13, JOGRT_QUEUE_CAP - 1
  mov r11, r9
  and r11, JOGRT_QUEUE_CAP - 1
  cmp r13, r11
  je .Lwq_push_full                      # full: drop lock and panic
  mov r10, r8
  and r10, JOGRT_QUEUE_CAP - 1
  mov rax, rbx
  imul rax, rax, JOGRT_QUEUE_CAP
  add rax, r10
  lea rdx, [rip + jogrt_wq_data]
  mov qword ptr [rdx + rax * 8], r12
  lea rdx, [rip + jogrt_wq_tail]
  mov rax, rbx
  inc r8
  mov qword ptr [rdx + rax * 8], r8
  lea rdi, [rip + jogrt_wq_lock]
  lea rdi, [rdi + rbx * 8]
  call jogrt_unlock
  pop r13
  pop r12
  pop rbx
  ret
.Lwq_push_full:
  lea rdi, [rip + jogrt_wq_lock]
  lea rdi, [rdi + rbx * 8]
  call jogrt_unlock
  lea rdi, [rip + jogrt_wq_full_msg]
  mov esi, jogrt_wq_full_msg_len
  call jogrt_panic

.type jogrt_wq_pop, @function
# rdi = wid → rax = coro or -1 (FIFO from head; matches go enqueue order)
jogrt_wq_pop:
  mov rdi, rdi
  jmp jogrt_wq_steal

.type jogrt_wq_steal, @function
# rdi = victim wid → rax = coro or -1
jogrt_wq_steal:
  push rbx
  mov rbx, rdi
  lea rdi, [rip + jogrt_wq_lock]
  lea rdi, [rdi + rbx * 8]
  call jogrt_lock
  mov rax, rbx
  imul rax, rax, 8
  lea rdx, [rip + jogrt_wq_tail]
  mov r8, qword ptr [rdx + rax]
  lea rdx, [rip + jogrt_wq_head]
  mov r9, qword ptr [rdx + rax]
  cmp r8, r9
  je .Lwq_steal_empty
  mov r10, r9
  and r10, JOGRT_QUEUE_CAP - 1
  mov rax, rbx
  imul rax, rax, JOGRT_QUEUE_CAP
  add rax, r10
  lea rdx, [rip + jogrt_wq_data]
  mov rsi, qword ptr [rdx + rax * 8]
  inc r9
  lea rdx, [rip + jogrt_wq_head]
  mov qword ptr [rdx + rbx * 8], r9
  lea rdi, [rip + jogrt_wq_lock]
  lea rdi, [rdi + rbx * 8]
  call jogrt_unlock
  mov rax, rsi
  pop rbx
  ret
.Lwq_steal_empty:
  lea rdi, [rip + jogrt_wq_lock]
  lea rdi, [rdi + rbx * 8]
  call jogrt_unlock
  mov rax, -1
  pop rbx
  ret

.type jogrt_take_work, @function
# → rax = coro index or -1
jogrt_take_work:
  push rbx
  push r12
  mov rbx, qword ptr fs:[TLS_OFF_WID]
  mov rdi, rbx
  call jogrt_wq_pop
  cmp rax, -1
  jne .Ltake_done
  # steal from others
  mov r12, qword ptr [rip + jogrt_nworkers]
  xor ebx, ebx
.Ltake_steal_loop:
  cmp rbx, r12
  jae .Ltake_none
  cmp rbx, qword ptr fs:[TLS_OFF_WID]
  je .Ltake_steal_next
  mov rdi, rbx
  call jogrt_wq_steal
  cmp rax, -1
  jne .Ltake_done
.Ltake_steal_next:
  inc rbx
  jmp .Ltake_steal_loop
.Ltake_none:
  mov rax, -1
.Ltake_done:
  pop r12
  pop rbx
  ret

.type jogrt_wake_one, @function
jogrt_wake_one:
  push rbx
  mov rbx, qword ptr [rip + jogrt_nworkers]
  xor ecx, ecx
.Lwake_scan:
  cmp rcx, rbx
  jae .Lwake_done
  lea rdx, [rip + jogrt_wq_park]
  cmp dword ptr [rdx + rcx * 8], 1
  jne .Lwake_next
  mov dword ptr [rdx + rcx * 8], 0
  # futex wake
  push rcx
  lea rdi, [rdx + rcx * 8]
  mov eax, SYS_FUTEX
  mov esi, FUTEX_WAKE
  mov edx, 1
  xor r10d, r10d
  syscall
  pop rcx
  jmp .Lwake_done
.Lwake_next:
  inc rcx
  jmp .Lwake_scan
.Lwake_done:
  pop rbx
  ret

.type jogrt_park_self, @function
jogrt_park_self:
  mov rcx, qword ptr fs:[TLS_OFF_WID]
  lea rdx, [rip + jogrt_wq_park]
  mov dword ptr [rdx + rcx * 8], 1
  # recheck work before sleep
  push rcx
  call jogrt_take_work
  pop rcx
  cmp rax, -1
  jne .Lpark_got
  lea rdi, [rip + jogrt_wq_park]
  lea rdi, [rdi + rcx * 8]
  mov eax, SYS_FUTEX
  mov esi, FUTEX_WAIT
  mov edx, 1
  xor r10d, r10d
  syscall
  mov rcx, qword ptr fs:[TLS_OFF_WID]
  lea rdx, [rip + jogrt_wq_park]
  mov dword ptr [rdx + rcx * 8], 0
  mov rax, -1
  ret
.Lpark_got:
  # undo park flag; return work in rax
  push rax
  mov rcx, qword ptr fs:[TLS_OFF_WID]
  lea rdx, [rip + jogrt_wq_park]
  mov dword ptr [rdx + rcx * 8], 0
  pop rax
  ret

# ---------------------------------------------------------------------------
# Switch helpers (multi-worker aware)
# ---------------------------------------------------------------------------
.type jogrt_save_ctx_ptr, @function
# → rax = ctx* for currently running (home or slot)
jogrt_save_ctx_ptr:
  mov rax, qword ptr fs:[TLS_OFF_CUR]
  test rax, rax
  jnz .Lsave_slot
  mov rax, qword ptr fs:[TLS_OFF_HOME]
  ret
.Lsave_slot:
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rax, rcx
  ret

.type jogrt_switch_to_coro, @function
# rdi = destination coro index (must be >0 user coro, or 0 only on worker0 main)
jogrt_switch_to_coro:
  push rbx
  mov rbx, rdi
  call jogrt_save_ctx_ptr
  mov r8, rax                            # save ctx
  mov qword ptr fs:[TLS_OFF_CUR], rbx
  imul rbx, rbx, JOGRT_CORO_SLOT
  lea rax, [rip + jogrt_coro_slots]
  add rax, rbx
  mov rdi, r8
  mov rsi, rax
  pop rbx
  jmp jogrt_coro_switch

.type jogrt_switch_to_home, @function
jogrt_switch_to_home:
  call jogrt_save_ctx_ptr
  mov rdi, rax
  mov qword ptr fs:[TLS_OFF_CUR], 0
  mov rsi, qword ptr fs:[TLS_OFF_HOME]
  cmp rdi, rsi
  je .Lsw_home_same
  jmp jogrt_coro_switch
.Lsw_home_same:
  ret

# ---------------------------------------------------------------------------
# Pool start / worker entry
# ---------------------------------------------------------------------------
# After a coro switches home, push tls.yield_requeue if set (coro already saved).
.type jogrt_handle_requeue, @function
jogrt_handle_requeue:
  mov rsi, qword ptr fs:[TLS_OFF_YRQ]
  test rsi, rsi
  jz .Lhrq_done
  mov qword ptr fs:[TLS_OFF_YRQ], 0
  # push onto next worker to encourage OS-thread hop when nworkers>1
  mov rax, qword ptr fs:[TLS_OFF_WID]
  inc rax
  xor edx, edx
  div qword ptr [rip + jogrt_nworkers]
  mov rdi, rdx
  call jogrt_wq_push
  call jogrt_wake_one
.Lhrq_done:
  ret

.type jogrt_worker_entry, @function
# On entry after clone: wid is in r12 (parent arranged)
jogrt_worker_entry:
  and rsp, -16
  mov rdi, r12
  call jogrt_tls_install
.Lworker_loop:
  call jogrt_handle_requeue
  call jogrt_take_work
  cmp rax, -1
  jne .Lworker_run
  call jogrt_park_self
  cmp rax, -1
  je .Lworker_loop
.Lworker_run:
  mov rdi, rax
  call jogrt_switch_to_coro
  jmp .Lworker_loop

.type jogrt_pool_ensure, @function
jogrt_pool_ensure:
  mov rax, qword ptr [rip + jogrt_pool_started]
  test rax, rax
  jnz .Lpool_ens_done
  cmp qword ptr [rip + jogrt_disp_mode], 1
  je .Lpool_ens_single
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov qword ptr [rip + jogrt_pool_started], 1
  mov r13, qword ptr [rip + jogrt_nworkers]
  mov r12, 1
.Lpool_spawn:
  cmp r12, r13
  jae .Lpool_spawn_done
  # mmap worker stack
  mov eax, SYS_MMAP
  xor edi, edi
  mov esi, JOGRT_WORKER_STACK
  mov edx, 3
  mov r10d, 0x22
  mov r8, -1
  xor r9d, r9d
  syscall
  cmp rax, -4095
  jae .Lpool_spawn_fail
  mov r14, rax
  mov rax, r12
  lea rcx, [rip + jogrt_worker_stack_ptr]
  mov qword ptr [rcx + rax * 8], r14
  # stack top
  lea r15, [r14 + JOGRT_WORKER_STACK]
  and r15, -16
  # clone
  mov eax, SYS_CLONE
  mov edi, CLONE_FLAGS
  mov rsi, r15
  xor edx, edx
  xor r10d, r10d
  xor r8d, r8d
  syscall
  test rax, rax
  jz .Lpool_child
  # parent: next
  inc r12
  jmp .Lpool_spawn
.Lpool_child:
  # child: r12 still holds wid
  jmp jogrt_worker_entry
.Lpool_spawn_done:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.Lpool_ens_single:
  mov qword ptr [rip + jogrt_pool_started], 1
.Lpool_ens_done:
  ret
.Lpool_spawn_fail:
  lea rdi, [rip + jogrt_pool_fail_msg]
  mov esi, jogrt_pool_fail_msg_len
  call jogrt_panic

# ---------------------------------------------------------------------------
# go / yield / park / unpark / drain
# ---------------------------------------------------------------------------
.globl jogrt_go
.type jogrt_go, @function
jogrt_go:
  push rbx
  push r12
  push r13
  push r14
  mov r12, rdi
  mov r13, rsi
  call jogrt_coro_init
  call jogrt_pool_ensure
  test r12, r12
  jz .Lcoro_go_fail
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_lock
  mov rcx, qword ptr [rip + jogrt_coro_count]
  mov rbx, 1
.Lcoro_go_scan:
  cmp rbx, rcx
  jae .Lcoro_go_new
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
  mov r14, rbx
  jmp .Lcoro_go_enqueue
.Lcoro_go_scan_next:
  inc rbx
  jmp .Lcoro_go_scan
.Lcoro_go_new:
  cmp rcx, JOGRT_CORO_MAX
  jae .Lcoro_go_fail_unlock
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
  mov r14, rbx
.Lcoro_go_enqueue:
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_unlock
  # enqueue onto ambient dispatcher: RR across workers
  mov rax, qword ptr [rip + jogrt_rr]
  mov rcx, rax
  inc rax
  mov qword ptr [rip + jogrt_rr], rax
  xor edx, edx
  div qword ptr [rip + jogrt_nworkers]
  # rdx = target wid
  mov rdi, rdx
  mov rsi, r14
  call jogrt_wq_push
  call jogrt_wake_one
  xor eax, eax
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.Lcoro_go_fail_unlock:
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_unlock
.Lcoro_go_fail:
  lea rdi, [rip + jogrt_coro_go_fail_msg]
  mov esi, jogrt_coro_go_fail_msg_len
  call jogrt_panic

.globl jogrt_yield
.type jogrt_yield, @function
jogrt_yield:
  call jogrt_coro_init
  mov rax, qword ptr fs:[TLS_OFF_CUR]
  # already home: try to run someone else, else no-op
  test rax, rax
  jz .Lyield_from_home
  # mark requeue only if still runnable; home pushes AFTER rsp save
  mov rsi, rax
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  mov rax, qword ptr [rcx + CORO_OFF_FLAGS]
  mov qword ptr fs:[TLS_OFF_YRQ], 0
  test rax, CORO_ALIVE
  jz .Lyield_to_home
  test rax, CORO_PARKED
  jnz .Lyield_to_home
  mov qword ptr fs:[TLS_OFF_YRQ], rsi
.Lyield_to_home:
  jmp jogrt_switch_to_home
.Lyield_from_home:
  call jogrt_handle_requeue
  call jogrt_take_work
  cmp rax, -1
  je .Lyield_noop
  mov rdi, rax
  jmp jogrt_switch_to_coro
.Lyield_noop:
  ret

.globl jogrt_coro_park
.type jogrt_coro_park, @function
jogrt_coro_park:
  call jogrt_coro_init
  mov rax, qword ptr fs:[TLS_OFF_CUR]
  test rax, rax
  jz .Lpark_ret
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  lea rdi, [rip + jogrt_slots_lock]
  push rcx
  call jogrt_lock
  pop rcx
  mov rax, qword ptr [rcx + CORO_OFF_FLAGS]
  or rax, CORO_PARKED
  mov qword ptr [rcx + CORO_OFF_FLAGS], rax
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_unlock
  jmp jogrt_yield
.Lpark_ret:
  ret

.globl jogrt_coro_unpark
.type jogrt_coro_unpark, @function
jogrt_coro_unpark:
  push rbx
  mov rbx, rdi
  call jogrt_coro_init
  cmp rbx, JOGRT_CORO_MAX
  jae .Lunpark_ret
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_lock
  mov rax, rbx
  imul rax, rax, JOGRT_CORO_SLOT
  lea rcx, [rip + jogrt_coro_slots]
  add rcx, rax
  mov rax, qword ptr [rcx + CORO_OFF_FLAGS]
  test rax, CORO_ALIVE
  jz .Lunpark_unlock
  and rax, ~CORO_PARKED
  mov qword ptr [rcx + CORO_OFF_FLAGS], rax
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_unlock
  # enqueue RR
  mov rax, qword ptr [rip + jogrt_rr]
  mov rcx, rax
  inc rax
  mov qword ptr [rip + jogrt_rr], rax
  xor edx, edx
  div qword ptr [rip + jogrt_nworkers]
  mov rdi, rdx
  mov rsi, rbx
  call jogrt_wq_push
  call jogrt_wake_one
  pop rbx
  ret
.Lunpark_unlock:
  lea rdi, [rip + jogrt_slots_lock]
  call jogrt_unlock
.Lunpark_ret:
  pop rbx
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
  mov rax, qword ptr fs:[TLS_OFF_CUR]
  ret

.globl jogrt_worker_id
.type jogrt_worker_id, @function
jogrt_worker_id:
  call jogrt_coro_init
  mov rax, qword ptr fs:[TLS_OFF_WID]
  ret

.globl jogrt_gettid
.type jogrt_gettid, @function
jogrt_gettid:
  mov eax, SYS_GETTID
  syscall
  ret

# print "tid=<tid> wid=<wid> cid=<coro>\n" (locked). Same cid + different tid ⇒ hop.
.globl jogrt_print_tid
.type jogrt_print_tid, @function
jogrt_print_tid:
  push rbx
  push r12
  push r13
  push r14
  call jogrt_coro_init
  mov eax, SYS_GETTID
  syscall
  mov r12, rax
  mov r13, qword ptr fs:[TLS_OFF_WID]
  mov r14, qword ptr fs:[TLS_OFF_CUR]
  lea rdi, [rip + jogrt_print_lock]
  call jogrt_lock
  lea rbx, [rip + jogrt_tid_buf]
  mov byte ptr [rbx], 't'
  mov byte ptr [rbx+1], 'i'
  mov byte ptr [rbx+2], 'd'
  mov byte ptr [rbx+3], '='
  lea rdi, [rbx + 4]
  mov rsi, r12
  call jogrt_u64_to_dec
  mov r8, rax
  mov byte ptr [r8], ' '
  inc r8
  mov byte ptr [r8], 'w'
  inc r8
  mov byte ptr [r8], 'i'
  inc r8
  mov byte ptr [r8], 'd'
  inc r8
  mov byte ptr [r8], '='
  inc r8
  mov rdi, r8
  mov rsi, r13
  call jogrt_u64_to_dec
  mov r8, rax
  mov byte ptr [r8], ' '
  inc r8
  mov byte ptr [r8], 'c'
  inc r8
  mov byte ptr [r8], 'i'
  inc r8
  mov byte ptr [r8], 'd'
  inc r8
  mov byte ptr [r8], '='
  inc r8
  mov rdi, r8
  mov rsi, r14
  call jogrt_u64_to_dec
  mov byte ptr [rax], 10
  inc rax
  mov rdx, rax
  lea rsi, [rip + jogrt_tid_buf]
  sub rdx, rsi
  mov edi, 1
  mov eax, SYS_WRITE
  syscall
  lea rdi, [rip + jogrt_print_lock]
  call jogrt_unlock
  xor eax, eax
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

# rdi = dest buf, rsi = value → rax = end ptr (after digits)
.type jogrt_u64_to_dec, @function
jogrt_u64_to_dec:
  push rbx
  push r12
  mov rax, rsi
  mov r12, rdi
  mov rbx, rdi
  test rax, rax
  jnz .Ldec_loop
  mov byte ptr [rbx], '0'
  lea rax, [rbx + 1]
  pop r12
  pop rbx
  ret
.Ldec_loop:
  xor rdx, rdx
  mov rcx, 10
  div rcx
  add dl, '0'
  mov byte ptr [rbx], dl
  inc rbx
  test rax, rax
  jnz .Ldec_loop
  # reverse [r12, rbx)
  mov rcx, rbx
  dec rcx
  mov rsi, r12
.Ldec_rev:
  cmp rsi, rcx
  jae .Ldec_done
  mov al, byte ptr [rsi]
  mov dl, byte ptr [rcx]
  mov byte ptr [rsi], dl
  mov byte ptr [rcx], al
  inc rsi
  dec rcx
  jmp .Ldec_rev
.Ldec_done:
  mov rax, rbx
  pop r12
  pop rbx
  ret

.type jogrt_queues_empty, @function
# → rax = 1 if all empty else 0
jogrt_queues_empty:
  mov rcx, qword ptr [rip + jogrt_nworkers]
  xor edx, edx
.Lqe_loop:
  cmp rdx, rcx
  jae .Lqe_yes
  mov rax, rdx
  imul rax, rax, 8
  lea rsi, [rip + jogrt_wq_tail]
  mov r8, qword ptr [rsi + rax]
  lea rsi, [rip + jogrt_wq_head]
  mov r9, qword ptr [rsi + rax]
  cmp r8, r9
  jne .Lqe_no
  inc rdx
  jmp .Lqe_loop
.Lqe_yes:
  mov eax, 1
  ret
.Lqe_no:
  xor eax, eax
  ret

.globl jogrt_coro_drain
.type jogrt_coro_drain, @function
jogrt_coro_drain:
  push rbx
  push r12
  call jogrt_coro_init
  call jogrt_pool_ensure
.Ldrain_loop:
  call jogrt_handle_requeue
  call jogrt_coro_alive
  cmp rax, 1
  ja .Ldrain_work
  call jogrt_queues_empty
  test eax, eax
  jz .Ldrain_work
  # alive<=1 and queues empty — also wait until no yield_requeue
  cmp qword ptr fs:[TLS_OFF_YRQ], 0
  je .Ldrain_done
.Ldrain_work:
  call jogrt_take_work
  cmp rax, -1
  je .Ldrain_idle
  mov rdi, rax
  call jogrt_switch_to_coro
  jmp .Ldrain_loop
.Ldrain_idle:
  call jogrt_coro_alive
  cmp rax, 1
  jbe .Ldrain_check_q
  mov eax, SYS_SCHED_YIELD
  syscall
  jmp .Ldrain_loop
.Ldrain_check_q:
  call jogrt_queues_empty
  test eax, eax
  jnz .Ldrain_done
  jmp .Ldrain_loop
.Ldrain_done:
  pop r12
  pop rbx
  ret

# Locked print used from language print under multi-thread (optional hook).
# Existing jogrt_print stays unlocked; print_tid uses jogrt_print_lock.

.section .rodata
jogrt_coro_oom_msg:
  .ascii "jogrt: coro stack mmap failed\n"
jogrt_coro_oom_msg_len = . - jogrt_coro_oom_msg
jogrt_coro_go_fail_msg:
  .ascii "jogrt: coro go failed\n"
jogrt_coro_go_fail_msg_len = . - jogrt_coro_go_fail_msg
jogrt_pool_fail_msg:
  .ascii "jogrt: worker clone/mmap failed\n"
jogrt_pool_fail_msg_len = . - jogrt_pool_fail_msg
jogrt_disp_late_msg:
  .ascii "jogrt: dispatcher change after pool start\n"
jogrt_disp_late_msg_len = . - jogrt_disp_late_msg
jogrt_wq_full_msg:
  .ascii "jogrt: worker run queue full\n"
jogrt_wq_full_msg_len = . - jogrt_wq_full_msg
