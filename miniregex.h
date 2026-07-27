#include "libc.h"
int miniregex_match(const char *pattern, size_t pattern_len,
                    const char *text, size_t text_len, int anchored,
                    size_t *start, size_t *length);
