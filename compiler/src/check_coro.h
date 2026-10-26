#ifndef JOG_CHECK_CORO_H
#define JOG_CHECK_CORO_H

#include "check_body.h"

/* go, scope, Task, Chan, and select. Not a contract header.
   return a, b inside go is one tuple result; a later return must match.
   1 when the node was handled, 0 when it is not a coroutine form,
   -1 when the checker has stopped. */

int jog_check_coro_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call);
int jog_check_coro_method(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call);
int jog_check_coro_type(Check *ck, CheckJob *job, JogSymbol *symbol, JogNode *type_name, JogType **out);
JogType *jog_check_coro_go(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *go);
void jog_check_coro_return(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *ret);

#endif
