/*
 * bishaders: the first triangle's two shaders, compiled on this machine by Mesa's Bifrost compiler for the phone's
 * Mali-G52 r1 (product 0x7402), and what Panfrost prepares for them at compile and state-object time, written out as
 * userland/src/mali/shaders_v7.h. Built and run by tools/mk-shaders.sh against a host build of Mesa 25.0.7 with
 * panfrost (libpanfrost_bifrost, libpan-arch-v7, libpanfrost_lib, libnir): the phone's library carries the result
 * and needs no compiler.
 *
 * The shaders, as NIR (nir_builder; no GLSL front end on this side):
 *   tri_vs   gl_Position = attribute 0 (a vec4 per vertex)
 *   tri_fs   colour 0 = a constant (1, 0, 0, 1)
 * The vertex shader goes through pan_shader_preprocess (bifrost_preprocess_nir), which puts in the viewport transform
 * as load_viewport_scale/offset; Panfrost turns those into system values pushed from a buffer
 * (panfrost_nir_lower_sysvals). Here they become constants of a 64x64 viewport (scale and offset 32, 32, 0.5), so
 * the shader pushes nothing: NDC (x, y) lands on pixel (32 + 32x, 32 + 32y). No IDVS (inputs.no_idvs): a vertex job
 * and a tiler job, as Panfrost runs a draw whose vertex shader is not split.
 *
 * What is written for each, besides the code:
 *   the Renderer State words as Panfrost would hand the GPU for them: pan_shader_prepare_rsd (pan_shader.h) for the
 *   vertex shader; for the fragment shader that merged with panfrost_prepare_fs_state's fields and the state objects'
 *   (pan_cmdstream.c) for the fixed state of this draw -- no depth, stencil, MSAA, alpha-to-coverage or query, one
 *   RGBA8Unorm target, blending off -- and the Blend descriptor of that target (panfrost_emit_blend). The shader's
 *   address (words 0 and 1) is 0: the driver puts it in, as Panfrost merges a partial RSD (pan_merge);
 *   the vertex attribute's format (RGBA32F, GENX(panfrost_format_from_pipe_format)).
 */
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "panfrost/compiler/bifrost_compile.h"
#include "util/u_dynarray.h"

#include "genxml/gen_macros.h"
#include "pan_blend.h"
#include "pan_earlyzs.h"
#include "pan_format.h"
#include "pan_shader.h"

#define GPU_ID 0x7402u

static void
fatal(int sig)
{
	void *frames[48];
	int n = backtrace(frames, 48);

	fprintf(stderr, "bishaders: signal %d\n", sig);
	backtrace_symbols_fd(frames, n, 2);
	_exit(70);
}

/* load_viewport_scale/offset of a 64x64 viewport, as constants */
static bool
viewport_constants(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	if (intr->intrinsic != nir_intrinsic_load_viewport_scale && intr->intrinsic != nir_intrinsic_load_viewport_offset) {
		return false;
	}
	b->cursor = nir_before_instr(&intr->instr);
	nir_def_replace(&intr->def, nir_imm_vec3(b, 32.0f, 32.0f, 0.5f));
	return true;
}

static void
words(FILE *out, const char *name, const uint32_t *w, unsigned n)
{
	fprintf(out, "static const uint32_t %s[%u] = {", name, n);
	for (unsigned i = 0; i < n; i++) {
		fprintf(out, "%s0x%08x,", i % 8 ? " " : "\n\t", w[i]);
	}
	fprintf(out, "\n};\n");
}

static void
compile(nir_shader *s, struct util_dynarray *binary, struct pan_shader_info *info)
{
	struct panfrost_compile_inputs inputs = { .gpu_id = GPU_ID, .no_idvs = true };

	pan_shader_preprocess(s, GPU_ID);
	if (s->info.stage == MESA_SHADER_VERTEX) {
		NIR_PASS(_, s, nir_shader_intrinsics_pass, viewport_constants, nir_metadata_control_flow, NULL);
	}
	util_dynarray_init(binary, NULL);
	GENX(pan_shader_compile)(s, &inputs, binary, info);
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "shaders_v7.h";
	struct util_dynarray vs_bin, fs_bin;
	struct pan_shader_info vs, fs;
	FILE *out;

	for (int sig = 1; sig < 32; sig++) {
		if (sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGTRAP || sig == SIGFPE) {
			signal(sig, fatal);
		}
	}
	glsl_type_singleton_init_or_ref();

	/* tri_vs */
	{
		nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, GENX(pan_shader_get_compiler_options)(),
		    "tri_vs");
		nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(), "a_position");
		nir_variable *pos = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");

		in->data.location = VERT_ATTRIB_GENERIC0;
		in->data.driver_location = 0;
		pos->data.location = VARYING_SLOT_POS;
		pos->data.driver_location = 0;
		b.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0);
		b.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS);
		nir_store_var(&b, pos, nir_load_var(&b, in), 0xf);
		compile(b.shader, &vs_bin, &vs);
	}
	/* tri_fs */
	{
		nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, GENX(pan_shader_get_compiler_options)(),
		    "tri_fs");
		nir_variable *color = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "color");

		color->data.location = FRAG_RESULT_DATA0;
		color->data.driver_location = 0;
		b.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		nir_store_var(&b, color, nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f), 0xf);
		compile(b.shader, &fs_bin, &fs);
	}
	if (vs.vs.idvs || vs.vs.secondary_enable || vs.push.count != 0 || fs.push.count != 0 || vs.ubo_count || fs.ubo_count ||
	    vs.varyings.output_count != 1 || vs.varyings.output[0].location != VARYING_SLOT_POS ||
	    fs.varyings.input_count != 0 || vs.tls_size || fs.tls_size) {
		fprintf(stderr, "bishaders: the shaders are not what the triangle's descriptors expect (idvs %d, push %u/%u, "
		    "ubos %u/%u, varyings %u out %u in, tls %u/%u)\n", vs.vs.idvs, vs.push.count, fs.push.count, vs.ubo_count,
		    fs.ubo_count, vs.varyings.output_count, fs.varyings.input_count, vs.tls_size, fs.tls_size);
		return 1;
	}

	/* the vertex shader's Renderer State: pan_shader_prepare_rsd alone (panfrost_emit_compute_shader_meta) */
	struct mali_renderer_state_packed vs_rsd;
	pan_pack(&vs_rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&vs, 0, &cfg);
	}

	/* the fragment shader's: panfrost_prepare_fs_state for this draw's state, merged with the shader's own part
	 * (fs->partial_rsd) and the state objects' words (panfrost_emit_frag_shader) */
	struct mali_renderer_state_packed fs_rsd, fs_partial;
	struct pan_earlyzs_state earlyzs = pan_earlyzs_get(pan_earlyzs_analyze(&fs), false, false, true);
	pan_pack(&fs_rsd, RENDERER_STATE, cfg) {
		cfg.properties.pixel_kill_operation = (enum mali_pixel_kill)earlyzs.kill;
		cfg.properties.zs_update_operation = (enum mali_pixel_kill)earlyzs.update;
		/* pan_allow_forward_pixel_to_kill: the one target written, blending reads nothing, no alpha-to-coverage */
		cfg.properties.allow_forward_pixel_to_kill = fs.fs.can_fpk;
		cfg.multisample_misc.sample_mask = 0xFFFF;
		cfg.multisample_misc.evaluate_per_sample = false;
		cfg.properties.point_sprite_coord_origin_max_y = false;
		/* panfrost_overdraw_alpha: true unless an enabled target lacks the flag; the target's blending is off */
		cfg.multisample_misc.overdraw_alpha0 = true;
		cfg.multisample_misc.overdraw_alpha1 = true;
		cfg.stencil_mask_misc.alpha_to_coverage = false;
		cfg.depth_units = 0.0f;
		cfg.depth_factor = 0.0f;
		cfg.depth_bias_clamp = 0.0f;
		cfg.stencil_front.reference_value = 0;
		cfg.stencil_back.reference_value = 0;
	}
	pan_pack(&fs_partial, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&fs, 0, &cfg);
	}
	pan_merge(fs_rsd, fs_partial, RENDERER_STATE);
	{
		/* the rasterizer's (panfrost_create_rasterizer_state: GL defaults, depth clipped near and far) and the depth
		 * and stencil state's (panfrost_create_depth_stencil_state: depth off, so ALWAYS; stencil off) words 8..11 */
		struct mali_multisample_misc_packed rast_ms, zsa_depth;
		struct mali_stencil_mask_misc_packed rast_st, zsa_st;
		struct mali_stencil_packed front;

		pan_pack(&rast_ms, MULTISAMPLE_MISC, cfg) {
			cfg.multisample_enable = false;
			cfg.fixed_function_near_discard = true;
			cfg.fixed_function_far_discard = true;
			cfg.fixed_function_depth_range_fixed = true;
			cfg.shader_depth_range_fixed = true;
		}
		pan_pack(&rast_st, STENCIL_MASK_MISC, cfg) {
			cfg.single_sampled_lines = true;
		}
		pan_pack(&zsa_depth, MULTISAMPLE_MISC, cfg) {
			cfg.depth_function = MALI_FUNC_ALWAYS;
			cfg.depth_write_mask = false;
		}
		pan_pack(&zsa_st, STENCIL_MASK_MISC, cfg) {
			cfg.stencil_enable = false;
		}
		/* pan_pipe_to_stencil of a zeroed pipe_stencil_state: func NEVER (0), ops KEEP */
		pan_pack(&front, STENCIL, s) {
			s.mask = 0;
			s.compare_function = MALI_FUNC_NEVER;
			s.stencil_fail = MALI_STENCIL_OP_KEEP;
			s.depth_fail = MALI_STENCIL_OP_KEEP;
			s.depth_pass = MALI_STENCIL_OP_KEEP;
		}
		fs_rsd.opaque[8] |= zsa_depth.opaque[0] | rast_ms.opaque[0];
		fs_rsd.opaque[9] |= zsa_st.opaque[0] | rast_st.opaque[0];
		fs_rsd.opaque[10] |= front.opaque[0];
		fs_rsd.opaque[11] |= front.opaque[0];
	}

	/* the target's Blend descriptor (panfrost_emit_blend): blending off, all four channels written */
	struct mali_blend_packed blend;
	struct pan_blend_equation eq = { .blend_enable = false, .color_mask = 0xf };
	struct mali_blend_equation_packed eqp;
	bool opaque = pan_blend_is_opaque(eq);

	pan_pack(&eqp, BLEND_EQUATION, cfg) {
		pan_blend_to_fixed_function_equation(eq, &cfg);
	}
	pan_pack(&blend, BLEND, cfg) {
		cfg.srgb = false;
		cfg.load_destination = pan_blend_reads_dest(eq);
		cfg.round_to_fb_precision = false;       /* !dithered: Gallium's default blend state dithers */
		cfg.alpha_to_one = false;
		cfg.constant = 0;
	}
	blend.opaque[1] = eqp.opaque[0];
	{
		struct mali_internal_blend_packed *internal = (struct mali_internal_blend_packed *)&blend.opaque[2];

		pan_pack(internal, INTERNAL_BLEND, cfg) {
			cfg.mode = opaque ? MALI_BLEND_MODE_OPAQUE : MALI_BLEND_MODE_FIXED_FUNCTION;
			cfg.fixed_function.num_comps = 4;
			cfg.fixed_function.conversion.memory_format =
			    GENX(panfrost_dithered_format_from_pipe_format)(PIPE_FORMAT_R8G8B8A8_UNORM, true);
			cfg.fixed_function.rt = 0;
			if (!opaque) {
				cfg.fixed_function.alpha_zero_nop = pan_blend_alpha_zero_nop(eq);
				cfg.fixed_function.alpha_one_store = pan_blend_alpha_one_store(eq);
			}
			cfg.fixed_function.conversion.register_format = fs.bifrost.blend[0].format;
		}
	}

	out = fopen(path, "w");
	if (out == NULL) {
		perror(path);
		return 1;
	}
	fprintf(out, "/*\n * Generated by tools/bishaders.c (tools/mk-shaders.sh): the first triangle's shaders, compiled by Mesa "
	    "25.0.7's\n * Bifrost compiler for the Mali-G52 r1 (0x%x), and their Renderer State and Blend words as Panfrost "
	    "prepares\n * them. Do not edit.\n */\n#pragma once\n#include <stdint.h>\n\n", GPU_ID);
	words(out, "hm_tri_vs_code", (const uint32_t *)vs_bin.data, vs_bin.size / 4);
	words(out, "hm_tri_vs_rsd", vs_rsd.opaque, 16);
	fprintf(out, "/* work registers %u, preload %#llx, attributes %u, varyings out %u */\n", vs.work_reg_count,
	    (unsigned long long)vs.preload, vs.attribute_count, vs.varyings.output_count);
	words(out, "hm_tri_fs_code", (const uint32_t *)fs_bin.data, fs_bin.size / 4);
	words(out, "hm_tri_fs_rsd", fs_rsd.opaque, 16);
	fprintf(out, "/* work registers %u, preload %#llx, blend register format %u, early ZS kill %u update %u, fpk %d */\n",
	    fs.work_reg_count, (unsigned long long)fs.preload, fs.bifrost.blend[0].format, earlyzs.kill, earlyzs.update,
	    fs.fs.can_fpk);
	words(out, "hm_tri_fs_blend", blend.opaque, 4);
	fprintf(out, "/* the vertex attribute's format: RGBA32F (GENX(panfrost_format_from_pipe_format)) */\n");
	fprintf(out, "static const uint32_t hm_tri_attribute_format = 0x%08x;\n",
	    (unsigned)GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R32G32B32A32_FLOAT)->hw);
	fclose(out);
	printf("bishaders: tri_vs %u bytes, tri_fs %u bytes -> %s\n", vs_bin.size, fs_bin.size, path);
	return 0;
}
