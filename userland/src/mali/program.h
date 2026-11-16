/*
 * A shader program as tools/bishaders.c writes it into shaders_v7.h: a vertex and a fragment shader compiled by Mesa's
 * Bifrost compiler, and what Panfrost prepares for them before a draw -- their Renderer State words (the shader's
 * address left 0), the Blend descriptor of the one target, the 32-bit words each stage pushes (Fast Access Uniforms:
 * uniform buffer and byte offset of each), and the varyings to link (by location, as panfrost_emit_varying_descs
 * links them).
 */
#pragma once
#include <stdint.h>

#define HM_VARYING_SLOT_POS 0          /* gl_varying_slot: VARYING_SLOT_POS */
#define HM_VARYING_SLOT_VAR0 32        /* VARYING_SLOT_VAR0 */

struct hm_varying {
	uint32_t location;                  /* gl_varying_slot */
	uint32_t format;                    /* the Mali pixel format word (0 for the position: special) */
	uint32_t size;                      /* bytes a vertex */
};

struct hm_push_word {
	uint16_t ubo, offset;               /* struct panfrost_ubo_word */
};

struct hm_program {
	const char *name;
	const uint32_t *vs_code;
	uint32_t vs_size;
	const uint32_t *fs_code;
	uint32_t fs_size;
	const uint32_t *vs_rsd;             /* 16 words */
	const uint32_t *fs_rsd;             /* 16 words: blending off */
	const uint32_t *fs_rsd_blended;     /* the same with forward pixel kill off: the blend reads the destination */
	const uint32_t *blend_off;          /* 4 words */
	const uint32_t *blend_over;         /* 4 words: source over, premultiplied (One, OneMinusSourceAlpha) */
	const struct hm_push_word *vs_push;
	uint32_t vs_push_count;
	const struct hm_push_word *fs_push;
	uint32_t fs_push_count;
	const struct hm_varying *vs_out;
	uint32_t vs_out_count;
	const struct hm_varying *fs_in;
	uint32_t fs_in_count;
	const uint32_t *attribute_formats;
	const uint32_t *attribute_sizes;
	uint32_t attribute_count;
	uint32_t texture_count, sampler_count;
	uint32_t can_blend;
	const uint32_t *fs_rsd_depth;       /* 16 words: depth test Less, depth written (in the tile buffer) */
	uint32_t vs_work_registers, fs_work_registers;
	const uint32_t *blend_off_bgra;     /* the two Blend descriptors for a BGRA8Unorm target */
	const uint32_t *blend_over_bgra;
};

/* a compute kernel: its code, its Renderer State (the address left 0), the words it pushes, its workgroup size */
struct hm_kernel {
	const char *name;
	const uint32_t *code;
	uint32_t size;
	const uint32_t *rsd;
	const struct hm_push_word *push;
	uint32_t push_count;
	uint32_t local[3];
	uint32_t tls_size;                  /* bytes of stack a thread (0: none) */
	uint32_t work_registers;
};
