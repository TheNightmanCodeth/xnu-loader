#ifndef XNU_LOADER_FDT_H
#define XNU_LOADER_FDT_H

// The flattened device tree reader every boot path shares: the efi app, the Linux Image
// entries and the arm32 shim. Reads are bytewise and nothing calls a library, so it is
// safe before the MMU is on (built with -mstrict-align there)
#include <stdint.h>

#define FDT_MAGIC 0xd00dfeedU

// what fdt_walk_next found
enum {
  FDT_EV_DONE = 0,   // past the root, or the tree is malformed
  FDT_EV_BEGIN,      // a node opens: name
  FDT_EV_PROP,       // a property of the open node: prop, value, len
  FDT_EV_END,        // the open node closes
};

typedef struct {
  const uint8_t *at, *end;
  const char *strings;
  uint32_t level;
  // the node the event belongs to, the root is depth 1
  uint32_t depth;
  const char *name;
  const char *prop;
  const uint8_t *value;
  uint32_t len;
} FdtWalk;

// one property read ahead of the walk, as the properties of a node come before its children
typedef struct {
  const uint8_t *at;
  const char *prop;
  const uint8_t *value;
  uint32_t len;
} FdtProp;

uint32_t fdt_be32(const void *p);
// n big-endian cells as one number, the low 64 bits when n > 2
uint64_t fdt_cells(const void *p, uint32_t n);

// the tree's totalsize when blob is a flattened device tree, else 0
uint32_t fdt_check(const void *blob);

int fdt_walk_init(FdtWalk *w, const void *blob);
int fdt_walk_next(FdtWalk *w);

// the properties of the node an FDT_EV_BEGIN just opened, without moving the walk
int fdt_prop_first(const FdtWalk *w, FdtProp *p);
int fdt_prop_next(const FdtWalk *w, FdtProp *p);

// the index-th memory reservation block entry, 0 past the last
int fdt_rsv(const void *blob, uint32_t index, uint64_t *base, uint64_t *size);

int fdt_str_eq(const char *a, const char *b);
int fdt_str_prefix(const char *s, const char *prefix);
uint32_t fdt_str_len(const char *s);
// a node name against a bare name: "memory" matches "memory" and "memory@40000000"
int fdt_name_is(const char *name, const char *want);
// an exact entry of a nul-separated string list, as compatible is
int fdt_list_has(const void *list, uint32_t len, const char *want);
// a status property that leaves the device on
int fdt_okay(const void *status);

#endif
