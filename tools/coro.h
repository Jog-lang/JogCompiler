#ifndef CT_CORO_H
#define CT_CORO_H

#include <stdbool.h>
#include <stddef.h>

/* Process-global, single-threaded coroutine scheduler for the host
   compiler. The switch is a GCC/Clang x86_64 context with a 64 KiB
   stack and one guard page. Main keeps the real thread stack.
   coroutine_go only enqueues. coroutine_yield round-robins to the next
   live coroutine. The main coroutine is id 0. The caller's arg is not
   copied and must stay live until that coroutine returns. A dead slot
   keeps its stack until a later coroutine_go reuses it, or until
   coroutine_shutdown frees it from the main coroutine. The running
   coroutine does not free its own stack. This is not language
   spawn/join, and generated code does not call it. */

/* Prepares the scheduler once. Later calls succeed and do nothing.
   False if the scheduler table cannot be allocated. */
bool coroutine_init(void);

/* Enqueues fn(arg). Does not switch yet. False if init has not
   succeeded, fn is NULL, or the stack cannot be allocated. */
bool coroutine_go(void (*fn)(void *), void *arg);

/* Switches to the next live coroutine. If the caller is the only live
   coroutine, returns immediately. */
void coroutine_yield(void);

/* Id of the running coroutine. Main is 0. Returns 0 before init. */
size_t coroutine_id(void);

/* Number of live coroutines. 0 before init. Includes main after init. */
size_t coroutine_alive(void);

/* Frees every other stack and the scheduler table. Call it from the
   main coroutine. Does nothing before init. */
void coroutine_shutdown(void);

#endif
