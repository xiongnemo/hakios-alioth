/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air2nir: the frontend from the command line, as spirv2nir is for SPIR-V.
 *
 *   air2nir FILE [FUNCTION]          translate and print the NIR
 *   air2nir --ll FILE [FUNCTION]     print what the bitcode reader read, as LLVM assembly
 *   air2nir --list FILE.metallib     the functions of a container
 *   air2nir --report FILE...         one line per module, for tools/air/corpus.py
 *   air2nir --interface FILE [FUNC]  what the shader expects of the runtime
 *                                    (air_shader_info), key=value per line
 *
 * FILE is a .metallib (then FUNCTION picks one; without it, every function)
 * or one module's bitcode, wrapped or not. --report forks for every module,
 * so that an assertion in one is a line in the report and not the end of it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <inttypes.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"
#include "air_module.h"
#include "util/blob.h"
#include "util/ralloc.h"

/* --library: translate an air.visible library function inside a generated
 * entry point (nir_air.h). Off by default, because a wrapped function is not
 * an entry point of the corpus and must not be counted as one.
 */
static bool library_wrapper = false;

static const nir_shader_compiler_options default_options = {
   .lower_fdiv = false,
};

static void *
map_file(const char *path, size_t *size)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0) {
      fprintf(stderr, "air2nir: %s: %s\n", path, strerror(errno));
      return NULL;
   }
   struct stat st;
   fstat(fd, &st);
   *size = st.st_size;
   void *p = mmap(NULL, st.st_size ? st.st_size : 1, PROT_READ, MAP_PRIVATE, fd, 0);
   close(fd);
   return p == MAP_FAILED ? NULL : p;
}

struct module_ref {
   const char *name;
   int container_type;
   const void *bitcode;
   size_t size;
};

/* every module of FILE (one, unless it is a container) */
static unsigned
modules_of(void *ctx, const char *path, const char *only, struct module_ref **out)
{
   size_t size;
   const uint8_t *data = map_file(path, &size);
   if (!data)
      return 0;
   if (size >= 4 && !memcmp(data, "MTLB", 4)) {
      struct air_metallib lib;
      const char *err;
      if (!air_metallib_parse(ctx, data, size, &lib, &err)) {
         fprintf(stderr, "air2nir: %s: %s\n", path, err);
         return 0;
      }
      *out = rzalloc_array(ctx, struct module_ref, lib.num_functions ? lib.num_functions : 1);
      unsigned n = 0;
      for (unsigned i = 0; i < lib.num_functions; i++) {
         if (only && strcmp(only, lib.functions[i].name))
            continue;
         (*out)[n++] = (struct module_ref) {
            lib.functions[i].name, lib.functions[i].type,
            lib.functions[i].bitcode, lib.functions[i].bitcode_size,
         };
      }
      return n;
   }
   *out = rzalloc(ctx, struct module_ref);
   (*out)[0] = (struct module_ref) { path, -1, data, size };
   return 1;
}

static const char *
stage_name(int stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX: return "vertex";
   case MESA_SHADER_FRAGMENT: return "fragment";
   case MESA_SHADER_COMPUTE: return "kernel";
   case MESA_SHADER_NONE: return "visible";
   default: return "unknown";
   }
}

/* -- evaluating a shader by folding it -------------------------------------
 *
 * For the class of shader that only reads buffers and computes -- 256 of the
 * corpus's 271 fragment shaders -- the whole thing folds to constants once the
 * buffer loads are replaced by the bytes actually in the buffers. No
 * interpreter is needed: NIR's own constant folding is the interpreter, and
 * what comes out can be compared against LLVM's answer for the same bytes
 * (tools/air/cpuref.py). This is the check docs/air.md section 4 calls the
 * strongest one, and it needs no GPU.
 */

/* The argument buffer of a wrapped library function holds floats, not the byte
 * pattern.
 *
 * Every one of these functions is compiled with no-nans-fp-math and
 * no-infs-fp-math: the module promises its inputs are finite. Arbitrary bytes
 * are mostly not -- they are infinities, NaNs and denormals -- so the inputs
 * would land outside the contract the shader was compiled under, where
 * 0 * inf may legally be either NaN or inf and a disagreement means nothing.
 * One case did disagree exactly there (folded NaN, softpipe +inf) and said
 * nothing about either side.
 *
 * Word w is ((w * 13) % 29 - 14) / 4: finite, exactly representable in
 * binary32, and different for 29 words running -- so a harness that mixed up
 * two slots, or two lanes of one slot, would get a different answer. cpuref.py
 * fills the same words.
 */
static uint8_t
library_byte(unsigned i)
{
   float f = (float)((int)((i / 4 * 13) % 29) - 14) / 4.0f;
   uint32_t u;
   memcpy(&u, &f, sizeof(u));
   return (u >> (8 * (i % 4))) & 0xff;
}

/* the same pattern cpuref.py fills its buffers with */
static uint8_t
pattern_byte(unsigned i)
{
   return library_wrapper ? library_byte(i) : (i * 7 + 13) & 0xff;
}

struct eval_state {
   struct air_shader_info *info;
   bool folded_a_load;
};

static bool
fold_buffer_loads(struct nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct eval_state *st = data;
   if (intr->intrinsic != nir_intrinsic_load_ubo && intr->intrinsic != nir_intrinsic_load_ssbo)
      return false;

   unsigned isrc = 0, osrc = 1;
   if (!nir_src_is_const(intr->src[isrc]) || !nir_src_is_const(intr->src[osrc]))
      return false;
   unsigned index = nir_src_as_uint(intr->src[isrc]);
   unsigned offset = nir_src_as_uint(intr->src[osrc]);

   /* only a buffer the interface actually declared, and only within its size */
   unsigned size = 0;
   bool found = false;
   for (unsigned i = 0; i < st->info->num_resources; i++) {
      const struct air_resource *r = &st->info->resources[i];
      bool want = intr->intrinsic == nir_intrinsic_load_ubo
                     ? r->kind == AIR_RESOURCE_CONSTANT_BUFFER
                     : r->kind == AIR_RESOURCE_DEVICE_BUFFER;
      if (want && r->index == index) {
         size = r->size ? r->size : 256;
         found = true;
         break;
      }
   }
   if (!found)
      return false;

   unsigned bytes = intr->def.num_components * intr->def.bit_size / 8;
   if (offset + bytes > size)
      return false;

   nir_const_value v[NIR_MAX_VEC_COMPONENTS] = { 0 };
   for (unsigned c = 0; c < intr->def.num_components; c++) {
      uint64_t bits = 0;
      unsigned nb = intr->def.bit_size / 8;
      for (unsigned k = 0; k < nb; k++)
         bits |= (uint64_t)pattern_byte(offset + c * nb + k) << (8 * k);
      v[c].u64 = bits;
   }

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *imm = nir_build_imm(b, intr->def.num_components, intr->def.bit_size, v);
   nir_def_replace(&intr->def, imm);
   st->folded_a_load = true;
   return true;
}

/* print one stored output value, if it folded to a constant */
static void
print_output(nir_variable *var, nir_def *value)
{
   printf("  %-24s ", var->name ? var->name : "?");
   if (!nir_src_is_const(nir_src_for_ssa(value))) {
      printf("did not fold to a constant\n");
      return;
   }
   const struct glsl_type *t = glsl_without_array(var->type);
   bool is_float = glsl_get_base_type(t) == GLSL_TYPE_FLOAT ||
                   glsl_get_base_type(t) == GLSL_TYPE_FLOAT16;
   for (unsigned c = 0; c < value->num_components; c++) {
      uint64_t bits = nir_src_comp_as_uint(nir_src_for_ssa(value), c);
      if (is_float && value->bit_size == 32) {
         float f;
         uint32_t u = bits;
         memcpy(&f, &u, 4);
         printf("%s%.9g", c ? " " : "", (double)f);
      } else if (is_float && value->bit_size == 16) {
         printf("%s0x%04" PRIx64, c ? " " : "", bits);
      } else {
         printf("%s%" PRIu64, c ? " " : "", bits);
      }
   }
   printf("\n");
}

/* A wrapped library function returns its value by storing it to a buffer, not
 * to a shader output, so --eval has to read it from there. The type name comes
 * from Apple's own air.visible_output metadata, carried on the resource: how
 * to print the bytes is not something this can infer from a store.
 */
static unsigned
print_buffer_stores(nir_shader *nir, const struct air_shader_info *info)
{
   unsigned printed = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_ssbo)
               continue;
            if (!nir_src_is_const(intr->src[1]))
               continue;
            unsigned index = nir_src_as_uint(intr->src[1]);
            const char *type_name = NULL;
            for (unsigned i = 0; i < info->num_resources; i++) {
               const struct air_resource *r = &info->resources[i];
               if (r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->index == index && r->write)
                  type_name = r->type_name;
            }
            nir_def *value = intr->src[0].ssa;
            printf("  buffer%u%*s", index, 18, "");
            if (!nir_src_is_const(nir_src_for_ssa(value))) {
               printf("did not fold to a constant\n");
               printed++;
               continue;
            }
            bool is_float = !type_name || strstr(type_name, "float") ||
                            strstr(type_name, "half");
            for (unsigned c = 0; c < value->num_components; c++) {
               uint64_t bits = nir_src_comp_as_uint(nir_src_for_ssa(value), c);
               if (is_float && value->bit_size == 32) {
                  float f;
                  uint32_t u = bits;
                  memcpy(&f, &u, 4);
                  printf("%s%.9g", c ? " " : "", (double)f);
               } else if (is_float && value->bit_size == 16) {
                  printf("%s0x%04" PRIx64, c ? " " : "", bits);
               } else {
                  printf("%s%" PRIu64, c ? " " : "", bits);
               }
            }
            printf("\n");
            printed++;
         }
      }
   }
   return printed;
}

static int
eval_module(const struct module_ref *mod)
{
   void *ctx = ralloc_context(NULL);
   const char *err = NULL;
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, mod->bitcode, mod->size, &fc);
   struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
   struct air_shader_info info;
   nir_shader *nir = air_to_nir(ctx, mod->bitcode, mod->size, NULL, &options,
                                &default_options, &info, &err);
   if (!nir) {
      fprintf(stderr, "air2nir: %s: %s\n", mod->name, err);
      ralloc_free(ctx);
      return 1;
   }

   printf("eval: %s (%s), buffers filled with (i * 7 + 13) & 0xff\n", info.entry_point,
          gl_shader_stage_name(info.stage));
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER || r->kind == AIR_RESOURCE_DEVICE_BUFFER)
         printf("  %s buffer at index %u, %u bytes\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, r->size);
   }

   struct eval_state st = { .info = &info };
   nir_shader_intrinsics_pass(nir, fold_buffer_loads, nir_metadata_control_flow, &st);

   /* Fold what the loads made foldable, and nothing else: no algebraic
    * rewriting, so the arithmetic is IEEE and comparable with LLVM's.
    *
    * Control flow is simplified, though, and that is not a rewriting of any
    * arithmetic: once a comparison has folded, the branch under it is dead and
    * the phi below it has one live source. Without this, 20 of the corpus's
    * library functions had no folded value at all -- every one of them is
    * straight-line arithmetic behind one `if` -- and a shader with a branch
    * could never be compared against LLVM, which has no such trouble.
    */
   bool progress;
   unsigned rounds = 0;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
   } while (progress && ++rounds < 16);

   printf("eval: the outputs this shader stores:\n");
   unsigned printed = 0;
   printed += print_buffer_stores(nir, &info);
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
            if (!deref || deref->deref_type != nir_deref_type_var)
               continue;
            print_output(deref->var, intr->src[1].ssa);
            printed++;
         }
      }
   }
   if (!printed)
      printf("  (none: every store was removed, or none folded)\n");
   ralloc_free(ctx);
   return 0;
}

static void
one_line(const char *s, FILE *fp)
{
   for (; *s; s++)
      fputc(*s == '\n' || *s == '\t' ? ' ' : *s, fp);
}

/* --interface: what air_shader_info says the shader expects of the runtime,
 * which nir_print_shader cannot show because it is not in the NIR. Its own
 * mode rather than a line added to the others: the report's lines are parsed
 * by tools/air/corpus.py and the translated NIR by tools/air/fbfetchcheck.py,
 * and a field added to either would move numbers that are a baseline.
 *
 * One `key=value` per line, tab-separated, so a check can read it.
 */
static int
interface_module(const struct module_ref *mod)
{
   void *ctx = ralloc_context(NULL);
   const char *err = NULL;
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, mod->bitcode, mod->size, &fc);
   struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
   struct air_shader_info info;
   nir_shader *nir = air_to_nir(ctx, mod->bitcode, mod->size, NULL, &options,
                                &default_options, &info, &err);
   if (!nir) {
      fprintf(stderr, "air2nir: %s: %s\n", mod->name, err);
      ralloc_free(ctx);
      return 1;
   }

   printf("entry=%s\tstage=%s\n", info.entry_point, gl_shader_stage_name(info.stage));
   printf("inputs_read=0x%" PRIx64 "\n", (uint64_t)nir->info.inputs_read);
   printf("vertex_attributes=%u\tvertex_attributes_read=0x%x\n",
          info.num_vertex_attributes, info.vertex_attributes_read);
   for (unsigned i = 0; i < info.num_vertex_attributes; i++) {
      const struct air_vertex_attribute *a = &info.vertex_attributes[i];
      printf("attribute\tindex=%u\tlocation=%s\tdriver_location=%d\tcomponents=%u"
             "\tbits=%u\ttype=%s\tname=%s\tdecl=%s\tused=%s\tfc=%s\n",
             a->index, gl_vert_attrib_name((gl_vert_attrib)a->location),
             a->driver_location == ~0u ? -1 : (int)a->driver_location, a->components,
             a->bit_size,
             a->base_type == AIR_ATTRIB_FLOAT ? "float" :
             a->base_type == AIR_ATTRIB_UINT ? "uint" : "int",
             a->name ? a->name : "-", a->type_name ? a->type_name : "-",
             a->used ? "yes" : "no",
             a->function_constant ? a->function_constant : "-");
   }
   for (unsigned i = 0; i < info.num_varyings; i++)
      printf("varying\tslot=%u\tcomponents=%u\tname=%s\n", info.varyings[i].slot,
             info.varyings[i].components,
             info.varyings[i].name ? info.varyings[i].name : "-");
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      /* set and id are the Metal name of an argument-buffer member, which is
       * the one case where index is this frontend's number and not Metal's --
       * so a check can see both halves of the mapping and not just the one it
       * would have guessed (tools/air/argbufcheck.py)
       */
      printf("resource\tkind=%d\tindex=%u\tsize=%u\tname=%s\tset=%d\tid=%d\n",
             r->kind, r->index, r->size, r->name ? r->name : "-",
             r->argbuf_set == ~0u ? -1 : (int)r->argbuf_set,
             r->argbuf_id == ~0u ? -1 : (int)r->argbuf_id);
   }
   for (unsigned i = 0; i < info.num_notes; i++) {
      fputs("note\t", stdout);
      one_line(info.notes[i], stdout);
      fputc('\n', stdout);
   }
   ralloc_free(ctx);
   return 0;
}

/* (the K40's, 11-18) every field of an info, for --roundtrip's comparison */
static void
dump_info(FILE *fp, const struct air_shader_info *i)
{
#define S(x) ((x) ? (x) : "(null)")
   fprintf(fp, "stage %d entry %s res %u var %u attr %u read %#x rts %u fbf %#x rog %#x notes %u tg %u grid %d\n",
           i->stage, S(i->entry_point), i->num_resources, i->num_varyings, i->num_vertex_attributes,
           i->vertex_attributes_read, i->num_render_targets, i->fbfetch_targets, i->raster_order_groups,
           i->num_notes, i->threadgroup_bytes, i->reads_group_grid);
   for (unsigned k = 0; k < i->num_resources; k++) {
      const struct air_resource *r = &i->resources[k];
      const struct air_sampler_state *s = &r->sampler;
      fprintf(fp, "r %d %u %u %d%d %u %s %s %d %u %u %u %u %d %#" PRIx64 " %u%u%u %u%u%u %d %u %u %u %u %u %u %d %u %u\n",
              r->kind, r->index, r->size, r->read, r->write, r->param, S(r->name), S(r->type_name), r->unused,
              r->array_length, r->type_size, r->type_align, r->num_members, r->inline_sampler, r->sampler_state,
              s->address[0], s->address[1], s->address[2], s->mag_filter, s->min_filter, s->mip_filter,
              s->pixel_coord, s->compare_func, s->max_anisotropy, s->lod_min, s->lod_max, s->border_color,
              s->reduction, r->raster_order_group, r->argbuf_set, r->argbuf_id);
      for (unsigned m = 0; m < r->num_members; m++)
         fprintf(fp, "  m %s %s %u %u %d\n", S(r->members[m].name), S(r->members[m].type_name),
                 r->members[m].offset, r->members[m].size, r->members[m].is_struct);
   }
   for (unsigned k = 0; k < i->num_varyings; k++) {
      const struct air_varying *v = &i->varyings[k];
      fprintf(fp, "v %s %u %u %d%d%d%d\n", S(v->name), v->slot, v->components, v->flat, v->centroid, v->sample,
              v->no_perspective);
   }
   for (unsigned k = 0; k < i->num_vertex_attributes; k++) {
      const struct air_vertex_attribute *a = &i->vertex_attributes[k];
      fprintf(fp, "a %u %u %u %u %u %d %s %s %d %s\n", a->index, a->location, a->components, a->driver_location,
              a->bit_size, a->base_type, S(a->name), S(a->type_name), a->used, S(a->function_constant));
   }
   for (unsigned k = 0; k < i->num_notes; k++)
      fprintf(fp, "n %s\n", S(i->notes[k]));
#undef S
}

static char *
shader_text(nir_shader *nir, const struct air_shader_info *info)
{
   char *buf = NULL;
   size_t len = 0;
   FILE *fp = open_memstream(&buf, &len);
   nir_foreach_function_impl(impl, nir) {
      nir_index_ssa_defs(impl);
      nir_index_blocks(impl);
   }
   nir_print_shader(nir, fp);
   dump_info(fp, info);
   fclose(fp);
   return buf;
}

/* (the K40's, 11-18) --roundtrip: each module translated, then through air_translation_serialize and
 * air_translation_deserialize (the Metal library's translation cache on disk), and the two compared as text -- the
 * NIR as nir_print shows it, every field of the info. One line a module; 1 if any differs. */
static int
roundtrip_module(const struct module_ref *mod)
{
   void *ctx = ralloc_context(NULL);
   const char *err = NULL;
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, mod->bitcode, mod->size, &fc);
   struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
   struct air_shader_info info, back;
   nir_shader *nir = air_to_nir(ctx, mod->bitcode, mod->size, NULL, &options, &default_options, &info, &err);
   if (!nir) {
      printf("roundtrip -\t%s\t(refused: not compared)\n", mod->name);
      ralloc_free(ctx);
      return 0;
   }
   struct blob b;
   blob_init(&b);
   bool ok = air_translation_serialize(&b, nir, &info);
   nir_shader *nir2 = ok ? air_translation_deserialize(ctx, &default_options, b.data, b.size, &back) : NULL;
   int bad = 1;
   if (!nir2) {
      printf("roundtrip FAIL\t%s\t(the blob did not come back: %zu bytes)\n", mod->name, b.size);
   } else {
      char *x = shader_text(nir, &info), *y = shader_text(nir2, &back);
      bad = strcmp(x, y) != 0;
      printf("roundtrip %s\t%s\t%zu bytes\n", bad ? "DIFF" : "ok", mod->name, b.size);
      if (bad) {
         /* the first line that differs */
         const char *p = x, *q = y;
         while (*p && *p == *q)
            p++, q++;
         while (p > x && p[-1] != '\n')
            p--, q--;
         printf("  was: %.160s\n  now: %.160s\n", p, q);
      }
      free(x);
      free(y);
   }
   blob_finish(&b);
   ralloc_free(ctx);
   return bad;
}

/* tools/air/corpus.py reads these lines: tab-separated key=value */
static void
report_one(const struct module_ref *mod)
{
   void *ctx = ralloc_context(NULL);
   const char *err = NULL, *calls = NULL;

   printf("module=%s\tcontainer=%s", mod->name,
          mod->container_type >= 0 ? air_metallib_type_name(mod->container_type) : "-");

   struct air_module *m = air_module_read(ctx, mod->bitcode, mod->size, &err);
   if (!m) {
      printf("\treader=fail\terror=");
      one_line(err, stdout);
      printf("\n");
      ralloc_free(ctx);
      return;
   }
   printf("\treader=ok");

   int stage = air_module_stage(ctx, mod->bitcode, mod->size, &calls, &err);
   printf("\tstage=%s\tcalls=", stage_name(stage));
   for (const char *c = calls; c && *c; c += strlen(c) + 1)
      printf("%s%s", c == calls ? "" : ",", c);

   /* Function constants are values the API supplies at pipeline creation. The
    * report has no API, so it compiles with every one set to zero and says so:
    * a module that needs values is not a module the translator cannot handle.
    */
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, mod->bitcode, mod->size, &fc);
   struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
   if (num_fc)
      printf("\tfc=%u", num_fc);

   struct air_shader_info info;
   nir_shader *nir = air_to_nir(ctx, mod->bitcode, mod->size, NULL, &options, &default_options,
                                &info, &err);
   if (!nir) {
      printf("\ttranslate=fail\terror=");
      one_line(err, stdout);
      printf("\n");
      ralloc_free(ctx);
      return;
   }
   nir_validate_shader(nir, "after air_to_nir");
   unsigned instrs = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block)
            instrs++;
      }
   }
   printf("\ttranslate=ok\tnir_instrs=%u", instrs);
   /* What was translated with a caveat: the shader was accepted and something
    * other than exactly what it asked for was done (air_shader_info.notes).
    * On the report line rather than on stderr, because the report is the only
    * output anything parses, and a caveat nobody can count is the silence this
    * channel exists to end.
    */
   for (unsigned i = 0; i < info.num_notes; i++) {
      fputs(i ? ";" : "\tnotes=", stdout);
      one_line(info.notes[i], stdout);
   }
   printf("\n");
   ralloc_free(nir);
   ralloc_free(ctx);
}

int
main(int argc, char **argv)
{
   enum { TRANSLATE, LL, LIST, REPORT, EVAL, INTERFACE, ROUNDTRIP } mode = TRANSLATE;
   int i = 1;
   while (i < argc && !strcmp(argv[i], "--library"))
      library_wrapper = true, i++;
   if (i < argc && !strcmp(argv[i], "--ll"))
      mode = LL, i++;
   else if (i < argc && !strcmp(argv[i], "--list"))
      mode = LIST, i++;
   else if (i < argc && !strcmp(argv[i], "--report"))
      mode = REPORT, i++;
   else if (i < argc && !strcmp(argv[i], "--eval"))
      mode = EVAL, i++;
   else if (i < argc && !strcmp(argv[i], "--interface"))
      mode = INTERFACE, i++;
   else if (i < argc && !strcmp(argv[i], "--roundtrip"))
      mode = ROUNDTRIP, i++;
   if (i >= argc) {
      fprintf(stderr,
              "usage: air2nir [--library] "
              "[--ll | --list | --report | --eval | --interface | --roundtrip] "
              "FILE [FUNCTION]\n");
      return 2;
   }

   glsl_type_singleton_init_or_ref();
   void *ctx = ralloc_context(NULL);
   int status = 0;

   if (mode == REPORT) {
      setvbuf(stdout, NULL, _IOLBF, 0);
      for (; i < argc; i++) {
         struct module_ref *mods;
         unsigned n = modules_of(ctx, argv[i], NULL, &mods);
         for (unsigned k = 0; k < n; k++) {
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
               alarm(60);
               report_one(&mods[k]);
               fflush(stdout);
               _exit(0);
            }
            int ws;
            waitpid(pid, &ws, 0);
            if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
               printf("module=%s\tcrash=%s %d\n", mods[k].name,
                      WIFSIGNALED(ws) ? "signal" : "exit",
                      WIFSIGNALED(ws) ? WTERMSIG(ws) : WEXITSTATUS(ws));
         }
      }
      ralloc_free(ctx);
      glsl_type_singleton_decref();
      return 0;
   }

   struct module_ref *mods;
   unsigned n = modules_of(ctx, argv[i], i + 1 < argc ? argv[i + 1] : NULL, &mods);
   if (n == 0)
      return 1;

   for (unsigned k = 0; k < n; k++) {
      const char *err = NULL;
      if (mode == EVAL) {
         status |= eval_module(&mods[k]);
         continue;
      }
      if (mode == INTERFACE) {
         status |= interface_module(&mods[k]);
         continue;
      }
      if (mode == ROUNDTRIP) {
         status |= roundtrip_module(&mods[k]);
         continue;
      }
      if (mode == LIST) {
         printf("%-60s %-12s %zu\n", mods[k].name,
                air_metallib_type_name(mods[k].container_type), mods[k].size);
         continue;
      }
      if (mode == LL) {
         struct air_module *m = air_module_read(ctx, mods[k].bitcode, mods[k].size, &err);
         if (!m) {
            fprintf(stderr, "air2nir: %s: %s\n", mods[k].name, err);
            status = 1;
            continue;
         }
         air_module_print(m, stdout);
         continue;
      }
      struct air_constant_value *fc;
      unsigned num_fc = air_module_function_constants(ctx, mods[k].bitcode, mods[k].size, &fc);
      struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
      struct air_shader_info info;
      nir_shader *nir = air_to_nir(ctx, mods[k].bitcode, mods[k].size, NULL, &options,
                                   &default_options, &info, &err);
      if (!nir) {
         fprintf(stderr, "air2nir: %s: %s\n", mods[k].name, err);
         status = 1;
         continue;
      }
      nir_print_shader(nir, stdout);
   }

   ralloc_free(ctx);
   glsl_type_singleton_decref();
   return status;
}
