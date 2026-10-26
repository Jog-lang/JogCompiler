#ifndef JOG_CHECK_STRING_H
#define JOG_CHECK_STRING_H

#include "check_body.h"

/* Interpolated strings and the fmt builtin. A hole is an integer, bool,
   str/bytes, or a user type with fn format(buf: bytes): (str, error?).
   An interpolated value needs an Allocator context. fmt writes the same
   template into a caller buffer and does not allocate.
   Not a contract header. */

enum { JOG_STRING_FORMAT_PATH = 8 };

/* fn is the format method. path_len is 0 when the method is on the hole
   type itself, or the embed path from that type to the receiver. */
typedef struct JogStringFormat {
    JogNode *fn;
    int is_ptr;
    uint32_t index[JOG_STRING_FORMAT_PATH];
    uint8_t path_len;
} JogStringFormat;

/* 1 when type has a usable format method and *out is set.
   0 when there is no method. 2 when a format method has the wrong shape.
   3 when two methods match. -1 when the checker has stopped.
   ck and job are NULL when lowering. pkg and file are the use site. */
int jog_string_format_method(Check *ck, CheckJob *job, JogProgram *program, JogType *type, const JogPackage *pkg,
                             const char *file, JogStringFormat *out);

/* 1 when call is the fmt builtin. 0 when the caller should check it.
   -1 when the checker has stopped. */
int jog_check_string_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call);

/* Type of an interpolated string. NULL when the checker has stopped. */
JogType *jog_check_string_interp(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);

#endif
