#ifndef JOG_CHECK_PLATFORM_H
#define JOG_CHECK_PLATFORM_H

#include "program.h"

#include <stdbool.h>

/* After resolve and before bodies are checked. Each expect fn needs
   exactly one actual per listed target, with the same signature as
   written. Other targets are the parsed file lists; they are not
   type-checked. */
bool jog_check_platform(JogProgram *program);

#endif
