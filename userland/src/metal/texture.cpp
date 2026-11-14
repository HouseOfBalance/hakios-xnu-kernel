/*
 * A texture: the compiler library's (struct rwa_texture), its shape, and the two roads between it and
 * an IOSurface the Mali cannot hand over as GPU memory -- copied IN from a surface another process
 * draws, copied BACK into a surface the window server shows. Which thread does the copying, and what
 * is said when it fails, is the plug-in's (HakiosMetalTexture in src/hakiosmetal.m).
 */
#include "rwm.h"

#include <IOKit/IOReturn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#include <memory>
#include <new>

struct rwm_texture {
	struct rwa_texture *texture = nullptr;
	struct rwa_texture_shape shape = {};
	IOSurfaceRef surface = nullptr;   /* the copy-back road's (rwm_texture_sync_surface) */
	IOSurfaceRef source = nullptr;    /* the copy-in road's (rwm_texture_copy_in) */
	uint32_t source_seed = 0;         /* the source's seed when it was last copied in */
	bool source_two_planes = false;   /* ... and it is YCbCr 4:2:0 in two planes, packed into one */
	uint32_t in_place = 0;            /* the surface it lies over, drawn in place (src/metal/import.cpp) */
	/* the copy-in road from memory (-newTextureWithBytesNoCopy:...): the caller's bytes, re-copied before a use
	 * whenever their signature has moved -- the caller fills them after it makes the texture */
	const uint8_t *memory = nullptr;
	size_t memory_length = 0, memory_stride = 0;
	unsigned memory_layers = 1;
	uint64_t memory_signature = 0;
	bool memory_written = false;
	/* for rwm_texture_sample_report: where it came from (0 made here, 1 over a surface in place, 2 copied in from a
	 * surface, 3 copied in from memory, 4 a view) and whether anything ever went into it */
	uint8_t origin = 0;
	bool filled = false;
	int8_t surface_empty = -1;        /* origin 1: its surface's memory all zeroes when first sampled (1), or not (0) */
	unsigned samples = 0;             /* draws that sampled it (texshot) */
	uint8_t fill_kind = 0;            /* what filled it (texshot): 1 drawn into, 2 written, 4 copied in from memory, 8 a view */

	~rwm_texture()
	{
		if (texture != nullptr) {
			rwm_air()->texture_destroy(texture);
		}
		if (surface != nullptr) {
			CFRelease(surface);
		}
		if (source != nullptr) {
			CFRelease(source);
		}
	}
};

namespace {

struct free_deleter {
	void operator()(void *p) const { free(p); }
};
using block = std::unique_ptr<uint8_t, free_deleter>;

/* a lock held for as long as the copy reads the surface */
class surface_lock {
public:
	surface_lock(IOSurfaceRef surface, IOSurfaceLockOptions options)
		: surface_(surface), options_(options),
		  held_(IOSurfaceLock(surface, options, nullptr) == kIOReturnSuccess) {}
	~surface_lock()
	{
		if (held_) {
			IOSurfaceUnlock(surface_, options_, nullptr);
		}
	}
	bool held() const { return held_; }

private:
	IOSurfaceRef surface_;
	IOSurfaceLockOptions options_;
	bool held_;
};

/*
 * A plane of a surface as a copy reads it. The surface is another process's, and its size is its own:
 * the texture's shape says how much is read, and nothing here reads before it is shown that the plane
 * and the surface's allocation hold that much (the automated review's finding, 31 October: the copy
 * trusted the shape). A surface without planes answers the plane calls with zero, so it is read
 * through the whole-surface calls, as the copy-back does.
 */
struct plane {
	const uint8_t *base = nullptr;
	size_t row = 0, width = 0, height = 0, element = 0;
};

plane
plane_of(IOSurfaceRef surface, size_t index)
{
	const size_t planes = IOSurfaceGetPlaneCount(surface);

	if (planes == 0) {
		if (index != 0) {
			return plane{};
		}
		return plane{ (const uint8_t *)IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface),
			      IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
			      IOSurfaceGetBytesPerElement(surface) };
	}
	if (index >= planes) {
		return plane{};
	}
	return plane{ (const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, index),
		      IOSurfaceGetBytesPerRowOfPlane(surface, index), IOSurfaceGetWidthOfPlane(surface, index),
		      IOSurfaceGetHeightOfPlane(surface, index), IOSurfaceGetBytesPerElementOfPlane(surface, index) };
}

/* `rows` rows of `bytes` bytes, a row apart from the plane's start: inside the plane and the allocation */
bool
covers(IOSurfaceRef surface, const plane &p, size_t bytes, size_t rows)
{
	const uint8_t *start = (const uint8_t *)IOSurfaceGetBaseAddress(surface);
	const size_t alloc = IOSurfaceGetAllocSize(surface);
	size_t end;

	if (p.base == nullptr || start == nullptr || p.base < start || bytes == 0 || rows == 0 ||
	    bytes > p.row || rows > p.height) {
		return false;
	}
	if (__builtin_mul_overflow(rows - 1, p.row, &end) || __builtin_add_overflow(end, bytes, &end) ||
	    __builtin_add_overflow(end, (size_t)(p.base - start), &end)) {
		return false;
	}
	return end <= alloc;
}

/* the bytes of a texel of the formats the plug-in copies a surface into (HakiosMetalDevice's import:
 * MTLPixelFormatBGRA8Unorm, its sRGB twin, RGBA8Unorm, RG8Unorm, R8Unorm), 0 for any other */
size_t
texel_bytes(unsigned format)
{
	switch (format) {
	case 80: case 81: case 70:
		return 4;
	case 30:
		return 2;
	case 10:
		return 1;
	}
	return 0;
}

} // namespace

namespace {

/* a texture's kilobytes as made, roughly: 8 bytes a texel for the 64-bit formats, 4 for the rest */
long
texture_kb(const struct rwa_texture_shape &sh)
{
	const long texel = sh.pixel_format == 115 || sh.pixel_format == 105 || sh.pixel_format == 125 ? 8 : 4;

	return ((long)sh.width * (sh.height ? sh.height : 1) * (sh.depth ? sh.depth : 1) *
		(sh.array_length ? sh.array_length : 1) * texel) >> 10;
}

} // namespace

extern "C" struct rwm_texture *
rwm_texture_new(struct rwa_texture *texture, const struct rwa_texture_shape *shape)
{
	rwm_texture *made = new (std::nothrow) rwm_texture;

	if (made == nullptr) {
		rwm_air()->texture_destroy(texture);
		return nullptr;
	}
	made->texture = texture;
	made->shape = *shape;
	__atomic_add_fetch(&rwm_live_textures, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&rwm_live_texture_kb, texture_kb(*shape), __ATOMIC_RELAXED);
	return made;
}

extern "C" void
rwm_texture_free(struct rwm_texture *texture)
{
	if (texture != nullptr) {
		__atomic_sub_fetch(&rwm_live_textures, 1, __ATOMIC_RELAXED);
		__atomic_sub_fetch(&rwm_live_texture_kb, texture_kb(texture->shape), __ATOMIC_RELAXED);
		__atomic_add_fetch(&rwm_textures_freed, 1, __ATOMIC_RELAXED);
	}
	delete texture;
}

extern "C" struct rwa_texture *
rwm_texture_mesa(const struct rwm_texture *texture)
{
	return texture->texture;
}

extern "C" const struct rwa_texture_shape *
rwm_texture_shape(const struct rwm_texture *texture)
{
	return &texture->shape;
}

/*
 * The copy-IN road: a surface another process draws (a widget's, a layer's backing store) that cannot
 * be laid under a texture as it is -- rows not a multiple of 64 bytes, or a format the import does not
 * serve ('LA08') -- is copied into the library's own texture, again before every command buffer that
 * samples it once the surface has changed (its seed). Before, such a texture was made empty and stayed
 * so: the widgets' solid squares (24 October).
 */
extern "C" void
rwm_texture_copy_in_from(struct rwm_texture *texture, IOSurfaceRef surface, bool two_planes)
{
	if (texture->source != nullptr) {
		CFRelease(texture->source);
	}
	texture->source = (IOSurfaceRef)CFRetain(surface);
	texture->origin = 2;
	texture->source_seed = IOSurfaceGetSeed(surface) - 1;
	texture->source_two_planes = two_planes;
}

extern "C" void
rwm_texture_copy_in_from_memory(struct rwm_texture *texture, const void *bytes, size_t length, size_t stride,
				unsigned layers)
{
	texture->memory = (const uint8_t *)bytes;
	texture->memory_length = length;
	texture->memory_stride = stride;
	texture->memory_layers = layers ? layers : 1;
	texture->memory_written = false;
	texture->origin = 3;
}

namespace {

/* 256 words spread over the memory and its length: what changes when an image is drawn into it */
uint64_t
memory_signature(const uint8_t *m, size_t length)
{
	uint64_t h = 1469598103934665603ull ^ length;

	for (unsigned i = 0; length >= 8 && i < 256; i++) {
		uint64_t word;
		const size_t at = (length - 8) * i / 255 & ~(size_t)7;

		memcpy(&word, m + at, 8);
		h = (h ^ word) * 1099511628211ull;
	}
	return h;
}

} // namespace

extern "C" int
rwm_texture_copy_in(struct rwm_texture *texture, size_t *bytes, char *why, size_t why_size)
{
	if (texture->memory != nullptr) {
		const uint64_t signature = memory_signature(texture->memory, texture->memory_length);
		const unsigned origin[3] = { 0, 0, 0 };
		const unsigned size[3] = { texture->shape.width, texture->shape.height,
					   texture->shape.depth ? texture->shape.depth : 1 };

		*bytes = 0;
		if (texture->memory_written && signature == texture->memory_signature) {
			return 0;
		}
		for (unsigned layer = 0; layer < texture->memory_layers; layer++) {
			if (!rwm_texture_write(texture, 0, layer, origin, size,
					       texture->memory + layer * (texture->memory_length / texture->memory_layers),
					       texture->memory_stride)) {
				snprintf(why, why_size, "the caller's %zu bytes (%zu a row) would not go into the texture",
					 texture->memory_length, texture->memory_stride);
				return -1;
			}
		}
		texture->memory_signature = signature;
		texture->memory_written = true;
		texture->filled = true;
		texture->fill_kind |= 4;
		*bytes = texture->memory_length;
		{
			static unsigned said;

			if (said++ < 12) {
				unsigned nonzero = 0;
				char line[200];

				for (unsigned i = 0; texture->memory_length >= 4 && i < 64; i++) {
					uint32_t word;

					memcpy(&word, texture->memory + ((texture->memory_length - 4) * i / 63 & ~(size_t)3), 4);
					nonzero += word != 0;
				}
				snprintf(line, sizeof(line), "!!rwm image copied in: %ux%u format %u, %zu bytes, %u of 64 words not zero",
					 texture->shape.width, texture->shape.height, texture->shape.pixel_format,
					 texture->memory_length, nonzero);
				sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
			}
		}
		return 0;
	}

	const struct rwm_air *air = rwm_air();
	const unsigned width = texture->shape.width, height = texture->shape.height;
	const unsigned origin[3] = { 0, 0, 0 }, size[3] = { width, height, 1 };

	*bytes = 0;
	if (texture->source == nullptr || air->texture_write == nullptr) {
		return 0;
	}

	uint32_t seed = IOSurfaceGetSeed(texture->source);

	if (seed != 0 && seed == texture->source_seed) {
		return 0;               /* (a surface whose seed never moves is copied every time) */
	}

	surface_lock lock(texture->source, kIOSurfaceLockReadOnly);

	if (!lock.held()) {
		return 0;
	}

	const plane first = plane_of(texture->source, 0);
	const void *texels = first.base;
	size_t row = first.row;
	block packed;

	/*
	 * A YCbCr surface in two planes ('420f', '420v': the wallpaper, photographs), which Metal samples
	 * as one texture of format 500 -- (Y, Cb, Cr) out of the sampler, and QuartzCore's shader
	 * converting with its own matrix (fc_has_ycbcr_matrix). Here it is an RGBA8 texture holding
	 * exactly that: Y in red, the chroma of its 2 x 2 block in green and blue.
	 */
	if (texture->source_two_planes) {
		const plane second = plane_of(texture->source, 1);
		/* a byte of Y per texel; a pair (Cb, Cr) per two texels, a row of them per two rows */
		const size_t chroma_bytes = (size_t)((width - 1) / 2) * 2 + 2, chroma_rows = (height - 1) / 2 + 1;

		if (width == 0 || height == 0 || !covers(texture->source, first, width, height) ||
		    !covers(texture->source, second, chroma_bytes, chroma_rows)) {
			snprintf(why, why_size, "the two-plane surface (%zux%zu, %zu and %zu bytes a row) is smaller "
				 "than its %ux%u texture", first.width, first.height, first.row, second.row, width,
				 height);
			return -1;
		}

		const uint8_t *luma = first.base, *chroma = second.base;
		const size_t luma_row = first.row, chroma_row = second.row;

		packed.reset((uint8_t *)malloc((size_t)width * height * 4));
		if (packed == nullptr) {
			return 0;
		}
		for (unsigned y = 0; y < height; y++) {
			const uint8_t *l = luma + y * luma_row, *c = chroma + (y / 2) * chroma_row;
			uint8_t *out = packed.get() + (size_t)y * width * 4;

			for (unsigned x = 0; x < width; x++) {
				out[4 * x + 0] = l[x];
				out[4 * x + 1] = c[(x / 2) * 2];
				out[4 * x + 2] = c[(x / 2) * 2 + 1];
				out[4 * x + 3] = 255;
			}
		}
		texels = packed.get();
		row = (size_t)width * 4;
	} else {
		/* the library reads `width` texels of the texture's format a row, rows `row` apart: that many
		 * bytes of every one of `height` rows inside the plane and the allocation, measured by the
		 * texture's own format and not the surface's element, which need not agree (the automated
		 * review's finding) */
		const size_t texel = texel_bytes(texture->shape.pixel_format);

		if (texel == 0) {
			snprintf(why, why_size, "a surface is copied into formats 80, 81, 70, 30 and 10, not %u",
				 texture->shape.pixel_format);
			return -1;
		}
		if (width == 0 || height == 0 || width > first.width ||
		    !covers(texture->source, first, (size_t)width * texel, height)) {
			snprintf(why, why_size, "the surface (%zux%zu, %zu bytes a row) is smaller than its %ux%u "
				 "texture of format %u", first.width, first.height, first.row, width, height,
				 texture->shape.pixel_format);
			return -1;
		}
	}
	*bytes = row * height;
	if (air->texture_write(texture->texture, 0, 0, origin, size, texels, row, why, why_size) != 0) {
		return -1;
	}
	texture->source_seed = seed;
	return 0;
}

/*
 * The copy-BACK road, for a GPU whose kernel driver cannot hand an IOSurface over as GPU memory (the
 * Mali's: there is no HakiosGPU service, which is what kHakiosSurface lives on). The texture is then
 * the library's own, and what was drawn into it goes into the surface's memory at the end of each
 * command buffer that drew into it -- the one copy the drawable path exists to save, paid so that the
 * window server's frames reach the display at all.
 */
extern "C" void
rwm_texture_copy_back_to(struct rwm_texture *texture, IOSurfaceRef surface)
{
	if (texture->surface != nullptr) {
		CFRelease(texture->surface);
	}
	texture->surface = (IOSurfaceRef)CFRetain(surface);
}

extern "C" bool
rwm_texture_has_surface(const struct rwm_texture *texture)
{
	return texture->surface != nullptr;
}

/*
 * What touches a texture's memory at once (-replaceRegion:, -getBytes:, a depth read) waits first for the buffers
 * committed before it: in Metal's order they come first, and on the driver's worker they are drawn later.
 */
extern "C" bool
rwm_texture_write(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		  const unsigned size[3], const void *bytes, size_t stride)
{
	char why[256] = { 0 };

	rwm_worker_drain("replaceRegion");
	if (rwm_air()->texture_write(texture->texture, level, slice, origin, size, bytes, stride, why, sizeof(why)) != 0) {
		rwm_print("writing texels: %s\n", why);
		return false;
	}
	texture->filled = true;
	texture->fill_kind |= 2;
	return true;
}

extern "C" bool
rwm_texture_read(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		 const unsigned size[3], void *bytes, size_t stride)
{
	char why[256] = { 0 };

	rwm_worker_drain("getBytes");
	if (rwm_air()->texture_read(texture->texture, level, slice, origin, size, bytes, stride, why, sizeof(why)) != 0) {
		rwm_print("reading texels: %s\n", why);
		return false;
	}
	return true;
}

/* a depth/stencil texture by aspect, which Metal's -getBytes: has no way to name and the library's texture read
 * refuses by name */
extern "C" bool
rwm_texture_read_depth(struct rwm_texture *texture, const unsigned origin[3], const unsigned size[3], float *depth,
		       uint8_t *stencil)
{
	const struct rwm_air *air = rwm_air();
	char why[256] = { 0 };

	if (air->depth_read == nullptr) {
		rwm_print("this compiler library has no rwa_depth_read, so it is "
			  "older than this driver and cannot say what a depth buffer holds\n");
		return false;
	}
	rwm_worker_drain("depth read");
	if (air->depth_read(texture->texture, 0, 0, origin, size, depth, stencil, why, sizeof(why)) != 0) {
		rwm_print("reading the depth of a texture: %s\n", why);
		return false;
	}
	return true;
}

extern "C" uint32_t
rwm_texture_in_place(const struct rwm_texture *texture)
{
	return texture->in_place;
}

extern "C" void
rwm_texture_set_in_place(struct rwm_texture *texture, uint32_t surface)
{
	texture->in_place = surface;
	texture->origin = 1;
}

/*
 * What the frames sample, by where each texture came from, and the ones sampled with nothing ever put into them --
 * a layer drawn as nothing (the 27 release's home screen: its stripes and none of its images). Counted per draw at
 * recording, said every five seconds by rwm_perf_report ("rwm sampled"), the empty ones by their shape.
 */
namespace {

struct sampled_counts {
	unsigned long by_origin[5], empty_by_origin[5];
	struct { const rwm_texture *t; unsigned w, h, f; uint8_t origin; } empty[8];
	unsigned empty_count;
} sampled;
pthread_mutex_t sampled_lock = PTHREAD_MUTEX_INITIALIZER;

} // namespace

extern "C" void
rwm_texture_note_filled(struct rwm_texture *texture)
{
	if (texture != nullptr) {
		texture->filled = true;
		texture->fill_kind |= 1;
	}
}

extern "C" void
rwm_texture_note_view(struct rwm_texture *texture)
{
	texture->origin = 4;
	texture->filled = true;           /* (what is in it is its parent's) */
	texture->fill_kind |= 8;
}

/*
 * Boot-arg rwm_texshot=WxH: the first texture of that size in format 80 (BGRA8Unorm) that is sampled 30 times is read
 * back from the GPU and written into the host's kept lines as a small picture (48 pixels wide, red green blue alpha,
 * base64: "!!texshot k: ..."; tools/texshot.py makes a PNG of it), with how many of its pixels have any alpha. With
 * rwa_tint=1 the 27 release's icons and widgets came on the screen as magenta rounded rectangles and without it as
 * nothing (run 70): the draws that put them on the screen work and what they sample -- a texture the window server
 * drew them into first, 129 x 158 for an icon and its name (rwm_capture, run 66) -- holds nothing to see. This says
 * what it holds. The 30th sample and not the first, so that the pass that draws it has been run.
 */
namespace {

/*
 * ... and rwm_texshot=N (no "x"): N different textures, any size from 32 x 32 in a four-byte format (RGBA8 70/71,
 * BGRA8 80/81), that reach their 30th sample once 100 s have passed since the first draw of this process -- the home
 * screen's and not the boot logo's (the 129 x 158 of run 71 was the Apple logo, whole) -- 32 pixels wide each, as
 * "texshot#k" so that tools/texshot.py can tell them apart. Each one's header says what filled it (fill_kind).
 */
void
texshot(struct rwm_texture *texture, unsigned samples)
{
	static unsigned want_w, want_h, want_n, shots;
	static int read = 0;
	static uint64_t first_ns;
	char line[240];

	if (!read) {
		const char *at = strstr(rwm_boot_args(), "rwm_texshot=");

		read = 1;
		first_ns = rwm_ns();
		if (at == nullptr || sscanf(at + 12, "%ux%u", &want_w, &want_h) != 2) {
			want_w = want_h = 0;
			if (at == nullptr || sscanf(at + 12, "%u", &want_n) != 1) {
				want_n = 0;
			}
		}
	}
	if (samples != 30) {
		return;
	}
	const unsigned f = texture->shape.pixel_format;
	if (want_w != 0) {
		if (shots != 0 || texture->shape.width != want_w || texture->shape.height != want_h || f != 80) {
			return;
		}
	} else if (shots >= want_n || texture->shape.width < 32 || texture->shape.height < 32 ||
		   (f != 70 && f != 71 && f != 80 && f != 81) || rwm_ns() - first_ns < 100000000000ull) {
		return;
	}
	const unsigned shot = shots++;
	const bool bgra = f == 80 || f == 81;

	const unsigned w = texture->shape.width, h = texture->shape.height, cap = want_w != 0 ? 48 : 32;
	const unsigned ow = w < cap ? w : cap, oh = (h * ow + w - 1) / w;
	uint8_t *pixels = (uint8_t *)calloc((size_t)w * h, 4);
	unsigned origin[3] = { 0, 0, 0 }, size[3] = { w, h, 1 };
	unsigned long lit = 0, sum[4] = { 0, 0, 0, 0 };

	if (pixels == nullptr || !rwm_texture_read(texture, 0, 0, origin, size, pixels, (size_t)w * 4)) {
		free(pixels);
		return;
	}
	for (size_t i = 0; i < (size_t)w * h; i++) {
		lit += pixels[i * 4 + 3] != 0;
		for (unsigned c = 0; c < 4; c++) {
			sum[c] += pixels[i * 4 + c];
		}
	}
	snprintf(line, sizeof(line), "!!texshot#%u begin %u %u (of %ux%u format %u, origin %u, filled by %#x): %lu of %u "
		 "pixels with alpha, average %lu %lu %lu a %lu", shot, ow, oh, w, h, f, texture->origin, texture->fill_kind, lit,
		 w * h, sum[0] / (w * h), sum[1] / (w * h), sum[2] / (w * h), sum[3] / (w * h));
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));

	/* nearest pixel, as R G B A, 30 pixels (120 bytes) a line */
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint8_t *small = (uint8_t *)malloc((size_t)ow * oh * 4);

	for (unsigned y = 0; small != nullptr && y < oh; y++) {
		for (unsigned x = 0; x < ow; x++) {
			const uint8_t *p = pixels + (((size_t)(y * h / oh) * w + x * w / ow) * 4);
			uint8_t *q = small + ((size_t)y * ow + x) * 4;

			q[0] = bgra ? p[2] : p[0];
			q[1] = p[1];
			q[2] = bgra ? p[0] : p[2];
			q[3] = p[3];
		}
	}
	for (size_t at = 0, k = 0; small != nullptr && at < (size_t)ow * oh * 4; at += 120, k++) {
		const size_t end = at + 120 < (size_t)ow * oh * 4 ? at + 120 : (size_t)ow * oh * 4;
		int n = snprintf(line, sizeof(line), "!!texshot#%u %zu: ", shot, k);

		for (size_t i = at; i < end; i += 3) {
			const uint32_t v = (uint32_t)small[i] << 16 | (i + 1 < end ? (uint32_t)small[i + 1] << 8 : 0) |
					   (i + 2 < end ? small[i + 2] : 0);

			line[n++] = digits[v >> 18 & 63];
			line[n++] = digits[v >> 12 & 63];
			line[n++] = i + 1 < end ? digits[v >> 6 & 63] : '=';
			line[n++] = i + 2 < end ? digits[v & 63] : '=';
		}
		line[n] = 0;
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	}
	snprintf(line, sizeof(line), "!!texshot#%u end", shot);
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	free(small);
	free(pixels);
}

} // namespace

extern "C" void
rwm_texture_note_sampled(struct rwm_texture *texture)
{
	bool empty;

	if (texture == nullptr) {
		return;
	}
	texshot(texture, ++texture->samples);
	if (texture->origin == 1 && texture->surface_empty < 0) {
		IOSurfaceRef s = IOSurfaceLookup(texture->in_place);
		unsigned nonzero = 0;

		if (s != nullptr) {
			const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(s);
			const size_t size = IOSurfaceGetAllocSize(s);

			for (unsigned i = 0; base != nullptr && size >= 4 && i < 64; i++) {
				uint32_t word;

				memcpy(&word, base + ((size - 4) * i / 63 & ~(size_t)3), 4);
				nonzero += word != 0;
			}
			CFRelease(s);
		}
		texture->surface_empty = nonzero == 0;
	}
	empty = texture->origin == 1 ? texture->surface_empty == 1 : !texture->filled;
	pthread_mutex_lock(&sampled_lock);
	sampled.by_origin[texture->origin]++;
	if (empty) {
		unsigned k;

		sampled.empty_by_origin[texture->origin]++;
		for (k = 0; k < sampled.empty_count && sampled.empty[k].t != texture; k++) {
		}
		if (k == sampled.empty_count && k < 8) {
			sampled.empty[k] = { texture, texture->shape.width, texture->shape.height, texture->shape.pixel_format,
					     texture->origin };
			sampled.empty_count++;
		}
	}
	pthread_mutex_unlock(&sampled_lock);
}

extern "C" void
rwm_texture_sample_report(void (*say)(const char *))
{
	static const char *const names[5] = { "made", "surface", "copied", "memory", "view" };
	char line[240];
	int at;

	pthread_mutex_lock(&sampled_lock);
	sampled_counts now = sampled;
	memset(&sampled, 0, sizeof(sampled));
	pthread_mutex_unlock(&sampled_lock);
	at = snprintf(line, sizeof(line), "rwm sampled 5s:");
	for (unsigned o = 0; o < 5; o++) {
		at += snprintf(line + at, sizeof(line) - at, " %s %lu (%lu empty)", names[o], now.by_origin[o],
			       now.empty_by_origin[o]);
	}
	say(line);
	for (unsigned k = 0; k < now.empty_count; k++) {
		snprintf(line, sizeof(line), "rwm sampled empty %u: %ux%u format %u, %s", k + 1, now.empty[k].w, now.empty[k].h,
			 now.empty[k].f, names[now.empty[k].origin]);
		say(line);
	}
}

extern "C" int
rwm_texture_sync_surface(struct rwm_texture *texture, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	const unsigned origin[3] = { 0, 0, 0 };
	const unsigned size[3] = { texture->shape.width, texture->shape.height, 1 };
	const size_t row = (size_t)texture->shape.width * 4, rows = texture->shape.height;
	IOSurfaceRef surface = texture->surface;
	char inner[256] = { 0 };

	if (surface == nullptr || air->texture_read == nullptr) {
		return 0;
	}
	/* one buffer per call: the window server commits from more than one thread, and a buffer
	   shared between them was freed under a copy (signal 11 in memmove) */
	block scratch((uint8_t *)malloc(row * rows));

	if (scratch == nullptr || air->texture_read(texture->texture, 0, 0, origin, size, scratch.get(),
						     row /* bytes per row */, inner, sizeof(inner)) != 0) {
		snprintf(why, why_size, "the frame could not be read: %s", inner);
		return -1;
	}

	surface_lock lock(surface, 0);

	if (!lock.held()) {
		snprintf(why, why_size, "the surface could not be locked");
		return -1;
	}
	/* a surface without planes answers the plane calls with zero: the whole-surface ones */
	const bool planar = IOSurfaceGetPlaneCount(surface) > 0;
	uint8_t *base = (uint8_t *)(planar ? IOSurfaceGetBaseAddressOfPlane(surface, 0)
					   : IOSurfaceGetBaseAddress(surface));
	const size_t pitch = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, 0) : IOSurfaceGetBytesPerRow(surface);
	const size_t height = planar ? IOSurfaceGetHeightOfPlane(surface, 0) : IOSurfaceGetHeight(surface);
	const size_t alloc = IOSurfaceGetAllocSize(surface), copy = row < pitch ? row : pitch;

	for (size_t y = 0; base != nullptr && pitch != 0 && y < rows && y < height &&
	     y * pitch + copy <= alloc; y++) {
		memcpy(base + y * pitch, scratch.get() + y * row, copy);
	}
	return 0;
}
