#ifndef CT_PROC_H
#define CT_PROC_H

#include <stdbool.h>

/* Start a program, wait until it exits, and report its exit code.

   argv is NULL-terminated. argv[0] is the program file, absolute or
   relative to the current directory, and is not searched for on PATH.
   Arguments and paths are UTF-8. Windows quotes every argument for
   CommandLineToArgvW, converts the command line to UTF-16, and starts the
   process with CreateProcessW. Other systems use posix_spawn.

   stdout_path and stderr_path, when non-NULL, are created or truncated
   before the program is started, so a failed start can leave them empty.
   NULL inherits the caller's stream. Passing the same path string for
   both shares one file. Parent directories must already exist. Paths
   are not normalized.

   On success returns true and writes the child's status to *exit_code.
   On Windows that is the process exit code converted to int. Elsewhere
   it is the status passed to exit, or 128 plus the signal number when
   the child is killed by a signal. *exit_code is unchanged on failure.
   exit_code must be non-NULL. Invalid arguments start nothing and
   create no files. */
bool ct_proc_run(const char *const *argv, const char *stdout_path, const char *stderr_path, int *exit_code);

#endif
