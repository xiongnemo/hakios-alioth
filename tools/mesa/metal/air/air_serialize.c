/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * (the K40's, 11-18) A translation -- the NIR air_to_nir hands back and its air_shader_info -- into a blob and out of one,
 * for a translation cache on disk (air_metal.c): an application's next launch in the boot finds its pipelines' stages
 * translated. Flutter Check's second launch still spent 0.57 ms of its 1.05 ms a pipeline translating AIR (round b91:
 * 67 pipelines, 38 of their 70 ms), the ir3 compile already coming from ir3's own disk cache.
 *
 * The NIR goes through Mesa's nir_serialize. The info's structs go as their bytes -- the cache is keyed by the
 * library's build (its file's time), so a layout never meets another build's -- and every pointer in them as what it
 * points to: the strings and arrays air_shader_info_copy copies, the same list. air2nir --roundtrip checks the two
 * against a fresh translation for every function of a library.
 */

#include <string.h>

#include "nir.h"
#include "nir_serialize.h"
#include "util/blob.h"
#include "util/ralloc.h"
#include "nir_air.h"

#define AIR_BLOB_MAGIC 0x41495231u   /* "AIR1" */

static void
put_str(struct blob *b, const char *s)
{
   blob_write_uint8(b, s != NULL);
   if (s)
      blob_write_string(b, s);
}

static const char *
get_str(void *ctx, struct blob_reader *r)
{
   if (!blob_read_uint8(r))
      return NULL;
   const char *s = blob_read_string(r);
   return s && !r->overrun ? ralloc_strdup(ctx, s) : NULL;
}

bool
air_translation_serialize(struct blob *b, const nir_shader *nir, const struct air_shader_info *info)
{
   blob_write_uint32(b, AIR_BLOB_MAGIC);
   blob_write_uint32(b, sizeof(*info));
   blob_write_bytes(b, info, sizeof(*info));
   put_str(b, info->entry_point);
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      blob_write_bytes(b, r, sizeof(*r));
      put_str(b, r->name);
      put_str(b, r->type_name);
      for (unsigned m = 0; m < r->num_members; m++) {
         blob_write_bytes(b, &r->members[m], sizeof(r->members[m]));
         put_str(b, r->members[m].name);
         put_str(b, r->members[m].type_name);
      }
   }
   for (unsigned i = 0; i < info->num_varyings; i++) {
      blob_write_bytes(b, &info->varyings[i], sizeof(info->varyings[i]));
      put_str(b, info->varyings[i].name);
   }
   for (unsigned i = 0; i < info->num_vertex_attributes; i++) {
      const struct air_vertex_attribute *a = &info->vertex_attributes[i];
      blob_write_bytes(b, a, sizeof(*a));
      put_str(b, a->name);
      put_str(b, a->type_name);
      put_str(b, a->function_constant);
   }
   for (unsigned i = 0; i < info->num_notes; i++)
      put_str(b, info->notes[i]);
   nir_serialize(b, nir, false);
   return !b->out_of_memory;
}

static nir_shader *
deserialize(void *mem_ctx, const nir_shader_compiler_options *options, const void *data, size_t size,
            struct air_shader_info *info)
{
   struct blob_reader r;
   blob_reader_init(&r, data, size);
   if (blob_read_uint32(&r) != AIR_BLOB_MAGIC || blob_read_uint32(&r) != sizeof(*info))
      return NULL;
   blob_copy_bytes(&r, info, sizeof(*info));
   if (r.overrun)
      return NULL;
   info->entry_point = get_str(mem_ctx, &r);
   if (info->num_resources) {
      info->resources = ralloc_array(mem_ctx, struct air_resource, info->num_resources);
      for (unsigned i = 0; i < info->num_resources && !r.overrun; i++) {
         struct air_resource *res = &info->resources[i];
         blob_copy_bytes(&r, res, sizeof(*res));
         res->name = get_str(mem_ctx, &r);
         res->type_name = get_str(mem_ctx, &r);
         if (res->num_members && !r.overrun) {
            res->members = ralloc_array(mem_ctx, struct air_struct_member, res->num_members);
            for (unsigned m = 0; m < res->num_members && !r.overrun; m++) {
               blob_copy_bytes(&r, &res->members[m], sizeof(res->members[m]));
               res->members[m].name = get_str(mem_ctx, &r);
               res->members[m].type_name = get_str(mem_ctx, &r);
            }
         } else {
            res->members = NULL;
         }
      }
   } else {
      info->resources = NULL;
   }
   if (info->num_varyings) {
      info->varyings = ralloc_array(mem_ctx, struct air_varying, info->num_varyings);
      for (unsigned i = 0; i < info->num_varyings && !r.overrun; i++) {
         blob_copy_bytes(&r, &info->varyings[i], sizeof(info->varyings[i]));
         info->varyings[i].name = get_str(mem_ctx, &r);
      }
   } else {
      info->varyings = NULL;
   }
   if (info->num_vertex_attributes) {
      info->vertex_attributes = ralloc_array(mem_ctx, struct air_vertex_attribute, info->num_vertex_attributes);
      for (unsigned i = 0; i < info->num_vertex_attributes && !r.overrun; i++) {
         struct air_vertex_attribute *a = &info->vertex_attributes[i];
         blob_copy_bytes(&r, a, sizeof(*a));
         a->name = get_str(mem_ctx, &r);
         a->type_name = get_str(mem_ctx, &r);
         a->function_constant = get_str(mem_ctx, &r);
      }
   } else {
      info->vertex_attributes = NULL;
   }
   if (info->num_notes) {
      info->notes = ralloc_array(mem_ctx, const char *, info->num_notes);
      for (unsigned i = 0; i < info->num_notes && !r.overrun; i++)
         info->notes[i] = get_str(mem_ctx, &r);
   } else {
      info->notes = NULL;
   }
   if (r.overrun)
      return NULL;
   nir_shader *nir = nir_deserialize(mem_ctx, options, &r);
   if (!nir || r.overrun || r.current != r.end)
      return NULL;
   return nir;
}

/* NULL, and the info zeroed -- no pointer of the blob's left in it -- when the blob is not a whole translation */
nir_shader *
air_translation_deserialize(void *mem_ctx, const nir_shader_compiler_options *options, const void *data, size_t size,
                            struct air_shader_info *info)
{
   nir_shader *nir = deserialize(mem_ctx, options, data, size, info);
   if (!nir)
      memset(info, 0, sizeof(*info));
   return nir;
}
