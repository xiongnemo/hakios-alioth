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

void
air_finalize_for_gallium(nir_shader *nir, bool fs_position_is_sysval, bool base_group)
{
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
   if (nir->info.stage == MESA_SHADER_VERTEX)
      nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, MESA_SHADER_VERTEX);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4, nir_lower_io_use_interpolated_input_intrinsics);
}
