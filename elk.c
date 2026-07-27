// Copyright (c) 2013-2022 Cesanta Software Limited
// All rights reserved
//
// This software is dual-licensed: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License version 3 as
// published by the Free Software Foundation. For the terms of this
// license, see http://www.fsf.org/licensing/licenses/agpl-3.0.html
//
// You are free to use this software under the terms of the GNU General
// Public License, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// Alternatively, you can license this software under a commercial
// license, please contact us at https://cesanta.com/contact.html

#include "stdio.h"
#include "libc.h"
#include "elk.h"
#include "miniregex.h"

#ifndef JS_EXPR_MAX
#define JS_EXPR_MAX 20
#endif

typedef uint32_t jsoff_t;

#define GCMASK ((jsoff_t) 1U << 31)   // Entity deletion marker
#define P_CONST ((jsoff_t) 1U << 30)  // Read-only variable property
#define ENTITY_META (GCMASK | P_CONST | 3U)

static jsoff_t entity_next(jsoff_t word) {
  return word & ~ENTITY_META;
}

struct js {
  jsoff_t css;        // Max observed C stack size
  jsoff_t lwm;        // JS RAM low watermark: min free RAM observed
  const char *code;   // Currently parsed code snippet
  char errmsg[33];    // Error message placeholder
  uint8_t tok;        // Last parsed token value
  uint8_t consumed;   // Indicator that last parsed token was consumed
  uint8_t flags;      // Execution flags, see F_* constants below
#define F_NOEXEC 1U   // Parse code, but not execute
#define F_LOOP 2U     // We're inside the loop
#define F_CALL 4U     // We're inside a function call
#define F_BREAK 8U    // Exit the loop
#define F_RETURN 16U  // Return has been executed
  jsoff_t clen;       // Code snippet length
  jsoff_t pos;        // Current parsing position
  jsoff_t toff;       // Offset of the last parsed token
  jsoff_t tlen;       // Length of the last parsed token
  jsoff_t nogc;       // Entity offset to exclude from GC
  jsval_t tval;       // Holds last parsed numeric or string literal value
  jsval_t scope;      // Current scope
  uint8_t *mem;       // Available JS memory
  jsoff_t size;       // Memory size
  jsoff_t brk;        // Current mem usage boundary
  jsoff_t gct;        // GC threshold. If brk > gct, trigger GC
  jsoff_t maxcss;     // Maximum allowed C stack size usage
  void *cstk;         // C stack pointer at the beginning of js_eval()
};

// A JS memory stores diffenent entities: objects, properties, strings
// All entities are packed to the beginning of a buffer.
// The `brk` marks the end of the used memory:
//
//    | entity1 | entity2| .... |entityN|         unused memory        |
//    |---------|--------|------|-------|------------------------------|
//  js.mem                           js.brk                        js.size
//
//  Each entity is 4-byte aligned, therefore 2 LSB bits store entity type.
//  Object:   8 bytes: offset of the first property, offset of the upper obj
//  Property: 8 bytes + val: 4 byte next prop, 4 byte key offs, N byte value
//  String:   4xN bytes: 4 byte len << 2, 4byte-aligned 0-terminated data
//
// If C functions are imported, they use the upper part of memory as stack for
// passing params. Each argument is pushed to the top of the memory as jsval_t,
// and js.size is decreased by sizeof(jsval_t), i.e. 8 bytes. When function
// returns, js.size is restored back. So js.size is used as a stack pointer.

// clang-format off
enum {
  TOK_ERR, TOK_EOF, TOK_IDENTIFIER, TOK_NUMBER, TOK_STRING, TOK_SEMICOLON,
  TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
  // Keyword tokens
  TOK_BREAK = 50, TOK_CASE, TOK_CATCH, TOK_CLASS, TOK_CONST, TOK_CONTINUE,
  TOK_DEFAULT, TOK_DELETE, TOK_DO, TOK_ELSE, TOK_FINALLY, TOK_FOR, TOK_FUNC,
  TOK_IF, TOK_IN, TOK_INSTANCEOF, TOK_LET, TOK_NEW, TOK_RETURN, TOK_SWITCH,
  TOK_THIS, TOK_THROW, TOK_TRY, TOK_VAR, TOK_VOID, TOK_WHILE, TOK_WITH,
  TOK_YIELD, TOK_UNDEF, TOK_NULL, TOK_TRUE, TOK_FALSE,
  // JS Operator tokens
  TOK_DOT = 100, TOK_CALL, TOK_POSTINC, TOK_POSTDEC, TOK_NOT, TOK_TILDA,    // 100
  TOK_TYPEOF, TOK_UPLUS, TOK_UMINUS, TOK_EXP, TOK_MUL, TOK_DIV, TOK_REM,    // 106
  TOK_PLUS, TOK_MINUS, TOK_SHL, TOK_SHR, TOK_ZSHR, TOK_LT, TOK_LE, TOK_GT,  // 113
  TOK_GE, TOK_EQ, TOK_NE, TOK_AND, TOK_XOR, TOK_OR, TOK_LAND, TOK_LOR,      // 121
  TOK_COLON, TOK_Q,  TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN,
  TOK_MUL_ASSIGN, TOK_DIV_ASSIGN, TOK_REM_ASSIGN, TOK_SHL_ASSIGN,
  TOK_SHR_ASSIGN, TOK_ZSHR_ASSIGN, TOK_AND_ASSIGN, TOK_XOR_ASSIGN,
  TOK_OR_ASSIGN, TOK_COMMA, TOK_ARROW, TOK_NULLISH, TOK_TEMPLATE,
  TOK_LOOSE_EQ, TOK_LOOSE_NE, TOK_OPT_DOT,
};

enum {
  // IMPORTANT: T_OBJ, T_PROP, T_STR must go first.  That is required by the
  // memory layout functions: memory entity types are encoded in the 2 bits,
  // thus type values must be 0,1,2,3
  T_OBJ, T_PROP, T_STR, T_UNDEF, T_NULL, T_NUM, T_BOOL, T_FUNC, T_CODEREF,
  T_CFUNC, T_ERR, T_BUILTIN
};

static const char *typestr(uint8_t t) {
  const char *names[] = { "object", "prop", "string", "undefined", "null",
                          "number", "boolean", "function", "coderef",
                          "cfunc", "err", "builtin", "nan" };
  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

// Pack JS values into uin64_t, double nan, per IEEE 754
// 64bit "double": 1 bit sign, 11 bits exponent, 52 bits mantissa
//
// seeeeeee|eeeemmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm
// 11111111|11110000|00000000|00000000|00000000|00000000|00000000|00000000 inf
// 11111111|11111000|00000000|00000000|00000000|00000000|00000000|00000000 qnan
//
// 11111111|1111tttt|vvvvvvvv|vvvvvvvv|vvvvvvvv|vvvvvvvv|vvvvvvvv|vvvvvvvv
//  NaN marker |type|  48-bit placeholder for values: pointers, strings
//
// On 64-bit platforms, pointers are really 48 bit only, so they can fit,
// provided they are sign extended
static jsval_t tov(double d) { union { double d; jsval_t v; } u = {d}; return u.v; }
static double tod(jsval_t v) { union { jsval_t v; double d; } u = {v}; return u.d; }
static jsval_t mkval(uint8_t type, uint64_t data) { return ((jsval_t) 0x7ff0U << 48U) | ((jsval_t) (type) << 48) | (data & 0xffffffffffffUL); }
static bool is_nan(jsval_t v) { return (v >> 52U) == 0x7ffU; }
static uint8_t vtype(jsval_t v) { return is_nan(v) ? ((v >> 48U) & 15U) : (uint8_t) T_NUM; }
static size_t vdata(jsval_t v) { return (size_t) (v & ~((jsval_t) 0x7fffUL << 48U)); }
static jsval_t mkcoderef(jsval_t off, jsoff_t len) { return mkval(T_CODEREF, (off & 0xffffffU) | ((jsval_t)(len & 0xffffffU) << 24U)); }
static jsoff_t coderefoff(jsval_t v) { return v & 0xffffffU; }
static jsoff_t codereflen(jsval_t v) { return (v >> 24U) & 0xffffffU; }

static uint8_t unhex(uint8_t c) { return (c >= '0' && c <= '9') ? (uint8_t) (c - '0') : (c >= 'a' && c <= 'f') ? (uint8_t) (c - 'W') : (c >= 'A' && c <= 'F') ? (uint8_t) (c - '7') : 0; }
static bool is_space(int c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == '\v'; }
static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_xdigit(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_ident_begin(int c) { return c == '_' || c == '$' || is_alpha(c); }
static bool is_ident_continue(int c) { return c == '_' || c == '$' || is_alpha(c) || is_digit(c); }
static bool is_err(jsval_t v) { return vtype(v) == T_ERR; }
static bool is_unary(uint8_t tok) { return tok >= TOK_POSTINC && tok <= TOK_UMINUS; }
static bool is_assign(uint8_t tok) { return (tok >= TOK_ASSIGN && tok <= TOK_OR_ASSIGN); }
static void saveoff(struct js *js, jsoff_t off, jsoff_t val) { memcpy(&js->mem[off], &val, sizeof(val)); }
static void saveval(struct js *js, jsoff_t off, jsval_t val) { memcpy(&js->mem[off], &val, sizeof(val)); }
static jsoff_t loadoff(struct js *js, jsoff_t off) { jsoff_t v = 0; memcpy(&v, &js->mem[off], sizeof(v)); return v; }
static jsoff_t offtolen(jsoff_t off) { return (off >> 2) - 1; }
static jsoff_t vstrlen(struct js *js, jsval_t v) { return offtolen(loadoff(js, (jsoff_t) vdata(v))); }
static jsval_t loadval(struct js *js, jsoff_t off) { jsval_t v = 0; memcpy(&v, &js->mem[off], sizeof(v)); return v; }
#define SCOPE_FUNC 1U
static jsval_t upper(struct js *js, jsval_t scope) { return mkval(T_OBJ, loadoff(js, (jsoff_t) (vdata(scope) + sizeof(jsoff_t))) & ~3U); }
static jsoff_t align32(jsoff_t v) { return ((v + 3) >> 2) << 2; }

#define CHECKV(_v) do { if (is_err(_v)) { res = (_v); goto done; } } while (0)
#define EXPECT(_tok, _e) do { if (next(js) != _tok) { _e; return js_mkerr(js, "parse error"); }; js->consumed = 1; } while (0)
// clang-format on

// Forward declarations of the private functions
static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len);
static jsval_t js_expr(struct js *js);
static jsval_t js_assignment(struct js *js);
static jsval_t js_stmt(struct js *js);
static jsval_t resolveprop(struct js *js, jsval_t v);
static jsval_t do_op(struct js *, uint8_t op, jsval_t l, jsval_t r);

static jsval_t js_getchar(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  if (nargs != 0) return js_mkerr(js, "getchar expects 0 args");
  int ch = getchar();
  return js_mknum((double) (ch < 0 ? -1 : ch));
}

static jsval_t js_msleep(struct js *js, jsval_t *args, int nargs) {
  if (!js_chkargs(args, nargs, "d"))
    return js_mkerr(js, "msleep expects 1 number");
  double milliseconds = js_getnum(args[0]);
  if (milliseconds < 0 || milliseconds > 4294967295.0 ||
      milliseconds != (double) (unsigned long) milliseconds)
    return js_mkerr(js, "bad msleep duration");
  msleep((int) milliseconds);
  return js_mkundef();
}

static void setlwm(struct js *js) {
  jsoff_t n = 0, css = 0;
  if (js->brk < js->size) n = js->size - js->brk;
  if (js->lwm > n) js->lwm = n;
  if ((char *) js->cstk > (char *) &n)
    css = (jsoff_t) ((char *) js->cstk - (char *) &n);
  if (css > js->css) js->css = css;
}

// Copy src to dst, make no overflows, 0-terminate. Return bytes copied
static size_t cpy(char *dst, size_t dstlen, const char *src, size_t srclen) {
  size_t i = 0;
  for (i = 0; i < dstlen && i < srclen && src[i] != 0; i++) dst[i] = src[i];
  if (dstlen > 0) dst[i < dstlen ? i : dstlen - 1] = '\0';
  return i;
}

// Stringify JS object
static size_t strobj(struct js *js, jsval_t obj, char *buf, size_t len) {
  size_t n = cpy(buf, len, "{", 1);
  jsoff_t next = entity_next(loadoff(js, (jsoff_t) vdata(obj)));
  while (next < js->brk && next != 0) {                    // Iterate over props
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    // printf("PROP %u, koff %u\n", next & ~3, koff);
    n += cpy(buf + n, len - n, ",", n == 1 ? 0 : 1);
    n += tostr(js, mkval(T_STR, koff), buf + n, len - n);
    n += cpy(buf + n, len - n, ":", 1);
    n += tostr(js, val, buf + n, len - n);
    next = entity_next(loadoff(js, next));  // Load next prop offset
  }
  return n + cpy(buf + n, len - n, "}", 1);
}

// Stringify numeric JS value
static size_t strnum(jsval_t value, char *buf, size_t len) {
  double dv = tod(value), iv;
  const char *fmt = modf(dv, &iv) == 0.0 ? "%.17g" : "%g";
  return (size_t) snprintf(buf, len, fmt, dv);
}

// Return mem offset and length of the JS string
static jsoff_t vstr(struct js *js, jsval_t value, jsoff_t *len) {
  jsoff_t off = (jsoff_t) vdata(value);
  if (len) *len = offtolen(loadoff(js, off));
  return (jsoff_t) (off + sizeof(off));
}

// Stringify string JS value
static size_t strstring(struct js *js, jsval_t value, char *buf, size_t len) {
  jsoff_t slen, off = vstr(js, value, &slen);
  size_t n = 0;
  n += cpy(buf + n, len - n, "\"", 1);
  n += cpy(buf + n, len - n, (char *) &js->mem[off], slen);
  n += cpy(buf + n, len - n, "\"", 1);
  return n;
}

// Stringify JS function
static size_t strfunc(struct js *js, jsval_t value, char *buf, size_t len) {
  jsoff_t sn, off = vstr(js, value, &sn);
  size_t n = cpy(buf, len, "function", 8);
  return n + cpy(buf + n, len - n, (char *) &js->mem[off], sn);
}

jsval_t js_mkerr(struct js *js, const char *xx, ...) {
  __builtin_va_list ap;
  size_t n = cpy(js->errmsg, sizeof(js->errmsg), "ERROR: ", 7);
  __builtin_va_start(ap, xx);
  vsnprintf(js->errmsg + n, sizeof(js->errmsg) - n, xx, ap);
  __builtin_va_end(ap);
  js->errmsg[sizeof(js->errmsg) - 1] = '\0';
  js->pos = js->clen, js->tok = TOK_EOF, js->consumed = 0;  // Jump to the end
  return mkval(T_ERR, 0);
}

// Stringify JS value into the given buffer
static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len) {
  switch (vtype(value)) {  // clang-format off
    case T_UNDEF: return cpy(buf, len, "undefined", 9);
    case T_NULL:  return cpy(buf, len, "null", 4);
    case T_BOOL:  return cpy(buf, len, vdata(value) & 1 ? "true" : "false", vdata(value) & 1 ? 4 : 5);
    case T_OBJ:   return strobj(js, value, buf, len);
    case T_STR:   return strstring(js, value, buf, len);
    case T_NUM:   return strnum(value, buf, len);
    case T_FUNC:  return strfunc(js, value, buf, len);
    case T_CFUNC: return (size_t) snprintf(buf, len, "\"c_func_0x%lx\"", (unsigned long) vdata(value));
    case T_PROP:  return (size_t) snprintf(buf, len, "PROP@%lu", (unsigned long) vdata(value));
    default:      return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }  // clang-format on
}

// Stringify JS value into a free JS memory block
const char *js_str(struct js *js, jsval_t value) {
  // Leave jsoff_t placeholder between js->brk and a stringify buffer,
  // in case if next step is convert it into a JS variable
  char *buf = (char *) &js->mem[js->brk + sizeof(jsoff_t)];
  size_t len, available = js->size - js->brk - sizeof(jsoff_t);
  if (is_err(value)) {
    if (vdata(value) != 0) {
      jsoff_t n, off = vstr(js, mkval(T_STR, vdata(value)), &n);
      (void) n;
      return (char *) &js->mem[off];
    }
    return js->errmsg;
  }
  if (js->brk + sizeof(jsoff_t) >= js->size) return "";
  len = tostr(js, value, buf, available);
  js_mkstr(js, NULL, len);
  return buf;
}

bool js_truthy(struct js *js, jsval_t v) {
  uint8_t t = vtype(v);
  return (t == T_BOOL && vdata(v) != 0) || (t == T_NUM && tod(v) != 0.0) ||
         (t == T_OBJ || t == T_FUNC) || (t == T_STR && vstrlen(js, v) > 0);
}

static jsoff_t js_alloc(struct js *js, size_t size) {
  jsoff_t ofs = js->brk;
  size = align32((jsoff_t) size);  // 4-byte align, (n + k - 1) / k * k
  if (js->brk + size > js->size) return ~(jsoff_t) 0;
  js->brk += (jsoff_t) size;
  return ofs;
}

static jsval_t mkentity(struct js *js, jsoff_t b, const void *buf, size_t len) {
  jsoff_t ofs = js_alloc(js, len + sizeof(b));
  if (ofs == (jsoff_t) ~0) return js_mkerr(js, "oom");
  memcpy(&js->mem[ofs], &b, sizeof(b));
  // Using memmove - in case we're stringifying data from the free JS mem
  if (buf != NULL) memmove(&js->mem[ofs + sizeof(b)], buf, len);
  if ((b & 3) == T_STR) js->mem[ofs + sizeof(b) + len - 1] = 0;  // 0-terminate
  // printf("MKE: %u @ %u type %d\n", js->brk - ofs, ofs, b & 3);
  return mkval(b & 3, ofs);
}

jsval_t js_mkstr(struct js *js, const void *ptr, size_t len) {
  jsoff_t n = (jsoff_t) (len + 1);
  // printf("MKSTR %u %u\n", n, js->brk);
  return mkentity(js, (jsoff_t) ((n << 2) | T_STR), ptr, n);
}

static jsval_t js_mkthrow_value(struct js *js, jsval_t message) {
  message = resolveprop(js, message);
  if (vtype(message) != T_STR) return js_mkerr(js, "Error expects string");
  jsoff_t len, off = vstr(js, message, &len);
  jsval_t text = js_mkstr(js, NULL, (size_t) len + 7);
  if (is_err(text)) return text;
  jsoff_t outlen, out = vstr(js, text, &outlen);
  (void) outlen;
  memcpy(&js->mem[out], "ERROR: ", 7);
  memmove(&js->mem[out + 7], &js->mem[off], len);
  js->pos = js->clen, js->tok = TOK_EOF, js->consumed = 0;
  return mkval(T_ERR, vdata(text));
}

static jsval_t mkobj(struct js *js, jsoff_t parent) {
  return mkentity(js, 0 | T_OBJ, &parent, sizeof(parent));
}

static jsval_t setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v) {
  jsoff_t koff = (jsoff_t) vdata(k);          // Key offset
  jsoff_t b, head = (jsoff_t) vdata(obj);     // Property list head
  char buf[sizeof(koff) + sizeof(v)];         // Property memory layout
  memcpy(&b, &js->mem[head], sizeof(b));      // Load current 1st prop offset
  memcpy(buf, &koff, sizeof(koff));           // Initialize prop data: copy key
  memcpy(buf + sizeof(koff), &v, sizeof(v));  // Copy value
  jsoff_t brk = js->brk | T_OBJ;              // New prop offset
  // printf("PROP: %u -> %u\n", b, brk);
  jsval_t res = mkentity(js, entity_next(b) | T_PROP, buf, sizeof(buf));
  if (!is_err(res))
    memcpy(&js->mem[head], &brk, sizeof(brk));  // Repoint head to the new prop
  return res;
}

// Return T_OBJ/T_PROP/T_STR entity size based on the first word in memory
static inline jsoff_t esize(jsoff_t w) {
  switch (w & 3U) {  // clang-format off
    case T_OBJ:   return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t));
    case T_PROP:  return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t) + sizeof(jsval_t));
    case T_STR:   return (jsoff_t) (sizeof(jsoff_t) + align32(w >> 2U));
    default:      return (jsoff_t) ~0U;
  }  // clang-format on
}

void js_gc(struct js *js) {
  (void) js;  // GC disabled
}

// Skip whitespaces and comments
static jsoff_t skiptonext(const char *code, jsoff_t len, jsoff_t n) {
  // printf("SKIP: [%.*s]\n", len - n, &code[n]);
  while (n < len) {
    if (is_space(code[n])) {
      n++;
    } else if (n + 1 < len && code[n] == '/' && code[n + 1] == '/') {
      for (n += 2; n < len && code[n] != '\n';) n++;
    } else if (n + 3 < len && code[n] == '/' && code[n + 1] == '*') {
      for (n += 4; n < len && (code[n - 2] != '*' || code[n - 1] != '/');) n++;
    } else {
      break;
    }
  }
  return n;
}

static bool streq(const char *buf, size_t len, const char *p, size_t n) {
  return n == len && memcmp(buf, p, len) == 0;
}

static uint8_t parsekeyword(const char *buf, size_t len) {
  switch (buf[0]) {  // clang-format off
    case 'b': if (streq("break", 5, buf, len)) return TOK_BREAK; break;
    case 'c': if (streq("class", 5, buf, len)) return TOK_CLASS; if (streq("case", 4, buf, len)) return TOK_CASE; if (streq("catch", 5, buf, len)) return TOK_CATCH; if (streq("const", 5, buf, len)) return TOK_CONST; if (streq("continue", 8, buf, len)) return TOK_CONTINUE; break;
    case 'd': if (streq("do", 2, buf, len)) return TOK_DO;  if (streq("default", 7, buf, len)) return TOK_DEFAULT; break; // if (streq("delete", 6, buf, len)) return TOK_DELETE; break;
    case 'e': if (streq("else", 4, buf, len)) return TOK_ELSE; break;
    case 'f': if (streq("for", 3, buf, len)) return TOK_FOR; if (streq("function", 8, buf, len)) return TOK_FUNC; if (streq("finally", 7, buf, len)) return TOK_FINALLY; if (streq("false", 5, buf, len)) return TOK_FALSE; break;
    case 'i': if (streq("if", 2, buf, len)) return TOK_IF; if (streq("in", 2, buf, len)) return TOK_IN; if (streq("instanceof", 10, buf, len)) return TOK_INSTANCEOF; break;
    case 'l': if (streq("let", 3, buf, len)) return TOK_LET; break;
    case 'n': if (streq("new", 3, buf, len)) return TOK_NEW; if (streq("null", 4, buf, len)) return TOK_NULL; break;
    case 'r': if (streq("return", 6, buf, len)) return TOK_RETURN; break;
    case 's': if (streq("switch", 6, buf, len)) return TOK_SWITCH; break;
    case 't': if (streq("try", 3, buf, len)) return TOK_TRY; if (streq("this", 4, buf, len)) return TOK_THIS; if (streq("throw", 5, buf, len)) return TOK_THROW; if (streq("true", 4, buf, len)) return TOK_TRUE; if (streq("typeof", 6, buf, len)) return TOK_TYPEOF; break;
    case 'u': if (streq("undefined", 9, buf, len)) return TOK_UNDEF; break;
    case 'v': if (streq("var", 3, buf, len)) return TOK_VAR; if (streq("void", 4, buf, len)) return TOK_VOID; break;
    case 'w': if (streq("while", 5, buf, len)) return TOK_WHILE; if (streq("with", 4, buf, len)) return TOK_WITH; break;
    case 'y': if (streq("yield", 5, buf, len)) return TOK_YIELD; break;
  }  // clang-format on
  return TOK_IDENTIFIER;
}

static uint8_t parseident(const char *buf, jsoff_t len, jsoff_t *tlen) {
  if (is_ident_begin(buf[0])) {
    while (*tlen < len && is_ident_continue(buf[*tlen])) (*tlen)++;
    return parsekeyword(buf, *tlen);
  }
  return TOK_ERR;
}

static uint8_t next(struct js *js) {
  if (js->consumed == 0) return js->tok;
  js->consumed = 0;
  js->tok = TOK_ERR;
  js->toff = js->pos = skiptonext(js->code, js->clen, js->pos);
  js->tlen = 0;
  const char *buf = js->code + js->toff;
  // clang-format off
  if (js->toff >= js->clen) { js->tok = TOK_EOF; return js->tok; }
#define TOK(T, LEN) { js->tok = T; js->tlen = (LEN); break; }
#define LOOK(OFS, CH) js->toff + OFS < js->clen && buf[OFS] == CH
  switch (buf[0]) {
    case '?': if (LOOK(1, '?')) TOK(TOK_NULLISH, 2); if (LOOK(1, '.')) TOK(TOK_OPT_DOT, 2); TOK(TOK_Q, 1);
    case '`':
      js->tlen = 1;
      while (js->toff + js->tlen < js->clen && buf[js->tlen] != '`') {
        if (buf[js->tlen] == '\\' && js->toff + js->tlen + 1 < js->clen)
          js->tlen++;
        js->tlen++;
      }
      if (js->toff + js->tlen < js->clen) {
        js->tok = TOK_TEMPLATE;
        js->tlen++;
      }
      break;
    case ':': TOK(TOK_COLON, 1);
    case '(': TOK(TOK_LPAREN, 1);
    case ')': TOK(TOK_RPAREN, 1);
    case '{': TOK(TOK_LBRACE, 1);
    case '}': TOK(TOK_RBRACE, 1);
    case '[': TOK(TOK_LBRACKET, 1);
    case ']': TOK(TOK_RBRACKET, 1);
    case ';': TOK(TOK_SEMICOLON, 1);
    case ',': TOK(TOK_COMMA, 1);
    case '!': if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_NE, 3); if (LOOK(1, '=')) TOK(TOK_LOOSE_NE, 2); TOK(TOK_NOT, 1);
    case '.': TOK(TOK_DOT, 1);
    case '~': TOK(TOK_TILDA, 1);
    case '-': if (LOOK(1, '-')) TOK(TOK_POSTDEC, 2); if (LOOK(1, '=')) TOK(TOK_MINUS_ASSIGN, 2); TOK(TOK_MINUS, 1);
    case '+': if (LOOK(1, '+')) TOK(TOK_POSTINC, 2); if (LOOK(1, '=')) TOK(TOK_PLUS_ASSIGN, 2); TOK(TOK_PLUS, 1);
    case '*': if (LOOK(1, '*')) TOK(TOK_EXP, 2); if (LOOK(1, '=')) TOK(TOK_MUL_ASSIGN, 2); TOK(TOK_MUL, 1);
    case '/': if (LOOK(1, '=')) TOK(TOK_DIV_ASSIGN, 2); TOK(TOK_DIV, 1);
    case '%': if (LOOK(1, '=')) TOK(TOK_REM_ASSIGN, 2); TOK(TOK_REM, 1);
    case '&': if (LOOK(1, '&')) TOK(TOK_LAND, 2); if (LOOK(1, '=')) TOK(TOK_AND_ASSIGN, 2); TOK(TOK_AND, 1);
    case '|': if (LOOK(1, '|')) TOK(TOK_LOR, 2); if (LOOK(1, '=')) TOK(TOK_OR_ASSIGN, 2); TOK(TOK_OR, 1);
    case '=': if (LOOK(1, '>')) TOK(TOK_ARROW, 2); if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_EQ, 3); if (LOOK(1, '=')) TOK(TOK_LOOSE_EQ, 2); TOK(TOK_ASSIGN, 1);
    case '<': if (LOOK(1, '<') && LOOK(2, '=')) TOK(TOK_SHL_ASSIGN, 3); if (LOOK(1, '<')) TOK(TOK_SHL, 2); if (LOOK(1, '=')) TOK(TOK_LE, 2); TOK(TOK_LT, 1);
    case '>': if (LOOK(1, '>') && LOOK(2, '=')) TOK(TOK_SHR_ASSIGN, 3); if (LOOK(1, '>')) TOK(TOK_SHR, 2); if (LOOK(1, '=')) TOK(TOK_GE, 2); TOK(TOK_GT, 1);
    case '^': if (LOOK(1, '=')) TOK(TOK_XOR_ASSIGN, 2); TOK(TOK_XOR, 1);
    case '"': case '\'':
      js->tlen++;
      while (js->toff + js->tlen < js->clen && buf[js->tlen] != buf[0]) {
        uint8_t increment = 1;
        if (buf[js->tlen] == '\\') {
          if (js->toff + js->tlen + 2 > js->clen) break;
          increment = 2;
          if (buf[js->tlen + 1] == 'x') {
            if (js->toff + js->tlen + 4 > js->clen) break;
            increment = 4;
          }
        }
        js->tlen += increment;
      }
      if (buf[0] == buf[js->tlen]) js->tok = TOK_STRING, js->tlen++;
      break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
      char *end;
      js->tval = tov(strtod(buf, &end)); // TODO(lsm): protect against OOB access
      TOK(TOK_NUMBER, (jsoff_t) (end - buf));
    }
    default: js->tok = parseident(buf, js->clen - js->toff, &js->tlen); break;
  }  // clang-format on
  js->pos = js->toff + js->tlen;
  // printf("NEXT: %d %d [%.*s]\n", js->tok, js->pos, (int) js->tlen, buf);
  return js->tok;
}

static inline uint8_t lookahead(struct js *js) {
  uint8_t old = js->tok, consumed = js->consumed, tok = 0;
  jsoff_t pos = js->pos, toff = js->toff, tlen = js->tlen;
  jsval_t tval = js->tval;
  js->consumed = 1;
  tok = next(js);
  js->pos = pos, js->toff = toff, js->tlen = tlen;
  js->tok = old, js->tval = tval, js->consumed = consumed;
  return tok;
}

static void mkscope(struct js *js) {
  jsoff_t prev = (jsoff_t) vdata(js->scope);
  js->scope = mkobj(js, prev);
  // printf("ENTER SCOPE %u, prev %u\n", (jsoff_t) vdata(js->scope), prev);
}

static void delscope(struct js *js) {
  js->scope = upper(js, js->scope);
  // printf("EXIT  SCOPE %u\n", (jsoff_t) vdata(js->scope));
}

static jsval_t js_block(struct js *js, bool create_scope) {
  jsval_t res = js_mkundef();
  if (create_scope) mkscope(js);  // Enter new scope
  js->consumed = 1;
  // jsoff_t pos = js->pos;
  while (next(js) != TOK_EOF && next(js) != TOK_RBRACE && !is_err(res)) {
    uint8_t t = js->tok;
    res = js_stmt(js);
    if (!is_err(res) && t != TOK_LBRACE && t != TOK_IF && t != TOK_WHILE &&
        js->tok != TOK_SEMICOLON) {
      res = js_mkerr(js, "; expected");
      break;
    }
  }
  // printf("BLOCKEND %s\n", js_str(js, res));
  if (create_scope) delscope(js);  // Exit scope
  return res;
}

// Seach for property in a single object
static jsoff_t lkp(struct js *js, jsval_t obj, const char *buf, size_t len) {
  jsoff_t off = entity_next(loadoff(js, (jsoff_t) vdata(obj)));
  // printf("LKP: %lu %u [%.*s]\n", vdata(obj), off, (int) len, buf);
  while (off < js->brk && off != 0) {  // Iterate over props
    jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(off)));
    jsoff_t klen = (loadoff(js, koff) >> 2) - 1;
    const char *p = (char *) &js->mem[koff + sizeof(koff)];
    // printf("  %u %u[%.*s]\n", off, (int) klen, (int) klen, p);
    if (streq(buf, len, p, klen)) return off;  // Found !
    off = entity_next(loadoff(js, off));       // Load next prop offset
  }
  return 0;  // Not found
}

static void mark_var_scope(struct js *js) {
  jsoff_t off = (jsoff_t) (vdata(js->scope) + sizeof(jsoff_t));
  saveoff(js, off, loadoff(js, off) | SCOPE_FUNC);
}

static jsval_t find_var_scope(struct js *js) {
  jsval_t scope = js->scope;
  for (;;) {
    jsoff_t parent =
        loadoff(js, (jsoff_t) (vdata(scope) + sizeof(jsoff_t)));
    if (vdata(scope) == 0 || (parent & SCOPE_FUNC) != 0) return scope;
    scope = upper(js, scope);
  }
}

// Lookup variable in the scope chain
static jsval_t lookup(struct js *js, const char *buf, size_t len) {
  if (js->flags & F_NOEXEC) return 0;
  for (jsval_t scope = js->scope;;) {
    jsoff_t off = lkp(js, scope, buf, len);
    if (off != 0) return mkval(T_PROP, off);
    if (vdata(scope) == 0) break;
    scope = upper(js, scope);
  }
  if (streq("JSON", 4, buf, len)) return mkval(T_BUILTIN, 1);
  if (streq("getchar", 7, buf, len)) return js_mkfun(js_getchar);
  if (streq("msleep", 6, buf, len)) return js_mkfun(js_msleep);
  return js_mkerr(js, "'%.*s' not found", (int) len, buf);
}

static jsval_t resolveprop(struct js *js, jsval_t v) {
  if (vtype(v) != T_PROP) return v;
  return resolveprop(js,
                     loadval(js, (jsoff_t) (vdata(v) + sizeof(jsoff_t) * 2)));
}

static bool is_const_prop(struct js *js, jsval_t prop) {
  return vtype(prop) == T_PROP &&
         (loadoff(js, (jsoff_t) vdata(prop)) & P_CONST) != 0;
}

static void mark_const_prop(struct js *js, jsval_t prop) {
  jsoff_t off = (jsoff_t) vdata(prop);
  saveoff(js, off, loadoff(js, off) | P_CONST);
}

static jsval_t assign(struct js *js, jsval_t lhs, jsval_t val) {
  if (is_const_prop(js, lhs)) return js_mkerr(js, "assignment to const");
  saveval(js, (jsoff_t) ((vdata(lhs) & ~3U) + sizeof(jsoff_t) * 2), val);
  return lhs;
}

static jsval_t do_assign_op(struct js *js, uint8_t op, jsval_t l, jsval_t r) {
  uint8_t m[] = {TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_REM, TOK_SHL,
                 TOK_SHR,  TOK_ZSHR,  TOK_AND, TOK_XOR, TOK_OR};
  jsval_t res = do_op(js, m[op - TOK_PLUS_ASSIGN], resolveprop(js, l), r);
  if (is_err(res)) return res;
  return assign(js, l, res);
}

static jsval_t do_string_op(struct js *js, uint8_t op, jsval_t l, jsval_t r) {
  jsoff_t n1, off1 = vstr(js, l, &n1);
  jsoff_t n2, off2 = vstr(js, r, &n2);
  if (op == TOK_PLUS) {
    jsval_t res = js_mkstr(js, NULL, n1 + n2);
    // printf("STRPLUS %u %u %u %u [%.*s] [%.*s]\n", n1, off1, n2, off2, (int)
    // n1,
    //       &js->mem[off1], (int) n2, &js->mem[off2]);
    if (vtype(res) == T_STR) {
      jsoff_t n, off = vstr(js, res, &n);
      memmove(&js->mem[off], &js->mem[off1], n1);
      memmove(&js->mem[off + n1], &js->mem[off2], n2);
    }
    return res;
  } else if (op == TOK_EQ) {
    bool eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
    return mkval(T_BOOL, eq ? 1 : 0);
  } else if (op == TOK_NE) {
    bool eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
    return mkval(T_BOOL, eq ? 0 : 1);
  } else {
    return js_mkerr(js, "bad str op");
  }
}

static bool append_utf8(char *out, size_t cap, size_t *used, uint32_t cp) {
  size_t need = cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
  if (*used + need > cap || cp > 0x10ffff) return false;
  if (need == 1) {
    out[(*used)++] = (char) cp;
  } else if (need == 2) {
    out[(*used)++] = (char) (0xc0U | (cp >> 6));
    out[(*used)++] = (char) (0x80U | (cp & 0x3fU));
  } else if (need == 3) {
    out[(*used)++] = (char) (0xe0U | (cp >> 12));
    out[(*used)++] = (char) (0x80U | ((cp >> 6) & 0x3fU));
    out[(*used)++] = (char) (0x80U | (cp & 0x3fU));
  } else {
    out[(*used)++] = (char) (0xf0U | (cp >> 18));
    out[(*used)++] = (char) (0x80U | ((cp >> 12) & 0x3fU));
    out[(*used)++] = (char) (0x80U | ((cp >> 6) & 0x3fU));
    out[(*used)++] = (char) (0x80U | (cp & 0x3fU));
  }
  return true;
}

static bool read_hex4(const char *src, uint32_t *value) {
  *value = 0;
  for (size_t i = 0; i < 4; i++) {
    if (!is_xdigit((unsigned char) src[i])) return false;
    *value = (*value << 4U) | unhex((uint8_t) src[i]);
  }
  return true;
}

// Decode one quoted string token. In addition to JSON escapes, accept \xNN,
// which is useful for small language front-ends.
static jsval_t js_json_parse(struct js *js, jsval_t *args, int nargs) {
  if (nargs != 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "parse expects string");
  jsoff_t len, off = vstr(js, args[0], &len);
  const char *src = (char *) &js->mem[off];
  if (len < 2 || src[0] != '"' || src[len - 1] != '"')
    return js_mkerr(js, "bad quoted string");
  char *out = (char *) malloc(len);
  if (out == NULL) return js_mkerr(js, "oom");
  const char *error = NULL;
  size_t used = 0;
  for (size_t i = 1; i + 1 < len && error == NULL; i++) {
    unsigned char ch = (unsigned char) src[i];
    if (ch < 0x20) {
      error = "control in string";
    } else if (ch != '\\') {
      out[used++] = (char) ch;
    } else {
      if (++i + 1 >= len) {
        error = "bad string escape";
        break;
      }
      ch = (unsigned char) src[i];
      if (ch == '"' || ch == '\\' || ch == '/') {
        out[used++] = (char) ch;
      } else if (ch == 'b') {
        out[used++] = '\b';
      } else if (ch == 'f') {
        out[used++] = '\f';
      } else if (ch == 'n') {
        out[used++] = '\n';
      } else if (ch == 'r') {
        out[used++] = '\r';
      } else if (ch == 't') {
        out[used++] = '\t';
      } else if (ch == 'x') {
        if (i + 2 >= len || !is_xdigit((unsigned char) src[i + 1]) ||
            !is_xdigit((unsigned char) src[i + 2])) {
          error = "bad hex escape";
        } else {
          out[used++] =
              (char) ((unhex((uint8_t) src[i + 1]) << 4U) |
                      unhex((uint8_t) src[i + 2]));
          i += 2;
        }
      } else if (ch == 'u') {
        uint32_t cp = 0;
        if (i + 4 >= len || !read_hex4(src + i + 1, &cp)) {
          error = "bad unicode escape";
        } else {
          i += 4;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            uint32_t low = 0;
            if (i + 6 >= len || src[i + 1] != '\\' ||
                src[i + 2] != 'u' || !read_hex4(src + i + 3, &low) ||
                low < 0xdc00 || low > 0xdfff) {
              error = "bad unicode pair";
            } else {
              cp = 0x10000U + ((cp - 0xd800U) << 10U) + (low - 0xdc00U);
              i += 6;
            }
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            error = "bad unicode pair";
          }
          if (error == NULL && !append_utf8(out, len, &used, cp))
            error = "unicode too large";
        }
      } else {
        error = "bad string escape";
      }
    }
  }
  jsval_t result =
      error == NULL ? js_mkstr(js, out, used) : js_mkerr(js, "%s", error);
  free(out);
  return result;
}

static jsval_t do_dot_op(struct js *js, jsval_t l, jsval_t r) {
  const char *ptr = (char *) &js->code[coderefoff(r)];
  if (vtype(r) != T_CODEREF) return js_mkerr(js, "ident expected");
  if (vtype(l) == T_BUILTIN && vdata(l) == 1 &&
      streq(ptr, codereflen(r), "parse", 5))
    return js_mkfun(js_json_parse);
  // Handle stringvalue.length
  if (vtype(l) == T_STR && streq(ptr, codereflen(r), "length", 6)) {
    return tov(offtolen(loadoff(js, (jsoff_t) vdata(l))));
  }
  if (vtype(l) != T_OBJ) return js_mkerr(js, "lookup in non-obj");
  jsoff_t off = lkp(js, l, ptr, codereflen(r));
  return off == 0 ? js_mkundef() : mkval(T_PROP, off);
}

static jsval_t do_index_op(struct js *js, jsval_t lhs, jsval_t key) {
  if (js->flags & F_NOEXEC) return 0;
  jsval_t obj = resolveprop(js, lhs), k = resolveprop(js, key);
  char nbuf[32];
  const char *ptr = NULL;
  size_t len = 0;
  if (vtype(k) == T_NUM) {
    len = strnum(k, nbuf, sizeof(nbuf));
    ptr = nbuf;
  } else if (vtype(k) == T_STR) {
    jsoff_t slen, off = vstr(js, k, &slen);
    ptr = (char *) &js->mem[off];
    len = slen;
  } else {
    return js_mkerr(js, "bad index");
  }
  if (vtype(obj) == T_STR && vtype(k) == T_NUM) {
    long index = (long) tod(k);
    jsoff_t slen, off = vstr(js, obj, &slen);
    if (index < 0 || (unsigned long) index >= slen) return js_mkundef();
    return js_mkstr(js, &js->mem[off + (jsoff_t) index], 1);
  }
  if (vtype(obj) != T_OBJ) return js_mkerr(js, "index in non-obj");
  jsoff_t off = lkp(js, obj, ptr, len);
  return off == 0 ? js_mkundef() : mkval(T_PROP, off);
}

static bool bytes_contains(const char *haystack, size_t hlen,
                           const char *needle, size_t nlen) {
  if (nlen == 0) return true;
  for (size_t i = 0; i + nlen <= hlen; i++) {
    if (memcmp(haystack + i, needle, nlen) == 0) return true;
  }
  return false;
}

static jsval_t js_string_includes(struct js *js, jsval_t str, jsval_t arg) {
  str = resolveprop(js, str), arg = resolveprop(js, arg);
  if (vtype(str) != T_STR)
    return js_mkerr(js, "includes on %s", typestr(vtype(str)));
  if (vtype(arg) == T_UNDEF) return js_mkfalse();
  if (vtype(arg) != T_STR)
    return js_mkerr(js, "includes arg %s", typestr(vtype(arg)));
  jsoff_t hlen, hoff = vstr(js, str, &hlen);
  jsoff_t nlen, noff = vstr(js, arg, &nlen);
  return bytes_contains((char *) &js->mem[hoff], hlen,
                        (char *) &js->mem[noff], nlen)
             ? js_mktrue()
             : js_mkfalse();
}

static jsval_t js_string_starts_with(struct js *js, jsval_t str, jsval_t arg) {
  str = resolveprop(js, str), arg = resolveprop(js, arg);
  if (vtype(str) != T_STR || vtype(arg) != T_STR)
    return js_mkerr(js, "bad startsWith args");
  jsoff_t slen, soff = vstr(js, str, &slen);
  jsoff_t plen, poff = vstr(js, arg, &plen);
  return plen <= slen &&
                 memcmp(&js->mem[soff], &js->mem[poff], plen) == 0
             ? js_mktrue()
             : js_mkfalse();
}

static jsval_t array_set(struct js *js, jsval_t array, size_t index,
                         jsval_t value) {
  char key[24];
  int n = snprintf(key, sizeof(key), "%lu", (unsigned long) index);
  jsval_t k = js_mkstr(js, key, n > 0 ? (size_t) n : 0);
  if (is_err(k)) return k;
  return setprop(js, array, k, value);
}

static jsval_t array_push_string(struct js *js, jsval_t array, size_t index,
                                 const char *ptr, size_t len) {
  jsval_t value = js_mkstr(js, ptr, len);
  if (is_err(value)) return value;
  return array_set(js, array, index, value);
}

static jsval_t js_array_push(struct js *js, jsval_t array, jsval_t value) {
  array = resolveprop(js, array), value = resolveprop(js, value);
  if (vtype(array) != T_OBJ) return js_mkerr(js, "push on non-array");
  jsoff_t length_prop = lkp(js, array, "length", 6);
  if (length_prop == 0) return js_mkerr(js, "push on non-array");
  jsval_t length = resolveprop(js, mkval(T_PROP, length_prop));
  if (vtype(length) != T_NUM) return js_mkerr(js, "bad array length");
  size_t index = (size_t) tod(length);
  jsval_t added = array_set(js, array, index, value);
  if (is_err(added)) return added;
  assign(js, mkval(T_PROP, length_prop), tov((double) (index + 1)));
  return tov((double) (index + 1));
}

// Regular expression support is provided by miniregex.c.
static int regex_pattern(struct js *js, jsval_t regexp,
                         const char **ptr, jsoff_t *len) {
  regexp = resolveprop(js, regexp);
  if (vtype(regexp) == T_STR) {
    jsoff_t off = vstr(js, regexp, len);
    *ptr = (char *) &js->mem[off];
    return 1;
  }
  if (vtype(regexp) == T_OBJ) {
    jsoff_t prop = lkp(js, regexp, "_regex", 6);
    if (prop != 0) {
      jsval_t pattern = resolveprop(js, mkval(T_PROP, prop));
      if (vtype(pattern) == T_STR) {
        jsoff_t off = vstr(js, pattern, len);
        *ptr = (char *) &js->mem[off];
        return 1;
      }
    }
  }
  return 0;
}

static jsval_t js_string_match(struct js *js, jsval_t str, jsval_t regexp) {
  str = resolveprop(js, str);
  if (vtype(str) != T_STR) return js_mkerr(js, "match on non-string");
  const char *pattern;
  jsoff_t plen, slen, soff = vstr(js, str, &slen);
  if (!regex_pattern(js, regexp, &pattern, &plen))
    return js_mkerr(js, "match expects regexp");
  jsval_t array = mkobj(js, 0);
  if (is_err(array)) return array;
  size_t count = 0, pos = 0;
  while (pos <= slen) {
    size_t start = 0, len = 0;
    int matched = miniregex_match(pattern, plen,
                                  (char *) &js->mem[soff + pos], slen - pos,
                                  0, &start, &len);
    if (matched < 0) return js_mkerr(js, "bad regexp");
    if (matched == 0) break;
    jsval_t added = array_push_string(js, array, count++,
                                      (char *) &js->mem[soff + pos + start], len);
    if (is_err(added)) return added;
    pos += start + (len == 0 ? 1 : len);
  }
  jsval_t key = js_mkstr(js, "length", 6);
  if (is_err(key)) return key;
  jsval_t added = setprop(js, array, key, tov((double) count));
  return is_err(added) ? added : array;
}

static jsval_t js_string_trim(struct js *js, jsval_t str) {
  str = resolveprop(js, str);
  if (vtype(str) != T_STR) return js_mkerr(js, "trim on non-string");
  jsoff_t len, off = vstr(js, str, &len), start = 0;
  while (start < len && is_space(js->mem[off + start])) start++;
  while (len > start && is_space(js->mem[off + len - 1])) len--;
  return js_mkstr(js, &js->mem[off + start], len - start);
}

static jsval_t js_string_repeat(struct js *js, jsval_t str, jsval_t count) {
  str = resolveprop(js, str), count = resolveprop(js, count);
  if (vtype(str) != T_STR || vtype(count) != T_NUM)
    return js_mkerr(js, "bad repeat args");
  double d = tod(count);
  if (d < 0 || d > 4096 || d != (double) (size_t) d)
    return js_mkerr(js, "bad repeat count");
  size_t n = (size_t) d;
  jsoff_t len, off = vstr(js, str, &len);
  if (len > 0 && n > ((size_t) ~0) / len) return js_mkerr(js, "repeat too big");
  jsval_t result = js_mkstr(js, NULL, (size_t) len * n);
  if (is_err(result)) return result;
  jsoff_t outlen, out = vstr(js, result, &outlen);
  (void) outlen;
  for (size_t i = 0; i < n; i++)
    memmove(&js->mem[out + (jsoff_t) (i * len)], &js->mem[off], len);
  return result;
}

static jsval_t js_regex_test(struct js *js, jsval_t regexp, jsval_t str) {
  str = resolveprop(js, str);
  if (vtype(str) != T_STR) return js_mkerr(js, "bad test arg");
  const char *pattern;
  jsoff_t plen, slen, soff = vstr(js, str, &slen);
  if (!regex_pattern(js, regexp, &pattern, &plen))
    return js_mkerr(js, "test expects regexp");
  int matched = miniregex_match(pattern, plen, (char *) &js->mem[soff], slen,
                                0, NULL, NULL);
  if (matched < 0) return js_mkerr(js, "bad regexp");
  return matched ? js_mktrue() : js_mkfalse();
}

static jsval_t js_string_split(struct js *js, jsval_t str, jsval_t separator) {
  str = resolveprop(js, str);
  if (vtype(str) != T_STR) return js_mkerr(js, "split on non-string");
  const char *pattern;
  jsoff_t plen, slen, soff = vstr(js, str, &slen);
  if (!regex_pattern(js, separator, &pattern, &plen))
    return js_mkerr(js, "split expects regexp");
  jsval_t array = mkobj(js, 0);
  if (is_err(array)) return array;
  size_t count = 0, pos = 0;
  while (pos <= slen) {
    size_t start = 0, len = 0;
    int matched = miniregex_match(pattern, plen,
                                  (char *) &js->mem[soff + pos], slen - pos,
                                  0, &start, &len);
    if (matched < 0) return js_mkerr(js, "bad regexp");
    if (matched == 0) {
      jsval_t added = array_push_string(js, array, count++,
                                        (char *) &js->mem[soff + pos], slen - pos);
      if (is_err(added)) return added;
      break;
    }
    jsval_t added = array_push_string(js, array, count++,
                                      (char *) &js->mem[soff + pos], start);
    if (is_err(added)) return added;
    pos += start + (len == 0 ? 1 : len);
  }
  jsval_t key = js_mkstr(js, "length", 6);
  if (is_err(key)) return key;
  jsval_t added = setprop(js, array, key, tov((double) count));
  return is_err(added) ? added : array;
}

static jsval_t js_regex_exec(struct js *js, jsval_t regexp, jsval_t str) {
  regexp = resolveprop(js, regexp), str = resolveprop(js, str);
  if (vtype(regexp) != T_OBJ || vtype(str) != T_STR)
    return js_mkerr(js, "bad exec args");
  const char *pattern;
  jsoff_t plen, slen, soff = vstr(js, str, &slen);
  if (!regex_pattern(js, regexp, &pattern, &plen))
    return js_mkerr(js, "exec on non-regexp");
  jsoff_t index_prop = lkp(js, regexp, "lastIndex", 9);
  if (index_prop == 0) return js_mkerr(js, "bad regexp state");
  jsval_t index_value = resolveprop(js, mkval(T_PROP, index_prop));
  if (vtype(index_value) != T_NUM) return js_mkerr(js, "bad regexp state");
  size_t pos = (size_t) tod(index_value), start = 0, len = 0;
  int matched = pos <= slen
                    ? miniregex_match(pattern, plen,
                                      (char *) &js->mem[soff + pos], slen - pos,
                                      1, &start, &len)
                    : 0;
  if (matched < 0) return js_mkerr(js, "bad regexp");
  if (matched == 0) {
    assign(js, mkval(T_PROP, index_prop), tov(0));
    return js_mknull();
  }
  assign(js, mkval(T_PROP, index_prop), tov((double) (pos + len)));
  jsval_t match = mkobj(js, 0);
  if (is_err(match)) return match;
  jsval_t added = array_push_string(js, match, 0,
                                    (char *) &js->mem[soff + pos], len);
  if (is_err(added)) return added;
  jsval_t key = js_mkstr(js, "length", 6);
  if (is_err(key)) return key;
  added = setprop(js, match, key, tov(1));
  return is_err(added) ? added : match;
}

static jsval_t js_string_method(struct js *js, jsval_t receiver,
                                const char *name, size_t namelen) {
  EXPECT(TOK_LPAREN, );
  bool noarg = streq(name, namelen, "trim", 4);
  jsval_t arg = js_mkundef();
  if (!noarg) {
    arg = resolveprop(js, js_expr(js));
    if (is_err(arg)) return arg;
  }
  EXPECT(TOK_RPAREN, );
  if (js->flags & F_NOEXEC) return 0;
  if (noarg) return js_string_trim(js, receiver);
  if (streq(name, namelen, "includes", 8))
    return js_string_includes(js, receiver, arg);
  if (streq(name, namelen, "startsWith", 10))
    return js_string_starts_with(js, receiver, arg);
  if (streq(name, namelen, "match", 5))
    return js_string_match(js, receiver, arg);
  if (streq(name, namelen, "repeat", 6))
    return js_string_repeat(js, receiver, arg);
  if (streq(name, namelen, "test", 4))
    return js_regex_test(js, receiver, arg);
  if (streq(name, namelen, "split", 5))
    return js_string_split(js, receiver, arg);
  return js_mkerr(js, "unknown string method");
}

static jsval_t js_call_params(struct js *js) {
  jsoff_t pos = js->pos;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  js->consumed = 1;
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    js_expr(js);
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, js->flags = flags);
  }
  EXPECT(TOK_RPAREN, js->flags = flags);
  js->flags = flags;
  return mkcoderef(pos, js->pos - pos - js->tlen);
}

static void reverse(jsval_t *args, int nargs) {
  for (int i = 0; i < nargs / 2; i++) {
    jsval_t tmp = args[i];
    args[i] = args[nargs - i - 1], args[nargs - i - 1] = tmp;
  }
}

// Call native C function
static jsval_t call_c(struct js *js,
                      jsval_t (*fn)(struct js *, jsval_t *, int)) {
  int argc = 0;
  while (js->pos < js->clen) {
    if (next(js) == TOK_RPAREN) break;
    jsval_t arg = resolveprop(js, js_expr(js));
    if (js->brk + sizeof(arg) > js->size) return js_mkerr(js, "call oom");
    js->size -= (jsoff_t) sizeof(arg);
    memcpy(&js->mem[js->size], &arg, sizeof(arg));
    argc++;
    // printf("  arg %d -> %s\n", argc, js_str(js, arg));
    if (next(js) == TOK_COMMA) js->consumed = 1;
  }
  reverse((jsval_t *) &js->mem[js->size], argc);
  jsval_t res = fn(js, (jsval_t *) &js->mem[js->size], argc);
  setlwm(js);
  js->size += (jsoff_t) sizeof(jsval_t) * (jsoff_t) argc;  // Restore stack
  return res;
}

// Call JS function. 'fn' looks like this: "(a,b) { return a + b; }"
static jsval_t call_js(struct js *js, const char *fn, jsoff_t fnlen) {
  jsoff_t fnpos = 1;
  // printf("JSCALL [%.*s] -> %.*s\n", (int) js->clen, js->code, (int) fnlen,
  // fn);
  // printf("JSCALL, nogc %u [%.*s]\n", js->nogc, (int) fnlen, fn);
  mkscope(js);  // Create function call scope
  mark_var_scope(js);
  // Loop over arguments list "(a, b)" and set scope variables
  while (fnpos < fnlen) {
    fnpos = skiptonext(fn, fnlen, fnpos);          // Skip to the identifier
    if (fnpos < fnlen && fn[fnpos] == ')') break;  // Closing paren? break!
    jsoff_t identlen = 0;                          // Identifier length
    uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
    if (tok != TOK_IDENTIFIER) break;
    // Here we have argument name. Calculate arg value
    // printf("  [%.*s] -> %u [%.*s] -> ", (int) identlen, &fn[fnpos], js->pos,
    //       (int) js->clen, js->code);
    js->pos = skiptonext(js->code, js->clen, js->pos);
    js->consumed = 1;
    jsval_t v = js->code[js->pos] == ')' ? js_mkundef() : js_expr(js);
    // Set argument in the function scope
    setprop(js, js->scope, js_mkstr(js, &fn[fnpos], identlen), v);
    js->pos = skiptonext(js->code, js->clen, js->pos);
    if (js->pos < js->clen && js->code[js->pos] == ',') js->pos++;
    fnpos = skiptonext(fn, fnlen, fnpos + identlen);  // Skip past identifier
    if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;   // And skip comma
  }
  if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;  // Skip to the function body
  fnpos = skiptonext(fn, fnlen, fnpos);            // Up to the opening brace
  if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;  // And skip the brace
  size_t n = fnlen - fnpos - 1U;  // Function code with stripped braces
  // printf("flags: %d, body: %zu [%.*s]\n", js->flags, n, (int) n, &fn[fnpos]);
  js->flags = F_CALL;                        // Mark we're in the function call
  jsval_t res = js_eval(js, &fn[fnpos], n);  // Call function, no GC
  if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();  // No return
  delscope(js);  // Delete call scope
  // printf("  -> %d [%s], tok %d\n", js->flags, js_str(js, res), js->tok);
  return res;
}

static jsval_t call_js_direct(struct js *js, jsval_t func, jsval_t arg) {
  if (vtype(func) != T_FUNC) return js_mkerr(js, "map expects function");
  const char *code = js->code;
  jsoff_t clen = js->clen, pos = js->pos;
  jsoff_t toff = js->toff, tlen = js->tlen, nogc = js->nogc;
  uint8_t tok = js->tok, consumed = js->consumed, flags = js->flags;
  jsoff_t fnlen, fnoff = vstr(js, func, &fnlen), fnpos = 1;
  const char *fn = (char *) &js->mem[fnoff];

  mkscope(js);
  mark_var_scope(js);
  fnpos = skiptonext(fn, fnlen, fnpos);
  if (fnpos < fnlen && fn[fnpos] != ')') {
    jsoff_t identlen = 0;
    if (parseident(&fn[fnpos], fnlen - fnpos, &identlen) != TOK_IDENTIFIER) {
      delscope(js);
      return js_mkerr(js, "bad callback");
    }
    jsval_t key = js_mkstr(js, &fn[fnpos], identlen);
    if (is_err(key)) {
      delscope(js);
      return key;
    }
    jsval_t prop = setprop(js, js->scope, key, resolveprop(js, arg));
    if (is_err(prop)) {
      delscope(js);
      return prop;
    }
    fnpos += identlen;
  }
  while (fnpos < fnlen && fn[fnpos] != ')') fnpos++;
  if (fnpos < fnlen) fnpos++;
  fnpos = skiptonext(fn, fnlen, fnpos);
  if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
  size_t bodylen = fnlen - fnpos - 1U;
  js->flags = F_CALL;
  js->nogc = (jsoff_t) (fnoff - sizeof(jsoff_t));
  jsval_t result = js_eval(js, &fn[fnpos], bodylen);
  if (!is_err(result) && !(js->flags & F_RETURN)) result = js_mkundef();
  delscope(js);

  js->code = code, js->clen = clen, js->pos = pos;
  js->toff = toff, js->tlen = tlen, js->nogc = nogc;
  js->tok = tok, js->consumed = consumed, js->flags = flags;
  return result;
}

static jsval_t scope_value(struct js *js, const char *name, size_t len) {
  jsoff_t prop = lkp(js, js->scope, name, len);
  return prop == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, prop));
}

static jsval_t js_array_iterate(struct js *js, jsval_t source,
                                jsval_t callback, bool filter) {
  source = resolveprop(js, source), callback = resolveprop(js, callback);
  if (vtype(source) != T_OBJ || vtype(callback) != T_FUNC)
    return js_mkerr(js, "bad map args");
  jsoff_t length_prop = lkp(js, source, "length", 6);
  if (length_prop == 0) return js_mkerr(js, "map on non-array");
  jsval_t length = resolveprop(js, mkval(T_PROP, length_prop));
  if (vtype(length) != T_NUM) return js_mkerr(js, "bad array length");
  size_t count = (size_t) tod(length);
  jsval_t result = mkobj(js, 0);
  if (is_err(result)) return result;

  mkscope(js);
  setprop(js, js->scope, js_mkstr(js, "_map_source", 11), source);
  setprop(js, js->scope, js_mkstr(js, "_map_callback", 13), callback);
  setprop(js, js->scope, js_mkstr(js, "_map_result", 11), result);
  size_t output_count = 0;
  for (size_t i = 0; i < count; i++) {
    source = scope_value(js, "_map_source", 11);
    callback = scope_value(js, "_map_callback", 13);
    char key[24];
    int n = snprintf(key, sizeof(key), "%lu", (unsigned long) i);
    jsoff_t item_prop =
        lkp(js, source, key, n > 0 ? (size_t) n : 0);
    jsval_t item = item_prop == 0
                       ? js_mkundef()
                       : resolveprop(js, mkval(T_PROP, item_prop));
    jsval_t mapped = call_js_direct(js, callback, item);
    if (is_err(mapped)) {
      delscope(js);
      return mapped;
    }
    result = scope_value(js, "_map_result", 11);
    mapped = resolveprop(js, mapped);
    if (!filter || js_truthy(js, mapped)) {
      if (filter) {
        source = scope_value(js, "_map_source", 11);
        item_prop = lkp(js, source, key, n > 0 ? (size_t) n : 0);
        item = item_prop == 0
                   ? js_mkundef()
                   : resolveprop(js, mkval(T_PROP, item_prop));
      }
      jsval_t output = filter ? item : mapped;
      jsval_t added = array_set(js, result, output_count++, output);
      if (is_err(added)) {
        delscope(js);
        return added;
      }
    }
  }
  result = scope_value(js, "_map_result", 11);
  jsval_t length_key = js_mkstr(js, "length", 6);
  jsval_t added = is_err(length_key)
                      ? length_key
                      : setprop(js, result, length_key,
                                tov((double) output_count));
  delscope(js);
  return is_err(added) ? added : result;
}

static jsval_t js_array_map(struct js *js, jsval_t source, jsval_t callback) {
  return js_array_iterate(js, source, callback, false);
}

static jsval_t js_array_filter(struct js *js, jsval_t source,
                               jsval_t callback) {
  return js_array_iterate(js, source, callback, true);
}

static jsval_t do_call_op(struct js *js, jsval_t func, jsval_t args) {
  if (vtype(args) != T_CODEREF) return js_mkerr(js, "bad call");
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC)
    return js_mkerr(js, "calling non-function");
  const char *code = js->code;             // Save current parser state
  jsoff_t clen = js->clen, pos = js->pos;  // code, position and code length
  js->code = &js->code[coderefoff(args)];  // Point parser to args
  js->clen = codereflen(args);             // Set args length
  js->pos = skiptonext(js->code, js->clen, 0);  // Skip to 1st arg
  uint8_t tok = js->tok, flags = js->flags;     // Save flags
  jsoff_t nogc = js->nogc;
  jsval_t res = js_mkundef();
  if (vtype(func) == T_FUNC) {
    jsoff_t fnlen, fnoff = vstr(js, func, &fnlen);
    js->nogc = (jsoff_t) (fnoff - sizeof(jsoff_t));
    res = call_js(js, (const char *) (&js->mem[fnoff]), fnlen);
  } else {
    res = call_c(js, (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func));
  }
  js->code = code, js->clen = clen, js->pos = pos;  // Restore parser
  js->flags = flags, js->tok = tok, js->nogc = nogc;
  js->consumed = 1;
  return res;
}

// clang-format off
static jsval_t do_op(struct js *js, uint8_t op, jsval_t lhs, jsval_t rhs) {
  if (js->flags & F_NOEXEC) return 0;
  jsval_t l = resolveprop(js, lhs), r = resolveprop(js, rhs);
  // printf("OP %d %d %d\n", op, vtype(lhs), vtype(r));
  setlwm(js);
  if (is_err(l)) return l;
  if (is_err(r)) return r;
  if (op == TOK_LOOSE_EQ || op == TOK_LOOSE_NE) {
    bool equal = false;
    if ((vtype(l) == T_NULL && vtype(r) == T_UNDEF) ||
        (vtype(l) == T_UNDEF && vtype(r) == T_NULL)) {
      equal = true;
    } else if (vtype(l) == vtype(r)) {
      if (vtype(l) == T_STR) {
        jsval_t compared = do_string_op(js, TOK_EQ, l, r);
        if (is_err(compared)) return compared;
        equal = js_getbool(compared) != 0;
      } else if (vtype(l) == T_NUM) {
        equal = tod(l) == tod(r);
      } else {
        equal = vdata(l) == vdata(r);
      }
    }
    return mkval(T_BOOL, op == TOK_LOOSE_EQ ? equal : !equal);
  }
  if (is_assign(op) && vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs");
  switch (op) {
    case TOK_TYPEOF:  return js_mkstr(js, typestr(vtype(r)), strlen(typestr(vtype(r))));
    case TOK_CALL:    return do_call_op(js, l, r);
    case TOK_ASSIGN:  return assign(js, lhs, r);
    case TOK_POSTINC: {
      if (vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs for ++");
      jsval_t assigned = do_assign_op(js, TOK_PLUS_ASSIGN, lhs, tov(1));
      return is_err(assigned) ? assigned : l;
    }
    case TOK_POSTDEC: {
      if (vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs for --");
      jsval_t assigned = do_assign_op(js, TOK_MINUS_ASSIGN, lhs, tov(1));
      return is_err(assigned) ? assigned : l;
    }
    case TOK_NOT:     return mkval(T_BOOL, !js_truthy(js, r));
  }
  if (is_assign(op))    return do_assign_op(js, op, lhs, r);
  if (op == TOK_UPLUS && vtype(r) == T_STR) {
    jsoff_t len, off = vstr(js, r, &len);
    char *end = NULL;
    double value = strtod((char *) &js->mem[off], &end);
    if (end == (char *) &js->mem[off + len]) return tov(value);
    return js_mkerr(js, "bad number");
  }
  if (vtype(l) == T_STR && vtype(r) == T_STR) return do_string_op(js, op, l, r);
  if (is_unary(op) && vtype(r) != T_NUM) return js_mkerr(js, "type mismatch");
  if (!is_unary(op) && op != TOK_DOT && (vtype(l) != T_NUM || vtype(r) != T_NUM)) return js_mkerr(js, "type mismatch");
  double a = tod(l), b = tod(r);
  switch (op) {
    //case TOK_EXP:     return tov(pow(a, b));
    case TOK_DIV:     return tod(r) == 0 ? js_mkerr(js, "div by zero") : tov(a / b);
    case TOK_REM:     return tov(a - b * ((double) (long) (a / b)));
    case TOK_MUL:     return tov(a * b);
    case TOK_PLUS:    return tov(a + b);
    case TOK_MINUS:   return tov(a - b);
    case TOK_XOR:     return tov((double)((long) a ^ (long) b));
    case TOK_AND:     return tov((double)((long) a & (long) b));
    case TOK_OR:      return tov((double)((long) a | (long) b));
    case TOK_UMINUS:  return tov(-b);
    case TOK_UPLUS:   return r;
    case TOK_TILDA:   return tov((double)(~(long) b));
    case TOK_NOT:     return mkval(T_BOOL, b == 0);
    case TOK_SHL:     return tov((double)((long) a << (long) b));
    case TOK_SHR:     return tov((double)((long) a >> (long) b));
    case TOK_DOT:     return do_dot_op(js, l, r);
    case TOK_EQ:      return mkval(T_BOOL, (long) a == (long) b);
    case TOK_NE:      return mkval(T_BOOL, (long) a != (long) b);
    case TOK_LT:      return mkval(T_BOOL, a < b);
    case TOK_LE:      return mkval(T_BOOL, a <= b);
    case TOK_GT:      return mkval(T_BOOL, a > b);
    case TOK_GE:      return mkval(T_BOOL, a >= b);
    default:          return js_mkerr(js, "unknown op %d", (int) op);  // LCOV_EXCL_LINE
  }
}  // clang-format on

static jsval_t js_str_literal(struct js *js) {
  uint8_t *in = (uint8_t *) &js->code[js->toff];
  uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
  size_t n1 = 0, n2 = 0;
  // printf("STR %u %lu %lu\n", js->brk, js->tlen, js->clen);
  if (js->brk + sizeof(jsoff_t) + js->tlen > js->size)
    return js_mkerr(js, "oom");
  while (n2++ + 2 < js->tlen) {
    if (in[n2] == '\\') {
      if (in[n2 + 1] == in[0]) {
        out[n1++] = in[0];
      } else if (in[n2 + 1] == 'n') {
        out[n1++] = '\n';
      } else if (in[n2 + 1] == 't') {
        out[n1++] = '\t';
      } else if (in[n2 + 1] == 'r') {
        out[n1++] = '\r';
      } else if (in[n2 + 1] == 'x' && is_xdigit(in[n2 + 2]) &&
                 is_xdigit(in[n2 + 3])) {
        out[n1++] = (uint8_t) ((unhex(in[n2 + 2]) << 4U) | unhex(in[n2 + 3]));
        n2 += 2;
      } else {
        return js_mkerr(js, "bad str literal");
      }
      n2++;
    } else {
      out[n1++] = ((uint8_t *) js->code)[js->toff + n2];
    }
  }
  return js_mkstr(js, NULL, n1);
}

static jsval_t js_eval_fragment(struct js *js, const char *code, size_t len) {
  const char *saved_code = js->code;
  jsoff_t clen = js->clen, pos = js->pos, toff = js->toff, tlen = js->tlen;
  jsoff_t nogc = js->nogc, gct = js->gct;
  uint8_t tok = js->tok, consumed = js->consumed, flags = js->flags;
  void *cstk = js->cstk;
  js->gct = js->size;  // Keep the enclosing function source at a stable address
  jsval_t value = resolveprop(js, js_eval(js, code, len));
  js->code = saved_code, js->clen = clen, js->pos = pos;
  js->toff = toff, js->tlen = tlen, js->nogc = nogc, js->gct = gct;
  js->tok = tok, js->consumed = consumed, js->flags = flags, js->cstk = cstk;
  return value;
}

static bool template_reserve(char **buf, size_t *cap, size_t used,
                             size_t extra) {
  if (extra > (size_t) -1 - used) return false;
  size_t need = used + extra;
  if (need <= *cap) return true;

  size_t next = *cap == 0 ? 32 : *cap;
  while (next < need) {
    if (next > (size_t) -1 / 2) {
      next = need;
      break;
    }
    next *= 2;
  }

  char *p = (char *) malloc(next);
  if (p == NULL) return false;
  if (used > 0) memcpy(p, *buf, used);
  free(*buf);
  *buf = p;
  *cap = next;
  return true;
}

static bool template_append(char **buf, size_t *cap, size_t *used,
                            const char *src, size_t len) {
  if (!template_reserve(buf, cap, *used, len)) return false;
  memcpy(*buf + *used, src, len);
  *used += len;
  return true;
}

static jsval_t js_template_literal(struct js *js) {
  if (js->flags & F_NOEXEC) return 0;

  const char *src = js->code + js->toff + 1;
  size_t len = js->tlen - 2, out = 0, cap = len < 32 ? 32 : len;
  char *buf = (char *) malloc(cap);
  if (buf == NULL) return js_mkerr(js, "oom");

  for (size_t pos = 0; pos < len;) {
    if (src[pos] == '\\' && pos + 1 < len) {
      char ch = src[pos + 1];
      if (ch == 'n') ch = '\n';
      else if (ch == 'r') ch = '\r';
      else if (ch == 't') ch = '\t';
      if (!template_append(&buf, &cap, &out, &ch, 1)) {
        free(buf);
        return js_mkerr(js, "oom");
      }
      pos += 2;
    } else if (src[pos] == '$' && pos + 1 < len && src[pos + 1] == '{') {
      size_t begin = pos + 2, end = begin;
      int depth = 1;
      char quote = 0;
      bool escaped = false;
      for (; end < len && depth > 0; end++) {
        char ch = src[end];
        if (escaped) {
          escaped = false;
        } else if (ch == '\\') {
          escaped = true;
        } else if (quote != 0) {
          if (ch == quote) quote = 0;
        } else if (ch == '\'' || ch == '"') {
          quote = ch;
        } else if (ch == '{') {
          depth++;
        } else if (ch == '}') {
          depth--;
        }
      }
      if (depth != 0) {
        free(buf);
        return js_mkerr(js, "bad template");
      }

      jsval_t value = js_eval_fragment(js, src + begin, end - begin - 1);
      if (is_err(value)) {
        free(buf);
        return value;
      }

      if (vtype(value) == T_STR) {
        jsoff_t vlen, voff = vstr(js, value, &vlen);
        if (!template_append(&buf, &cap, &out,
                             (char *) &js->mem[voff], vlen)) {
          free(buf);
          return js_mkerr(js, "oom");
        }
      } else {
        char valuebuf[64];
        size_t n = tostr(js, value, valuebuf, sizeof(valuebuf));
        if (!template_append(&buf, &cap, &out, valuebuf, n)) {
          free(buf);
          return js_mkerr(js, "oom");
        }
      }
      pos = end;
    } else {
      if (!template_append(&buf, &cap, &out, src + pos, 1)) {
        free(buf);
        return js_mkerr(js, "oom");
      }
      pos++;
    }
  }

  jsval_t result = js_mkstr(js, buf, out);
  free(buf);
  return result;
}

static jsval_t js_regex_literal(struct js *js) {
  jsoff_t pos = js->toff + 1, start = pos;
  bool escaped = false, in_class = false;
  while (pos < js->clen) {
    char ch = js->code[pos];
    if (escaped) {
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '[') {
      in_class = true;
    } else if (ch == ']') {
      in_class = false;
    } else if (ch == '/' && !in_class) {
      break;
    }
    pos++;
  }
  if (pos >= js->clen) return js_mkerr(js, "bad regexp");
  jsval_t result = js_mkstr(js, js->code + start, pos - start);
  if (is_err(result)) return result;
  pos++;
  bool sticky = false;
  while (pos < js->clen && is_alpha((unsigned char) js->code[pos])) {
    if (js->code[pos] == 'y') sticky = true;
    pos++;
  }
  js->pos = pos;
  js->consumed = 1;
  if (!sticky || (js->flags & F_NOEXEC)) return result;
  jsval_t regexp = mkobj(js, 0);
  if (is_err(regexp)) return regexp;
  jsval_t pattern_key = js_mkstr(js, "_regex", 6);
  if (is_err(pattern_key)) return pattern_key;
  jsval_t prop = setprop(js, regexp, pattern_key, result);
  if (is_err(prop)) return prop;
  jsval_t index_key = js_mkstr(js, "lastIndex", 9);
  if (is_err(index_key)) return index_key;
  prop = setprop(js, regexp, index_key, tov(0));
  return is_err(prop) ? prop : regexp;
}

static jsval_t js_array_literal(struct js *js) {
  bool exe = !(js->flags & F_NOEXEC);
  jsval_t array = exe ? mkobj(js, 0) : js_mkundef();
  if (is_err(array)) return array;
  js->consumed = 1;
  if (next(js) != TOK_RBRACKET)
    return js_mkerr(js, "only empty array literal supported");
  js->consumed = 1;
  if (exe) {
    jsval_t key = js_mkstr(js, "length", 6);
    if (is_err(key)) return key;
    jsval_t prop = setprop(js, array, key, tov(0));
    if (is_err(prop)) return prop;
  }
  return array;
}

static jsval_t js_obj_literal(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  // printf("OLIT1\n");
  jsval_t obj = exe ? mkobj(js, 0) : js_mkundef();
  if (is_err(obj)) return obj;
  js->consumed = 1;
  while (next(js) != TOK_RBRACE) {
    jsval_t key = 0;
    if (js->tok == TOK_IDENTIFIER) {
      if (exe) key = js_mkstr(js, js->code + js->toff, js->tlen);
    } else if (js->tok == TOK_STRING) {
      if (exe) key = js_str_literal(js);
    } else {
      return js_mkerr(js, "parse error");
    }
    js->consumed = 1;
    EXPECT(TOK_COLON, );
    jsval_t val = js_expr(js);
    if (exe) {
      // printf("XXXX [%s] scope: %lu\n", js_str(js, val), vdata(js->scope));
      if (is_err(val)) return val;
      if (is_err(key)) return key;
      jsval_t res = setprop(js, obj, key, resolveprop(js, val));
      if (is_err(res)) return res;
    }
    if (next(js) == TOK_RBRACE) break;
    EXPECT(TOK_COMMA, );
  }
  EXPECT(TOK_RBRACE, );
  return obj;
}

static jsval_t js_func_literal(struct js *js) {
  uint8_t flags = js->flags;  // Save current flags
  js->consumed = 1;
  EXPECT(TOK_LPAREN, js->flags = flags);
  jsoff_t pos = js->pos - 1;
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    EXPECT(TOK_IDENTIFIER, js->flags = flags);
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, js->flags = flags);
  }
  EXPECT(TOK_RPAREN, js->flags = flags);
  EXPECT(TOK_LBRACE, js->flags = flags);
  js->consumed = 0;
  js->flags |= F_NOEXEC;              // Set no-execution flag to parse the
  jsval_t res = js_block(js, false);  // Skip function body - no exec
  if (is_err(res)) {                  // But fail short on parse error
    js->flags = flags;
    return res;
  }
  js->flags = flags;  // Restore flags
  jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
  js->consumed = 1;
  // printf("FUNC: %u [%.*s]\n", pos, js->pos - pos, &js->code[pos]);
  return mkval(T_FUNC, (unsigned long) vdata(str));
}

// Parse an arrow body and store it in the same "(args){body}" representation
// used by ordinary functions. The arrow token itself is the current token.
static jsval_t js_arrow_literal(struct js *js, const char *param,
                                jsoff_t paramlen) {
  uint8_t flags = js->flags;
  js->consumed = 1;  // Consume =>
  jsoff_t body = skiptonext(js->code, js->clen, js->pos), end = body;
  bool block = body < js->clen && js->code[body] == '{';
  js->flags |= F_NOEXEC;
  jsval_t parsed;
  if (block) {
    js->pos = body;
    js->consumed = 1;
    if (next(js) != TOK_LBRACE) return js_mkerr(js, "{ expected");
    parsed = js_block(js, false);
    end = js->pos;
    js->consumed = 1;  // The closing brace belongs to the arrow literal
  } else {
    js->pos = body;
    js->consumed = 1;
    parsed = js_assignment(js);
    end = js->toff;
    if (js->tok == TOK_EOF) end = js->clen;
  }
  js->flags = flags;
  if (is_err(parsed)) return parsed;

  size_t prefix = (size_t) paramlen + (block ? 2U : 10U);
  size_t suffix = block ? 0U : 2U;
  size_t bodylen = end - body;
  jsval_t str = js_mkstr(js, NULL, prefix + bodylen + suffix);
  if (is_err(str)) return str;
  jsoff_t n, off = vstr(js, str, &n);
  char *dst = (char *) &js->mem[off];
  size_t p = 0;
  dst[p++] = '(';
  if (paramlen > 0) memcpy(dst + p, param, paramlen), p += paramlen;
  dst[p++] = ')';
  if (!block) memcpy(dst + p, "{return ", 8), p += 8;
  memcpy(dst + p, js->code + body, bodylen), p += bodylen;
  if (!block) memcpy(dst + p, ";}", 2);
  return mkval(T_FUNC, (unsigned long) vdata(str));
}

#define RTL_BINOP(_f1, _f2, _cond)  \
  jsval_t res = _f1(js);            \
  while (!is_err(res) && (_cond)) { \
    uint8_t op = js->tok;           \
    js->consumed = 1;               \
    jsval_t rhs = _f2(js);          \
    if (is_err(rhs)) return rhs;    \
    res = do_op(js, op, res, rhs);  \
  }                                 \
  return res;

#define LTR_BINOP(_f, _cond)        \
  jsval_t res = _f(js);             \
  while (!is_err(res) && (_cond)) { \
    uint8_t op = js->tok;           \
    js->consumed = 1;               \
    jsval_t rhs = _f(js);           \
    if (is_err(rhs)) return rhs;    \
    res = do_op(js, op, res, rhs);  \
  }                                 \
  return res;

static jsval_t js_literal(struct js *js) {
  next(js);
  setlwm(js);
  // printf("css : %u\n", js->css);
  if (js->maxcss > 0 && js->css > js->maxcss) return js_mkerr(js, "C stack");
  js->consumed = 1;
  switch (js->tok) {  // clang-format off
    case TOK_ERR:         return js_mkerr(js, "parse error");
    case TOK_NUMBER:      return js->tval;
    case TOK_STRING:      return js_str_literal(js);
    case TOK_TEMPLATE:    return js_template_literal(js);
    case TOK_LBRACKET:    return js_array_literal(js);
    case TOK_DIV:         return js_regex_literal(js);
    case TOK_LBRACE:      return js_obj_literal(js);
    case TOK_FUNC:        return js_func_literal(js);
    case TOK_NULL:        return js_mknull();
    case TOK_UNDEF:       return js_mkundef();
    case TOK_TRUE:        return js_mktrue();
    case TOK_FALSE:       return js_mkfalse();
    case TOK_IDENTIFIER:  return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    default:              return js_mkerr(js, "bad expr");
  }  // clang-format on
}

static jsval_t js_group(struct js *js) {
  if (next(js) == TOK_LPAREN) {
    js->consumed = 1;
    if (next(js) == TOK_RPAREN) {
      js->consumed = 1;
      if (next(js) != TOK_ARROW) return js_mkerr(js, "bad expr");
      return js_arrow_literal(js, NULL, 0);
    }
    jsval_t v = js_expr(js);
    if (is_err(v)) return v;
    while (next(js) == TOK_COMMA) {
      js->consumed = 1;
      v = js_expr(js);
      if (is_err(v)) return v;
    }
    if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected");
    js->consumed = 1;
    return v;
  } else {
    return js_literal(js);
  }
}

static jsval_t js_call_dot(struct js *js) {
  jsval_t res = js_group(js);
  if (is_err(res)) return res;
  if (vtype(res) == T_CODEREF) {
    res = lookup(js, &js->code[coderefoff(res)], codereflen(res));
  }
  while (next(js) == TOK_LPAREN || next(js) == TOK_DOT ||
         next(js) == TOK_OPT_DOT || next(js) == TOK_LBRACKET) {
    if (js->tok == TOK_DOT || js->tok == TOK_OPT_DOT) {
      bool optional = js->tok == TOK_OPT_DOT;
      js->consumed = 1;
      jsval_t member = js_group(js);
      if (is_err(member)) return member;
      jsval_t receiver = resolveprop(js, res);
      if (optional &&
          (vtype(receiver) == T_UNDEF || vtype(receiver) == T_NULL)) {
        if (next(js) == TOK_LPAREN) {
          jsval_t skipped = js_call_params(js);
          if (is_err(skipped)) return skipped;
        }
        res = js_mkundef();
        continue;
      }
      if (vtype(resolveprop(js, res)) == T_STR &&
          vtype(member) == T_CODEREF && next(js) == TOK_LPAREN) {
        const char *name = js->code + coderefoff(member);
        size_t namelen = codereflen(member);
        if (streq(name, namelen, "includes", 8) ||
            streq(name, namelen, "startsWith", 10) ||
            streq(name, namelen, "match", 5) ||
            streq(name, namelen, "repeat", 6) ||
            streq(name, namelen, "trim", 4) ||
            streq(name, namelen, "test", 4) ||
            streq(name, namelen, "split", 5)) {
          res = js_string_method(js, res, name, namelen);
          if (is_err(res)) return res;
          continue;
        }
      }
      if (vtype(resolveprop(js, res)) == T_OBJ &&
          vtype(member) == T_CODEREF && next(js) == TOK_LPAREN) {
        const char *name = js->code + coderefoff(member);
        size_t namelen = codereflen(member);
        if (streq(name, namelen, "push", 4) ||
            streq(name, namelen, "exec", 4) ||
            streq(name, namelen, "map", 3) ||
            streq(name, namelen, "filter", 6)) {
          EXPECT(TOK_LPAREN, );
          jsval_t arg = js_expr(js);
          if (is_err(arg)) return arg;
          EXPECT(TOK_RPAREN, );
          if (js->flags & F_NOEXEC) {
            res = 0;
          } else if (streq(name, namelen, "push", 4)) {
            res = js_array_push(js, res, arg);
          } else if (streq(name, namelen, "map", 3)) {
            res = js_array_map(js, res, arg);
          } else if (streq(name, namelen, "filter", 6)) {
            res = js_array_filter(js, res, arg);
          } else {
            res = js_regex_exec(js, res, arg);
          }
          if (is_err(res)) return res;
          continue;
        }
      }
      res = do_op(js, TOK_DOT, res, member);
    } else if (js->tok == TOK_LBRACKET) {
      js->consumed = 1;
      jsval_t key = js_expr(js);
      if (is_err(key)) return key;
      EXPECT(TOK_RBRACKET, );
      res = do_index_op(js, res, key);
    } else {
      jsval_t params = js_call_params(js);
      if (is_err(params)) return params;
      res = do_op(js, TOK_CALL, res, params);
    }
  }
  return res;
}

static jsval_t js_postfix(struct js *js) {
  jsval_t res = js_call_dot(js);
  if (is_err(res)) return res;
  next(js);
  if (js->tok == TOK_POSTINC || js->tok == TOK_POSTDEC) {
    js->consumed = 1;
    res = do_op(js, js->tok, res, 0);
  }
  return res;
}

static jsval_t js_unary(struct js *js) {
  if (next(js) == TOK_NOT || js->tok == TOK_TILDA || js->tok == TOK_TYPEOF ||
      js->tok == TOK_MINUS || js->tok == TOK_PLUS) {
    uint8_t t = js->tok;
    if (t == TOK_MINUS) t = TOK_UMINUS;
    if (t == TOK_PLUS) t = TOK_UPLUS;
    js->consumed = 1;
    return do_op(js, t, js_mkundef(), js_unary(js));
  } else {
    return js_postfix(js);
  }
}

static jsval_t js_mul_div_rem(struct js *js) {
  LTR_BINOP(js_unary,
            (next(js) == TOK_MUL || js->tok == TOK_DIV || js->tok == TOK_REM));
}

static jsval_t js_plus_minus(struct js *js) {
  LTR_BINOP(js_mul_div_rem, (next(js) == TOK_PLUS || js->tok == TOK_MINUS));
}

static jsval_t js_shifts(struct js *js) {
  LTR_BINOP(js_plus_minus, (next(js) == TOK_SHR || next(js) == TOK_SHL ||
                            next(js) == TOK_ZSHR));
}

static jsval_t js_comparison(struct js *js) {
  LTR_BINOP(js_shifts, (next(js) == TOK_LT || next(js) == TOK_LE ||
                        next(js) == TOK_GT || next(js) == TOK_GE));
}

static jsval_t js_equality(struct js *js) {
  LTR_BINOP(js_comparison,
            (next(js) == TOK_EQ || next(js) == TOK_NE ||
             next(js) == TOK_LOOSE_EQ || next(js) == TOK_LOOSE_NE));
}

static jsval_t js_bitwise_and(struct js *js) {
  LTR_BINOP(js_equality, (next(js) == TOK_AND));
}

static jsval_t js_bitwise_xor(struct js *js) {
  LTR_BINOP(js_bitwise_and, (next(js) == TOK_XOR));
}

static jsval_t js_bitwise_or(struct js *js) {
  LTR_BINOP(js_bitwise_xor, (next(js) == TOK_OR));
}

static jsval_t js_logical_and(struct js *js) {
  jsval_t res = js_bitwise_or(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_LAND) {
    js->consumed = 1;
    res = resolveprop(js, res);
    if (!js_truthy(js, res)) js->flags |= F_NOEXEC;  // false && ... shortcut
    if (js->flags & F_NOEXEC) {
      js_logical_and(js);
    } else {
      res = js_logical_and(js);
    }
  }
  js->flags = flags;
  return res;
}

static jsval_t js_logical_or(struct js *js) {
  jsval_t res = js_logical_and(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_LOR) {
    js->consumed = 1;
    res = resolveprop(js, res);
    if (js_truthy(js, res)) js->flags |= F_NOEXEC;  // true || ... shortcut
    if (js->flags & F_NOEXEC) {
      js_logical_or(js);
    } else {
      res = js_logical_or(js);
    }
  }
  js->flags = flags;
  return res;
}

static jsval_t js_nullish(struct js *js) {
  jsval_t res = js_logical_or(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_NULLISH) {
    js->consumed = 1;
    jsval_t value = resolveprop(js, res);
    bool use_rhs = vtype(value) == T_NULL || vtype(value) == T_UNDEF;
    if (!use_rhs) js->flags |= F_NOEXEC;
    jsval_t rhs = js_logical_or(js);
    if (is_err(rhs)) {
      js->flags = flags;
      return rhs;
    }
    if (use_rhs) res = rhs;
    js->flags = flags;
  }
  return res;
}

static jsval_t js_ternary(struct js *js) {
  jsval_t res = js_nullish(js);
  if (next(js) == TOK_Q) {
    uint8_t flags = js->flags;
    js->consumed = 1;
    if (js_truthy(js, resolveprop(js, res))) {
      res = js_ternary(js);
      js->flags |= F_NOEXEC;
      EXPECT(TOK_COLON, js->flags = flags);
      js_ternary(js);
      js->flags = flags;
    } else {
      js->flags |= F_NOEXEC;
      js_ternary(js);
      EXPECT(TOK_COLON, js->flags = flags);
      js->flags = flags;
      res = js_ternary(js);
    }
  }
  return res;
}

static jsval_t js_assignment(struct js *js) {
  if (next(js) == TOK_IDENTIFIER && lookahead(js) == TOK_ARROW) {
    const char *param = js->code + js->toff;
    jsoff_t paramlen = js->tlen;
    js->consumed = 1;
    if (next(js) != TOK_ARROW) return js_mkerr(js, "=> expected");
    return js_arrow_literal(js, param, paramlen);
  }
  RTL_BINOP(js_ternary, js_assignment,
            (next(js) == TOK_ASSIGN || js->tok == TOK_PLUS_ASSIGN ||
             js->tok == TOK_MINUS_ASSIGN || js->tok == TOK_MUL_ASSIGN ||
             js->tok == TOK_DIV_ASSIGN || js->tok == TOK_REM_ASSIGN ||
             js->tok == TOK_SHL_ASSIGN || js->tok == TOK_SHR_ASSIGN ||
             js->tok == TOK_ZSHR_ASSIGN || js->tok == TOK_AND_ASSIGN ||
             js->tok == TOK_XOR_ASSIGN || js->tok == TOK_OR_ASSIGN));
}

static jsval_t js_expr(struct js *js) {
  return js_assignment(js);
}

static jsval_t js_let(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  for (;;) {
    EXPECT(TOK_IDENTIFIER, );
    js->consumed = 0;
    jsoff_t noff = js->toff, nlen = js->tlen;
    char *name = (char *) &js->code[noff];
    jsval_t v = js_mkundef();
    js->consumed = 1;
    if (next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      v = js_expr(js);
      if (is_err(v)) return v;  // Propagate error if any
    }
    if (exe) {
      if (lkp(js, js->scope, name, nlen) > 0)
        return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
      jsval_t x =
          setprop(js, js->scope, js_mkstr(js, name, nlen), resolveprop(js, v));
      if (is_err(x)) return x;
    }
    if (next(js) == TOK_SEMICOLON || next(js) == TOK_EOF) break;  // Stop
    EXPECT(TOK_COMMA, );
  }
  return js_mkundef();
}

// Limited const semantics: block scoped and immutable after an obligatory
// initializer. Temporal Dead Zone behaviour is intentionally not implemented.
static jsval_t js_const(struct js *js) {
  bool exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  for (;;) {
    EXPECT(TOK_IDENTIFIER, );
    jsoff_t noff = js->toff, nlen = js->tlen;
    const char *name = &js->code[noff];
    js->consumed = 1;
    if (next(js) != TOK_ASSIGN)
      return js_mkerr(js, "const needs initializer");
    js->consumed = 1;
    jsval_t value = js_expr(js);
    if (is_err(value)) return value;
    if (exe) {
      if (lkp(js, js->scope, name, nlen) > 0)
        return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
      jsval_t key = js_mkstr(js, name, nlen);
      if (is_err(key)) return key;
      jsval_t prop =
          setprop(js, js->scope, key, resolveprop(js, value));
      if (is_err(prop)) return prop;
      mark_const_prop(js, prop);
    }
    if (next(js) == TOK_SEMICOLON || next(js) == TOK_EOF) break;
    EXPECT(TOK_COMMA, );
  }
  return js_mkundef();
}

// Limited var semantics: function/global scope, redeclaration allowed, no
// declaration hoisting. Initialisers still execute at the declaration point.
static jsval_t js_var(struct js *js) {
  bool exe = !(js->flags & F_NOEXEC);
  jsval_t target = exe ? find_var_scope(js) : js_mkundef();
  js->consumed = 1;
  for (;;) {
    EXPECT(TOK_IDENTIFIER, );
    jsoff_t noff = js->toff, nlen = js->tlen;
    const char *name = &js->code[noff];
    jsval_t value = js_mkundef();
    bool has_initializer = false;
    js->consumed = 1;
    if (next(js) == TOK_ASSIGN) {
      has_initializer = true;
      js->consumed = 1;
      value = js_expr(js);
      if (is_err(value)) return value;
    }
    if (exe) {
      jsoff_t existing = lkp(js, target, name, nlen);
      if (existing != 0) {
        if (has_initializer) {
          jsval_t assigned =
              assign(js, mkval(T_PROP, existing), resolveprop(js, value));
          if (is_err(assigned)) return assigned;
        }
      } else {
        jsval_t key = js_mkstr(js, name, nlen);
        if (is_err(key)) return key;
        jsval_t prop = setprop(js, target, key, resolveprop(js, value));
        if (is_err(prop)) return prop;
      }
    }
    if (next(js) == TOK_SEMICOLON || next(js) == TOK_EOF) break;
    EXPECT(TOK_COMMA, );
  }
  return js_mkundef();
}

static jsval_t js_block_or_stmt(struct js *js) {
  if (next(js) == TOK_LBRACE) return js_block(js, !(js->flags & F_NOEXEC));
  jsval_t res = resolveprop(js, js_stmt(js));
  js->consumed = 0;  //
  return res;
}

static jsval_t js_if(struct js *js) {
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsval_t res = js_mkundef(), cond = resolveprop(js, js_expr(js));
  EXPECT(TOK_RPAREN, );
  bool cond_true = js_truthy(js, cond), exe = !(js->flags & F_NOEXEC);
  // printf("IF COND: %s, true? %d\n", js_str(js, cond), cond_true);
  if (!cond_true) js->flags |= F_NOEXEC;
  jsval_t blk = js_block_or_stmt(js);
  if (cond_true) res = blk;
  if (exe && !cond_true) js->flags &= (uint8_t) ~F_NOEXEC;
  if (lookahead(js) == TOK_ELSE) {
    js->consumed = 1;
    next(js);
    js->consumed = 1;
    if (cond_true) js->flags |= F_NOEXEC;
    blk = js_block_or_stmt(js);
    if (!cond_true) res = blk;
    if (cond_true && exe) js->flags &= (uint8_t) ~F_NOEXEC;
  }
  return res;
}

static inline bool expect(struct js *js, uint8_t tok, jsval_t *res) {
  if (next(js) != tok) {
    *res = js_mkerr(js, "parse error");
    return false;
  } else {
    js->consumed = 1;
    return true;
  }
}

static inline bool is_err2(jsval_t *v, jsval_t *res) {
  bool r = is_err(*v);
  if (r) *res = *v;
  return r;
}

static jsval_t js_for(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t v, res = js_mkundef();
  jsoff_t pos1 = 0, pos2 = 0, pos3 = 0, pos4 = 0;
  if (exe) mkscope(js);  // Enter new scope
  if (!expect(js, TOK_FOR, &res)) goto done;
  if (!expect(js, TOK_LPAREN, &res)) goto done;

  if (next(js) == TOK_SEMICOLON) {  // initialisation
  } else if (next(js) == TOK_LET) {
    v = js_let(js);
    if (is_err2(&v, &res)) goto done;
  } else if (next(js) == TOK_CONST) {
    v = js_const(js);
    if (is_err2(&v, &res)) goto done;
  } else if (next(js) == TOK_VAR) {
    v = js_var(js);
    if (is_err2(&v, &res)) goto done;
  } else {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_SEMICOLON, &res)) goto done;
  js->flags |= F_NOEXEC;
  pos1 = js->pos;  // condition
  if (next(js) != TOK_SEMICOLON) {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_SEMICOLON, &res)) goto done;
  pos2 = js->pos;  // final expr
  if (next(js) != TOK_RPAREN) {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_RPAREN, &res)) goto done;
  pos3 = js->pos;  // body
  v = js_block_or_stmt(js);
  if (is_err2(&v, &res)) goto done;
  pos4 = js->pos;  // end of body
  while (!(flags & F_NOEXEC)) {
    js->flags = flags, js->pos = pos1, js->consumed = 1;
    if (next(js) != TOK_SEMICOLON) {     // Is condition specified?
      v = resolveprop(js, js_expr(js));  // Yes. check condition
      if (is_err2(&v, &res)) goto done;  // Fail short on error
      if (!js_truthy(js, v)) break;      // Exit the loop if condition is false
    }
    js->pos = pos3, js->consumed = 1, js->flags |= F_LOOP;  // Execute the
    v = js_block_or_stmt(js);                               // loop body
    if (is_err2(&v, &res)) goto done;                       // Fail on error
    if (js->flags & F_BREAK) break;  // break was executed - exit the loop!
    js->flags = flags, js->pos = pos2, js->consumed = 1;  // Jump to final expr
    if (next(js) != TOK_RPAREN) {                         // Is it specified?
      v = js_expr(js);                                    // Yes. Execute it
      if (is_err2(&v, &res)) goto done;  // On error, fail short
    }
  }
  js->pos = pos4, js->tok = TOK_SEMICOLON, js->consumed = 0;
done:
  if (exe) delscope(js);  // Exit scope
  js->flags = flags;      // Restore flags
  return res;
}

static jsval_t js_while(struct js *js) {
  uint8_t flags = js->flags;
  jsval_t v, res = js_mkundef();
  jsoff_t condpos = 0, bodypos = 0, endpos = 0;
  if (!expect(js, TOK_WHILE, &res)) goto done;
  if (!expect(js, TOK_LPAREN, &res)) goto done;
  condpos = js->pos;
  js->flags |= F_NOEXEC;
  v = js_expr(js);
  if (is_err2(&v, &res)) goto done;
  if (!expect(js, TOK_RPAREN, &res)) goto done;
  bodypos = js->pos;
  v = js_block_or_stmt(js);
  if (is_err2(&v, &res)) goto done;
  endpos = js->pos;

  while (!(flags & F_NOEXEC)) {
    js->flags = flags;
    js->pos = condpos;
    js->consumed = 1;
    v = resolveprop(js, js_expr(js));
    if (is_err2(&v, &res)) goto done;
    if (!js_truthy(js, v)) break;
    js->flags = flags | F_LOOP;
    js->pos = bodypos;
    js->consumed = 1;
    v = js_block_or_stmt(js);
    if (is_err2(&v, &res)) goto done;
    if (js->flags & F_BREAK) break;
  }
  js->pos = endpos;
  js->tok = TOK_SEMICOLON;
  js->consumed = 0;
done:
  js->flags = flags;
  return res;
}

static jsval_t js_break(struct js *js) {
  if (js->flags & F_NOEXEC) {
  } else {
    if (!(js->flags & F_LOOP)) return js_mkerr(js, "not in loop");
    js->flags |= F_BREAK | F_NOEXEC;
  }
  js->consumed = 1;
  return js_mkundef();
}

static jsval_t js_continue(struct js *js) {
  if (js->flags & F_NOEXEC) {
  } else {
    if (!(js->flags & F_LOOP)) return js_mkerr(js, "not in loop");
    js->flags |= F_NOEXEC;
  }
  js->consumed = 1;
  return js_mkundef();
}

static jsval_t js_return(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  if (exe && !(js->flags & F_CALL)) return js_mkerr(js, "not in func");
  if (next(js) == TOK_SEMICOLON) return js_mkundef();
  jsval_t res = resolveprop(js, js_expr(js));
  if (exe) {
    js->pos = js->clen;     // Shift to the end - exit the code snippet
    js->flags |= F_RETURN;  // Tell caller we've executed
  }
  return resolveprop(js, res);
}

static jsval_t js_throw(struct js *js) {
  bool exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  if (next(js) != TOK_IDENTIFIER ||
      !streq("Error", 5, js->code + js->toff, js->tlen))
    return js_mkerr(js, "Error expected");
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsval_t message = resolveprop(js, js_expr(js));
  if (is_err(message)) return message;
  EXPECT(TOK_RPAREN, );
  return exe ? js_mkthrow_value(js, message) : js_mkundef();
}

static jsval_t js_stmt(struct js *js) {
  jsval_t res;
  // jsoff_t pos = js->pos - js->tlen;
  switch (next(js)) {  // clang-format off
    case TOK_CASE: case TOK_CATCH: case TOK_CLASS:
    case TOK_DEFAULT: case TOK_DELETE: case TOK_DO: case TOK_FINALLY:
    case TOK_IN: case TOK_INSTANCEOF: case TOK_NEW: case TOK_SWITCH:
    case TOK_THIS: case TOK_TRY: case TOK_VOID:
    case TOK_WITH: case TOK_YIELD:
      res = js_mkerr(js, "'%.*s' not implemented", (int) js->tlen, js->code + js->toff);
      break;
    case TOK_CONTINUE:  res = js_continue(js); break;
    case TOK_BREAK:     res = js_break(js); break;
    case TOK_LET:       res = js_let(js); break;
    case TOK_CONST:     res = js_const(js); break;
    case TOK_VAR:       res = js_var(js); break;
    case TOK_IF:        res = js_if(js); break;
    case TOK_LBRACE:    res = js_block(js, !(js->flags & F_NOEXEC)); break;
    case TOK_FOR:       res = js_for(js); break; // 25222 -> 27660
    case TOK_WHILE:     res = js_while(js); break;
    case TOK_RETURN:    res = js_return(js); break;
    case TOK_THROW:     res = js_throw(js); break;
    default:
      res = resolveprop(js, js_expr(js));
      while (!is_err(res) && next(js) == TOK_COMMA) {
        js->consumed = 1;
        res = resolveprop(js, js_expr(js));
      }
      break;
  }
  //printf("STMT [%.*s] -> %s, tok %d, flags %d\n", (int) (js->pos - pos), &js->code[pos], js_str(js, res), next(js), js->flags);
  if (is_err(res)) return res;
  if (next(js) != TOK_SEMICOLON && next(js) != TOK_EOF && next(js) != TOK_RBRACE) return js_mkerr(js, "; expected");
  js->consumed = 1;
  // clang-format on
  return res;
}

struct js *js_create(void *buf, size_t len) {
  struct js *js = NULL;
  if (len < sizeof(*js) + esize(T_OBJ)) return js;
  memset(buf, 0, len);                       // Important!
  js = (struct js *) buf;                    // struct js lives at the beginning
  js->mem = (uint8_t *) (js + 1);            // Then goes memory for JS data
  js->size = (jsoff_t) (len - sizeof(*js));  // JS memory size
  js->scope = mkobj(js, 0);                  // Create global scope
  js->size = js->size / 8U * 8U;             // Align js->size by 8 byte
  js->lwm = js->size;                        // Initial LWM: 100% free
  js->gct = js->size;  // GC disabled
  return js;
}

// clang-format off
void js_setgct(struct js *js, size_t gct) { (void) js; (void) gct; }
void js_setmaxcss(struct js *js, size_t max) { js->maxcss = (jsoff_t) max; }
jsval_t js_mktrue(void) { return mkval(T_BOOL, 1); }
jsval_t js_mkfalse(void) { return mkval(T_BOOL, 0); }
jsval_t js_mkundef(void) { return mkval(T_UNDEF, 0); }
jsval_t js_mknull(void) { return mkval(T_NULL, 0); }
jsval_t js_mknum(double value) { return tov(value); }
jsval_t js_mkobj(struct js *js) { return mkobj(js, 0); }
jsval_t js_mkfun(jsval_t (*fn)(struct js *, jsval_t *, int)) { return mkval(T_CFUNC, (size_t) (void *) fn); }
double js_getnum(jsval_t value) { return tod(value); }
int js_getbool(jsval_t value) { return vdata(value) & 1 ? 1 : 0; }

jsval_t js_glob(struct js *js) { (void) js; return mkval(T_OBJ, 0); }

void js_set(struct js *js, jsval_t obj, const char *key, jsval_t val) {
  if (vtype(obj) == T_OBJ) setprop(js, obj, js_mkstr(js, key, strlen(key)), val);
}

char *js_getstr(struct js *js, jsval_t value, size_t *len) {
  if (vtype(value) != T_STR) return NULL;
  jsoff_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *) &js->mem[off];
}

int js_type(jsval_t val) {
  switch (vtype(val)) {
    case T_UNDEF:   return JS_UNDEF;
    case T_NULL:    return JS_NULL;
    case T_BOOL:    return vdata(val) == 0 ? JS_FALSE: JS_TRUE;
    case T_STR:     return JS_STR;
    case T_NUM:     return JS_NUM;
    case T_ERR:     return JS_ERR;
    default:        return JS_PRIV;
  }
}
void js_stats(struct js *js, size_t *total, size_t *lwm, size_t *css) {
  if (total) *total = js->size;
  if (lwm) *lwm = js->lwm;
  if (css) *css = js->css;
}
// clang-format on

bool js_chkargs(jsval_t *args, int nargs, const char *spec) {
  int i = 0, ok = 1;
  for (; ok && i < nargs && spec[i]; i++) {
    uint8_t t = vtype(args[i]), c = (uint8_t) spec[i];
    ok = (c == 'b' && t == T_BOOL) || (c == 'd' && t == T_NUM) ||
         (c == 's' && t == T_STR) || (c == 'j');
  }
  if (spec[i] != '\0' || i != nargs) ok = 0;
  return ok;
}

jsval_t js_eval(struct js *js, const char *buf, size_t len) {
  // printf("EVAL: [%.*s]\n", (int) len, buf);
  jsval_t res = js_mkundef();
  if (len == (size_t) ~0U) len = strlen(buf);
  js->consumed = 1;
  js->tok = TOK_ERR;
  js->code = buf;
  js->clen = (jsoff_t) len;
  js->pos = 0;
  js->cstk = &res;
  while (next(js) != TOK_EOF && !is_err(res)) {
    res = js_stmt(js);
  }
  return res;
}

void js_dump(struct js *js) {
  jsoff_t off = 0, v;
  printf("JS size %u, brk %u, lwm %u, css %u, nogc %u\n", js->size, js->brk,
         js->lwm, (unsigned) js->css, js->nogc);
  while (off < js->brk) {
    memcpy(&v, &js->mem[off], sizeof(v));
    printf(" %5u: ", off);
    if ((v & 3U) == T_OBJ) {
      printf("OBJ %u %u\n", v & ~3U,
             loadoff(js, (jsoff_t) (off + sizeof(off))));
    } else if ((v & 3U) == T_PROP) {
      jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(v)));
      jsval_t val = loadval(js, (jsoff_t) (off + sizeof(v) + sizeof(v)));
      printf("PROP next %u%s, koff %u vtype %d vdata %lu\n", entity_next(v),
             (v & P_CONST) ? " const" : "", koff, vtype(val),
             (unsigned long) vdata(val));
    } else if ((v & 3) == T_STR) {
      jsoff_t len = offtolen(v);
      printf("STR %u [%.*s]\n", len, (int) len, js->mem + off + sizeof(v));
    } else {
      printf("???\n");
      break;
    }
    off += esize(v);
  }
}
