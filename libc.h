// Small libc-independent support layer for Elk.
#ifndef LIBC_H
#define LIBC_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

typedef __SIZE_TYPE__      size_t;
typedef __PTRDIFF_TYPE__   ptrdiff_t;
typedef __UINTPTR_TYPE__   uintptr_t;
typedef __INTPTR_TYPE__    intptr_t;

typedef _Bool bool;
#define true  1
#define false 0

#ifndef NULL
#define NULL ((void *) 0)
#endif

void *memcpy(void *dst, const void *src, size_t len);
void *memmove(void *dst, const void *src, size_t len);
void *memset(void *dst, int value, size_t len);
int memcmp(const void *lhs, const void *rhs, size_t len);

size_t strlen(const char *str);
char *strchr(const char *str, int ch);
double strtod(const char *str, char **end);
double modf(double value, double *integer);

// Supported conversions are those used by elk.c:
//   %s, %.*s, %d, %u, %lu, %lx, %g, %.17g and %%
int vsnprintf(char *dst, size_t size, const char *format, __builtin_va_list ap);
int snprintf(char *dst, size_t size, const char *format, ...);
void printf(const char *fmt, ...);

// Fixed-arena allocator. The application owns the supplied memory.
// Calling heap_init again discards all previous allocations.
void heap_init(void *memory, size_t size);
void *malloc(size_t size);
void free(void *ptr);
size_t heap_available(void);

#endif
