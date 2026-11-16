/*
 * The descriptors of v7.h, packed and unpacked by Mesa's generated genxml/v7_pack.h (gen_pack.py over v7.xml,
 * Mesa 25.0.7). This is the only file that includes it.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * The generated unpackers say a bit set outside every field with fprintf(stderr, "XXX: Invalid field of ...").
 * That sentence is the refusal: it is caught here instead of printed, and handed back through *why. The
 * printers (MALI_*_print) write to the FILE they are given and go through unchanged.
 */
static __thread char hm_invalid[160];
static int hm_fprintf(FILE *fp, const char *format, ...) __attribute__((format(printf, 2, 3)));
static int
hm_fprintf(FILE *fp, const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	if (fp == stderr) {
		if (hm_invalid[0] == 0) {
			n = vsnprintf(hm_invalid, sizeof(hm_invalid), format, ap);
			if (n > 0 && hm_invalid[strlen(hm_invalid) - 1] == '\n') {
				hm_invalid[strlen(hm_invalid) - 1] = 0;
			}
		} else {
			n = 0;
		}
	} else {
		n = vfprintf(fp, format, ap);
	}
	va_end(ap);
	return n;
}
#define fprintf hm_fprintf

/* (kept defined to the end of the file: the unpackers are macros, expanded where they are used) */
#include "genxml/v7_pack.h"

#include "util/rounding.h"
#include "v7.h"

/* v7.h's numbers are the generated enum's */
_Static_assert(HM_JOB_NULL == MALI_JOB_TYPE_NULL, "Job Type");
_Static_assert(HM_JOB_WRITE_VALUE == MALI_JOB_TYPE_WRITE_VALUE, "Job Type");
_Static_assert(HM_JOB_CACHE_FLUSH == MALI_JOB_TYPE_CACHE_FLUSH, "Job Type");
_Static_assert(HM_JOB_COMPUTE == MALI_JOB_TYPE_COMPUTE, "Job Type");
_Static_assert(HM_JOB_VERTEX == MALI_JOB_TYPE_VERTEX, "Job Type");
_Static_assert(HM_JOB_TILER == MALI_JOB_TYPE_TILER, "Job Type");
_Static_assert(HM_JOB_FRAGMENT == MALI_JOB_TYPE_FRAGMENT, "Job Type");

const struct hm_v7_sizes *
hm_v7_sizes(void)
{
	static const struct hm_v7_sizes sizes = {
		.job_header = pan_size(JOB_HEADER),
		.fragment_job = pan_size(FRAGMENT_JOB),
		.write_value_job = pan_size(WRITE_VALUE_JOB),
		.framebuffer = pan_size(FRAMEBUFFER),
		.render_target = pan_size(RENDER_TARGET),
		.zs_crc_extension = pan_size(ZS_CRC_EXTENSION),
		.tiler_context = pan_size(TILER_CONTEXT),
		.tiler_heap = pan_size(TILER_HEAP),
		.sample_positions = 256,
		.job_align = pan_alignment(JOB_HEADER),
		.framebuffer_align = pan_alignment(FRAMEBUFFER),
		.tiler_align = pan_alignment(TILER_CONTEXT),
		.vertex_job = pan_size(COMPUTE_JOB),
		.tiler_job = pan_size(TILER_JOB),
		.renderer_state = pan_size(RENDERER_STATE),
		.blend = pan_size(BLEND),
		.attribute = pan_size(ATTRIBUTE),
		.attribute_buffer = pan_size(ATTRIBUTE_BUFFER),
		.viewport = pan_size(VIEWPORT),
		.local_storage = pan_size(LOCAL_STORAGE),
	};
	return &sizes;
}

/* --- writing ---------------------------------------------------------------------------------------------- */

void
hm_v7_job_header(void *job, enum hm_job_type type, unsigned index, uint64_t next, bool barrier)
{
	pan_cast_and_pack(job, JOB_HEADER, cfg) {
		cfg.type = (enum mali_job_type)type;
		cfg.index = index;
		cfg.next = next;
		cfg.barrier = barrier;
	}
}

/* pan_jc.h's pan_jc_add_job writes a job's header this way; the payload is GENX(jm_emit_write_timestamp)'s with
 * an immediate in place of the timestamp */
void
hm_v7_write_value_job(void *job, unsigned index, uint64_t next, uint64_t target, uint32_t value)
{
	pan_section_pack(job, WRITE_VALUE_JOB, HEADER, cfg) {
		cfg.type = MALI_JOB_TYPE_WRITE_VALUE;
		cfg.index = index;
		cfg.next = next;
	}
	pan_section_pack(job, WRITE_VALUE_JOB, PAYLOAD, cfg) {
		cfg.address = target;
		cfg.type = MALI_WRITE_VALUE_TYPE_IMMEDIATE_32;
		cfg.immediate_value = value;
	}
}

uint32_t
hm_v7_rt_swizzle(enum hm_rt_format format)
{
	/* panfrost_translate_swizzle_4 of panfrost_invert_swizzle(desc->swizzle): R8G8B8A8 is XYZW, its own
	 * inverse; B8G8R8A8 is ZYXW, also its own inverse. Three bits a channel, MALI_CHANNEL_* */
	switch (format) {
	case HM_RT_RGBA8_UNORM:
		return MALI_CHANNEL_R | (MALI_CHANNEL_G << 3) | (MALI_CHANNEL_B << 6) | (MALI_CHANNEL_A << 9);
	case HM_RT_BGRA8_UNORM:
		return MALI_CHANNEL_B | (MALI_CHANNEL_G << 3) | (MALI_CHANNEL_R << 6) | (MALI_CHANNEL_A << 9);
	}
	return 0;
}

static bool
power_of_two(uint32_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

uint64_t
hm_v7_framebuffer(void *out, uint64_t gpu, const struct hm_v7_fb *fb, const struct hm_v7_rt *rts, const char **why)
{
	const char *no = NULL;

	if ((gpu & (pan_alignment(FRAMEBUFFER) - 1)) != 0) {
		no = "the framebuffer descriptor is not 64-byte aligned";
	} else if (fb->width == 0 || fb->height == 0 || fb->width > 65536 || fb->height > 65536) {
		no = "the frame's size is outside 1..65536";
	} else if (fb->bound_max_x >= fb->width || fb->bound_max_y >= fb->height || fb->bound_min_x > fb->bound_max_x ||
	    fb->bound_min_y > fb->bound_max_y) {
		no = "the frame's bounds are not inside it";
	} else if (!power_of_two(fb->tile_size) || fb->tile_size < 16 || fb->tile_size > 16 * 16) {
		no = "the effective tile size is not a power of two in 16..256 (panfrost_max_effective_tile_size)";
	} else if (fb->cbuf_allocation == 0 || (fb->cbuf_allocation & 1023) != 0 || fb->cbuf_allocation > 255 * 1024) {
		no = "the colour buffer allocation is not a non-zero multiple of 1 KB";
	} else if (fb->rt_count < 1 || fb->rt_count > 8) {
		no = "the render target count is outside 1..8";
	} else if (fb->sample_locations == 0) {
		no = "no sample locations (Bifrost always reads them: pan_jm.c)";
	}
	for (unsigned i = 0; no == NULL && i < fb->rt_count; i++) {
		if (rts[i].format != HM_RT_RGBA8_UNORM && rts[i].format != HM_RT_BGRA8_UNORM) {
			no = "a render target's format is not one this library knows";
		} else if ((rts[i].internal_buffer_offset & 15) != 0) {
			no = "a render target's tile buffer offset is not a multiple of 16";
		} else if (rts[i].write_enable && rts[i].base == 0) {
			no = "a render target that is written has no memory";
		}
	}
	if (no != NULL) {
		if (why != NULL) {
			*why = no;
		}
		return 0;
	}

	/* GENX(pan_emit_fbd), the PAN_ARCH 6/7 branches, for a frame with no depth or stencil and no CRC */
	pan_section_pack(out, FRAMEBUFFER, PARAMETERS, cfg) {
		cfg.sample_locations = fb->sample_locations;
		cfg.pre_frame_0 = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
		cfg.pre_frame_1 = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
		cfg.post_frame = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
		cfg.frame_shader_dcds = 0;      /* no preload: pan_fb_preload.c makes none for a cleared frame */
		cfg.tiler = fb->tiler;
		cfg.width = fb->width;
		cfg.height = fb->height;
		cfg.bound_min_x = fb->bound_min_x;
		cfg.bound_min_y = fb->bound_min_y;
		cfg.bound_max_x = fb->bound_max_x;
		cfg.bound_max_y = fb->bound_max_y;
		cfg.effective_tile_size = fb->tile_size;
		cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
		cfg.render_target_count = fb->rt_count;
		cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D24;    /* "Default to 24 bit depth if there's no surface" */
		cfg.z_clear = 0.0f;
		cfg.s_clear = 0;
		cfg.color_buffer_allocation = fb->cbuf_allocation;
		cfg.sample_count = 1;
		cfg.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
		cfg.z_write_enable = false;
		cfg.s_write_enable = false;
		cfg.has_zs_crc_extension = false;
	}
	pan_section_pack(out, FRAMEBUFFER, PADDING, padding) {
		(void)padding;
	}

	/* pan_emit_rt -> pan_prepare_rt, linear (DRM_FORMAT_MOD_LINEAR), single-sampled, layer 0 */
	for (unsigned i = 0; i < fb->rt_count; i++) {
		const struct hm_v7_rt *rt = &rts[i];
		struct mali_render_target_packed *rtd =
		    (struct mali_render_target_packed *)((uint8_t *)out + pan_size(FRAMEBUFFER) + i * pan_size(RENDER_TARGET));

		pan_pack(rtd, RENDER_TARGET, cfg) {
			cfg.clean_pixel_write_enable = rt->clean_pixel_write;
			cfg.internal_buffer_offset = rt->internal_buffer_offset;
			if (rt->clear) {
				cfg.clear.color_0 = rt->clear_words[0];
				cfg.clear.color_1 = rt->clear_words[1];
				cfg.clear.color_2 = rt->clear_words[2];
				cfg.clear.color_3 = rt->clear_words[3];
			}
			cfg.write_enable = rt->write_enable;
			cfg.dithering_enable = rt->dithering;
			cfg.writeback_msaa = MALI_MSAA_SINGLE;
			cfg.srgb = false;
			cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
			cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
			cfg.swizzle = hm_v7_rt_swizzle(rt->format);
			cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
			cfg.rgb.base = rt->base;
			cfg.rgb.row_stride = rt->row_stride;
			cfg.rgb.surface_stride = 0;     /* "Only set layer_stride for layered MSAA rendering" */
		}
	}

	struct mali_framebuffer_pointer_packed tag;
	pan_pack(&tag, FRAMEBUFFER_POINTER, cfg) {
		cfg.zs_crc_extension_present = false;
		cfg.render_target_count = fb->rt_count;
	}
	return gpu | tag.opaque[0];
}

void
hm_v7_fragment_job(void *job, unsigned index, uint64_t next, uint64_t fbd_tagged, unsigned min_tx, unsigned min_ty,
    unsigned max_tx, unsigned max_ty)
{
	/* GENX(pan_emit_fragment_job_payload) and GENX(jm_emit_fragment_job) */
	pan_section_pack(job, FRAGMENT_JOB, PAYLOAD, payload) {
		payload.bound_min_x = min_tx;
		payload.bound_min_y = min_ty;
		payload.bound_max_x = max_tx;
		payload.bound_max_y = max_ty;
		payload.framebuffer = fbd_tagged;
	}
	pan_section_pack(job, FRAGMENT_JOB, HEADER, header) {
		header.type = MALI_JOB_TYPE_FRAGMENT;
		header.index = index;
		header.next = next;
	}
}

void
hm_v7_tiler_heap(void *out, uint64_t base, uint32_t size)
{
	pan_cast_and_pack(out, TILER_HEAP, heap) {
		heap.size = size;
		heap.base = base;
		heap.bottom = base;
		heap.top = base + size;
	}
}

void
hm_v7_tiler_context(void *out, uint64_t heap, uint32_t fb_width, uint32_t fb_height, uint32_t hierarchy_mask)
{
	pan_cast_and_pack(out, TILER_CONTEXT, tiler) {
		tiler.hierarchy_mask = hierarchy_mask;
		tiler.fb_width = fb_width;
		tiler.fb_height = fb_height;
		tiler.heap = heap;
		tiler.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
	}
}

uint32_t
hm_v7_hierarchy_mask(uint32_t width, uint32_t height, unsigned max_levels)
{
	uint32_t max_fb_wh = width > height ? width : height;
	uint32_t last_hierarchy_bit = util_last_bit((max_fb_wh + 15) / 16);
	uint32_t mask = BITFIELD_MASK(max_levels);

	if (last_hierarchy_bit > max_levels) {
		mask <<= last_hierarchy_bit - max_levels;
	}
	return mask;
}

void
hm_v7_sample_positions(void *out)
{
	/* pan_samples.c: SAMPLE4(0, 0) = SAMPLE16(0, 0) = ((0 + 8) * (256 / 16)) in x and y, for position 0 and for the
	 * origin at index 32; the rest of the 64 entries zero */
	uint16_t *p = (uint16_t *)out;

	memset(out, 0, 256);
	p[0] = p[1] = 8 * (256 / 16);
	p[64] = p[65] = 8 * (256 / 16);
}

void
hm_v7_tile_size(unsigned bytes_per_pixel, unsigned tib_budget, uint32_t *tile_size, uint32_t *cbuf_allocation)
{
	/* GENX(pan_select_tile_size): budget >> log2ceil(bpp), at most 16 x 16 on v7; the allocation 1 KB aligned */
	unsigned size = tib_budget >> util_logbase2_ceil(bytes_per_pixel);

	if (size > 16 * 16) {
		size = 16 * 16;
	}
	*tile_size = size;
	*cbuf_allocation = ALIGN_POT(bytes_per_pixel * size, 1024);
}

static uint32_t
unorm8(float f)
{
	/* pan_clear.c float_to_fixed(f, 8, 0, false): roundeven(f * 255), after SATURATE */
	if (!(f > 0.0f)) {
		f = 0.0f;
	}
	if (f > 1.0f) {
		f = 1.0f;
	}
	return (uint32_t)_mesa_roundevenf(f * 255.0f);
}

void
hm_v7_pack_clear_rgba8(const float rgba[4], uint32_t out[4])
{
	uint32_t word = unorm8(rgba[0]) | (unorm8(rgba[1]) << 8) | (unorm8(rgba[2]) << 16) | (unorm8(rgba[3]) << 24);

	for (unsigned i = 0; i < 4; i++) {
		out[i] = word;
	}
}

/* --- a draw ---------------------------------------------------------------------------------------------- */

void
hm_v7_attribute(void *out, unsigned buffer_index, uint32_t format, int32_t offset, bool offset_enable)
{
	pan_cast_and_pack(out, ATTRIBUTE, cfg) {
		cfg.buffer_index = buffer_index;
		cfg.offset_enable = offset_enable;
		cfg.format = format;
		cfg.offset = offset;
	}
}

void
hm_v7_attribute_buffer(void *out, uint64_t pointer, uint32_t stride, uint32_t size)
{
	if (pointer == 0 && stride == 0 && size == 0) {
		memset(out, 0, pan_size(ATTRIBUTE_BUFFER));
		return;
	}
	pan_cast_and_pack(out, ATTRIBUTE_BUFFER, cfg) {
		cfg.pointer = pointer;
		cfg.stride = stride;
		cfg.size = size;
	}
}

uint32_t
hm_v7_position_format(void)
{
	return MALI_SNAP_4 << 12;
}

void
hm_v7_viewport(void *out, unsigned min_x, unsigned min_y, unsigned max_x, unsigned max_y, float min_z, float max_z)
{
	pan_cast_and_pack(out, VIEWPORT, cfg) {
		cfg.scissor_minimum_x = min_x;
		cfg.scissor_minimum_y = min_y;
		cfg.scissor_maximum_x = max_x;
		cfg.scissor_maximum_y = max_y;
		cfg.minimum_z = min_z;
		cfg.maximum_z = max_z;
	}
}

void
hm_v7_local_storage(void *out)
{
	pan_cast_and_pack(out, LOCAL_STORAGE, cfg) {
		cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
	}
}

static void
pack_invocation(struct mali_invocation_packed *out, unsigned vertex_count)
{
	/* GENX(jm_launch_draw), instance_count 1 */
	pan_pack(out, INVOCATION, cfg) {
		cfg.invocations = vertex_count - 1;
		cfg.size_y_shift = 0;
		cfg.size_z_shift = 0;
		cfg.workgroups_x_shift = 0;
		cfg.workgroups_y_shift = 0;
		cfg.workgroups_z_shift = 32;
		cfg.thread_group_split = MALI_SPLIT_MIN_EFFICIENT;
	}
}

void
hm_v7_vertex_job(void *job, unsigned index, uint64_t next, unsigned vertex_count, const struct hm_v7_draw *draw)
{
	pack_invocation(pan_section_ptr(job, COMPUTE_JOB, INVOCATION), vertex_count);
	pan_section_pack(job, COMPUTE_JOB, PARAMETERS, cfg) {
		cfg.job_task_split = 5;
	}
	/* jm_emit_vertex_draw */
	pan_section_pack(job, COMPUTE_JOB, DRAW, cfg) {
		cfg.state = draw->state;
		cfg.attributes = draw->attributes;
		cfg.attribute_buffers = draw->attribute_buffers;
		cfg.varyings = draw->varyings;
		cfg.varying_buffers = draw->varyings ? draw->varying_buffers : 0;
		cfg.thread_storage = draw->thread_storage;
		cfg.offset_start = 0;
		cfg.instance_size = 1;
	}
	pan_section_pack(job, COMPUTE_JOB, HEADER, cfg) {
		cfg.type = MALI_JOB_TYPE_VERTEX;
		cfg.index = index;
		cfg.next = next;
	}
}

void
hm_v7_tiler_job(void *job, unsigned index, unsigned dependency, uint64_t next, unsigned vertex_count, uint64_t tiler,
    const struct hm_v7_draw *draw)
{
	pack_invocation(pan_section_ptr(job, TILER_JOB, INVOCATION), vertex_count);
	/* jm_emit_primitive */
	pan_section_pack(job, TILER_JOB, PRIMITIVE, cfg) {
		cfg.draw_mode = MALI_DRAW_MODE_TRIANGLES;
		cfg.first_provoking_vertex = false;     /* rast->flatshade_first */
		cfg.job_task_split = 6;
		cfg.low_depth_cull = true;              /* rast->depth_clip_near */
		cfg.high_depth_cull = true;             /* rast->depth_clip_far */
		cfg.index_count = vertex_count;
		cfg.index_type = MALI_INDEX_TYPE_NONE;
		cfg.secondary_shader = false;
	}
	/* panfrost_emit_primitive_size: not points, so the constant (the line width) */
	pan_section_pack(job, TILER_JOB, PRIMITIVE_SIZE, cfg) {
		cfg.constant = 1.0f;
	}
	pan_section_pack(job, TILER_JOB, TILER, cfg) {
		cfg.address = tiler;
	}
	pan_section_pack(job, TILER_JOB, PADDING, cfg) {
		(void)cfg;
	}
	/* jm_emit_tiler_draw, v7 */
	pan_section_pack(job, TILER_JOB, DRAW, cfg) {
		cfg.cull_front_face = false;
		cfg.cull_back_face = false;
		cfg.front_face_ccw = false;
		cfg.position = draw->position;
		cfg.state = draw->state;
		cfg.attributes = draw->attributes;
		cfg.attribute_buffers = draw->attribute_buffers;
		cfg.viewport = draw->viewport;
		cfg.varyings = draw->varyings;
		cfg.varying_buffers = draw->varyings ? draw->varying_buffers : 0;
		cfg.thread_storage = draw->thread_storage;
		cfg.offset_start = 0;
		cfg.instance_size = 1;
	}
	pan_section_pack(job, TILER_JOB, HEADER, cfg) {
		cfg.type = MALI_JOB_TYPE_TILER;
		cfg.index = index;
		cfg.dependency_1 = dependency;
		cfg.next = next;
	}
}

/* --- reading ---------------------------------------------------------------------------------------------- */

static int
said(const char **why)
{
	if (hm_invalid[0] == 0) {
		return 0;
	}
	if (why != NULL) {
		static __thread char kept[160];

		strlcpy(kept, hm_invalid, sizeof(kept));
		*why = kept;
	}
	hm_invalid[0] = 0;
	return -1;
}

int
hm_v7_read_job(const void *job, struct hm_v7_job *out, const char **why)
{
	hm_invalid[0] = 0;
	pan_unpack((const struct mali_job_header_packed *)job, JOB_HEADER, h);
	out->exception_status = h.exception_status;
	out->first_incomplete_task = h.first_incomplete_task;
	out->fault_pointer = h.fault_pointer;
	out->is_64b = h.is_64b;
	out->barrier = h.barrier;
	out->type = h.type;
	out->index = h.index;
	out->dependency_1 = h.dependency_1;
	out->dependency_2 = h.dependency_2;
	out->next = h.next;
	return said(why);
}

int
hm_v7_read_fragment(const void *job, struct hm_v7_fragment *out, const char **why)
{
	hm_invalid[0] = 0;
	pan_section_unpack(job, FRAGMENT_JOB, PAYLOAD, p);
	out->min_tx = p.bound_min_x;
	out->min_ty = p.bound_min_y;
	out->max_tx = p.bound_max_x;
	out->max_ty = p.bound_max_y;
	out->has_tile_enable_map = p.has_tile_enable_map;
	out->framebuffer = p.framebuffer;
	return said(why);
}

int
hm_v7_read_write_value(const void *job, struct hm_v7_write_value *out, const char **why)
{
	hm_invalid[0] = 0;
	pan_section_unpack(job, WRITE_VALUE_JOB, PAYLOAD, p);
	out->address = p.address;
	out->type = p.type;
	out->immediate = p.immediate_value;
	return said(why);
}

void
hm_v7_read_fbd_pointer(uint64_t tagged, struct hm_v7_fbd_pointer *out)
{
	struct mali_framebuffer_pointer_packed packed = { { (uint32_t)tagged, (uint32_t)(tagged >> 32) } };

	hm_invalid[0] = 0;
	pan_unpack(&packed, FRAMEBUFFER_POINTER, p);
	out->type = p.type;
	out->zs_crc_extension = p.zs_crc_extension_present;
	out->rt_count = p.render_target_count;
	out->pointer = p.pointer;
	hm_invalid[0] = 0;
}

int
hm_v7_read_framebuffer(const void *fbd, struct hm_v7_fb_read *out, const char **why)
{
	hm_invalid[0] = 0;
	pan_section_unpack(fbd, FRAMEBUFFER, PARAMETERS, p);
	memset(out, 0, sizeof(*out));
	out->fb.width = p.width;
	out->fb.height = p.height;
	out->fb.bound_min_x = p.bound_min_x;
	out->fb.bound_min_y = p.bound_min_y;
	out->fb.bound_max_x = p.bound_max_x;
	out->fb.bound_max_y = p.bound_max_y;
	out->fb.sample_locations = p.sample_locations;
	out->fb.tiler = p.tiler;
	out->fb.tile_size = p.effective_tile_size;
	out->fb.cbuf_allocation = p.color_buffer_allocation;
	out->fb.rt_count = p.render_target_count;
	out->pre_frame_0 = p.pre_frame_0;
	out->pre_frame_1 = p.pre_frame_1;
	out->post_frame = p.post_frame;
	out->frame_shader_dcds = p.frame_shader_dcds;
	out->sample_count = p.sample_count;
	out->sample_pattern = p.sample_pattern;
	out->effective_tile_size = p.effective_tile_size;
	out->has_zs_crc_extension = p.has_zs_crc_extension;
	out->z_write_enable = p.z_write_enable;
	out->s_write_enable = p.s_write_enable;
	out->crc_read_enable = p.crc_read_enable;
	out->crc_write_enable = p.crc_write_enable;
	if (said(why) != 0) {
		return -1;
	}
	pan_section_unpack(fbd, FRAMEBUFFER, PADDING, pad);
	(void)pad;
	return said(why);
}

int
hm_v7_read_rt(const void *rt, struct hm_v7_rt_read *out, const char **why)
{
	hm_invalid[0] = 0;
	pan_unpack((const struct mali_render_target_packed *)rt, RENDER_TARGET, r);
	memset(out, 0, sizeof(*out));
	out->rt.base = r.rgb.base;
	out->rt.row_stride = r.rgb.row_stride;
	out->rt.internal_buffer_offset = r.internal_buffer_offset;
	out->rt.write_enable = r.write_enable;
	out->rt.clean_pixel_write = r.clean_pixel_write_enable;
	out->rt.dithering = r.dithering_enable;
	out->rt.clear_words[0] = r.clear.color_0;
	out->rt.clear_words[1] = r.clear.color_1;
	out->rt.clear_words[2] = r.clear.color_2;
	out->rt.clear_words[3] = r.clear.color_3;
	out->internal_format = r.internal_format;
	out->writeback_format = r.writeback_format;
	out->writeback_block_format = r.writeback_block_format;
	out->writeback_msaa = r.writeback_msaa;
	out->swizzle = r.swizzle;
	out->yuv_enable = r.yuv_enable;
	out->srgb = r.srgb;
	out->dithered_clear = r.dithered_clear;
	out->surface_stride = r.rgb.surface_stride;
	if (r.swizzle == hm_v7_rt_swizzle(HM_RT_RGBA8_UNORM)) {
		out->rt.format = HM_RT_RGBA8_UNORM;
	} else if (r.swizzle == hm_v7_rt_swizzle(HM_RT_BGRA8_UNORM)) {
		out->rt.format = HM_RT_BGRA8_UNORM;
	}
	return said(why);
}

int
hm_v7_read_tiler_context(const void *ctx, struct hm_v7_tiler_read *out, const char **why)
{
	hm_invalid[0] = 0;
	pan_unpack((const struct mali_tiler_context_packed *)ctx, TILER_CONTEXT, t);
	out->polygon_list = t.polygon_list;
	out->heap = t.heap;
	out->hierarchy_mask = t.hierarchy_mask;
	out->fb_width = t.fb_width;
	out->fb_height = t.fb_height;
	out->sample_pattern = t.sample_pattern;
	return said(why);
}

int
hm_v7_read_tiler_heap(const void *heap, struct hm_v7_tiler_read *out, const char **why)
{
	hm_invalid[0] = 0;
	pan_unpack((const struct mali_tiler_heap_packed *)heap, TILER_HEAP, h);
	out->heap_base = h.base;
	out->heap_bottom = h.bottom;
	out->heap_top = h.top;
	out->heap_size = h.size;
	return said(why);
}

int
hm_v7_read_draw(const void *job, bool tiler, struct hm_v7_draw_read *out, const char **why)
{
	hm_invalid[0] = 0;
	memset(out, 0, sizeof(*out));
	if (tiler) {
		pan_section_unpack(job, TILER_JOB, INVOCATION, inv);
		pan_section_unpack(job, TILER_JOB, PRIMITIVE, prim);
		pan_section_unpack(job, TILER_JOB, TILER, t);
		pan_section_unpack(job, TILER_JOB, DRAW, d);
		out->invocations = inv.invocations;
		out->draw_mode = prim.draw_mode;
		out->index_count = prim.index_count;
		out->index_type = prim.index_type;
		out->tiler = t.address;
		out->draw.state = d.state;
		out->draw.attributes = d.attributes;
		out->draw.attribute_buffers = d.attribute_buffers;
		out->draw.varyings = d.varyings;
		out->draw.varying_buffers = d.varying_buffers;
		out->draw.position = d.position;
		out->draw.viewport = d.viewport;
		out->draw.thread_storage = d.thread_storage;
	} else {
		pan_section_unpack(job, COMPUTE_JOB, INVOCATION, inv);
		pan_section_unpack(job, COMPUTE_JOB, DRAW, d);
		out->invocations = inv.invocations;
		out->draw.state = d.state;
		out->draw.attributes = d.attributes;
		out->draw.attribute_buffers = d.attribute_buffers;
		out->draw.varyings = d.varyings;
		out->draw.varying_buffers = d.varying_buffers;
		out->draw.position = d.position;
		out->draw.viewport = d.viewport;
		out->draw.thread_storage = d.thread_storage;
	}
	return said(why);
}

void
hm_v7_set_exception_status(void *job, uint32_t status)
{
	((struct mali_job_header_packed *)job)->opaque[0] = status;
}

void
hm_v7_print_job(FILE *fp, const void *job)
{
	hm_invalid[0] = 0;
	pan_unpack((const struct mali_job_header_packed *)job, JOB_HEADER, h);
	MALI_JOB_HEADER_print(fp, &h, 2);
	if (h.type == MALI_JOB_TYPE_FRAGMENT) {
		pan_section_unpack(job, FRAGMENT_JOB, PAYLOAD, p);
		MALI_FRAGMENT_JOB_PAYLOAD_print(fp, &p, 2);
	} else if (h.type == MALI_JOB_TYPE_WRITE_VALUE) {
		pan_section_unpack(job, WRITE_VALUE_JOB, PAYLOAD, p);
		MALI_WRITE_VALUE_JOB_PAYLOAD_print(fp, &p, 2);
	}
	hm_invalid[0] = 0;
}

void
hm_v7_print_framebuffer(FILE *fp, const void *fbd, unsigned rt_count)
{
	hm_invalid[0] = 0;
	pan_section_unpack(fbd, FRAMEBUFFER, PARAMETERS, p);
	MALI_FRAMEBUFFER_PARAMETERS_print(fp, &p, 2);
	for (unsigned i = 0; i < rt_count && i < 8; i++) {
		pan_unpack((const struct mali_render_target_packed *)((const uint8_t *)fbd + pan_size(FRAMEBUFFER) +
		    i * pan_size(RENDER_TARGET)), RENDER_TARGET, r);
		fprintf(fp, "  Render Target %u:\n", i);
		MALI_RENDER_TARGET_print(fp, &r, 4);
	}
	hm_invalid[0] = 0;
}
