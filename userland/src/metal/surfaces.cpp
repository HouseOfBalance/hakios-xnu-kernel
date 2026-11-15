/*
 * A command buffer's surfaces, by their numbers. The ones it READS in place (another process's layer, an icon, a
 * widget) are counted in use from -commit until the GPU has read them, as Apple's GPU drivers count them:
 * CoreAnimation's backing stores draw the next picture into a surface that is not in use, and into another one while
 * it is -- without it the icons came out cut in two and the widget stack an "H" (27 October). The ones it DRAWS into
 * in place are marked pending in the kernel at -commit (the display waits for them, MiniIOSurfaceDrawn) and given the
 * buffer's last GPU job when its work has been sent off.
 */
#include "rwm.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/*
 * A surface's memory as the processors left it, for the GPU to read and for a processor to read what the GPU drew:
 * every line of it cleaned and invalidated (dc civac reaches the point of coherency, every core's cache). A surface
 * is mapped CACHED in every process (MiniIOSurface.cpp), the Mali is not coherent with the processors' caches, and
 * a picture another process drew with the processor -- an icon, a label, a layer's backing store -- may still be in
 * a cache when the GPU samples the surface in place: it then reads what memory held before, zeros. Boot-arg
 * rwm_clean=0: not done. Counted in rwm_surface_clean_report's line.
 */
static unsigned long cleaned_surfaces, cleaned_bytes;
static uint64_t cleaned_ns;

extern "C" void
rwm_surface_to_memory(IOSurfaceRef surface)
{
	static int on = -1;

	if (on < 0) {
		on = strstr(rwm_boot_args(), "rwm_clean=0") == nullptr;
	}
	if (!on || surface == nullptr) {
		return;
	}
	const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surface);
	const size_t size = IOSurfaceGetAllocSize(surface);

	if (base == nullptr || size == 0) {
		return;
	}
	const uint64_t began = rwm_ns();

	for (uintptr_t line = (uintptr_t)base & ~(uintptr_t)63; line < (uintptr_t)base + size; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
	cleaned_surfaces++;
	cleaned_bytes += size;
	cleaned_ns += rwm_ns() - began;
	if (cleaned_surfaces == 1 || (cleaned_surfaces & 4095) == 0) {
		char line[160];

		snprintf(line, sizeof(line), "!!rwm surfaces cleaned for the GPU: %lu, %lu MB, %llu ms", cleaned_surfaces,
			 cleaned_bytes >> 20, (unsigned long long)(cleaned_ns / 1000000));
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	}
}

extern "C" void
rwm_sampled_add(struct rwm_sampled *s, uint32_t surface)
{
	static bool said;
	IOSurfaceRef found;

	for (unsigned k = 0; k < s->count; k++) {
		if (IOSurfaceGetID(s->at[k]) == surface) {
			return;
		}
	}
	if (s->count == sizeof(s->at) / sizeof(s->at[0])) {
		if (!said) {
			said = true;
			rwm_print("NOT DONE: a command buffer sampling more than 32 surfaces; the rest are not counted in use\n");
		}
		return;
	}
	if ((found = IOSurfaceLookup(surface)) == nullptr) {
		return;
	}
	IOSurfaceIncrementUseCount(found);
	rwm_surface_to_memory(found);
	s->at[s->count++] = found;
}

/* once the GPU has done with them, or the buffer goes without being run */
extern "C" void
rwm_sampled_release(struct rwm_sampled *s)
{
	const unsigned count = s->count;

	s->count = 0;
	for (unsigned k = 0; k < count; k++) {
		IOSurfaceDecrementUseCount(s->at[k]);
		CFRelease(s->at[k]);
	}
}

extern "C" void
rwm_drawn_add(struct rwm_drawn *d, uint32_t surface)
{
	static bool said;
	const struct rwm_air *air = rwm_air();
	unsigned k;

	for (k = 0; k < d->count && d->at[k] != surface; k++) {
	}
	if (k < d->count) {
		return;
	}
	if (d->count == sizeof(d->at) / sizeof(d->at[0])) {
		if (!said) {
			said = true;
			rwm_print("NOT DONE: a command buffer drawing into more than 8 surfaces; the display does not wait for "
				  "the 9th\n");
		}
		return;
	}
	d->at[d->count++] = surface;
	if (air->surface_seq != nullptr) {
		air->surface_seq(surface, ~0ull);
	}

	/* a frame begun in a surface the display still holds (queued, or being copied to the panel): what it shows would
	 * be part one frame, part the next (stage 4 of docs/metal-x6826b-plan.md) -- counted, the first few by name */
	IOSurfaceRef held = IOSurfaceLookup(surface);

	if (held != nullptr) {
		if (IOSurfaceIsInUse(held)) {
			static unsigned over;
			char line[160];

			if (++over <= 4 || (over & 255) == 0) {
				snprintf(line, sizeof(line), "!rwm %s: surface %u drawn into while the display holds it (%u so far)",
					 getprogname(), surface, over);
				sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
			}
		}
		CFRelease(held);
	}
}

extern "C" void
rwm_drawn_done(const struct rwm_drawn *d, uint64_t seq)
{
	const struct rwm_air *air = rwm_air();

	for (unsigned k = 0; air->surface_seq != nullptr && k < d->count; k++) {
		air->surface_seq(d->at[k], seq);
	}
}

/* at -commit: the surfaces a buffer's works read in place counted in use, and those they draw into pending */
extern "C" void
rwm_works_sampled(struct rwm_work *const *works, unsigned count, struct rwm_sampled *s)
{
	for (unsigned n = 0; n < count; n++) {
		for (unsigned k = 0; k < works[n]->sampled_count; k++) {
			rwm_sampled_add(s, works[n]->sampled[k]);
		}
	}
}

extern "C" void
rwm_works_drawn(struct rwm_work *const *works, unsigned count, struct rwm_drawn *d)
{
	for (unsigned n = 0; n < count; n++) {
		for (unsigned k = 0; k < works[n]->target_surface_count; k++) {
			if (works[n]->target_surface[k] != 0) {
				rwm_drawn_add(d, works[n]->target_surface[k]);
			}
		}
	}
}
