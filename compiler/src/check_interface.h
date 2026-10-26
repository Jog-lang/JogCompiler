#ifndef JOG_CHECK_INTERFACE_H
#define JOG_CHECK_INTERFACE_H

#include "check_body.h"

/* Interface declarations, satisfaction, calls, properties, as / as?, and is.
   Not a contract header. A pointer to a struct or enum converts to an
   interface when that type's own methods and fields satisfy the requirements.
   Universe error is not a target. A property is a field, not a getter.
   is narrows a val or a parameter to *T; a var stays the interface. */

JogType *jog_check_interface(Check *ck, CheckJob *job);

/* True when have converts implicitly to need. Silent. */
bool jog_iface_converts(Check *ck, JogType *have, JogType *need);

/* 1 when want is an interface and this conversion was accepted or
   diagnosed (*out is want, or the error type). A pointer with message(): str,
   and an error value, also convert to error and error?. 0 when the caller
   should report an ordinary mismatch. -1 when the checker stopped. */
int jog_iface_expect(Check *ck, JogNode *node, JogType *got, JogType *want, JogType **out);

/* 1 when call is a method on an interface (handled, including errors).
   0 when base_ty is not an interface. -1 when the checker stopped.
   base_ty is already checked. */
int jog_check_interface_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *base_ty,
                             JogType **out);

/* 1 when this as? reads an interface (type is set, or an error is
   reported). 0 when it is not an interface as?, so the caller keeps the
   "not checked yet" diagnostic. -1 when the checker stopped. */
int jog_check_interface_asq(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);

/* 1 when this as is an interface conversion or a downcast to *T (type is
   set, or an error is reported). 0 when it is not. got and dest are
   already checked. */
int jog_check_interface_as(Check *ck, JogNode *node, JogType *got, JogType *dest);

/* 1 when member reads a property of an interface (type is set, or an error
   is reported). 0 when iface is not a user interface. -1 when stopped.
   iface is the receiver with one pointer peeled, already checked. */
int jog_check_interface_member(Check *ck, CheckJob *job, JogNode *member, JogType *iface, JogType **out);

/* 1 when assign stores through an interface property (handled, including
   errors). 0 when the target is not one. -1 when stopped. */
int jog_check_interface_store(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign);

/* 1 when node is v is T on an interface (type is bool, or an error is
   reported). 0 when the subject is not an interface; the subject expression
   is already checked. -1 when stopped. */
int jog_check_interface_is(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType **out);

/* 1 when pat is `is T` on an interface subject. 0 when it is not, or the
   pattern was rejected. -1 when stopped. subject is already checked. */
int jog_check_interface_pat_is(Check *ck, CheckJob *job, JogNode *pat, JogType *subject);

/* Bindings whose type is swapped to *T while a condition's true side is
   checked. enter saves the previous types. leave puts them back. */
enum { JOG_IFACE_NARROW = 8 };

typedef struct JogIfaceNarrow {
    JogSymbol *syms[JOG_IFACE_NARROW];
    JogType *saved[JOG_IFACE_NARROW];
    size_t count;
} JogIfaceNarrow;

/* True side of cond: `v is T` and the right of `&&`. */
void jog_iface_narrow_enter(Check *ck, JogNode *cond, JogIfaceNarrow *st);
/* when arm whose single pattern is `is T`. subject is the when subject. */
void jog_iface_narrow_pat(Check *ck, JogNode *subject, JogNode *pat, JogIfaceNarrow *st);
void jog_iface_narrow_leave(JogIfaceNarrow *st);

#endif
