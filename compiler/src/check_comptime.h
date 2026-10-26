#ifndef JOG_CHECK_COMPTIME_H
#define JOG_CHECK_COMPTIME_H

#include "check_body.h"

/* @comptime and @embed. Not a contract header.
   @embed reads a file relative to the source and yields const bytes.
   --comptime-steps is not parsed here. */

typedef enum JogEmbedStatus {
    JOG_EMBED_OK = 0,
    JOG_EMBED_OOM,
    JOG_EMBED_COUNT,    /* not one positional argument */
    JOG_EMBED_LITERAL,  /* the argument is not a string literal */
    JOG_EMBED_RELATIVE, /* empty, absolute, or not a file path */
    JOG_EMBED_ESCAPE,   /* leaves the module, or the source file is unknown */
    JOG_EMBED_DIR,
    JOG_EMBED_MISSING
} JogEmbedStatus;

/* 0 when node is one positional string literal. COUNT or LITERAL otherwise.
   text is NUL-terminated; len is its byte length. */
int jog_embed_literal(const JogNode *node, const char **text, size_t *len);

/* Lexical path of a relative literal. Does not follow symlinks and does not
   check that the file exists. path_out uses '/' separators and lives in the
   program arena. The file must stay inside the root of the module that owns
   source_file (JogPackage.module_root), or inside program->module_root for a
   file no package owns, or inside the source file's directory when the
   program has no module root. */
JogEmbedStatus jog_embed_resolve(JogProgram *program, const char *source_file, const char *literal,
                                 size_t literal_len, const char **path_out);

/* OK when path is a regular file. DIR or MISSING otherwise. */
JogEmbedStatus jog_embed_stat(const char *path);

JogType *jog_check_comptime(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target);

#endif
