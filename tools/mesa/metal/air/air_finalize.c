/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * (the K40's, 11-18) What a Gallium driver expects of a shader it is handed: NIR's own system values lowered to the
 * hardware's, and I/O as intrinsics rather than variables -- ONE function now, used by the Metal library
 * (air_metal.c make_stage_cso / compute_state_for) and by the PC's ir3_compiler (tools/mesa/metal/air-ir3.patch,
 * load_air). Until 11-18 the PC tool did its own three passes, so a lowering that lived only in the library was never
 * exercised on the PC: [[point_coord]]'s (5d3c03c) looked fine there and did nothing on the phone (round b84).
 * air_run.c keeps its own copy (softpipe's road).
 */

#include "nir.h"
#include "util/set.h"
#include "nir_builder.h"
#include "nir_air.h"

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* load_point_coord -> a load of the input at VARYING_SLOT_PNTC, made at its first use */
struct pntc_lowering {
   nir_shader *nir;
   nir_variable *var;
};

static bool
lower_point_coord(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct pntc_lowering *st = data;
   if (intr->intrinsic != nir_intrinsic_load_point_coord)
      return false;
   if (!st->var) {
      st->var = nir_variable_create(st->nir, nir_var_shader_in, glsl_vec_type(2), "point_coord");
      st->var->data.location = VARYING_SLOT_PNTC;
      st->var->data.driver_location = st->nir->num_inputs++;
      st->var->data.interpolation = INTERP_MODE_NONE;   /* as GL's gl_PointCoord varying: replaced, not interpolated */
   }
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *v = nir_load_var(b, st->var);
   /* T turned over: freedreno's replacement is GL's orientation, and a Metal pass is drawn the other way up here --
    * round b85 read the point's top left as (12, 243) and its bottom left as (12, 12), Metal's being (0, 0) at the top
    * left (System Check metal.point.coord) */
   v = nir_vec2(b, nir_channel(b, v, 0), nir_fsub_imm(b, 1.0, nir_channel(b, v, 1)));
   if (intr->def.bit_size != 32)
      v = nir_f2fN(b, v, intr->def.bit_size);
   nir_def_rewrite_uses(&intr->def, v);
   nir_instr_remove(&intr->instr);
   return true;
}

/* a load_scratch / store_scratch this pass can turn into array elements: 32-bit words at a 4-byte-aligned offset */
static bool
scratch_word_access(nir_intrinsic_instr *intr)
{
   bool load = intr->intrinsic == nir_intrinsic_load_scratch;
   unsigned bits = load ? intr->def.bit_size : intr->src[0].ssa->bit_size;
   return bits == 32 && nir_intrinsic_align_mul(intr) >= 4 && nir_intrinsic_align_offset(intr) % 4 == 0;
}

/*
 * (the K40's, 11-18) A SHADER'S SCRATCH AS A LOCAL ARRAY, for ir3 to keep in registers. The frontend makes every alloca
 * (a local array, a struct reached by pointer) byte-addressed scratch -- load_scratch / store_scratch, which ir3 emits
 * as ldp / stp of private memory, through the caches. Given the same array as a function_temp variable, ir3 does what
 * Mesa's own frontends get: nir_opt_large_constants, vars_to_ssa for the elements reached at constant indices, an
 * indirect register array (nir_lower_locals_to_regs) for the rest, and private memory only past 256 bytes
 * (nir_lower_vars_to_scratch's threshold in ir3_nir.c). So a shader whose scratch is at most 256 bytes and only ever
 * read and written as aligned 32-bit words gets a uint[size / 4] instead: each word of an access one element, at the
 * offset / 4. Anything else -- 8-, 16- or 64-bit accesses, more than 256 bytes, scratch in a second function -- stays
 * as it was. The owner's apps had 28 such functions after a26c7b5 (Flutter's Porter-Duff and advanced blends,
 * Telegram's media editor, premium and liquid-glass shaders).
 */
static bool
scratch_to_local_array(nir_shader *nir)
{
   if (!nir->scratch_size || nir->scratch_size > 256 || nir->scratch_size % 4)
      return false;
   nir_function_impl *entry = nir_shader_get_entrypoint(nir);
   if (!entry)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_scratch && intr->intrinsic != nir_intrinsic_store_scratch)
               continue;
            if (impl != entry || !scratch_word_access(intr))
               return false;
         }
      }
   }

   nir_variable *var = nir_local_variable_create(entry, glsl_array_type(glsl_uint_type(), nir->scratch_size / 4, 4),
                                                 "air_scratch");
   nir_builder b = nir_builder_create(entry);
   nir_foreach_block(block, entry) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         bool load = intr->intrinsic == nir_intrinsic_load_scratch;
         if (!load && intr->intrinsic != nir_intrinsic_store_scratch)
            continue;
         b.cursor = nir_before_instr(instr);
         nir_def *word = nir_ushr_imm(&b, intr->src[load ? 0 : 1].ssa, 2);
         nir_deref_instr *array = nir_build_deref_var(&b, var);
         if (load) {
            nir_def *comps[NIR_MAX_VEC_COMPONENTS];
            for (unsigned i = 0; i < intr->def.num_components; i++)
               comps[i] = nir_load_deref(&b, nir_build_deref_array(&b, array, nir_iadd_imm(&b, word, i)));
            nir_def_rewrite_uses(&intr->def, nir_vec(&b, comps, intr->def.num_components));
         } else {
            nir_def *value = intr->src[0].ssa;
            unsigned mask = nir_intrinsic_write_mask(intr);
            for (unsigned i = 0; i < value->num_components; i++)
               if (mask & (1u << i))
                  nir_store_deref(&b, nir_build_deref_array(&b, array, nir_iadd_imm(&b, word, i)),
                                  nir_channel(&b, value, i), 1);
         }
         nir_instr_remove(instr);
      }
   }
   nir->scratch_size = 0;
   nir_metadata_preserve(entry, nir_metadata_control_flow);
   return true;
}

/* a threadgroup buffer's base marker (nir_air.h AIR_TG_BASE_INPUT) -> its offset */
static bool
place_tg_marker(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const unsigned *base = data;
   if (intr->intrinsic != nir_intrinsic_load_kernel_input)
      return false;
   unsigned at = nir_intrinsic_base(intr);
   if (at < AIR_TG_BASE_INPUT || at >= AIR_TG_BASE_INPUT + 4 * AIR_TG_BUFFERS || (at - AIR_TG_BASE_INPUT) % 4)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_rewrite_uses(&intr->def, nir_imm_int(b, base[(at - AIR_TG_BASE_INPUT) / 4]));
   nir_instr_remove(&intr->instr);
   return true;
}

/*
 * (the K40's, 11-18) Where a compute shader's threadgroup buffers go, for one dispatch's lengths (Metal's
 * -setThreadgroupMemoryLength:atIndex:, each rounded up to 16): the shader's static threadgroup memory first
 * (nir->info.shared_size, aligned to 16), then every buffer in index order -- one the shader does not declare has
 * length 0 and takes no room. Each base marker becomes its offset, and nir->info.shared_size the whole, which is
 * returned. The Metal library calls it as it makes the compute state for those lengths (air_metal.c
 * compute_state_for), the PC's ir3_compiler with lengths of its own (air-ir3.patch).
 */
unsigned
air_place_threadgroup_buffers(nir_shader *nir, const unsigned lengths[AIR_TG_BUFFERS])
{
   unsigned base[AIR_TG_BUFFERS], at = ALIGN_POT(nir->info.shared_size, 16);
   for (unsigned i = 0; i < AIR_TG_BUFFERS; i++) {
      base[i] = at;
      at += ALIGN_POT(lengths[i], 16);
   }
   nir_shader_intrinsics_pass(nir, place_tg_marker, nir_metadata_control_flow, base);
   nir->info.shared_size = at;
   return at;
}

/*
 * (the K40's, 11-27; HAKIOS_AIR_OUT_ONCE=1, boot-arg metal_out_once=1, off by default) AN OUTPUT STORED ON TWO PATHS.
 * A Metal function that returns from inside a branch stores its returned struct there (air_to_nir.c emit_return),
 * so an output is stored in more than one block. ir3 keeps the last store it compiles as THE output (its
 * outputs[] at nir->ir3), whichever path ran, and the end's collect then uses a value that does not dominate it --
 * ir3's register allocator asserted "handle_live_in: physreg != ~0" (release: SIGSEGV) on Photos' grid renderer
 * (PhotosUICore fragment_main). Mesa's own frontends make outputs temporaries that are copied out once, at the end
 * (nir_lower_io_to_temporaries); this does the same, and only for a shader that stores some output twice, so every
 * other translation stays what it was.
 */
static bool
outputs_stored_twice(nir_shader *nir)
{
   struct set *seen = _mesa_pointer_set_create(NULL);
   bool twice = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);
            if (st->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(st->src[0]));
            if (!var || var->data.mode != nir_var_shader_out)
               continue;
            /* or stored anywhere but the last block: on some path not at all (a discard), the same thing to ir3 */
            if (block != nir_impl_last_block(impl) || _mesa_set_search(seen, var)) {
               twice = true;
               break;
            }
            _mesa_set_add(seen, var);
         }
      }
   }
   _mesa_set_destroy(seen, NULL);
   return twice;
}

static bool
out_once_enabled(void)
{
   static int on = -1;

   if (on < 0) {
      const char *e = getenv("HAKIOS_AIR_OUT_ONCE");
      on = e && e[0] == '1';
   }
   return on;
}

void
air_finalize_for_gallium(nir_shader *nir, bool fs_position_is_sysval, bool base_group)
{
   NIR_PASS(_, nir, scratch_to_local_array);
   /* A fragment shader's [[position]]: the frontend hands it over as a system-value
    * variable, because that is the one form both of the drivers behind this library can
    * be served from (the long version is beside air.position in air_to_nir.c and in
    * docs/metal.md 10.28). freedreno sets pipe_caps.fs_position_is_sysval and wants the
    * intrinsic; softpipe does not set it and reads (0,0,0,0) from the intrinsic, because
    * nothing fills TGSI's system values for a fragment shader. For such a screen the
    * variable becomes an input varying here -- and then needs a driver_location, which
    * nothing else would give it.
    */
   if (nir->info.stage == MESA_SHADER_FRAGMENT && !fs_position_is_sysval) {
      static const struct nir_lower_sysvals_to_varyings_options pos_opts = {
         .frag_coord = true,
      };
      if (nir_lower_sysvals_to_varyings(nir, &pos_opts)) {
         nir_foreach_shader_in_variable(var, nir)
            if (var->data.location == VARYING_SLOT_POS)
               var->data.driver_location = nir->num_inputs++;
      }
   }
   /* (the K40's, 11-17) [[point_coord]] on the GPU: ir3 has no load_point_coord ("Unhandled intrinsic", SpriteKit's
    * PointSprite_FragFunc, VFX's sprite and particle fragment shaders, in the PC's debug ir3) and takes the point's
    * coordinate as the input at VARYING_SLOT_PNTC, which freedreno has the rasterizer write for every fragment of a
    * point (ir3_point_sprite: PNTC is always replaced; the origin at the top left, Metal's as GL's default). The
    * frontend emits load_point_coord itself, not a system-value variable, so this is its own small pass:
    * nir_lower_sysvals_to_varyings rewrites only variables (round b84). */
   if (nir->info.stage == MESA_SHADER_FRAGMENT && fs_position_is_sysval) {
      struct pntc_lowering st = { .nir = nir };
      nir_shader_intrinsics_pass(nir, lower_point_coord, nir_metadata_control_flow, &st);
   }
   NIR_PASS(_, nir, nir_lower_system_values);
   /* An options struct of all-false and not NULL, which are not the same thing here and
    * the difference is a trap. nir_lower_system_values *introduces*
    * load_base_workgroup_id when it lowers SYSTEM_VALUE_WORKGROUP_ID, and the only
    * thing that removes it again is
    *
    *     if (options && !options->has_base_workgroup_id)
    *        return nir_imm_zero(...);
    *
    * so NULL reads as "the driver has base workgroup ids" -- the opposite of what is
    * wanted -- and the intrinsic survives into the backend. With softpipe that is an
    * assertion in nir_to_tgsi rather than a message, and it stopped every kernel that
    * asks for its workgroup id. Zero is also the right answer here: rwa_dispatch
    * launches its grid from the origin, as Metal's own dispatch does.
    */
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   /* (the K40's, 11-17) and on the GPU (not softpipe: nir_to_tgsi has no base) a kernel's load_base_workgroup_id is
    * kept, for ir3's base_group driver param: thread_position_in_grid's thread offset (air_to_nir.c), Gallium's
    * grid_base, which rwa_dispatch_base sets for the pieces of a grid that does not divide into its threadgroups and
    * every other dispatch leaves zero. Nothing else reads it: the translator makes no load_global_invocation_id. */
   static const nir_lower_compute_system_values_options cs_options_base = { .has_base_workgroup_id = true };
   NIR_PASS(_, nir, nir_lower_compute_system_values, base_group ? &cs_options_base : &cs_options);
   /*
    * nir_lower_io_use_interpolated_input_intrinsics, and WITHOUT it every varying this
    * library drew was FLAT on the Adreno while softpipe interpolated it -- the mistake
    * that looked like a vertically flipped picture for three boots (docs/metal.md 10.26,
    * 10.32). Mesa's own comment on the flag says why: without it nir_lower_io "generates
    * regular load_input intrinsics and interpolation information must be inferred from
    * the list of input nir_variables", and the two consumers infer it from different
    * places. nir_to_tgsi reads each FS input's variable (INTERP_MODE_SMOOTH, as the
    * frontend sets it); ir3's setup_input does `so->inputs[n].flat = !coord` and builds
    * `coord` only for load_interpolated_input, so a plain load_input is flat.
    *
    * Nothing caught it because the only draws whose varying VARIES are Apple's
    * texturedVertex2D/3D feeding texturedFragment*: the generated vertex stage hands
    * every vertex the same texture coordinate (RWA_TEX_* in air_metal.c), and over a
    * constant flat and smooth are the same number.
    */
   /*
    * (the K40's, 11-17) A VERTEX STAGE'S OUTPUTS NUMBERED BY SLOT, position first -- the long black Flutter launch. The
    * translator numbered them in the order of the Metal output struct, and SPIRV-Cross (Impeller's shaders) puts the
    * position LAST, after the varyings: uv 0, colour 1, position 2. ir3 files an output at outputs[driver_location],
    * and the variant it builds for freedreno's hardware binning pass drops the varyings, leaving outputs[0] and [1]
    * empty -- and an empty entry reads as slot 0, which IS VARYING_SLOT_POS, at register r0.x. ir3_find_output_regid
    * then hands the binning pass that register for the position, every primitive lands somewhere else, every bin is
    * empty and every draw of the batch is skipped: Flutter's whole frame lost whenever freedreno rendered it in tiles
    * with binning (the old road's ~1 launch in 4, the tile resolve's every one; rounds b16-b32, System Check
    * metal.msaa.vsout: "position first: drawn; position last: LOST"). Mesa's own frontends number by slot, which is
    * what this asks for: nir_assign_io_var_locations sorts by location, so the built-ins come before VAR0. The
    * fragment stage is linked to the vertex stage by slot, not by these numbers.
    */
   if (out_once_enabled() && outputs_stored_twice(nir)) {
      /* (11-27) outputs_stored_twice above: temporaries, copied out once at the end */
      NIR_PASS(_, nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir), true, false);
      NIR_PASS(_, nir, nir_lower_global_vars_to_local);
      NIR_PASS(_, nir, nir_split_var_copies);
      NIR_PASS(_, nir, nir_lower_var_copies);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   }
   if (nir->info.stage == MESA_SHADER_VERTEX)
      nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, MESA_SHADER_VERTEX);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4, nir_lower_io_use_interpolated_input_intrinsics);
}
