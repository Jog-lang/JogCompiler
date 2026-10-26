#if !defined(__clang__) && !defined(__GNUC__)
#error coroutines require GCC or Clang
#endif

#if !defined(__x86_64__) && !defined(_M_X64)
#error coroutines require an x86_64 target
#endif

#if !defined(_WIN32)
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* Ten words: rbx, rbp, r12, rsp, r13, r14, r15, resume address, rdi, rsi.
   rdi and rsi are callee-saved on the Windows x64 ABI. XMM state is not
   stored here. The set site clobbers xmm0-15, and ymm0-15 when AVX is
   on, so the compiler spills callee-saved XMM around that site. */

enum {
    CORO_CTX_RBX = 0,
    CORO_CTX_RBP = 1,
    CORO_CTX_R12 = 2,
    CORO_CTX_RSP = 3,
    CORO_CTX_R13 = 4,
    CORO_CTX_R14 = 5,
    CORO_CTX_R15 = 6,
    CORO_CTX_RIP = 7,
    CORO_CTX_RDI = 8,
    CORO_CTX_RSI = 9,
    CORO_CTX_WORDS = 10,
    CORO_STACK_BYTES = 64 * 1024
};

_Static_assert(CORO_CTX_WORDS == 10, "context is ten uint64_t words");

extern void coro_trampoline(void);

#if defined(__CET__) && (__CET__ & 1)
#define CORO_ENDBR "endbr64\n\t"
#define CORO_STUB_ENDBR "endbr64\n"
#else
#define CORO_ENDBR ""
#define CORO_STUB_ENDBR ""
#endif

#if defined(__ELF__)
#define CORO_STUB_TYPE ".type coro_entry_stub, @function\n"
#define CORO_STUB_SIZE ".size coro_entry_stub, .-coro_entry_stub\n"
#else
#define CORO_STUB_TYPE ""
#define CORO_STUB_SIZE ""
#endif

#if defined(__AVX__)
#define CORO_VEC_CLOBBERS \
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", \
    "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7", \
    "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15"
#else
#define CORO_VEC_CLOBBERS \
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
#endif

/* First resume address. Aligns rsp, reserves the 32-byte Windows shadow
   space (unused on System V), and calls the C trampoline. A returned
   trampoline stays here and does not run off the stack. */
__asm__(
    ".text\n"
    ".globl coro_entry_stub\n"
    CORO_STUB_TYPE
    ".align 16\n"
    "coro_entry_stub:\n"
    CORO_STUB_ENDBR
    "andq $-16, %rsp\n"
    "subq $32, %rsp\n"
    "call coro_trampoline\n"
    "1:\n"
    "jmp 1b\n"
    CORO_STUB_SIZE
);

extern void coro_entry_stub(void);

static int coro_set(uint64_t *ctx) {
    uintptr_t rax = (uintptr_t)ctx;
    __asm__ volatile (
        "leaq 1f(%%rip), %%rcx\n\t"
        "movq %%rbx, 0(%%rax)\n\t"
        "movq %%rbp, 8(%%rax)\n\t"
        "movq %%r12, 16(%%rax)\n\t"
        "movq %%rsp, 24(%%rax)\n\t"
        "movq %%r13, 32(%%rax)\n\t"
        "movq %%r14, 40(%%rax)\n\t"
        "movq %%r15, 48(%%rax)\n\t"
        "movq %%rcx, 56(%%rax)\n\t"
        "movq %%rdi, 64(%%rax)\n\t"
        "movq %%rsi, 72(%%rax)\n\t"
        "xorl %%eax, %%eax\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        CORO_ENDBR
        "movl $1, %%eax\n\t"
        "2:\n\t"
        : "+a"(rax)
        :
        : "memory", "cc", "rcx", "rdx", "r8", "r9", "r10", "r11", CORO_VEC_CLOBBERS
    );
    return (int)rax;
}

__attribute__((noreturn))
static void coro_jump(uint64_t *ctx) {
    __asm__ volatile (
        "movq 0(%%rax), %%rbx\n\t"
        "movq 8(%%rax), %%rbp\n\t"
        "movq 16(%%rax), %%r12\n\t"
        "movq 32(%%rax), %%r13\n\t"
        "movq 40(%%rax), %%r14\n\t"
        "movq 48(%%rax), %%r15\n\t"
        "movq 64(%%rax), %%rdi\n\t"
        "movq 72(%%rax), %%rsi\n\t"
        "movq 24(%%rax), %%rsp\n\t"
        "jmpq *56(%%rax)\n\t"
        :
        : "a"(ctx)
        : "memory"
    );
    __builtin_unreachable();
}

void coro_ctx_switch(uint64_t *save, uint64_t *restore) {
    if (coro_set(save) == 0) {
        coro_jump(restore);
    }
}

void coro_ctx_prepare(uint64_t *ctx, void *top) {
    memset(ctx, 0, sizeof(uint64_t) * CORO_CTX_WORDS);
    ctx[CORO_CTX_RSP] = (uint64_t)(uintptr_t)top;
    ctx[CORO_CTX_RIP] = (uint64_t)(uintptr_t)coro_entry_stub;
}

static size_t coro_page_size(void) {
    static size_t page = 0;
    if (page != 0) {
        return page;
    }
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    page = (size_t)info.dwPageSize;
#else
    long value = sysconf(_SC_PAGESIZE);
    if (value > 0) {
        page = (size_t)value;
    }
#endif
    if (page == 0) {
        page = 4096;
    }
    return page;
}

void *coro_stack_top(void *base) {
    size_t page = coro_page_size();
    uintptr_t top = (uintptr_t)base + page + (size_t)CORO_STACK_BYTES;
    top &= ~(uintptr_t)15u;
    return (void *)top;
}

bool coro_stack_alloc(void **base, void **top) {
    if (!base || !top) {
        return false;
    }
    *base = NULL;
    *top = NULL;

    size_t page = coro_page_size();
    if (page == 0 || (page & (page - 1u)) != 0) {
        return false;
    }
    size_t bytes = page + (size_t)CORO_STACK_BYTES;

#if defined(_WIN32)
    void *block = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block) {
        return false;
    }
    DWORD previous = 0;
    if (!VirtualProtect(block, page, PAGE_NOACCESS, &previous)) {
        VirtualFree(block, 0, MEM_RELEASE);
        return false;
    }
#else
    void *block = NULL;
    if (posix_memalign(&block, page, bytes) != 0 || !block) {
        return false;
    }
    if (mprotect(block, page, PROT_NONE) != 0) {
        free(block);
        return false;
    }
#endif

    *base = block;
    *top = coro_stack_top(block);
    return true;
}

void coro_stack_free(void *base) {
    if (!base) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(base, 0, MEM_RELEASE);
#else
    size_t page = coro_page_size();
    mprotect(base, page, PROT_READ | PROT_WRITE);
    free(base);
#endif
}
