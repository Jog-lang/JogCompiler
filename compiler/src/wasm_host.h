#ifndef JOG_WASM_HOST_H
#define JOG_WASM_HOST_H

#include "diag.h"

#include <stdbool.h>

/* Run a freestanding wasm32 module with node and runtime_root/wasm/host.js.
   args, when nargs > 0, are the program arguments after the module path.
   False means the host did not start. A process that exits, including
   with a failure status, returns true and stores that status in *exit_code. */
bool jog_wasm_run(const char *wasm_path, const char *runtime_root, char **args, int nargs, const char *stdout_path,
                  int *exit_code, CtDiag *diag);

#endif
