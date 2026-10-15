/* Declarations only, for `build.py size`: the freestanding cross-compile has no
 * libc headers. On the target these come from the SDK's libc (newlib-nano),
 * which the firmware links anyway, so the size gate does not count them. */
#ifndef TFS_FREESTANDING_STRING_H
#define TFS_FREESTANDING_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
