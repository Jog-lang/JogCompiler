#ifndef JOG_PHASES_H
#define JOG_PHASES_H

#include "ast.h"
#include "ir.h"
#include "program.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "file.h"
#include "intern.h"
#include "str.h"

#include <stdbool.h>

/* Entry points of every compiler phase, in pipeline order. Each phase
   reports problems through the program's CtDiag and returns false (or NULL)
   when it emitted errors; the driver stops before the next phase. The
   stream that owns each function is listed in docs/plans/V1Parallel.md.

   Changing a signature here is a contract change. */

/* Parser stream. Parses one lexed file into a JOG_NODE_FILE. Returns the
   node even when there were syntax errors, so later files can still be
   parsed; returns NULL only on out of memory. tokens holds JogToken items
   from jog_lex and ends with EOF. Implemented in parser.c. */
JogNode *jog_parse(CtArena *arena, CtDiag *diag, CtIntern *intern, const CtFile *file, const CtDa *tokens);

/* Parser stream. Indented S-expression form of a tree, for `jog parse` and
   golden tests. Implemented in ast_print.c. */
void jog_ast_print(const JogNode *node, CtSb *out);

/* Driver stream. Finds jog.mod upward from start (a folder or a file),
   loads the root package and everything it imports, lexes and parses every
   file, and fills program->packages in dependency order. Implemented in
   loader.c. */
bool jog_load(JogProgram *program, const char *start);

/* Driver stream. Sets std_root, core_root, and runtime_root when those
   directories exist. JOG_ROOT is exact. Without it, the search starts at
   the executable's parent and tries one directory higher. A missing
   directory leaves that root NULL. False on a bad JOG_ROOT or out of
   memory. Implemented in loader.c. */
bool jog_locate_roots(JogProgram *program);

/* Checker stream. Resolves names and checks types for the whole program,
   filling JogNode.type and JogNode.symbol. Implemented in resolve.c and
   check.c. */
bool jog_check(JogProgram *program);

/* Lowering stream. Builds the IR for a checked program. NULL on errors.
   Implemented in lower.c. */
JirModule *jog_lower(JogProgram *program);

/* C backend stream. Writes one C17 translation unit for the module. The
   output includes "jogrt.h" and defines jog_entry_init and jog_entry_main
   (see runtime/jogrt.h). Implemented in emit_c.c. */
bool jog_emit_c(const JirModule *module, JogBuildMode mode, CtSb *out, CtDiag *diag);

/* Driver stream. Compiles c_path together with the runtime into exe_path
   with clang (preferred) or gcc. Implemented in cc.c. */
typedef struct JogCcRequest {
    const char *c_path;
    const char *exe_path;
    const char *runtime_root;
    JogBuildMode mode;
    const char *const *extra_libs; /* from @link, e.g. "SDL3" */
    size_t extra_lib_count;
    bool verbose;                  /* print the command line */
} JogCcRequest;

bool jog_cc_build(const JogCcRequest *request, CtDiag *diag);

/* Interpreter stream. Runs function func_index of module at compile time
   with constant arguments. The result is written in target layout (the same
   form as JirGlobal.data) into module->arena: *out points at
   func's return type size bytes. Pointers inside the result are not
   supported until milestone 7 adds relocations. A panic or a step-budget
   overrun is reported as a compile error. Implemented in interp.c. */
bool jir_interp_call(const JirModule *module, uint32_t func_index, const JirOperand *args, size_t arg_count,
                     uint64_t step_budget, const uint8_t **out, CtDiag *diag);

#endif
