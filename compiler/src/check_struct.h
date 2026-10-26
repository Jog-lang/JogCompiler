#ifndef JOG_CHECK_STRUCT_H
#define JOG_CHECK_STRUCT_H

#include "check_body.h"

/* Struct declarations, literals, fields, methods, embedding, and extensions.
   Not a contract header. Methods are jog_<pkg>__<Type>__<method> at lower
   time. A value receiver is passed by value. There is no implicit
   conversion from a struct to an embedded struct. */

enum { JOG_STRUCT_PATH = 8 };

typedef struct JogStructPath {
    uint32_t index[JOG_STRUCT_PATH];
    uint8_t len; /* 0 when the match is the type itself */
    JogField *field;
    bool found;
    bool ambiguous;
    bool hidden; /* a match exists but is not visible from here */
} JogStructPath;

/* Fill fields, symbols, and C layout. The type is published on the symbol
   before fields are resolved so *T inside T is not a declaration cycle.
   Only another struct declaration may observe that unfinished type. */
JogType *jog_check_struct(Check *ck, CheckJob *job);

/* Prepend the receiver parameter. fn_type is the signature without it. */
JogType *jog_check_method_type(Check *ck, CheckJob *job, JogType *fn_type);

/* Bind implicit self in the function scope. Value receivers are read-only. */
void jog_check_bind_self(Check *ck, CheckJob *job, JogBlockScope *scope);

/* After every signature is done. Same package, same receiver type, same name. */
void jog_check_method_duplicates(Check *ck);

/* 1 when this call is a struct literal or a method call (*out is set).
   0 when the caller should keep checking. -1 when the checker has stopped. */
int jog_check_struct_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType **out);

/* 1 when member is a field or a method. 0 when it is a package access,
   ?. , or not a struct operation. -1 when stopped. */
int jog_check_struct_member(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *member, JogType **out);

void jog_check_field_assign(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign);

/* True when every field has a zero value. Incomplete structs, including
   universe void, do not. */
bool jog_struct_has_zero(const JogType *type);

/* on is the type the method is declared for. is_ptr is 1 for fn *T.m.
   self is that type or *that type, taken from the function's first parameter. */
void jog_method_receiver(const JogNode *fn, JogType **on, int *is_ptr);

/* Visible field or promoted field. here is the declaration being checked
   or lowered (a function, for private(scope)); NULL hides private(scope). */
void jog_struct_lookup_field(JogType *type, const char *name, const JogNode *here, const JogPackage *pkg,
                             const char *file, JogStructPath *out);

/* Index path from outer to an embedded inner struct. len 0 when they are
   the same type. False when there is no path or two paths. */
bool jog_struct_embed_path(JogType *outer, JogType *inner, uint32_t *index, uint8_t *len);

#endif
