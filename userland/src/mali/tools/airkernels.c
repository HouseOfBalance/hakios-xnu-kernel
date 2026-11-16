/*
 * airkernels LIB.metallib [FUNCTION]: every compute function of an Apple .metallib through compiler.c's
 * hm_compile_air_kernel on this machine -- translated, its device buffers made global, compiled for the phone's
 * 0x7402 -- and what came of each (built by tools/mk-shaders.sh AIR=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compiler.h"

int
main(int argc, char **argv)
{
	FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
	if (f == NULL) {
		fprintf(stderr, "usage: airkernels LIB.metallib [FUNCTION]\n");
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *lib = malloc(size);
	fread(lib, 1, size, f);
	fclose(f);
	hm_compile_init();
	static char names[65536];
	int n = hm_air_kernels(lib, size, names, sizeof(names));
	int ok = 0;
	printf("airkernels: %d kernels\n", n);
	for (const char *name = names; n > 0 && *name; name += strlen(name) + 1) {
		if (argc > 2 && strcmp(argv[2], name) != 0) continue;
		struct hm_kernel k;
		uint32_t dev = 0, con = 0;
		char why[300];
		if (hm_compile_air_kernel(lib, size, name, 0x7402, &k, &dev, &con, why, sizeof(why)) == 0) {
			printf("  %-40s %5u bytes, local %ux%ux%u, device buffers %#x, constant buffers %#x, pushes %u, tls %u\n", name,
			    k.size, k.local[0], k.local[1], k.local[2], dev, con, k.push_count, k.tls_size);
			ok++;
			hm_kernel_free(&k);
		} else {
			printf("  %-40s REFUSED: %s\n", name, why);
		}
	}
	printf("airkernels: %d compiled\n", ok);
	return 0;
}
