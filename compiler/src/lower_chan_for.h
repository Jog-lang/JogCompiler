#ifndef JOG_LOWER_CHAN_FOR_H
#define JOG_LOWER_CHAN_FOR_H

#include "lower_internal.h"

/* 1 when node is for v in ch and this file lowered it, including on failure.
   0 when the iterated value is not a channel. */

bool jog_lower_chan_for(Lower *L, JogNode *node);

#endif
