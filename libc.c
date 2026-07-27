#include "stdio.h"
#include "libc.h"

#define ELK_ALIGN (sizeof(void *))
#define ELK_ALIGN_UP(n) (((n) + ELK_ALIGN - 1U) / ELK_ALIGN * ELK_ALIGN)

struct heap_block {
  size_t size;
  struct heap_block *next;
  int free;
};

static struct heap_block *s_heap;

void *memcpy(void *dst, const void *src, size_t len) {
  unsigned char *d = (unsigned char *) dst;
  const unsigned char *s = (const unsigned char *) src;
  for (size_t i = 0; i < len; i++) d[i] = s[i];
  return dst;
}

void *memmove(void *dst, const void *src, size_t len) {
  unsigned char *d = (unsigned char *) dst;
  const unsigned char *s = (const unsigned char *) src;
  if (d < s) {
    for (size_t i = 0; i < len; i++) d[i] = s[i];
  } else if (d > s) {
    while (len > 0) {
      len--;
      d[len] = s[len];
    }
  }
  return dst;
}

void *memset(void *dst, int value, size_t len) {
  unsigned char *d = (unsigned char *) dst;
  for (size_t i = 0; i < len; i++) d[i] = (unsigned char) value;
  return dst;
}

int memcmp(const void *lhs, const void *rhs, size_t len) {
  const unsigned char *a = (const unsigned char *) lhs;
  const unsigned char *b = (const unsigned char *) rhs;
  for (size_t i = 0; i < len; i++) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

size_t strlen(const char *str) {
  size_t len = 0;
  if (str != NULL) while (str[len] != '\0') len++;
  return len;
}

char *strchr(const char *str, int ch) {
  char wanted = (char) ch;
  if (str == NULL) return NULL;
  for (;;) {
    if (*str == wanted) return (char *) str;
    if (*str++ == '\0') return NULL;
  }
}

static int space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
         ch == '\f' || ch == '\v';
}

static int digit(char ch) {
  return ch >= '0' && ch <= '9';
}

static int hex_digit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static double pow10(int exponent) {
  double result = 1.0, factor = exponent < 0 ? 0.1 : 10.0;
  unsigned int count =
      (unsigned int) (exponent < 0 ? -(exponent + 1) + 1 : exponent);
  while (count > 0) {
    if (count & 1U) result *= factor;
    factor *= factor;
    count >>= 1U;
  }
  return result;
}

double strtod(const char *str, char **end) {
  const char *start = str;
  while (space(*str)) str++;
  int negative = 0;
  if (*str == '+' || *str == '-') negative = *str++ == '-';

  if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X') &&
      hex_digit(str[2]) >= 0) {
    unsigned long long hex = 0;
    str += 2;
    int digit = 0;
    while ((digit = hex_digit(*str)) >= 0) {
      hex = hex * 16ULL + (unsigned int) digit;
      str++;
    }
    if (end != NULL) *end = (char *) str;
    double value = (double) hex;
    return negative ? -value : value;
  }

  double value = 0.0;
  int digits = 0;
  while (digit(*str)) {
    value = value * 10.0 + (double) (*str++ - '0');
    digits++;
  }
  if (*str == '.') {
    double place = 0.1;
    str++;
    while (digit(*str)) {
      value += (double) (*str++ - '0') * place;
      place *= 0.1;
      digits++;
    }
  }

  const char *exponent_start = str;
  int exponent = 0, exponent_negative = 0, exponent_digits = 0;
  if (digits > 0 && (*str == 'e' || *str == 'E')) {
    str++;
    if (*str == '+' || *str == '-') exponent_negative = *str++ == '-';
    while (digit(*str)) {
      if (exponent < 10000) exponent = exponent * 10 + (*str - '0');
      str++;
      exponent_digits++;
    }
    if (exponent_digits == 0) str = exponent_start;
  }

  if (digits == 0) {
    str = start;
    value = 0.0;
  } else if (exponent_digits > 0) {
    value *= pow10(exponent_negative ? -exponent : exponent);
  }
  if (end != NULL) *end = (char *) str;
  return negative ? -value : value;
}

double modf(double value, double *integer) {
  union {
    double d;
    unsigned long long u;
  } x = {value};
  unsigned int exponent = (unsigned int) ((x.u >> 52U) & 0x7ffU);
  unsigned long long sign = x.u & 0x8000000000000000ULL;

  if (exponent == 0x7ffU) {
    *integer = value;
    return exponent && (x.u & 0xfffffffffffffULL) ? value
                                                   : (union { unsigned long long u; double d; }) {sign}.d;
  }

  int shift = (int) exponent - 1023;
  if (shift < 0) {
    *integer = (union { unsigned long long u; double d; }) {sign}.d;
    return value;
  }
  if (shift >= 52) {
    *integer = value;
    return (union { unsigned long long u; double d; }) {sign}.d;
  }

  unsigned long long mask = (1ULL << (52 - shift)) - 1ULL;
  if ((x.u & mask) == 0) {
    *integer = value;
    return (union { unsigned long long u; double d; }) {sign}.d;
  }

  x.u &= ~mask;
  *integer = x.d;
  return value - x.d;
}

struct output {
  char *dst;
  size_t size;
  size_t used;
};

static void out_char(struct output *out, char ch) {
  if (out->size > 0 && out->used + 1U < out->size) out->dst[out->used] = ch;
  out->used++;
}

static void out_data(struct output *out, const char *str, size_t len) {
  for (size_t i = 0; i < len; i++) out_char(out, str[i]);
}

static void out_unsigned(struct output *out,
                             unsigned long long value,
                             unsigned int base) {
  char reversed[sizeof(value) * 8U];
  size_t len = 0;
  do {
    unsigned int digit = (unsigned int) (value % base);
    reversed[len++] = (char) (digit < 10U ? '0' + digit : 'a' + digit - 10U);
    value /= base;
  } while (value != 0);
  while (len > 0) out_char(out, reversed[--len]);
}

static unsigned long long pow10_u(unsigned int exponent) {
  unsigned long long result = 1;
  while (exponent-- > 0) result *= 10ULL;
  return result;
}

static void out_double(struct output *out, double value,
                           unsigned int precision) {
  if (value != value) {
    out_data(out, "nan", 3);
    return;
  }
  if (value > 1.7976931348623157e308) {
    out_data(out, "inf", 3);
    return;
  }
  if (value < -1.7976931348623157e308) {
    out_data(out, "-inf", 4);
    return;
  }
  if (value < 0.0) {
    out_char(out, '-');
    value = -value;
  }
  if (value == 0.0) {
    out_char(out, '0');
    return;
  }
  double integer = 0.0;
  if (modf(value, &integer) == 0.0 &&
      integer <= 9007199254740991.0) {
    out_unsigned(out, (unsigned long long) integer, 10);
    return;
  }
  if (precision == 0) precision = 1;
  if (precision > 17) precision = 17;

  int exponent = 0;
  double normalized = value;
  while (normalized >= 10.0) normalized *= 0.1, exponent++;
  while (normalized < 1.0) normalized *= 10.0, exponent--;

  unsigned long long scale = pow10_u(precision - 1U);
  unsigned long long digits =
      (unsigned long long) (normalized * (double) scale + 0.5);
  if (digits >= scale * 10ULL) digits /= 10ULL, exponent++;

  char text[18];
  for (unsigned int i = 0; i < precision; i++) {
    text[precision - i - 1U] = (char) ('0' + digits % 10ULL);
    digits /= 10ULL;
  }
  size_t significant = precision;
  while (significant > 1 && text[significant - 1] == '0') significant--;

  if (exponent < -4 || exponent >= (int) precision) {
    out_char(out, text[0]);
    if (significant > 1) {
      out_char(out, '.');
      out_data(out, text + 1, significant - 1);
    }
    out_char(out, 'e');
    if (exponent < 0) {
      out_char(out, '-');
      exponent = -exponent;
    } else {
      out_char(out, '+');
    }
    if (exponent < 10) out_char(out, '0');
    out_unsigned(out, (unsigned long long) exponent, 10);
  } else if (exponent >= 0) {
    size_t integer_digits = (size_t) exponent + 1U;
    for (size_t i = 0; i < integer_digits; i++)
      out_char(out, i < significant ? text[i] : '0');
    if (significant > integer_digits) {
      out_char(out, '.');
      out_data(out, text + integer_digits, significant - integer_digits);
    }
  } else {
    out_data(out, "0.", 2);
    for (int i = -1; i > exponent; i--) out_char(out, '0');
    out_data(out, text, significant);
  }
}

int vsnprintf(char *dst, size_t size, const char *format, __builtin_va_list ap) {
  struct output out = {dst, size, 0};
  while (*format != '\0') {
    if (*format != '%') {
      out_char(&out, *format++);
      continue;
    }
    format++;
    if (*format == '%') {
      out_char(&out, *format++);
      continue;
    }

    int precision = -1;
    if (*format == '.') {
      format++;
      if (*format == '*') {
        precision = __builtin_va_arg(ap, int);
        format++;
      } else {
        precision = 0;
        while (digit(*format))
          precision = precision * 10 + (*format++ - '0');
      }
    }
    int is_long = *format == 'l';
    if (is_long) format++;
    char conversion = *format == '\0' ? '\0' : *format++;

    if (conversion == 's') {
      const char *str = __builtin_va_arg(ap, const char *);
      size_t len = strlen(str);
      if (precision >= 0 && (size_t) precision < len) len = (size_t) precision;
      out_data(&out, str == NULL ? "(null)" : str,
                   str == NULL ? 6U : len);
    } else if (conversion == 'd') {
      long value = is_long ? __builtin_va_arg(ap, long) : (long) __builtin_va_arg(ap, int);
      if (value < 0) {
        out_char(&out, '-');
        out_unsigned(&out, 0ULL - (unsigned long long) value, 10);
      } else {
        out_unsigned(&out, (unsigned long long) value, 10);
      }
    } else if (conversion == 'u' || conversion == 'x') {
      unsigned long value =
          is_long ? __builtin_va_arg(ap, unsigned long)
                  : (unsigned long) __builtin_va_arg(ap, unsigned int);
      out_unsigned(&out, (unsigned long long) value,
                       conversion == 'x' ? 16U : 10U);
    } else if (conversion == 'g') {
      out_double(&out, __builtin_va_arg(ap, double),
                     precision < 0 ? 6U : (unsigned int) precision);
    } else {
      out_char(&out, '%');
      if (conversion != '\0') out_char(&out, conversion);
    }
  }
  if (size > 0) dst[out.used < size ? out.used : size - 1U] = '\0';
  return out.used > 2147483647U ? 2147483647 : (int) out.used;
}

int snprintf(char *dst, size_t size, const char *format, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, format);
  int result = vsnprintf(dst, size, format, ap);
  __builtin_va_end(ap);
  return result;
}

void heap_init(void *memory, size_t size) {
  size_t header = ELK_ALIGN_UP(sizeof(struct heap_block));
  s_heap = NULL;
  if (memory == NULL || size <= header) return;
  size_t address = (size_t) memory;
  size_t aligned = ELK_ALIGN_UP(address);
  if (aligned - address >= size || size - (aligned - address) <= header) return;
  size -= aligned - address;
  s_heap = (struct heap_block *) aligned;
  s_heap->size = size - header;
  s_heap->next = NULL;
  s_heap->free = 1;
}

static void heap_merge(void) {
  struct heap_block *block = s_heap;
  size_t header = ELK_ALIGN_UP(sizeof(struct heap_block));
  while (block != NULL && block->next != NULL) {
    unsigned char *end =
        (unsigned char *) block + header + block->size;
    if (block->free && block->next->free &&
        end == (unsigned char *) block->next) {
      block->size += header + block->next->size;
      block->next = block->next->next;
    } else {
      block = block->next;
    }
  }
}

void *malloc(size_t size) {
  size_t header = ELK_ALIGN_UP(sizeof(struct heap_block));
  size = ELK_ALIGN_UP(size);
  if (size == 0) return NULL;
  for (struct heap_block *block = s_heap; block != NULL;
       block = block->next) {
    if (!block->free || block->size < size) continue;
    if (block->size >= size + header + ELK_ALIGN) {
      struct heap_block *split =
          (struct heap_block *) ((unsigned char *) block + header + size);
      split->size = block->size - size - header;
      split->next = block->next;
      split->free = 1;
      block->next = split;
      block->size = size;
    }
    block->free = 0;
    return (unsigned char *) block + header;
  }
  return NULL;
}

void free(void *ptr) {
  if (ptr == NULL) return;
  size_t header = ELK_ALIGN_UP(sizeof(struct heap_block));
  struct heap_block *block =
      (struct heap_block *) ((unsigned char *) ptr - header);
  for (struct heap_block *it = s_heap; it != NULL; it = it->next) {
    if (it == block) {
      it->free = 1;
      heap_merge();
      return;
    }
  }
}

size_t heap_available(void) {
  size_t available = 0;
  for (struct heap_block *block = s_heap; block != NULL;
       block = block->next)
    if (block->free) available += block->size;
  return available;
}

void printf(const char *fmt, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  while (*fmt) {
    if (*fmt != '%') { putchar(*fmt++); continue; }
    fmt++;
    char pad = *fmt == '0' ? *fmt++ : ' ';
    int width = 0;
    if (*fmt == '*') width = __builtin_va_arg(ap, int), fmt++;
    else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
    if (*fmt == 's') {
      char *s = __builtin_va_arg(ap, char *);
      int len = 0;
      while (s[len]) len++;
      while (len < width) putchar(pad), width--;
      while (*s) putchar(*s++);
    } else if (*fmt == 'c') {
      putchar(__builtin_va_arg(ap, int));
    } else if (*fmt == 'd' || *fmt == 'x' || *fmt == 'X') {
      unsigned x;
      char buf[16];
      int i = 0, base = *fmt == 'd' ? 10 : 16;
      const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
      if (*fmt == 'd') {
        int n = __builtin_va_arg(ap, int);
        if (n < 0) putchar('-'), n = -n, width--;
        x = (unsigned) n;
      } else {
        x = __builtin_va_arg(ap, unsigned);
      }
      do buf[i++] = digits[x % (unsigned) base], x /= (unsigned) base; while (x);
      while (i < width) putchar(pad), width--;
      while (i--) putchar(buf[i]);
    } else {
      if (*fmt != '%') putchar('%');
      putchar(*fmt);
    }
    fmt++;
  }
  __builtin_va_end(ap);
}
