#ifndef JOG_CHECK_ENUM_H
#define JOG_CHECK_ENUM_H

#include "check_body.h"

/* Enums, when, patterns, exhaustiveness, Option sugar, and `is`.
   Not a contract header. T? stays the structural option type from the
   error slice: null, T? == null, and niche *T? are not reimplemented here.
   T converts implicitly to T?, and Some / Option name that option.
   Nested when patterns and variant field defaults are checked here. */

/* Fill variants, symbols, the tag type, and C layout. The nominal type is
   published before payloads are resolved so *E inside E is not a cycle. */
JogType *jog_check_enum(Check *ck, CheckJob *job);

/* True when some variant's tag is 0 and that payload has a zero value. */
bool jog_enum_has_zero(const JogType *type);

/* 1 when call builds a variant. 0 when the caller should keep checking.
   -1 when the checker has stopped. */
int jog_check_enum_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *target,
                        JogType **out);

/* 1 when member is a payload-free variant value. 0 when it is not an enum
   variant. -1 when the checker has stopped. */
int jog_check_enum_member(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *member, JogType **out);

JogType *jog_check_enum_dot(Check *ck, CheckJob *job, JogNode *node, JogType *target);
JogType *jog_check_enum_when(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target,
                             bool as_value);
JogType *jog_check_enum_is(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
JogType *jog_check_enum_force(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
JogType *jog_check_enum_elvis(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
JogType *jog_check_enum_safe(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);

/* 1 when cast reads an enum's tag, or builds a payload-free enum from an
   integer tag. 0 when it is not that cast. -1 when the checker has stopped. */
int jog_check_enum_cast(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType **out);

/* 1 when member is the generated variant name. 0 when it is not, including
   when a payload field is already called name. -1 when stopped. */
int jog_check_enum_name(Check *ck, JogNode *member, JogType *outer);

#endif
