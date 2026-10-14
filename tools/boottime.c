/*
 * boottime: seconds from SoC reset to the kernel start (bootloader +
 * uniLoader), from the arch counter (26 MHz; CNTFRQ is not programmed) and
 * CLOCK_BOOTTIME. With an LZ4 uniLoader, /proc/device-tree/chosen/
 * uniloader,lz4-ticks holds the unpacking time in the same ticks.
 * Build: aarch64-linux-gnu-gcc -O2 -static -o boottime boottime.c
 */
#include <stdio.h>
#include <time.h>
#include <stdint.h>
int main(void)
{
	uint64_t c, f;
	struct timespec ts;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(c));
	clock_gettime(CLOCK_BOOTTIME, &ts);
	f = 26000000; /* CNTFRQ is not programmed; DT says 26 MHz */
	double up = ts.tv_sec + ts.tv_nsec / 1e9, cs = (double)c / f;
	printf("counter %.3f s, boottime %.3f s, before kernel %.3f s (freq %llu)\n",
	       cs, up, cs - up, (unsigned long long)f);
	return 0;
}
