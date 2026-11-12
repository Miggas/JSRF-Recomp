/* m9: the game's containers: one item swapped for new bytes of any length,
 * everything after it moved, the NORM's child entries (end, size, the
 * later ones' start and end) and the MULT's entries fixed. Shared by the
 * sounds (m7) and the models (m9). */
#ifndef JCONT_H
#define JCONT_H
#include <stddef.h>
#include <stdint.h>

/* item..item+len, a child of the NORM at norm..norm_end (norm 0: the file
 * is that NORM); the new file in nb, nsize (malloc'd); 0 done */
int jc_replace(const uint8_t *buf, size_t size, size_t item, size_t len, size_t norm, size_t norm_end,
               const uint8_t *nitem, size_t nlen, uint8_t **nb, size_t *nsize);

#endif
