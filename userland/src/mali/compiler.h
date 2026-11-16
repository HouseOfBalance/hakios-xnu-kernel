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
/*
 * A compute kernel of Apple's: function ENTRY of the .metallib LIB (AIR, LLVM bitcode), translated to NIR by the AIR
 * frontend (tools/mesa/air, nir_air.h), its device buffers made global memory at addresses the driver pushes
 * (uniform buffer HM_AIR_SYSVAL_UBO, 8 bytes a Metal buffer index), compiled by Mesa's Bifrost compiler. *OUT as
 * hm_compile_kernel's; *DEVICE_BUFFERS the Metal indices of the device buffers it reads or writes (bit n: buffer n),
 * *CONSTANT_BUFFERS those it reads as constants (uniform buffer n). 0, or -1 with WHY (the frontend's refusal by
 * category, or the compiler's).
 */
#define HM_AIR_SYSVAL_UBO 15
#define HM_AIR_SYSVAL_GRID 256       /* uint3: threadgroups in the grid */
#define HM_AIR_SYSVAL_LOCAL 272      /* uint3: threads in a threadgroup */
#define HM_AIR_SYSVAL_SIZE 288
int hm_compile_air_kernel(const void *lib, size_t size, const char *entry, unsigned gpu_id, struct hm_kernel *out,
    uint32_t *device_buffers, uint32_t *constant_buffers, char *why, size_t why_size);
/* the names of the compute functions of the .metallib LIB, NUL-separated into NAMES (at most SIZE bytes): how many */
int hm_air_kernels(const void *lib, size_t size, char *names, size_t names_size);

/* RGBA8Unorm's texture format word and the identity swizzle (GENX(panfrost_new_texture)) */
uint32_t hm_compile_texture_format_rgba8(void);
uint32_t hm_compile_swizzle_identity(void);

#ifdef __cplusplus
}
#endif
