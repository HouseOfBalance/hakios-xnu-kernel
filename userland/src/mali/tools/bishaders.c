/*
 * bishaders: the library's shader programs, compiled on this machine by Mesa's Bifrost compiler for the phone's
 * Mali-G52 r1 (product 0x7402), and what Panfrost prepares for them at compile and state-object time, written out as
 * userland/src/mali/shaders_v7.h (struct hm_program, program.h). Built and run by tools/mk-shaders.sh against a host
 * build of Mesa 25.0.7 with panfrost; the phone's library carries the result and needs no compiler.
 *
 * The programs, as NIR (nir_builder; no GLSL front end on this side):
 *   tri      gl_Position = attribute 0; colour 0 = (1, 0, 0, 1)
 *   color    gl_Position = attribute 0, v_color (VAR0) = attribute 1; colour 0 = v_color * u_tint, u_tint a vec4 at
 *            byte 0 of uniform buffer 0, which the compiler pushes (Fast Access Uniforms: pushable_ubos 1)
 *   tex      gl_Position = attribute 0, v_uv (VAR0, vec2) = attribute 1; colour 0 = texture 0 with sampler 0 at v_uv
 * Each vertex shader goes through pan_shader_preprocess (bifrost_preprocess_nir), which puts in the viewport transform
 * as load_viewport_scale/offset; Panfrost turns those into system values pushed from a buffer
 * (panfrost_nir_lower_sysvals). Here they become constants of a 64x64 viewport (scale and offset 32, 32, 0.5): NDC
 * (x, y) lands on pixel (32 + 32x, 32 + 32y). No IDVS (inputs.no_idvs): a vertex job and a tiler job.
 *
 * For each program, besides the code:
 *   the Renderer State words as Panfrost would hand the GPU for them: pan_shader_prepare_rsd (pan_shader.h) for the
 *   vertex shader; for the fragment shader that merged with panfrost_prepare_fs_state's fields and the state objects'
 *   (pan_cmdstream.c) for a fixed state -- no depth, stencil, MSAA, alpha-to-coverage or query, one RGBA8Unorm
 *   target -- and the Blend descriptor of that target (panfrost_emit_blend), blending off, and again with Metal's
 *   source-over (One, OneMinusSourceAlpha) where the program says so. The shader's address (words 0 and 1) is 0: the
 *   driver puts it in, as Panfrost merges a partial RSD (pan_merge);
 *   the words each stage pushes (info.push: uniform buffer and byte offset of each 32-bit FAU word, in order);
 *   the varyings the vertex shader writes and the fragment shader reads, by location, each with its Mali format
 *   (GENX(panfrost_format_from_pipe_format)) and size, for the driver to link as panfrost_emit_varying_descs does;
 *   the vertex attributes' formats.
 */
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "panfrost/compiler/bifrost_compile.h"
#include "util/format/u_format.h"
#include "util/u_dynarray.h"

#include "genxml/gen_macros.h"
#include "pan_blend.h"
#include "pan_earlyzs.h"
#include "pan_format.h"
#include "pan_shader.h"

#define GPU_ID 0x7402u

static FILE *out;

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
words(const char *name, const uint32_t *w, unsigned n)
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
	struct panfrost_compile_inputs inputs = { .gpu_id = GPU_ID, .no_idvs = true, .pushable_ubos = 1 };

	pan_shader_preprocess(s, GPU_ID);
	if (s->info.stage == MESA_SHADER_VERTEX) {
		NIR_PASS(_, s, nir_shader_intrinsics_pass, viewport_constants, nir_metadata_control_flow, NULL);
		/* panfrost_shader_compile: no varying is noperspective (key->vs.noperspective_varyings 0) */
		NIR_PASS(_, s, pan_nir_lower_static_noperspective, 0);
	}
	util_dynarray_init(binary, NULL);
	GENX(pan_shader_compile)(s, &inputs, binary, info);
}

static nir_variable *
var(nir_builder *b, nir_variable_mode mode, const struct glsl_type *type, const char *name, int location, int driver)
{
	nir_variable *v = nir_variable_create(b->shader, mode, type, name);

	v->data.location = location;
	v->data.driver_location = driver;
	return v;
}

/* the fragment shader's Renderer State for the fixed state above, as panfrost_emit_frag_shader merges it */
static void
fs_state(const struct pan_shader_info *fs, struct mali_renderer_state_packed *fs_rsd)
{
	struct mali_renderer_state_packed partial;
	struct pan_earlyzs_state earlyzs = pan_earlyzs_get(pan_earlyzs_analyze(fs), false, false, true);

	pan_pack(fs_rsd, RENDERER_STATE, cfg) {
		cfg.properties.pixel_kill_operation = (enum mali_pixel_kill)earlyzs.kill;
		cfg.properties.zs_update_operation = (enum mali_pixel_kill)earlyzs.update;
		/* pan_allow_forward_pixel_to_kill: the one target written; a blend that reads the destination says no below */
		cfg.properties.allow_forward_pixel_to_kill = fs->fs.can_fpk;
		cfg.multisample_misc.sample_mask = 0xFFFF;
		cfg.multisample_misc.evaluate_per_sample = false;
		cfg.properties.point_sprite_coord_origin_max_y = false;
		cfg.multisample_misc.overdraw_alpha0 = true;
		cfg.multisample_misc.overdraw_alpha1 = true;
	}
	pan_pack(&partial, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(fs, 0, &cfg);
	}
	pan_merge(*fs_rsd, partial, RENDERER_STATE);
	{
		/* the rasterizer's (GL defaults, depth clipped near and far) and the depth and stencil state's (depth off, so
		 * ALWAYS; stencil off) words 8..11 */
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
		pan_pack(&front, STENCIL, s) {
			s.mask = 0;
			s.compare_function = MALI_FUNC_NEVER;
			s.stencil_fail = MALI_STENCIL_OP_KEEP;
			s.depth_fail = MALI_STENCIL_OP_KEEP;
			s.depth_pass = MALI_STENCIL_OP_KEEP;
		}
		fs_rsd->opaque[8] |= zsa_depth.opaque[0] | rast_ms.opaque[0];
		fs_rsd->opaque[9] |= zsa_st.opaque[0] | rast_st.opaque[0];
		fs_rsd->opaque[10] |= front.opaque[0];
		fs_rsd->opaque[11] |= front.opaque[0];
	}
}

/* the target's Blend descriptor (panfrost_emit_blend) for equation EQ */
static void
blend_desc(const struct pan_shader_info *fs, struct pan_blend_equation eq, struct mali_blend_packed *blend)
{
	struct mali_blend_equation_packed eqp;
	bool opaque = pan_blend_is_opaque(eq);

	pan_pack(&eqp, BLEND_EQUATION, cfg) {
		pan_blend_to_fixed_function_equation(eq, &cfg);
	}
	pan_pack(blend, BLEND, cfg) {
		cfg.srgb = false;
		cfg.load_destination = pan_blend_reads_dest(eq);
		cfg.round_to_fb_precision = false;       /* !dithered: Gallium's default blend state dithers */
		cfg.alpha_to_one = false;
		cfg.constant = 0;
	}
	blend->opaque[1] = eqp.opaque[0];
	struct mali_internal_blend_packed *internal = (struct mali_internal_blend_packed *)&blend->opaque[2];

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
		cfg.fixed_function.conversion.register_format = fs->bifrost.blend[0].format;
	}
}

static void
varyings(const char *name, const struct pan_shader_varying *v, unsigned count)
{
	fprintf(out, "static const struct hm_varying %s[%u] = {\n", name, count ? count : 1);
	for (unsigned i = 0; i < count; i++) {
		fprintf(out, "\t{ %u, 0x%08x, %u },   /* %s, %s */\n", v[i].location,
		    v[i].location == VARYING_SLOT_POS ? 0u : (unsigned)GENX(panfrost_format_from_pipe_format)(v[i].format)->hw,
		    v[i].location == VARYING_SLOT_POS ? 16u : util_format_get_blocksize(v[i].format),
		    gl_varying_slot_name_for_stage(v[i].location, MESA_SHADER_VERTEX), util_format_short_name(v[i].format));
	}
	if (count == 0) {
		fprintf(out, "\t{ 0, 0, 0 },\n");
	}
	fprintf(out, "};\n");
}

static void
pushes(const char *name, const struct pan_shader_info *info)
{
	fprintf(out, "static const struct hm_push_word %s[%u] = {", name, info->push.count ? info->push.count : 1);
	for (unsigned i = 0; i < info->push.count; i++) {
		fprintf(out, "%s{ %u, %u },", i % 8 ? " " : "\n\t", info->push.words[i].ubo, info->push.words[i].offset);
	}
	fprintf(out, "%s\n};\n", info->push.count ? "" : " { 0, 0 }");
}

/* one program: its code, states and tables under NAME, then the struct hm_program that names them */
static void
program(const char *name, nir_shader *vs_nir, nir_shader *fs_nir, const enum pipe_format *attributes, unsigned nattr,
    bool blend_over)
{
	struct util_dynarray vs_bin, fs_bin;
	struct pan_shader_info vs, fs;
	char n[96];

	compile(vs_nir, &vs_bin, &vs);
	compile(fs_nir, &fs_bin, &fs);
	if (vs.vs.idvs || vs.vs.secondary_enable || vs.tls_size || fs.tls_size || vs.attribute_count != nattr) {
		fprintf(stderr, "bishaders: %s is not what the descriptors here expect (idvs %d, tls %u/%u, attributes %u)\n",
		    name, vs.vs.idvs, vs.tls_size, fs.tls_size, vs.attribute_count);
		exit(1);
	}
	struct mali_renderer_state_packed vs_rsd, fs_rsd;
	pan_pack(&vs_rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&vs, 0, &cfg);
	}
	fs_state(&fs, &fs_rsd);

	struct mali_blend_packed off, over;
	blend_desc(&fs, (struct pan_blend_equation){ .blend_enable = false, .color_mask = 0xf }, &off);
	blend_desc(&fs, (struct pan_blend_equation){
		.blend_enable = true, .rgb_func = PIPE_BLEND_ADD, .rgb_src_factor = PIPE_BLENDFACTOR_ONE,
		.rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA, .alpha_func = PIPE_BLEND_ADD,
		.alpha_src_factor = PIPE_BLENDFACTOR_ONE, .alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
		.color_mask = 0xf }, &over);

	fprintf(out, "\n/* --- %s: vertex shader %u bytes, %u work registers, preload %#llx; fragment shader %u bytes, %u "
	    "work registers, preload %#llx, blend register format %u --- */\n", name, vs_bin.size, vs.work_reg_count,
	    (unsigned long long)vs.preload, fs_bin.size, fs.work_reg_count, (unsigned long long)fs.preload,
	    fs.bifrost.blend[0].format);
	snprintf(n, sizeof(n), "hm_%s_vs_code", name); words(n, (const uint32_t *)vs_bin.data, vs_bin.size / 4);
	snprintf(n, sizeof(n), "hm_%s_fs_code", name); words(n, (const uint32_t *)fs_bin.data, fs_bin.size / 4);
	snprintf(n, sizeof(n), "hm_%s_vs_rsd", name); words(n, vs_rsd.opaque, 16);
	snprintf(n, sizeof(n), "hm_%s_fs_rsd", name); words(n, fs_rsd.opaque, 16);
	/* a blend that reads the destination keeps the GPU from killing the pixels it covers (pan_allow_forward_pixel_to_kill) */
	fs_rsd.opaque[4] &= ~(1u << 19);
	snprintf(n, sizeof(n), "hm_%s_fs_rsd_blended", name); words(n, fs_rsd.opaque, 16);
	snprintf(n, sizeof(n), "hm_%s_blend_off", name); words(n, off.opaque, 4);
	snprintf(n, sizeof(n), "hm_%s_blend_over", name); words(n, over.opaque, 4);
	snprintf(n, sizeof(n), "hm_%s_vs_push", name); pushes(n, &vs);
	snprintf(n, sizeof(n), "hm_%s_fs_push", name); pushes(n, &fs);
	snprintf(n, sizeof(n), "hm_%s_vs_out", name); varyings(n, vs.varyings.output, vs.varyings.output_count);
	snprintf(n, sizeof(n), "hm_%s_fs_in", name); varyings(n, fs.varyings.input, fs.varyings.input_count);
	fprintf(out, "static const uint32_t hm_%s_attribute_formats[%u] = {", name, nattr);
	for (unsigned i = 0; i < nattr; i++) {
		fprintf(out, " 0x%08x,", (unsigned)GENX(panfrost_format_from_pipe_format)(attributes[i])->hw);
	}
	fprintf(out, " };\nstatic const uint32_t hm_%s_attribute_sizes[%u] = {", name, nattr);
	for (unsigned i = 0; i < nattr; i++) {
		fprintf(out, " %u,", util_format_get_blocksize(attributes[i]));
	}
	fprintf(out, " };\n");
	fprintf(out, "static const struct hm_program hm_%s = {\n\t\"%s\",\n"
	    "\thm_%s_vs_code, sizeof(hm_%s_vs_code), hm_%s_fs_code, sizeof(hm_%s_fs_code),\n"
	    "\thm_%s_vs_rsd, hm_%s_fs_rsd, hm_%s_fs_rsd_blended, hm_%s_blend_off, hm_%s_blend_over,\n"
	    "\thm_%s_vs_push, %u, hm_%s_fs_push, %u,\n\thm_%s_vs_out, %u, hm_%s_fs_in, %u,\n"
	    "\thm_%s_attribute_formats, hm_%s_attribute_sizes, %u, %u, %u, %u,\n};\n",
	    name, name, name, name, name, name, name, name, name, name, name, name, vs.push.count, name, fs.push.count, name,
	    vs.varyings.output_count, name, fs.varyings.input_count, name, name, nattr, fs.texture_count, fs.sampler_count,
	    blend_over ? 1 : 0);
	(void)blend_over;
}

static nir_builder
shader(gl_shader_stage stage, const char *name)
{
	return nir_builder_init_simple_shader(stage, GENX(pan_shader_get_compiler_options)(), "%s", name);
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "shaders_v7.h";

	for (int sig = 1; sig < 32; sig++) {
		if (sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGTRAP || sig == SIGFPE) {
			signal(sig, fatal);
		}
	}
	glsl_type_singleton_init_or_ref();
	out = fopen(path, "w");
	if (out == NULL) {
		perror(path);
		return 1;
	}
	fprintf(out, "/*\n * Generated by tools/bishaders.c (tools/mk-shaders.sh): the library's shader programs, compiled by "
	    "Mesa 25.0.7's\n * Bifrost compiler for the Mali-G52 r1 (0x%x), with their Renderer State and Blend words as "
	    "Panfrost prepares\n * them, the words they push and the varyings they link. Do not edit.\n */\n#pragma once\n"
	    "#include \"program.h\"\n", GPU_ID);

	/* tri */
	{
		nir_builder v = shader(MESA_SHADER_VERTEX, "tri_vs");
		nir_variable *in = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_position", VERT_ATTRIB_GENERIC0, 0);
		nir_variable *pos = var(&v, nir_var_shader_out, glsl_vec4_type(), "gl_Position", VARYING_SLOT_POS, 0);
		v.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0);
		v.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS);
		nir_store_var(&v, pos, nir_load_var(&v, in), 0xf);

		nir_builder f = shader(MESA_SHADER_FRAGMENT, "tri_fs");
		nir_variable *color = var(&f, nir_var_shader_out, glsl_vec4_type(), "color", FRAG_RESULT_DATA0, 0);
		f.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		nir_store_var(&f, color, nir_imm_vec4(&f, 1.0f, 0.0f, 0.0f, 1.0f), 0xf);
		enum pipe_format attrs[] = { PIPE_FORMAT_R32G32B32A32_FLOAT };
		program("tri", v.shader, f.shader, attrs, 1, false);
	}
	/* color: a varying and a pushed uniform */
	{
		nir_builder v = shader(MESA_SHADER_VERTEX, "color_vs");
		nir_variable *in = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_position", VERT_ATTRIB_GENERIC0, 0);
		nir_variable *in_c = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_color", VERT_ATTRIB_GENERIC1, 1);
		nir_variable *pos = var(&v, nir_var_shader_out, glsl_vec4_type(), "gl_Position", VARYING_SLOT_POS, 0);
		nir_variable *col = var(&v, nir_var_shader_out, glsl_vec4_type(), "v_color", VARYING_SLOT_VAR0, 1);
		v.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0) | BITFIELD64_BIT(VERT_ATTRIB_GENERIC1);
		v.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS) | BITFIELD64_BIT(VARYING_SLOT_VAR0);
		nir_store_var(&v, pos, nir_load_var(&v, in), 0xf);
		nir_store_var(&v, col, nir_load_var(&v, in_c), 0xf);

		nir_builder f = shader(MESA_SHADER_FRAGMENT, "color_fs");
		nir_variable *vc = var(&f, nir_var_shader_in, glsl_vec4_type(), "v_color", VARYING_SLOT_VAR0, 0);
		nir_variable *color = var(&f, nir_var_shader_out, glsl_vec4_type(), "color", FRAG_RESULT_DATA0, 0);
		vc->data.interpolation = INTERP_MODE_SMOOTH;
		f.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_VAR0);
		f.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		f.shader->info.num_ubos = 1;
		f.shader->info.first_ubo_is_default_ubo = true;
		nir_def *tint = nir_load_ubo(&f, 4, 32, nir_imm_int(&f, 0), nir_imm_int(&f, 0), .align_mul = 16,
		    .align_offset = 0, .range_base = 0, .range = 16);
		nir_store_var(&f, color, nir_fmul(&f, nir_load_var(&f, vc), tint), 0xf);
		enum pipe_format attrs[] = { PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT };
		program("color", v.shader, f.shader, attrs, 2, true);
	}
	/* tex: a texture sampled at a varying */
	{
		nir_builder v = shader(MESA_SHADER_VERTEX, "tex_vs");
		nir_variable *in = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_position", VERT_ATTRIB_GENERIC0, 0);
		nir_variable *in_uv = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_uv", VERT_ATTRIB_GENERIC1, 1);
		nir_variable *pos = var(&v, nir_var_shader_out, glsl_vec4_type(), "gl_Position", VARYING_SLOT_POS, 0);
		nir_variable *uv = var(&v, nir_var_shader_out, glsl_vec_type(2), "v_uv", VARYING_SLOT_VAR0, 1);
		v.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0) | BITFIELD64_BIT(VERT_ATTRIB_GENERIC1);
		v.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS) | BITFIELD64_BIT(VARYING_SLOT_VAR0);
		nir_store_var(&v, pos, nir_load_var(&v, in), 0xf);
		nir_store_var(&v, uv, nir_trim_vector(&v, nir_load_var(&v, in_uv), 2), 0x3);

		nir_builder f = shader(MESA_SHADER_FRAGMENT, "tex_fs");
		nir_variable *vuv = var(&f, nir_var_shader_in, glsl_vec_type(2), "v_uv", VARYING_SLOT_VAR0, 0);
		nir_variable *color = var(&f, nir_var_shader_out, glsl_vec4_type(), "color", FRAG_RESULT_DATA0, 0);
		vuv->data.interpolation = INTERP_MODE_SMOOTH;
		f.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_VAR0);
		f.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		f.shader->info.num_textures = 1;
		BITSET_SET(f.shader->info.textures_used, 0);
		BITSET_SET(f.shader->info.samplers_used, 0);
		/* texture(t0, v_uv) as the state tracker leaves it: a tex instruction naming texture 0 and sampler 0 */
		nir_tex_instr *tex = nir_tex_instr_create(f.shader, 1);
		tex->op = nir_texop_tex;
		tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
		tex->dest_type = nir_type_float32;
		tex->coord_components = 2;
		tex->texture_index = 0;
		tex->sampler_index = 0;
		tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_load_var(&f, vuv));
		nir_def_init(&tex->instr, &tex->def, 4, 32);
		nir_builder_instr_insert(&f, &tex->instr);
		nir_store_var(&f, color, &tex->def, 0xf);
		enum pipe_format attrs[] = { PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT };
		program("tex", v.shader, f.shader, attrs, 2, false);
	}
	/* the texture format of RGBA8Unorm (GENX(panfrost_new_texture): GENX(panfrost_format_from_pipe_format)) and the
	 * identity swizzle (panfrost_translate_swizzle_4 of XYZW) */
	{
		const unsigned char identity[4] = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W };
		fprintf(out, "\n/* textures: RGBA8Unorm's format word and the identity swizzle */\n"
		    "static const uint32_t hm_texture_format_rgba8 = 0x%08x;\nstatic const uint32_t hm_texture_swizzle_identity = "
		    "0x%03x;\n", (unsigned)GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R8G8B8A8_UNORM)->hw,
		    panfrost_translate_swizzle_4(identity));
	}
	fclose(out);
	printf("bishaders: programs -> %s\n", path);
	return 0;
}
