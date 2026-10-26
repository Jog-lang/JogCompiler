#ifndef JOG_LOWER_GLOBAL_H
#define JOG_LOWER_GLOBAL_H

#include "lower_internal.h"

/* 1 when init is now global's data image, including pointer relocations.
   0 when the initializer must run at startup. -1 after a diagnostic. */
int jog_lower_global_place(Lower *L, JirGlobal *global, JogNode *init);

#endif
