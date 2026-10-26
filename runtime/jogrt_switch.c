#if !defined(__clang__) && !defined(__GNUC__)
#error coroutines require GCC or Clang
#endif

#if !defined(__x86_64__) && !defined(_M_X64)
#error coroutines require an x86_64 target
#endif

/* sigaltstack, stack_t, and SA_ONSTACK are X/Open, not POSIX.1-2001. */
#if !defined(_WIN32)
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 700
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

#include "jogrt_switch.h"

#include "jogrt.h"

#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* Ten words: rbx, rbp, r12, rsp, r13, r14, r15, resume address, rdi, rsi.
   rdi and rsi are callee-saved on the Windows x64 ABI. XMM state is not
   stored. The set site clobbers xmm0-15, so the compiler spills callee-saved
   XMM around that site. Same layout as tools/coro_switch.c. */

enum {
    JOGRT_CTX_RBX = 0,
    JOGRT_CTX_RBP = 1,
    JOGRT_CTX_R12 = 2,
    JOGRT_CTX_RSP = 3,
    JOGRT_CTX_R13 = 4,
    JOGRT_CTX_R14 = 5,
    JOGRT_CTX_R15 = 6,
    JOGRT_CTX_RIP = 7,
    JOGRT_CTX_RDI = 8,
    JOGRT_CTX_RSI = 9
};

extern void jogrt_trampoline(void);

#if defined(__CET__) && (__CET__ & 1)
#define JOGRT_ENDBR "endbr64\n\t"
#define JOGRT_STUB_ENDBR "endbr64\n"
#else
#define JOGRT_ENDBR ""
#define JOGRT_STUB_ENDBR ""
#endif

#if defined(__ELF__)
#define JOGRT_STUB_TYPE ".type jogrt_entry_stub, @function\n"
#define JOGRT_STUB_SIZE ".size jogrt_entry_stub, .-jogrt_entry_stub\n"
#else
#define JOGRT_STUB_TYPE ""
#define JOGRT_STUB_SIZE ""
#endif

#if defined(__AVX__)
#define JOGRT_VEC_CLOBBERS \
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", \
    "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7", \
    "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15"
#else
#define JOGRT_VEC_CLOBBERS \
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
#endif

/* First resume address. Aligns rsp, reserves the 32-byte Windows shadow
   space (unused on System V), and calls the C trampoline. */
__asm__(
    ".text\n"
    ".globl jogrt_entry_stub\n"
    JOGRT_STUB_TYPE
    ".align 16\n"
    "jogrt_entry_stub:\n"
    JOGRT_STUB_ENDBR
    "andq $-16, %rsp\n"
    "subq $32, %rsp\n"
    "call jogrt_trampoline\n"
    "1:\n"
    "jmp 1b\n"
    JOGRT_STUB_SIZE
);

extern void jogrt_entry_stub(void);

static int jogrt_set(uint64_t *ctx) {
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
        JOGRT_ENDBR
        "movl $1, %%eax\n\t"
        "2:\n\t"
        : "+a"(rax)
        :
        : "memory", "cc", "rcx", "rdx", "r8", "r9", "r10", "r11", JOGRT_VEC_CLOBBERS
    );
    return (int)rax;
}

__attribute__((noreturn))
static void jogrt_jump(uint64_t *ctx) {
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

void jogrt_ctx_switch(uint64_t *save, uint64_t *restore) {
    if (jogrt_set(save) == 0) {
        jogrt_jump(restore);
    }
}

void jogrt_ctx_prepare(uint64_t *ctx, void *top) {
    memset(ctx, 0, sizeof(uint64_t) * JOGRT_CTX_WORDS);
    ctx[JOGRT_CTX_RSP] = (uint64_t)(uintptr_t)top;
    ctx[JOGRT_CTX_RIP] = (uint64_t)(uintptr_t)jogrt_entry_stub;
}

static size_t jogrt_page_size(void) {
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

enum { JOGRT_STACK_DEFAULT = 256 * 1024, JOGRT_REGION_MAX = 4096 };

typedef struct JogrtRegion {
    uintptr_t base;
    uintptr_t guard_end;
    uintptr_t end;
} JogrtRegion;

static JogrtRegion g_regions[JOGRT_REGION_MAX];
static size_t g_region_count;
static int g_guard_ready;

static void jogrt_guard_hit(void) {
    static const char msg[] = "stack overflow";
    jogrt_panic(msg, sizeof(msg) - 1, "jogrt", 0);
}

#if defined(_WIN32)
static LONG CALLBACK jogrt_guard_fault(EXCEPTION_POINTERS *info) {
    uintptr_t addr;
    size_t i;
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    addr = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
    for (i = 0; i < g_region_count; i++) {
        if (addr >= g_regions[i].base && addr < g_regions[i].guard_end) {
            jogrt_guard_hit();
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static struct sigaction g_prev_segv;
/* The guard hit runs here, not on the coroutine stack that just faulted.
   64 KiB is above MINSIGSTKSZ on x86-64, including the AVX variants. */
static unsigned char g_alt_stack[64 * 1024] __attribute__((aligned(16)));

static void jogrt_guard_signal(int signo, siginfo_t *info, void *context) {
    uintptr_t addr;
    size_t i;
    addr = info ? (uintptr_t)info->si_addr : 0;
    for (i = 0; i < g_region_count; i++) {
        if (addr >= g_regions[i].base && addr < g_regions[i].guard_end) {
            jogrt_guard_hit();
        }
    }
    if ((g_prev_segv.sa_flags & SA_SIGINFO) && g_prev_segv.sa_sigaction) {
        g_prev_segv.sa_sigaction(signo, info, context);
        return;
    }
    if (g_prev_segv.sa_handler && g_prev_segv.sa_handler != SIG_DFL &&
        g_prev_segv.sa_handler != SIG_IGN) {
        g_prev_segv.sa_handler(signo);
        return;
    }
    {
        struct sigaction def;
        memset(&def, 0, sizeof(def));
        def.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &def, NULL);
        raise(SIGSEGV);
    }
}
#endif

void jogrt_stack_guard_init(void) {
    if (g_guard_ready) {
        return;
    }
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, jogrt_guard_fault);
#else
    struct sigaction action;
    stack_t alt;
    memset(&alt, 0, sizeof(alt));
    alt.ss_sp = g_alt_stack;
    alt.ss_size = sizeof g_alt_stack;
    alt.ss_flags = 0;
    /* Failure leaves the handler on the faulting stack. A full coroutine
       stack then cannot deliver the signal, and the process dies with
       SIGSEGV instead of the panic below. */
    if (sigaltstack(&alt, NULL) != 0) {
        alt.ss_sp = NULL;
    }
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = jogrt_guard_signal;
    action.sa_flags = SA_SIGINFO;
    if (alt.ss_sp) {
        action.sa_flags |= SA_ONSTACK;
    }
    sigaction(SIGSEGV, &action, &g_prev_segv);
#endif
    g_guard_ready = 1;
}

static void jogrt_note_region(void *base, size_t page, size_t bytes) {
    JogrtRegion region;
    if (g_region_count >= JOGRT_REGION_MAX) {
        return;
    }
    region.base = (uintptr_t)base;
    region.guard_end = region.base + page;
    region.end = region.base + bytes;
    g_regions[g_region_count++] = region;
}

bool jogrt_stack_alloc(size_t usable, void **base, void **top) {
    size_t page;
    size_t bytes;
    uintptr_t aligned;
    if (!base || !top) {
        return false;
    }
    *base = NULL;
    *top = NULL;
    page = jogrt_page_size();
    if (page == 0 || (page & (page - 1u)) != 0) {
        return false;
    }
    if (usable == 0) {
        usable = JOGRT_STACK_DEFAULT;
    }
    if (usable < page) {
        usable = page;
    }
    usable = (usable + page - 1u) & ~(page - 1u);
    bytes = page + usable;
    jogrt_stack_guard_init();

#if defined(_WIN32)
    {
        void *block = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD previous = 0;
        if (!block) {
            return false;
        }
        if (!VirtualProtect(block, page, PAGE_NOACCESS, &previous)) {
            VirtualFree(block, 0, MEM_RELEASE);
            return false;
        }
        jogrt_note_region(block, page, bytes);
        aligned = (uintptr_t)block + bytes;
        aligned &= ~(uintptr_t)15u;
        *base = block;
        *top = (void *)aligned;
        return true;
    }
#else
    {
        void *block = NULL;
        if (posix_memalign(&block, page, bytes) != 0 || !block) {
            return false;
        }
        if (mprotect(block, page, PROT_NONE) != 0) {
            free(block);
            return false;
        }
        jogrt_note_region(block, page, bytes);
        aligned = (uintptr_t)block + bytes;
        aligned &= ~(uintptr_t)15u;
        *base = block;
        *top = (void *)aligned;
        return true;
    }
#endif
}
