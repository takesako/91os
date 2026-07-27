#include "miniregex.h"

enum node_type {
  N_EMPTY, N_CHAR, N_DOT, N_CLASS, N_BOL, N_EOL,
  N_CAT, N_ALT, N_STAR, N_PLUS, N_QUESTION
};

struct node {
  unsigned char type, ch, negate;
  int left, right;
  unsigned char bits[32];
};

struct parser {
  const unsigned char *pattern;
  size_t length, position;
  struct node *nodes;
  size_t count, capacity;
  int error;
};

static int new_node(struct parser *p, unsigned char type, int left, int right) {
  if (p->count >= p->capacity) {
    size_t capacity = p->capacity == 0 ? 32 : p->capacity * 2;
    struct node *nodes = (struct node *) malloc(capacity * sizeof(*nodes));
    if (nodes == NULL) {
      p->error = 1;
      return -1;
    }
    if (p->nodes != NULL) {
      memcpy(nodes, p->nodes, p->count * sizeof(*nodes));
      free(p->nodes);
    }
    p->nodes = nodes;
    p->capacity = capacity;
  }
  struct node *n = &p->nodes[p->count];
  memset(n, 0, sizeof(*n));
  n->type = type;
  n->left = left;
  n->right = right;
  return (int) p->count++;
}

static void class_add(struct node *n, unsigned char c) {
  n->bits[c >> 3] |= (unsigned char) (1U << (c & 7));
}

static int escaped_char(struct parser *p, int in_class, struct node *class_node,
                        unsigned char *literal) {
  if (p->position >= p->length) {
    p->error = 1;
    return 0;
  }
  unsigned char c = p->pattern[p->position++];
  if (c == 'd' || c == 's' || c == 'w') {
    if (!in_class) return c == 'd' ? -1 : c == 's' ? -2 : -3;
    for (int i = 0; i < 256; i++) {
      int yes = c == 'd' ? (i >= '0' && i <= '9')
              : c == 's' ? (i == ' ' || i == '\t' || i == '\r' ||
                            i == '\n' || i == '\f' || i == '\v')
              : ((i >= '0' && i <= '9') || (i >= 'A' && i <= 'Z') ||
                 (i >= 'a' && i <= 'z') || i == '_');
      if (yes) class_add(class_node, (unsigned char) i);
    }
    return 0;
  }
  *literal = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' :
             c == 'f' ? '\f' : c == 'v' ? '\v' : c;
  return 1;
}

static int parse_alt(struct parser *p);

static int parse_class(struct parser *p) {
  int index = new_node(p, N_CLASS, -1, -1);
  if (index < 0) return -1;
  struct node *n = &p->nodes[index];
  if (p->position < p->length && p->pattern[p->position] == '^') {
    n->negate = 1;
    p->position++;
  }
  int have = 0;
  while (p->position < p->length && p->pattern[p->position] != ']') {
    unsigned char first = 0, last = 0;
    int first_literal;
    if (p->pattern[p->position++] == '\\')
      first_literal = escaped_char(p, 1, n, &first);
    else {
      first = p->pattern[p->position - 1];
      first_literal = 1;
    }
    if (p->error) return -1;
    if (!first_literal) {
      have = 1;
      continue;
    }
    if (p->position + 1 < p->length && p->pattern[p->position] == '-' &&
        p->pattern[p->position + 1] != ']') {
      p->position++;
      int last_literal;
      if (p->pattern[p->position++] == '\\')
        last_literal = escaped_char(p, 1, n, &last);
      else {
        last = p->pattern[p->position - 1];
        last_literal = 1;
      }
      if (!last_literal || first > last) {
        p->error = 1;
        return -1;
      }
      for (unsigned int c = first; c <= last; c++) class_add(n, (unsigned char) c);
    } else {
      class_add(n, first);
    }
    have = 1;
  }
  if (!have || p->position >= p->length || p->pattern[p->position] != ']') {
    p->error = 1;
    return -1;
  }
  p->position++;
  return index;
}

static int predefined_class(struct parser *p, int kind) {
  int index = new_node(p, N_CLASS, -1, -1);
  if (index < 0) return -1;
  struct node *n = &p->nodes[index];
  for (int i = 0; i < 256; i++) {
    int yes = kind == -1 ? (i >= '0' && i <= '9')
            : kind == -2 ? (i == ' ' || i == '\t' || i == '\r' ||
                            i == '\n' || i == '\f' || i == '\v')
            : ((i >= '0' && i <= '9') || (i >= 'A' && i <= 'Z') ||
               (i >= 'a' && i <= 'z') || i == '_');
    if (yes) class_add(n, (unsigned char) i);
  }
  return index;
}

static int parse_atom(struct parser *p) {
  if (p->position >= p->length) return new_node(p, N_EMPTY, -1, -1);
  unsigned char c = p->pattern[p->position++];
  if (c == '(') {
    if (p->position + 1 < p->length && p->pattern[p->position] == '?' &&
        p->pattern[p->position + 1] == ':') p->position += 2;
    int node = parse_alt(p);
    if (p->position >= p->length || p->pattern[p->position++] != ')') p->error = 1;
    return node;
  }
  if (c == '[') return parse_class(p);
  if (c == '.') return new_node(p, N_DOT, -1, -1);
  if (c == '^') return new_node(p, N_BOL, -1, -1);
  if (c == '$') return new_node(p, N_EOL, -1, -1);
  if (c == '\\') {
    unsigned char literal = 0;
    int kind = escaped_char(p, 0, NULL, &literal);
    if (kind < 0) return predefined_class(p, kind);
    if (!kind) return -1;
    int node = new_node(p, N_CHAR, -1, -1);
    if (node >= 0) p->nodes[node].ch = literal;
    return node;
  }
  int node = new_node(p, N_CHAR, -1, -1);
  if (node >= 0) p->nodes[node].ch = c;
  return node;
}

static int parse_repeat(struct parser *p) {
  int node = parse_atom(p);
  while (!p->error && p->position < p->length) {
    unsigned char c = p->pattern[p->position];
    unsigned char type = c == '*' ? N_STAR : c == '+' ? N_PLUS :
                         c == '?' ? N_QUESTION : 255;
    if (type == 255) break;
    p->position++;
    node = new_node(p, type, node, -1);
  }
  return node;
}

static int parse_concat(struct parser *p) {
  int node = -1;
  while (!p->error && p->position < p->length &&
         p->pattern[p->position] != ')' && p->pattern[p->position] != '|') {
    int right = parse_repeat(p);
    node = node < 0 ? right : new_node(p, N_CAT, node, right);
  }
  return node < 0 ? new_node(p, N_EMPTY, -1, -1) : node;
}

static int parse_alt(struct parser *p) {
  int node = parse_concat(p);
  while (!p->error && p->position < p->length && p->pattern[p->position] == '|') {
    p->position++;
    node = new_node(p, N_ALT, node, parse_concat(p));
  }
  return node;
}

static unsigned char *new_set(size_t length) {
  unsigned char *set = (unsigned char *) malloc(length + 1);
  if (set != NULL) memset(set, 0, length + 1);
  return set;
}

static int evaluate(const struct node *nodes, int index,
                    const unsigned char *text, size_t text_len,
                    const unsigned char *input, unsigned char *output) {
  const struct node *n = &nodes[index];
  size_t size = text_len + 1;
  if (n->type == N_EMPTY) {
    for (size_t i = 0; i < size; i++) output[i] |= input[i];
  } else if (n->type == N_CHAR || n->type == N_DOT || n->type == N_CLASS) {
    for (size_t i = 0; i < text_len; i++) if (input[i]) {
      int yes = n->type == N_CHAR ? text[i] == n->ch : n->type == N_DOT ? 1 :
                ((n->bits[text[i] >> 3] >> (text[i] & 7)) & 1) != n->negate;
      if (yes) output[i + 1] = 1;
    }
  } else if (n->type == N_BOL || n->type == N_EOL) {
    size_t position = n->type == N_BOL ? 0 : text_len;
    if (input[position]) output[position] = 1;
  } else if (n->type == N_CAT) {
    unsigned char *middle = new_set(text_len);
    if (middle == NULL) return 0;
    int ok = evaluate(nodes, n->left, text, text_len, input, middle) &&
             evaluate(nodes, n->right, text, text_len, middle, output);
    free(middle);
    return ok;
  } else if (n->type == N_ALT) {
    if (!evaluate(nodes, n->left, text, text_len, input, output)) return 0;
    if (!evaluate(nodes, n->right, text, text_len, input, output)) return 0;
  } else if (n->type == N_QUESTION) {
    for (size_t i = 0; i < size; i++) output[i] |= input[i];
    if (!evaluate(nodes, n->left, text, text_len, input, output)) return 0;
  } else if (n->type == N_STAR || n->type == N_PLUS) {
    unsigned char *closure = new_set(text_len), *next = new_set(text_len);
    if (closure == NULL || next == NULL) {
      free(closure); free(next);
      return 0;
    }
    if (n->type == N_STAR) memcpy(closure, input, size);
    else if (!evaluate(nodes, n->left, text, text_len, input, closure)) {
      free(closure); free(next);
      return 0;
    }
    for (;;) {
      memset(next, 0, size);
      if (!evaluate(nodes, n->left, text, text_len, closure, next)) {
        free(closure); free(next);
        return 0;
      }
      int changed = 0;
      for (size_t i = 0; i < size; i++) if (next[i] && !closure[i]) {
        closure[i] = 1;
        changed = 1;
      }
      if (!changed) break;
    }
    for (size_t i = 0; i < size; i++) output[i] |= closure[i];
    free(closure); free(next);
  }
  return 1;
}

int miniregex_match(const char *pattern, size_t pattern_len,
                    const char *text, size_t text_len, int anchored,
                    size_t *start, size_t *length) {
  struct parser parser;
  memset(&parser, 0, sizeof(parser));
  parser.pattern = (const unsigned char *) pattern;
  parser.length = pattern_len;
  int root = parse_alt(&parser);
  if (parser.error || parser.position != parser.length || root < 0) {
    free(parser.nodes);
    return -1;
  }

  unsigned char *input = new_set(text_len), *output = new_set(text_len);
  if (input == NULL || output == NULL) {
    free(input); free(output); free(parser.nodes);
    return -1;
  }

  int result = 0;
  size_t first = 0, last = 0;
  size_t limit = anchored ? 0 : text_len;
  for (size_t begin = 0; begin <= limit; begin++) {
    memset(input, 0, text_len + 1);
    memset(output, 0, text_len + 1);
    input[begin] = 1;
    if (!evaluate(parser.nodes, root, (const unsigned char *) text,
                  text_len, input, output)) {
      result = -1;
      break;
    }
    int found = 0;
    for (size_t end = text_len + 1; end-- > begin;) if (output[end]) {
      first = begin;
      last = end;
      found = 1;
      break;
    }
    if (found) {
      result = 1;
      break;
    }
  }
  if (result == 1) {
    if (start != NULL) *start = first;
    if (length != NULL) *length = last - first;
  }
  free(input);
  free(output);
  free(parser.nodes);
  return result;
}
