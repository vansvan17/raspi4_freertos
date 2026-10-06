#ifndef FREERTOS_MINIMAL_STRING_H
#define FREERTOS_MINIMAL_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t length);
void *memset(void *destination, int value, size_t length);
size_t strlen(const char *text);
char *strcpy(char *destination, const char *source);

#endif
