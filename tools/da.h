#ifndef CT_DA_H
#define CT_DA_H

#include <stdbool.h>
#include <stddef.h>

/* Growable array of fixed-size items. The caller owns the CtDa object.
   The item buffer is owned by realloc and free, not by an arena. It is
   allocated on the first successful push and released by ct_da_free.
   Pointers from ct_da_at die on a growing push, on ct_da_swap_remove,
   and on ct_da_free. Swap-remove may reorder the remaining items.
   count is the number of live items. capacity is the allocated slots.
   data is NULL until the first successful push. */

typedef struct CtDa {
    void *data;
    size_t count;
    size_t capacity;
    size_t item_size;
} CtDa;

/* Empty array, no buffer yet. False if da is NULL or item_size is 0.
   A failed init with a non-NULL da leaves a freeable empty array. */
bool ct_da_init(CtDa *da, size_t item_size);

/* Frees the buffer. NULL is ignored. The object can be init'd again. */
void ct_da_free(CtDa *da);

/* Copies item_size bytes from item. A zero capacity becomes 8, then
   each growth doubles. False if da or item is NULL, item_size is 0,
   or allocation fails. On failure the array is unchanged. */
bool ct_da_push(CtDa *da, const void *item);

/* Moves the last live item into index and shrinks count by one. False
   if da is NULL or index is out of range. */
bool ct_da_swap_remove(CtDa *da, size_t index);

/* Pointer to the item at index. NULL if da is NULL or index is out of
   range. The caller may write through the pointer. */
void *ct_da_at(CtDa *da, size_t index);

#endif
