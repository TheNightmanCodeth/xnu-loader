// flattened device tree reader, see include/fdt.h
#include "fdt.h"

#define TOK_BEGIN 1
#define TOK_END   2
#define TOK_PROP  3
#define TOK_NOP   4

uint32_t fdt_be32(const void *p) {
  const uint8_t *b = (const uint8_t *)p;
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

uint64_t fdt_cells(const void *p, uint32_t n) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i)
    v = v << 32 | fdt_be32((const uint8_t *)p + 4 * i);
  return v;
}

uint32_t fdt_str_len(const char *s) {
  uint32_t n = 0;
  while (s[n])
    ++n;
  return n;
}

int fdt_str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

int fdt_str_prefix(const char *s, const char *prefix) {
  while (*prefix) {
    if (*s++ != *prefix++)
      return 0;
  }
  return 1;
}

int fdt_name_is(const char *name, const char *want) {
  while (*want && *name == *want) {
    ++name;
    ++want;
  }
  return !*want && (!*name || *name == '@');
}

int fdt_list_has(const void *list, uint32_t len, const char *want) {
  const char *s = (const char *)list;
  uint32_t o = 0;
  while (o < len) {
    if (fdt_str_eq(s + o, want))
      return 1;
    while (o < len && s[o])
      ++o;
    ++o;
  }
  return 0;
}

int fdt_okay(const void *status) {
  return fdt_str_eq((const char *)status, "okay") || fdt_str_eq((const char *)status, "ok");
}

uint32_t fdt_check(const void *blob) {
  const uint8_t *h = (const uint8_t *)blob;
  if (!h || fdt_be32(h) != FDT_MAGIC)
    return 0;
  uint32_t size = fdt_be32(h + 4);
  return size >= 40 ? size : 0;
}

int fdt_walk_init(FdtWalk *w, const void *blob) {
  const uint8_t *h = (const uint8_t *)blob;
  uint32_t size = fdt_check(blob);

  w->at = w->end = 0;
  w->strings = 0;
  w->level = w->depth = w->len = 0;
  w->name = w->prop = 0;
  w->value = 0;
  if (!size)
    return 0;
  w->at = h + fdt_be32(h + 8);
  w->end = h + size;
  w->strings = (const char *)(h + fdt_be32(h + 12));
  return 1;
}

int fdt_walk_next(FdtWalk *w) {
  while (w->at && w->at + 4 <= w->end) {
    uint32_t tok = fdt_be32(w->at);
    w->at += 4;
    if (tok == TOK_NOP)
      continue;
    if (tok == TOK_BEGIN) {
      w->name = (const char *)w->at;
      w->at += (fdt_str_len(w->name) + 4) & ~3U;
      w->depth = ++w->level;
      return FDT_EV_BEGIN;
    }
    if (tok == TOK_END && w->level > 0) {
      w->depth = w->level--;
      // the root's end is the last thing in the tree
      if (w->level == 0)
        w->end = w->at;
      return FDT_EV_END;
    }
    if (tok == TOK_PROP && w->level > 0 && w->at + 8 <= w->end) {
      w->len = fdt_be32(w->at);
      w->prop = w->strings + fdt_be32(w->at + 4);
      w->value = w->at + 8;
      w->at += 8 + ((w->len + 3) & ~3U);
      w->depth = w->level;
      return FDT_EV_PROP;
    }
    break;
  }
  w->at = 0;
  return FDT_EV_DONE;
}

int fdt_prop_first(const FdtWalk *w, FdtProp *p) {
  p->at = w->at;
  return fdt_prop_next(w, p);
}

int fdt_prop_next(const FdtWalk *w, FdtProp *p) {
  while (p->at && p->at + 4 <= w->end) {
    uint32_t tok = fdt_be32(p->at);
    if (tok == TOK_NOP) {
      p->at += 4;
      continue;
    }
    if (tok != TOK_PROP || p->at + 12 > w->end)
      break;
    p->len = fdt_be32(p->at + 4);
    p->prop = w->strings + fdt_be32(p->at + 8);
    p->value = p->at + 12;
    p->at += 12 + ((p->len + 3) & ~3U);
    return 1;
  }
  p->at = 0;
  return 0;
}

int fdt_rsv(const void *blob, uint32_t index, uint64_t *base, uint64_t *size) {
  const uint8_t *h = (const uint8_t *)blob;
  const uint8_t *e = h + fdt_be32(h + 16) + 16 * index;
  *base = fdt_cells(e, 2);
  *size = fdt_cells(e + 8, 2);
  return *base || *size;
}
