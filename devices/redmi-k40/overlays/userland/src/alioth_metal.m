/*
 * The device object of a Metal driver for this port, as a real library.
 *
 * (The K40's, plan M1: hackios-xnu-userland a79ff07's userland/src/redwoodmetal.m -- MIT, Copyright (c) 2026 Darwin
 * on redwood contributors, the licence in tools/mesa/metal/LICENSE.hackios-xnu-userland -- under this port's names:
 * the GPU service HakiosGPU, the compiler library libhakios.air, the classes AliothMetal*. Measurements quoted below
 * are a79ff07's, made on its Adreno 635; this one is an Adreno 650.)
 *
 * What Metal demands of a plugin's device class was measured rather than guessed
 * (docs/metal.md section 10): `MTLAddDevice` (0x181a1875c) wants an object that comes back
 * non-nil from -initWithAcceleratorPort:, conforms to MTLDevice and MTLDeviceSPI -- both
 * asserted, both fatal -- and answers -featureProfile, -maxBufferLength and
 * -supportsVertexAmplificationCount:. Four methods. Everything else in the registration
 * path, including the 64-field limits table and the 42 feature queries, _MTLDevice fills
 * in itself once -featureProfile answers.
 *
 * Three decisions in how this is written, each for a measured reason:
 *
 *   - The class is declared in SOURCE, not built with objc_allocateClassPair as the probes
 *     did. That needs the private class symbol at link time, and it is bindable: IOGPU
 *     itself imports `_OBJC_CLASS_$__MTLDevice` together with its metaclass. Only the name
 *     is declared here -- nothing about Apple's 36 ivars or their offsets, which belong to
 *     Metal 306.6.4 and are nowhere written down. Our own ivars are placed by the runtime
 *     after Apple's, which is what the non-fragile ABI is for.
 *   - `<MTLDeviceSPI>` is declared at RUNTIME, with class_addProtocol. The protocol is
 *     private and requires 337 instance methods; writing that declaration out would mean
 *     copying 337 signatures we do not implement, and the only thing the two assertions in
 *     MTLAddDevice read is conformsToProtocol:. One call says the same thing truthfully.
 *     MTLDeviceSPI adopts MTLDevice, so one declaration satisfies both.
 *   - -initWithAcceleratorPort: opens this port's OWN gpu service, `HakiosGPU`, the way
 *     Mesa's shim does (tools/mesa/hakios-drm/hakios.drm.c: IOServiceOpen with type 0,
 *     then msm-shaped requests through IOConnectCallStructMethod). That is the road the
 *     resource layer will take, and it already carries Mesa's own dispatches today. The
 *     IOGPU road was measured shut: IOServiceOpen on our IOAcceleratorES service with
 *     Apple's type 1 answers kIOReturnUnsupported.
 *
 * The first resource is here too. A Metal buffer on this port is a GEM allocation of the
 * kernel's own gpu driver, made and mapped exactly the way Mesa's shim makes and maps one
 * (tools/mesa/hakios-drm/hakios.drm.c): kMsmGemNew (0x42) for the memory, MSM_INFO_GET_IOVA
 * through kMsmGemInfo (0x43) for the address the GPU sees, IOConnectMapMemory64 for the
 * address the processor sees, kDrmGemClose (0x09) to give it back. The kernel makes that
 * memory write-combined, with no cache of this processor between the program and what the
 * GPU reads, which is also why -didModifyRange: has nothing to do.
 *
 * Metal.framework has no `_MTLBuffer` to inherit -- only `_MTLResource`, a thin base over
 * `_MTLObjectWithLabel` -- so the buffer class is ours, as it is in Apple's own drivers
 * (`IOGPUMetalBuffer`, `AGXA11FamilyBuffer`).
 *
 * The compute path is here too, and it is where subsystem (1) finally carries something.
 * Metal hands a driver one function's LLVM module through -[_MTLFunction bitcodeData], and
 * those bytes were checked against our own AIR reader over all eleven functions of
 * AV1SW.metallib -- same lengths, same checksums (docs/metal.md 10.12). The front end that
 * reads them lives in Mesa, so the compiling and the dispatching are asked of
 * /redwood/lib/libhakios.air.dylib through the contract in tools/mesa/air/air_metal.h.
 *
 * That library is loaded with dlopen rather than linked, for a measured reason: the device
 * object must exist even when there is no GPU. The port starts the Adreno only with the
 * boot argument gpu=1, and a device that refuses to be created at all would take Metal's
 * whole discovery down with it; a device that answers its name and its limits and refuses
 * to make a pipeline is the truthful object in that case.
 *
 * Ordering is Metal's, not Gallium's: an encoder RECORDS its dispatches and the command
 * buffer runs them when it is committed. Dispatching as they are encoded would be less
 * code and would quietly reorder work an application committed in a particular order.
 *
 * Depth and stencil are here, and what that amounts to is four things: a texture of a depth
 * pixel format, an MTLDepthStencilState over the private _MTLDepthStencilState (whose SPI
 * has four methods Apple leaves answering NO, overridden here with the truth about the
 * descriptor), the render pass descriptor's depth and stencil attachments, and the
 * depth-range half of -setViewport:, which is the only thing in Metal that moves this
 * driver's quad in z at all.
 *
 * ARGUMENT BUFFERS are here, and what they are here is a RECORDER: Metal's encoder writes
 * 8-byte handles into a buffer and those are an Apple GPU's descriptors, so this driver
 * records (resource, Metal id) against (destination buffer, destination offset) instead
 * and the draw binds every member the pipeline's shaders declare. The buffer's contents
 * are never read, and an argument buffer the application did not fill through an encoder
 * is refused BY NAME (docs/air.md 8.2, and the MTLArgumentEncoder implementation at the
 * end of this file).
 *
 * BUFFER OFFSETS are honoured, which they were not: Metal's own `offset:` is recorded at
 * the encoder and judged at the draw, where the pipeline says whether an index is a
 * stream, a set or a buffer argument. A `constant` one is served at any offset -- the
 * compiler library uploads the block when the GPU cannot fetch from that offset -- and a
 * `device` one the GPU cannot fetch from is refused by name, because the shader may write
 * it and a copy would lose the writes.
 *
 * What is NOT here, and is not pretended: heaps, events, fences and texture views -- the
 * rest of docs/metal.md 10.8. (The K40, 2026-11-09: blit encoders, the viewport rectangle,
 * [[threadgroup(0)]] memory and multisample resolve came off this list.)
 *
 * Every line this file writes carries the PROCESS it was written by
 * (`aliothmetal[progname pid]:`, see rw_print): a driver is a library, four or five
 * Metal clients are alive in one boot of this port, and a refusal nobody can attribute
 * is half a refusal.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>          /* for MTLSize and the public enumerations, nothing private */
#import <objc/message.h>
#include <pthread.h>                /* (the K40's) alioth_gpu, the one lock of the compiler library */
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>

#include "alioth_drawable.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* The contract with Mesa's side, copied here rather than included: this file is built by
 * userland/build.sh, which knows nothing of Mesa's tree, and the declarations are few.
 * tools/mesa/air/air_metal.h is the original and explains each call. */
struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
struct rwa_compute;
struct rwa_compute_info {
	unsigned max_threads_per_group, thread_execution_width, static_shared_bytes;
	unsigned buffers, textures, samplers;
	/* which of those buffer indices are ARGUMENT BUFFERS, one bit each: Metal has one
	 * buffer table and an index in it is either a set or an argument. Appended, so
	 * every field above keeps the offset this file already reads -- and this file and
	 * the library still have to be rebuilt together. */
	unsigned argbuf_mask;
};
struct rwa_texture_shape {
	unsigned pixel_format, type, width, height, depth;
	unsigned array_length, mip_levels, sample_count, usage;
};
struct rwa_sampler_state {
	unsigned address[3];
	unsigned min_filter, mag_filter, mip_filter, compare_function, max_anisotropy;
	unsigned normalized_coordinates;
	float lod_min, lod_max;
};
struct rwa_bindings {
	struct rwa_buffer *const *buffers;
	unsigned buffer_count;
	struct rwa_texture *const *textures;
	unsigned texture_count;
	struct rwa_sampler *const *samplers;
	unsigned sampler_count;
};
/* ARGUMENT BUFFERS. There is no argument buffer to write on this driver: the frontend
 * resolves every member of a set to a binding while translating, and Metal's 8-byte
 * handles are an Apple GPU's descriptors, which a Gallium driver cannot read. So
 * MTLArgumentEncoder RECORDS (resource, Metal id) against (destination buffer,
 * destination offset) and the draw binds what it finds -- air_metal.h carries the whole
 * of why, including why an argument buffer nobody encoded has to be refused by name. */
#define RWA_MAX_ARGBUF_MEMBERS 64
enum rwa_argbuf_kind {
	RWA_ARGBUF_BUFFER = 0,
	RWA_ARGBUF_TEXTURE = 1,
	RWA_ARGBUF_SAMPLER = 2,
};
struct rwa_argbuf_member {
	unsigned id;             /* Metal's [[id(n)]], which the application names */
	unsigned kind;           /* enum rwa_argbuf_kind */
	unsigned index;          /* the flat index the frontend allocated */
	unsigned write;
	char name[48];
};
struct rwa_argbuf_layout {
	unsigned metal_index;    /* the [[buffer(n)]] the set is bound at */
	unsigned encoded_length; /* 8 bytes per member the set declares: Apple's own number */
	unsigned alignment;
	unsigned member_count;
	struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];
};
/* Metal's buffer table for one stage: what is bound at each index AND AT WHAT OFFSET,
 * which `rwa_bindings.buffers` has no room for. One array for both of the things an
 * offset means, because Metal has one -setFragmentBuffer:offset:atIndex: behind both: for
 * an argument buffer it is WHICH ENCODING in that buffer to read, and for a buffer
 * argument it is which block of it the shader's pointer starts at. The library serves a
 * `constant` one at any offset (uploading when the GPU cannot fetch from it) and refuses
 * a `device` one the GPU cannot fetch from, by name -- air_metal.h says why. */
struct rwa_buffer_bind {
	struct rwa_buffer *buffer;   /* what the application bound at this Metal index */
	size_t offset;               /* Metal's own `offset:` */
};
struct rwa_render;
struct rwa_depth_stencil;
/* MTLVertexDescriptor's two arrays, indexed by Metal's own indices: attributes[n] by the
 * shader's [[attribute(n)]] and layouts[k] by the buffer index an attribute names. A
 * format of MTLVertexFormatInvalid (0) is "not configured", which is what a fresh
 * MTLVertexDescriptor holds everywhere, so a zeroed shape carries no layout at all. */
#define RWA_MAX_VERTEX_ATTRIBUTES 31
#define RWA_MAX_VERTEX_BUFFERS    31
struct rwa_vertex_attribute {
	unsigned format, offset, buffer_index;
};
struct rwa_vertex_layout {
	unsigned stride, step_function, step_rate;
};
/* MTLRenderPipelineColorAttachmentDescriptor's blend half, per colour attachment, as
 * Metal's own numbers: the library maps MTLBlendFactor, MTLBlendOperation and
 * MTLColorWriteMask by table and refuses what it cannot serve by name. `write_mask` is
 * MTLColorWriteMask, whose bits run the OTHER WAY from Gallium's colormask (Metal's Red is
 * 1 << 3) -- which is the library's business and not this driver's: this passes the number
 * Metal gave it and reverses nothing. */
struct rwa_color_blend {
	unsigned blending_enabled;
	unsigned rgb_blend_operation, alpha_blend_operation;
	unsigned source_rgb_blend_factor, destination_rgb_blend_factor;
	unsigned source_alpha_blend_factor, destination_alpha_blend_factor;
	unsigned write_mask;
};
struct rwa_render_shape {
	unsigned color_format[8];
	unsigned color_count;
	unsigned depth_format;
	unsigned stencil_format;
	unsigned sample_count;
	struct rwa_vertex_attribute vertex_attribute[RWA_MAX_VERTEX_ATTRIBUTES];
	struct rwa_vertex_layout vertex_layout[RWA_MAX_VERTEX_BUFFERS];
	/* APPENDED, 260 bytes at the END: every field above keeps the offset this driver
	 * already writes. A field inserted in the MIDDLE of a struct shared with the library
	 * has cost this port twice -- a primitive type that arrived as 1871292816, and
	 * struct air_resource growing by eight bytes so that every resource after the first
	 * was read eight bytes early. `color_blend_count` of 0 means this shape carries NO
	 * blend state, which is blending off and all four channels written; a zeroed entry
	 * cannot stand in for that, because MTLColorWriteMaskNone is 0 and Metal's default
	 * writeMask is All. */
	struct rwa_color_blend color_blend[8];
	unsigned color_blend_count;
};
struct rwa_render_info {
	unsigned vertex_buffers, fragment_buffers, fragment_textures, fragment_samplers;
	unsigned generated_vertex_stage;
	unsigned fragment_writes_depth;
	/* what the vertex descriptor became: how many elements were built, the Metal
	 * attribute index of each (in driver_location order, which is the shader's and not
	 * Metal's), how many stream slots a draw must cover and which of them are fetched
	 * from. The mask is what splits Metal's one buffer table at the draw. */
	unsigned vertex_attribute_count;
	unsigned vertex_attribute_index[16];
	unsigned vertex_streams;
	unsigned vertex_stream_mask;
	/* and which buffer indices of each stage are argument buffers. Metal's one table per
	 * stage now holds three things -- a vertex stream, a buffer argument and a set --
	 * and only the shader and the vertex descriptor together say which an index is. */
	unsigned vertex_argbuf_mask;
	unsigned fragment_argbuf_mask;
	/* and what the blend state became: one bit per attachment that ends up blending, and
	 * four bits per attachment of colour write mask in GALLIUM's order (R 0x1 .. A 0x8),
	 * which is MTLColorWriteMask's reversed. Printed rather than trusted: for any mask
	 * but All, None, green|blue and red|alpha these four bits MUST differ from the
	 * writeMask this driver passed, so a log line where they agree is a log line saying
	 * the bits were never reversed. Filled in BY the library, so this struct must not be
	 * shorter than the library's. */
	unsigned blend_enabled_mask;
	unsigned color_write_masks;
};
struct rwa_vertex_stream {
	struct rwa_buffer *buffer;
	size_t offset;
};
struct rwa_stencil_face {
	unsigned compare_function;
	unsigned stencil_fail_operation;
	unsigned depth_fail_operation;
	unsigned depth_stencil_pass_operation;
	unsigned read_mask, write_mask;
};
struct rwa_depth_stencil_state {
	unsigned depth_compare_function;
	unsigned depth_write_enabled;
	struct rwa_stencil_face front, back;
};
struct rwa_draw {
	struct rwa_texture *const *color;
	unsigned color_count;
	const float *clear_color;
	struct rwa_texture *depth;
	const float *clear_depth;
	const unsigned *clear_stencil;
	const struct rwa_depth_stencil *depth_stencil;
	unsigned stencil_reference[2];
	const float *depth_range;
	struct rwa_bindings vertex;
	struct rwa_bindings fragment;
	unsigned primitive;
	unsigned vertex_start, vertex_count;
	/* the indexed forms: -drawIndexedPrimitives: and its instanced ones. Appended, as
	 * air_metal.h says, so that every field above keeps the offset this driver already
	 * reads -- but the library and this file still have to be rebuilt together, and
	 * `vertex_count` is the INDEX count for an indexed draw (there is deliberately no
	 * second count field for the two to disagree in). */
	struct rwa_buffer *index_buffer;
	unsigned index_type;
	size_t index_buffer_offset;
	int base_vertex;
	unsigned instance_count;
	unsigned base_instance;
	/* the vertex streams: -setVertexBuffer:offset:atIndex:, indexed by that index, and
	 * a second array beside `vertex.buffers` because a stream carries an OFFSET and is
	 * checked against the layout rather than against the shader's [[buffer(n)]]. Both
	 * come off the one table Metal has, and the pipeline's vertex_stream_mask says
	 * which index belongs to which side. */
	const struct rwa_vertex_stream *vertex_streams;
	unsigned vertex_stream_count;
	/* Metal's buffer table for each stage: the offsets `vertex.buffers` and
	 * `fragment.buffers` have nowhere to put -- an argument buffer's encoding offset
	 * and a buffer argument's own offset alike. APPENDED rather than put inside
	 * rwa_bindings, which is embedded by value above: a field added there would move
	 * `primitive` and every index field after it, which is how a primitive type once
	 * arrived as 1871292816. */
	const struct rwa_buffer_bind *vertex_buffer_table;
	unsigned vertex_buffer_table_count;
	const struct rwa_buffer_bind *fragment_buffer_table;
	unsigned fragment_buffer_table_count;
	/* (the K40's) MTLViewport's rectangle, { originX, originY, width, height }; NULL is the whole attachment.
	 * Appended, as the tables were. */
	const float *viewport;
	/* (the K40's) the rasterizer state: MTLCullMode, CounterClockwise as 1, MTLScissorRect or NULL, Lines as 1 */
	unsigned cull_mode;
	unsigned front_ccw;
	const unsigned *scissor;
	unsigned fill_lines;
	/* (the K40's) 1: not waited for, the framebuffer kept -- a command buffer's draws batched into one pass */
	unsigned keep_bound;
	/* (the K40's) the blend colour (NULL: zero) and the depth bias { bias, slope, clamp } (NULL: none) */
	const float *blend_color;
	const float *depth_bias;
};
/* (the K40's) Metal's pipeline reflection, air_metal.h's rwa_argument (rwa_render_arguments) */
struct rwa_struct_member {
	const char *name;
	const char *type_name;
	unsigned offset, size;
	unsigned is_struct;
};
struct rwa_argument {
	unsigned kind;                    /* MTLArgumentType's own numbers */
	unsigned index;
	unsigned active;
	unsigned access;                  /* MTLArgumentAccess */
	unsigned size, align;
	unsigned array_length;
	unsigned texture_type;
	unsigned type_known;
	unsigned element, element_bits;   /* 1 float, 2 int, 3 uint (enum rwa_element) */
	unsigned is_depth;
	const char *name;
	const char *type_name;
	unsigned member_count;
	const struct rwa_struct_member *members;
};

/*
 * The library's MTLVertexFormat table, checked against Apple's own header HERE, where the
 * SDK is in hand: tools/mesa/air/air_metal.c carries those numbers as literals (it has no
 * Metal headers to read them from), this file is compiled against
 * iPhoneOS16.5.sdk/System/Library/Frameworks/Metal.framework/Headers/MTLVertexDescriptor.h,
 * and a disagreement between the two would be a vertex fetched in the wrong format rather
 * than anything that fails. So it is a build failure of this driver instead.
 *
 * Nothing in this file switches on these names; they are asserted and otherwise the
 * numbers pass through as the application gave them, exactly as MTLPixelFormat does.
 */
_Static_assert((int)MTLVertexFormatInvalid == 0 && (int)MTLVertexFormatUChar2 == 1 &&
		   (int)MTLVertexFormatChar2 == 4 &&
		   (int)MTLVertexFormatUChar2Normalized == 7 &&
		   (int)MTLVertexFormatChar2Normalized == 10,
	       "MTLVertexFormat's 8-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUShort2 == 13 && (int)MTLVertexFormatShort2 == 16 &&
		   (int)MTLVertexFormatUShort2Normalized == 19 &&
		   (int)MTLVertexFormatShort2Normalized == 22 &&
		   (int)MTLVertexFormatHalf2 == 25,
	       "MTLVertexFormat's 16-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatFloat == 28 && (int)MTLVertexFormatFloat4 == 31 &&
		   (int)MTLVertexFormatInt == 32 && (int)MTLVertexFormatUInt == 36 &&
		   (int)MTLVertexFormatInt1010102Normalized == 40 &&
		   (int)MTLVertexFormatUInt1010102Normalized == 41 &&
		   (int)MTLVertexFormatUChar4Normalized_BGRA == 42,
	       "MTLVertexFormat's 32-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUChar == 45 && (int)MTLVertexFormatChar == 46 &&
		   (int)MTLVertexFormatUCharNormalized == 47 &&
		   (int)MTLVertexFormatCharNormalized == 48 &&
		   (int)MTLVertexFormatUShort == 49 && (int)MTLVertexFormatShort == 50 &&
		   (int)MTLVertexFormatUShortNormalized == 51 &&
		   (int)MTLVertexFormatShortNormalized == 52 &&
		   (int)MTLVertexFormatHalf == 53,
	       "MTLVertexFormat's one-component forms are not where air_metal.c's table "
	       "has them");
_Static_assert((int)MTLVertexStepFunctionConstant == 0 &&
		   (int)MTLVertexStepFunctionPerVertex == 1 &&
		   (int)MTLVertexStepFunctionPerInstance == 2 &&
		   (int)MTLVertexStepFunctionPerPatch == 3 &&
		   (int)MTLVertexStepFunctionPerPatchControlPoint == 4,
	       "MTLVertexStepFunction is not what air_metal.c's refusals name");

/*
 * THE BLEND ENUMERATIONS, asserted for the same reason and with more riding on it: the
 * compiler library maps all three BY TABLE, and if Apple's numbering were not what those
 * tables assume the result would be a composite with the wrong arithmetic in it rather
 * than anything that fails. MTLBlendFactor and pipe_blendfactor agree on only two of
 * nineteen values, so there is no cast to fall back on and no accident that would be
 * harmless.
 *
 * This is one of the two independent sources for the numbering, and it is the one that
 * can be made into a BUILD FAILURE: the SDK header compiled against here is
 * iPhoneOS16.5.sdk's own MTLRenderPipeline.h, the release this driver runs on.
 *
 * MTLColorWriteMask is the one to look at twice. Its bits run from Alpha at 1 << 0 up to
 * Red at 1 << 3, which is the OPPOSITE ORDER from Gallium's colormask, where red is 0x1.
 * Four of the sixteen masks are palindromes and survive the confusion -- None, All,
 * green|blue and red|alpha -- so a driver that got this backwards would still pass any
 * test built out of "write everything" and "write nothing".
 */
_Static_assert((int)MTLBlendFactorZero == 0 && (int)MTLBlendFactorOne == 1 &&
		   (int)MTLBlendFactorSourceColor == 2 &&
		   (int)MTLBlendFactorOneMinusSourceColor == 3 &&
		   (int)MTLBlendFactorSourceAlpha == 4 &&
		   (int)MTLBlendFactorOneMinusSourceAlpha == 5 &&
		   (int)MTLBlendFactorDestinationColor == 6 &&
		   (int)MTLBlendFactorOneMinusDestinationColor == 7 &&
		   (int)MTLBlendFactorDestinationAlpha == 8 &&
		   (int)MTLBlendFactorOneMinusDestinationAlpha == 9 &&
		   (int)MTLBlendFactorSourceAlphaSaturated == 10,
	       "MTLBlendFactor's served values are not where air_metal.c's table has them");
_Static_assert((int)MTLBlendFactorBlendColor == 11 &&
		   (int)MTLBlendFactorOneMinusBlendColor == 12 &&
		   (int)MTLBlendFactorBlendAlpha == 13 &&
		   (int)MTLBlendFactorOneMinusBlendAlpha == 14 &&
		   (int)MTLBlendFactorSource1Color == 15 &&
		   (int)MTLBlendFactorOneMinusSource1Color == 16 &&
		   (int)MTLBlendFactorSource1Alpha == 17 &&
		   (int)MTLBlendFactorOneMinusSource1Alpha == 18,
	       "MTLBlendFactor's refused values are not where air_metal.c's refusals name "
	       "them, so a refusal would name the wrong factor");
_Static_assert((int)MTLBlendOperationAdd == 0 && (int)MTLBlendOperationSubtract == 1 &&
		   (int)MTLBlendOperationReverseSubtract == 2 &&
		   (int)MTLBlendOperationMin == 3 && (int)MTLBlendOperationMax == 4,
	       "MTLBlendOperation is not the identity air_metal.c's table states it is");
_Static_assert((int)MTLColorWriteMaskNone == 0 && (int)MTLColorWriteMaskAlpha == 1 &&
		   (int)MTLColorWriteMaskBlue == 2 && (int)MTLColorWriteMaskGreen == 4 &&
		   (int)MTLColorWriteMaskRed == 8 && (int)MTLColorWriteMaskAll == 0xf,
	       "MTLColorWriteMask's bits are not the reverse of Gallium's colormask, which "
	       "is what air_metal.c's table is");

/*
 * AND THE SHAPE OF THE TWO STRUCTS THIS FILE TYPES OUT BY HAND, which is the bug this
 * port has paid for twice: a primitive type that arrived as 1871292816, and struct
 * air_resource growing by eight bytes so that every resource after the first was read
 * eight bytes early.
 *
 * The numbers below are the same numbers asserted in tools/mesa/air/air_metal.c against
 * the library's own definitions. Neither file can see the other's structs, so this is
 * not a comparison -- it is two independent statements of one agreement, and an edit to
 * one side alone fails to build on that side. The offsets are asserted as well as the
 * sizes, because the whole discipline here is that a new field goes on the END: a field
 * inserted in the middle keeps the size and moves everything after it, which is exactly
 * the failure that is invisible.
 */
_Static_assert(sizeof(struct rwa_color_blend) == 32 &&
		   sizeof(struct rwa_render_shape) == 1052 &&
		   offsetof(struct rwa_render_shape, vertex_attribute) == 48 &&
		   offsetof(struct rwa_render_shape, vertex_layout) == 420 &&
		   offsetof(struct rwa_render_shape, color_blend) == 792 &&
		   offsetof(struct rwa_render_shape, color_blend_count) == 1048,
	       "struct rwa_render_shape here is not the shape air_metal.c asserts: a field "
	       "was added in the middle, or only one of the two copies was rebuilt");
_Static_assert(sizeof(struct rwa_render_info) == 116 &&
		   offsetof(struct rwa_render_info, vertex_argbuf_mask) == 100 &&
		   offsetof(struct rwa_render_info, blend_enabled_mask) == 108,
	       "struct rwa_render_info here is not the shape air_metal.c asserts, and this "
	       "one the LIBRARY writes into: too short is a buffer overrun");

/*
 * WHOSE SENTENCE THIS IS. Every line this driver writes goes through here, and the reason
 * is an attribution that could not be made: a boot's log held
 *
 *     aliothmetal: fragment buffer 2 at offset 1164: ...
 *     aliothmetal: no drawable: pixel format 500 is not one ...
 *
 * one after the other, and four or five Metal clients were alive in that boot (backboardd,
 * SpringBoard, chronod and this port's own application). The drawable refusal was pinned on
 * backboardd only by reading QuartzCore's own os_log line beside it, which is a guess, and
 * the offset refusals could not be attributed at all. A driver is a library: its lines are
 * written by somebody else's process, and the process is the first thing a reader needs.
 *
 * So the prefix is `aliothmetal[progname pid]:`, built once per process -- getprogname()
 * is the name launchd or the shell gave it, which is what the log's other lines carry too.
 *
 * It flushes, for the reason RW_ONCE below flushes: stdio to a console is block-buffered,
 * the processes that reach these sites are the ones that then abort, and a line written and
 * not flushed dies with the process. One flush per sentence costs nothing next to the draw
 * that provoked it.
 */
static void rw_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* and the one method this driver installs into Metal's own function class, whose reason
 * is a measurement and is written where it is done (the end of this file) */
static void ensure_function_encoder(void);

/*
 * (The K40: in an application -- the device registered by the application itself, plan M1 step 1 -- stdout goes
 * nowhere; the sentence goes to the console as well, where the kernel's log keeps it, in one write.)
 */
static void
rw_print(const char *fmt, ...)
{
	static char who[96];
	static int console = -2;
	char line[1024];
	int n;
	va_list ap;

	if (who[0] == '\0') {
		snprintf(who, sizeof(who), "aliothmetal[%s %d]", getprogname(), getpid());
	}
	if (console == -2) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	n = snprintf(line, sizeof(line), "%s: ", who);
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fflush(stdout);
	if (console >= 0) {
		(void)write(console, line, strlen(line));
	}
}

static struct {
	void *handle;
	struct rwa_device *(*device_open)(char *, size_t);
	void (*device_close)(struct rwa_device *);
	const char *(*device_name)(struct rwa_device *);
	struct rwa_buffer *(*buffer_create)(struct rwa_device *, size_t, char *, size_t);
	void *(*buffer_map)(struct rwa_buffer *);
	size_t (*buffer_size)(struct rwa_buffer *);
	void (*buffer_destroy)(struct rwa_buffer *);
	struct rwa_compute *(*compute_create)(struct rwa_device *, const void *, size_t,
					      const char *, struct rwa_compute_info *, char *,
					      size_t);
	void (*compute_destroy)(struct rwa_compute *);
	struct rwa_texture *(*texture_create)(struct rwa_device *,
					      const struct rwa_texture_shape *, char *, size_t);
	int (*texture_write)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			     const unsigned[3], const void *, size_t, char *, size_t);
	int (*texture_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			    const unsigned[3], void *, size_t, char *, size_t);
	int (*depth_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			  const unsigned[3], float *, uint8_t *, char *, size_t);
	void (*texture_destroy)(struct rwa_texture *);
	struct rwa_sampler *(*sampler_create)(struct rwa_device *,
					      const struct rwa_sampler_state *, char *, size_t);
	void (*sampler_destroy)(struct rwa_sampler *);
	int (*dispatch)(struct rwa_compute *, const struct rwa_bindings *,
			const unsigned[3], const unsigned[3], char *, size_t);
	int (*dispatch_buffers)(struct rwa_compute *, const struct rwa_bindings *,
				const struct rwa_buffer_bind *, unsigned, const unsigned[3],
				const unsigned[3], char *, size_t);
	int (*module_argbufs)(struct rwa_device *, const void *, size_t, const char *,
			      struct rwa_argbuf_layout *, unsigned, unsigned *, char *,
			      size_t);
	int (*argbuf_encode_buffer)(struct rwa_buffer *, size_t, unsigned,
				    struct rwa_buffer *, size_t, char *, size_t);
	int (*argbuf_encode_texture)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_texture *, char *, size_t);
	int (*argbuf_encode_sampler)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_sampler *, char *, size_t);
	unsigned (*argbuf_encoded_members)(const struct rwa_buffer *, size_t);
	struct rwa_render *(*render_create)(struct rwa_device *, const void *, size_t,
					    const char *, const void *, size_t, const char *,
					    const struct rwa_render_shape *,
					    struct rwa_render_info *, char *, size_t);
	void (*render_destroy)(struct rwa_render *);
	int (*render_draw)(struct rwa_render *, const struct rwa_draw *, char *, size_t);
	/* (the K40's: a pass that clears and draws nothing, -[AliothMetalRenderEncoder endEncoding]) */
	int (*render_clear)(struct rwa_device *, struct rwa_texture *const *, unsigned, const float *,
			    struct rwa_texture *, const float *, const unsigned *, char *, size_t);
	struct rwa_depth_stencil *(*depth_stencil_create)(struct rwa_device *,
							  const struct rwa_depth_stencil_state *,
							  char *, size_t);
	void (*depth_stencil_destroy)(struct rwa_depth_stencil *);
	int (*finish)(struct rwa_device *, char *, size_t);
	/* (the K40's) a kernel's [[threadgroup(0)]] size for its next dispatch; looked up, not required */
	int (*compute_threadgroup_bytes)(struct rwa_compute *, unsigned, char *, size_t);
	/* (the K40's) a multisample attachment resolved by the GPU; looked up, not required */
	int (*texture_resolve)(struct rwa_device *, struct rwa_texture *, struct rwa_texture *, char *, size_t);
	/* (the K40's) pipeline reflection, by stage; looked up, not required */
	int (*render_arguments)(struct rwa_render *, unsigned, struct rwa_argument *, unsigned, unsigned *, char *,
	    size_t);
	int (*compute_arguments)(struct rwa_compute *, struct rwa_argument *, unsigned, unsigned *, char *, size_t);
	/* (the K40's) a texture view over the same memory; looked up, not required */
	struct rwa_texture *(*texture_view)(struct rwa_texture *, const struct rwa_texture_shape *, char *, size_t);
	/* (the K40's) rwa_finish in two halves: the wait off the encoding thread (AliothMetalCommandBuffer
	 * -aliothDeliverHandlers); looked up, not required */
	void *(*flush)(struct rwa_device *, unsigned);
	int (*fence_wait)(struct rwa_device *, void *, char *, size_t);
} mesa;

/*
 * (the K40's) ONE LOCK FOR THE COMPILER LIBRARY. Its device is one Gallium pipe_context, which is not thread-safe,
 * and since a command buffer's completed handlers wait for the GPU on a queue of their own (-aliothDeliverHandlers),
 * a handler -- or the last release of a resource it held -- can call in while another thread commits. So every call
 * that touches the context goes through a wrapper that holds this lock for the call (recursive: a handler run
 * inline may call back in). Not wrapped: what reads only (buffer_map and _size, the arguments of reflection), the
 * translation of module_argbufs, and fence_wait, whose whole point is to wait without it.
 */
static pthread_mutex_t alioth_gpu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

/*
 * (the K40's) metal_stats=1 (a boot-arg): where command buffers' time goes, one line every 2 s while they are
 * committed -- made to -commit (encode: the application's own work and its encoders'), -commit's recorded work (run)
 * and the library's draw calls inside it, the flush, the GPU (flush to its fence), waits for the GPU inside -commit
 * (finish: blits, signals, indirect draws) and in -waitUntilCompleted, the library's lock when another thread held
 * it, and CAMetalLayer's -nextDrawable. Off, it costs one test where it would measure.
 */
#include <mach/mach_time.h>

enum { ST_ENCODE, ST_RUN, ST_DRAW, ST_FLUSH, ST_GPU, ST_FINISH, ST_WAIT, ST_LOCK, ST_DRAWABLE, ST_SIGNAL, ST_BLIT, ST_N };
static bool alioth_stats_on;
static uint64_t alioth_stats_t[ST_N], alioth_stats_n[ST_N], alioth_stats_buffers, alioth_stats_since;
static mach_timebase_info_data_t alioth_timebase;
static id (*alioth_next_drawable_imp)(id, SEL);

static void
alioth_stat(unsigned which, uint64_t since)
{
	if (alioth_stats_on) {
		__atomic_fetch_add(&alioth_stats_t[which], mach_absolute_time() - since, __ATOMIC_RELAXED);
		__atomic_fetch_add(&alioth_stats_n[which], 1, __ATOMIC_RELAXED);
	}
}

static double
alioth_ms(uint64_t ticks)
{
	return (double)ticks * alioth_timebase.numer / alioth_timebase.denom / 1e6;
}

/* each total over its count, in ms (0 for none) */
static double
alioth_stat_avg(const uint64_t *t, const uint64_t *n, unsigned which)
{
	return n[which] != 0 ? alioth_ms(t[which]) / (double)n[which] : 0.0;
}

/* at each -commit: the window's line once 2 s have passed (whichever thread sees it first prints it) */
static void
alioth_stats_commit(void)
{
	uint64_t now = mach_absolute_time(), since = __atomic_load_n(&alioth_stats_since, __ATOMIC_RELAXED);
	uint64_t t[ST_N], n[ST_N], buffers;

	if (!alioth_stats_on) {
		return;
	}
	__atomic_fetch_add(&alioth_stats_buffers, 1, __ATOMIC_RELAXED);
	if (since == 0) {
		(void)__atomic_compare_exchange_n(&alioth_stats_since, &since, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
		return;
	}
	if (alioth_ms(now - since) < 2000.0 ||
	    !__atomic_compare_exchange_n(&alioth_stats_since, &since, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		return;
	}
	buffers = __atomic_exchange_n(&alioth_stats_buffers, 0, __ATOMIC_RELAXED);
	for (unsigned i = 0; i < ST_N; i++) {
		t[i] = __atomic_exchange_n(&alioth_stats_t[i], 0, __ATOMIC_RELAXED);
		n[i] = __atomic_exchange_n(&alioth_stats_n[i], 0, __ATOMIC_RELAXED);
	}
	rw_print("stats: %.1f s, %llu buffers (%.0f/s); a buffer: encode %.2f ms, run %.2f ms (%.1f draws, %.2f ms of "
	         "them in the library), flush %.2f ms, GPU %.2f ms after it (%llu fenced); finish %llu x %.2f ms (%llu "
	         "blits, %llu signals set after the fence), "
	         "waitUntilCompleted %llu x %.2f ms, lock held elsewhere %llu x %.2f ms, nextDrawable %llu x %.2f ms\n",
	         alioth_ms(now - since) / 1000.0, (unsigned long long)buffers,
	         (double)buffers * 1000.0 / alioth_ms(now - since), alioth_stat_avg(t, n, ST_ENCODE),
	         alioth_stat_avg(t, n, ST_RUN), buffers != 0 ? (double)n[ST_DRAW] / (double)buffers : 0.0,
	         buffers != 0 ? alioth_ms(t[ST_DRAW]) / (double)buffers : 0.0, alioth_stat_avg(t, n, ST_FLUSH),
	         alioth_stat_avg(t, n, ST_GPU), (unsigned long long)n[ST_GPU], (unsigned long long)n[ST_FINISH],
	         alioth_stat_avg(t, n, ST_FINISH), (unsigned long long)n[ST_BLIT], (unsigned long long)n[ST_SIGNAL],
	         (unsigned long long)n[ST_WAIT], alioth_stat_avg(t, n, ST_WAIT),
	         (unsigned long long)n[ST_LOCK], alioth_stat_avg(t, n, ST_LOCK), (unsigned long long)n[ST_DRAWABLE],
	         alioth_stat_avg(t, n, ST_DRAWABLE));
}

static id
alioth_next_drawable(id layer, SEL selector)
{
	uint64_t t0 = mach_absolute_time();
	id drawable = alioth_next_drawable_imp(layer, selector);

	alioth_stat(ST_DRAWABLE, t0);
	return drawable;
}

/* once, when the device is made: the boot-arg read, and -nextDrawable timed when it is on */
static void
alioth_stats_setup(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		char args[1024] = "";
		size_t len = sizeof(args) - 1;
		Class layer;
		Method m;

		if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0 || strstr(args, "metal_stats=1") == NULL) {
			return;
		}
		mach_timebase_info(&alioth_timebase);
		layer = objc_getClass("CAMetalLayer");
		m = layer != Nil ? class_getInstanceMethod(layer, sel_registerName("nextDrawable")) : NULL;
		if (m != NULL) {
			alioth_next_drawable_imp = (id (*)(id, SEL))method_setImplementation(m, (IMP)alioth_next_drawable);
		}
		alioth_stats_on = true;
		rw_print("metal_stats=1: where command buffers' time goes, a line every 2 s (-nextDrawable %s)\n",
		         m != NULL ? "timed too" : "not found, not timed");
	});
}

/*
 * (the K40's) MTLSharedEventListener on this port. +alloc hands out an MTLSharedEventListenerInternal, whose
 * -initWithDispatchQueue: and -init make an IOKit notification port, give it the queue, and then register the pair with
 * a call that fails here -- and answer nil (20F66: 0x181b19ec0, the branch at +140; -init at +136). An engine that waits
 * on a shared event through a listener then has nil, and this driver's events took a nil listener as an exception
 * (System Check's metal.event). Those events never use that port -- they put the block on the listener's queue
 * themselves (-[AliothMetalEvent notifyListener:atValue:block:]) -- so in a process this driver is loaded in, the two
 * initialisers are replaced by ones that do the same less the registration: the port made and given the queue, both
 * kept in the ivars Apple's own -dealloc lets go of.
 */
static Method
alioth_own_method(Class cls, SEL selector)
{
	unsigned count = 0;
	Method *methods = class_copyMethodList(cls, &count), found = NULL;

	for (unsigned i = 0; i < count && found == NULL; i++) {
		if (method_getName(methods[i]) == selector) {
			found = methods[i];
		}
	}
	free(methods);
	return found;
}

static id
alioth_listener_init_queue(id self, SEL selector, dispatch_queue_t queue)
{
	Class internal = objc_getClass("MTLSharedEventListenerInternal");
	Ivar queue_ivar = class_getInstanceVariable(internal, "_dispatchQueue");
	Ivar port_ivar = class_getInstanceVariable(internal, "_notificationPort");
	struct objc_super above = { self, class_getSuperclass(internal) };
	IONotificationPortRef port;

	(void)selector;
	self = ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&above, sel_registerName("init"));
	if (self == nil) {
		return nil;
	}
	if (queue == NULL || (port = IONotificationPortCreate(MACH_PORT_NULL)) == NULL) {
		[self release];
		return nil;
	}
	*(IONotificationPortRef *)((char *)self + ivar_getOffset(port_ivar)) = port;
	*(dispatch_queue_t *)((char *)self + ivar_getOffset(queue_ivar)) = (dispatch_queue_t)[(id)queue retain];
	IONotificationPortSetDispatchQueue(port, queue);
	return self;
}

static id
alioth_listener_init(id self, SEL selector)
{
	dispatch_queue_t queue = dispatch_queue_create("MTLSharedEventListener", NULL);
	id made = alioth_listener_init_queue(self, selector, queue);

	[(id)queue release];
	return made;
}

static void
alioth_listener_fix(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		Class internal = objc_getClass("MTLSharedEventListenerInternal");
		Method with_queue = internal != Nil ? alioth_own_method(internal, sel_registerName("initWithDispatchQueue:")) :
		    NULL;
		Method plain = internal != Nil ? alioth_own_method(internal, sel_registerName("init")) : NULL;

		if (with_queue == NULL || plain == NULL || class_getInstanceVariable(internal, "_dispatchQueue") == NULL ||
		    class_getInstanceVariable(internal, "_notificationPort") == NULL) {
			rw_print("MTLSharedEventListenerInternal is not as 20F66 has it: its initialisers are left as they "
			         "are, and answer nil here\n");
			return;
		}
		method_setImplementation(with_queue, (IMP)alioth_listener_init_queue);
		method_setImplementation(plain, (IMP)alioth_listener_init);
	});
}

/* the library's lock; a wait for it, when another thread holds it, is counted (metal_stats=1) */
static void
alioth_gpu_lock(void)
{
	if (pthread_mutex_trylock(&alioth_gpu) != 0) {
		uint64_t t0 = mach_absolute_time();

		pthread_mutex_lock(&alioth_gpu);
		alioth_stat(ST_LOCK, t0);
	}
}

#define GPU_LOCKED(ret, name, params, args) \
	static ret (*unlocked_##name) params; \
	static ret locked_##name params \
	{ \
		alioth_gpu_lock(); \
		ret gpu_locked_result = unlocked_##name args; \
		pthread_mutex_unlock(&alioth_gpu); \
		return gpu_locked_result; \
	}
#define GPU_LOCKED_VOID(name, params, args) \
	static void (*unlocked_##name) params; \
	static void locked_##name params \
	{ \
		alioth_gpu_lock(); \
		unlocked_##name args; \
		pthread_mutex_unlock(&alioth_gpu); \
	}
#define GPU_LOCK_WRAP(name) \
	do { \
		if (mesa.name != NULL) { \
			unlocked_##name = mesa.name; \
			mesa.name = locked_##name; \
		} \
	} while (0)

GPU_LOCKED(struct rwa_device *, device_open, (char *w, size_t ws), (w, ws))
GPU_LOCKED_VOID(device_close, (struct rwa_device *d), (d))
GPU_LOCKED(struct rwa_buffer *, buffer_create, (struct rwa_device *d, size_t n, char *w, size_t ws), (d, n, w, ws))
GPU_LOCKED_VOID(buffer_destroy, (struct rwa_buffer *b), (b))
GPU_LOCKED(struct rwa_compute *, compute_create, (struct rwa_device *d, const void *bc, size_t n, const char *e,
    struct rwa_compute_info *i, char *w, size_t ws), (d, bc, n, e, i, w, ws))
GPU_LOCKED_VOID(compute_destroy, (struct rwa_compute *c), (c))
GPU_LOCKED(struct rwa_texture *, texture_create, (struct rwa_device *d, const struct rwa_texture_shape *s, char *w,
    size_t ws), (d, s, w, ws))
GPU_LOCKED(int, texture_write, (struct rwa_texture *t, unsigned l, unsigned s, const unsigned o[3],
    const unsigned z[3], const void *b, size_t st, char *w, size_t ws), (t, l, s, o, z, b, st, w, ws))
GPU_LOCKED(int, texture_read, (struct rwa_texture *t, unsigned l, unsigned s, const unsigned o[3],
    const unsigned z[3], void *b, size_t st, char *w, size_t ws), (t, l, s, o, z, b, st, w, ws))
GPU_LOCKED(int, depth_read, (struct rwa_texture *t, unsigned l, unsigned s, const unsigned o[3], const unsigned z[3],
    float *dp, uint8_t *sp, char *w, size_t ws), (t, l, s, o, z, dp, sp, w, ws))
GPU_LOCKED_VOID(texture_destroy, (struct rwa_texture *t), (t))
GPU_LOCKED(struct rwa_sampler *, sampler_create, (struct rwa_device *d, const struct rwa_sampler_state *s, char *w,
    size_t ws), (d, s, w, ws))
GPU_LOCKED_VOID(sampler_destroy, (struct rwa_sampler *s), (s))
GPU_LOCKED(int, dispatch, (struct rwa_compute *c, const struct rwa_bindings *b, const unsigned g[3],
    const unsigned z[3], char *w, size_t ws), (c, b, g, z, w, ws))
GPU_LOCKED(int, dispatch_buffers, (struct rwa_compute *c, const struct rwa_bindings *b,
    const struct rwa_buffer_bind *t, unsigned n, const unsigned g[3], const unsigned z[3], char *w, size_t ws),
    (c, b, t, n, g, z, w, ws))
GPU_LOCKED(int, argbuf_encode_buffer, (struct rwa_buffer *d, size_t o, unsigned i, struct rwa_buffer *b, size_t bo,
    char *w, size_t ws), (d, o, i, b, bo, w, ws))
GPU_LOCKED(int, argbuf_encode_texture, (struct rwa_buffer *d, size_t o, unsigned i, struct rwa_texture *t, char *w,
    size_t ws), (d, o, i, t, w, ws))
GPU_LOCKED(int, argbuf_encode_sampler, (struct rwa_buffer *d, size_t o, unsigned i, struct rwa_sampler *s, char *w,
    size_t ws), (d, o, i, s, w, ws))
GPU_LOCKED(struct rwa_render *, render_create, (struct rwa_device *d, const void *fb, size_t fn, const char *fe,
    const void *vb, size_t vn, const char *ve, const struct rwa_render_shape *s, struct rwa_render_info *i, char *w,
    size_t ws), (d, fb, fn, fe, vb, vn, ve, s, i, w, ws))
GPU_LOCKED_VOID(render_destroy, (struct rwa_render *r), (r))
GPU_LOCKED(int, render_draw, (struct rwa_render *r, const struct rwa_draw *dr, char *w, size_t ws), (r, dr, w, ws))
GPU_LOCKED(int, render_clear, (struct rwa_device *d, struct rwa_texture *const *c, unsigned n, const float *cc,
    struct rwa_texture *z, const float *cz, const unsigned *cs, char *w, size_t ws), (d, c, n, cc, z, cz, cs, w, ws))
GPU_LOCKED(struct rwa_depth_stencil *, depth_stencil_create, (struct rwa_device *d,
    const struct rwa_depth_stencil_state *s, char *w, size_t ws), (d, s, w, ws))
GPU_LOCKED_VOID(depth_stencil_destroy, (struct rwa_depth_stencil *s), (s))
GPU_LOCKED(int, finish, (struct rwa_device *d, char *w, size_t ws), (d, w, ws))
GPU_LOCKED(int, compute_threadgroup_bytes, (struct rwa_compute *c, unsigned n, char *w, size_t ws), (c, n, w, ws))
GPU_LOCKED(int, texture_resolve, (struct rwa_device *d, struct rwa_texture *s, struct rwa_texture *t, char *w,
    size_t ws), (d, s, t, w, ws))
GPU_LOCKED(struct rwa_texture *, texture_view, (struct rwa_texture *t, const struct rwa_texture_shape *s, char *w,
    size_t ws), (t, s, w, ws))
GPU_LOCKED(void *, flush, (struct rwa_device *d, unsigned f), (d, f))

/*
 * (the K40's) rwa_finish in its two halves: flushed with a fence under the library's lock, the fence waited for
 * outside it (rwa_fence_wait takes no lock), so that no other thread's calls stand behind the GPU's work. Whole,
 * the finish held the lock for all of the wait: metal_stats=1 had it "held elsewhere" 26 times in 2 s, 5.7 ms each,
 * while VectorKit drew (logs/094301).
 */
static int
alioth_flush_wait(struct rwa_device *device, char *why, size_t size)
{
	if (mesa.flush != NULL && mesa.fence_wait != NULL) {
		return mesa.fence_wait(device, mesa.flush(device, 1), why, size);
	}
	return mesa.finish(device, why, size);
}

/* a wait for the GPU inside -commit (a blit, an indirect draw), counted with metal_stats=1 */
static int
alioth_finish(struct rwa_device *device, char *why, size_t size)
{
	uint64_t t0 = mach_absolute_time();
	int rc = alioth_flush_wait(device, why, size);

	alioth_stat(ST_FINISH, t0);
	return rc;
}

static NSUInteger alioth_texel_bytes(NSUInteger format);     /* (with the blit encoder, below) */

/* Loaded once, and its absence is an answer rather than a crash: without the GPU there is
 * no library worth loading, and the device object still has to exist. */
static bool
mesa_available(void)
{
	static bool tried;

	if (tried) {
		return mesa.handle != NULL;
	}
	tried = true;
	mesa.handle = dlopen("/redwood/lib/libhakios.air.dylib", RTLD_NOW);
	if (mesa.handle == NULL) {
		rw_print("no compiler library: %s\n", dlerror());
		return false;
	}
	*(void **)&mesa.device_open = dlsym(mesa.handle, "rwa_device_open");
	*(void **)&mesa.device_close = dlsym(mesa.handle, "rwa_device_close");
	*(void **)&mesa.device_name = dlsym(mesa.handle, "rwa_device_name");
	*(void **)&mesa.buffer_create = dlsym(mesa.handle, "rwa_buffer_create");
	*(void **)&mesa.buffer_map = dlsym(mesa.handle, "rwa_buffer_map");
	*(void **)&mesa.buffer_size = dlsym(mesa.handle, "rwa_buffer_size");
	*(void **)&mesa.buffer_destroy = dlsym(mesa.handle, "rwa_buffer_destroy");
	*(void **)&mesa.compute_create = dlsym(mesa.handle, "rwa_compute_create");
	*(void **)&mesa.compute_destroy = dlsym(mesa.handle, "rwa_compute_destroy");
	*(void **)&mesa.texture_create = dlsym(mesa.handle, "rwa_texture_create");
	*(void **)&mesa.texture_write = dlsym(mesa.handle, "rwa_texture_write");
	*(void **)&mesa.texture_read = dlsym(mesa.handle, "rwa_texture_read");
	*(void **)&mesa.depth_read = dlsym(mesa.handle, "rwa_depth_read");
	*(void **)&mesa.texture_destroy = dlsym(mesa.handle, "rwa_texture_destroy");
	*(void **)&mesa.sampler_create = dlsym(mesa.handle, "rwa_sampler_create");
	*(void **)&mesa.sampler_destroy = dlsym(mesa.handle, "rwa_sampler_destroy");
	*(void **)&mesa.dispatch = dlsym(mesa.handle, "rwa_dispatch");
	/* The argument-buffer calls, looked up but NOT required, for the reason drawing is
	 * not required: a library without them is a real state of this work, and the
	 * encoder says so when an application asks for one rather than the driver refusing
	 * to load. What must not happen is answering an encoder and then binding nothing,
	 * so -newArgumentEncoder... checks for these before it makes one. */
	*(void **)&mesa.dispatch_buffers = dlsym(mesa.handle, "rwa_dispatch_buffers");
	*(void **)&mesa.module_argbufs = dlsym(mesa.handle, "rwa_module_argbufs");
	*(void **)&mesa.argbuf_encode_buffer = dlsym(mesa.handle, "rwa_argbuf_encode_buffer");
	*(void **)&mesa.argbuf_encode_texture = dlsym(mesa.handle, "rwa_argbuf_encode_texture");
	*(void **)&mesa.argbuf_encode_sampler = dlsym(mesa.handle, "rwa_argbuf_encode_sampler");
	*(void **)&mesa.argbuf_encoded_members = dlsym(mesa.handle,
						       "rwa_argbuf_encoded_members");
	/* Drawing is looked up but not required: a compiler library that can compute and not
	 * yet draw is a real state of this work, and the driver says so when asked to draw
	 * rather than refusing to exist. */
	*(void **)&mesa.render_create = dlsym(mesa.handle, "rwa_render_create");
	*(void **)&mesa.render_destroy = dlsym(mesa.handle, "rwa_render_destroy");
	*(void **)&mesa.render_draw = dlsym(mesa.handle, "rwa_render_draw");
	*(void **)&mesa.render_clear = dlsym(mesa.handle, "rwa_render_clear");
	/* Depth likewise: a library that can draw and not test depth is a real state of
	 * this work, and -newDepthStencilStateWithDescriptor: says so when it is asked
	 * rather than the driver refusing to load. */
	*(void **)&mesa.depth_stencil_create = dlsym(mesa.handle, "rwa_depth_stencil_create");
	*(void **)&mesa.depth_stencil_destroy = dlsym(mesa.handle, "rwa_depth_stencil_destroy");
	*(void **)&mesa.finish = dlsym(mesa.handle, "rwa_finish");
	*(void **)&mesa.compute_threadgroup_bytes = dlsym(mesa.handle, "rwa_compute_set_threadgroup_bytes");
	*(void **)&mesa.texture_resolve = dlsym(mesa.handle, "rwa_texture_resolve");
	*(void **)&mesa.render_arguments = dlsym(mesa.handle, "rwa_render_arguments");
	*(void **)&mesa.compute_arguments = dlsym(mesa.handle, "rwa_compute_arguments");
	*(void **)&mesa.texture_view = dlsym(mesa.handle, "rwa_texture_view");
	*(void **)&mesa.flush = dlsym(mesa.handle, "rwa_flush");
	*(void **)&mesa.fence_wait = dlsym(mesa.handle, "rwa_fence_wait");
	if (mesa.device_open == NULL || mesa.compute_create == NULL || mesa.dispatch == NULL ||
	    mesa.texture_create == NULL || mesa.sampler_create == NULL) {
		/* rwa_dispatch's arguments changed when textures were added to the contract, so
		 * an older library would be called with the wrong shape of argument and do
		 * something arbitrary. Refusing it outright is the only safe reading of a
		 * missing rwa_texture_create. */
		rw_print("the compiler library is missing calls the contract names, "
		         "so it is older than this driver and will not be used\n");
		dlclose(mesa.handle);
		mesa.handle = NULL;
		return false;
	}
	/* (the K40's) every call that touches the context, under the one lock (alioth_gpu says why) */
	GPU_LOCK_WRAP(device_open);
	GPU_LOCK_WRAP(device_close);
	GPU_LOCK_WRAP(buffer_create);
	GPU_LOCK_WRAP(buffer_destroy);
	GPU_LOCK_WRAP(compute_create);
	GPU_LOCK_WRAP(compute_destroy);
	GPU_LOCK_WRAP(texture_create);
	GPU_LOCK_WRAP(texture_write);
	GPU_LOCK_WRAP(texture_read);
	GPU_LOCK_WRAP(depth_read);
	GPU_LOCK_WRAP(texture_destroy);
	GPU_LOCK_WRAP(sampler_create);
	GPU_LOCK_WRAP(sampler_destroy);
	GPU_LOCK_WRAP(dispatch);
	GPU_LOCK_WRAP(dispatch_buffers);
	GPU_LOCK_WRAP(argbuf_encode_buffer);
	GPU_LOCK_WRAP(argbuf_encode_texture);
	GPU_LOCK_WRAP(argbuf_encode_sampler);
	GPU_LOCK_WRAP(render_create);
	GPU_LOCK_WRAP(render_destroy);
	GPU_LOCK_WRAP(render_draw);
	GPU_LOCK_WRAP(render_clear);
	GPU_LOCK_WRAP(depth_stencil_create);
	GPU_LOCK_WRAP(depth_stencil_destroy);
	GPU_LOCK_WRAP(finish);
	GPU_LOCK_WRAP(compute_threadgroup_bytes);
	GPU_LOCK_WRAP(texture_resolve);
	GPU_LOCK_WRAP(texture_view);
	GPU_LOCK_WRAP(flush);
	return true;
}

/*
 * Say a thing once, per place that says it.
 *
 * Every method of this driver that declines to do something is called by a daemon that will
 * call it again on the next frame, so a line per call buries the log that matters and a
 * silent return tells us nothing at all. One line the first time a site is reached is what
 * names the feature the system actually wanted -- which is the whole value of answering
 * "no" instead of being absent: an absent selector aborts the daemon and says nothing, an
 * answer that refuses keeps it running AND leaves its name in the log.
 *
 * And it is flushed, because the measurement says it has to be: searching every log in
 * `logs/` for four different RW_ONCE strings -- the GPU priority, the one ring, the texture
 * view, the resource ID -- finds them **zero** times, while plain printf lines from these
 * same files are in every boot. The processes that reach these sites are the ones that then
 * abort (backboardd aborted 43 times in logs/214841-run.log), and stdio to a console is
 * block-buffered, so a line written and not flushed dies with the process. That is a
 * suspicion with evidence rather than a proof -- the sites may also simply never have been
 * reached -- and the flush is what makes the next boot able to tell the two apart: a line
 * that is now still missing was never reached. One flush per site per process costs nothing;
 * a diagnostic that has never once been seen costs the whole point of writing it.
 */
#define RW_ONCE(...)							\
	do {								\
		static BOOL rw_said_;					\
									\
		if (!rw_said_) {					\
			rw_said_ = YES;					\
			rw_print(__VA_ARGS__);				\
		}							\
	} while (0)

/*
 * A sentence that is true every time it is reached and says nothing new after the first: print
 * it, then only on powers of two, carrying the count. The window server asks this driver for a
 * two-plane YCbCr drawable 862 times in a boot, and 862 identical lines pushed the rest of that
 * boot out of the loader's log, which keeps the first 256 KB and the last megabyte. A refusal
 * that drowns the evidence of what it refused is a worse instrument than a quiet one.
 */
#define RW_AGAIN(...)							\
	do {								\
		static unsigned long rw_times_;				\
									\
		if ((++rw_times_ & (rw_times_ - 1)) == 0) {		\
			rw_print(__VA_ARGS__);				\
			if (rw_times_ > 1) {				\
				rw_print("  ... and that is the %lu'th "\
					 "time\n", rw_times_);		\
			}						\
		}							\
	} while (0)

/*
 * A BLEND LINE, deduplicated the way a repeated refusal is -- but on the CONTENT and not on
 * the call site, which is the difference between this and RW_AGAIN above.
 *
 * These lines are the measurement this driver exists to make, and they repeat enormously:
 * the window server built 127 pipelines in logs/080121-run.log and printed 240 attachment
 * lines, and there were THIRTEEN DISTINCT ONES among them. One boot of Resident Evil 4
 * would be a different order of magnitude -- 7 879 modules in its library -- and the
 * loader keeps the first 256 KB of a log and the last megabyte and drops the middle, which
 * is how 862 identical drawable refusals once pushed out the lines saying whether the game
 * had launched at all.
 *
 * So a line is printed the first time its exact content is seen and then on powers of two
 * of that content's own count, carrying the count. RW_AGAIN cannot do this: its counter is
 * per call site, so it would have suppressed twelve of those thirteen as if they were
 * repetitions of the first.
 *
 * Returns whether it printed, so the caller can attach the pipeline's LABEL to a line that
 * is new -- the label is what names the blend mode, and one per distinct configuration is
 * the whole of what it has to say.
 *
 * 32 entries is more than twice the thirteen a whole boot of the compositor produced, and
 * past it every further distinct line shares one counter rather than being dropped: a table
 * that silently stopped reporting would be the same instrument failure as the flood.
 */
static bool
rw_blend_line(unsigned attachment, unsigned format, const struct rwa_color_blend *b)
{
	static struct {
		unsigned attachment, format;
		struct rwa_color_blend b;
		unsigned long times;
	} seen[32];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = NULL;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].attachment == attachment && seen[i].format == format &&
		    memcmp(&seen[i].b, b, sizeof(*b)) == 0) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].attachment = attachment;
		seen[nseen].format = format;
		seen[nseen].b = *b;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == NULL) {
		times = &beyond;
	}
	if ((++*times & (*times - 1)) != 0) {
		return false;
	}
	rw_print("blend: attachment %u format %u blend %u rgb %u/%u op %u alpha %u/%u op %u "
	         "writeMask 0x%x\n", attachment, format, b->blending_enabled,
	         b->source_rgb_blend_factor, b->destination_rgb_blend_factor,
	         b->rgb_blend_operation, b->source_alpha_blend_factor,
	         b->destination_alpha_blend_factor, b->alpha_blend_operation, b->write_mask);
	if (*times > 1) {
		rw_print("  ... and that is the %lu'th pipeline asking for exactly that%s\n",
		         *times, times == &beyond ? ", counting every configuration past the "
		                                    "thirty-two this keeps apart" : "");
	}
	return true;
}

/* The same, for what the LIBRARY made of it: four distinct lines in that whole boot. */
static void
rw_blend_result(unsigned enabled_mask, unsigned write_masks)
{
	static struct { unsigned enabled, masks; unsigned long times; } seen[16];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = NULL;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].enabled == enabled_mask && seen[i].masks == write_masks) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].enabled = enabled_mask;
		seen[nseen].masks = write_masks;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == NULL) {
		times = &beyond;
	}
	if ((++*times & (*times - 1)) != 0) {
		return;
	}
	rw_print("blend: the library blends attachments 0x%x and writes 0x%08x (four Gallium "
	         "bits per attachment, R 0x1 .. A 0x8)\n", enabled_mask, write_masks);
	if (*times > 1) {
		rw_print("  ... and that is the %lu'th pipeline it made that of\n", *times);
	}
}

/* The private base class. Only what is used of it is declared, and nothing of its layout.
 * -init is its own (its method list has it); -initWithAcceleratorPort: is abstract there,
 * which is precisely the invitation this file accepts. */
@interface _MTLDevice : NSObject
- (instancetype)init;
@end

/* The private bases this driver stands on. Only what is used of each is declared, and
 * nothing of any layout. The designated initialisers are the ones Apple's own driver sends
 * to super (read out of IOGPUMetalCommandQueue 0x1ec16da90, IOGPUMetalCommandEncoder
 * 0x1ec171d14 and AGXA11FamilyComputePipeline 0x2092477e0), which is a better authority
 * than a guess at what an init of this kind ought to be called. */
@interface _MTLObjectWithLabel : NSObject
- (instancetype)init;
@end

@interface _MTLResource : _MTLObjectWithLabel
@end

@interface _MTLCommandQueue : _MTLObjectWithLabel
- (instancetype)initWithDevice:(id)device descriptor:(id)descriptor;
@end

@interface _MTLCommandBuffer : _MTLObjectWithLabel
- (instancetype)initWithQueue:(id)queue
	   retainedReferences:(BOOL)retained
	 synchronousDebugMode:(BOOL)synchronous;
@end

@interface _MTLCommandEncoder : _MTLObjectWithLabel
- (instancetype)initWithCommandBuffer:(id)commandBuffer;
/* Declared because it is CALLED: that initialiser retains the command buffer and this is the
 * only thing that gives it back (see -[AliothMetalRenderEncoder endEncoding]). */
- (void)endEncoding;
@end

@interface _MTLComputePipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

@interface _MTLRenderPipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

/*
 * The depth/stencil state's base class, and it is one of the good ones: everything but four
 * methods is Apple's and works for us. Read out of the cache the way the rest of this driver
 * was (tools/metal/absmeth.py over `_MTLDepthStencilState`, 10 selectors):
 *
 *   - -initWithDevice:depthStencilDescriptor: (0x181a29b38) is REAL and is the designated
 *     initialiser. It validates the descriptor field by field -- depthCompareFunction and
 *     each face's stencilCompareFunction, stencilFailureOperation, depthFailureOperation and
 *     depthStencilPassOperation are each compared against 8 and reported through
 *     MTLReportFailure when out of range -- then sends `init` to super
 *     (objc_msgSendSuper2), retains the device into _device and copies the descriptor's
 *     label into _label. It also asserts isKindOfClass: on both arguments (_MTLDevice for
 *     the device, MTLDepthStencilDescriptorInternal for the descriptor) and REPORTS rather
 *     than refuses: the failure paths branch back into the chain and the object is built
 *     either way.
 *   - -device and -label are ivar reads, so they work once that initialiser has run;
 *     -description, -formattedDescription: and -dealloc are Apple's.
 *   - -readsDepth, -writesDepth, -readsStencil, -writesStencil (0x181b1b5e0 and its three
 *     neighbours) are two-instruction NIL-STUBS: they answer NO. Those four are the whole of
 *     <MTLDepthStencilStateSPI> beyond the public protocol, and for a state that does write
 *     depth, NO is a false statement about this object -- so they are the four this driver
 *     overrides, out of the descriptor it was given.
 */
@interface _MTLDepthStencilState : NSObject
- (instancetype)initWithDevice:(id)device depthStencilDescriptor:(id)descriptor;
@end

/* The sampler has a real base class that owes its protocol nothing, and the initialiser
 * AGXA11FamilySampler sends to super (0x20930f0f0 -> 0x181a2d414). */
@interface _MTLSamplerState : NSObject
- (instancetype)initWithDevice:(id)device samplerDescriptor:(id)descriptor;
@end

/*
 * The one struct Metal reads out of a device by raw pointer, and the one answer on this
 * device that cannot be a refusal. Nothing here is guessed: the name and all three field
 * types are Apple's own, out of the type encoding MTLDeviceSPI records for the method
 * (`tools/metal/protometh.py`, joined to `absmeth.py` by implementation address):
 *
 *     -targetDeviceInfo    r^{MTLTargetDeviceArch=QI*}16@0:8
 *
 * so this declaration makes our method's recorded encoding the same string as Apple's,
 * rather than a `^v` that happens to pass in the same register. Which field means what was
 * read separately, off the readers -- see -targetDeviceInfo.
 */
struct MTLTargetDeviceArch {
	uint64_t target;      /* +0x00: nothing on this path reads it; Apple's own branch writes 0 */
	uint32_t version;     /* +0x08: the second half of MTLCompilerCache::Version{1, version} */
	char *name;           /* +0x10: formatted into "<pipelinelib>/pipelines.%s", never nil */
};

/* The offsets are the point of the struct, so they are checked rather than trusted: 0x10 is
 * the offset in the instruction that crashed without this answer. */
_Static_assert(sizeof(struct MTLTargetDeviceArch) == 24 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, version) == 0x8 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, name) == 0x10,
	       "Metal reads the name at +0x10 (0x181a3ca3c) and the version at +0x8");

@interface AliothMetalDevice : _MTLDevice {
	io_service_t _acceleratorService;   /* the IOAcceleratorES service Metal handed us */
	io_connect_t _gpu;                  /* this port's own gpu client, or IO_OBJECT_NULL */
	struct rwa_device *_mesa;           /* Mesa's screen and context, or NULL */
	BOOL _assertions;                   /* Metal's own assertion switch, stored for it */
	struct MTLTargetDeviceArch _target;     /* answered by pointer; see -targetDeviceInfo */
	char _targetName[96];               /* the bytes _target.name points at, inside the object */
}
/* Not part of any Metal protocol: how this driver's own objects reach the compiler. */
- (struct rwa_device *)mesaDevice;
/* and the argument encoder of one function's `[[buffer(n)]]` set, which the method this
 * driver installs into Metal's own _MTLFunction forwards to (the end of this file says
 * why it has to): the layout is read out of the function's own bitcode, which is the one
 * thing Metal has here and its reflection has not. */
- (id)aliothArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index;
@end

/* Declaring the protocol is the gate both assertions in MTLAddDevice read. Done at +load
 * for the bundle case (Metal loads us, so Metal is already there) and again before the
 * first use, because a library can also be brought in by hand, before Metal. */
static void
ensure_spi(Class cls)
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");

	if (spi == NULL) {
		rw_print("MTLDeviceSPI is not in this process yet; "
		         "MTLAddDevice would abort\n");
		return;
	}
	if (!class_conformsToProtocol(cls, spi) && !class_addProtocol(cls, spi)) {
		rw_print("class_addProtocol(MTLDeviceSPI) refused\n");
	}
	/* and the one method of Metal's OWN function class this driver has to answer: it is
	 * installed here for the same reason the protocol is declared here, and the whole of
	 * why it is installed at all is at the end of this file. */
	ensure_function_encoder();
}

/* One request of this port's own gpu client, shaped like the msm ioctl it stands for
 * (iokit/Kernel/HakiosGPU.cpp: kMsmGetParam = 0x40, struct msm_param). The parameters
 * worth asking for are 0x03 CHIP_ID (with the speed bin in the high half), 0x02 GMEM_SIZE
 * and 0x04 MAX_FREQ. Mesa reaches the same client the same way. */
static bool
gpu_param(io_connect_t gpu, uint32_t which, uint64_t *value)
{
	struct {
		uint32_t pipe, param;
		uint64_t value;
		uint32_t len, pad;
	} request = { .pipe = 0, .param = which, .value = 0, .len = 0, .pad = 0 };
	size_t size = sizeof(request);

	if (gpu == IO_OBJECT_NULL) {
		return false;
	}
	if (IOConnectCallStructMethod(gpu, 0x40, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS) {
		return false;
	}
	*value = request.value;
	return true;
}

/* The kernel's gpu interface, by the numbers its own header gives
 * (iokit/Kernel/HakiosGPU.cpp). */
enum {
	kDrmGemClose  = 0x09,
	kMsmGetParam  = 0x40,
	kMsmGemNew    = 0x42,
	kMsmGemInfo   = 0x43,
};
#define MSM_INFO_GET_IOVA  1

struct gem_new   { uint64_t size; uint32_t flags, handle; };
struct gem_info  { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct gem_close { uint32_t handle, pad; };

#define RW_MAX_BUFFERS 32

@class AliothMetalCommandQueue;
@class AliothMetalWork;

@interface AliothMetalComputePipeline : _MTLComputePipelineState {
	AliothMetalDevice *_owner;
	struct rwa_compute *_compute;
	struct rwa_compute_info _info;
}
@end

/* One recorded dispatch. Metal's rule is that nothing runs until the command buffer is
 * committed, so an encoder records and the command buffer runs; dispatching at encode time
 * would be less code and would quietly reorder work. */
@interface AliothMetalComputePipeline (AliothMetalInternal)
- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor;
- (struct rwa_compute *)compute;
/* (the K40's) Metal's pipeline reflection of it, autoreleased; nil when the library has none */
- (id)aliothReflection;
@end

@interface AliothMetalCommandQueue : _MTLCommandQueue
- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device descriptor:(id)descriptor;
- (dispatch_queue_t)aliothWaitQueue;    /* (the K40's) where its buffers wait for the GPU, in order */
@end

@interface AliothMetalCommandBuffer : _MTLCommandBuffer
- (instancetype)initWithAliothQueue:(AliothMetalCommandQueue *)queue;
- (void)recordWork:(AliothMetalWork *)work;
- (void)recordBlit:(id)blit;
- (id)device;
@end

@interface AliothMetalComputeEncoder : _MTLCommandEncoder
- (instancetype)initWithAliothCommandBuffer:(AliothMetalCommandBuffer *)commandBuffer;
@end

/*
 * (the K40's) The blit encoder. It answered nil, and SpriteKit, SceneKit and most engines upload their textures with
 * one (a staging buffer copied into the texture) and make their mipmaps with one. Each copy, fill and mipmap chain is
 * recorded in the command buffer's order and done by the processor at -commit, after the GPU has finished whatever
 * came before it in that buffer (mesa.finish): every buffer of ours is memory the processor maps, and every texture is
 * read and written through the compiler library's texture_read / texture_write, which know its layout. Slower than the
 * GPU's own copy engine, and right; the GPU path can come later behind the same records.
 */
enum {
	ALIOTH_BLIT_FILL, ALIOTH_BLIT_BUFFER_TO_BUFFER, ALIOTH_BLIT_BUFFER_TO_TEXTURE, ALIOTH_BLIT_TEXTURE_TO_BUFFER,
	ALIOTH_BLIT_TEXTURE_TO_TEXTURE, ALIOTH_BLIT_MIPMAPS,
	ALIOTH_BLIT_RESOLVE,    /* a render pass's MultisampleResolve, recorded at its end; the GPU does this one */
};

@interface AliothMetalBlit : NSObject {
@public
	int kind;
	id source, destination;         /* retained */
	NSUInteger sourceOffset, destinationOffset, length;
	uint8_t value;
	NSUInteger sourceBytesPerRow, sourceBytesPerImage, destinationBytesPerRow, destinationBytesPerImage;
	NSUInteger sourceSlice, sourceLevel, destinationSlice, destinationLevel;
	MTLOrigin sourceOrigin, destinationOrigin;
	MTLSize size;
}
@end

@interface AliothMetalBlitEncoder : _MTLCommandEncoder
- (instancetype)initWithAliothCommandBuffer:(AliothMetalCommandBuffer *)commandBuffer;
@end

/*
 * (the K40's) Fences and events. The GPU has one ring and a command buffer's work runs in order at -commit, so a fence
 * orders nothing that is not in order already -- but the OBJECT has to exist: -newFence answered nil, and an engine
 * that makes its fences up front asserts on that. An event (MTLEvent, and MTLSharedEvent: the same object here) has a
 * value: -encodeSignalEvent:value: is recorded in the command buffer's order, and at -commit, once the GPU has finished
 * what came before it, the value is set and the listeners waiting for it are told. A wait is recorded too and checked
 * there; it cannot hold the work back for a value the processor sets later (the work runs on the committing thread),
 * and says so once.
 */
@interface AliothMetalFence : NSObject {
@public
	id device;                      /* not retained: the device outlives what it makes */
	NSString *label;
}
@end

@interface AliothMetalEvent : NSObject {
@public
	id device;
	NSString *label;
	uint64_t value;                 /* under @synchronized (self) */
	NSMutableArray *waiting;        /* @[ listener, value, block ] each */
}
- (void)aliothSignal:(uint64_t)value;
@end

@interface AliothMetalSignal : NSObject {
@public
	AliothMetalEvent *event;        /* retained */
	uint64_t value;
	BOOL wait;                      /* -encodeWaitForEvent:value:, not a signal */
}
@end

/*
 * (the K40's) MTLHeap. Metal's heap is one allocation its resources are cut out of; this one gives each resource an
 * allocation of its own through the device, which is the same thing to everything but the bookkeeping -- and an engine
 * that sizes its heaps (-heapBufferSizeAndAlignWithLength:options:) and allocates from them gets an object instead of
 * nil. The size is accounted, not enforced: a heap here never refuses for being full, and -usedSize only grows.
 */
@interface AliothMetalHeap : NSObject {
@public
	id device;                      /* not retained: the device outlives what it makes */
	NSString *label;
	NSUInteger size, used, storageMode, cpuCacheMode, hazardTrackingMode, type;
}
@end

@interface AliothMetalRenderEncoder : _MTLCommandEncoder
- (instancetype)initWithAliothCommandBuffer:(AliothMetalCommandBuffer *)commandBuffer
				  descriptor:(id)descriptor;
/* Every form of every draw method records through this one, indexed or not: the thirteen
 * lines that copy the encoder's state into the work item are the same either way, and two
 * copies of them are how one form ends up carrying state the other does not. */
- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance;
@end

@interface AliothMetalRenderPipeline : _MTLRenderPipelineState {
	AliothMetalDevice *_owner;
	struct rwa_render *_render;
	struct rwa_render_info _info;
}
- (struct rwa_render *)render;
/* (the K40's) Metal's pipeline reflection of it, autoreleased; nil when the library has none */
- (id)aliothReflection;
/* Not part of any Metal protocol: whether this pipeline's fragment shader stores to
 * [[depth]], which only whoever read the shader knows. A pass without a depth attachment
 * drops those writes -- Metal's own behaviour -- and a draw that says so is the difference
 * between a measurement and a colour attachment holding its clear value. */
- (BOOL)writesDepth;
/* Which buffer indices this pipeline's vertex stage FETCHES ATTRIBUTES from, one bit
 * each, and how many slots that covers. Metal has ONE table behind
 * -setVertexBuffer:offset:atIndex: and both the vertex descriptor's layouts and the
 * stage's [[buffer(n)]] arguments read out of it, so the encoder cannot know which an
 * index is until it knows the pipeline -- and this is what the draw splits the table
 * with. Zero for a stage that fetches nothing, which is every pipeline this driver drew
 * before the layout existed. */
- (unsigned)vertexStreamMask;
- (unsigned)vertexStreamCount;
/* And the element array the compiler library built out of the descriptor and the module's
 * own interface: how many, and which Metal [[attribute(n)]] each one is. The order is the
 * shader's driver_location order and not Metal's attribute order (docs/metal.md 10.44), so
 * these two are the only way a caller can read that mapping back instead of trusting it --
 * which is what metal_drv_test --draw-attrib prints beside its pixels. Not part of any
 * Metal protocol; Metal's own pipeline reflection would report the same thing. */
- (unsigned)vertexElementCount;
- (unsigned)vertexElementAttribute:(unsigned)index;
@end

@interface AliothMetalWork : NSObject {
@public
	AliothMetalComputePipeline *pipeline;
	AliothMetalRenderPipeline *render;      /* set instead for a draw */
	BOOL clearOnly;                         /* (the K40's) neither: a pass that clears and draws nothing */
	id color[8];
	unsigned colorCount;
	float clearColor[4];
	BOOL clears;
	/* the depth/stencil attachment of the pass, its two load actions and the state
	 * bound when the draw was encoded. Separate flags per aspect because Metal keeps
	 * two attachment descriptors over one texture and a pass that clears the depth and
	 * keeps the stencil is ordinary. */
	id depth;
	id depthStencilState;
	float clearDepth;
	unsigned clearStencil;
	BOOL clearsDepth, clearsStencil;
	unsigned stencilReference[2];
	float depthRange[2];
	BOOL hasDepthRange;
	float viewport[4];                      /* (the K40's) MTLViewport's rectangle, when hasViewport */
	BOOL hasViewport;
	unsigned threadgroupBytes;              /* (the K40's) a dispatch's -setThreadgroupMemoryLength: for index 0 */
	unsigned cullMode, frontCCW, fillLines; /* (the K40's) the encoder's rasterizer state (rwa_draw says how) */
	unsigned scissor[4];                    /* ... and its MTLScissorRect, when hasScissor */
	BOOL hasScissor;
	float blendColor[4];                    /* (the K40's) -setBlendColorRed:..., zero by default */
	float depthBias[3];                     /* (the K40's) -setDepthBias:slopeScale:clamp:, zeros: none */
	id indirectBuffer;                      /* (the K40's) an indirect draw's arguments, read at -commit */
	NSUInteger indirectOffset;
	unsigned primitive, vertexStart, vertexCount;
	/* the index buffer of an indexed draw and the three numbers that come with it in
	 * the same Metal call; indexBuffer nil is -drawPrimitives:, and then vertexCount
	 * counts vertices rather than indices */
	id indexBuffer;
	unsigned indexType;
	size_t indexBufferOffset;
	int baseVertex;
	unsigned instanceCount, baseInstance;
	id vertexBuffers[RW_MAX_BUFFERS];
	size_t vertexBufferOffsets[RW_MAX_BUFFERS];
	unsigned vertexBufferCount;
	id buffers[RW_MAX_BUFFERS];
	/* Metal's own `offset:` for each of them, which until argument buffers existed was
	 * refused at the encoder as unimplemented. It is RECORDED here and judged at the
	 * draw instead, because which indices may carry one is the pipeline's answer: a
	 * member of an argument buffer has its own offset and a buffer argument still has
	 * none. */
	size_t bufferOffsets[RW_MAX_BUFFERS];
	id textures[RW_MAX_BUFFERS];
	id samplers[RW_MAX_BUFFERS];
	unsigned count, textureCount, samplerCount;
	unsigned groups[3], size[3];
}
@end

@interface AliothMetalTexture : _MTLResource {
	AliothMetalDevice *_owner;
	struct rwa_texture *_texture;
	struct rwa_texture_shape _shape;
	int _responsibleProcess;   /* as on the buffer: accounting, not behaviour */
	id _parent;                /* (the K40's) a view's texture, retained: -parentTexture */
}
@property (readonly) struct rwa_texture *mesaTexture;
/* Not part of MTLTexture: the depth or stencil aspect of this texture, one float and one
 * byte per texel. Metal's own -getBytes: has no aspect to name and the compiler library
 * refuses a depth texture through it by name, so a caller that wants to know what a depth
 * buffer holds asks for an aspect here instead. Either pointer may be NULL. */
- (BOOL)aliothReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region;
@end

@interface AliothMetalSampler : _MTLSamplerState {
	struct rwa_sampler *_sampler;
}
@property (readonly) struct rwa_sampler *mesaSampler;
@end

@interface AliothMetalDepthStencilState : _MTLDepthStencilState {
	struct rwa_depth_stencil *_state;
	BOOL _readsDepth, _writesDepth, _readsStencil, _writesStencil;
}
@property (readonly) struct rwa_depth_stencil *mesaDepthStencil;
- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor;
@end

/*
 * MTLArgumentEncoder. Not a writer: the records live in the compiler library with the
 * destination buffer, because the frontend has already resolved every member of every set
 * to a binding and Metal's 8-byte handles are an Apple GPU's descriptors (air_metal.h).
 * The implementation below carries the whole of why.
 */
@interface AliothMetalArgumentEncoder : NSObject
- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule;
/* Not part of MTLArgumentEncoder: the mapping between Metal's [[id(n)]] and the index the
 * GPU is programmed with, so a test can read it back instead of trusting it. */
- (NSUInteger)aliothMemberCount;
- (NSUInteger)aliothMetalIndex;
- (BOOL)aliothMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index;
@end

@interface AliothMetalBuffer : _MTLResource {
	AliothMetalDevice *_owner;
	io_connect_t _gpu;        /* not owned: the device holds it */
	uint32_t _handle;         /* set on the kernel path only */
	uint64_t _iova;
	void *_contents;
	NSUInteger _length;
	struct rwa_buffer *_mesa; /* set instead when the memory came from Mesa */
	int _responsibleProcess;  /* what <MTLResourceSPI> calls the owner, for accounting */
}
@property (readonly) struct rwa_buffer *mesaBuffer;
- (instancetype)initWithDevice:(AliothMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length;
@end

@implementation AliothMetalDevice

+ (void)load
{
	ensure_spi(self);
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	ensure_spi([self class]);

	self = [super init];
	if (self == nil) {
		return nil;
	}
	_acceleratorService = port;
	if (port != IO_OBJECT_NULL) {
		IOObjectRetain(port);
	}

	/* The gpu this driver would actually drive. Not fatal if it is absent: the port
	 * starts the Adreno only with the boot argument gpu=1, and a device object that
	 * cannot allocate is still the thing under test until the resource layer exists. */
	io_service_t gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));

	_gpu = IO_OBJECT_NULL;
	if (gpu == IO_OBJECT_NULL) {
		rw_print("no HakiosGPU service (the gpu was not started)\n");
	} else {
		kern_return_t kr = IOServiceOpen(gpu, mach_task_self(), 0, &_gpu);

		rw_print("HakiosGPU client %s (%#x)\n",
		         kr == KERN_SUCCESS ? "opened" : "refused", kr);
		IOObjectRelease(gpu);
	}

	/* And the compiler, if it is there. Its absence is not fatal: the device still has to
	 * exist and answer, it just cannot make a pipeline (and says so when asked). */
	if (mesa_available()) {
		char why[256] = { 0 };

		_mesa = mesa.device_open(why, sizeof(why));
		rw_print("the compiler library %s\n",
		         _mesa != NULL ? mesa.device_name(_mesa) : why);
		/* (the K40's) dispatches not waited for one by one: the command buffer's flush and fence say when its
		 * work is done (-aliothDeliverHandlers, -waitUntilCompleted), as they do for its draws */
		void (*deferred)(struct rwa_device *, unsigned) =
		    (void (*)(struct rwa_device *, unsigned))dlsym(mesa.handle, "rwa_device_set_deferred");

		if (_mesa != NULL && deferred != NULL) {
			deferred(_mesa, 1);
		}
	}
	alioth_stats_setup();
	alioth_listener_fix();

	/* The struct Metal reads out of this device by raw pointer, filled here rather than on
	 * demand: the pointer is handed out without a lock, from whichever thread a renderer
	 * happens to be on, and here is the one moment when nobody can have asked yet. The
	 * name is Apple's own rule for a device that declines -- its -name with the spaces
	 * taken out (0x181a3ca0c) -- but the bytes are COPIED, which Apple's branch does not
	 * need to do: it keeps the autoreleased -UTF8String in a struct on its own stack and is
	 * done with it nine instructions later, while this one is answered to callers for as
	 * long as the device lives. See -targetDeviceInfo. */
	NSString *plain = [[self name] stringByReplacingOccurrencesOfString:@" "
								 withString:@""];
	const char *bytes = [plain UTF8String];

	snprintf(_targetName, sizeof(_targetName), "%s", bytes != NULL ? bytes : "");
	_target.target = 0;
	_target.version = 0;
	_target.name = _targetName;
	rw_print("targetDeviceInfo answers {0, 0, \"%s\"} -- _MTLDevice's own entry "
	         "answers NULL and Metal reads it without looking, so the precompiled pipelines "
	         "this device will be asked for are pipelines.%s\n", _target.name, _target.name);
	return self;
}

- (struct rwa_device *)mesaDevice
{
	return _mesa;
}

- (void)dealloc
{
	if (_mesa != NULL) {
		mesa.device_close(_mesa);
	}
	if (_gpu != IO_OBJECT_NULL) {
		IOServiceClose(_gpu);
	}
	if (_acceleratorService != IO_OBJECT_NULL) {
		IOObjectRelease(_acceleratorService);
	}
	[super dealloc];
}

/* The profile is what -[_MTLDevice initLimits] switches on, and it was swept on the device:
 * 0, 1 and 4 to 9 fill the limits table, while 2, 3 and 10 to 12 abort inside Apple on
 * `_limits.maxColorAttachments > 0`. 0 is taken because it is the lowest that works and
 * because its table (31 buffers, 16 samplers, 512 threads per threadgroup, 16 KB of
 * threadgroup memory, 8192 textures) is a plausible floor for this Adreno.
 *
 * "Which profile the hardware deserves is a separate question, to be answered when something
 * is drawn" -- and something has now been drawn, so here is the answer so far. This number is
 * the ONLY input Apple has for what this device supports: -[_MTLDevice initGPUFamilySupport]
 * switches on it and pushes a list of MTLGPUFamily into the vector that -supportsFamily:
 * searches. Profile 0 pushes Apple1 and Common1 and nothing else.
 *
 * That is why SpringBoard cannot start. Siri's orb prewarms an MPSImageGaussianBlur, and
 * MPSDevice::GetMPSDevice is a descending ladder of -supportsFamily: from Apple8 (1008) down
 * to Apple2 (1002) which returns NULL when none answers. It never asks about Apple1 -- the
 * constant 0x3e9 is nowhere in that function -- so the one family we claim is the one family
 * it does not want, and the process aborts in MPSKernel with "Failed to get MPSDevice"
 * (docs/metal.md 10.47). Profile 1 is the lowest that pushes Apple2.
 *
 * So the profile is a boot-arg (metal_profile=N), not because it should stay configurable but
 * because the cost of each step up has to be measured rather than argued: every family claimed
 * is a promise, and some of those promises this driver cannot keep. Answering LESS is not the
 * safe direction either -- an empty family vector makes
 * +[MTLFXSpatialScalerDescriptor supportsDevice:] take this for a discrete GPU and say yes
 * unconditionally -- so there is no answer here that is safe by default, only one that is
 * measured. Apple's own A11 driver answers 5.
 */
- (NSUInteger)featureProfile
{
	static NSUInteger profile = (NSUInteger)-1;

	if (profile == (NSUInteger)-1) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		profile = 0;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "metal_profile=")) != NULL && (at == args || at[-1] == ' ')) {
			profile = (NSUInteger)strtoul(at + strlen("metal_profile="), NULL, 10);
		}
		rw_print("feature profile %lu, which is what Metal reads this device's "
		         "whole capability surface out of (metal_profile= to change it; 0 claims Apple1 "
		         "alone, 1 adds Apple2, which is the lowest rung MPS asks about)\n",
		         (unsigned long)profile);
	}
	return profile;
}

/* A placeholder, and named as one: nothing has yet measured what the kernel's gemNew will
 * give in one allocation. It only has to be non-zero for initLimits to accept it. */
- (uint64_t)maxBufferLength
{
	return 256ULL << 20;
}

/*
 * The render pipeline, by the same rule as the compute one: both of _MTLDevice's entry points
 * go through its nil -compiler, so a driver that leaves them alone gets nil and no reason.
 *
 * One asymmetry is deliberate and is the measured road rather than a shortcut. When the
 * descriptor names no vertex function, the compiler library supplies a pass-through vertex
 * stage over the whole attachment -- which is exactly how air_run has drawn every fragment
 * shader of the corpus, 255 of them compared pixel by pixel (docs/air.md). A descriptor that
 * does name one passes both modules and nothing is generated.
 */
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
				     error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (descriptor == nil) {
		return [self fail:error with:@"no render pipeline descriptor"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libhakios.air.dylib is "
					     @"missing or older than this driver"];
	}
	if (mesa.render_create == NULL) {
		/* The order of these two matters: the first version asked about render_create
		 * first and would have told a caller that a library which never loaded "can
		 * compute but not yet draw", which is a sentence about a library that is not
		 * there. */
		return [self fail:error with:@"this compiler library can compute but not yet "
					     @"draw"];
	}

	id vertex = [descriptor vertexFunction];
	id fragment = [descriptor fragmentFunction];

	if (fragment == nil) {
		return [self fail:error with:@"the descriptor has no fragment function, and this "
					     @"driver has nothing to draw without one"];
	}
	if (![fragment respondsToSelector:bitcode] ||
	    (vertex != nil && ![vertex respondsToSelector:bitcode])) {
		return [self fail:error with:@"a function in this descriptor does not answer "
					     @"bitcodeData, so its AIR cannot be read"];
	}
	NSData *fragment_air = ((NSData *(*)(id, SEL))objc_msgSend)(fragment, bitcode);
	NSData *vertex_air = vertex != nil ?
	    ((NSData *(*)(id, SEL))objc_msgSend)(vertex, bitcode) : nil;

	if (fragment_air == nil || [fragment_air length] == 0) {
		return [self fail:error with:@"the fragment function has no bitcode"];
	}

	struct rwa_render_shape shape = { .sample_count = (unsigned)[descriptor sampleCount] };

	/*
	 * THE COLOUR ATTACHMENTS, each one's format AND ITS BLEND STATE.
	 *
	 * The blend half is what the home screen was missing: with no blend state in the
	 * contract the library made every pipeline opaque, and CoreAnimation composites with
	 * PREMULTIPLIED ALPHA -- so a label layer whose background is transparent black was
	 * drawn as opaque black, which is the black box behind every icon name in the
	 * photographs.
	 *
	 * All eight are read, as the formats are, and for the same reason: Apple's array
	 * answers -objectAtIndexedSubscript: for every index in range and an attachment an
	 * application never touched holds Apple's own defaults -- blendingEnabled NO,
	 * writeMask All, the factors One and Zero, both operations Add (MTLRenderPipeline.h).
	 * Nothing is interpreted here. Every number goes across as Metal's own and the
	 * library maps the three enumerations by table, because a cast between them is wrong:
	 * MTLBlendFactor and pipe_blendfactor agree on two of nineteen values and
	 * MTLColorWriteMask's bits run in the opposite order to Gallium's colormask.
	 *
	 * `color_blend_count` is set to eight -- all eight entries filled in -- and NOT to
	 * the attachment count: a zero there means "this shape carries no blend state at
	 * all", which is the old opaque behaviour, and that is a thing this driver must never
	 * say by accident.
	 */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		unsigned format = attachment != nil ? (unsigned)[attachment pixelFormat] : 0;

		shape.color_format[i] = format;
		if (format != 0) {
			shape.color_count = i + 1;
		}
		if (attachment == nil) {
			/* an array that answered nil has no defaults to pass on, so this
			 * entry says what Metal's own untouched descriptor says: no
			 * blending, every channel written */
			shape.color_blend[i].write_mask = 0xf;   /* MTLColorWriteMaskAll */
			continue;
		}
		shape.color_blend[i].blending_enabled =
		    [attachment isBlendingEnabled] ? 1 : 0;
		shape.color_blend[i].rgb_blend_operation =
		    (unsigned)[attachment rgbBlendOperation];
		shape.color_blend[i].alpha_blend_operation =
		    (unsigned)[attachment alphaBlendOperation];
		shape.color_blend[i].source_rgb_blend_factor =
		    (unsigned)[attachment sourceRGBBlendFactor];
		shape.color_blend[i].destination_rgb_blend_factor =
		    (unsigned)[attachment destinationRGBBlendFactor];
		shape.color_blend[i].source_alpha_blend_factor =
		    (unsigned)[attachment sourceAlphaBlendFactor];
		shape.color_blend[i].destination_alpha_blend_factor =
		    (unsigned)[attachment destinationAlphaBlendFactor];
		shape.color_blend[i].write_mask = (unsigned)[attachment writeMask];
	}
	shape.color_blend_count = 8;
	/*
	 * And the measurement this was built to make: what the window server actually asks
	 * for, one line per attachment it has, printed BEFORE the library can refuse any of
	 * it. These are Metal's own numbers, so `blend 1 rgb 1/5 op 0 alpha 1/5 op 0` is
	 * premultiplied source-over (One, OneMinusSourceAlpha, Add) and `blend 1 rgb 4/5` is
	 * straight alpha instead -- the one distinction a boot log has to be able to make.
	 *
	 * THE LABEL IS HERE BECAUSE IT IS THE ANSWER. CoreAnimation builds a pipeline's
	 * label out of the specification it compiled: `encode_segment` emits a `B<mnemonic>`
	 * segment IF AND ONLY IF the fixed-function blend mode is not its "none" value
	 * (0x36), so `com.apple.coreanimation.draw.PBGRABsovXm_...` is a pipeline whose
	 * blending this descriptor carries and `com.apple.coreanimation.draw.PBGRAXm_...` is
	 * one that blends INSIDE ITS FRAGMENT SHADER and leaves this descriptor at Apple's
	 * defaults. One grep over a boot log then says which of the two the home screen is
	 * made of, which no amount of reading the enumerations can.
	 */
	bool blend_is_new = false;

	for (unsigned i = 0; i < shape.color_count; i++) {
		blend_is_new |= rw_blend_line(i, shape.color_format[i],
		                              &shape.color_blend[i]);
	}
	/* and the label, on a pipeline whose blend configuration has not been seen before:
	 * one per distinct configuration is the whole of what the label has to say, and the
	 * compositor has over a hundred of them per boot */
	if (blend_is_new) {
		NSString *label = [descriptor label];

		rw_print("blend: ... and the first such pipeline's label is %s\n",
		         label != nil ? [label UTF8String] : "(none)");
	}
	/* Both aspect formats, as Metal's descriptor has them, and the library requires
	 * them to name one texture: an application with a combined format sets the same
	 * MTLPixelFormat on both, because there is one attachment behind the two
	 * properties. Passing them through separately rather than folding them here is
	 * what lets the library refuse a descriptor that names two different formats by
	 * name instead of this driver picking one of them. */
	shape.depth_format = (unsigned)[descriptor depthAttachmentPixelFormat];
	shape.stencil_format = (unsigned)[descriptor stencilAttachmentPixelFormat];

	/*
	 * THE VERTEX DESCRIPTOR, which is what the window server asked this driver for and
	 * did not get: one pipeline specification was refused 80 times with "reads vertex
	 * attributes ([[stage_in]]), and this contract carries no vertex attribute layout"
	 * (docs/metal.md 10.44, 10.45), because the layout lives here and nowhere in the AIR.
	 *
	 * Both of Metal's arrays are read whole, index by index, and passed through as
	 * Metal's own numbers -- the library decides what it can honour and refuses the
	 * rest BY NAME, which is the half this driver must not do for it: a format a6xx
	 * cannot fetch and an offset past twelve bits are both things freedreno would take
	 * silently (air_metal.h has the register fields).
	 *
	 * There is no count to read on either side. Apple's arrays answer
	 * -objectAtIndexedSubscript: for every index in range and a descriptor an
	 * application never touched holds MTLVertexFormatInvalid in every attribute, which
	 * is what the library reads as "not configured" -- so the loop is the whole array
	 * and the formats decide. The range is Apple's 31 of each, from the feature tables,
	 * and asking past it would raise.
	 */
	MTLVertexDescriptor *vd = [descriptor vertexDescriptor];

	if (vd != nil) {
		unsigned configured = 0, highest = 0;

		for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
			MTLVertexAttributeDescriptor *a =
			    [[vd attributes] objectAtIndexedSubscript:i];
			unsigned format = a != nil ? (unsigned)[a format] : 0;

			if (format == 0) {
				continue;
			}
			shape.vertex_attribute[i].format = format;
			shape.vertex_attribute[i].offset = (unsigned)[a offset];
			shape.vertex_attribute[i].buffer_index = (unsigned)[a bufferIndex];
			configured++;
			if ((unsigned)[a bufferIndex] > highest) {
				highest = (unsigned)[a bufferIndex];
			}
		}
		for (unsigned k = 0; k < RWA_MAX_VERTEX_BUFFERS; k++) {
			MTLVertexBufferLayoutDescriptor *l =
			    [[vd layouts] objectAtIndexedSubscript:k];

			if (l == nil) {
				continue;
			}
			shape.vertex_layout[k].stride = (unsigned)[l stride];
			shape.vertex_layout[k].step_function = (unsigned)[l stepFunction];
			shape.vertex_layout[k].step_rate = (unsigned)[l stepRate];
		}
		if (configured > 0) {
			rw_print("this pipeline's vertex descriptor configures %u "
			         "attribute%s, the highest from buffer index %u\n", configured,
			         configured == 1 ? "" : "s", highest);
		}
	}

	char why[512] = { 0 };
	struct rwa_render_info info = { 0 };
	struct rwa_render *render = mesa.render_create(_mesa,
	    vertex_air != nil ? [vertex_air bytes] : NULL,
	    vertex_air != nil ? [vertex_air length] : 0,
	    vertex != nil ? [[vertex name] UTF8String] : NULL,
	    [fragment_air bytes], [fragment_air length], [[fragment name] UTF8String],
	    &shape, &info, why, sizeof(why));

	if (render == NULL) {
		/* PRINTED as well as returned, and RW_AGAIN because the same specification
		 * is asked for over and over: -fail: puts the sentence in an NSError and
		 * nothing more, and whether anyone ever sees it is then the caller's
		 * business -- CoreAnimation discards this error and retries. A refusal that
		 * reaches no log is a refusal nobody can grep for, which makes the whole
		 * "refused by name" discipline unobservable from a boot: one pipeline
		 * specification was declined 80 times over 42 lives of backboardd before the
		 * vertex layout existed, and that number had to be counted by hand. */
		RW_AGAIN("this render pipeline is refused: %s\n", why);
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	/* and what the library made of it, in GALLIUM's bits: for any mask but All, None,
	 * green|blue and red|alpha these nibbles must DIFFER from the writeMask printed
	 * above, because the two enumerations number their channels in opposite directions.
	 * A log where 0xe went in and 0xe came back is a log saying nothing reversed them. */
	if (shape.color_count > 0) {
		rw_blend_result(info.blend_enabled_mask, info.color_write_masks);
	}
	return [[AliothMetalRenderPipeline alloc] initWithAliothDevice:self
								 render:render
								   info:&info
							     descriptor:descriptor];
}

/* (the K40's) with Metal's reflection of what was built, which SpriteKit and SceneKit bind by:
 * AliothMetalArgument says how */
- (id)newRenderPipelineStateWithDescriptor:(id)descriptor
				   options:(NSUInteger)options
				reflection:(id *)reflection
				     error:(NSError **)error
{
	(void)options;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor error:error];

	if (reflection != NULL) {
		*reflection = state != nil ? [state aliothReflection] : nil;
	}
	return state;
}

/*
 * The asynchronous forms, and they are not a convenience: without them Metal's own versions
 * send the pipeline request to the nil compiler, return, and NEVER CALL THE HANDLER -- so an
 * application that builds a pipeline asynchronously waits for ever instead of being told no.
 * Reversing the four entry points is what turned that up (0x181aabc48 and 0x181aabccc both
 * end in a stack block handed to the compiler), and a hang is the one failure mode that
 * cannot be read out of a log.
 *
 * Building synchronously and calling the handler at once is allowed: Metal does not promise
 * the handler runs later, only that it runs.
 */
- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
			   completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
				     options:(NSUInteger)options
			   completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor
						     options:options
						  reflection:&reflection
						       error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

/*
 * The mesh forms are the opposite hazard: they are NOT routed through the compiler, they ask
 * the mesh descriptor for a plain render pipeline descriptor and re-enter one of the four
 * forms above on self (0x181aabf60 and its three neighbours). Inherited, they would hand a
 * mesh pipeline to a vertex-and-fragment builder and produce something built wrong instead of
 * a refusal. Mesh shaders are an A14-and-later feature and nothing here implements them.
 */
- (id)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
				       options:(NSUInteger)options
				    reflection:(id *)reflection
					 error:(NSError **)error
{
	(void)descriptor; (void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self fail:error with:@"mesh pipelines are not implemented, and the inherited "
				     @"path would have built this one as a vertex pipeline"];
}

- (void)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
					 options:(NSUInteger)options
			       completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;

	(void)[self newRenderPipelineStateWithMeshDescriptor:descriptor options:options
						 reflection:NULL error:&error];
	if (handler != NULL) {
		handler(nil, nil, error);
	}
}

/* A texture is Metal's descriptor handed through to the compiler library in Metal's own
 * terms; a pixel format it cannot translate is refused by name, because a format that is
 * almost right computes a wrong picture instead of failing. */
/*
 * THE SELECTORS THE PROTOCOLS DO NOT LIST, and this is the second time one of them has killed a
 * process. owes.py counts what the protocols require and found nothing missing; these are
 * declared on Apple's counterpart CLASSES -- AGXA11FamilyDevice and IOGPUMetalDevice -- which our
 * device does not descend from, so nothing in the chain answers them and no protocol says so.
 * tools/metal/owesclass.py counts that other list: 343 such selectors, 72 of which something in
 * this image actually sends, and NONE of the 72 behind a -respondsToSelector:. QuartzCore does not
 * ask, it sends. So every one of them is terminate(), not a composite that merely looks wrong --
 * which is what setProtectionOptions: did to the window server and what the first of these just
 * did to mediaserverd.
 *
 * A texture LAYOUT answers bytes-per-row, image stride, size and alignment for a descriptor
 * without allocating anything, so CoreAnimation can size an IOSurface before it has a texture.
 * nil is the honest answer rather than a measured one: CA::Render::new_mtl_texture_layout hands
 * our result straight back to its own caller, and that caller is in no other image of this cache,
 * so whether a nil is tolerated is unknown. If the log says it is not, the next step is a small
 * object over the arithmetic -newTextureWithDescriptor: below already does.
 */
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor
			isHeapOrBufferBacked:(BOOL)backed
{
	(void)descriptor;
	(void)backed;
	RW_ONCE("aliothmetal: a texture layout was asked for and this driver has none to give; "
		"CoreAnimation sizes a surface with it before any texture exists, and nil is the "
		"answer until a boot says nil is not tolerated\n");
	return nil;
}

/*
 * A resource group names resources whose cache lines the GPU may drop at the end of a pass -- an
 * AGX cache-management optimisation this port has no control over. nil here is not a guess: the
 * caller tests it (cbz x0 in CA::OGL::MetalContext::mark_cache_drop_hints) and composites without
 * it, and a nil also leaves the counter at zero, which makes -encodeCacheHintTag:... and
 * -encodeCacheHintFinalize:... unreachable. One nil answers three selectors, which is the right
 * shape: refuse where the decision is made rather than at each leaf.
 */
- (id)newResourceGroupFromResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources;
	(void)count;
	return nil;
}

/*
 * Overriding this keeps _MTLDevice's own body from running on our device, and that body is what
 * sends -copyIOSurfaceSharedTextureProperties: and -sparseSurfaceDefaultValue, neither of which
 * we answer. So one override closes three. It agrees with -newSharedTextureHandle below, which
 * answers nil for the same reason: nothing here is shared between processes.
 */
- (id)newSharedTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	(void)descriptor;
	return nil;
}

/*
 * A texture over memory the CALLER allocated, which CoreAnimation's update_texture stores and
 * uses (docs/metal.md 10.48, selector 5 of the class-declared list). It is answered by name
 * rather than left unrecognized, because an unrecognized selector is terminate() and a named nil
 * is a line in the log -- mediaserverd died 43 times on the one above this before it was
 * answered at all.
 *
 * nil and not a texture, because the contract has no call for it. rwa_texture_from_surface takes
 * an IOSurface, which the kernel hands over as a buffer (kHakiosSurface); a bare pointer into
 * the caller's address space is a different import, and a texture made by copying instead would
 * be wrong in a way nothing would report: the caller writes to its own pages and expects the
 * texture to change.
 */
- (id)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length
		     descriptor:(MTLTextureDescriptor *)descriptor
		    deallocator:(void (^)(void *, NSUInteger))deallocator
{
	(void)bytes;
	(void)length;
	(void)descriptor;
	(void)deallocator;
	RW_ONCE("aliothmetal: a texture over the caller's own memory was asked for, and the "
		"contract has no import for a bare pointer -- only for an IOSurface. Copying "
		"instead would be worse than nil: the caller writes to its pages and expects the "
		"texture to follow\n");
	return nil;
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a texture needs the compiler library\n");
		return nil;
	}
	struct rwa_texture_shape shape = {
		.pixel_format = (unsigned)[descriptor pixelFormat],
		.type = (unsigned)[descriptor textureType],
		.width = (unsigned)[descriptor width],
		.height = (unsigned)[descriptor height],
		.depth = (unsigned)[descriptor depth],
		.array_length = (unsigned)[descriptor arrayLength],
		.mip_levels = (unsigned)[descriptor mipmapLevelCount],
		.sample_count = (unsigned)[descriptor sampleCount],
		.usage = (unsigned)[descriptor usage],
	};
	struct rwa_texture *texture = mesa.texture_create(_mesa, &shape, why, sizeof(why));

	if (texture == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[AliothMetalTexture alloc] initWithAliothDevice:self
							 texture:texture
							   shape:&shape];
}

/* The sampler's own base class exists and owes its protocol nothing, so this is the cheapest
 * object in the driver: the state goes to the compiler library, which maps it the same way it
 * maps a shader's own `constexpr sampler` word -- one vocabulary, so the two can be checked
 * against each other (docs/air.md 3.3). */
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a sampler needs the compiler library\n");
		return nil;
	}
	struct rwa_sampler_state state = {
		.address = { (unsigned)[descriptor sAddressMode],
			     (unsigned)[descriptor tAddressMode],
			     (unsigned)[descriptor rAddressMode] },
		.min_filter = (unsigned)[descriptor minFilter],
		.mag_filter = (unsigned)[descriptor magFilter],
		.mip_filter = (unsigned)[descriptor mipFilter],
		.compare_function = (unsigned)[descriptor compareFunction],
		.max_anisotropy = (unsigned)[descriptor maxAnisotropy],
		.normalized_coordinates = [descriptor normalizedCoordinates] ? 1 : 0,
		.lod_min = [descriptor lodMinClamp],
		.lod_max = [descriptor lodMaxClamp],
	};
	struct rwa_sampler *sampler = mesa.sampler_create(_mesa, &state, why, sizeof(why));

	if (sampler == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[AliothMetalSampler alloc] initWithAliothDevice:self
							 sampler:sampler
						      descriptor:descriptor];
}

/*
 * The depth/stencil state, which _MTLDevice does not implement at all: unlike -compiler,
 * whose body is `mov x0, #0; ret`, this selector is simply absent from the base class's
 * method list, so the whole of it is a driver's. (Apple's own is AGXA11FamilyDepthStencilState
 * over the same _MTLDepthStencilState this one inherits.)
 *
 * MTLDepthStencilDescriptor is read here, in Metal's words, and translated by the compiler
 * library -- which is where the two enumerations that do NOT line up are turned over:
 * MTLStencilOperationInvert is 5 and Gallium's INVERT is 7, so a cast would turn "invert the
 * stencil" into "increment it and wrap", and that is a different picture rather than a
 * failure. This side only reads properties and names them.
 */
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (descriptor == nil) {
		rw_print("a depth/stencil state needs a descriptor\n");
		return nil;
	}
	if (_mesa == NULL) {
		rw_print("a depth/stencil state needs the compiler library\n");
		return nil;
	}
	if (mesa.depth_stencil_create == NULL) {
		rw_print("this compiler library has no depth/stencil state, so it "
		         "is older than this driver and cannot test depth\n");
		return nil;
	}

	/* frontFaceStencil and backFaceStencil are null_resettable: the setter takes nil
	 * and the getter hands back Metal's default descriptor, so both are read without a
	 * nil check -- and when one is nil anyway, the zeroed face left behind is compare
	 * Always with every operation Keep, which is that same default. */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };
	struct rwa_depth_stencil_state state = {
		.depth_compare_function = (unsigned)[descriptor depthCompareFunction],
		.depth_write_enabled = [descriptor isDepthWriteEnabled] ? 1 : 0,
	};
	struct rwa_stencil_face *out[2] = { &state.front, &state.back };

	for (unsigned i = 0; i < 2; i++) {
		if (faces[i] == nil) {
			*out[i] = (struct rwa_stencil_face){
				.compare_function = MTLCompareFunctionAlways,
				.read_mask = 0xffffffff, .write_mask = 0xffffffff,
			};
			continue;
		}
		*out[i] = (struct rwa_stencil_face){
			.compare_function = (unsigned)[faces[i] stencilCompareFunction],
			.stencil_fail_operation = (unsigned)[faces[i] stencilFailureOperation],
			.depth_fail_operation = (unsigned)[faces[i] depthFailureOperation],
			.depth_stencil_pass_operation =
			    (unsigned)[faces[i] depthStencilPassOperation],
			.read_mask = (unsigned)[faces[i] readMask],
			.write_mask = (unsigned)[faces[i] writeMask],
		};
	}

	struct rwa_depth_stencil *made = mesa.depth_stencil_create(_mesa, &state, why,
								   sizeof(why));

	if (made == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[AliothMetalDepthStencilState alloc] initWithAliothDevice:self
								     state:made
								descriptor:descriptor];
}

/* _MTLDevice's own newCommandQueue and newCommandQueueWithMaxCommandBufferCount: are real
 * and both end here, where its body is `mov x0, #0; ret` (0x181aac688) -- a refusal that
 * announces itself as nothing at all. */
/*
 * -[MTLDevice newArgumentEncoderWithArguments:], the path an application takes when it
 * describes the set itself instead of taking it from a shader.
 *
 * Everything this encoder needs is in the descriptors -- how many members, which kind
 * each is, and therefore the length and the alignment -- and that is the whole of the
 * layout, because the records are keyed by Metal's `[[id(n)]]` and the FLAT index each
 * member is bound at comes from the pipeline's own shader at the draw. So an encoder made
 * this way records exactly as one made from a module does.
 *
 * Three shapes are refused BY NAME rather than encoded into something, each because the
 * frontend refuses the matching shader when it translates it (docs/air.md 8.2.1), so a
 * record of ours could never be asked for:
 *
 *   - a member that is not a pointer, a texture or a sampler: a VALUE inside the set,
 *     which this frontend binds nothing for;
 *   - an array of descriptors (arrayLength above 1), which no module in reach declares
 *     and which a shader could index at run time;
 *   - indices with a gap in them. Metal's own encodedLength is over the highest index, so
 *     a set declaring [[id(0)]] and [[id(5)]] is 48 bytes and not 16 -- and this library
 *     answers 8 bytes per member. Saying so is better than handing back a length the
 *     application would allocate too little memory from.
 */
- (id)newArgumentEncoderWithArguments:(NSArray *)arguments
{
	struct rwa_argbuf_layout layout = { 0 };
	NSUInteger count = [arguments count];

	if (mesa.argbuf_encode_buffer == NULL || mesa.module_argbufs == NULL) {
		rw_print("this compiler library has no argument buffers in it, so "
		         "an encoder would record what no draw could read\n");
		return nil;
	}
	if (count == 0 || count > RWA_MAX_ARGBUF_MEMBERS) {
		rw_print("an argument encoder of %lu members: this driver records "
		         "1 to %u\n", (unsigned long)count, RWA_MAX_ARGBUF_MEMBERS);
		return nil;
	}
	layout.metal_index = ~0u;      /* the application binds it where it likes */
	layout.alignment = 8;
	layout.member_count = (unsigned)count;
	layout.encoded_length = 8 * (unsigned)count;

	uint64_t seen = 0;
	for (NSUInteger i = 0; i < count; i++) {
		MTLArgumentDescriptor *d = [arguments objectAtIndex:i];
		NSUInteger index = [d index];
		NSUInteger type = [d dataType];
		NSUInteger array = [d arrayLength];
		unsigned kind;

		switch (type) {
		case MTLDataTypePointer: kind = RWA_ARGBUF_BUFFER; break;
		case MTLDataTypeTexture: kind = RWA_ARGBUF_TEXTURE; break;
		case MTLDataTypeSampler: kind = RWA_ARGBUF_SAMPLER; break;
		default:
			rw_print("member %lu of this argument encoder is "
			         "MTLDataType %lu, and this port binds a buffer, a texture or a "
			         "sampler member: a value inside a set is refused when the "
			         "shader is translated\n", (unsigned long)index,
			         (unsigned long)type);
			return nil;
		}
		if (array > 1) {
			rw_print("member %lu of this argument encoder is an array "
			         "of %lu descriptors, and this port's frontend refuses a member "
			         "spanning more than one descriptor index\n",
			         (unsigned long)index, (unsigned long)array);
			return nil;
		}
		if (index >= count || (seen & (1ull << index)) != 0) {
			rw_print("this argument encoder has %lu members and one of "
			         "them is index %lu: Metal's encodedLength is over the highest "
			         "index, so this driver will not answer a length for indices "
			         "with a gap or a repeat in them\n", (unsigned long)count,
			         (unsigned long)index);
			return nil;
		}
		seen |= 1ull << index;
		layout.members[i].id = (unsigned)index;
		layout.members[i].kind = kind;
		/* The flat index is the SHADER's to allocate, and there is no shader here:
		 * the draw takes it from the pipeline's own resource list, where the member
		 * is named by (set, id) exactly as the application named it. */
		layout.members[i].index = ~0u;
	}
	return [[AliothMetalArgumentEncoder alloc] initWithAliothDevice:self layout:&layout
							      fromModule:NO];
}

/* The iOS 16 form, which takes a reflection object (MTLBufferBinding) instead of
 * descriptors. Declined by name: every reflection object in this process comes from
 * Metal's own compiler, which is nil here (docs/metal.md 10.7), so an application that
 * has one did not get it from us and this driver has nothing to read out of it. */
- (id)newArgumentEncoderWithBufferBinding:(id)binding
{
	(void)binding;
	rw_print("-newArgumentEncoderWithBufferBinding: takes a reflection "
	         "object, and reflection on this port needs Metal's own compiler, which is "
	         "nil; ask a function for an encoder (-newArgumentEncoderWithBufferIndex:) or "
	         "describe the set with MTLArgumentDescriptors\n");
	return nil;
}

/*
 * And the function path, which the method installed into Metal's own _MTLFunction
 * forwards here.
 * The layout is read out of the function's bitcode -- the one thing this port has that
 * Metal's reflection has not -- so the numbers are the module's own.
 */
- (id)aliothArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (_mesa == NULL || mesa.module_argbufs == NULL) {
		rw_print("no compiler library, so the argument buffer layout of "
		         "this function cannot be read\n");
		return nil;
	}
	if (function == nil || ![function respondsToSelector:bitcode]) {
		rw_print("this function does not answer bitcodeData, so its "
		         "argument buffers cannot be read\n");
		return nil;
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

	if (air == nil || [air length] == 0) {
		rw_print("this function has no bitcode\n");
		return nil;
	}

	char why[512] = { 0 };
	struct rwa_argbuf_layout sets[8];
	unsigned count = 0;

	if (mesa.module_argbufs(_mesa, [air bytes], [air length],
				[[function name] UTF8String], sets,
				(unsigned)(sizeof(sets) / sizeof(sets[0])), &count, why,
				sizeof(why)) != 0) {
		/* the frontend's own sentence, passed through: it names what it refused */
		rw_print("%s\n", why);
		return nil;
	}
	for (unsigned i = 0; i < count; i++) {
		if (sets[i].metal_index != (unsigned)index) {
			continue;
		}
		return [[AliothMetalArgumentEncoder alloc] initWithAliothDevice:self
									  layout:&sets[i]
								      fromModule:YES];
	}
	/* Metal's own words for this, and its own reason: there is no argument buffer at
	 * that index. Ours says which indices there ARE, because an application that asked
	 * for the wrong one has no other way to find out. */
	char which[96] = "";

	for (unsigned i = 0; i < count; i++) {
		char one[16];

		snprintf(one, sizeof(one), "%s%u", i ? ", " : " at ", sets[i].metal_index);
		strlcat(which, one, sizeof(which));
	}
	rw_print("bufferIndex %lu does not identify an argument buffer: this function "
	         "declares %u%s\n", (unsigned long)index, count, which);
	return nil;
}

- (id)newCommandQueueWithDescriptor:(id)descriptor
{
	/* Apple's own -newCommandQueue and -newCommandQueueWithMaxCommandBufferCount: build a
	 * descriptor and end here, so one usually arrives; when this is called directly with
	 * nil, one is made, because the base class validates it and refuses nil. */
	id use = descriptor != nil ? [[descriptor retain] autorelease] :
	    [[[MTLCommandQueueDescriptor alloc] init] autorelease];

	return [[AliothMetalCommandQueue alloc] initWithAliothDevice:self descriptor:use];
}

/*
 * Where subsystem (1) is finally asked to do its work.
 *
 * Both pipeline entry points of _MTLDevice route through -[_MTLDevice compiler], which is
 * `mov x0, #0; ret`, so a driver that leaves them alone gets nil and no error -- which is
 * exactly what this device did before this method existed. Overriding them, as AGX does, is
 * what keeps us out of Apple's compiler: the AIR comes from the function object and goes to
 * our own front end.
 *
 * The cost of that road, named in docs/metal.md 10.7: with -compiler still nil,
 * -newFunctionWithName:constantValues: and -[MTLFunction reflectionWithOptions:] return nil,
 * because those go through the compiler inside Metal's own classes where we cannot reach.
 */
- (id)newComputePipelineStateWithDescriptor:(id)descriptor
				    options:(NSUInteger)options
				 reflection:(id *)reflection
				      error:(NSError **)error
{
	(void)options;
	id state = [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor error:error];

	/* (the K40's) Metal's reflection of it, which was nil: AliothMetalArgument */
	if (reflection != NULL) {
		*reflection = state != nil ? [state aliothReflection] : nil;
	}
	return state;
}

- (id)newComputePipelineStateWithFunction:(id)function
				  options:(NSUInteger)options
			       reflection:(id *)reflection
				    error:(NSError **)error
{
	(void)options;
	id state = [self newComputePipelineStateWithFunction:function error:error];

	if (reflection != NULL) {
		*reflection = state != nil ? [state aliothReflection] : nil;
	}
	return state;
}

- (id)newComputePipelineStateWithDescriptor:(id)descriptor error:(NSError **)error
{
	return [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor
				  error:error];
}

/* _MTLDevice's own version of this builds an MTLComputePipelineDescriptorInternal and
 * re-sends to self, so overriding it is not strictly needed -- but a descriptor is what the
 * pipeline state's base class demands, and making one here keeps the three entry points
 * reaching the same code with the same arguments. */
/* The same hole on the compute side, and the same reason. */
- (void)newComputePipelineStateWithFunction:(id)function
			  completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newComputePipelineStateWithFunction:function error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newComputePipelineStateWithDescriptor:(id)descriptor
				      options:(NSUInteger)options
			    completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newComputePipelineStateWithDescriptor:descriptor
						      options:options
						   reflection:&reflection
							error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

/* (the K40's) and the function form of it, which Metal's own sends to the nil compiler as well */
- (void)newComputePipelineStateWithFunction:(id)function
				    options:(NSUInteger)options
			  completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newComputePipelineStateWithFunction:function options:options reflection:&reflection
						       error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

- (id)newComputePipelineStateWithFunction:(id)function error:(NSError **)error
{
	Class internal = objc_getClass("MTLComputePipelineDescriptorInternal");
	id descriptor = internal != Nil ? [[internal alloc] init] :
	    (id)[[MTLComputePipelineDescriptor alloc] init];

	if (descriptor == nil) {
		return [self fail:error with:@"no pipeline descriptor class to build one from"];
	}
	[descriptor setComputeFunction:function];
	id state = [self pipelineForFunction:function descriptor:descriptor error:error];

	[descriptor release];
	return state;
}

/* One place where a function becomes a pipeline, so that the three entry points above
 * cannot drift apart. */
- (id)pipelineForFunction:(id)function descriptor:(id)descriptor error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (function == nil) {
		return [self fail:error with:@"the descriptor has no compute function"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libhakios.air.dylib "
					     @"is missing"];
	}
	if (![function respondsToSelector:bitcode]) {
		return [self fail:error with:@"this function does not answer bitcodeData, so its "
					     @"AIR cannot be read"];
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

	if (air == nil || [air length] == 0) {
		return [self fail:error with:@"the function has no bitcode"];
	}

	char why[512] = { 0 };
	struct rwa_compute_info info = { 0 };
	struct rwa_compute *compute = mesa.compute_create(_mesa, [air bytes], [air length],
							  [[function name] UTF8String], &info,
							  why, sizeof(why));
	if (compute == NULL) {
		/* The front end's own sentence, passed through rather than replaced: it says
		 * which construct it refused, and an application can act on that. */
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	return [[AliothMetalComputePipeline alloc] initWithAliothDevice:self
								 compute:compute
								    info:&info
							      descriptor:descriptor];
}

- (id)fail:(NSError **)error with:(NSString *)reason
{
	if (error != NULL) {
		*error = [NSError errorWithDomain:@"AliothMetal" code:1
					 userInfo:@{ NSLocalizedDescriptionKey: reason }];
	}
	return nil;
}

/*
 * The rest of what MTLDevice and MTLDeviceSPI require.
 *
 * Measured on the phone: 44 of the methods those two protocols require are answered by
 * nothing in the _MTLDevice chain (docs/metal.md 10.8). Until they are answered, any
 * application that asks one gets "unrecognized selector" and dies -- and that matters more
 * than it looks, because the gates inside Metal are assertions, so a process that asks a
 * question this device cannot answer does not degrade, it aborts, and launchd restarts it.
 *
 * So each of these answers, and each answers HONESTLY: a capability we have not got is
 * declined rather than claimed, a quantity we do not track says so in its comment rather
 * than inventing a number that something would then rely on. Where a number is a placeholder
 * it is marked as one; where the right authority is the compiler library rather than this
 * file, that is written down, because the next person to touch it will want to know.
 */

- (uint64_t)registryID
{
	/* Not invented: the registry's own identifier for the service Metal handed us. */
	uint64_t id = 0;

	if (_acceleratorService != IO_OBJECT_NULL &&
	    IORegistryEntryGetRegistryEntryID(_acceleratorService, &id) == KERN_SUCCESS) {
		return id;
	}
	return 0;
}

/* The a635 runs up to 1024 threads in a threadgroup, which is also what every pipeline the
 * compiler library has compiled reports back (10.15). The per-dimension limits are Metal's
 * usual shape for that; the authority should become the library itself once the contract has
 * a call for it. */
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 64); }

/* The number _MTLDevice's own limits table was filled with for this feature profile. Also a
 * placeholder in the same sense: the hardware's figure belongs to Mesa, not here. */
- (NSUInteger)maxThreadgroupMemoryLength { return 16384; }

- (BOOL)hasUnifiedMemory { return YES; }   /* one memory on this SoC, shared with the CPU */

/* Nothing here tracks allocation yet, and two of these are read by applications deciding how
 * much to allocate, so they answer a figure that is plausible for this phone rather than 0,
 * which would read as "allocate nothing". That they are not measured is the point of this
 * comment. */
- (uint64_t)recommendedMaxWorkingSetSize { return 512ULL << 20; }
- (uint64_t)currentAllocatedSize         { return 0; }
- (uint64_t)sharedMemorySize             { return 512ULL << 20; }
- (uint64_t)dedicatedMemorySize          { return 0; }   /* none: the memory is shared */
- (uint64_t)maxBufferLengthInternal      { return 256ULL << 20; }

- (BOOL)isLowPower      { return NO; }
- (BOOL)isHeadless      { return NO; }
- (BOOL)isRemovable     { return NO; }
- (BOOL)supportPriorityBand { return NO; }

/* Declined rather than claimed: no sparse textures, and a yes here would be acted on.
 *
 * Depth24Unorm_Stencil8 stays NO now that there IS a depth path, and it is not a leftover:
 * Metal's own header marks that format API_UNAVAILABLE(ios), the compiler library refuses it
 * by name for the same reason, and iOS depth is Depth16Unorm, Depth32Float or
 * Depth32Float_Stencil8 -- which this device does answer for. A yes here would offer a
 * format no texture could be made of. */
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }

/*
 * One sample, STILL -- and this one needs its reason written down, because it is now the
 * narrower statement of the two this driver could make.
 *
 * The compiler library does make multisample textures since docs/metal.md 10.34: the
 * Adreno gives 2 and 4 samples, a render pass of that count fills one, and a shader reads
 * one with read(coord, sample). So "count == 1" is no longer what this device CAN do with
 * a multisample texture.
 *
 * It is what this device can do with one for an APPLICATION, and that is what the
 * selector asks. An application that is told four samples makes a four-sample attachment
 * and then asks for its result in a one-sample texture -- MTLStoreActionMultisampleResolve,
 * or MTLRenderPassColorAttachmentDescriptor.resolveTexture -- and this driver has neither:
 * the contract stores what it has and resolves nothing. AppleMetalGLRenderer is exactly
 * such an application (its own six multisample fragment shaders are the ones 10.34 is
 * about), and a GL context given an MSAA config it cannot resolve is worse off than one
 * given none. So the yes waits for the resolve, and the measurement of the texture does
 * not wait for the yes: a test binds one without asking.
 *
 * (The K40, 2026-11-09: the resolve is here -- -endEncoding records it and the GPU does it,
 * rwa_texture_resolve -- so the answer is the Adreno's own, 1, 2 and 4.)
 *
 * Note also that -supportsTextureSampleCount: is not a second answer to keep in step. It
 * is one instruction in _MTLDevice (0x181a17890) that tail-calls -supportsSampleCount:,
 * which _MTLDevice itself does not implement at all -- so this method is the whole of
 * both, and without it that forward would die on an unrecognized selector naming OUR
 * class (docs/metal.md 10.1's trap).
 */
- (BOOL)supportsSampleCount:(NSUInteger)count
{
	return count == 1 || (mesa.texture_resolve != NULL && (count == 2 || count == 4));
}
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
{
	(void)type; (void)format; (void)samples;
	return MTLSizeMake(0, 0, 0);
}
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
			  sparsePageSize:(NSInteger)page
{
	(void)page;
	return [self sparseTileSizeWithTextureType:type pixelFormat:format sampleCount:samples];
}

/* These are bitfields of floating-point behaviours a device claims. Claiming none is the
 * honest answer while nothing has verified any of them on this hardware -- and docs/air.md
 * is precisely the document that would have to grow the evidence first. */
- (uint64_t)singleFPConfig { return 0; }
- (uint64_t)halfFPConfig   { return 0; }
- (uint64_t)doubleFPConfig { return 0; }

- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(NSUInteger)format
{
	(void)format;
	return 16;   /* a placeholder: the real figure is per format and belongs to Mesa */
}

/* Heaps are suballocation of one big allocation, and nothing here suballocates. (The K40, 2026-11-09:) a heap is
 * served as separate allocations through this device (AliothMetalHeap), and the size-and-align questions are answered
 * with what such an allocation takes, so an engine that sizes its heaps sizes them sensibly. */
- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor
{
	AliothMetalHeap *heap = [[AliothMetalHeap alloc] init];

	heap->device = self;
	heap->size = [descriptor size];
	heap->storageMode = [descriptor storageMode];
	heap->cpuCacheMode = [descriptor cpuCacheMode];
	heap->hazardTrackingMode = [descriptor hazardTrackingMode];
	heap->type = [descriptor type];
	return heap;
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)options;
	return (MTLSizeAndAlign){ (length + 255) & ~(NSUInteger)255, 256 };
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
	NSUInteger texel = alioth_texel_bytes([d pixelFormat]);
	NSUInteger faces = ([d textureType] == MTLTextureTypeCube || [d textureType] == MTLTextureTypeCubeArray) ? 6 : 1;
	NSUInteger total = 0;

	if (texel == 0) {
		texel = 4;      /* (a compressed or unusual format: an estimate rather than nothing) */
	}
	for (NSUInteger level = 0; level < MAX([d mipmapLevelCount], 1); level++) {
		total += MAX([d width] >> level, 1) * MAX([d height] >> level, 1) * MAX([d depth] >> level, 1) * texel;
	}
	total *= MAX([d arrayLength], 1) * faces * MAX([d sampleCount], 1);
	return (MTLSizeAndAlign){ (total + 4095) & ~(NSUInteger)4095, 4096 };
}

/* (the K40's) fences and events: see AliothMetalFence */
- (id)newFence
{
	AliothMetalFence *fence = [[AliothMetalFence alloc] init];

	fence->device = self;
	return fence;
}

- (id)newEvent
{
	AliothMetalEvent *event = [[AliothMetalEvent alloc] init];

	event->device = self;
	event->waiting = [[NSMutableArray alloc] init];
	return event;
}

- (id)newSharedEvent { return [self newEvent]; }

- (id)newSharedEventWithHandle:(id)handle
{
	(void)handle;
	RW_ONCE("a shared event from another process's handle; this driver's events live in one process, so there is none\n");
	return nil;
}

/*
 * The one method -[CAMetalLayer nextDrawable] asks of a device (docs/metal.md 10.46):
 * allocate_drawable_texture builds an ordinary MTLTextureDescriptor, sets its private
 * rotation/isDrawable/framebufferOnly/usage and sends this. The surface, its geometry and
 * every refusal live in src/alioth_drawable.m, because they answer CoreAnimation rather than
 * Metal; what is left here is the object, which has to be of this class.
 *
 * The refusal is printed rather than returned silently, and that is not a style choice:
 * CoreAnimation does not test what it gets back, so a bare nil becomes a CAMetalDrawable
 * whose -texture is nil, and the application finds out later and somewhere else.
 */
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)s plane:(NSUInteger)p
{
	char why[512] = { 0 };
	struct rwd_surface took;
	struct rwa_texture_shape shape;   /* this file's own declaration; rwd_texture_shape is the
					   * same layout under the other file's name, and the cast
					   * below is the one place the two meet */
	struct rwa_texture *texture;

	if (_mesa == NULL) {
		rw_print("a drawable's texture needs the compiler library\n");
		return nil;
	}
	memset(&shape, 0, sizeof(shape));
	shape.pixel_format = (unsigned)[d pixelFormat];
	shape.type = (unsigned)[d textureType];
	shape.width = (unsigned)[d width];
	shape.height = (unsigned)[d height];
	shape.depth = (unsigned)[d depth];
	shape.array_length = (unsigned)[d arrayLength];
	shape.mip_levels = (unsigned)[d mipmapLevelCount];
	shape.sample_count = (unsigned)[d sampleCount];
	/*
	 * (The K40: a texture over an IOSurface can always be drawn into. CAMetalLayer's drawable with framebufferOnly NO
	 * came with a usage that did not say so, and its first draw was refused -- "made without
	 * MTLTextureUsageRenderTarget" -- while a pass that only cleared it went through, Metal Check 2026-11-08.)
	 */
	shape.usage = (unsigned)([d usage] | MTLTextureUsageRenderTarget);
	texture = rwd_texture_for_surface(_mesa, s, p, (const struct rwd_texture_shape *)&shape,
					  &took, why, sizeof(why));
	if (texture == NULL) {
		RW_AGAIN("no drawable: %s\n", why);
		return nil;
	}
	{
		/* (the K40: which IOSurface, for the display's "surfaces in layers" lines -- the first few) */
		static _Atomic unsigned told;

		if (told < 4) {
			told++;
			rw_print("a texture over IOSurface %u (%ux%u, rows of %u bytes, format %#x, %u planes), GPU buffer %u\n",
			    took.surface, took.width, took.height, took.rowBytes, took.pixelFormat, took.planes, took.handle);
		}
	}
	return [[AliothMetalTexture alloc] initWithAliothDevice:self
							 texture:texture
							   shape:&shape];
}
- (id)newBufferWithIOSurface:(void *)surface        { (void)surface; return nil; }
- (id)newBufferWithBytesNoCopy:(void *)bytes
			length:(NSUInteger)length
		       options:(NSUInteger)options
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	(void)bytes; (void)length; (void)options; (void)deallocator;
	return nil;   /* wrapping the caller's own pages needs the kernel to map them: not yet */
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
			      maxCommandCount:(NSUInteger)count
				      options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
				    maxCount:(NSUInteger)count
				     options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectComputeCommandEncoderWithBuffer:(id)b { (void)b; return nil; }
- (id)newIndirectRenderCommandEncoderWithBuffer:(id)b  { (void)b; return nil; }
- (id)newIndirectArgumentBufferLayoutWithStructType:(id)t { (void)t; return nil; }
- (id)newVisibleFunctionTableWithDescriptor:(id)d      { (void)d; return nil; }
- (id)newIntersectionFunctionTableWithDescriptor:(id)d { (void)d; return nil; }

/*
 * The last paths in the chain that would still abort.
 *
 * Asking the device whether it responds to these says yes -- they are real entries in
 * _MTLDevice's method list -- and calling one aborts, because each body is
 * [self doesNotRecognizeSelector:_cmd]. Seven methods of _MTLDevice are abstract in that
 * way (tools/metal/absmeth.py), one of them is -initWithAcceleratorPort: which this driver
 * implements, and the other six are these: ray tracing. Overriding them to decline turns the
 * last aborting paths into ordinary nil, which is what the "0 unanswered" measurement needs
 * in order to mean anything.
 */
- (id)newAccelerationStructureWithSize:(NSUInteger)size { (void)size; return nil; }
- (id)newAccelerationStructureWithSize:(NSUInteger)size resourceIndex:(NSUInteger)index
{
	(void)size; (void)index;
	return nil;
}
- (id)newAccelerationStructureWithSize:(NSUInteger)size withDescriptor:(id)descriptor
{
	(void)size; (void)descriptor;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer offset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer
				  offset:(NSUInteger)offset
			   resourceIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	return nil;
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)size
{
	(void)size;
	return (MTLSizeAndAlign){ 0, 0 };
}

- (BOOL)metalAssertionsEnabled { return _assertions; }
- (void)setMetalAssertionsEnabled:(BOOL)enabled { _assertions = enabled; }

/* Without this the device inherits _MTLDevice's own answer, which is "Apple GPU" -- true of
 * the code and false of the hardware. The name is Mesa's renderer string for this chip
 * (docs/air.md: `Adreno 7c+ Gen 3, chip 0x06030500`), and the identifier beside it is asked
 * of the kernel rather than written down, so a different phone says something different.
 * Nothing in Metal's registration reads this; a person reading a log does. */
- (NSString *)name
{
	uint64_t chip = 0;

	if (gpu_param(_gpu, 0x03, &chip)) {
		return [NSString stringWithFormat:@"Adreno 650 (chip %#x, K40)",
				 (unsigned)(chip & 0xffffffffu)];
	}
	return @"Adreno (K40; the GPU is not started)";
}

/*
 * The answer that could not be "nothing", and the first place in this driver where that is
 * true. logs/210311-run.log is backboardd dying every ten seconds on
 *
 *   signal 11 at 0x10, pc 0x181a3ca3c, called from 0x18847f6dc
 *
 * which is MTLPipelineLibraryBuilder::newLibraryWithFile + 836, reached from
 * CA::OGL::new_metal_context + 1808 -- QuartzCore asking this device for the precompiled
 * pipelines of its own default.pipelinelib. Nothing of ours is on that stack:
 * -[_MTLDevice newPipelineLibraryWithFilePath:error:] is three instructions (0x181a3d9b0,
 * `mov x1, x0; ldr x0, [x0, #0x180]; b`) that hand the device and its own builder ivar to
 * Apple's C++, which gets as far as parsing pipelines.desc out of the image and then asks us
 * one question. The three instructions that matter:
 *
 *   0x181a3c9ec  objc_opt_respondsToSelector(device, "targetDeviceInfo")   -> YES
 *   0x181a3c9f8  objc_msgSend$targetDeviceInfo                             -> NULL
 *   0x181a3ca3c  ldr x8, [x23, #0x10]                                      -> 0x10
 *
 * Both halves of that are Apple's. `_MTLDevice` HAS an entry for -targetDeviceInfo and its
 * body is `mov x0, #0; ret` (0x181aa8d98 -- tools/metal/absmeth.py calls that a nil-stub),
 * so the runtime answers the question YES on our behalf and the pointer is NULL; and the
 * caller treats a YES as a promise and dereferences what comes back. Declining is therefore
 * not on the table: returning nil IS what crashes, and the only way to make the runtime say
 * NO would be to lie in -respondsToSelector:, which would also lie to every caller that
 * does not ask first. Of the eight objc_opt_respondsToSelector sites in all of
 * Metal.framework this is the only one whose answer is a raw pointer rather than a scalar or
 * an object -- the other seven ask about -shaderDebugInfoCaching, -maximumComputeSubstreams
 * (twice), -presentWithOptions: (twice), -boolValue and one selector held in a register.
 *
 * The shape can recur, and the population is counted rather than feared: of `_MTLDevice`'s
 * 522 instance methods 74 are nil-stubs, and 14 of those are declared to return a raw
 * pointer (absmeth.py joined to protometh.py by implementation address). Thirteen of the
 * fourteen are the pipeline-cache key family -- `*FunctionKeyWith...:keySize:` and
 * `get*FunctionId:compilerOptions:compilerOptionsSize:` -- which is reached only once a
 * pipeline cache actually holds something; this is the fourteenth, and the first one
 * anything asked for.
 *
 * So the struct is real, and its contents are the ones Apple's own code writes in the branch
 * it takes for a device that does not respond (0x181a3ca04): zero, zero, and the device's
 * -name with the spaces removed. Its shape is known twice over, from two sources that agree
 * -- that branch, which builds one on its own stack, and the type MTLDeviceSPI records for
 * this method, `r^{MTLTargetDeviceArch=QI*}16@0:8`. Each field by its reader:
 *
 *   +0x00  nobody on this path reads it, and Apple's branch writes 0.
 *   +0x08  read by -[_MTLPipelineCache initWithFilePath:readOnly:deviceInfo:] (0x181a3b720),
 *          which pairs it with 1 into an MTLCompilerCache::Version for
 *          MTLCompilerCache::createPipelineDataCache. That version decides whether a cache
 *          FILE is stale; this cache is opened read-only (w3=1 at 0x181a3ca64) over a file
 *          no image here has, so there is nothing for it to invalidate, and 0 is the number
 *          Apple's own branch uses. It becomes a real question the day this driver WRITES a
 *          pipeline cache, and not before.
 *   +0x10  formatted into "%s/pipelines.%s" (0x181a3ca48) -- the per-GPU cache file inside
 *          QuartzCore.framework/default.pipelinelib. It must be a C string and not NULL:
 *          Metal's second cache, the one for `functions`, it builds with a zeroed struct and
 *          an EMPTY name itself (0x181a3ca7c), which is the measurement that says a zeroed
 *          struct is acceptable to _MTLPipelineCache.
 *
 * What this buys is a refusal with a reason instead of a crash, and the refusal is the
 * honest one: that image ships `functions` and `pipelines.desc` and no pipelines.* for any
 * GPU at all (not even the A11 this phone was born with), so the cache comes back nil,
 * Metal fills the NSError with "Could not open pipeline cache file: %s" (code 6,
 * 0x181a3cb2c), and QuartzCore logs "Metal failed to load pipeline library: %s" and carries
 * on -- 0x18847f6f0 is a `cbnz`, not an assert. Nothing is lost by it: a precompiled
 * pipeline in that cache would be A11 machine code, which this driver could not run anyway.
 * The pipelines CoreAnimation needs get compiled the ordinary way, through
 * -newRenderPipelineStateWithDescriptor: and AIR.
 */
- (const struct MTLTargetDeviceArch *)targetDeviceInfo
{
	/* The whole point of this method is that a NULL here is fatal, so it must not depend on
	 * an initialiser having run: an object built with plain -init rather than Metal's
	 * -initWithAcceleratorPort: has a zeroed ivar, and an empty name is what Apple's own
	 * second cache passes for itself anyway. The array is zeroed by the runtime, so it
	 * already spells "". */
	if (_target.name == NULL) {
		_target.name = _targetName;
	}
	RW_ONCE("Metal asked for targetDeviceInfo (pipelines.%s); it is answered "
		"with a real struct because Apple's own answer here is a NULL this driver does "
		"not get to decline\n", _target.name);
	return &_target;
}

/* The two buffer entry points of MTLDevice that nothing in the chain answers. Anything
 * Metal would ask about storage modes is answered by the buffer itself. */
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)options;   /* every allocation here is CPU-visible; the buffer says so honestly */
	return [[AliothMetalBuffer alloc] initWithDevice:self gpu:_gpu length:length];
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(NSUInteger)options
{
	AliothMetalBuffer *buffer = [self newBufferWithLength:length options:options];

	if (buffer != nil && bytes != NULL && [buffer contents] != NULL) {
		memcpy([buffer contents], bytes, length);
	}
	return buffer;
}

/* Profile 0's own table says maxVertexAmplificationCount is 1, so anything above one is
 * not supported, and saying otherwise would be a lie Metal would act on. */
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count
{
	return count <= 1;
}

@end

@implementation AliothMetalBuffer

+ (void)load
{
	Protocol *p = objc_getProtocol("MTLBuffer");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLBuffer) refused\n");
	}
}

- (instancetype)initWithDevice:(AliothMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length
{
	struct gem_new request = { .size = length, .flags = 0, .handle = 0 };
	size_t size = sizeof(request);

	self = [super init];
	if (self == nil) {
		return nil;
	}

	/* Two ways to the same kernel. When the compiler library is loaded, the memory comes
	 * from Mesa, because only such a buffer can be handed to a dispatch; without it the
	 * kernel's GEM interface is used directly, so that a device with no GPU library still
	 * gives memory a program can write. Which one happened is visible in -mesaBuffer. */
	if (mesa_available() && [device mesaDevice] != NULL) {
		char why[256] = { 0 };

		_mesa = mesa.buffer_create([device mesaDevice], length, why, sizeof(why));
		if (_mesa == NULL) {
			rw_print("Mesa would not allocate %lu bytes: %s\n",
			         (unsigned long)length, why);
			[self release];
			return nil;
		}
		_owner = device;
		_length = length;
		_contents = mesa.buffer_map(_mesa);
		return self;
	}
	if (gpu == IO_OBJECT_NULL || length == 0) {
		rw_print("a buffer of %lu bytes needs a gpu client\n",
		         (unsigned long)length);
		[self release];
		return nil;
	}
	if (IOConnectCallStructMethod(gpu, kMsmGemNew, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS || request.handle == 0) {
		rw_print("the kernel would not allocate %lu bytes\n",
		         (unsigned long)length);
		[self release];
		return nil;
	}
	_owner = device;
	_gpu = gpu;
	_handle = request.handle;
	_length = length;

	/* the address the GPU will see, which is what a command stream carries */
	struct gem_info info = { .handle = _handle, .info = MSM_INFO_GET_IOVA };

	size = sizeof(info);
	if (IOConnectCallStructMethod(gpu, kMsmGemInfo, &info, sizeof(info), &info,
				      &size) == KERN_SUCCESS) {
		_iova = info.value;
	}

	/* and the address this processor will see: the mapping is as the memory is made,
	 * write-combined, exactly as Mesa takes it */
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	if (IOConnectMapMemory64(gpu, _handle, mach_task_self(), &address, &mapped,
				 kIOMapAnywhere) == KERN_SUCCESS) {
		_contents = (void *)(uintptr_t)address;
	} else {
		rw_print("buffer %u is allocated but could not be mapped\n", _handle);
	}
	return self;
}

- (struct rwa_buffer *)mesaBuffer
{
	return _mesa;
}

- (void)dealloc
{
	if (_mesa != NULL) {
		mesa.buffer_destroy(_mesa);
		[super dealloc];
		return;
	}
	if (_contents != NULL) {
		IOConnectUnmapMemory64(_gpu, _handle, mach_task_self(),
				       (mach_vm_address_t)(uintptr_t)_contents);
	}
	if (_handle != 0) {
		struct gem_close request = { .handle = _handle, .pad = 0 };
		size_t size = sizeof(request);

		IOConnectCallStructMethod(_gpu, kDrmGemClose, &request, sizeof(request), &request,
					  &size);
	}
	[super dealloc];
}

- (void *)contents          { return _contents; }
- (NSUInteger)length        { return _length; }
- (id)device                { return _owner; }
- (uint64_t)gpuAddress      { return _iova; }
- (NSUInteger)allocatedSize { return _length; }

/* The memory is write-combined and the GPU reads it through no cache of ours, so there is
 * nothing to push out; saying so is not the same as doing nothing by accident. */
- (void)didModifyRange:(NSRange)range    { (void)range; }

/* What was asked for does not change what was given: every allocation here is one kind of
 * memory, and the honest answer is the kind it is. */
- (NSUInteger)storageMode        { return 0; }   /* MTLStorageModeShared */
- (NSUInteger)cpuCacheMode       { return 1; }   /* MTLCPUCacheModeWriteCombined */
- (NSUInteger)resourceOptions    { return 0; }
- (NSUInteger)hazardTrackingMode { return 0; }   /* MTLHazardTrackingModeDefault */

/* MTLPurgeableStateNonVolatile (2), whatever was asked for, and that is the answer and not
 * a refusal: this memory is never volatile -- the kernel holds it from kMsmGemNew to
 * kDrmGemClose -- so there is no state for a caller to change and none to report but this
 * one. (Zero, which this used to answer, is not an MTLPurgeableState at all, and Metal has
 * a path that then believes the resource was purged: see -addPurgedResource: on the command
 * buffer.) */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }
- (id)heap                      { return nil; }
- (NSUInteger)heapOffset        { return 0; }
- (BOOL)isAliasable             { return NO; }
- (void)makeAliasable           { }

/*
 * What <MTLResourceSPI> and <MTLBuffer> require and nothing in this chain answered: ten
 * methods, every one of them an abort in whatever daemon asked first. They are answers, and
 * most of them are true rather than evasive -- which is the point of counting what a
 * protocol demands instead of what our own tests happen to send.
 *
 * Aliasing: nothing here aliases anything. Every buffer is its own GEM allocation (or its
 * own Mesa allocation), there are no heaps and -makeAliasable does nothing, so "does this
 * overlap that" is NO for every pair, for certain.
 *
 * Completion: this driver's -commit runs the recorded work and -waitUntilCompleted fences
 * it, and nothing hands a resource back to a caller before the submission it was part of
 * has been made. There is no per-resource completion to wait for, so YES and a return are
 * the honest pair -- and if a second ring ever appears, these two are where it has to be
 * answered again.
 */
- (BOOL)doesAliasResource:(id)resource { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)isComplete        { return YES; }
- (void)waitUntilComplete { }
- (BOOL)isPurgeable       { return NO; }
/* Protected (DRM) memory is a kernel capability this port does not have; zero is "none of
 * the protection options", which is what this memory has. */
- (NSUInteger)protectionOptions { return 0; }
/* The options before the device filtered them: the device filters nothing, so this is the
 * same answer -resourceOptions gives. */
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Whose allocation this is for the kernel's accounting. We do not tell the kernel, so the
 * only true answer is this process, and a setter that remembers what it was told. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }
/*
 * A texture laid over this buffer's memory. Textures exist now -- the comment here used to say
 * they did not -- but the contract still has no call that lays one over a buffer we already own,
 * so this is nil with a reason rather than nil with a stale excuse.
 */
- (id)newTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
		   bytesPerRow:(NSUInteger)bytesPerRow
{
	(void)descriptor; (void)offset; (void)bytesPerRow;
	RW_ONCE("aliothmetal: a texture over a buffer was asked for; the contract lays a texture "
		"over an IOSurface and over memory of its own, but not over a buffer this driver "
		"already holds\n");
	return nil;
}

- (id)newLinearTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
			 bytesPerRow:(NSUInteger)bytesPerRow
		       bytesPerImage:(NSUInteger)bytesPerImage
{
	(void)bytesPerImage;
	return [self newTextureWithDescriptor:descriptor offset:offset bytesPerRow:bytesPerRow];
}
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range { (void)marker; (void)range; }
- (void)removeAllDebugMarkers { }

@end

/*
 * (the K40's) A texture object this driver did not make answers no compiler-library texture, and the draw that binds
 * it is refused by name ("reads texture 0 and 0 textures were bound") instead of the process dying of an
 * unrecognized selector. SpriteKit is how it happens: in a process whose SKViews began with OpenGL ES, an SKRenderer
 * on this device was handed SpriteKit's GL-backed texture (System Check, 2026-11-09: -[EAGLContext mesaTexture]).
 */
@interface NSObject (AliothMetalForeign)
- (struct rwa_texture *)mesaTexture;
@end

@implementation NSObject (AliothMetalForeign)
- (struct rwa_texture *)mesaTexture
{
	RW_ONCE("a %s was bound where a texture of this driver belongs; it is taken as nothing bound\n",
		object_getClassName(self));
	return NULL;
}
@end

@implementation AliothMetalTexture

+ (void)load
{
	/* MTLTexture is what an application sees. MTLTextureImplementation is what Metal
	 * itself checks: -[MTLRenderPassColorAttachmentDescriptorInternal setTexture:]
	 * (0x181a350a0) asks conformsToProtocol: for it and reports "texture is not a
	 * MTLTextureImplementation." Only the conformance is tested on that path -- nothing
	 * sends its 68 members -- so declaring it is honest and is what lets a texture of ours
	 * be a render target at all. */
	const char *names[] = { "MTLTexture", "MTLTextureImplementation" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *p = objc_getProtocol(names[i]);

		if (p == NULL) {
			continue;
		}
		if (!class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
			rw_print("class_addProtocol(%s) refused\n", names[i]);
		}
	}
}

- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
			      texture:(struct rwa_texture *)texture
				shape:(const struct rwa_texture_shape *)shape
{
	self = [super init];
	if (self == nil) {
		mesa.texture_destroy(texture);
		return nil;
	}
	_owner = device;
	_texture = texture;
	_shape = *shape;
	return self;
}

- (void)dealloc
{
	if (_texture != NULL) {
		mesa.texture_destroy(_texture);
	}
	[_parent release];
	[super dealloc];
}

- (struct rwa_texture *)mesaTexture { return _texture; }

- (id)device                       { return _owner; }
- (NSUInteger)width                { return _shape.width; }
- (NSUInteger)height               { return _shape.height; }
- (NSUInteger)depth                { return _shape.depth; }
- (NSUInteger)mipmapLevelCount     { return _shape.mip_levels; }
- (NSUInteger)arrayLength          { return _shape.array_length; }
- (NSUInteger)sampleCount          { return _shape.sample_count; }
- (NSUInteger)pixelFormat          { return _shape.pixel_format; }
- (NSUInteger)textureType          { return _shape.type; }
- (NSUInteger)usage                { return _shape.usage; }
- (BOOL)isFramebufferOnly          { return NO; }
- (BOOL)allowGPUOptimizedContents  { return NO; }
- (id)parentTexture                { return _parent; }
- (NSUInteger)parentRelativeLevel  { return 0; }
- (NSUInteger)parentRelativeSlice  { return 0; }
- (id)buffer                       { return nil; }
- (NSUInteger)bufferOffset         { return 0; }
- (NSUInteger)bufferBytesPerRow    { return 0; }
- (id)iosurface                    { return nil; }
- (NSUInteger)iosurfacePlane       { return 0; }
/* MTLStorageModeShared, except for a multisample texture, which Metal does not allow to be
 * one: "MTLTextureDescriptor: Multisample textures cannot be allocated with
 * MTLStorageModeShared on this device" is Metal's own sentence, in the framework's own
 * __cstring and not in the validation layer. Answering Private for one is not bookkeeping
 * -- Shared is what makes -replaceRegion: and -getBytes: legal, and those are exactly the
 * two calls the compiler library refuses on a multisample texture (and refuses because
 * neither side can give the samples of a texel a defined order through a map). So the
 * storage mode this reports and the calls that work on it say the same thing. */
- (NSUInteger)storageMode
{
	return _shape.sample_count > 1 ? 2 : 0;   /* MTLStorageModePrivate : Shared */
}
- (NSUInteger)cpuCacheMode         { return 1; }
- (NSUInteger)resourceOptions      { return 0; }
- (NSUInteger)hazardTrackingMode   { return 0; }
- (NSUInteger)allocatedSize        { return 0; }   /* not accounted for yet, and says so */
/* MTLPurgeableStateNonVolatile, for the reason written over the buffer's own: a texture of
 * the compiler library's is held until it is destroyed, so there is no volatile state to
 * report and zero was not a state at all. */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }
- (id)heap                         { return nil; }
- (NSUInteger)heapOffset           { return 0; }
- (BOOL)isAliasable                { return NO; }
- (void)makeAliasable              { }

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
		slice:(NSUInteger)slice
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
	bytesPerImage:(NSUInteger)image
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	(void)image;
	if (mesa.texture_write(_texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			       stride, why, sizeof(why)) != 0) {
		rw_print("writing texels: %s\n", why);
	}
}

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
{
	[self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:stride
	      bytesPerImage:0];
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
   bytesPerImage:(NSUInteger)image
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
	   slice:(NSUInteger)slice
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	(void)image;
	if (mesa.texture_read(_texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			      stride, why, sizeof(why)) != 0) {
		rw_print("reading texels: %s\n", why);
	}
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:stride bytesPerImage:0 fromRegion:region
	   mipmapLevel:level slice:0];
}

- (BOOL)aliothReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	if (mesa.depth_read == NULL) {
		rw_print("this compiler library has no rwa_depth_read, so it is "
		         "older than this driver and cannot say what a depth buffer holds\n");
		return NO;
	}
	if (mesa.depth_read(_texture, 0, 0, origin, size, depth, stencil, why,
			    sizeof(why)) != 0) {
		rw_print("reading the depth of a texture: %s\n", why);
		return NO;
	}
	return YES;
}

/* A view of a texture is another texture over the same memory. (The K40, 2026-11-09: nil until
 * the compiler library had rwa_texture_view; SceneKit asks for both forms.) */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
{
	return [self newTextureViewWithPixelFormat:format textureType:_shape.type
					    levels:NSMakeRange(0, MAX(_shape.mip_levels, 1))
					    slices:NSMakeRange(0, [self aliothSlices])];
}

/* the slices a view of the whole texture spans: Metal counts a cube's faces as slices */
- (NSUInteger)aliothSlices
{
	return MAX(_shape.array_length, 1) * ((_shape.type == 5 || _shape.type == 6) ? 6 : 1);
}

/*
 * THE REST OF WHAT <MTLTexture>, <MTLTextureSPI>, <MTLResource> AND <MTLResourceSPI>
 * REQUIRE: 27 methods with no answer anywhere in this chain before this section. The same
 * three kinds as everywhere else -- served, true by construction, or refused out loud.
 *
 * A view is the whole of the texture in another format of its texel size or another type of
 * its dimensions (rwa_texture_view); a range of its levels or slices is refused out loud, nil,
 * because a view that aliased the wrong part would be found much later, by a picture that did
 * not change.
 */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
{
	char why[512] = { 0 };
	struct rwa_texture_shape shape = _shape;
	struct rwa_texture *view;

	if (mesa.texture_view == NULL) {
		RW_ONCE("a texture view, and this compiler library has no rwa_texture_view; nil\n");
		return nil;
	}
	if (levels.location != 0 || levels.length != MAX(_shape.mip_levels, 1) || slices.location != 0 ||
	    slices.length != [self aliothSlices]) {
		RW_AGAIN("a texture view of levels %lu+%lu and slices %lu+%lu: only a view of the whole texture is "
			 "made here; nil\n", (unsigned long)levels.location, (unsigned long)levels.length,
			 (unsigned long)slices.location, (unsigned long)slices.length);
		return nil;
	}
	shape.pixel_format = (unsigned)format;
	shape.type = (unsigned)type;
	if ((view = mesa.texture_view(_texture, &shape, why, sizeof(why))) == NULL) {
		RW_AGAIN("a texture view: %s; nil\n", why);
		return nil;
	}
	AliothMetalTexture *texture = [[AliothMetalTexture alloc] initWithAliothDevice:_owner texture:view
										 shape:&shape];

	if (texture != nil) {
		texture->_parent = [self retain];
	}
	return texture;
}
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
			    swizzle:(MTLTextureSwizzleChannels)swizzle
{
	(void)swizzle;
	return [self newTextureViewWithPixelFormat:format textureType:type levels:levels
					    slices:slices];
}
- (id)newSharedTextureHandle { return nil; }   /* nothing here is shared between processes */

/* Served out of the shape this texture was made with, which is the only place that
 * knowledge exists: a cube is six faces, everything else is one. (The K40, 2026-11-09:
 * MTLTextureTypeCube is 5 and CubeArray 6 in the iOS 16.5 SDK's MTLTexture.h; this said 3
 * and 4, which are 2DArray and 2DMultisample, and answered six faces for those.) */
- (NSUInteger)numFaces
{
	return (_shape.type == MTLTextureTypeCube || _shape.type == MTLTextureTypeCubeArray) ? 6 : 1;
}
/* The identity swizzle, and it is the truth rather than a default: a texture of this driver
 * is made from a descriptor's format alone and nothing reorders its channels, so red reads
 * as red. MTLTextureSwizzleChannelsDefault is that same quadruple. */
- (MTLTextureSwizzleChannels)swizzle
{
	return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
					     MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
}
- (unsigned int)swizzleKey { return 0; }   /* the key of "nothing is swizzled" */
/* Nothing here is a view, so every texture is its own root, and nil is what Apple's own
 * resource answers for one that was allocated rather than derived. */
- (id)rootResource { return nil; }

/*
 * True by construction, and each is a sentence about this driver rather than a shrug:
 *
 *   - nothing aliases: each texture is its own allocation of the compiler library's, there
 *     are no heaps, and -makeAliasable does nothing, so "does this overlap that" is NO for
 *     every pair, for certain;
 *   - nothing is compressed: the library allocates surfaces with no lossless-compression
 *     metadata, so there is no compression and nothing to feed back;
 *   - nothing is sparse: there is no tail, so its size and its first mipmap are zero;
 *   - nothing is a drawable: a texture of this driver never comes from a CAMetalLayer, it is
 *     the attachment a pass draws into and -getBytes: reads back;
 *   - nothing is purgeable or incomplete, for the reason written over the buffer's pair.
 */
- (BOOL)doesAliasResource:(id)resource { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)isCompressed             { return NO; }
- (NSInteger)compressionFeedback { return 0; }
- (BOOL)isSparse                 { return NO; }
- (NSUInteger)firstMipmapInTail  { return 0; }
- (NSUInteger)tailSizeInBytes    { return 0; }
- (BOOL)isShareable              { return NO; }
- (BOOL)isDrawable               { return NO; }
- (BOOL)isComplete               { return YES; }
- (void)waitUntilComplete        { }
- (BOOL)isPurgeable              { return NO; }
- (NSUInteger)protectionOptions  { return 0; }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Zero is "not rotated": this texture is not a rotated view of a display surface, which is
 * the only thing in Metal that sets this. */
- (NSUInteger)rotation { return 0; }
/*
 * The identifier an argument buffer would hold for this texture. This driver has no
 * argument buffers -- the device answers no to every tier of them -- and the compiler
 * library hands out no bindless handle, so there is no identifier to give; zero is the one
 * value that cannot be mistaken for a resource, and it is said once because a caller that
 * asks is a caller that was about to write it into a buffer a shader reads.
 */
- (MTLResourceID)gpuResourceID
{
	MTLResourceID resourceID = { 0 };

	RW_ONCE("a GPU resource ID was asked of a texture; this driver has no "
		"argument buffers and no bindless handles, so it answers zero\n");
	return resourceID;
}
- (NSUInteger)gpuHandle { return 0; }
/* Whose allocation this is for the kernel's accounting. We never tell the kernel, so the
 * true answer is this process until somebody says otherwise, and then it is what they said. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }

@end

@implementation AliothMetalSampler

- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
			      sampler:(struct rwa_sampler *)sampler
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilySampler sends to super; the descriptor is passed on
	 * because the base keeps it and the pipeline-state assertions taught us not to send
	 * nil to one of these */
	self = [super initWithDevice:device samplerDescriptor:descriptor];
	if (self == nil) {
		mesa.sampler_destroy(sampler);
		return nil;
	}
	_sampler = sampler;
	return self;
}

- (void)dealloc
{
	if (_sampler != NULL) {
		mesa.sampler_destroy(_sampler);
	}
	[super dealloc];
}

- (struct rwa_sampler *)mesaSampler { return _sampler; }

@end

@implementation AliothMetalDepthStencilState

- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor
{
	/* The base class's own designated initialiser, which validates the descriptor,
	 * sends `init` to super and fills _device and _label (0x181a29b38). The descriptor
	 * is handed on for the same reason the sampler's is: the base reads it, and a nil
	 * there is reported by Apple's own validation. */
	self = [super initWithDevice:device depthStencilDescriptor:descriptor];
	if (self == nil) {
		mesa.depth_stencil_destroy(state);
		return nil;
	}
	_state = state;

	/*
	 * The four answers <MTLDepthStencilStateSPI> requires and the base class gives as
	 * NO (nil-stubs at 0x181b1b5e0 and its neighbours). They are the only methods of
	 * this object that are a statement about what it will do, so they are derived from
	 * the descriptor rather than left at a default that would be false for every state
	 * that writes anything.
	 *
	 * "Reads" means the test consults the buffer: MTLCompareFunctionAlways consults
	 * nothing, anything else does. For the stencil an operation other than Keep also
	 * reads it, because incrementing or inverting a value means fetching it first.
	 */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };

	_writesDepth = [descriptor isDepthWriteEnabled];
	_readsDepth = [descriptor depthCompareFunction] != MTLCompareFunctionAlways;
	for (unsigned i = 0; i < 2; i++) {
		MTLStencilDescriptor *f = faces[i];
		BOOL keeps;

		if (f == nil) {
			continue;   /* null_resettable, so this is Metal's default: no stencil */
		}
		keeps = [f stencilFailureOperation] == MTLStencilOperationKeep &&
			[f depthFailureOperation] == MTLStencilOperationKeep &&
			[f depthStencilPassOperation] == MTLStencilOperationKeep;
		if ([f stencilCompareFunction] != MTLCompareFunctionAlways || !keeps) {
			_readsStencil = YES;
		}
		if (!keeps && [f writeMask] != 0) {
			_writesStencil = YES;
		}
	}
	return self;
}

- (void)dealloc
{
	if (_state != NULL) {
		mesa.depth_stencil_destroy(_state);
	}
	[super dealloc];
}

- (struct rwa_depth_stencil *)mesaDepthStencil { return _state; }

- (BOOL)readsDepth    { return _readsDepth; }
- (BOOL)writesDepth   { return _writesDepth; }
- (BOOL)readsStencil  { return _readsStencil; }
- (BOOL)writesStencil { return _writesStencil; }

@end

/*
 * (the K40's) Metal's pipeline reflection, made of what the compiler library read out of Apple's metadata
 * (rwa_render_arguments, rwa_compute_arguments): MTLArgument and the iOS 16 MTLBinding in one object, MTLStructType,
 * MTLStructMember, MTLArrayType, and MTLRenderPipelineReflection / MTLComputePipelineReflection. Metal's own classes
 * are made by its compiler, which is nil here, so it was reflection that was nil -- and SpriteKit's renderer (Jet)
 * and SceneKit look every argument up BY NAME in -vertexArguments / -fragmentArguments and bind at the index it
 * reports (SceneKit also finds a buffer's members by name). With nil they bound nothing: "FastSingle_FragFunc reads
 * texture 0 and 0 textures were bound". These answer by the protocols' selectors; nothing checks their class.
 */

/* MTLDataType from a type name as Apple's metadata writes it ("float4x4", "half3", "packed_float3", "uint") */
static NSUInteger
alioth_data_type(const char *type)
{
	static const struct {
		const char *name;
		NSUInteger scalar;
	} bases[] = {
		{ "float", MTLDataTypeFloat }, { "half", MTLDataTypeHalf }, { "int", MTLDataTypeInt },
		{ "uint", MTLDataTypeUInt }, { "short", MTLDataTypeShort }, { "ushort", MTLDataTypeUShort },
		{ "char", MTLDataTypeChar }, { "uchar", MTLDataTypeUChar }, { "bool", MTLDataTypeBool },
	};

	if (type == NULL) {
		return MTLDataTypeNone;
	}
	if (strncmp(type, "packed_", 7) == 0) {
		type += 7;
	}
	for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
		size_t n = strlen(bases[i].name);
		const char *rest = type + n;

		if (strncmp(type, bases[i].name, n) != 0) {
			continue;
		}
		if (*rest == '\0') {
			return bases[i].scalar;
		}
		if (rest[0] >= '2' && rest[0] <= '4') {
			unsigned columns = (unsigned)(rest[0] - '0');

			if (rest[1] == '\0') {
				return bases[i].scalar + (columns - 1);         /* Float2 = Float + 1, ... */
			}
			if (rest[1] == 'x' && rest[2] >= '2' && rest[2] <= '4' && rest[3] == '\0') {
				unsigned rows = (unsigned)(rest[2] - '0');

				if (bases[i].scalar == MTLDataTypeFloat) {
					return MTLDataTypeFloat2x2 + (columns - 2) * 3 + (rows - 2);
				}
				if (bases[i].scalar == MTLDataTypeHalf) {
					return MTLDataTypeHalf2x2 + (columns - 2) * 3 + (rows - 2);
				}
			}
		}
	}
	return MTLDataTypeNone;
}

@interface AliothMetalArrayType : NSObject {
@public
	NSUInteger elementType, arrayLength, stride;
}
@end

@implementation AliothMetalArrayType
- (NSUInteger)dataType                  { return MTLDataTypeArray; }
- (NSUInteger)elementType               { return elementType; }
- (NSUInteger)arrayLength               { return arrayLength; }
- (NSUInteger)stride                    { return stride; }
- (NSUInteger)argumentIndexStride       { return 0; }
- (id)elementStructType                 { return nil; }
- (id)elementArrayType                  { return nil; }
- (id)elementTextureReferenceType       { return nil; }
- (id)elementPointerType                { return nil; }
@end

@interface AliothMetalStructType : NSObject {
@public
	NSArray *members;                       /* AliothMetalStructMember each */
}
@end

@interface AliothMetalStructMember : NSObject {
@public
	NSString *name;
	NSUInteger offset, dataType;
	AliothMetalStructType *structType;      /* a struct member: empty, its members are not kept */
	AliothMetalArrayType *arrayType;        /* "T[n]" */
}
@end

@implementation AliothMetalStructMember
- (NSString *)name                      { return name; }
- (NSUInteger)offset                    { return offset; }
- (NSUInteger)dataType                  { return dataType; }
- (id)structType                        { return structType; }
- (id)arrayType                         { return arrayType; }
- (id)textureReferenceType              { return nil; }
- (id)pointerType                       { return nil; }
- (NSUInteger)argumentIndex             { return 0; }

- (void)dealloc
{
	[name release];
	[structType release];
	[arrayType release];
	[super dealloc];
}
@end

@implementation AliothMetalStructType
- (NSUInteger)dataType                  { return MTLDataTypeStruct; }
- (NSArray *)members                    { return members; }

- (id)memberByName:(NSString *)name
{
	for (AliothMetalStructMember *m in members) {
		if ([m->name isEqualToString:name]) {
			return m;
		}
	}
	return nil;
}

- (void)dealloc
{
	[members release];
	[super dealloc];
}
@end

/* a buffer's struct, from air.struct_type_info's members */
static AliothMetalStructType *
alioth_struct_type(const struct rwa_struct_member *members, unsigned count)
{
	AliothMetalStructType *type = [[AliothMetalStructType alloc] init];
	NSMutableArray *list = [[NSMutableArray alloc] initWithCapacity:count];

	for (unsigned i = 0; i < count; i++) {
		AliothMetalStructMember *m = [[AliothMetalStructMember alloc] init];
		const char *tn = members[i].type_name ? members[i].type_name : "";
		const char *bracket = strchr(tn, '[');

		m->name = [[NSString alloc] initWithUTF8String:members[i].name ? members[i].name : ""];
		m->offset = members[i].offset;
		if (members[i].is_struct) {
			m->dataType = MTLDataTypeStruct;
			m->structType = [[AliothMetalStructType alloc] init];
		} else if (bracket != NULL) {
			char element[64];
			size_t n = MIN((size_t)(bracket - tn), sizeof(element) - 1);
			unsigned long length = strtoul(bracket + 1, NULL, 10);

			memcpy(element, tn, n);
			element[n] = '\0';
			m->dataType = MTLDataTypeArray;
			m->arrayType = [[AliothMetalArrayType alloc] init];
			m->arrayType->elementType = alioth_data_type(element);
			m->arrayType->arrayLength = length;
			m->arrayType->stride = length != 0 ? members[i].size / length : 0;
		} else {
			m->dataType = alioth_data_type(tn);
		}
		[list addObject:m];
		[m release];
	}
	type->members = list;
	return [type autorelease];
}

@interface AliothMetalArgument : NSObject {
@public
	NSString *name;
	NSUInteger type, access, index, size, align, dataType, textureType, textureDataType, arrayLength;
	BOOL active, depth;
	AliothMetalStructType *structType;
}
@end

@implementation AliothMetalArgument
/* MTLArgument */
- (NSString *)name                      { return name; }
- (NSUInteger)type                      { return type; }     /* MTLArgumentType and MTLBindingType agree */
- (NSUInteger)access                    { return access; }
- (NSUInteger)index                     { return index; }
- (BOOL)isActive                        { return active; }
- (NSUInteger)bufferAlignment           { return align; }
- (NSUInteger)bufferDataSize            { return size; }
- (NSUInteger)bufferDataType            { return dataType; }
- (id)bufferStructType                  { return structType; }
- (id)bufferPointerType                 { return nil; }
- (NSUInteger)threadgroupMemoryAlignment { return align; }
- (NSUInteger)threadgroupMemoryDataSize { return size; }
- (NSUInteger)textureType               { return textureType; }
- (NSUInteger)textureDataType           { return textureDataType; }
- (BOOL)isDepthTexture                  { return depth; }
- (NSUInteger)arrayLength               { return arrayLength; }
/* MTLBinding and its kin, iOS 16 */
- (BOOL)isUsed                          { return active; }
- (BOOL)isArgument                      { return NO; }
- (NSUInteger)objectPayloadAlignment    { return 0; }
- (NSUInteger)objectPayloadDataSize     { return 0; }

- (NSString *)description
{
	return [NSString stringWithFormat:@"<AliothMetalArgument %@: type %lu index %lu%s>", name, (unsigned long)type,
	    (unsigned long)index, active ? "" : ", inactive"];
}

- (void)dealloc
{
	[name release];
	[structType release];
	[super dealloc];
}
@end

static NSArray *
alioth_arguments(const struct rwa_argument *args, unsigned count)
{
	NSMutableArray *list = [NSMutableArray arrayWithCapacity:count];

	for (unsigned i = 0; i < count; i++) {
		const struct rwa_argument *a = &args[i];
		AliothMetalArgument *m = [[AliothMetalArgument alloc] init];
		BOOL half = a->element_bits == 16;

		m->name = [[NSString alloc] initWithUTF8String:a->name ? a->name : ""];
		m->type = a->kind;
		m->access = a->access;
		m->index = a->index;
		m->active = a->active != 0;
		m->size = a->size;
		m->align = a->align;
		m->arrayLength = a->array_length != 0 ? a->array_length : 1;
		m->textureType = a->type_known ? a->texture_type : MTLTextureType2D;
		m->textureDataType = a->element == 1 ? (half ? MTLDataTypeHalf : MTLDataTypeFloat) :
		    a->element == 2 ? (half ? MTLDataTypeShort : MTLDataTypeInt) :
		    a->element == 3 ? (half ? MTLDataTypeUShort : MTLDataTypeUInt) : MTLDataTypeNone;
		m->depth = a->is_depth != 0;
		if (a->kind == MTLArgumentTypeBuffer) {
			m->dataType = alioth_data_type(a->type_name);
			/* a struct: its members when the metadata lists them, an empty struct when it does not */
			if (a->member_count != 0 || (m->dataType == MTLDataTypeNone && a->type_name && a->type_name[0])) {
				m->dataType = MTLDataTypeStruct;
				m->structType = [alioth_struct_type(a->members, a->member_count) retain];
			}
		}
		[list addObject:m];
		[m release];
	}
	return list;
}

@interface AliothMetalReflection : NSObject {
@public
	NSArray *vertex, *fragment;             /* a compute pipeline's arguments are `vertex` */
}
@end

@implementation AliothMetalReflection
- (NSArray *)vertexArguments            { return vertex; }
- (NSArray *)fragmentArguments          { return fragment; }
- (NSArray *)tileArguments              { return @[]; }
- (NSArray *)vertexBindings             { return vertex; }
- (NSArray *)fragmentBindings           { return fragment; }
- (NSArray *)tileBindings               { return @[]; }
- (NSArray *)objectBindings             { return @[]; }
- (NSArray *)meshBindings               { return @[]; }
- (NSArray *)arguments                  { return vertex; }
- (NSArray *)bindings                   { return vertex; }

- (void)dealloc
{
	[vertex release];
	[fragment release];
	[super dealloc];
}
@end

/* The first few reflections of a process written down: what a framework is told it may bind, by name */
static void
alioth_reflection_note(const char *what, const struct rwa_argument *args, unsigned count)
{
	static _Atomic unsigned told;
	char line[640];
	size_t k;

	if (told >= 12) {
		return;
	}
	told++;
	k = (size_t)snprintf(line, sizeof(line), "reflection of %s:", what);
	for (unsigned i = 0; i < count && k < sizeof(line) - 1; i++) {
		static const char *const kinds[] = { "buffer", "threadgroup", "texture", "sampler" };

		k += (size_t)snprintf(line + k, sizeof(line) - k, " %s=%s %u%s", args[i].name ? args[i].name : "?",
		    args[i].kind < 4 ? kinds[args[i].kind] : "?", args[i].index, args[i].active ? "" : " (unused)");
	}
	rw_print("%s\n", line);
}

@implementation AliothMetalComputePipeline

- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor
{
	/* _MTLComputePipelineState's designated initialiser is the one AGX sends to super
	 * (0x2092477e0 -> 0x181a2e068). The descriptor must not be nil: the base asserts on
	 * that at its own line 1504, and the first version of this passed nil and was killed
	 * by it. */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		mesa.compute_destroy(compute);
		return nil;
	}
	_owner = device;
	_compute = compute;
	_info = *info;
	/* and the same for a kernel with a set, for the same reason */
	if (_info.argbuf_mask != 0) {
		RW_ONCE("a compute pipeline declares argument buffers at Metal buffer indices "
			"%#x; their members are bound from what an MTLArgumentEncoder "
			"recorded\n", _info.argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_compute != NULL) {
		mesa.compute_destroy(_compute);
	}
	[super dealloc];
}

- (struct rwa_compute *)compute
{
	return _compute;
}

- (id)aliothReflection
{
	enum { MAX_ARGS = 128 };
	struct rwa_argument *args;
	unsigned count = 0;
	char why[512] = { 0 };

	if (mesa.compute_arguments == NULL) {
		RW_ONCE("pipeline reflection was asked for and this compiler library has no rwa_compute_arguments: nil\n");
		return nil;
	}
	if ((args = calloc(MAX_ARGS, sizeof(*args))) == NULL) {
		return nil;
	}
	if (mesa.compute_arguments(_compute, args, MAX_ARGS, &count, why, sizeof(why)) != 0) {
		rw_print("pipeline reflection: %s\n", why);
		free(args);
		return nil;
	}
	alioth_reflection_note("a kernel", args, count);
	AliothMetalReflection *reflection = [[AliothMetalReflection alloc] init];

	reflection->vertex = [alioth_arguments(args, count) retain];
	reflection->fragment = [[NSArray alloc] init];
	free(args);
	return [reflection autorelease];
}

/* The three the base class answers with nothing at all (docs/metal.md 10.8), so they can
 * only come from whoever compiled the shader -- which is why the contract carries them. */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _info.max_threads_per_group; }
- (NSUInteger)threadExecutionWidth          { return _info.thread_execution_width; }
- (NSUInteger)staticThreadgroupMemoryLength { return _info.static_shared_bytes; }

- (id)device { return _owner; }

@end

@implementation AliothMetalRenderPipeline

- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
			       render:(struct rwa_render *)render
				 info:(const struct rwa_render_info *)info
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilyRenderPipeline sends to super (0x181a294dc), and a real
	 * descriptor, for the reason the compute one learned at its own line 1504 */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		mesa.render_destroy(render);
		return nil;
	}
	_owner = device;
	_render = render;
	_info = *info;
	if (info->generated_vertex_stage) {
		rw_print("this pipeline's vertex stage was generated, so what it "
		         "draws is the fragment shader over the whole attachment\n");
	}
	/* The first pipeline of this process whose shaders declare an ARGUMENT BUFFER, named
	 * once: every measurement this port has of argument buffers is on the host, and this
	 * line is how a boot says a pipeline with a set was built at all. The indices matter
	 * because the draw binds nothing of that set out of the buffer table -- it binds what
	 * the encoder recorded for whatever buffer is bound there. */
	if (info->vertex_argbuf_mask != 0 || info->fragment_argbuf_mask != 0) {
		RW_ONCE("a render pipeline declares argument buffers: the vertex stage at "
			"Metal buffer indices %#x and the fragment stage at %#x. Their members "
			"are bound from what an MTLArgumentEncoder recorded, and a buffer "
			"bound there that nobody encoded is refused by name\n",
			info->vertex_argbuf_mask, info->fragment_argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_render != NULL) {
		mesa.render_destroy(_render);
	}
	[super dealloc];
}

- (struct rwa_render *)render { return _render; }

- (id)aliothReflection
{
	enum { MAX_ARGS = 128 };
	struct rwa_argument *args[2];
	unsigned count[2] = { 0, 0 };
	char why[512] = { 0 };
	AliothMetalReflection *reflection = nil;

	if (mesa.render_arguments == NULL) {
		RW_ONCE("pipeline reflection was asked for and this compiler library has no rwa_render_arguments: nil\n");
		return nil;
	}
	args[0] = calloc(MAX_ARGS, sizeof(struct rwa_argument));
	args[1] = calloc(MAX_ARGS, sizeof(struct rwa_argument));
	if (args[0] == NULL || args[1] == NULL) {
		goto out;
	}
	for (unsigned stage = 0; stage < 2; stage++) {
		if (mesa.render_arguments(_render, stage, args[stage], MAX_ARGS, &count[stage], why, sizeof(why)) != 0) {
			rw_print("pipeline reflection: %s\n", why);
			goto out;
		}
	}
	alioth_reflection_note("a vertex function", args[0], count[0]);
	alioth_reflection_note("a fragment function", args[1], count[1]);
	reflection = [[[AliothMetalReflection alloc] init] autorelease];
	reflection->vertex = [alioth_arguments(args[0], count[0]) retain];
	reflection->fragment = [alioth_arguments(args[1], count[1]) retain];
out:
	free(args[0]);
	free(args[1]);
	return reflection;
}

- (BOOL)writesDepth           { return _info.fragment_writes_depth != 0; }
- (unsigned)vertexStreamMask  { return _info.vertex_stream_mask; }
- (unsigned)vertexStreamCount { return _info.vertex_streams; }
- (unsigned)vertexElementCount { return _info.vertex_attribute_count; }
- (unsigned)vertexElementAttribute:(unsigned)index
{
	unsigned n = sizeof(_info.vertex_attribute_index) /
		     sizeof(_info.vertex_attribute_index[0]);

	/* ~0u and not 0 for an index past the end: attribute 0 is a real answer, and a
	 * caller that asked for element 5 of a pipeline with two would otherwise read the
	 * mapping as if it had five. */
	return index < _info.vertex_attribute_count && index < n
	    ? _info.vertex_attribute_index[index] : ~0u;
}
- (id)device                  { return _owner; }

/* The base class answers this one with `mov x0, #0; ret` (0x181b0eee0), and a zero would read
 * as "no threads"; the number is the one the compiler library reports for a compiled stage. */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (BOOL)threadgroupSizeMatchesTileSize      { return NO; }
- (NSUInteger)imageblockSampleLength        { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions
{
	(void)dimensions;
	return 0;
}

/* The last two of <MTLRenderPipelineStateSPI> that nothing in this chain answered. Shader
 * debug information is what Metal's own debugger reads out of a pipeline built with it;
 * there is none here, and nil is the answer Apple's driver gives for a pipeline that was
 * built without it, so the caller already has this path. */
- (id)newVertexShaderDebugInfo   { return nil; }
- (id)newFragmentShaderDebugInfo { return nil; }

@end

@implementation AliothMetalWork
@end

/*
 * THE SLOT EVERY COMMAND BUFFER TAKES FROM THE QUEUE, AND WHY IT HAS TO BE GIVEN BACK.
 *
 * -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] -- the
 * designated initialiser this driver's command buffer calls, because docs/metal.md 10.13
 * says to call the real one -- does this, at 0x181a1ea14 and on:
 *
 *     ldr  x8, [x19, _MTLCommandBuffer._queue]
 *     ldr  x0, [x8, _MTLCommandQueue._commandBufferSemaphore]
 *     mov  x1, #-1                       ; DISPATCH_TIME_FOREVER
 *     bl   dispatch_semaphore_wait
 *
 * and just before it sets _MTLCommandBuffer._needsCommandBufferSemaphoreSignal to 1. The
 * semaphore is made in -[_MTLCommandQueue initWithDevice:descriptor:] (0x181a1d1fc), which
 * reads maxCommandBufferCount off the descriptor (0x181a1d268) and refuses zero; the
 * default for MTLCommandQueueDescriptor is 64. Every ivar name above is the cache's own
 * (`ipsw dyld a2s`: OBJC_IVAR_$__MTLCommandQueue._commandBufferSemaphore,
 * OBJC_IVAR_$__MTLCommandBuffer._needsCommandBufferSemaphoreSignal), not a guess.
 *
 * So each command buffer takes one of 64 slots AND OWES A SIGNAL -- that ivar is Apple's
 * own name for the debt. In Apple's driver something on the completion path pays it. This
 * driver has no completion path at all: it overrides -commit and -waitUntilCompleted and
 * never calls -enqueueCommandBuffer:, -commitCommandBuffer:wake: or any of the rest, so
 * nothing in Metal will ever pay it for us. The 65th -commandBuffer then waits for ever.
 *
 * MEASURED, not reasoned: logs/114823-run.log. A test that made 64 command buffers (one
 * per shader of an 8x8 mosaic) blocked on the 65th inside -[MTLCommandQueue commandBuffer]
 * and stayed blocked for four minutes, with a heartbeat thread proving the process alive.
 * It is not a test bug: an application makes a command buffer per frame, so this driver
 * would hang anything that drew for a couple of seconds.
 *
 * The debt is paid at -commit, and that is a choice with a reason. Metal holds the slot
 * until the buffer completes; here -commit IS the completion -- the recorded work runs
 * inside it and -waitUntilCompleted only fences -- so holding it any longer would throttle
 * an application for a wait this driver does not have. A buffer that is never committed
 * pays at -dealloc instead, which is the case Apple's flag exists for.
 *
 * Both are done through the runtime by ivar NAME, and a name that is not there is said out
 * loud rather than worked around: on an iOS whose Metal keeps this differently, the right
 * answer is to find out, not to run with a queue that silently blocks at 64.
 */
static dispatch_semaphore_t
alioth_queue_semaphore(id queue)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_commandBufferSemaphore")
				   : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no "
			         "_commandBufferSemaphore in this Metal, so the slot every "
			         "command buffer takes cannot be given back and this queue "
			         "will block on its maxCommandBufferCount'th buffer\n");
		}
	}
	return ivar != NULL ? (dispatch_semaphore_t)object_getIvar(queue, ivar) : nil;
}

/* Where a queue's completion queue lives: Apple's own _MTLCommandQueue._completionQueue,
 * found by NAME for the same reason the semaphore above is -- the base class already has
 * the getter for it, and a second copy in this file would mean a queue built from a
 * descriptor that carried one reading back as nil. A Metal without the ivar is said out
 * loud once and falls back to this driver's own field. */
static Ivar
alioth_completion_ivar(void)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_completionQueue") : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no _completionQueue in this "
			         "Metal, so -setCompletionQueue: keeps the queue in this driver's "
			         "own field instead of the one Apple's getter reads\n");
		}
	}
	return ivar;
}

/* Apple's own "this buffer still owes a signal" flag, cleared when we pay, so that its
 * bookkeeping and ours cannot disagree about a debt that has been settled. */
static void
alioth_clear_semaphore_debt(id commandBuffer)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandBuffer");

		looked = YES;
		ivar = base != Nil ?
		    class_getInstanceVariable(base, "_needsCommandBufferSemaphoreSignal") : NULL;
	}
	if (ivar != NULL) {
		*(unsigned char *)((char *)commandBuffer + ivar_getOffset(ivar)) = 0;
	}
}

@implementation AliothMetalCommandQueue {
	AliothMetalDevice *_owner;
	NSUInteger _outstanding;        /* buffers of this queue that still hold a slot */
	NSUInteger _made, _returned;
	/* What <MTLCommandQueueSPI> keeps about a queue and this driver cannot act on: the
	 * two priorities are remembered so that a caller reading back what it set is not
	 * lied to, and -setCompletionQueue: explains the third below. */
	NSUInteger _gpuPriority, _backgroundGPUPriority;
	dispatch_queue_t _completionQueue;   /* only used when Apple's own ivar is missing */
	dispatch_queue_t _waitQueue;         /* (the K40's) where this queue's buffers wait for the GPU, in order */
}

- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device descriptor:(id)descriptor
{
	/* The initialiser IOGPUMetalCommandQueue itself sends to super (0x1ec16dabc). The
	 * descriptor is not optional: the base validates it at its own line 137 and the first
	 * version of this passed nil and was killed by that assertion. */
	self = [super initWithDevice:device descriptor:descriptor];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	/* Said at birth and once: this number is how many command buffers may exist at a
	 * time before -commandBuffer blocks, and until this driver returned the slot it was
	 * also how many this process could ever make. */
	rw_print("this queue takes %lu command buffers at a time "
	         "(maxCommandBufferCount), and each one waits on the queue's semaphore in "
	         "-[_MTLCommandBuffer initWithQueue:...]; this driver gives the slot back at "
	         "-commit%s\n", (unsigned long)[self maxCommandBufferCount],
	         alioth_queue_semaphore(self) != nil ? "" :
	         ", EXCEPT that the semaphore was not found and it cannot");
	return self;
}

- (id)device { return _owner; }

/* What the queue itself thinks, for a test that wants the number rather than the
 * inference: how many command buffers of this queue still hold one of those slots. */
- (NSUInteger)aliothOutstandingCommandBuffers { return _outstanding; }
- (NSUInteger)aliothCommandBuffersMade        { return _made; }

- (void)aliothTookSlot
{
	_outstanding++;
	_made++;
}

- (void)aliothReturnedSlot
{
	if (_outstanding > 0) {
		_outstanding--;
	}
	_returned++;
}

- (id)commandBuffer
{
	return [[[AliothMetalCommandBuffer alloc] initWithAliothQueue:self] autorelease];
}

- (id)commandBufferWithUnretainedReferences
{
	return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary { }

/*
 * THE TWO METHODS THE SYSTEM NAMED, AND WHAT THEY REALLY ARE.
 *
 * With metal=1 and launchd running, backboardd and mediaserverd sent this queue
 * -setGPUPriority: 58 times and -setCompletionQueue: 30 times in a hundred seconds, and
 * every one of them killed the daemon that sent it (logs/130132-run.log). Neither is in any
 * public header: both are required members of <MTLCommandQueueSPI>, which is declared in
 * AGXMetalA11.bundle and therefore does not exist in any process on this phone -- so
 * objc_getProtocol cannot be asked what this class owes, and the list was read out of the
 * cache instead (tools/metal/protometh.py, tools/metal/owes.py).
 *
 * -setGPUPriority: (B24@0:8Q16 -- NSUInteger in, BOOL out) is NOT bookkeeping.
 * -[IOGPUMetalCommandQueue setGPUPriority:] (0x1ec174cb8) calls _validateGPUPriority
 * (0x1ec178f74) and then -_setGPUPriority:backgroundPriority: (0x1ec178eec), which packs
 * the foreground and background numbers into a three-word request and hands it to the
 * kernel's own command-queue object (_IOGPUCommandQueueSetPriorityAndBackground); only if
 * the kernel accepts does it write the two ivars that -getGPUPriority reads back. So the
 * number is read by the GPU's scheduler, and the ivar is just a cache of what the scheduler
 * agreed to. The validation is worth copying exactly: anything above 5 is refused and 3 is
 * refused for a foreground queue -- "priority (%d) is not valid for foreground" is the
 * cache's own string -- and it only REPORTS, it does not stop the call, which is why
 * Apple's BOOL is the kernel's answer and not the validator's.
 *
 * This driver has one ring and one priority. Every command buffer goes to the kernel's
 * single msm ring in the order it was committed, and there is no second queue to be ahead
 * of; so there is nothing here to tell, and the honest answer is to remember the number,
 * report it back, and say once that it changes nothing.
 */
- (BOOL)setGPUPriority:(NSUInteger)priority
{
	if (priority > 5 || priority == 3) {
		RW_ONCE("GPU priority %lu is outside what Apple's own "
			"_validateGPUPriority allows (0..5, and 3 is the background-only "
			"one); it is remembered as asked, like Apple's own path, which "
			"reports such a value and carries on\n", (unsigned long)priority);
	}
	RW_ONCE("a GPU priority of %lu was asked for; this driver has one ring "
		"and one priority, so the number is remembered and reported back but nothing "
		"schedules by it\n", (unsigned long)priority);
	_gpuPriority = priority;
	return YES;
}

/* The offset is a place WITHIN a priority band (Apple's second argument is an unsigned
 * short, S24 in the encoding). With one ring there is no band to be placed in. */
- (BOOL)setGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	if (offset != 0) {
		RW_ONCE("a GPU priority offset of %u is ignored: this driver has "
			"one ring, so a place inside a priority band means nothing here\n",
			(unsigned)offset);
	}
	return [self setGPUPriority:priority];
}

/* The background priority is the one a queue gets while its process is not frontmost. Same
 * answer, kept separately so that the two reads do not lie about each other. */
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
	RW_ONCE("a background GPU priority of %lu was asked for; remembered, "
		"not honoured, for the same reason as the foreground one\n",
		(unsigned long)priority);
	_backgroundGPUPriority = priority;
	return YES;
}

- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	(void)offset;
	return [self setBackgroundGPUPriority:priority];
}

- (NSUInteger)getGPUPriority           { return _gpuPriority; }
- (NSUInteger)getBackgroundGPUPriority { return _backgroundGPUPriority; }

/*
 * -setCompletionQueue: (v24@0:8@16) names the dispatch queue on which this command queue
 * delivers what finishes. -[IOGPUMetalCommandQueue setCompletionQueue:] (0x1ec178e88)
 * releases the queue it held, retains the new one into _MTLCommandQueue._completionQueue --
 * the ivar the base class already has a getter for -- and re-targets the queue's own
 * dispatch source at it (0x1ec1765e8), so that every command buffer's completion handler
 * runs there instead of on whichever thread the kernel's notification arrived on. That is
 * exactly why backboardd sets one: a completion handler that runs on a daemon's render
 * thread is a completion handler that can stall a frame.
 *
 * So this one is SERVED, not declined: the queue is kept, and -[AliothMetalCommandBuffer
 * commit] delivers the scheduled and completed handlers on it (see the note there -- before
 * this they ran inline, at the moment they were added, which is before the work they are
 * about). It is kept in Apple's ivar rather than in one of ours so that a queue built from
 * a descriptor that already carried a completion queue (there is a
 * -[MTLCommandQueueDescriptorInternal setCompletionQueue:]) reads back as what it is; the
 * retain is balanced by -[_MTLCommandQueue dealloc], the ivar's owner.
 */
- (void)setCompletionQueue:(dispatch_queue_t)queue
{
	Ivar ivar = alioth_completion_ivar();
	dispatch_queue_t held = ivar != NULL ?
	    (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;

	if (held == queue) {
		return;
	}
	[queue retain];
	[held release];
	if (ivar != NULL) {
		object_setIvar(self, ivar, queue);
	} else {
		_completionQueue = queue;
	}
}

/* What this driver reads when it has something to deliver. Not an override of -completionQueue
 * (the base class's getter reads the same ivar): this is here for the Metal where that ivar
 * is not, so that one missing ivar does not quietly send every handler to the wrong thread. */
- (dispatch_queue_t)aliothCompletionQueue
{
	Ivar ivar = alioth_completion_ivar();

	return ivar != NULL ? (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;
}

/* (the K40's) One serial queue per command queue, made when first wanted: its buffers' GPU waits and completed
 * handlers run there one after another, which is Metal's promise -- a queue's buffers complete in commit order. */
- (dispatch_queue_t)aliothWaitQueue
{
	@synchronized (self) {
		if (_waitQueue == nil) {
			_waitQueue = dispatch_queue_create("aliothmetal.completion", DISPATCH_QUEUE_SERIAL);
		}
		return _waitQueue;
	}
}

- (void)dealloc
{
	/* Only the copy this driver keeps itself: the one in Apple's ivar belongs to
	 * -[_MTLCommandQueue dealloc], and releasing it twice would be worse than leaking
	 * one dispatch queue in the Metal where that dealloc does not. */
	[_completionQueue release];
	if (_waitQueue != nil) {
		dispatch_release(_waitQueue);
	}
	[super dealloc];
}

@end

/*
 * MTLCommandBufferStatus, AND WHAT A LOG FROM BEFORE THIS CHANGE SAYS.
 *
 * These are Metal's own numbers (MTLCommandBuffer.h): NotEnqueued 0, Enqueued 1, Committed 2,
 * Scheduled 3, Completed 4, Error 5. -[MTLCommandBuffer status] is how real code decides that a
 * buffer is done, so they are not cosmetic.
 *
 * Until 2026-10-21 this file answered 3 for committed, 4 for scheduled and 6 for completed -- and
 * 6 is not a status at all. Under those numbers an application comparing with
 * MTLCommandBufferStatusCompleted (4) saw completion the moment -commit returned, BEFORE the GPU
 * fence of -waitUntilCompleted was paid, and after the fence it saw a number Metal does not
 * define. Error was 5 by coincidence and has not moved.
 *
 * Every boot log under logs/ was written by the old numbering and none of them is going to be
 * regenerated, so those numbers have to be DATED rather than reinterpreted. That is what the
 * banner in -initWithAliothQueue: below is for: it names the numbering of the log that carries
 * it, and it is printed out of these constants, so the line cannot drift from what -status
 * answers. A log without that line is older than this change, and in it "status 6" is this
 * completion, "status 4" is a buffer that was only submitted, and 2 and 3 do not occur at all.
 * That is the rule tools/air/metalcheck.py reads a log by, and the measurement that makes it
 * safe: over all 141 logs in logs/ the only statuses the test ever printed are 5 (128 times) and
 * 6 (990 times) -- never 4.
 *
 * One file more keys on the old numbers and is NOT part of this change: userland/src/
 * metal_drv_test.m compares [cmd status] with the literal 6 in five places (its lines 1200, 1526,
 * 2041, 2293, 2795) and prints the literal "status 6" in a sixth (2308). Until those move, a boot
 * with this driver has the test calling every completed buffer a failure: the sweep prints
 * "drvtest: drawall <name> status 4" with nothing after it and reads back no pixel, and the single
 * draw returns before its own readback. metalcheck.py names that shape when it sees it.
 */
enum {
	RW_CB_NOT_ENQUEUED = 0,
	RW_CB_ENQUEUED     = 1,
	RW_CB_COMMITTED    = 2,
	RW_CB_SCHEDULED    = 3,
	RW_CB_COMPLETED    = 4,
	RW_CB_ERROR        = 5,
};

@implementation AliothMetalCommandBuffer {
	AliothMetalCommandQueue *_queue;
	NSMutableArray *_work;
	NSError *_error;
	NSUInteger _status;
	BOOL _owesSlot;                 /* this buffer holds one of the queue's slots */
	NSMutableArray *_scheduled, *_completed;   /* handler blocks, copied to the heap */
	NSUInteger _protectionOptions;  /* what the window server asked for; see -setProtectionOptions: */
	dispatch_group_t _done;         /* (the K40's) left when the GPU wait off this thread is over */
	uint64_t _created;              /* (the K40's) mach time it was made, for metal_stats=1 */
	NSMutableArray *_signals;       /* (the K40's) its events' signals, set once the GPU has finished it */
}

/*
 * The window server sends this to every command buffer it makes, and until it was answered the
 * boot of 22 October 01:51 ended it 78 times with
 *
 *   NSInvalidArgumentException: -[AliothMetalCommandBuffer setProtectionOptions:]:
 *   unrecognized selector sent to instance
 *
 * -- an uncaught Objective-C exception out of CA::WindowServer::IOMFBServer::thread_body, which
 * is terminate() and not a refusal we could read.
 *
 * It was missed by owes.py for a reason worth keeping: that tool counts what the PROTOCOLS
 * require, and this pair is declared on a CLASS -- IOGPUMetalCommandBuffer, whose own
 * _protectionOptions ivar it is. Our command buffer descends from _MTLCommandBuffer and not
 * from IOGPU's, so nothing in the chain answers it and nothing in the protocol lists says it is
 * missing. Every selector Apple's own counterpart class declares is a selector a caller may
 * send, and the protocols are only part of that list.
 *
 * The value is kept and not honoured. Protected content means memory the GPU can read and the
 * processor cannot, which this port has nothing to provide: a buffer of ours is ordinary memory
 * on both sides. So a non-zero request says so once rather than being refused -- the window
 * server asks for it on buffers that carry no protected content at all, and refusing would stop
 * a composite that is in no danger.
 */
- (void)setProtectionOptions:(NSUInteger)options
{
	if (options != 0 && _protectionOptions != options) {
		RW_ONCE("a command buffer was asked for protection options %lu, and this "
			"driver has no protected memory to give: the request is kept and not honoured, "
			"because nothing it would protect is protected here either\n",
			(unsigned long)options);
	}
	_protectionOptions = options;
}

- (NSUInteger)protectionOptions
{
	return _protectionOptions;
}

- (instancetype)initWithAliothQueue:(AliothMetalCommandQueue *)queue
{
	/* what IOGPUMetalCommandBuffer sends to super, retained references and no debug mode.
	 * THIS CALL BLOCKS when the queue's semaphore is empty -- see the long note above
	 * AliothMetalCommandQueue -- so by the time it returns, a slot is held and owed. */
	self = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];
	if (self == nil) {
		return nil;
	}
	_queue = [queue retain];
	_work = [[NSMutableArray alloc] init];
	/* The numbering this log is written in, said once per process and printed out of the
	 * constants that answer -status: a log that does NOT carry this line was written before
	 * the numbers became Metal's, and says committed 3, scheduled 4, completed 6 (see the
	 * note above this class). */
	RW_ONCE("MTLCommandBufferStatus: committed %u, scheduled %u, completed %u, "
		"error %u -- Metal's own numbers; a log without this line was written by the old "
		"ones (committed 3, scheduled 4, completed 6)\n",
		(unsigned)RW_CB_COMMITTED, (unsigned)RW_CB_SCHEDULED,
		(unsigned)RW_CB_COMPLETED, (unsigned)RW_CB_ERROR);
	_status = RW_CB_ENQUEUED;   /* nothing is pending yet */
	_owesSlot = YES;
	[queue aliothTookSlot];
	_created = mach_absolute_time();
	return self;
}

/*
 * Give the queue's slot back, exactly once. Called at the end of -commit, where this
 * driver's work is already done, and at -dealloc for a buffer that was never committed --
 * which is the case Apple's _needsCommandBufferSemaphoreSignal exists for and the one that
 * would otherwise lose a slot for the life of the process.
 */
- (void)aliothReturnSlot
{
	dispatch_semaphore_t semaphore;

	if (!_owesSlot) {
		return;
	}
	_owesSlot = NO;
	alioth_clear_semaphore_debt(self);
	[_queue aliothReturnedSlot];
	semaphore = alioth_queue_semaphore(_queue);
	if (semaphore != nil) {
		dispatch_semaphore_signal(semaphore);
	}
}

- (void)dealloc
{
	[self aliothReturnSlot];
	[_work release];
	[_error release];
	[_scheduled release];
	[_completed release];
	[_signals release];
	[_queue release];
	if (_done != nil) {
		dispatch_release(_done);
	}
	[super dealloc];
}

- (id)device       { return [_queue device]; }
- (id)commandQueue { return _queue; }
- (NSUInteger)status { return _status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }

- (id)computeCommandEncoder
{
	return [[[AliothMetalComputeEncoder alloc] initWithAliothCommandBuffer:self]
		   autorelease];
}

- (id)renderCommandEncoderWithDescriptor:(id)descriptor
{
	if (descriptor == nil) {
		rw_print("a render pass needs a descriptor\n");
		return nil;
	}
	return [[[AliothMetalRenderEncoder alloc] initWithAliothCommandBuffer:self
								    descriptor:descriptor]
		   autorelease];
}

- (id)parallelRenderCommandEncoderWithDescriptor:(id)descriptor
{
	(void)descriptor;
	return nil;   /* one encoder at a time here; a parallel pass needs the split */
}

/* The rest of what MTLCommandBuffer requires of a driver. Each declines rather than being
 * absent, for the reason the device's own 44 were answered: a missing selector aborts the
 * caller, and these are the four that were still missing. */
- (id)blitCommandEncoder
{
	return [[[AliothMetalBlitEncoder alloc] initWithAliothCommandBuffer:self] autorelease];
}

- (id)blitCommandEncoderWithDescriptor:(id)descriptor
{
	(void)descriptor;       /* (its sample buffer attachments: no counters here) */
	return [self blitCommandEncoder];
}

- (id)resourceStateCommandEncoder { return nil; }
- (id)accelerationStructureCommandEncoder { return nil; }
/* (the K40's) an event's signal or wait, in the command buffer's order (see AliothMetalFence) */
- (void)recordEvent:(id)event value:(uint64_t)value wait:(BOOL)wait
{
	if (![event isKindOfClass:[AliothMetalEvent class]]) {
		RW_ONCE("a command buffer was given an event this driver did not make; it is not signalled or waited for\n");
		return;
	}
	AliothMetalSignal *s = [[AliothMetalSignal alloc] init];

	s->event = [event retain];
	s->value = value;
	s->wait = wait;
	[_work addObject:s];
	[s release];
}

- (void)encodeSignalEvent:(id)event value:(uint64_t)value  { [self recordEvent:event value:value wait:NO]; }
- (void)encodeWaitForEvent:(id)event value:(uint64_t)value { [self recordEvent:event value:value wait:YES]; }

- (id)computeCommandEncoderWithDispatchType:(NSUInteger)type
{
	(void)type;   /* serial and concurrent dispatch are the same here: one queue, in order */
	return [self computeCommandEncoder];
}

- (void)recordWork:(AliothMetalWork *)work
{
	[_work addObject:work];
}

- (void)recordBlit:(id)blit
{
	[_work addObject:blit];
}

/* the bytes of one texel of the formats the blit path copies between textures; 0: not one of them */
static NSUInteger
alioth_texel_bytes(NSUInteger format)
{
	switch (format) {
	case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB:
	case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
	case MTLPixelFormatStencil8:
		return 1;
	case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm:
	case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Float: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
	case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm: case MTLPixelFormatABGR4Unorm:
	case MTLPixelFormatBGR5A1Unorm: case MTLPixelFormatDepth16Unorm:
		return 2;
	case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm:
	case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm:
	case MTLPixelFormatBGRA8Unorm_sRGB: case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatBGR10A2Unorm:
	case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float: case MTLPixelFormatR32Float:
	case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG16Float: case MTLPixelFormatRG16Unorm:
	case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatDepth32Float:
		return 4;
	case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Uint:
	case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRG32Float: case MTLPixelFormatRG32Uint:
	case MTLPixelFormatRG32Sint:
		return 8;
	case MTLPixelFormatRGBA32Float: case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
		return 16;
	default:
		return 0;
	}
}

/* a texture's region through the processor, slice by slice of a 3D region: what texture_read/write take is 2D */
static void
alioth_texture_out(id texture, void *bytes, NSUInteger stride, NSUInteger image, MTLOrigin origin, MTLSize size,
    NSUInteger level, NSUInteger slice)
{
	for (NSUInteger z = 0; z < MAX(size.depth, 1); z++) {
		[texture getBytes:(char *)bytes + z * image bytesPerRow:stride bytesPerImage:image
		       fromRegion:MTLRegionMake3D(origin.x, origin.y, origin.z + z, size.width, size.height, 1)
		      mipmapLevel:level slice:slice];
	}
}

static void
alioth_texture_in(id texture, const void *bytes, NSUInteger stride, NSUInteger image, MTLOrigin origin, MTLSize size,
    NSUInteger level, NSUInteger slice)
{
	for (NSUInteger z = 0; z < MAX(size.depth, 1); z++) {
		[texture replaceRegion:MTLRegionMake3D(origin.x, origin.y, origin.z + z, size.width, size.height, 1)
			   mipmapLevel:level slice:slice withBytes:(const char *)bytes + z * image bytesPerRow:stride
			 bytesPerImage:image];
	}
}

/* the next level of a mipmap chain from the one above it, a 2x2 box of each channel byte (8-bit formats only) */
static void
alioth_half_texels(const uint8_t *above, NSUInteger aw, NSUInteger ah, uint8_t *below, NSUInteger bw, NSUInteger bh,
    NSUInteger bpp)
{
	for (NSUInteger y = 0; y < bh; y++) {
		for (NSUInteger x = 0; x < bw; x++) {
			NSUInteger x0 = MIN(2 * x, aw - 1), x1 = MIN(2 * x + 1, aw - 1);
			NSUInteger y0 = MIN(2 * y, ah - 1), y1 = MIN(2 * y + 1, ah - 1);

			for (NSUInteger c = 0; c < bpp; c++) {
				unsigned sum = above[(y0 * aw + x0) * bpp + c] + above[(y0 * aw + x1) * bpp + c] +
				    above[(y1 * aw + x0) * bpp + c] + above[(y1 * aw + x1) * bpp + c];

				below[(y * bw + x) * bpp + c] = (uint8_t)((sum + 2) / 4);
			}
		}
	}
}

/* One blit record, by the processor (see AliothMetalBlit). NO and the reason when it cannot be done. */
- (BOOL)runBlit:(AliothMetalBlit *)b why:(char *)why size:(size_t)size
{
	struct rwa_device *device = [[_queue device] mesaDevice];

	alioth_stat(ST_BLIT, mach_absolute_time());
	/* what came before it in this buffer, finished: the processor reads and writes here */
	if (device != NULL && alioth_finish(device, why, size) != 0) {
		[self failWith:why];
		return NO;
	}
	switch (b->kind) {
	case ALIOTH_BLIT_RESOLVE:
		if (mesa.texture_resolve == NULL) {
			snprintf(why, size, "a multisample resolve, and this compiler library has no rwa_texture_resolve");
			[self failWith:why];
			return NO;
		}
		if (mesa.texture_resolve(device, [b->source mesaTexture], [b->destination mesaTexture], why, size) != 0) {
			[self failWith:why];
			return NO;
		}
		return YES;
	case ALIOTH_BLIT_FILL:
		memset((char *)[(id<MTLBuffer>)b->destination contents] + b->destinationOffset, b->value, b->length);
		return YES;
	case ALIOTH_BLIT_BUFFER_TO_BUFFER:
		memmove((char *)[(id<MTLBuffer>)b->destination contents] + b->destinationOffset,
			(const char *)[(id<MTLBuffer>)b->source contents] + b->sourceOffset, b->length);
		return YES;
	case ALIOTH_BLIT_BUFFER_TO_TEXTURE:
		alioth_texture_in(b->destination, (const char *)[(id<MTLBuffer>)b->source contents] + b->sourceOffset,
				  b->sourceBytesPerRow, b->sourceBytesPerImage, b->destinationOrigin, b->size,
				  b->destinationLevel, b->destinationSlice);
		return YES;
	case ALIOTH_BLIT_TEXTURE_TO_BUFFER:
		alioth_texture_out(b->source, (char *)[(id<MTLBuffer>)b->destination contents] + b->destinationOffset,
				   b->destinationBytesPerRow, b->destinationBytesPerImage, b->sourceOrigin, b->size,
				   b->sourceLevel, b->sourceSlice);
		return YES;
	case ALIOTH_BLIT_TEXTURE_TO_TEXTURE: {
		NSUInteger bpp = alioth_texel_bytes([(id<MTLTexture>)b->source pixelFormat]);
		NSUInteger stride = b->size.width * bpp, image = stride * b->size.height;
		void *staging;

		if (bpp == 0) {
			snprintf(why, size, "a texture-to-texture copy of pixel format %lu, whose texels this driver "
				 "does not know the size of", (unsigned long)[(id<MTLTexture>)b->source pixelFormat]);
			[self failWith:why];
			return NO;
		}
		if ((staging = malloc(image * MAX(b->size.depth, 1))) == NULL) {
			snprintf(why, size, "no memory for a texture copy of %lu bytes",
				 (unsigned long)(image * MAX(b->size.depth, 1)));
			[self failWith:why];
			return NO;
		}
		alioth_texture_out(b->source, staging, stride, image, b->sourceOrigin, b->size, b->sourceLevel,
				   b->sourceSlice);
		alioth_texture_in(b->destination, staging, stride, image, b->destinationOrigin, b->size,
				  b->destinationLevel, b->destinationSlice);
		free(staging);
		return YES;
	}
	case ALIOTH_BLIT_MIPMAPS: {
		id<MTLTexture> texture = b->destination;
		NSUInteger bpp = alioth_texel_bytes([texture pixelFormat]), w = [texture width], h = [texture height];
		NSUInteger levels = [texture mipmapLevelCount], format = [texture pixelFormat];
		uint8_t *above, *below;

		/* (a box of bytes is right for 8-bit channels only; sRGB is averaged as stored, a small error) */
		if (bpp == 0 || format == MTLPixelFormatRGBA16Float || format == MTLPixelFormatR16Float ||
		    format == MTLPixelFormatRG16Float || format == MTLPixelFormatR32Float || format == MTLPixelFormatRGBA32Float ||
		    format == MTLPixelFormatRG32Float || format == MTLPixelFormatB5G6R5Unorm || format == MTLPixelFormatDepth32Float) {
			RW_ONCE("mipmaps of pixel format %lu are not made: this driver averages 8-bit channels only\n",
				(unsigned long)format);
			return YES;
		}
		if ((above = malloc(w * h * bpp)) == NULL) {
			snprintf(why, size, "no memory for a mipmap chain");
			[self failWith:why];
			return NO;
		}
		alioth_texture_out(texture, above, w * bpp, w * h * bpp, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), 0, 0);
		for (NSUInteger level = 1; level < levels && (w > 1 || h > 1); level++) {
			NSUInteger bw = MAX(w / 2, 1), bh = MAX(h / 2, 1);

			if ((below = malloc(bw * bh * bpp)) == NULL) {
				break;
			}
			alioth_half_texels(above, w, h, below, bw, bh, bpp);
			alioth_texture_in(texture, below, bw * bpp, bw * bh * bpp, MTLOriginMake(0, 0, 0),
					  MTLSizeMake(bw, bh, 1), level, 0);
			free(above);
			above = below;
			w = bw;
			h = bh;
		}
		free(above);
		return YES;
	}
	}
	return YES;
}

- (void)enqueue { }

/* Metal's own rule, kept: the recorded dispatches run here and not when they were encoded. */
- (void)commit
{
	char why[512] = { 0 };
	uint64_t committed = mach_absolute_time();

	alioth_stat(ST_ENCODE, _created);
	_status = RW_CB_COMMITTED;
	for (id item in _work) {
		AliothMetalWork *work = item;

		if ([item isKindOfClass:[AliothMetalBlit class]]) {
			if (![self runBlit:item why:why size:sizeof(why)]) {
				rw_print("the blit failed: %s\n", why);
				goto done;
			}
			continue;
		}
		if ([item isKindOfClass:[AliothMetalSignal class]]) {
			AliothMetalSignal *s = item;
			struct rwa_device *device = [[_queue device] mesaDevice];

			if (s->wait) {
				uint64_t now;

				@synchronized (s->event) {
					now = s->event->value;
				}
				if (now < s->value) {
					RW_ONCE("a command buffer waits for an event value that is not signalled yet; this "
						"driver runs a buffer's work at -commit and goes on without it\n");
				}
				continue;
			}
			/*
			 * What came before the signal, finished on the GPU: that is what the value says. (The K40's) It was
			 * waited for HERE, which makes the GPU and the processor take turns for a renderer that signals an
			 * event at every frame's end (VectorKit's buffers waited inside -commit once each, 3 ms, 186 in 2 s --
			 * signals or blits; metal_stats=1 now counts the two apart). Now the signals are set in the buffer's
			 * order once its fence has been paid, off this thread (-aliothDeliverHandlers): later than the point
			 * Metal names, never before the work it stands for.
			 */
			alioth_stat(ST_SIGNAL, mach_absolute_time());
			(void)device;
			if (_signals == nil) {
				_signals = [[NSMutableArray alloc] init];
			}
			[_signals addObject:s];
			continue;
		}
		if (work->render != nil) {
			if (![self runDraw:work why:why size:sizeof(why)]) {
				goto done;
			}
			continue;
		}
		if (work->clearOnly) {
			struct rwa_texture *color[8] = { NULL };
			struct rwa_device *device = [[_queue device] mesaDevice];

			for (unsigned i = 0; i < work->colorCount; i++) {
				color[i] = [work->color[i] mesaTexture];
			}
			if (device == NULL || mesa.render_clear == NULL) {
				rw_print("a pass that only clears, and %s: nothing was cleared\n", device == NULL ?
				         "no GPU to clear with" : "a compiler library without rwa_render_clear");
			} else if (mesa.render_clear(device, color, work->colorCount, work->clears ? work->clearColor :
			    NULL, work->depth != nil ? [work->depth mesaTexture] : NULL, work->clearsDepth ?
			    &work->clearDepth : NULL, work->clearsStencil ? &work->clearStencil : NULL, why,
			    sizeof(why)) != 0) {
				rw_print("a pass that only clears: %s\n", why);
			}
			continue;
		}
		struct rwa_buffer *buffers[RW_MAX_BUFFERS] = { NULL };
		struct rwa_texture *textures[RW_MAX_BUFFERS] = { NULL };
		struct rwa_sampler *samplers[RW_MAX_BUFFERS] = { NULL };
		/* Metal's one [[buffer(n)]] table, passed BOTH ways: `buffers` is what is
		 * bound at each index, `table` is the same thing with Metal's own offset
		 * beside it, and the library takes from each side what that side owns -- an
		 * argument buffer's encoding offset and a buffer argument's own offset come
		 * from the table, and which an index is nobody but the shader knows. */
		struct rwa_buffer_bind table[RW_MAX_BUFFERS] = { { NULL, 0 } };

		for (unsigned i = 0; i < work->count; i++) {
			buffers[i] = [work->buffers[i] mesaBuffer];
			table[i].buffer = buffers[i];
			table[i].offset = work->bufferOffsets[i];
			if (buffers[i] == NULL && work->buffers[i] != nil) {
				rw_print("buffer %u of this dispatch did not come "
				         "from the compiler library, so it cannot be bound\n", i);
			}
		}
		for (unsigned i = 0; i < work->textureCount; i++) {
			textures[i] = [work->textures[i] mesaTexture];
		}
		for (unsigned i = 0; i < work->samplerCount; i++) {
			samplers[i] = [work->samplers[i] mesaSampler];
		}
		struct rwa_bindings bindings = {
			.buffers = buffers, .buffer_count = work->count,
			.textures = textures, .texture_count = work->textureCount,
			.samplers = samplers, .sampler_count = work->samplerCount,
		};

		/* (the K40's) the threadgroup buffer's size for this dispatch, kept on the pipeline by the library */
		if (work->threadgroupBytes != 0 && mesa.compute_threadgroup_bytes != NULL &&
		    mesa.compute_threadgroup_bytes([work->pipeline compute], work->threadgroupBytes, why,
						   sizeof(why)) != 0) {
			rw_print("the dispatch's threadgroup memory: %s\n", why);
			[self failWith:why];
			goto done;
		}
		/* rwa_dispatch_buffers when the library has it, which is the same call with
		 * Metal's offsets passed as well; a library too old to have it is used
		 * through rwa_dispatch, and then a kernel with a set is refused by name
		 * there and every offset is read as zero -- which is what an older library
		 * did with one in any case, out loud. */
		int rc = mesa.dispatch_buffers != NULL
			    ? mesa.dispatch_buffers([work->pipeline compute], &bindings,
						    table, work->count, work->groups,
						    work->size, why, sizeof(why))
			    : mesa.dispatch([work->pipeline compute], &bindings, work->groups,
					    work->size, why, sizeof(why));
		if (rc != 0) {
			rw_print("the dispatch failed: %s\n", why);
			_error = [[NSError errorWithDomain:@"AliothMetal" code:2
						  userInfo:@{ NSLocalizedDescriptionKey:
							      [NSString stringWithUTF8String:why] }]
				     retain];
			_status = RW_CB_ERROR;
			goto done;
		}
	}
	_status = RW_CB_SCHEDULED;   /* submitted; the fence below is what makes it completed */
done:
	alioth_stat(ST_RUN, committed);
	/* The queue's slot goes back here and on every way out of this method, a failed draw
	 * included: a buffer that errored is as finished as one that drew, and a slot kept
	 * because of an error would turn one bad draw into a queue that blocks for ever. */
	[self aliothReturnSlot];
	/* and only then the handlers, so that a handler which makes the next command buffer
	 * is not waiting for a slot this one still holds */
	[self aliothDeliverHandlers];
	alioth_stats_commit();
}

/* A draw, built out of what the encoder recorded. Separate from the dispatch loop because the
 * two have different bindings -- a draw has them per stage -- and folding them together would
 * mean a struct whose fields mean different things depending on a flag. */
- (BOOL)runDraw:(AliothMetalWork *)work why:(char *)why size:(size_t)size
{
	struct rwa_texture *color[8] = { NULL };
	struct rwa_buffer *vertex_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_vertex_stream vertex_streams[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer *fragment_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_texture *fragment_textures[RW_MAX_BUFFERS] = { NULL };
	struct rwa_sampler *fragment_samplers[RW_MAX_BUFFERS] = { NULL };
	/* and Metal's own table for each stage: the same entries as the arrays above with
	 * the OFFSET beside each, which rwa_bindings has nowhere to put. One array for
	 * both of the things an offset means -- which encoding of an argument buffer, and
	 * which block of a buffer argument -- because Metal has one call behind both. */
	struct rwa_buffer_bind vertex_table[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer_bind fragment_table[RW_MAX_BUFFERS] = { { NULL, 0 } };

	if (mesa.render_draw == NULL) {
		snprintf(why, size, "this compiler library can compute but not yet draw");
		[self failWith:why];
		return NO;
	}
	/* (the K40's) An indirect draw's arguments, read NOW: what came before it in this buffer finished on the GPU
	 * first, so a count a kernel wrote is the count drawn (see -drawPrimitives:indirectBuffer:...). Then the same
	 * road as any draw, with the numbers in the work. */
	if (work->indirectBuffer != nil) {
		BOOL indexed = work->indexBuffer != nil;
		NSUInteger words = indexed ? 5 : 4;
		struct rwa_device *device = [[_queue device] mesaDevice];
		const char *base = [work->indirectBuffer respondsToSelector:@selector(contents)] ?
		    (const char *)[work->indirectBuffer contents] : NULL;

		if (base == NULL || work->indirectOffset % 4 != 0 ||
		    work->indirectOffset + words * 4 > [work->indirectBuffer length]) {
			snprintf(why, size, "an indirect draw whose arguments (%lu words at %lu) are not in its buffer",
			         (unsigned long)words, (unsigned long)work->indirectOffset);
			[self failWith:why];
			return NO;
		}
		if (device != NULL && alioth_finish(device, why, size) != 0) {
			[self failWith:why];
			return NO;
		}
		const uint32_t *a = (const uint32_t *)(base + work->indirectOffset);

		if (a[0] == 0 || a[1] == 0) {
			return YES;     /* no vertices or no instances: Metal draws nothing, and so does this */
		}
		work->vertexCount = a[0];
		work->instanceCount = a[1];
		if (indexed) {
			unsigned index_bytes = work->indexType == MTLIndexTypeUInt32 ? 4 : 2;

			work->indexBufferOffset += (size_t)a[2] * index_bytes;
			work->baseVertex = (int)a[3];
			work->baseInstance = a[4];
		} else {
			work->vertexStart = a[2];
			work->baseInstance = a[3];
		}
		work->indirectBuffer = nil;     /* (read once: a second -commit of this work is not Metal's) */
	}
	for (unsigned i = 0; i < work->colorCount; i++) {
		color[i] = [work->color[i] mesaTexture];
	}
	/*
	 * Metal's one vertex buffer table, split in two by the pipeline's own mask: an index
	 * the vertex descriptor fetches attributes from is a STREAM, with its offset, and
	 * every other index is a [[buffer(n)]] ARGUMENT of the vertex stage. The library
	 * refuses a descriptor that claims an index the module also reads as a buffer
	 * (air_metal.h), so the two sets cannot overlap and this split cannot drop a
	 * binding.
	 *
	 * The offset is honoured on the stream side and still unimplemented on the argument
	 * side, and that asymmetry is said out loud rather than left in a comment: the
	 * library takes a stream's offset as Metal's own `offset:` and rwa_bindings has
	 * nowhere to put one.
	 */
	unsigned stream_mask = [work->render vertexStreamMask];

	for (unsigned i = 0; i < work->vertexBufferCount; i++) {
		/* Metal's own offset, for whichever of the other two things index i is: an
		 * argument buffer (which encoding to read) or a buffer argument (which
		 * block). The library takes it from here and judges it there, where the
		 * shader's own address space is in hand; a stream's offset is the stream
		 * array's below and an index is never both. */
		if (!(stream_mask & (1u << i))) {
			vertex_table[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_table[i].offset = work->vertexBufferOffsets[i];
		}
		if (i < RWA_MAX_VERTEX_BUFFERS && (stream_mask & (1u << i))) {
			vertex_streams[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_streams[i].offset = work->vertexBufferOffsets[i];
			if (work->vertexBuffers[i] != nil &&
			    vertex_streams[i].buffer == NULL) {
				snprintf(why, size, "the buffer bound as vertex stream %u did "
						    "not come from the compiler library, so "
						    "there is nothing to fetch vertices out "
						    "of", i);
				[self failWith:why];
				return NO;
			}
			continue;
		}
		vertex_buffers[i] = [work->vertexBuffers[i] mesaBuffer];
	}
	if ([work->render vertexStreamCount] > RW_MAX_BUFFERS) {
		snprintf(why, size, "this pipeline fetches from %u vertex stream slots and "
				    "this driver's table holds %u", [work->render
				    vertexStreamCount], RW_MAX_BUFFERS);
		[self failWith:why];
		return NO;
	}
	for (unsigned i = 0; i < work->count; i++) {
		fragment_buffers[i] = [work->buffers[i] mesaBuffer];
		fragment_table[i].buffer = fragment_buffers[i];
		fragment_table[i].offset = work->bufferOffsets[i];
		if (work->buffers[i] != nil && fragment_buffers[i] == NULL) {
			snprintf(why, size, "the buffer bound at fragment buffer index %u did "
					    "not come from the compiler library, so there is "
					    "nothing to bind", i);
			[self failWith:why];
			return NO;
		}
	}
	for (unsigned i = 0; i < work->textureCount; i++) {
		fragment_textures[i] = [work->textures[i] mesaTexture];
	}
	for (unsigned i = 0; i < work->samplerCount; i++) {
		fragment_samplers[i] = [work->samplers[i] mesaSampler];
	}
	struct rwa_draw draw = {
		.color = color,
		.color_count = work->colorCount,
		.clear_color = work->clears ? work->clearColor : NULL,
		/* The depth attachment, its two load actions, the state that was bound and
		 * the depth range of the viewport -- which is where the quad's own depth
		 * comes from, the generated vertex stage putting every corner at clip z = 0
		 * (-setViewport: has the whole of it). Without a viewport the contract's
		 * default [0, 1] applies and the quad sits at 0, the near plane. */
		.depth = [work->depth mesaTexture],
		.clear_depth = work->clearsDepth ? &work->clearDepth : NULL,
		.clear_stencil = work->clearsStencil ? &work->clearStencil : NULL,
		.depth_stencil = [work->depthStencilState mesaDepthStencil],
		.stencil_reference = { work->stencilReference[0], work->stencilReference[1] },
		.depth_range = work->hasDepthRange ? work->depthRange : NULL,
		.vertex = { .buffers = vertex_buffers, .buffer_count = work->vertexBufferCount },
		/* and the streams the layout fetches from, at Metal's own indices with
		 * Metal's own offsets; the count is what the library needs covered, which
		 * is the highest index the pipeline fetches from plus one. */
		.vertex_streams = vertex_streams,
		.vertex_stream_count = [work->render vertexStreamCount],
		.fragment = {
			.buffers = fragment_buffers, .buffer_count = work->count,
			.textures = fragment_textures, .texture_count = work->textureCount,
			.samplers = fragment_samplers, .sampler_count = work->samplerCount,
		},
		.primitive = work->primitive,
		.vertex_start = work->vertexStart,
		.vertex_count = work->vertexCount,
		/* The index buffer, by the same rule as every other resource here: it has to
		 * be one of ours, because what the library needs is the Gallium resource
		 * behind it and a buffer from the kernel path (or from another driver) has
		 * none. -mesaBuffer is nil for such a buffer, and nil here would be a draw
		 * the library reads as non-indexed -- the vertices in the wrong order, which
		 * is the one outcome worse than an error. So it is caught where the mapping
		 * fails and not left to the library, which has no way to tell the two apart. */
		.index_buffer = [work->indexBuffer mesaBuffer],
		.index_type = work->indexType,
		.index_buffer_offset = work->indexBufferOffset,
		.base_vertex = work->baseVertex,
		.instance_count = work->instanceCount,
		.base_instance = work->baseInstance,
		/* Metal's own table for each stage, at Metal's own indices: the offsets the
		 * arrays above have nowhere to put. The library takes an argument buffer's
		 * whole binding from here and a buffer argument's offset only, so the two
		 * readings cannot disagree -- and where both name a buffer it checks that
		 * they name the same one. */
		.vertex_buffer_table = vertex_table,
		.vertex_buffer_table_count = work->vertexBufferCount,
		.fragment_buffer_table = fragment_table,
		.fragment_buffer_table_count = work->count,
		.viewport = work->hasViewport ? work->viewport : NULL,
		.cull_mode = work->cullMode,
		.front_ccw = work->frontCCW,
		.scissor = work->hasScissor ? work->scissor : NULL,
		.fill_lines = work->fillLines,
		/* (the K40's) the draws of this buffer batched into one pass; -commit's flush and fence, or
		 * -waitUntilCompleted's finish, say when they are done (each draw was waited for, 40 frames a second) */
		.keep_bound = 1,
		.blend_color = work->blendColor,
		.depth_bias = work->depthBias,
	};

	if (work->indexBuffer != nil && draw.index_buffer == NULL) {
		snprintf(why, size, "the index buffer of this draw did not come from the "
		         "compiler library, so there is nothing to read the indices out of");
		[self failWith:why];
		return NO;
	}

	uint64_t drawn = mach_absolute_time();

	if (mesa.render_draw([work->render render], &draw, why, size) != 0) {
		rw_print("the draw failed: %s\n", why);
		[self failWith:why];
		return NO;
	}
	alioth_stat(ST_DRAW, drawn);
	return YES;
}

- (void)failWith:(const char *)reason
{
	_error = [[NSError errorWithDomain:@"AliothMetal" code:4
				  userInfo:@{ NSLocalizedDescriptionKey:
					      [NSString stringWithUTF8String:reason] }] retain];
	_status = RW_CB_ERROR;
}

/* The fence is the whole point: a dispatch that the GPU does not finish must not read as a
 * result. air_run learned that the expensive way (docs/air.md section 5). */
- (void)waitUntilCompleted
{
	char why[512] = { 0 };

	if (_status == RW_CB_ERROR || _status == RW_CB_COMPLETED) {
		return;
	}
	uint64_t waited = mach_absolute_time();

	/* (the K40's) the wait already going on off this thread (-aliothDeliverHandlers): its end, not a second one */
	if (_done != nil) {
		dispatch_group_wait(_done, DISPATCH_TIME_FOREVER);
		alioth_stat(ST_WAIT, waited);
		return;
	}
	int finished = alioth_flush_wait([[_queue device] mesaDevice], why, sizeof(why));

	alioth_stat(ST_WAIT, waited);
	if (finished != 0) {
		rw_print("THE GPU DID NOT FINISH: %s\n", why);
		_error = [[NSError errorWithDomain:@"AliothMetal" code:3
					  userInfo:@{ NSLocalizedDescriptionKey:
						      [NSString stringWithUTF8String:why] }] retain];
		_status = RW_CB_ERROR;
		return;
	}
	_status = RW_CB_COMPLETED;   /* the fence has been paid, and only now */
}

- (void)waitUntilScheduled { }

/*
 * HANDLERS, AND WHERE THEY RUN.
 *
 * Both of these used to call the block the moment it was added -- which is before the work
 * the block is about, on whichever thread encoded it. That was defensible while the only
 * caller was this port's own test; it is not once backboardd is the caller, and it is the
 * other half of why the system sends -[MTLCommandQueueSPI setCompletionQueue:]: a daemon
 * says WHERE its completion handlers may run precisely because it must not have them on its
 * own thread.
 *
 * So handlers are kept and delivered where the work happens: -commit. A scheduled handler
 * is delivered as soon as the recorded work has been submitted, a completed handler after
 * the GPU fence has been paid for -- the fence is only taken when somebody is actually
 * waiting on one, because this driver's -commit is otherwise asynchronous and a fence
 * nobody asked for would cost every frame. A handler added after the fact is delivered at
 * once rather than dropped: Metal calls that undefined, and losing it would be the kind of
 * silence this driver is trying to stop having.
 */
- (void)aliothDeliver:(void (^)(id))handler
{
	dispatch_queue_t queue = [_queue aliothCompletionQueue];

	if (queue != nil) {
		/* dispatch_async copies the block, which retains both this buffer and the
		 * handler it carries, so the buffer outlives its own completion. */
		dispatch_async(queue, ^{ handler(self); });
		return;
	}
	handler(self);
}

- (void)addScheduledHandler:(void (^)(id))handler
{
	if (handler == NULL) {
		return;
	}
	/* scheduled or further, an error included (3, 4, 5): the work this handler is about has
	 * already run, so it is delivered rather than kept */
	if (_status >= RW_CB_SCHEDULED) {
		[self aliothDeliver:handler];
		return;
	}
	if (_scheduled == nil) {
		_scheduled = [[NSMutableArray alloc] init];
	}
	[_scheduled addObject:[[handler copy] autorelease]];
}

- (void)addCompletedHandler:(void (^)(id))handler
{
	if (handler == NULL) {
		return;
	}
	/* (the K40's) added while the GPU wait off the encoding thread is still on: delivered after it, as promised */
	if (_done != nil && _status == RW_CB_SCHEDULED) {
		void (^copied)(id) = [[handler copy] autorelease];

		dispatch_group_notify(_done, [_queue aliothWaitQueue], ^{ [self aliothDeliver:copied]; });
		return;
	}
	if (_status >= RW_CB_SCHEDULED) {
		[self aliothDeliver:handler];
		return;
	}
	if (_completed == nil) {
		_completed = [[NSMutableArray alloc] init];
	}
	[_completed addObject:[[handler copy] autorelease]];
}

/* Called from the one place the work is done. The order is Metal's: scheduled first, then
 * the fence, then completed. */
- (void)aliothDeliverHandlers
{
	NSArray *scheduled = _scheduled;
	NSArray *completed = _completed;
	NSArray *signals = _signals;    /* (the K40's) events to set once the GPU is done (-commit) */

	_scheduled = nil;
	_completed = nil;
	_signals = nil;

	/*
	 * (the K40's) THE WORK SUBMITTED FIRST. The draws of a buffer are one batch now (rwa_draw.keep_bound) and
	 * nothing waits for each of them, so nothing would hand them to the GPU unless this did -- before anyone is told
	 * the buffer is scheduled: a present is one of those handlers, and the GPU's one ring in order is what puts this
	 * frame before the window server's reading of it. With a fence when completed handlers will wait on it.
	 */
	struct rwa_device *device = [[_queue device] mesaDevice];
	/* (with metal_stats=1 every buffer is fenced and waited for off this thread, handlers or not, so that each
	 * one's GPU time is counted) */
	BOOL async = ([completed count] > 0 || [signals count] > 0 || alioth_stats_on) && _status != RW_CB_ERROR &&
	    device != NULL && mesa.flush != NULL && mesa.fence_wait != NULL;
	void *fence = NULL;
	uint64_t flushed = mach_absolute_time();

	if (device != NULL && mesa.flush != NULL && _status != RW_CB_ERROR) {
		fence = mesa.flush(device, async ? 1 : 0);
		alioth_stat(ST_FLUSH, flushed);
		flushed = mach_absolute_time();
	}
	for (id handler in scheduled) {
		[self aliothDeliver:(void (^)(id))handler];
	}
	[scheduled release];

	/*
	 * (the K40's) THE WAIT OFF THIS THREAD. A completed handler is a promise about the GPU, so it is kept only once
	 * the GPU has finished -- and that wait was made here, in -commit, so the thread that commits (SceneKit's and
	 * SpriteKit's render loop, an MTKView's draw) stood still for the whole of the GPU's work before it could encode
	 * the next frame: 39 frames a second on this driver where OpenGL ES made 60 (System Check, 2026-11-09). Now the
	 * work is flushed with a fence here and the queue's own serial queue waits on it, sets the status and delivers
	 * the handlers (on the application's completion queue if it named one), in commit order; -waitUntilCompleted
	 * waits for that. The wait touches no context (rwa_fence_wait), and everything that does is under alioth_gpu.
	 */
	if (async) {
		dispatch_group_t done = dispatch_group_create();

		_done = done;
		dispatch_group_enter(done);
		dispatch_async([_queue aliothWaitQueue], ^{
			char why[512] = { 0 };

			int waited = mesa.fence_wait(device, fence, why, sizeof(why));

			alioth_stat(ST_GPU, flushed);
			/* (the events first, in the buffer's order: the GPU reached them before it finished the buffer;
			 * set on a failure too, so that nothing waits for ever on a value a broken buffer owed) */
			for (AliothMetalSignal *s in signals) {
				[s->event aliothSignal:s->value];
			}
			[signals release];
			if (waited != 0) {
				rw_print("THE GPU DID NOT FINISH: %s\n", why);
				_error = [[NSError errorWithDomain:@"AliothMetal" code:3
							  userInfo:@{ NSLocalizedDescriptionKey:
								      [NSString stringWithUTF8String:why] }] retain];
				_status = RW_CB_ERROR;
			} else {
				_status = RW_CB_COMPLETED;
			}
			for (id handler in completed) {
				[self aliothDeliver:(void (^)(id))handler];
			}
			[completed release];
			dispatch_group_leave(done);
		});
		return;
	}
	if (([completed count] > 0 || [signals count] > 0) && _status != RW_CB_ERROR) {
		/* the same promise, kept here where the library cannot split the wait */
		[self waitUntilCompleted];
	}
	/* (the K40's) the events, after the wait as above -- or at once for a buffer that failed, so that no waiter is
	 * left with a value nothing will set */
	for (AliothMetalSignal *s in signals) {
		[s->event aliothSignal:s->value];
	}
	[signals release];
	for (id handler in completed) {
		[self aliothDeliver:(void (^)(id))handler];
	}
	[completed release];
}

/* -commitWithDeadline: is an abstract entry of _MTLCommandBuffer, so inheriting it aborts
 * rather than defaulting. The deadline is for work that is worthless if it is late (a frame
 * that misses its vsync); Apple's drivers hand it to the GPU's scheduler. There is one
 * scheduler here and one priority, so the deadline cannot be honoured -- but the COMMIT
 * must happen, and a deadline that is merely unmet is a late frame while a commit that
 * never happens is a daemon waiting for ever. */
- (void)commitWithDeadline:(NSUInteger)deadline
{
	(void)deadline;
	RW_ONCE("a command buffer was committed with a deadline; this driver "
		"has one ring and no scheduler to tell, so the work is committed and the "
		"deadline is not promised\n");
	[self commit];
}

- (void)addPurgedResource:(id)resource { (void)resource; [self aliothPurgedNote]; }
- (void)addPurgedHeap:(id)heap         { (void)heap; [self aliothPurgedNote]; }

/* Metal records on a command buffer every resource of its that was purged while the buffer
 * referenced it, so that -commit can fail the buffer instead of reading pages that are
 * gone. Nothing of this driver's is ever volatile -- -setPurgeableState: answers
 * MTLPurgeableStateNonVolatile for every resource, which is what they are -- so a report
 * here means Metal and this driver disagree about a resource, and that is worth a line
 * rather than a silent buffer that fails later for no stated reason. */
- (void)aliothPurgedNote
{
	RW_ONCE("Metal reported a purged resource on a command buffer, but "
		"nothing this driver allocates is volatile; the report is noted and the "
		"buffer is not failed\n");
}


@end

@implementation AliothMetalRenderEncoder {
	AliothMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder */
	AliothMetalRenderPipeline *_pipeline;
	id _color[8];
	id _resolve[8];         /* (the K40's) each colour attachment's resolveTexture, resolved at -endEncoding */
	unsigned _colorCount;
	float _clearColor[4];
	BOOL _clears;
	id _depth;
	id _depthResolve;
	id _depthStencilState;
	float _clearDepth;
	unsigned _clearStencil;
	BOOL _clearsDepth, _clearsStencil;
	unsigned _stencilReference[2];
	float _depthRange[2];
	BOOL _hasDepthRange;
	float _viewport[4];
	BOOL _hasViewport;
	unsigned _cullMode, _frontCCW, _fillLines;      /* (the K40's) -setCullMode: and its kin */
	unsigned _scissor[4];
	BOOL _hasScissor;
	float _blendColor[4];                           /* (the K40's) blend colour, depth bias */
	float _depthBias[3];
	id _pendingIndirect;                            /* (the K40's) the indirect draw being recorded */
	NSUInteger _pendingIndirectOffset;
	id _vertexBuffers[RW_MAX_BUFFERS];
	NSUInteger _vertexBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentBuffers[RW_MAX_BUFFERS];
	NSUInteger _fragmentBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentTextures[RW_MAX_BUFFERS];
	id _fragmentSamplers[RW_MAX_BUFFERS];
	unsigned _vertexBufferCount, _fragmentBufferCount;
	unsigned _fragmentTextureCount, _fragmentSamplerCount;
	BOOL _complained;
	BOOL _drew, _ended;     /* (the K40's: a draw was recorded; -endEncoding has run) */
}

- (instancetype)initWithAliothCommandBuffer:(AliothMetalCommandBuffer *)commandBuffer
				  descriptor:(MTLRenderPassDescriptor *)descriptor
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;

	/* What the pass is drawing into, and whether it begins by clearing. Metal keeps this in
	 * the pass descriptor rather than in the pipeline, so it belongs to the encoder. */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		id texture = attachment != nil ? [attachment texture] : nil;

		if (texture == nil) {
			continue;
		}
		_color[i] = texture;
		_colorCount = i + 1;
		/*
		 * A resolve. The compiler library makes multisample attachments and renders
		 * into them (docs/metal.md 10.34), and the one-sample texture an application
		 * means to READ is the resolveTexture. (The K40, 2026-11-09: this driver
		 * resolved nothing and said so; now -endEncoding records the resolve after the
		 * pass's work, and the GPU does it at -commit, rwa_texture_resolve.) A resolve
		 * store action with no resolve texture is Metal's validation error; it is said
		 * and the attachment is stored.
		 */
		NSUInteger store = [attachment storeAction];

		if (store == MTLStoreActionMultisampleResolve ||
		    store == MTLStoreActionStoreAndMultisampleResolve) {
			if ([attachment resolveTexture] != nil) {
				_resolve[i] = [attachment resolveTexture];
			} else {
				rw_print("colour attachment %u asks for a multisample resolve "
				         "(storeAction %lu) with no resolveTexture; the attachment is "
				         "stored and nothing is resolved\n", i, (unsigned long)store);
			}
		}
		if (i == 0 && [attachment loadAction] == MTLLoadActionClear) {
			MTLClearColor c = [attachment clearColor];

			_clearColor[0] = (float)c.red;
			_clearColor[1] = (float)c.green;
			_clearColor[2] = (float)c.blue;
			_clearColor[3] = (float)c.alpha;
			_clears = YES;
		}
	}
	/*
	 * The depth/stencil attachment, which is two descriptors over one texture: for
	 * MTLPixelFormatDepth32Float_Stencil8 an application sets the same texture on
	 * both, and for a depth-only format it sets only the first. So whichever of the
	 * two carries a texture is the attachment, and both carrying DIFFERENT textures is
	 * refused out loud -- a Gallium framebuffer has one zsbuf, and quietly taking the
	 * depth one would drop the stencil half of what was asked for.
	 *
	 * Each aspect keeps its own load action, because the one struct Metal has behind
	 * both descriptors holds clearDepth and clearStencil separately
	 * (MTLRenderPassAttachmentDescriptorPrivate) and a pass that clears the depth and
	 * loads the stencil is an ordinary pass.
	 */
	MTLRenderPassDepthAttachmentDescriptor *za = [descriptor depthAttachment];
	MTLRenderPassStencilAttachmentDescriptor *sa = [descriptor stencilAttachment];
	id ztex = [za texture];
	id stex = [sa texture];

	if (ztex != nil && stex != nil && ztex != stex) {
		rw_print("the depth attachment and the stencil attachment are two "
		         "different textures; one texture carries both aspects here, so this "
		         "pass is encoded with neither\n");
	} else {
		_depth = ztex != nil ? ztex : stex;
		if (ztex != nil && [za loadAction] == MTLLoadActionClear) {
			_clearDepth = (float)[za clearDepth];
			_clearsDepth = YES;
		}
		/* a depth resolve takes sample 0 (Gallium's blit of depth); Min and Max are said */
		if (ztex != nil && [za resolveTexture] != nil &&
		    ([za storeAction] == MTLStoreActionMultisampleResolve ||
		     [za storeAction] == MTLStoreActionStoreAndMultisampleResolve)) {
			_depthResolve = [za resolveTexture];
			if ([za depthResolveFilter] != MTLMultisampleDepthResolveFilterSample0) {
				RW_ONCE("a depth resolve asks for the minimum or maximum of the samples; "
					"this driver resolves depth to sample 0\n");
			}
		}
		if (stex != nil && [sa loadAction] == MTLLoadActionClear) {
			_clearStencil = (unsigned)[sa clearStencil];
			_clearsStencil = YES;
		}
	}
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setRenderPipelineState:(id)state { _pipeline = state; }

/*
 * One table, two meanings, and the offset is no longer thrown away.
 *
 * Metal keeps vertex streams and the vertex stage's [[buffer(n)]] arguments in the SAME
 * table, and which an index is depends on the pipeline's vertex descriptor -- which may
 * not even be set yet when this is called. So the offset is RECORDED here and judged at
 * the draw: for a stream it is Metal's own `offset:` and goes to the library, and for a
 * buffer argument it is still unimplemented and the draw says so by name, having the
 * pipeline's own mask to tell the two apart. Dropping it here is what would draw the
 * wrong mesh out of a buffer that holds several.
 */
- (void)setVertexBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("vertex buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_vertexBuffers[index] = buffer;
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
}

/*
 * And the fragment stage's table, which is now the same shape as the vertex one: the
 * offset is RECORDED and judged at the draw rather than refused here.
 *
 * It used to be refused on the spot, because the contract bound whole buffers and
 * ignoring an offset would read the wrong bytes quietly. An ARGUMENT BUFFER is bound
 * through this very call with an offset of its own -- Metal lets one allocation hold many
 * encodings and the offset is which one -- and whether index n is a set or a plain buffer
 * argument is the pipeline's answer and may not be known yet when this is called. So the
 * draw decides: a set takes the offset, a buffer argument with a non-zero offset is still
 * refused there, by name.
 */
- (void)setFragmentBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("fragment buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_fragmentBuffers[index] = buffer;
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setVertexBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setFragmentBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)setFragmentTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentTextures[index] = texture;
	if (index + 1 > _fragmentTextureCount) {
		_fragmentTextureCount = (unsigned)index + 1;
	}
}

- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentSamplers[index] = sampler;
	if (index + 1 > _fragmentSamplerCount) {
		_fragmentSamplerCount = (unsigned)index + 1;
	}
}

/*
 * The viewport, of which exactly one half is served and the other half is declined, and the
 * split is the contract's: rwa_draw carries znear and zfar and carries no rectangle, because
 * the library's draw always covers the whole attachment.
 *
 * The depth range half is not a convenience either. It is the only thing in Metal that moves
 * a quad in z through this driver -- the generated vertex stage puts all four corners at
 * clip z = 0, and Metal's NDC z starts at 0 on the near plane, so the window depth of what
 * is drawn is znear exactly. Without it every draw here would be at depth 0 and no depth
 * test could ever be measured on the device.
 *
 * The rectangle half says so once, as it did before: a viewport silently ignored draws the
 * right triangle in the wrong place.
 */
- (void)setViewport:(MTLViewport)viewport
{
	id<MTLTexture> first = _colorCount > 0 ? _color[0] : _depth;
	NSUInteger w = [first width], h = [first height];

	/* (the K40's) the rectangle is carried now (rwa_draw.viewport); the whole attachment is sent as none, which
	 * is the draw every caller before the rectangle existed made */
	_hasViewport = viewport.originX != 0.0 || viewport.originY != 0.0 || (NSUInteger)viewport.width != w ||
	    (NSUInteger)viewport.height != h;
	_viewport[0] = (float)viewport.originX;
	_viewport[1] = (float)viewport.originY;
	_viewport[2] = (float)viewport.width;
	_viewport[3] = (float)viewport.height;
	_depthRange[0] = (float)viewport.znear;
	_depthRange[1] = (float)viewport.zfar;
	_hasDepthRange = YES;
}
/* (the K40's) The rasterizer state, recorded like everything else and drawn with (rwa_draw's cull_mode, front_ccw,
 * scissor, fill_lines); they were ignored, and SceneKit and SpriteKit set all four. A scissor of the whole
 * attachment is no scissor. */
- (void)setScissorRect:(MTLScissorRect)rect
{
	id first = _colorCount != 0 ? _color[0] : _depth;
	BOOL whole = first != nil && rect.x == 0 && rect.y == 0 && rect.width >= [first width] &&
	    rect.height >= [first height];

	_scissor[0] = (unsigned)rect.x;
	_scissor[1] = (unsigned)rect.y;
	_scissor[2] = (unsigned)MIN(rect.width, UINT32_MAX);
	_scissor[3] = (unsigned)MIN(rect.height, UINT32_MAX);
	_hasScissor = !whole;
}
- (void)setCullMode:(NSUInteger)mode                  { _cullMode = mode <= MTLCullModeBack ? (unsigned)mode : 0; }
- (void)setFrontFacingWinding:(NSUInteger)winding     { _frontCCW = winding == MTLWindingCounterClockwise; }
- (void)setTriangleFillMode:(NSUInteger)mode          { _fillLines = mode == MTLTriangleFillModeLines; }

/* The depth/stencil state, recorded like the pipeline state: nothing runs at encode time,
 * so what a draw tests against is whatever was bound when that draw was encoded. A state
 * from another driver -- or nil, which is Metal's "no test, no write" -- is passed on as
 * nil, and the library draws with its own zeroed state, which is the same thing. */
- (void)setDepthStencilState:(id)state
{
	if (state != nil && ![state isKindOfClass:[AliothMetalDepthStencilState class]]) {
		rw_print("this depth/stencil state is a %s and not one of ours, so "
		         "it cannot be bound; this pass draws with no depth test\n",
		         class_getName(object_getClass(state)));
		state = nil;
	}
	_depthStencilState = state;
}
/* (the K40's) the blend colour, for the pipelines whose factors are BlendColor / BlendAlpha (rwa_draw.blend_color) */
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	_blendColor[0] = r;
	_blendColor[1] = g;
	_blendColor[2] = b;
	_blendColor[3] = a;
}

- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance
{
	AliothMetalWork *work = [[AliothMetalWork alloc] init];

	if (_pipeline == nil || (_colorCount == 0 && _depth == nil)) {
		rw_print("this draw has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "nothing to draw into");
		[work release];
		return;
	}
	/*
	 * What the contract carries in 32 bits and Metal hands over in 64. These are not
	 * limits of this driver's choosing: `index_bias` in Gallium is a signed int and
	 * instance counts there are unsigned, so a number that does not fit is a number
	 * that would be recorded as a DIFFERENT number -- a base vertex of 2^32 as 0, and
	 * an instance count of 2^32 + 3 as 3. Saying so is the only answer that is not a
	 * wrong picture.
	 */
	if (baseVertex < INT32_MIN || baseVertex > INT32_MAX || instances > UINT32_MAX ||
	    baseInstance > UINT32_MAX || count > UINT32_MAX || start > UINT32_MAX) {
		rw_print("this draw does not fit the contract's 32 bits (count %lu, "
		         "start %lu, base vertex %ld, %lu instances from %lu) and is dropped "
		         "rather than wrapped\n", (unsigned long)count, (unsigned long)start,
		         (long)baseVertex, (unsigned long)instances, (unsigned long)baseInstance);
		[work release];
		return;
	}
	/* Said at the draw and not at the pass, because it is the pipeline that knows:
	 * Metal drops a [[depth]] write when there is no depth attachment, and so does
	 * this -- but a caller measuring such a shader would otherwise be left reading a
	 * colour attachment that holds its clear value and nothing else. */
	if ([_pipeline writesDepth] && _depth == nil && !_complained) {
		rw_print("this pipeline's fragment shader stores to [[depth]] and "
		         "this pass has no depth attachment, so those writes go nowhere\n");
		_complained = YES;
	}
	work->render = _pipeline;
	work->colorCount = _colorCount;
	for (unsigned i = 0; i < _colorCount; i++) {
		work->color[i] = _color[i];
	}
	work->clears = _clears;
	for (unsigned i = 0; i < 4; i++) {
		work->clearColor[i] = _clearColor[i];
	}
	work->depth = _depth;
	work->depthStencilState = _depthStencilState;
	work->clearDepth = _clearDepth;
	work->clearStencil = _clearStencil;
	work->clearsDepth = _clearsDepth;
	work->clearsStencil = _clearsStencil;
	work->stencilReference[0] = _stencilReference[0];
	work->stencilReference[1] = _stencilReference[1];
	work->depthRange[0] = _depthRange[0];
	work->depthRange[1] = _depthRange[1];
	work->hasDepthRange = _hasDepthRange;
	work->hasViewport = _hasViewport;
	for (unsigned i = 0; i < 4; i++) {
		work->viewport[i] = _viewport[i];
		work->scissor[i] = _scissor[i];
	}
	work->hasScissor = _hasScissor;
	work->cullMode = _cullMode;
	work->frontCCW = _frontCCW;
	work->fillLines = _fillLines;
	memcpy(work->blendColor, _blendColor, sizeof(work->blendColor));
	memcpy(work->depthBias, _depthBias, sizeof(work->depthBias));
	/* (the K40's) an indirect draw's arguments: read at -commit, when what wrote them is done (-runDraw) */
	work->indirectBuffer = _pendingIndirect;
	work->indirectOffset = _pendingIndirectOffset;
	_pendingIndirect = nil;
	work->primitive = (unsigned)primitive;
	work->vertexStart = (unsigned)start;
	work->vertexCount = (unsigned)count;
	work->indexBuffer = indexBuffer;
	work->indexType = (unsigned)indexType;
	work->indexBufferOffset = indexOffset;
	work->baseVertex = (int)baseVertex;
	work->instanceCount = (unsigned)instances;
	work->baseInstance = (unsigned)baseInstance;
	work->vertexBufferCount = _vertexBufferCount;
	for (unsigned i = 0; i < _vertexBufferCount; i++) {
		work->vertexBuffers[i] = _vertexBuffers[i];
		work->vertexBufferOffsets[i] = (size_t)_vertexBufferOffsets[i];
	}
	work->count = _fragmentBufferCount;
	for (unsigned i = 0; i < _fragmentBufferCount; i++) {
		work->buffers[i] = _fragmentBuffers[i];
		work->bufferOffsets[i] = (size_t)_fragmentBufferOffsets[i];
	}
	work->textureCount = _fragmentTextureCount;
	for (unsigned i = 0; i < _fragmentTextureCount; i++) {
		work->textures[i] = _fragmentTextures[i];
	}
	work->samplerCount = _fragmentSamplerCount;
	for (unsigned i = 0; i < _fragmentSamplerCount; i++) {
		work->samplers[i] = _fragmentSamplers[i];
	}
	[_commandBuffer recordWork:work];
	[work release];
	_drew = YES;
	/* (the K40's) the pass's load actions belong to its FIRST draw: every draw is its own rwa_render_draw, and
	 * one that carried the clears again cleared what the draws before it had drawn -- the depth buffer included,
	 * so a farther triangle drawn later covered a nearer one (System Check's metal.depth), and a pass of many
	 * sprites would have kept only the last. */
	_clears = NO;
	_clearsDepth = NO;
	_clearsStencil = NO;
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:1 baseInstance:0];
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:0];
}

/*
 * The rest of what MTLRenderCommandEncoder requires. Each is here for the reason the
 * device's own 44 were: a selector that is absent aborts the caller, and an application's
 * encoder sends a good many of these without thinking about it.
 *
 * The division is deliberate. A form that can be served by the single-binding path forwards
 * to it. A form that cannot be served -- the INDIRECT draws, whose whole argument list is
 * read out of a buffer the GPU may be writing -- DECLINES out loud, because drawing the
 * wrong primitives is worse than drawing none. A form that is merely state we do not vary
 * yet (store actions: we always store; residency: everything is resident) is a no-op, and
 * that is said here rather than guessed at by the next reader.
 */
- (void)setVertexBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

- (void)setFragmentBuffers:(const id *)buffers
		   offsets:(const NSUInteger *)offsets
		 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

/* The offset alone, which an application changes between draws out of one buffer. It is
 * the same table entry -setVertexBuffer:offset:atIndex: filled, so it is the same field:
 * for a stream the library honours it, and for a buffer argument the draw says it cannot
 * (it has the pipeline's mask and this method has not). */
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
}

/* The same for the fragment stage, and now with the same answer as the vertex one: the
 * offset is recorded against the index already bound there, and the draw judges it. An
 * application that moves an argument buffer's offset between draws -- which is the
 * ordinary way to walk a table of encodings -- is doing nothing this driver has to
 * refuse. */
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
}

- (void)setVertexTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;   /* the contract binds textures to the fragment stage only */
}
- (void)setVertexTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
}
- (void)setVertexSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
}
- (void)setVertexSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
}
- (void)setVertexSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
}
- (void)setFragmentTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentTexture:textures[i] atIndex:range.location + i];
	}
}
- (void)setFragmentSamplerState:(id)sampler
		    lodMinClamp:(float)lodMin
		    lodMaxClamp:(float)lodMax
			atIndex:(NSUInteger)index
{
	(void)lodMin; (void)lodMax;
	[self setFragmentSamplerState:sampler atIndex:index];
}
- (void)setFragmentSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] atIndex:range.location + i];
	}
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
	  baseInstance:(NSUInteger)base
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:base];
}

/*
 * Indexed drawing, which is what every drawing call in a real application is. All three
 * direct forms are one call here, because that is what they are in Metal: the shorter two
 * are the longest with instanceCount 1, baseVertex 0 and baseInstance 0, and Metal's own
 * header documents them that way (the parameter lists are identical as far as they go).
 *
 * Nothing is checked here except what the LIBRARY cannot check: that there is an index
 * buffer at all, and that it is one of ours (its Gallium resource is what the draw needs,
 * and a buffer allocated on the kernel path has none -- -mesaBuffer is nil for it, and
 * nil in the draw would read as "not indexed", which is the vertices in the wrong order).
 * The index type, the offset against the index size, the count against the buffer's
 * length and the indices against the quad are all refused BY NAME in air_metal.c, and
 * that refusal reaches the application as the command buffer's NSError, with the numbers
 * in it. Two readers of the same rule is how the two drift apart.
 */
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
		   baseVertex:(NSInteger)baseVertex
		 baseInstance:(NSUInteger)baseInstance
{
	if (buffer == nil) {
		rw_print("an indexed draw with no index buffer is refused rather "
		         "than drawn as if the indices were 0, 1, 2, ...\n");
		return;
	}
	[self recordDraw:primitive vertexStart:0 count:count indexBuffer:buffer
		indexType:type indexOffset:offset baseVertex:baseVertex
	    instanceCount:instances baseInstance:baseInstance];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset instanceCount:1
			 baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset
		      instanceCount:instances baseVertex:0 baseInstance:0];
}

/*
 * The INDIRECT forms: the draw's whole argument list out of a buffer, MTLDrawPrimitives-
 * IndirectArguments (vertexCount, instanceCount, vertexStart, baseInstance) or MTLDraw-
 * IndexedPrimitivesIndirectArguments (indexCount, instanceCount, indexStart, baseVertex,
 * baseInstance), 32-bit words at an offset.
 *
 * They declined, because the point of an indirect draw is a count the GPU writes and a count
 * read at ENCODE time would be the wrong one. (The K40, 2026-11-09:) they are recorded with
 * the buffer and read at -commit instead, when the work before them in the command buffer
 * has been finished on the GPU (-runDraw), so a count a kernel wrote earlier is the count
 * drawn; the draw then takes the same validated road as any other.
 */
- (void)drawPrimitives:(NSUInteger)primitive
	indirectBuffer:(id)indirectBuffer
  indirectBufferOffset:(NSUInteger)offset
{
	if (indirectBuffer == nil) {
		return;
	}
	_pendingIndirect = indirectBuffer;
	_pendingIndirectOffset = offset;
	[self recordDraw:primitive vertexStart:0 count:1 indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:1 baseInstance:0];
	_pendingIndirect = nil;
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
	       indirectBuffer:(id)indirectBuffer
	 indirectBufferOffset:(NSUInteger)indirectOffset
{
	if (indirectBuffer == nil || buffer == nil) {
		return;
	}
	_pendingIndirect = indirectBuffer;
	_pendingIndirectOffset = indirectOffset;
	[self recordDraw:primitive vertexStart:0 count:1 indexBuffer:buffer indexType:type
	     indexOffset:offset baseVertex:0 instanceCount:1 baseInstance:0];
	_pendingIndirect = nil;
}

/* Raster state the contract does not carry yet. Collected here with one line each so that
 * the next person can see at a glance what the draw ignores. */
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count
{
	if (count > 1 && !_complained) {
		rw_print("%lu viewports are not implemented; the first one is "
		         "taken\n", (unsigned long)count);
		_complained = YES;
	}
	if (count >= 1 && viewports != NULL) {
		[self setViewport:viewports[0]];
	}
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count
{
	/* one viewport here, so the first rectangle (as -setViewports:count: takes the first viewport) */
	if (rects != NULL && count != 0) {
		[self setScissorRect:rects[0]];
	}
}
/* MTLDepthClipModeClip is what the library's rasterizer state does (depth_clip_near and
 * _far are both on); MTLDepthClipModeClamp, which keeps a primitive outside the depth range
 * and clamps its z instead, is a different picture and the contract has nowhere to ask for
 * it. Said once rather than ignored. */
- (void)setDepthClipMode:(NSUInteger)mode
{
	if (mode != MTLDepthClipModeClip && !_complained) {
		rw_print("MTLDepthClipModeClamp is not implemented; this pass clips "
		         "against the depth range instead of clamping to it\n");
		_complained = YES;
	}
}
/* (the K40's) Gallium's polygon offset, number for number (rwa_draw.depth_bias): a shadow map's bias */
- (void)setDepthBias:(float)bias slopeScale:(float)scale clamp:(float)clamp
{
	_depthBias[0] = bias;
	_depthBias[1] = scale;
	_depthBias[2] = clamp;
}
/* The stencil reference, which Metal keeps on the encoder and not in the state object, so
 * it is recorded with the draw like the clear colour. */
- (void)setStencilReferenceValue:(uint32_t)value
{
	_stencilReference[0] = value;
	_stencilReference[1] = value;
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	_stencilReference[0] = front;
	_stencilReference[1] = back;
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset
{
	(void)mode; (void)offset;
}
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const void *)mappings
{
	(void)count; (void)mappings;
}

/*
 * We always store what was drawn, so these record nothing; saying it here is cheaper than
 * leaving the next reader to wonder whether a discarded attachment would be honoured.
 *
 * That is now also true of the depth attachment, and there it is more than tidiness: the
 * only thing MTLStoreActionDontCare permits that storing does not is leaving the contents
 * undefined, so keeping them is a superset of every store action Metal has. A pass whose
 * depth the next pass reads back therefore works whichever action it asked for -- and the
 * depth test inside one pass never depended on this at all.
 */
- (void)setColorStoreAction:(NSUInteger)action atIndex:(NSUInteger)index
{
	(void)action; (void)index;
}
- (void)setDepthStoreAction:(NSUInteger)action                 { (void)action; }
- (void)setStencilStoreAction:(NSUInteger)action               { (void)action; }
- (void)setColorStoreActionOptions:(NSUInteger)options atIndex:(NSUInteger)index
{
	(void)options; (void)index;
}
- (void)setDepthStoreActionOptions:(NSUInteger)options         { (void)options; }
- (void)setStencilStoreActionOptions:(NSUInteger)options       { (void)options; }

/* Residency: every allocation here is resident for its lifetime, so there is nothing to
 * make resident. */
/*
 * RESIDENCY, which on this driver is nothing to arrange, and that is worth saying rather
 * than implementing in silence.
 *
 * In Metal these calls exist because an argument buffer holds raw handles: the GPU reads
 * them, so the resources they name have to be made resident separately. Here every member
 * of every set is bound explicitly from the encoder's record (air_metal.h), so a resource
 * that is encoded is bound and a resource that is not encoded is a refusal at the draw --
 * there is no third state for residency to be about. What the application still owes is
 * what Metal makes it owe: keeping the resource alive, and this driver turns a resource
 * destroyed after it was encoded into a sentence rather than a read of freed memory.
 */
- (void)useResource:(id)resource usage:(NSUInteger)usage stages:(NSUInteger)stages
{
	(void)resource; (void)usage; (void)stages;
}
- (void)useResource:(id)resource usage:(NSUInteger)usage
{
	(void)resource; (void)usage;
	RW_ONCE("-useResource: is a no-op here: every member of an argument "
		"buffer is bound explicitly from what the encoder recorded, so there is no "
		"residency to arrange and no handle for the GPU to follow\n");
}
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)useResources:(const id *)resources
	       count:(NSUInteger)count
	       usage:(NSUInteger)usage
	      stages:(NSUInteger)stages
{
	(void)resources; (void)count; (void)usage; (void)stages;
}
- (void)useHeap:(id)heap
{
	(void)heap;
	RW_ONCE("-useHeap: is a no-op here, for -useResource:'s reason, and "
		"this driver has no heaps to make resident in any case\n");
}
- (void)useHeap:(id)heap stages:(NSUInteger)stages             { (void)heap; (void)stages; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count     { (void)heaps; (void)count; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count stages:(NSUInteger)stages
{
	(void)heaps; (void)count; (void)stages;
}

/*
 * THE REST OF WHAT <MTLRenderCommandEncoder> AND <MTLRenderCommandEncoderSPI> REQUIRE.
 *
 * 125 + 34 required methods between them, read out of the cache rather than the runtime
 * (MTLRenderCommandEncoderSPI is declared in AGXMetalA11.bundle, so no process on this
 * phone has it; tools/metal/owes.py does the subtraction). 48 of them had no answer
 * anywhere in this chain before this section, and an answer is the difference between a
 * daemon that keeps running and one launchd restarts for ever.
 *
 * Each one either serves, or refuses and says so once. Nothing here claims.
 */

/* Served, by forwarding: Metal's own shorthands for what this encoder already does. */
- (void)setFragmentTexture:(id)texture
	   atTextureIndex:(NSUInteger)textureIndex
	     samplerState:(id)sampler
	   atSamplerIndex:(NSUInteger)samplerIndex
{
	[self setFragmentTexture:texture atIndex:textureIndex];
	[self setFragmentSamplerState:sampler atIndex:samplerIndex];
}
- (void)setFragmentSamplerStates:(const id *)samplers
		    lodMinClamps:(const float *)lodMin
		    lodMaxClamps:(const float *)lodMax
		       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] lodMinClamp:lodMin[i]
				  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
- (void)setVertexSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexSamplerState:samplers[i] lodMinClamp:lodMin[i]
				lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
/* The SPI spelling of -setDepthClipMode:, which already says what it cannot do. */
- (void)setDepthClipModeSPI:(NSUInteger)mode { [self setDepthClipMode:mode]; }

/*
 * Served by construction, which is a different thing from ignored.
 *
 * A barrier and a fence both say "do not start this until that has finished". Everything
 * this driver submits goes to the kernel's single msm ring in the order it was committed,
 * and a command buffer's recorded work runs inside one -commit; there is no second queue
 * and no reordering, so the ordering a barrier asks for is already the only ordering there
 * is. If that ever stops being true -- a second ring, or work submitted from -endEncoding
 * -- these stop being correct, which is why it is written down here and not assumed.
 */
- (void)memoryBarrierWithScope:(NSUInteger)scope
		   afterStages:(NSUInteger)after
		  beforeStages:(NSUInteger)before
{
	(void)scope; (void)after; (void)before;
}
- (void)memoryBarrierWithResources:(const id *)resources
			     count:(NSUInteger)count
		       afterStages:(NSUInteger)after
		      beforeStages:(NSUInteger)before
{
	(void)resources; (void)count; (void)after; (void)before;
}
- (void)textureBarrier { }
- (void)updateFence:(id)fence afterStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}
- (void)waitForFence:(id)fence beforeStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}

/* Answers about this pass, and they are answers rather than placeholders: the attachments
 * are real textures in real memory (nothing memoryless), and there is no tile pipeline at
 * all, so there is no tile size to report. Zero is the only honest number for a feature
 * that does not exist -- a plausible 32x32 would be a claim, and the device already answers
 * no to every tile-shading query, so a caller that asks this has already been told. */
- (BOOL)isMemorylessRender { return NO; }
- (NSUInteger)tileWidth    { return 0; }
- (NSUInteger)tileHeight   { return 0; }

/* Refused, each with its name in the log once.
 *
 * Tessellation: the library has no tessellator, so a patch draw records nothing. Said out
 * loud because a draw that silently does nothing is the one failure this driver has already
 * paid for twice (docs/metal.md 10.21, 10.26). */
- (void)aliothNoTessellation
{
	RW_ONCE("tessellation is not implemented; this patch draw records "
		"nothing\n");
}
- (void)drawPatches:(NSUInteger)controlPoints
	 patchStart:(NSUInteger)start
	 patchCount:(NSUInteger)count
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
      instanceCount:(NSUInteger)instances
       baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)instances; (void)baseInstance;
	[self aliothNoTessellation];
}
- (void)drawPatches:(NSUInteger)controlPoints
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
     indirectBuffer:(id)indirectBuffer
indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)indirectBuffer;
	(void)indirectOffset;
	[self aliothNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
		patchStart:(NSUInteger)start
		patchCount:(NSUInteger)count
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	     instanceCount:(NSUInteger)instances
	      baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)controlPointBuffer; (void)controlPointOffset; (void)instances;
	(void)baseInstance;
	[self aliothNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	    indirectBuffer:(id)indirectBuffer
      indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)controlPointBuffer;
	(void)controlPointOffset; (void)indirectBuffer; (void)indirectOffset;
	[self aliothNoTessellation];
}
- (void)setTessellationFactorBuffer:(id)buffer
			     offset:(NSUInteger)offset
		     instanceStride:(NSUInteger)stride
{
	(void)buffer; (void)offset; (void)stride;
	[self aliothNoTessellation];
}
- (void)setTessellationFactorScale:(float)scale
{
	(void)scale;
	[self aliothNoTessellation];
}

/* Indirect command buffers: a buffer of commands the GPU reads for itself. The device
 * answers no to every indirect-command-buffer query, so this is a path a correct caller
 * does not take; the draws in it would be lost silently if it did. */
- (void)aliothNoIndirectCommands
{
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	[self aliothNoIndirectCommands];
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	[self aliothNoIndirectCommands];
}

/* Tile shading and imageblocks: a whole second pipeline kind (a tile function that runs over
 * the pass's imageblock), which this driver does not have. Every binding of it is refused
 * together, because one of them answering and the rest not would be worse than all of them
 * refusing: a tile dispatch would then run with half its bindings. */
- (void)aliothNoTileShading
{
	RW_ONCE("tile shading and imageblocks are not implemented; this tile "
		"binding or dispatch is dropped\n");
}
- (void)dispatchThreadsPerTile:(MTLSize)size
{
	(void)size;
	[self aliothNoTileShading];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length
			    offset:(NSUInteger)offset
			   atIndex:(NSUInteger)index
{
	(void)length; (void)offset; (void)index;
	[self aliothNoTileShading];
}
- (void)setTileBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self aliothNoTileShading];
}
- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self aliothNoTileShading];
}
- (void)setTileBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self aliothNoTileShading];
}
- (void)setTileBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self aliothNoTileShading];
}
- (void)setTileTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self aliothNoTileShading];
}
- (void)setTileTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self aliothNoTileShading];
}
- (void)setTileSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self aliothNoTileShading];
}
- (void)setTileSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self aliothNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self aliothNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self aliothNoTileShading];
}

/* Function tables: pointers to shader functions, which an AIR module of ours never names
 * and the device answers no to (no ray tracing, no function pointers). A table bound here
 * would be a table a shader cannot call. */
- (void)aliothNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVertexVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setVertexVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}
- (void)setTileVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setTileVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTables:(const id *)tables
			      withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}
- (void)setTileIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setTileIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}

/* A line width other than 1 would change the picture, so it is said; the rasterizer this
 * driver drives has one width. */
- (void)setLineWidth:(float)width
{
	if (width != 1.0f) {
		RW_ONCE("a line width of %g is not implemented; lines are one "
			"pixel wide\n", (double)width);
	}
}

/* Vertex amplification: one vertex shader run writing several positions (what stereo
 * rendering uses). The device answers 1 to -supportsVertexAmplificationCount:, so the only
 * mode a correct caller sets is the one that amplifies nothing. */
- (void)setVertexAmplificationMode:(NSUInteger)mode value:(NSUInteger)value
{
	if (value > 1) {
		RW_ONCE("vertex amplification to %lu views is not implemented; "
			"this pass draws one\n", (unsigned long)value);
	}
	(void)mode;
}

/* Counter sampling: the device has no counter sets (every query answers nil or zero), so
 * there is no sample buffer this could write into. */
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;
}

/*
 * MESH AND OBJECT STAGE BINDINGS, AND WHY THEY ARE HERE AT ALL.
 *
 * These 26 are not missing from the chain: _MTLCommandEncoder has an entry for every one of
 * them. Each entry is one of Apple's ABSTRACT methods -- a body that calls
 * doesNotRecognizeSelector: -- and that is the trap docs/metal.md 10.1 was written about:
 * `respondsToSelector:` answers YES, the method list lists it, and the abort when it is
 * called names the RECEIVER, so the log would read "-[AliothMetalRenderEncoder
 * setMeshTexture:atIndex:]: unrecognized selector" and send the next reader looking for a
 * method that is not the problem. An entry in a method table is not behaviour, and the only
 * way to tell these apart from working methods is to read the bodies out of the cache
 * (tools/metal/protometh.py --verdicts, which found exactly 28 of them under this driver's
 * classes: these, -commitWithDeadline: and -setThreadgroupPackingDisabled:).
 *
 * A mesh pipeline is two stages before the fragment one -- an object stage that decides how
 * much geometry to make and a mesh stage that makes it. This driver compiles a vertex and a
 * fragment function and nothing else, and the device answers no when asked whether mesh
 * shaders are supported, so there is no stage for any of this to bind to. They are refused
 * together for the same reason the tile bindings are: half of a stage's bindings arriving
 * would be worse than none.
 */
- (void)aliothNoMeshStages
{
	RW_ONCE("mesh and object shader stages are not implemented (the device "
		"answers no to them); this binding is dropped\n");
}
- (void)setObjectBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self aliothNoMeshStages];
}
- (void)setObjectBytes:(const void *)bytes
		length:(NSUInteger)length
	       atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self aliothNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self aliothNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self aliothNoMeshStages];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)length; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoMeshStages];
}
- (void)setObjectIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoMeshStages];
}
- (void)setMeshBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self aliothNoMeshStages];
}
- (void)setMeshBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self aliothNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self aliothNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self aliothNoMeshStages];
}
- (void)setMeshVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoMeshStages];
}
- (void)setMeshIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoMeshStages];
}
/*
 * The last four of the mesh group are OPTIONAL members of <MTLRenderCommandEncoderSPI>, and
 * optional is the worse case rather than the lighter one: an optional method is the one
 * Apple asks about with respondsToSelector: before sending it, and an abstract entry in
 * _MTLCommandEncoder makes that question answer YES. So the probe passes, the call goes
 * out, and the abort names our class for a method we were never asked to have. Overriding
 * them turns the yes into a yes that is true.
 */
- (void)setMeshIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoMeshStages];
}
- (void)setMeshAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self aliothNoMeshStages];
}
- (void)setObjectAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self aliothNoMeshStages];
}

/* A split handler is called when Metal splits one render pass into several submissions,
 * which this driver never does: a pass is one draw list and one submission. */
- (void)addSplitHandler:(void (^)(id, NSUInteger, NSUInteger))handler
{
	(void)handler;
	RW_ONCE("a render-pass split handler was installed; this driver never "
		"splits a pass, so it will not be called\n");
}

/*
 * Ending a pass is Apple's bookkeeping, and this override used to skip it.
 * `_MTLCommandEncoder` RETAINS the command buffer in -initWithCommandBuffer: -- the
 * designated initialiser this driver sends to super -- and releases it in exactly one place,
 * the tail of its own -endEncoding (0x181a1f4a8: `objc_release(_commandBuffer);
 * _commandBuffer = nil`). An override that does nothing leaves that ivar set, and Apple's
 * -dealloc checks it:
 *
 *   0x181a1e5f4  cbz x8, +124      ; _commandBuffer nil -> nothing to say
 *   0x181a1e610  MTLReportFailure(0, "-[_MTLCommandEncoder dealloc]", 131,
 *                                 @"Command encoder released without endEncoding")
 *
 * and MTLReportFailure here is an abort(). That is the whole of logs/214841-run.log: that
 * line 86 times and 43 aborts of backboardd, one every ten seconds, all from
 * CA::OGL::MetalContext::warmup_shaders -- which for each attachment combination it warms
 * opens a render encoder, ends it immediately, and pops the autorelease pool it made it in
 * (0x188476304, 0x188476308, then the pop whose return address is +1064). So every warmed
 * pass cost one abort. It also leaked a retain on every command buffer any encoder touched,
 * which is the quieter half of the same mistake.
 *
 * So the superclass is called, and what that runs for us is measured rather than hoped --
 * every step is Apple's own code, and the long middle of the function is skipped:
 *
 *   [_commandBuffer executeSynchronizationNotifications:1]   real; our list is empty
 *   [_commandBuffer setCurrentCommandEncoder:nil]            real; an ivar write
 *   _isProgressTrackingEncoder is 0, so no short cut through, and then
 *   [_commandBuffer errorOptions] with bit 0 clear jumps straight to the tail
 *   objc_release(_commandBuffer); _commandBuffer = nil
 *
 * Calling it twice is safe: the second time every message goes to a nil ivar and the release
 * is a release of nil. The one branch not taken is framework-assisted error tracking
 * (`errorOptions` bit 0, MTLCommandBufferErrorOptionEncoderExecutionStatus): it would build
 * an `_MTLCommandBufferEncoderInfo` and ask this encoder -getDriverUniqueID, which is a
 * one-instruction forward to -globalTraceObjectID. Nothing here turns that option on, and if
 * something ever does, that selector is where to look.
 *
 * What this deliberately does NOT do is seal the encoder. Metal calls encoding after
 * -endEncoding undefined, and this driver would still record it; refusing that is separate
 * work and nothing has asked for it.
 *
 * (The K40, 2026-11-08: a pass that clears and draws nothing -- an application's first frame
 * of one colour -- did nothing at all here, its clear being made only on the way to a draw.
 * Such a pass is recorded now as work of its own, the first time -endEncoding is called,
 * and the command buffer has the compiler library clear the attachments, rwa_render_clear.
 * CoreAnimation's warmed passes, if this driver ever serves the window server, clear nothing.)
 */
- (void)endEncoding
{
	if (!_ended && !_drew && (_clears || _clearsDepth || _clearsStencil) &&
	    (_colorCount != 0 || _depth != nil)) {
		AliothMetalWork *work = [[AliothMetalWork alloc] init];

		work->clearOnly = YES;
		work->colorCount = _colorCount;
		for (unsigned i = 0; i < _colorCount; i++) {
			work->color[i] = _color[i];
		}
		work->clears = _clears;
		for (unsigned i = 0; i < 4; i++) {
			work->clearColor[i] = _clearColor[i];
		}
		work->depth = _depth;
		work->clearDepth = _clearDepth;
		work->clearStencil = _clearStencil;
		work->clearsDepth = _clearsDepth;
		work->clearsStencil = _clearsStencil;
		[_commandBuffer recordWork:work];
		[work release];
	}
	/* (the K40's) the pass's resolves, after its work in the command buffer's order */
	for (unsigned i = 0; !_ended && i <= _colorCount; i++) {
		id from = i < _colorCount ? _color[i] : _depth;
		id to = i < _colorCount ? _resolve[i] : _depthResolve;

		if (from == nil || to == nil) {
			continue;
		}
		AliothMetalBlit *b = [[AliothMetalBlit alloc] init];

		b->kind = ALIOTH_BLIT_RESOLVE;
		b->source = [from retain];
		b->destination = [to retain];
		[_commandBuffer recordBlit:b];
		[b release];
	}
	_ended = YES;
	[super endEncoding];
}

@end

@implementation AliothMetalBlit

- (void)dealloc
{
	[source release];
	[destination release];
	[super dealloc];
}

@end

@implementation AliothMetalFence

- (id)device                       { return device; }
- (NSString *)label                { return label; }
- (void)setLabel:(NSString *)text  { [label release]; label = [text copy]; }

- (void)dealloc
{
	[label release];
	[super dealloc];
}

@end

@implementation AliothMetalEvent

- (id)device                       { return device; }
- (NSString *)label                { return label; }
- (void)setLabel:(NSString *)text  { [label release]; label = [text copy]; }
- (id)newSharedEventHandle         { return nil; }     /* one process: see -newSharedEventWithHandle: */

- (uint64_t)signaledValue
{
	@synchronized (self) {
		return value;
	}
}

- (void)setSignaledValue:(uint64_t)v { [self aliothSignal:v]; }

/* the value set, and every listener whose value it reached told on its own queue */
- (void)aliothSignal:(uint64_t)v
{
	NSMutableArray *due = [NSMutableArray array];

	@synchronized (self) {
		value = v;
		for (NSArray *w in waiting) {
			if ([w[1] unsignedLongLongValue] <= v) {
				[due addObject:w];
			}
		}
		[waiting removeObjectsInArray:due];
	}
	for (NSArray *w in due) {
		void (^block)(id, uint64_t) = w[2];

		dispatch_async(w[0], ^{ block(self, v); });
	}
}

/* (the K40's) the queue a listener's blocks run on: its own -- or, for a nil listener, a global queue rather than an
 * exception (Metal's own MTLSharedEventListener initialisers answer nil on this port; alioth_listener_fix) */
static dispatch_queue_t
alioth_listener_queue(id listener)
{
	dispatch_queue_t queue = [listener respondsToSelector:@selector(dispatchQueue)] ? [listener dispatchQueue] : nil;

	return queue != nil ? queue : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
}

- (void)notifyListener:(id)listener atValue:(uint64_t)v block:(void (^)(id, uint64_t))block
{
	dispatch_queue_t queue = alioth_listener_queue(listener);
	uint64_t now;

	if (block == nil) {
		return;
	}
	@synchronized (self) {
		now = value;
		if (now < v) {
			id copied = [block copy];

			[waiting addObject:@[ (id)queue, @(v), copied ]];
			[copied release];
			return;
		}
	}
	dispatch_async(queue, ^{ block(self, now); });
}

- (void)dealloc
{
	[label release];
	[waiting release];
	[super dealloc];
}

@end

@implementation AliothMetalSignal

- (void)dealloc
{
	[event release];
	[super dealloc];
}

@end

@implementation AliothMetalHeap

- (id)device                       { return device; }
- (NSString *)label                { return label; }
- (void)setLabel:(NSString *)text  { [label release]; label = [text copy]; }
- (NSUInteger)storageMode          { return storageMode; }
- (NSUInteger)cpuCacheMode         { return cpuCacheMode; }
- (NSUInteger)hazardTrackingMode   { return hazardTrackingMode; }
- (NSUInteger)resourceOptions
{
	return (storageMode << MTLResourceStorageModeShift) | (cpuCacheMode << MTLResourceCPUCacheModeShift) |
	    (hazardTrackingMode << MTLResourceHazardTrackingModeShift);
}
- (NSUInteger)size                 { return size; }
- (NSUInteger)type                 { return type; }
- (NSUInteger)usedSize             { return MIN(used, size); }
- (NSUInteger)currentAllocatedSize { return size; }

- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
	(void)alignment;
	return size > used ? size - used : 0;
}

- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
	id buffer = [device newBufferWithLength:length options:options];

	if (buffer != nil) {
		used += length;
	}
	return buffer;
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	id texture = [device newTextureWithDescriptor:descriptor];

	if (texture != nil) {
		used += [device heapTextureSizeAndAlignWithDescriptor:descriptor].size;
	}
	return texture;
}

/* placement at an offset is the one thing a heap of separate allocations cannot do: nil, Metal's own failure */
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options offset:(NSUInteger)offset
{
	(void)length; (void)options; (void)offset;
	RW_ONCE("a resource placed at an offset of a heap; this driver's heaps are separate allocations: nil\n");
	return nil;
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset
{
	(void)descriptor; (void)offset;
	RW_ONCE("a resource placed at an offset of a heap; this driver's heaps are separate allocations: nil\n");
	return nil;
}

- (NSUInteger)setPurgeableState:(NSUInteger)state
{
	(void)state;
	return MTLPurgeableStateNonVolatile;    /* nothing here is volatile (-aliothPurgedNote) */
}

- (void)dealloc
{
	[label release];
	[super dealloc];
}

@end

@implementation AliothMetalBlitEncoder {
	AliothMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
}

- (instancetype)initWithAliothCommandBuffer:(AliothMetalCommandBuffer *)commandBuffer
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

/* a record, handed to the command buffer (which keeps the order) */
- (AliothMetalBlit *)record:(int)kind from:(id)source to:(id)destination
{
	AliothMetalBlit *b = [[AliothMetalBlit alloc] init];

	b->kind = kind;
	b->source = [source retain];
	b->destination = [destination retain];
	[_commandBuffer recordBlit:b];
	[b release];
	return b;       /* (the command buffer holds it) */
}

- (void)fillBuffer:(id)buffer range:(NSRange)range value:(uint8_t)value
{
	AliothMetalBlit *b = [self record:ALIOTH_BLIT_FILL from:nil to:buffer];

	b->destinationOffset = range.location;
	b->length = range.length;
	b->value = value;
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)sourceOffset toBuffer:(id)destination
     destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size
{
	AliothMetalBlit *b = [self record:ALIOTH_BLIT_BUFFER_TO_BUFFER from:source to:destination];

	b->sourceOffset = sourceOffset;
	b->destinationOffset = destinationOffset;
	b->length = size;
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)stride
   sourceBytesPerImage:(NSUInteger)image sourceSize:(MTLSize)size toTexture:(id)texture
      destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
{
	AliothMetalBlit *b = [self record:ALIOTH_BLIT_BUFFER_TO_TEXTURE from:source to:texture];

	b->sourceOffset = offset;
	b->sourceBytesPerRow = stride;
	b->sourceBytesPerImage = image;
	b->size = size;
	b->destinationSlice = slice;
	b->destinationLevel = level;
	b->destinationOrigin = origin;
}

- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)stride
   sourceBytesPerImage:(NSUInteger)image sourceSize:(MTLSize)size toTexture:(id)texture
      destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
	       options:(NSUInteger)options
{
	(void)options;  /* (depth or stencil from a combined format: one aspect here) */
	[self copyFromBuffer:source sourceOffset:offset sourceBytesPerRow:stride sourceBytesPerImage:image sourceSize:size
		   toTexture:texture destinationSlice:slice destinationLevel:level destinationOrigin:origin];
}

- (void)copyFromTexture:(id)texture sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
	   sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id)buffer
      destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)stride
destinationBytesPerImage:(NSUInteger)image
{
	AliothMetalBlit *b = [self record:ALIOTH_BLIT_TEXTURE_TO_BUFFER from:texture to:buffer];

	b->sourceSlice = slice;
	b->sourceLevel = level;
	b->sourceOrigin = origin;
	b->size = size;
	b->destinationOffset = offset;
	b->destinationBytesPerRow = stride;
	b->destinationBytesPerImage = image;
}

- (void)copyFromTexture:(id)texture sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
	   sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id)buffer
      destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)stride
destinationBytesPerImage:(NSUInteger)image options:(NSUInteger)options
{
	(void)options;
	[self copyFromTexture:texture sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size
		     toBuffer:buffer destinationOffset:offset destinationBytesPerRow:stride destinationBytesPerImage:image];
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
	   sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)size toTexture:(id)destination
       destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel
      destinationOrigin:(MTLOrigin)destinationOrigin
{
	AliothMetalBlit *b = [self record:ALIOTH_BLIT_TEXTURE_TO_TEXTURE from:source to:destination];

	b->sourceSlice = sourceSlice;
	b->sourceLevel = sourceLevel;
	b->sourceOrigin = sourceOrigin;
	b->size = size;
	b->destinationSlice = destinationSlice;
	b->destinationLevel = destinationLevel;
	b->destinationOrigin = destinationOrigin;
}

- (void)copyFromTexture:(id)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel
	      toTexture:(id)destination destinationSlice:(NSUInteger)destinationSlice
       destinationLevel:(NSUInteger)destinationLevel sliceCount:(NSUInteger)slices levelCount:(NSUInteger)levels
{
	for (NSUInteger l = 0; l < levels; l++) {
		NSUInteger w = MAX([(id<MTLTexture>)source width] >> (sourceLevel + l), 1),
		    h = MAX([(id<MTLTexture>)source height] >> (sourceLevel + l), 1);

		for (NSUInteger s = 0; s < slices; s++) {
			[self copyFromTexture:source sourceSlice:sourceSlice + s sourceLevel:sourceLevel + l
				 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toTexture:destination
			     destinationSlice:destinationSlice + s destinationLevel:destinationLevel + l
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
		}
	}
}

- (void)copyFromTexture:(id)source toTexture:(id)destination
{
	[self copyFromTexture:source sourceSlice:0 sourceLevel:0 toTexture:destination destinationSlice:0
	     destinationLevel:0 sliceCount:MAX([(id<MTLTexture>)source arrayLength], 1)
	   levelCount:MIN([(id<MTLTexture>)source mipmapLevelCount], [(id<MTLTexture>)destination mipmapLevelCount])];
}

- (void)generateMipmapsForTexture:(id)texture
{
	[self record:ALIOTH_BLIT_MIPMAPS from:nil to:texture];
}

/* what a blit encoder may also be asked, with nothing to do on memory both sides see alike */
- (void)synchronizeResource:(id)resource { (void)resource; }
- (void)synchronizeTexture:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}
- (void)optimizeContentsForGPUAccess:(id)texture { (void)texture; }
- (void)optimizeContentsForGPUAccess:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}
- (void)optimizeContentsForCPUAccess:(id)texture { (void)texture; }
- (void)optimizeContentsForCPUAccess:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}
- (void)updateFence:(id)fence  { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }
- (void)resetCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	RW_ONCE("indirect command buffers are not implemented; a blit encoder's reset of one does nothing\n");
}
- (void)copyIndirectCommandBuffer:(id)source sourceRange:(NSRange)range destination:(id)destination
		 destinationIndex:(NSUInteger)index
{
	(void)source; (void)range; (void)destination; (void)index;
	RW_ONCE("indirect command buffers are not implemented; a blit encoder's copy of one does nothing\n");
}
- (void)optimizeIndirectCommandBuffer:(id)buffer withRange:(NSRange)range { (void)buffer; (void)range; }
- (void)sampleCountersInBuffer:(id)buffer atSampleIndex:(NSUInteger)index withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;
}
- (void)resolveCounters:(id)buffer inRange:(NSRange)range destinationBuffer:(id)destination
      destinationOffset:(NSUInteger)offset
{
	(void)buffer; (void)range; (void)destination; (void)offset;
}

- (void)endEncoding
{
	[super endEncoding];
}

@end

@implementation AliothMetalComputeEncoder {
	AliothMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
	AliothMetalComputePipeline *_pipeline;
	id _buffers[RW_MAX_BUFFERS];
	NSUInteger _offsets[RW_MAX_BUFFERS];
	id _textures[RW_MAX_BUFFERS];
	id _samplers[RW_MAX_BUFFERS];
	unsigned _highest, _highestTexture, _highestSampler;
	unsigned _threadgroupBytes;     /* (the K40's) -setThreadgroupMemoryLength: for index 0 */
	BOOL _complained;
}

- (instancetype)initWithAliothCommandBuffer:(AliothMetalCommandBuffer *)commandBuffer
{
	/* the initialiser IOGPUMetalCommandEncoder sends to super (0x1ec171d14 -> 0x181a1ee6c) */
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setComputePipelineState:(id)state
{
	_pipeline = state;
}

- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* The offset is RECORDED and judged at the dispatch, as the render encoder's is.
	 * An argument buffer is bound through this call with an offset of its own -- which
	 * encoding in that buffer to read -- and a plain buffer argument with one is an
	 * application packing several blocks into an allocation, which is served: the
	 * library takes a `constant` binding at any offset (uploading the block when the
	 * GPU cannot fetch from that offset) and refuses a `device` one the GPU cannot
	 * fetch from, by name, where the shader's own address space is in hand. */
	_buffers[index] = buffer;
	_offsets[index] = offset;
	if (index + 1 > _highest) {
		_highest = (unsigned)index + 1;
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("texture index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	_textures[index] = texture;
	if (index + 1 > _highestTexture) {
		_highestTexture = (unsigned)index + 1;
	}
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("sampler index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	_samplers[index] = sampler;
	if (index + 1 > _highestSampler) {
		_highestSampler = (unsigned)index + 1;
	}
}

/* Metal lets a binding narrow the sampler's lod range. The compiler library takes the range
 * when the sampler is made, so honouring it here would need a second sampler object; until
 * it does, saying nothing would silently sample the wrong level. */
- (void)setSamplerState:(id)sampler
	    lodMinClamp:(float)lodMin
	    lodMaxClamp:(float)lodMax
		atIndex:(NSUInteger)index
{
	if (!_complained && (lodMin != 0.0f || lodMax < 1000.0f)) {
		rw_print("a per-binding lod clamp (%g..%g) is not implemented; the "
		         "sampler's own range is used instead\n", (double)lodMin, (double)lodMax);
		_complained = YES;
	}
	[self setSamplerState:sampler atIndex:index];
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/* Metal's own shorthand for small constant data; it is a buffer here, because that is what
 * the shader's binding is. */
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)size
{
	AliothMetalWork *work = [[AliothMetalWork alloc] init];

	if (_pipeline == nil || _complained) {
		rw_print("this dispatch has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "a binding this driver refused");
		[work release];
		return;
	}
	work->pipeline = _pipeline;
	work->count = _highest;
	work->textureCount = _highestTexture;
	work->samplerCount = _highestSampler;
	for (unsigned i = 0; i < _highest; i++) {
		work->buffers[i] = _buffers[i];
		work->bufferOffsets[i] = (size_t)_offsets[i];
	}
	for (unsigned i = 0; i < _highestTexture; i++) {
		work->textures[i] = _textures[i];
	}
	for (unsigned i = 0; i < _highestSampler; i++) {
		work->samplers[i] = _samplers[i];
	}
	work->groups[0] = (unsigned)groups.width;
	work->groups[1] = (unsigned)groups.height;
	work->groups[2] = (unsigned)groups.depth;
	work->size[0] = (unsigned)size.width;
	work->size[1] = (unsigned)size.height;
	work->size[2] = (unsigned)size.depth;
	work->threadgroupBytes = _threadgroupBytes;
	[_commandBuffer recordWork:work];
	[work release];
}

/* Metal counts threads here and threadgroups above; the division is Metal's own, and a
 * grid that does not divide evenly is refused rather than rounded, because rounding runs
 * threads the application did not ask for. */
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)size
{
	MTLSize groups;

	if (size.width == 0 || size.height == 0 || size.depth == 0 ||
	    threads.width % size.width != 0 || threads.height % size.height != 0 ||
	    threads.depth % size.depth != 0) {
		rw_print("%lux%lux%lu threads do not divide into groups of "
		         "%lux%lux%lu, and this driver does not have the partial-group support "
		         "that would need\n", (unsigned long)threads.width,
		         (unsigned long)threads.height, (unsigned long)threads.depth,
		         (unsigned long)size.width, (unsigned long)size.height,
		         (unsigned long)size.depth);
		return;
	}
	groups.width = threads.width / size.width;
	groups.height = threads.height / size.height;
	groups.depth = threads.depth / size.depth;
	[self dispatchThreadgroups:groups threadsPerThreadgroup:size];
}

- (void)useResource:(id)resource usage:(NSUInteger)usage { (void)resource; (void)usage; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)memoryBarrierWithScope:(NSUInteger)scope { (void)scope; }
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	/* (the K40's) index 0, the one threadgroup argument the compiler library places, is carried to the dispatch
	 * (rwa_compute_set_threadgroup_bytes); another index is said once and not given */
	if (index == 0) {
		_threadgroupBytes = (unsigned)MIN(length, (NSUInteger)UINT32_MAX);
	} else {
		RW_ONCE("threadgroup memory at index %lu is not given: the compiler library places one threadgroup "
			"argument, index 0\n", (unsigned long)index);
	}
}

/*
 * THE REST OF WHAT <MTLComputeCommandEncoder> AND <MTLComputeCommandEncoderSPI> REQUIRE:
 * 20 methods that had no answer in this chain, each of which was an abort waiting for the
 * first daemon to encode a kernel. Counted by tools/metal/owes.py off the cache, because
 * MTLComputeCommandEncoderSPI is declared in AGXMetalA11.bundle and is in no process here.
 */

/* Served, by forwarding to the singular form -- which is where this driver's refusal of a
 * non-zero buffer offset already lives, so a plural bind inherits it instead of slipping
 * past it. */
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* Metal's rule is that this moves a buffer that is already bound at that index, so
	 * with nothing bound there is nothing to move -- and binding nil here would add an
	 * empty slot to the dispatch's count and make it complain about a buffer nobody
	 * asked for. */
	if (_buffers[index] != nil) {
		[self setBuffer:_buffers[index] offset:offset atIndex:index];
	}
}
- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}
- (void)setSamplerStates:(const id *)samplers
	    lodMinClamps:(const float *)lodMin
	    lodMaxClamps:(const float *)lodMax
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] lodMinClamp:lodMin[i]
			  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}

/* Served by construction: one ring, one submission per command buffer, in commit order, so
 * the ordering a barrier or a fence asks for is the only ordering there is. Residency is
 * the same answer as on the render encoder: every allocation is resident for its lifetime. */
- (void)memoryBarrierWithResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
}
- (void)updateFence:(id)fence  { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }
- (void)useHeap:(id)heap       { (void)heap; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count { (void)heaps; (void)count; }
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;   /* the device has no counter sets */
}

/* Declined: there is no debug information for a kernel of ours. nil is what Metal's own
 * driver returns when a pipeline was not built with it, so the caller has this path. */
- (id)newKernelDebugInfo { return nil; }

/* Declined, and each says its name once. An indirect dispatch reads its grid out of GPU
 * memory, which means the size is not known when the command is recorded -- this driver
 * records sizes, so there is nothing to record. A dispatch that vanishes without a word is
 * the worst outcome available, hence the line. */
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer
			  indirectBufferOffset:(NSUInteger)offset
			 threadsPerThreadgroup:(MTLSize)size
{
	(void)buffer; (void)offset; (void)size;
	RW_ONCE("an indirect dispatch is not implemented (the grid lives in GPU "
		"memory and this driver records the grid); this dispatch is dropped\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
/* The stage-in region is for a kernel whose arguments arrive as [[stage_in]] -- the
 * non-uniform-grid feature. Nothing in the AIR this driver compiles declares one. */
- (void)aliothNoStageIn
{
	RW_ONCE("a kernel stage-in region is not implemented; the dispatch "
		"covers the whole grid it was given\n");
}
- (void)setStageInRegion:(MTLRegion)region
{
	(void)region;
	[self aliothNoStageIn];
}
- (void)setStageInRegionWithIndirectBuffer:(id)buffer
		      indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	[self aliothNoStageIn];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
	(void)width; (void)height;
	RW_ONCE("imageblocks are not implemented; this kernel's imageblock "
		"size is ignored\n");
}
/* An entry for this exists in _MTLCommandEncoder and it is one of Apple's abstract ones
 * (tools/metal/protometh.py --verdicts), so inheriting it is an abort rather than a
 * default. It is a hint about how the hardware packs threadgroups together; the compiler
 * library decides that for itself and there is nothing here to turn off. */
- (void)setThreadgroupPackingDisabled:(BOOL)disabled { (void)disabled; }

- (void)aliothNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}
- (void)setIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self aliothNoFunctionTables];
}
- (void)setIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self aliothNoFunctionTables];
}

/* The same as the render encoder's, for the same measured reason: the superclass retains the
 * command buffer in the initialiser and releases it only here, and its -dealloc aborts on an
 * encoder released without this. Nothing has deallocated a compute encoder of ours on the
 * phone yet -- the one that aborted 43 times was the render one, out of warmup_shaders -- so
 * this half is the same fix made before it is measured, not after. */
- (void)endEncoding
{
	[super endEncoding];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * MTLArgumentEncoder, which on this driver is a RECORDER and not a writer.
 *
 * Metal's encoder writes one 8-byte handle per member into a plain buffer, and those
 * handles are an Apple GPU's descriptors: a Gallium driver, which binds by index, cannot
 * read them. It also does not need to -- the AIR frontend resolves every member of every
 * set to a binding while translating (docs/air.md 8.2) -- so each -setBuffer:atIndex:
 * here RECORDS (resource, Metal id) against (the destination buffer, the destination
 * offset) in the compiler library, and the draw binds every member the pipeline's shaders
 * declare from those records. The buffer's contents are never read, and an argument
 * buffer the application filled by any other means is REFUSED BY NAME at the draw.
 *
 * Two things are deliberate about this object:
 *
 *   - IT HOLDS NO RECORDS ITSELF. Its state is (buffer, offset), and the records live
 *     with the destination buffer in the library, because that is what Metal's own object
 *     model allows: one buffer holds several encodings at several offsets, each reused
 *     across many draws, and an application may release the encoder and go on drawing
 *     with what it encoded.
 *   - -setArgumentBuffer:offset: DOES NOT CLEAR the encoding at that offset, which is
 *     Metal's behaviour: the bytes an earlier encoding wrote there are still there, so a
 *     second encoding that sets three of five members leaves the other two as they were.
 *
 * The layout comes from the module (`air.indirect_buffer`), so `encodedLength` and
 * `alignment` are Apple's own numbers -- 8 bytes per member the set declares and 8 --
 * which matters because the application allocates its buffer from them and then encodes
 * at multiples of them.
 */
@implementation AliothMetalArgumentEncoder {
	AliothMetalDevice *_owner;         /* not retained: the device owns everything here */
	NSString *_label;
	struct rwa_argbuf_layout _layout;
	BOOL _fromModule;                   /* the layout came from a shader, not descriptors */
	AliothMetalBuffer *_destination;   /* what -setArgumentBuffer:offset: named */
	NSUInteger _offset;
	BOOL _complained;
}

/* Conformance declared at runtime, as the device's MTLDeviceSPI is and for the same
 * reason: an application (or Metal's own validation) may ask conformsToProtocol: before
 * it sends anything, and the protocol is only in the process once Metal is. Every method
 * it requires is answered below -- some of them by declining out loud, which is an answer
 * and not an absence. */
+ (void)load
{
	Protocol *p = objc_getProtocol("MTLArgumentEncoder");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLArgumentEncoder) refused\n");
	}
}

- (instancetype)initWithAliothDevice:(AliothMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	_layout = *layout;
	_fromModule = fromModule;
	return self;
}

- (void)dealloc
{
	[_label release];
	[_destination release];
	[super dealloc];
}

- (id)device       { return _owner; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label
{
	[_label release];
	_label = [label copy];
}

/* The two numbers an application allocates from, and they are the module's own: 8 bytes
 * per member the set declares, used by this shader or not (air_metal.h). A length of ours
 * that was too small would have the application overlapping its own encodings. */
- (NSUInteger)encodedLength { return _layout.encoded_length; }
- (NSUInteger)alignment     { return _layout.alignment; }

/* Not part of MTLArgumentEncoder: what this encoder is for, so a test can read back the
 * mapping between Metal's [[id(n)]] and the index the GPU is programmed with instead of
 * trusting it. Metal's own pipeline reflection would report the same thing. */
- (NSUInteger)aliothMemberCount { return _layout.member_count; }
- (NSUInteger)aliothMetalIndex  { return _layout.metal_index; }
- (BOOL)aliothMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index
{
	if (i >= _layout.member_count) {
		return NO;
	}
	if (mid != NULL) {
		*mid = _layout.members[i].id;
	}
	if (kind != NULL) {
		*kind = _layout.members[i].kind;
	}
	if (index != NULL) {
		*index = _layout.members[i].index;
	}
	return YES;
}

/*
 * The destination. Metal checks the offset against the buffer's length and the encoder's
 * alignment, and so does this -- an application that encodes at an unaligned offset has
 * allocated its buffer from something other than -encodedLength, and every record made
 * afterwards would be keyed by an offset the draw will never look up.
 */
- (void)setArgumentBuffer:(id)buffer offset:(NSUInteger)offset
{
	if (buffer != nil && ![buffer isKindOfClass:[AliothMetalBuffer class]]) {
		rw_print("the argument buffer is a %s and not one of ours, so "
		         "nothing can be encoded into it\n",
		         class_getName(object_getClass(buffer)));
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && [buffer mesaBuffer] == NULL) {
		rw_print("the argument buffer did not come from the compiler "
		         "library, so nothing can be encoded into it\n");
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && _layout.alignment != 0 && offset % _layout.alignment != 0) {
		rw_print("an argument buffer offset of %lu is not a multiple of "
		         "this encoder's alignment (%u), so the draw would look for the "
		         "encoding somewhere else\n", (unsigned long)offset, _layout.alignment);
		[_destination release];
		_destination = nil;
		return;
	}
	/* Retained, unlike the other back-pointers in this file, and for a measured
	 * reason: the records live in the compiler library WITH that buffer, so an
	 * application that released it while this encoder still pointed at it would have
	 * rwa_buffer_destroy free the records (and scrub them out of everyone else's) and
	 * leave this object holding a freed MTLBuffer to send -mesaBuffer to. Metal's own
	 * encoder outliving its argument buffer is an application's mistake; a crash in
	 * the driver is ours. */
	[buffer retain];
	[_destination release];
	_destination = buffer;
	_offset = offset;
}

/* Metal's array form: the same destination with an element's worth of offset added. The
 * arithmetic is Metal's own -- encodedLength per element -- and it is here rather than
 * declined because an application that encodes an ARRAY of sets is doing nothing this
 * driver cannot record. */
- (void)setArgumentBuffer:(id)buffer startOffset:(NSUInteger)offset
	     arrayElement:(NSUInteger)element
{
	[self setArgumentBuffer:buffer offset:offset + element * _layout.encoded_length];
}

/* Whether this encoder is in a state to record, said once per site: an id past the set's
 * own members is the mistake worth naming, because Metal would write a handle into the
 * buffer and the shader would read a descriptor that is not there. */
- (BOOL)aliothReady:(NSUInteger)index for:(const char *)what
{
	if (_destination == nil) {
		if (!_complained) {
			rw_print("%s was encoded at id %lu with no argument buffer "
			         "set (Metal's -setArgumentBuffer:offset: has to name one "
			         "first)\n", what, (unsigned long)index);
			_complained = YES;
		}
		return NO;
	}
	if (index >= _layout.member_count) {
		/* Which of the two layouts this is matters to whoever reads the line: a
		 * shader's set is the shader's business and an application cannot change
		 * it, while a set described with MTLArgumentDescriptors is the
		 * application's own and the id it just used is one it did not describe. */
		rw_print("%s was encoded at id %lu of %s that declares %u member%s\n", what,
		         (unsigned long)index,
		         _fromModule ? "a set a shader declares" : "an encoder this "
		                       "application described",
		         _layout.member_count, _layout.member_count == 1 ? "" : "s");
		return NO;
	}
	return YES;
}

/*
 * The three that record. The KIND is the call, as it is in Metal; a member encoded with
 * the wrong one is refused at the draw, by name, where the shader's own declaration is in
 * hand -- not here, because one buffer may be encoded for a set this driver has never
 * seen a shader for.
 *
 * The library's sentence is passed through rather than replaced: it names the id and what
 * went wrong with it.
 */
- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self aliothReady:index for:"a buffer"]) {
		return;
	}
	if (buffer != nil && ![buffer isKindOfClass:[AliothMetalBuffer class]]) {
		rw_print("the buffer encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(buffer)));
		return;
	}
	if (mesa.argbuf_encode_buffer([_destination mesaBuffer], _offset, (unsigned)index,
				      [buffer mesaBuffer], offset, why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets ? offsets[i] : 0
			atIndex:range.location + i];
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self aliothReady:index for:"a texture"]) {
		return;
	}
	if (texture != nil && ![texture isKindOfClass:[AliothMetalTexture class]]) {
		rw_print("the texture encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(texture)));
		return;
	}
	if (mesa.argbuf_encode_texture([_destination mesaBuffer], _offset, (unsigned)index,
				       [texture mesaTexture], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self aliothReady:index for:"a sampler"]) {
		return;
	}
	if (sampler != nil && ![sampler isKindOfClass:[AliothMetalSampler class]]) {
		rw_print("the sampler encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(sampler)));
		return;
	}
	if (mesa.argbuf_encode_sampler([_destination mesaBuffer], _offset, (unsigned)index,
				       [sampler mesaSampler], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/*
 * -constantDataAtIndex: is where an application writes a VALUE member -- an `int` or a
 * `float4` inside the set rather than a descriptor -- and it is answered with the real
 * bytes of the argument buffer at that member's place, which is what Metal answers.
 *
 * Nothing of this driver ever reads those bytes, and that is said here because it is the
 * kind of silence that costs a day: a shader that reads a value out of a set is refused
 * when it is TRANSLATED ("a value read out of the set instead of a descriptor",
 * docs/air.md 8.2.1), so an application writing here is writing into memory no shader of
 * ours will be compiled to read. Answering a real pointer is still the right answer --
 * the write lands in the application's own buffer and nothing is corrupted -- while
 * answering NULL would be a crash in the application's memcpy.
 */
- (void *)constantDataAtIndex:(NSUInteger)index
{
	if (![self aliothReady:index for:"constant data"]) {
		return NULL;
	}
	void *bytes = [_destination contents];

	if (bytes == NULL) {
		return NULL;
	}
	RW_ONCE("-constantDataAtIndex: hands back the argument buffer's own "
		"bytes, and nothing in this driver reads them: a shader that reads a VALUE "
		"out of an argument buffer is refused when it is translated, because every "
		"member this frontend binds is a descriptor\n");
	return (char *)bytes + _offset + 8 * index;
}

/*
 * A SET INSIDE A SET, declined by name. The frontend refuses a nested `air.indirect_buffer`
 * when it translates the shader -- one module of Resident Evil 4's 7 620 has one, the
 * kernel that encodes draws on the GPU -- so an encoder for one would be an object whose
 * records no pipeline could ever ask for.
 */
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
	rw_print("-newArgumentEncoderForBufferAtIndex:%lu asks for an encoder for "
	         "a set INSIDE a set, and this port's frontend refuses a nested argument buffer "
	         "when it translates the shader, so there is nothing to encode for\n",
	         (unsigned long)index);
	return nil;
}

/*
 * The members that are not a buffer, a texture or a sampler. Every one of them is declined
 * BY NAME and for the same reason: the frontend refuses such a member when it translates
 * the shader ("a member that is not a buffer, a texture or a sampler"), so a record here
 * would be a record no draw could use -- and a silent no-op would leave the application
 * believing its set was encoded.
 */
- (void)aliothNoSuchMember:(const char *)what atIndex:(NSUInteger)index
{
	rw_print("%s was encoded at id %lu of an argument buffer, and this port "
	         "binds a buffer, a texture or a sampler member and refuses the rest when the "
	         "shader is translated; this encoding is dropped\n", what, (unsigned long)index);
}

- (void)setRenderPipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self aliothNoSuchMember:"a render pipeline state" atIndex:index];
}
- (void)setRenderPipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self aliothNoSuchMember:"render pipeline states" atIndex:range.location];
}
- (void)setComputePipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self aliothNoSuchMember:"a compute pipeline state" atIndex:index];
}
- (void)setComputePipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self aliothNoSuchMember:"compute pipeline states" atIndex:range.location];
}
- (void)setIndirectCommandBuffer:(id)buffer atIndex:(NSUInteger)index
{
	(void)buffer;
	[self aliothNoSuchMember:"an indirect command buffer" atIndex:index];
}
- (void)setIndirectCommandBuffers:(const id *)buffers withRange:(NSRange)range
{
	(void)buffers;
	[self aliothNoSuchMember:"indirect command buffers" atIndex:range.location];
}
- (void)setAccelerationStructure:(id)structure atIndex:(NSUInteger)index
{
	(void)structure;
	[self aliothNoSuchMember:"an acceleration structure" atIndex:index];
}
- (void)setVisibleFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self aliothNoSuchMember:"a visible function table" atIndex:index];
}
- (void)setVisibleFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self aliothNoSuchMember:"visible function tables" atIndex:range.location];
}
- (void)setIntersectionFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self aliothNoSuchMember:"an intersection function table" atIndex:index];
}
- (void)setIntersectionFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self aliothNoSuchMember:"intersection function tables" atIndex:range.location];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * -[MTLFunction newArgumentEncoderWithBufferIndex:], which is a method of METAL'S OWN
 * class and is therefore installed at RUNTIME here rather than written as a category.
 *
 * Two decisions, and the first one is measured. Metal's own implementation cannot work on
 * this port:
 *
 *   -[_MTLFunction newArgumentEncoderWithBufferIndex:] (0x181aef7f4) forwards to
 *   ...reflection:binaryArchives: (0x181aef85c), which asks
 *   -[_MTLFunction reflectionWithOptions:binaryArchives:] (0x181aef458) for the
 *   function's reflection -- and that one calls -compiler, which on this port is
 *   -[_MTLDevice compiler] (0x181aac128), a two-instruction stub answering nil
 *   (docs/metal.md 10.7: everything behind Metal's own compiler is nil here). The
 *   builder then walks `[reflection arguments]`, finds a count of zero, and takes its
 *   failure path (0x181aef924):
 *
 *     MTLReportFailure(0, "-[_MTLFunction newArgumentEncoderWithBufferIndex:reflection:"
 *                      "functionReflection:]", 0x22fc,
 *                      @"bufferIndex %lu does not identify an argument buffer")
 *
 *   and returns nil.
 *
 * So the choice is not between Apple's implementation and ours; it is between nil and an
 * encoder. And what Metal was missing is what this port HAS: -[_MTLFunction bitcodeData]
 * works (docs/metal.md 10.12, checked over all eleven functions of AV1SW.metallib), and
 * the frontend reads the set's layout straight out of those bytes. Resident Evil 4 sends
 * this selector; so does any application that writes `[[id(n)]]` structs in MSL instead
 * of building MTLArgumentDescriptors.
 *
 * The second decision is HOW, and a category is the obvious way and the wrong one: a
 * category on `_MTLFunction` makes this dylib import `_OBJC_CLASS_$__MTLFunction`, and a
 * private class symbol that does not bind is a dylib that does not LOAD -- which would
 * take the whole driver down for every Metal client in the process, not just this
 * selector. The one private class symbol this driver does import,
 * `_OBJC_CLASS_$__MTLDevice`, is imported by IOGPU.framework too and has been measured
 * loading on the phone; `ipsw dyld symaddr -b` finds nothing in the cache that imports
 * _MTLFunction's, so nothing says it would bind. class_replaceMethod needs no symbol at
 * all, and it is the same move ensure_spi already makes for MTLDeviceSPI.
 *
 * What it costs, said plainly: the method is replaced for every MTLFunction in the
 * process, Apple's daemons included. Nothing is lost by that -- the implementation it
 * replaces returns nil for all of them, for the reason above -- and nothing else of
 * _MTLFunction is touched.
 */
static id
rw_function_argument_encoder(id self, SEL cmd, NSUInteger index)
{
	id device = ((id (*)(id, SEL))objc_msgSend)(self, sel_getUid("device"));

	(void)cmd;
	if (![device isKindOfClass:[AliothMetalDevice class]]) {
		rw_print("-newArgumentEncoderWithBufferIndex: on a function whose device is "
		         "a %s and not ours\n", class_getName(object_getClass(device)));
		return nil;
	}
	return [(AliothMetalDevice *)device aliothArgumentEncoderForFunction:self
								  bufferIndex:index];
}

/* The same with Metal's out-parameter, which is answered nil: a reflection object needs
 * Metal's own compiler, and every pipeline path of this driver already says so. */
static id
rw_function_argument_encoder_reflection(id self, SEL cmd, NSUInteger index, id *reflection)
{
	if (reflection != NULL) {
		*reflection = nil;
	}
	return rw_function_argument_encoder(self, cmd, index);
}

/* Installed once per process, from ensure_spi -- which runs at +load (Metal loaded us, so
 * Metal is there) and again before the first device is made (a library can also be
 * brought in by hand, before Metal). A process without Metal's class simply has no
 * MTLFunction to answer for. */
static void
ensure_function_encoder(void)
{
	static BOOL installed;
	Class fn = objc_lookUpClass("_MTLFunction");

	if (installed || fn == NULL) {
		return;
	}
	installed = YES;
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:"),
			    (IMP)rw_function_argument_encoder, "@@:Q");
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:reflection:"),
			    (IMP)rw_function_argument_encoder_reflection, "@@:Q^@");
}
