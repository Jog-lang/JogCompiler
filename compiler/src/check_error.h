#ifndef JOG_CHECK_ERROR_H
#define JOG_CHECK_ERROR_H

#include "check_body.h"

/* Tuples, multiple returns, destructuring, error?, null, defer, errdefer,
   and panic. Not a contract header. null adopts an expected T?. A pointer
   to a struct or enum with message(): str converts to error and to error?.
   An error value converts to error?. panic(str) diverges. errdefer is legal
   only when the function's last result is error?. A tuple field dereferences
   one pointer. Destructuring assignment writes lvalues, including nested
   tuples. error? == a sentinel compares identity. error? == error? does not. */

/* 1 when call is the builtin panic. 0 when the caller should keep checking.
   -1 when the checker has stopped. */
int jog_check_error_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *target);

JogType *jog_check_error_tuple(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target);
JogType *jog_check_error_null(Check *ck, JogNode *node, JogType *target);

/* 1 when member is a tuple field and *out is set. 0 when it has a name.
   -1 when the checker has stopped. */
int jog_check_error_field(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *member, JogType **out);

/* 1 when node is == or != of an option or null and *out is set.
   0 when neither side is an option or null. -1 when the checker has stopped. */
int jog_check_error_compare(Check *ck, JogNode *node, JogType *left, JogType *right, JogType **out);

/* 1 when want is error or error? and got is a concrete sentinel or an error
   value (*out is want). 0 when the caller should keep checking.
   -1 when the checker has stopped. */
int jog_check_error_expect(Check *ck, JogNode *node, JogType *got, JogType *want, JogType **out);

void jog_check_error_binding(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
JogType *jog_check_error_top_binding(Check *ck, CheckJob *job, JogNode *node);
void jog_check_error_destructure(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
void jog_check_error_return(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *ret);
void jog_check_error_defer(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *ret);

#endif
