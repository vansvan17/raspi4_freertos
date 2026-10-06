#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t length)
{
    unsigned char *to = destination;
    const unsigned char *from = source;
    while (length--) *to++ = *from++;
    return destination;
}

void *memset(void *destination, int value, size_t length)
{
    unsigned char *to = destination;
    while (length--) *to++ = (unsigned char)value;
    return destination;
}

size_t strlen(const char *text)
{
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

char *strcpy(char *destination, const char *source)
{
    char *result = destination;
    while ((*destination++ = *source++) != '\0') {}
    return result;
}
