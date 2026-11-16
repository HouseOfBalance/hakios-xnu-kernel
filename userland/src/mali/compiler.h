/*
 * The compiler inside the driver: the library's shader programs built as NIR and compiled by Mesa's Bifrost compiler in
 * this process, with what Panfrost prepares for them before a draw (struct hm_program, struct hm_kernel: program.h).
 * Linked against Mesa's own compiler libraries (libpanfrost_bifrost, libpan-arch-v7, libnir...), on the phone as on this
 * machine; tools/bishaders.c writes shaders_v7.h with it, so that what the phone compiles can be held word for word
 * against what was compiled here.
 */
#pragma once
#include <stddef.h>
#include "program.h"

#ifdef __cplusplus
extern "C" {
#endif

/* once, before the rest (glsl types) */
void hm_compile_init(void);
/* program NAME (tri, color, tex) into *OUT, its arrays newly allocated (hm_program_free); 0, or -1 with WHY */
int hm_compile_program(const char *name, unsigned gpu_id, struct hm_program *out, char *why, size_t why_size);
void hm_program_free(struct hm_program *p);
/* kernel NAME (fill) likewise */
int hm_compile_kernel(const char *name, unsigned gpu_id, struct hm_kernel *out, char *why, size_t why_size);
void hm_kernel_free(struct hm_kernel *k);
/* RGBA8Unorm's texture format word and the identity swizzle (GENX(panfrost_new_texture)) */
uint32_t hm_compile_texture_format_rgba8(void);
uint32_t hm_compile_swizzle_identity(void);

#ifdef __cplusplus
}
#endif
