/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Sizes and alignments from the module's datalayout string, by LLVM's rules
 * (LangRef, "Data Layout"). Every AIR module of iOS 16.5 has the same layout,
 * but it is read rather than assumed: GEP offsets are computed from it, and
 * a wrong offset is a wrong read.
 */

#include "air_module.h"

#include <stdlib.h>
#include <string.h>

#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_math.h"

/* (the K40's, 11-17) the datalayout's i / f / v entries into m->layout, once a module: this used to be parsed on
 * every lookup ("the string is short and this is not hot") -- it was a sixth of every translation (callgrind over
 * Impeller's 65 shaders: strtoul, strchr and this loop, half a million calls), GEPs asking member by member */
static void
layout_parse(struct air_module *m)
{
   const char *s = m->datalayout ? m->datalayout : "";
   unsigned n = 0;

   for (const char *p = s; *p; p++)
      n += *p == '-';
   m->layout = ralloc_array(m, struct air_layout_entry, n + 1);
   m->num_layout = 0;
   while (*s && m->layout) {
      const char *end = strchr(s, '-');
      size_t len = end ? (size_t)(end - s) : strlen(s);
      char kind = s[0];
      if (len > 1 && (kind == 'i' || kind == 'f' || kind == 'v') && s[1] >= '0' && s[1] <= '9') {
         char *q;
         unsigned b = strtoul(s + 1, &q, 10);
         unsigned abi = (*q == ':') ? strtoul(q + 1, NULL, 10) : b;
         m->layout[m->num_layout++] = (struct air_layout_entry) { kind, b, abi };
      }
      s += len + (end != NULL);
   }
   m->layout_parsed = true;
}

static unsigned
layout_lookup(const struct air_module *cm, char kind, unsigned bits, bool *found)
{
   struct air_module *m = (struct air_module *)cm;   /* the parse is a memo of the module's own string */
   unsigned best_bigger = 0, best_bigger_bits = ~0u, largest = 0, largest_bits = 0;
   *found = false;
   if (!m->layout_parsed)
      layout_parse(m);
   for (unsigned i = 0; i < m->num_layout; i++) {
      const struct air_layout_entry *e = &m->layout[i];
      if (e->kind != kind)
         continue;
      unsigned b = e->bits, abi = e->abi_bits;
      if (b == bits) {
         *found = true;
         return abi;
      }
      if (b > bits && b < best_bigger_bits) {
         best_bigger_bits = b;
         best_bigger = abi;
      }
      if (b > largest_bits) {
         largest_bits = b;
         largest = abi;
      }
   }
   if (kind == 'i') {
      /* the smallest wider integer's, or failing that the widest */
      if (best_bigger_bits != ~0u)
         return best_bigger;
      if (largest_bits)
         return largest;
   }
   return 0;
}

unsigned
air_type_bit_size(const struct air_type *t)
{
   switch (t->kind) {
   case AIR_TYPE_INT: return t->bits;
   case AIR_TYPE_HALF:
   case AIR_TYPE_BFLOAT: return 16;
   case AIR_TYPE_FLOAT: return 32;
   case AIR_TYPE_DOUBLE: return 64;
   case AIR_TYPE_POINTER: return 64;
   case AIR_TYPE_VECTOR: return air_type_bit_size(t->elem);
   default: return 0;
   }
}

static uint64_t
store_size(const struct air_module *m, const struct air_type *t)
{
   switch (t->kind) {
   case AIR_TYPE_VECTOR:
      return DIV_ROUND_UP(t->count * air_type_bit_size(t->elem), 8);
   case AIR_TYPE_ARRAY:
   case AIR_TYPE_STRUCT:
      return air_type_alloc_size(m, t);
   default:
      return DIV_ROUND_UP(air_type_bit_size(t), 8);
   }
}

static unsigned type_abi_align(const struct air_module *m, const struct air_type *t);
static uint64_t type_alloc_size(const struct air_module *m, const struct air_type *t);

/* (the K40's, 11-17) the two below are memos in the type (air_module.h layout_align / layout_size): a type belongs
 * to one module and one translation, and its layout never changes once it has a body (an opaque struct is not
 * kept) */
unsigned
air_type_abi_align(const struct air_module *m, const struct air_type *t)
{
   if (t->layout_align)
      return t->layout_align;
   unsigned a = type_abi_align(m, t);
   if (a && !(t->kind == AIR_TYPE_STRUCT && t->opaque))
      ((struct air_type *)t)->layout_align = a;
   return a;
}

uint64_t
air_type_alloc_size(const struct air_module *m, const struct air_type *t)
{
   if (t->layout_size_known)
      return t->layout_size;
   uint64_t size = type_alloc_size(m, t);
   if (!(t->kind == AIR_TYPE_STRUCT && t->opaque)) {
      ((struct air_type *)t)->layout_size = size;
      ((struct air_type *)t)->layout_size_known = true;
   }
   return size;
}

static unsigned
type_abi_align(const struct air_module *m, const struct air_type *t)
{
   bool found;
   switch (t->kind) {
   case AIR_TYPE_INT: {
      unsigned a = layout_lookup(m, 'i', t->bits, &found);
      return a ? a / 8 : MAX2(1, util_next_power_of_two(DIV_ROUND_UP(t->bits, 8)));
   }
   case AIR_TYPE_HALF:
   case AIR_TYPE_BFLOAT:
   case AIR_TYPE_FLOAT:
   case AIR_TYPE_DOUBLE: {
      unsigned bits = air_type_bit_size(t);
      unsigned a = layout_lookup(m, 'f', bits, &found);
      return found ? a / 8 : bits / 8;
   }
   case AIR_TYPE_POINTER:
      return 8; /* p:64:64:64 */
   case AIR_TYPE_VECTOR: {
      unsigned bits = t->count * air_type_bit_size(t->elem);
      unsigned a = layout_lookup(m, 'v', bits, &found);
      if (found)
         return a / 8;
      return MAX2(1, util_next_power_of_two64(store_size(m, t)));
   }
   case AIR_TYPE_ARRAY:
      return air_type_abi_align(m, t->elem);
   case AIR_TYPE_STRUCT: {
      if (t->packed)
         return 1;
      unsigned a = 1;
      for (unsigned i = 0; i < t->num_members; i++)
         a = MAX2(a, air_type_abi_align(m, t->members[i]));
      return a;
   }
   default:
      return 1;
   }
}

uint64_t
air_struct_member_offset(const struct air_module *m, const struct air_type *t, unsigned n)
{
   uint64_t off = 0;
   for (unsigned i = 0; i < t->num_members; i++) {
      const struct air_type *mt = t->members[i];
      if (!t->packed)
         off = ALIGN_NPOT(off, air_type_abi_align(m, mt));
      if (i == n)
         return off;
      off += air_type_alloc_size(m, mt);
   }
   return off;
}

static uint64_t
type_alloc_size(const struct air_module *m, const struct air_type *t)
{
   switch (t->kind) {
   case AIR_TYPE_ARRAY:
      return t->count * air_type_alloc_size(m, t->elem);
   case AIR_TYPE_STRUCT: {
      uint64_t end = air_struct_member_offset(m, t, t->num_members);
      return ALIGN_NPOT(end, air_type_abi_align(m, t));
   }
   default:
      return ALIGN_NPOT(store_size(m, t), air_type_abi_align(m, t));
   }
}
