// SPDX-License-Identifier: GPL-2.0
/*
 * mifmon - watch the Shannon CP's IPC channels on postmarketOS.
 *
 * The modem driver tells the CP that the AP side is ready (CMD_INIT_END)
 * only while both /dev/umts_ipc0 (FMT: Samsung IPC) and /dev/umts_rfs0
 * (RFS: remote file system) are open, so this tool also acts as the
 * "RIL is up" placeholder during bring-up. Every frame is printed with a
 * timestamp and the decoded header; with -r/-d it also answers the CP.
 *
 *   mifmon                     log umts_ipc0 + umts_rfs0 (+ umts_router)
 *   mifmon -a 'AT+CPIN?'       also send an AT command on umts_router
 *   mifmon -f <hex bytes>      also send a raw FMT frame on umts_ipc0
 *                              (up to 8 -f, sent in order)
 *   mifmon -d [-c cid] [-A apn] [-s hook]
 *                              also bring up a data call (see below)
 *   mifmon -I ...              same, but attach over IPC and never touch
 *                              the AT port (does NOT attach on this CP; kept
 *                              for experiments)
 *   mifmon -M                  also serve /dev/wwan0at0 for ModemManager
 *                              (via /dev/wwan_at_ctl, kernel wwan_at_bridge):
 *                              AT proxy to umts_router, our own attach
 *                              commands go between ModemManager's. With -d
 *                              the data call follows ModemManager's
 *                              AT+CGACT (shannon plugin) instead of
 *                              starting by itself; -s is then not needed
 *   mifmon -r [-e efs-dir]     also answer the CP's RFS (file/NV) requests
 *                              (default /var/lib/android-modem/efs)
 *
 * While running, lines written to the FIFO /run/mifmon.ctl are sent to the
 * CP (for experiments next to the services):
 *   fmt <group> <index> <type> [data...]   IPC frame, hex, header filled in
 *   at <command>                           AT command on umts_router,
 *                                          between ModemManager's commands
 *
 * Only one process may read umts_ipc0: a second reader steals the CP's
 * replies. So the data call is set up from here rather than by a separate
 * tool.
 *
 * Data call (decoded from the stock libsec-ril, IpcProtocol41):
 *   GPRS_DEFINE_PDP_CONTEXT 0d 01 SET: u8 1, u8 cid, u8 pdp_type,
 *       char apn[100], ... (134 bytes in all)
 *   GPRS_PS (attach)        0d 03 SET: u8 1, u8 0, u8 0 (10 bytes; not
 *       enough on its own, the attach is done with AT+CGATT instead)
 *   GPRS_PDP_CONTEXT        0d 04 SET: u8 1 (activate) / 0, u8 cid, ...
 *       (112 bytes)
 * The CP answers each with GEN_PHONE_RES (80 01, code 0x8000 = OK), then
 * sends GPRS_IP_CONFIGURATION (0d 09: cid, flags, ?, ip[4], dns1[4],
 * dns2[4], ..., u16 mtu at offset 0x9f) and GPRS_CALL_STATUS (0d 10: cid,
 * state; 1 = active). Packets of context <cid> then flow on rmnet<cid-1>.
 * The hook (-s) runs with IFACE, CID, IP, DNS1, DNS2 and MTU set.
 *
 * Frame formats (Samsung IPC 4.x, as used by libsec-ril):
 *   FMT: u16 len; u8 mseq; u8 aseq; u8 group; u8 index; u8 type; data
 *   RFS: u32 len; u8 cmd; u8 id; data
 *
 * Build on the phone: gcc -O2 -Wall -o mifmon mifmon.c
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE	(64 * 1024)

/* AP->CP and CP->AP use different type codes */
static const char *const fmt_tx_types[] = {
	[1] = "EXEC", [2] = "GET", [3] = "SET", [4] = "CFRM", [5] = "EVENT",
};
static const char *const fmt_rx_types[] = {
	[1] = "INDI", [2] = "RESP", [3] = "NOTI",
};

/* Samsung IPC main command groups (libsamsung-ipc / RIL naming) */
static const char *fmt_group(uint8_t g)
{
	switch (g) {
	case 0x01: return "PWR";
	case 0x02: return "CALL";
	case 0x04: return "SMS";
	case 0x05: return "SEC";
	case 0x06: return "PB";
	case 0x07: return "DISP";
	case 0x08: return "NET";
	case 0x09: return "SND";
	case 0x0a: return "MISC";
	case 0x0b: return "SVC";
	case 0x0c: return "SS";
	case 0x0d: return "GPRS";
	case 0x0e: return "SAT";
	case 0x0f: return "CFG";
	case 0x10: return "IMEI";
	case 0x11: return "GPS";
	case 0x12: return "SAP";
	case 0x80: return "GEN";
	default: return "?";
	}
}

static void stamp(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("[%5ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
}

static void hexdump(const uint8_t *p, size_t n)
{
	size_t i, j;

	for (i = 0; i < n; i += 16) {
		printf("    %04zx:", i);
		for (j = i; j < i + 16; j++) {
			if (j < n)
				printf(" %02x", p[j]);
			else
				printf("   ");
		}
		printf("  ");
		for (j = i; j < i + 16 && j < n; j++)
			putchar(isprint(p[j]) ? p[j] : '.');
		putchar('\n');
	}
}

static void show_fmt(const char *dir, const uint8_t *p, size_t n)
{
	stamp();
	if (n >= 7) {
		uint16_t len = p[0] | p[1] << 8;
		uint8_t type = p[6];
		const char *const *types = dir[0] == 't' ? fmt_tx_types : fmt_rx_types;
		const char *tname = type <= 5 && types[type] ? types[type] : "?";

		printf("FMT %s len=%u mseq=%u aseq=%u %s(0x%02x) idx=0x%02x %s\n",
		       dir, len, p[2], p[3], fmt_group(p[4]), p[4], p[5], tname);
	} else {
		printf("FMT %s short frame (%zu bytes)\n", dir, n);
	}
	hexdump(p, n);
}

static void show_rfs(const char *dir, const uint8_t *p, size_t n)
{
	stamp();
	if (n >= 6) {
		uint32_t len = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;

		printf("RFS %s len=%u cmd=0x%02x id=%u\n", dir, len, p[4], p[5]);
	} else {
		printf("RFS %s short frame (%zu bytes)\n", dir, n);
	}
	hexdump(p, n > 256 ? 256 : n);
}

static void show_raw(const char *name, const uint8_t *p, size_t n)
{
	size_t i;

	stamp();
	printf("%s rx %zu bytes: \"", name, n);
	for (i = 0; i < n; i++) {
		if (p[i] == '\r')
			printf("\\r");
		else if (p[i] == '\n')
			printf("\\n");
		else
			putchar(isprint(p[i]) ? p[i] : '.');
	}
	printf("\"\n");
}

static int open_dev(const char *path)
{
	int fd = open(path, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		fprintf(stderr, "mifmon: %s: %s\n", path, strerror(errno));
	return fd;
}

static size_t parse_hex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	unsigned int b;
	int used;

	while (n < max && sscanf(s, " %2x%n", &b, &used) == 1) {
		out[n++] = b;
		s += used;
	}
	return n;
}

/* --- RFS server --------------------------------------------------------- */

/*
 * The CP keeps its NV data and a few files on the AP (EFS) and asks for
 * them over umts_rfs0. Decoded from the stock libsec-ril (Nv::Process*).
 * All integers little endian; every frame starts u32 len (whole frame),
 * u8 cmd, u8 id; the reply echoes cmd and id. A frame can span several
 * reads (reassembled by len, at most 512 KiB).
 *
 *   01 NV_READ   req: u32 off, u32 len
 *                rep: u8 ok, u32 off, u32 len, data            (15 + len)
 *   02 NV_WRITE  req: u32 off, u32 len, data
 *                rep: u8 ok, u32 off, u32 len                  (15)
 *        nv_data.bin, then nv_data.bin.md5 = hex MD5(file + secret)
 *   03 READ      req: s32 fd, u32 len
 *                rep: s32 n, s32 errno, data                   (14 + n)
 *   04 WRITE     req: s32 fd, u32 len, data   rep: s32 n, s32 errno
 *   05 LSEEK     req: s32 fd, s32 off, s32 whence
 *                rep: u32 0, s32 ret, s32 errno                (18)
 *   06 CLOSE     req: s32 fd                  rep: s32 ret, s32 errno
 *   0a GET_FILE_INFO      req: u32 plen, path
 *   13 GET_HANDLE_INFO    req: s32 fd
 *                rep: s32 ret, u16 type (1 dir, 2 file), u32 size,
 *                     ctime[6], mtime[6] (yy-2000 mon day h m s),
 *                     s32 errno                                (32)
 *   0b DELETE / 0c MKDIR / 14 CREATE   req: u32 plen, path
 *                rep: s32 ret (fd for CREATE), s32 errno       (14)
 *   11 OPEN      req: s32 flags (Linux O_*), u32 plen, path
 *                rep: s32 fd, s32 errno                        (14)
 * Others (07 PUT_FILE, 08 GET_FILE, 09 RENAME, 15-17 NV write-all/buffer/
 * restore) are answered with ret -1, ENOSYS and logged.
 *
 * Paths: no ".." components; "err..." and "/data/log..." go to the log
 * directory, Audio_OEM/Audio_IFX/CP_AUDIO_SLSI/RF_NVM... to system/etc of
 * the Android root (read-only CP config), everything else to
 * <efs>/root/<path> (stock: /efs/root).
 */

#define RFS_MAX		(512 * 1024)
#define NV_SECRET	"Samsung_Android_RIL"

static struct {
	int fd;
	const char *efs;	/* EFS mount point */
	uint8_t *buf;		/* reassembly buffer */
	size_t have, want;
} rfs;

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void rfs_reply(uint8_t *r, size_t n, uint8_t cmd, uint8_t id)
{
	put32(r, n);
	r[4] = cmd;
	r[5] = id;
	if (write(rfs.fd, r, n) != (ssize_t)n)
		perror("mifmon: write umts_rfs0");
}

/* 14-byte reply: s32 ret, s32 errno */
static void rfs_reply_ret(uint8_t cmd, uint8_t id, int ret, int err)
{
	uint8_t r[14] = { 0 };

	put32(r + 6, ret);
	put32(r + 10, ret < 0 ? err : 0);
	rfs_reply(r, sizeof(r), cmd, id);
	stamp();
	printf("RFS 0x%02x id=%u -> ret=%d errno=%d\n", cmd, id, ret,
	       ret < 0 ? err : 0);
}

/* CP path -> AP path; NULL if rejected */
static char *rfs_path(const uint8_t *p, size_t plen, size_t room)
{
	char cp[1024], tmp[1024], *tok, *save;
	char *out;

	if (plen > room)
		plen = room;
	if (plen >= sizeof(cp))
		plen = sizeof(cp) - 1;
	memcpy(cp, p, plen);
	cp[plen] = 0;

	strcpy(tmp, cp);
	for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save))
		if (!strcmp(tok, ".."))
			return NULL;

	out = malloc(2048);
	if (!out)
		return NULL;
	if (!strncmp(cp, "err", 3) || !strncmp(cp, "/data/log", 9))
		snprintf(out, 2048, "%s/../data/log/%s", rfs.efs, cp);
	else if (!strncmp(cp, "Audio_OEM", 9) || !strncmp(cp, "Audio_IFX", 9) ||
		 !strncmp(cp, "CP_AUDIO_SLSI", 13) || !strncmp(cp, "RF_NVM", 6) ||
		 !strncmp(cp, "CP_OMC_AUDIO_SLSI", 17))
		snprintf(out, 2048, "%s/../system/etc/%s", rfs.efs, cp);
	else
		snprintf(out, 2048, "%s/root/%s", rfs.efs, cp);
	return out;
}

/* mkdir -p for the parent directory of path */
static void rfs_mkparents(const char *path)
{
	char d[2048], *s;

	snprintf(d, sizeof(d), "%s", path);
	for (s = d + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = 0;
		mkdir(d, 0775);
		*s = '/';
	}
}

/* Minimal MD5 (RFC 1321) for the NV checksum */
struct md5 {
	uint32_t h[4];
	uint64_t len;
	uint8_t blk[64];
};

static void md5_block(struct md5 *m, const uint8_t *p)
{
	static const uint32_t k[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
		0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
		0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
		0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
		0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
		0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
		0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
		0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
		0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
	};
	static const uint8_t r[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23,
				       6, 10, 15, 21 };
	uint32_t a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3], w[16], f, t;
	int i, g;

	for (i = 0; i < 16; i++)
		w[i] = get32(p + 4 * i);
	for (i = 0; i < 64; i++) {
		if (i < 16) { f = (b & c) | (~b & d); g = i; }
		else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
		else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
		else { f = c ^ (b | ~d); g = (7 * i) & 15; }
		t = d;
		d = c;
		c = b;
		f += a + k[i] + w[g];
		b += f << r[(i >> 4) * 4 + (i & 3)] | f >> (32 - r[(i >> 4) * 4 + (i & 3)]);
		a = t;
	}
	m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}

static void md5_init(struct md5 *m)
{
	m->h[0] = 0x67452301; m->h[1] = 0xefcdab89;
	m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
	m->len = 0;
}

static void md5_update(struct md5 *m, const uint8_t *p, size_t n)
{
	while (n--) {
		m->blk[m->len++ & 63] = *p++;
		if (!(m->len & 63))
			md5_block(m, m->blk);
	}
}

static void md5_hex(struct md5 *m, char out[33])
{
	uint64_t bits = m->len * 8;
	uint8_t pad = 0x80, z = 0, l[8];
	int i;

	md5_update(m, &pad, 1);
	while ((m->len & 63) != 56)
		md5_update(m, &z, 1);
	for (i = 0; i < 8; i++)
		l[i] = bits >> (8 * i);
	md5_update(m, l, 8);
	for (i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02x", m->h[i / 4] >> (8 * (i % 4)) & 0xff);
}

/* nv_data.bin.md5 = hex MD5(nv_data.bin + secret), as the stock RIL does */
static void nv_update_md5(void)
{
	char path[1024], hex[33];
	uint8_t blk[4096];
	struct md5 m;
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/nv_data.bin", rfs.efs);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto fail;
	md5_init(&m);
	while ((n = read(fd, blk, sizeof(blk))) > 0)
		md5_update(&m, blk, n);
	close(fd);
	if (n < 0)
		goto fail;
	md5_update(&m, (const uint8_t *)NV_SECRET, strlen(NV_SECRET));
	md5_hex(&m, hex);

	/* rewrite in place: keeps the file's owner and mode */
	strcat(path, ".md5");
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0700);
	if (fd < 0 || write(fd, hex, 32) != 32) {
		if (fd >= 0)
			close(fd);
		goto fail;
	}
	close(fd);
	return;
fail:
	stamp();
	printf("RFS nv_data.bin.md5 update FAILED: %s\n", strerror(errno));
}

static int nv_open(void)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/nv_data.bin", rfs.efs);
	return open(path, O_RDWR | O_SYNC);
}

static void rfs_nv(const uint8_t *q, size_t n)
{
	uint8_t cmd = q[4], id = q[5];
	uint32_t off = n >= 14 ? get32(q + 6) : 0;
	uint32_t len = n >= 14 ? get32(q + 10) : 0;
	struct stat st;
	ssize_t done = -1;
	uint8_t *r;
	int fd;

	if (len > RFS_MAX)
		len = RFS_MAX;
	r = calloc(1, 15 + len);
	fd = nv_open();
	if (!r || fd < 0 || fstat(fd, &st) < 0 || (uint64_t)off + len > (uint64_t)st.st_size) {
		/* never grow nv_data.bin */
		done = -1;
	} else if (cmd == 0x01) {
		done = pread(fd, r + 15, len, off);
	} else if (n >= 14 + (size_t)len) {
		done = pwrite(fd, q + 14, len, off);
		fsync(fd);
	}
	if (fd >= 0)
		close(fd);
	if (cmd == 0x02 && done == (ssize_t)len)
		nv_update_md5();

	stamp();
	printf("RFS NV_%s off=0x%x len=%u -> %zd\n", cmd == 0x01 ? "READ" : "WRITE",
	       off, len, done);
	if (!r)
		return;
	r[6] = done == (ssize_t)len;
	put32(r + 7, off);
	put32(r + 11, done > 0 ? done : 0);
	rfs_reply(r, 15 + (cmd == 0x01 && done > 0 ? done : 0), cmd, id);
	free(r);
}

static void rfs_info(uint8_t cmd, uint8_t id, int ret, int err, const struct stat *st)
{
	uint8_t r[32] = { 0 };
	const struct timespec *ts[2] = { &st->st_ctim, &st->st_mtim };
	int i;

	put32(r + 6, ret);
	if (ret == 0) {
		r[10] = S_ISDIR(st->st_mode) ? 1 : 2;
		put32(r + 12, st->st_size);
		for (i = 0; i < 2; i++) {
			struct tm tm;
			uint8_t *t = r + 16 + 6 * i;

			if (!localtime_r(&ts[i]->tv_sec, &tm))
				continue;
			t[0] = tm.tm_year - 100;
			t[1] = tm.tm_mon + 1;
			t[2] = tm.tm_mday;
			t[3] = tm.tm_hour;
			t[4] = tm.tm_min;
			t[5] = tm.tm_sec;
		}
	} else {
		put32(r + 28, err);
	}
	rfs_reply(r, sizeof(r), cmd, id);
	stamp();
	printf("RFS 0x%02x id=%u -> info ret=%d errno=%d\n", cmd, id, ret,
	       ret < 0 ? err : 0);
}

static void rfs_handle(const uint8_t *q, size_t n)
{
	uint8_t cmd = q[4], id = q[5];
	int32_t a = n >= 10 ? (int32_t)get32(q + 6) : -1;
	uint32_t b = n >= 14 ? get32(q + 10) : 0;
	char *path = NULL;
	struct stat st;
	int ret, err;

	switch (cmd) {
	case 0x01:
	case 0x02:
		rfs_nv(q, n);
		return;
	case 0x03: {		/* READ fd, len */
		uint8_t *r;

		if (b > RFS_MAX / 2)
			b = RFS_MAX / 2;
		r = calloc(1, 14 + b);
		if (!r)
			return;
		errno = EBADF;
		ret = a >= 0 ? read(a, r + 14, b) : -1;
		err = errno;
		put32(r + 6, ret > 0 ? ret : 0);
		put32(r + 10, ret < 0 ? err : 0);
		rfs_reply(r, 14 + (ret > 0 ? ret : 0), cmd, id);
		free(r);
		stamp();
		printf("RFS READ fd=%d len=%u -> %d\n", a, b, ret);
		return;
	}
	case 0x04:		/* WRITE fd, len, data */
		if (n < 14 + (size_t)b)
			b = n - 14;
		errno = EBADF;
		ret = a >= 0 ? write(a, q + 14, b) : -1;
		err = errno;
		if (ret >= 0)
			fsync(a);
		rfs_reply_ret(cmd, id, ret, err);
		return;
	case 0x05: {		/* LSEEK fd, off, whence */
		uint8_t r[18] = { 0 };
		int whence = n >= 18 ? (int32_t)get32(q + 14) : SEEK_SET;

		errno = EBADF;
		ret = a >= 0 ? lseek(a, (int32_t)b, whence) : -1;
		err = errno;
		put32(r + 10, ret);
		put32(r + 14, ret < 0 ? err : 0);
		rfs_reply(r, sizeof(r), cmd, id);
		stamp();
		printf("RFS LSEEK fd=%d off=%d whence=%d -> %d\n", a, (int32_t)b, whence, ret);
		return;
	}
	case 0x06:		/* CLOSE fd */
		errno = EBADF;
		ret = a >= 0 ? close(a) : -1;
		rfs_reply_ret(cmd, id, ret, errno);
		return;
	case 0x13:		/* GET_HANDLE_INFO fd */
		errno = EBADF;
		ret = a >= 0 ? fstat(a, &st) : -1;
		rfs_info(cmd, id, ret, errno, &st);
		return;
	case 0x11:		/* OPEN flags, plen, path */
		path = rfs_path(q + 14, b, n - 14);
		break;
	case 0x0a: case 0x0b: case 0x0c: case 0x14:	/* plen, path */
		path = rfs_path(q + 10, a, n - 10);
		break;
	default:
		stamp();
		printf("RFS cmd 0x%02x not implemented\n", cmd);
		rfs_reply_ret(cmd, id, -1, ENOSYS);
		return;
	}

	stamp();
	printf("RFS 0x%02x path \"%s\"\n", cmd, path ? path : "(rejected)");
	errno = EINVAL;
	ret = -1;
	if (path) {
		switch (cmd) {
		case 0x11:
			if (a & O_CREAT)
				rfs_mkparents(path);
			ret = open(path, (a | O_SYNC) & ~O_NOCTTY, 0775);
			break;
		case 0x0a:
			ret = stat(path, &st);
			break;
		case 0x0b:
			ret = unlink(path);
			break;
		case 0x0c:
			rfs_mkparents(path);
			ret = mkdir(path, 0775);
			if (ret < 0 && errno == EEXIST)
				ret = 0;
			break;
		case 0x14:
			rfs_mkparents(path);
			ret = creat(path, 0775);
			break;
		}
	}
	err = errno;
	free(path);
	if (cmd == 0x0a)
		rfs_info(cmd, id, ret < 0 ? -1 : 0, err, &st);
	else
		rfs_reply_ret(cmd, id, ret, err);
}

/* feed one read() worth of umts_rfs0 data */
static void rfs_rx(const uint8_t *p, size_t n)
{
	if (!rfs.have) {
		if (n < 6) {
			stamp();
			printf("RFS short frame (%zu bytes) dropped\n", n);
			return;
		}
		rfs.want = get32(p);
		if (rfs.want < 6 || rfs.want > RFS_MAX) {
			stamp();
			printf("RFS bad length %zu, dropped\n", rfs.want);
			return;
		}
	}
	if (rfs.have + n > rfs.want)
		n = rfs.want - rfs.have;
	memcpy(rfs.buf + rfs.have, p, n);
	rfs.have += n;
	if (rfs.have < rfs.want)
		return;
	show_rfs("rx", rfs.buf, rfs.have);
	rfs_handle(rfs.buf, rfs.have);
	rfs.have = 0;
}

/* --- data call ---------------------------------------------------------- */

enum dc_step { DC_OFF, DC_AT_CGDCONT, DC_AT_CGATT, DC_DEFINE, DC_PS_ATTACH, DC_ACTIVATE,
	       DC_UP, DC_ATTACHED };

static struct {
	enum dc_step step;
	int fd;			/* umts_ipc0 */
	int at_fd;		/* umts_router */
	uint8_t cid;
	const char *apn;
	const char *hook;
	uint8_t mseq;
	char at_rx[512];	/* AT reply collected so far */
	time_t deadline;	/* retry the attach after this */
	int ipc_attach;		/* -I: attach over IPC, leave the AT port alone */
	int on_demand;		/* -M with -d: data call only when MM asks */
	int want;		/* MM asked for the data call (AT+CGACT=1) */
	int mm_act_waiting;	/* MM's AT+CGACT=1 not answered yet */
	unsigned int mm_cid;	/* context number MM uses (mapped onto cid) */
	char ip[16], dns1[16], dns2[16];
} dc;

static void fmt_send(uint8_t *f, size_t n)
{
	f[0] = n & 0xff;
	f[1] = n >> 8;
	f[2] = ++dc.mseq;
	f[3] = 0;
	show_fmt("tx", f, n);
	if (write(dc.fd, f, n) < 0)
		perror("mifmon: write umts_ipc0");
}

/*
 * AT proxy (-M): ModemManager talks to /dev/wwan0at0, whose other end is
 * /dev/wwan_at_ctl (kernel wwan_at_bridge). mifmon owns umts_router and
 * passes ModemManager's commands through; its own attach commands go in
 * between, never in the middle of one of ModemManager's commands.
 */
static struct {
	int fd;			/* /dev/wwan_at_ctl, -1 if not proxying */
	int router;		/* umts_router */
	int mm_pending;		/* MM command sent, final result not seen */
	time_t mm_since;
	char rx[2048];		/* CP output since MM's last command */
	uint8_t q[4096];	/* MM input held while our command runs */
	size_t qlen;
	char own[160];		/* our command waiting for MM to finish */
	int own_wait;
	int own_active;		/* our command sent, final result not seen */
	time_t prompt;		/* "> " (SMS text entry) given to MM, when */
	int ctl;		/* own command is from /run/mifmon.ctl */
	char ctlrx[1024];	/* its output so far */
} px = { .fd = -1, .router = -1 };

static int at_final(const char *s)
{
	return strstr(s, "\r\nOK\r\n") || strstr(s, "ERROR") ||
	       strstr(s, "\r\nNO CARRIER") || strstr(s, "\r\nCONNECT") ||
	       strstr(s, "\r\nBUSY") || strstr(s, "\r\nNO ANSWER");
}

static void at_send_now(const char *cmd);
static time_t now_s(void);
static int own_at(const char *cmd);

/* MM may talk again: release what it queued, or run our waiting command */
static void px_idle(void)
{
	if (px.own_active || px.mm_pending)
		return;
	if (px.own_wait) {
		px.own_wait = 0;
		at_send_now(px.own);
		return;
	}
	if (px.qlen) {
		if (write(px.router, px.q, px.qlen) < 0)
			perror("mifmon: write umts_router");
		px.qlen = 0;
		px.mm_pending = 1;
		px.mm_since = 0;
		px.rx[0] = 0;
	}
}

static void px_reply(const char *r)
{
	if (px.fd >= 0 && write(px.fd, r, strlen(r)) < 0)
		perror("mifmon: write wwan_at_ctl");
}

/*
 * Incoming SMS. The CP does not report them on the AT channel (+CMT/+CMTI)
 * but as SMS_INCOMING_MSG over IPC (04 02, decoded from the stock
 * libsec-ril, IpcRxIncomingMsg): u8 type (2 = GSM), u8 tid, u16 sim index
 * (ffff), u8 ?, u8 pdu length, pdu (SMSC address + TPDU). The network
 * resends a message until it is acknowledged with SMS_DELIVER_REPORT
 * (04 06 EXEC, IpcTxDeliveryReport): u8 type, u16 result (0 = ok), u8 tid,
 * u8 0. Messages are acknowledged at once and kept here; with -M they are
 * announced to ModemManager as +CMTI: "SM",<n> with n from SMS_IDX_BASE up,
 * and its AT+CMGR / AT+CMGD for those indices are answered here (the SIM
 * storage is never used).
 */
#define SMS_SLOTS	16
#define SMS_IDX_BASE	200

static struct sms {
	int used;
	int read;		/* ModemManager fetched it */
	time_t told;		/* last +CMTI, 0 = not yet */
	unsigned int tpdu_len;
	char hex[2 * 256 + 1];	/* SMSC + TPDU */
} inbox[SMS_SLOTS];

static void sms_incoming(const uint8_t *p, size_t n)
{
	uint8_t ack[12] = { 0 };
	size_t len, i;
	struct sms *m = NULL;

	if (n < 13 || p[7] != 2)
		return;
	len = p[12];
	ack[4] = 0x04;		/* SMS */
	ack[5] = 0x06;		/* DELIVER_REPORT */
	ack[6] = 0x01;		/* EXEC */
	ack[7] = 2;		/* GSM */
	ack[10] = p[8];		/* tid */
	fmt_send(ack, sizeof(ack));
	if (n < 13 + len || len < 2 || p[13] + 1u >= len)
		return;
	for (i = 0; i < SMS_SLOTS; i++)
		if (!inbox[i].used) {
			m = &inbox[i];
			break;
		}
	stamp();
	if (!m) {
		printf("SMS incoming: inbox full, dropped\n");
		return;
	}
	memset(m, 0, sizeof(*m));
	m->used = 1;
	m->tpdu_len = len - p[13] - 1;
	for (i = 0; i < len; i++)
		sprintf(m->hex + 2 * i, "%02X", p[13 + i]);
	printf("SMS incoming: %zu bytes, acknowledged, slot %d\n", len,
	       (int)(m - inbox) + SMS_IDX_BASE);
}

static void px_reply(const char *r);

/* tell ModemManager about messages it has not fetched (again after 60 s) */
static void sms_announce(void)
{
	char urc[64];
	time_t now = now_s();
	int i;

	if (px.fd < 0 || px.mm_pending || px.own_active || px.own_wait)
		return;
	for (i = 0; i < SMS_SLOTS; i++) {
		if (!inbox[i].used || inbox[i].read ||
		    (inbox[i].told && now - inbox[i].told < 60))
			continue;
		inbox[i].told = now;
		snprintf(urc, sizeof(urc), "\r\n+CMTI: \"SM\",%d\r\n",
			 i + SMS_IDX_BASE);
		px_reply(urc);
		stamp();
		printf("SMS +CMTI slot %d to ModemManager\n", i + SMS_IDX_BASE);
	}
}

/* AT+CMGR=<n> / AT+CMGD=<n>[,0] for our slots; 0 = not ours */
static int sms_at(const char *cmd)
{
	char out[2 * 256 + 64];
	unsigned int idx, flag = 0;
	struct sms *m;

	if (sscanf(cmd, "AT+CMGR=%u", &idx) == 1) {
		if (idx < SMS_IDX_BASE || idx >= SMS_IDX_BASE + SMS_SLOTS)
			return 0;
		m = &inbox[idx - SMS_IDX_BASE];
		if (!m->used) {
			px_reply("\r\n+CMS ERROR: 321\r\n");	/* invalid index */
			return 1;
		}
		snprintf(out, sizeof(out), "\r\n+CMGR: %d,,%u\r\n%s\r\n\r\nOK\r\n",
			 m->read ? 1 : 0, m->tpdu_len, m->hex);
		m->read = 1;
		px_reply(out);
		return 1;
	}
	if (sscanf(cmd, "AT+CMGD=%u,%u", &idx, &flag) >= 1) {
		if (idx < SMS_IDX_BASE || idx >= SMS_IDX_BASE + SMS_SLOTS)
			return 0;
		inbox[idx - SMS_IDX_BASE].used = 0;
		px_reply("\r\nOK\r\n");
		return 1;
	}
	return 0;
}

/*
 * Allowed radio access technologies, for ModemManager's shannon plugin:
 * AT^SHNRAT? / AT^SHNRAT=<mask> map onto IPC NET_MODE_SEL (08 0a, a bitmask:
 * 01 GSM, 02 WCDMA, 08 LTE; kept in the CP's NV). Answered when the CP
 * replies (RESP 08 0a, or GEN_PHONE_RES for the SET).
 */
static struct {
	int get;		/* AT^SHNRAT? waiting for the CP */
	int set;		/* AT^SHNRAT= waiting for the CP */
	time_t since;
	unsigned int mask;	/* last mask set */
	time_t attach;		/* when to send AT+CGATT=1, 0 = no */
} rat;

static int rat_at(const char *cmd)
{
	uint8_t f[8] = { 0 };
	unsigned int mask;

	f[4] = 0x08;
	f[5] = 0x0a;
	if (!strcmp(cmd, "AT^SHNRAT?")) {
		f[6] = 0x02;	/* GET */
		fmt_send(f, 7);
		rat.get = 1;
	} else if (sscanf(cmd, "AT^SHNRAT=%u", &mask) == 1 && mask && mask < 0x100) {
		f[6] = 0x03;	/* SET */
		f[7] = mask;
		fmt_send(f, 8);
		rat.set = 1;
		rat.mask = mask;
	} else {
		return 0;
	}
	rat.since = now_s();
	return 1;
}

static void rat_rx(const uint8_t *p, size_t n)
{
	char out[48];

	if (rat.get && n >= 8 && p[4] == 0x08 && p[5] == 0x0a && p[6] == 0x02) {
		rat.get = 0;
		snprintf(out, sizeof(out), "\r\n^SHNRAT: %u\r\n\r\nOK\r\n", p[7]);
		px_reply(out);
	} else if (rat.set && n >= 12 && p[4] == 0x80 && p[5] == 0x01 &&
		   p[7] == 0x08 && p[8] == 0x0a) {
		rat.set = 0;
		if ((p[10] | p[11] << 8) != 0x8000) {
			px_reply("\r\nERROR\r\n");
			return;
		}
		px_reply("\r\nOK\r\n");
		/*
		 * Back on LTE the CP stays in limited service until it is told
		 * to attach, as after a CP boot.
		 */
		if (rat.mask & 0x08)
			rat.attach = now_s() + 5;
	}
}

static void rat_tick(void)
{
	if (rat.attach && now_s() >= rat.attach && own_at("AT+CGATT=1"))
		rat.attach = 0;
	if ((rat.get || rat.set) && now_s() - rat.since > 5) {
		rat.get = rat.set = 0;
		px_reply("\r\nERROR\r\n");
	}
}

/*
 * Voice calls, for ModemManager's generic (27.007) voice support. The CP
 * refuses call control on the AT channel (ATH: +CME ERROR: 3); the stock
 * RIL uses IPC CALL (group 02, decoded from libsec-ril IpcProtocol41):
 *   ORIGINATE 02 01 EXEC, 99 bytes: u16 call type (00 01 = CS voice),
 *       u8 clir (0 default), u8 number length, u8 number type (0x11 with a
 *       leading '+', else 0x21), ascii number (with the '+'), zero filled
 *   RELEASE 02 03 EXEC, ANSWER 02 04 EXEC: no data
 *   MANAGE 02 0c SET, 92 bytes: u8 type, u8 call id (ff none) - types:
 *       1 release held/waiting (CHLD 0), 2 release active + accept other
 *       (CHLD 1), 3 release call id (CHLD 1x), 4 swap (CHLD 2), 5 split id
 *       (CHLD 2x), 6 conference (CHLD 3), 7 transfer (CHLD 4)
 *   CONT_DTMF 02 08 SET: u8 1 start / 2 stop, u8 ascii digit
 *   CALL_LIST 02 06 GET -> RESP: u8 count, per call: u16 type, u8 id,
 *       u8 direction (1 = MO), u8 state (minus 1: 0 active, 1 held,
 *       2 dialing, 3 alerting, 4 incoming, 5 waiting - as in +CLCC),
 *       u8 multiparty, u8 number length, u8 number type (bits 4-6 = 1:
 *       international), number
 *   INCOMING 02 02, WAITING 02 09, CALL_STATUS 02 05 (u16 type, id,
 *       state: 1 outgoing, 3 connected, 4 released, 5 alerting, 6 held),
 *       CALL_LIST 02 06 NOTI: re-read the list
 * The list is re-read on every call event; AT+CLCC is answered from it
 * (index = CP call id + 1: ModemManager takes index 0 as unknown),
 * RING/+CLIP and NO CARRIER are generated from its changes.
 */
#define MAX_CALLS	8

struct call {
	unsigned int id, mt, stat, mpty, intl;
	char num[84];
};

static struct {
	struct call c[MAX_CALLS];
	int n;
	int dial;		/* ATD waiting for the CP */
	time_t dial_since;
	time_t ring;		/* last RING sent */
} vc;

static void call_send(uint8_t index, uint8_t type, const uint8_t *data, size_t len, size_t total)
{
	uint8_t f[128] = { 0 };

	f[4] = 0x02;
	f[5] = index;
	f[6] = type;
	if (len)
		memcpy(f + 7, data, len);
	fmt_send(f, total < 7 + len ? 7 + len : total);
}

static void call_get_list(void)
{
	call_send(0x06, 0x02, NULL, 0, 7);
}

static void call_clip(const struct call *c, int waiting)
{
	char out[160];

	if (waiting)
		snprintf(out, sizeof(out), "\r\n+CCWA: \"%s\",%u,1\r\n",
			 c->num, c->intl ? 145 : 129);
	else
		snprintf(out, sizeof(out), "\r\nRING\r\n\r\n+CLIP: \"%s\",%u\r\n",
			 c->num, c->intl ? 145 : 129);
	px_reply(out);
}

static void call_list_rx(const uint8_t *p, size_t n)
{
	struct call old[MAX_CALLS];
	int nold = vc.n, i, j;
	size_t o = 8;

	memcpy(old, vc.c, sizeof(old));
	vc.n = 0;
	for (i = 0; i < p[7] && vc.n < MAX_CALLS && o + 8 <= n; i++) {
		struct call *c = &vc.c[vc.n];
		size_t len = p[o + 6];

		if (o + 8 + len > n || !p[o + 4])
			break;
		c->id = p[o + 2];
		c->mt = p[o + 3] != 1;
		c->stat = p[o + 4] - 1;
		c->mpty = p[o + 5] == 1;
		c->intl = ((p[o + 7] >> 4) & 7) == 1;
		if (len > sizeof(c->num) - 2)
			len = sizeof(c->num) - 2;
		c->num[0] = 0;
		if (c->intl && len && p[o + 8] != '+')
			strcpy(c->num, "+");
		strncat(c->num, (const char *)p + o + 8, len);
		vc.n++;
		o += 8 + p[o + 6];
	}
	stamp();
	printf("CALL list: %d call(s)", vc.n);
	for (i = 0; i < vc.n; i++)
		printf(" [id %u %s stat %u]", vc.c[i].id, vc.c[i].mt ? "MT" : "MO", vc.c[i].stat);
	printf("\n");

	/* new incoming / waiting calls */
	for (i = 0; i < vc.n; i++) {
		int seen = 0;

		for (j = 0; j < nold; j++)
			if (old[j].id == vc.c[i].id)
				seen = 1;
		if (!seen && vc.c[i].stat == 4) {
			call_clip(&vc.c[i], 0);
			vc.ring = now_s();
		} else if (!seen && vc.c[i].stat == 5) {
			call_clip(&vc.c[i], 1);
		}
	}
	/* calls gone: ModemManager re-reads +CLCC on NO CARRIER */
	for (j = 0; j < nold; j++) {
		int still = 0;

		for (i = 0; i < vc.n; i++)
			if (old[j].id == vc.c[i].id)
				still = 1;
		if (!still) {
			px_reply("\r\nNO CARRIER\r\n");
			break;
		}
	}
}

static void call_rx(const uint8_t *p, size_t n)
{
	if (n >= 12 && p[4] == 0x80 && p[5] == 0x01 && p[7] == 0x02) {
		unsigned int code = p[10] | p[11] << 8;

		stamp();
		printf("CALL 02%02x -> 0x%04x\n", p[8], code);
		if (vc.dial && p[8] == 0x01) {
			vc.dial = 0;
			px_reply(code == 0x8000 ? "\r\nOK\r\n" : "\r\nNO CARRIER\r\n");
		}
		return;
	}
	if (n < 7 || p[4] != 0x02)
		return;
	if (p[5] == 0x06 && p[6] == 0x02 && n >= 8) {
		call_list_rx(p, n);
		return;
	}
	if (p[5] == 0x05 && n >= 11) {
		stamp();
		printf("CALL status id %u state %u cause %u/%u\n", p[9], p[10],
		       n >= 13 ? p[11] : 0, n >= 13 ? p[12] : 0);
		if (vc.dial && p[10] == 1) {
			vc.dial = 0;
			px_reply("\r\nOK\r\n");
		}
	}
	if (p[5] == 0x02 || p[5] == 0x05 || p[5] == 0x06 || p[5] == 0x09)
		call_get_list();
}

static void call_tick(void)
{
	int i;

	if (vc.dial && now_s() - vc.dial_since > 10) {
		vc.dial = 0;
		px_reply("\r\nNO CARRIER\r\n");
	}
	for (i = 0; i < vc.n; i++)
		if (vc.c[i].stat == 4 && now_s() - vc.ring >= 3) {
			call_clip(&vc.c[i], 0);
			vc.ring = now_s();
		}
}

static void call_manage(uint8_t type, uint8_t id)
{
	uint8_t d[2] = { type, id };

	call_send(0x0c, 0x03, d, 2, 92);
}

/* voice AT commands from ModemManager; 0 = not one of them */
static int call_at(const char *cmd)
{
	char out[1024];
	size_t len, o;
	unsigned int a;
	int i;

	if (!strcmp(cmd, "ATH") || !strcmp(cmd, "ATH0")) {
		/*
		 * Only ModemManager's voice support probe (it hangs up with
		 * +CHUP); it also comes after a resume, with a call up.
		 */
	} else if (!strcmp(cmd, "AT+CHUP")) {
		if (vc.n || vc.dial)
			call_send(0x03, 0x01, NULL, 0, 7);	/* RELEASE */
	} else if (!strcmp(cmd, "ATA")) {
		call_send(0x04, 0x01, NULL, 0, 7);		/* ANSWER */
	} else if (!strncmp(cmd, "ATD", 3) && cmd[strlen(cmd) - 1] == ';') {
		uint8_t d[92] = { 0x00, 0x01, 0 };	/* CS voice, CLIR default */

		len = strlen(cmd) - 4;
		if (len < 1 || len > 82)
			return px_reply("\r\nERROR\r\n"), 1;
		d[3] = len;
		d[4] = cmd[3] == '+' ? 0x11 : 0x21;
		memcpy(d + 5, cmd + 3, len);
		call_send(0x01, 0x01, d, sizeof(d), 99);	/* ORIGINATE */
		vc.dial = 1;
		vc.dial_since = now_s();
		return 1;	/* OK when the CP accepts */
	} else if (!strcmp(cmd, "AT+CLCC")) {
		o = snprintf(out, sizeof(out), "\r\n");
		for (i = 0; i < vc.n; i++)
			o += snprintf(out + o, sizeof(out) - o,
				      "+CLCC: %u,%u,%u,0,%u,\"%s\",%u\r\n",
				      vc.c[i].id + 1, vc.c[i].mt, vc.c[i].stat,
				      vc.c[i].mpty, vc.c[i].num,
				      vc.c[i].intl ? 145 : 129);
		snprintf(out + o, sizeof(out) - o, "\r\nOK\r\n");
		px_reply(out);
		return 1;
	} else if (!strcmp(cmd, "AT+CCWA=1,2")) {
		px_reply("\r\n+CCWA: 0,1\r\n\r\nOK\r\n");
		return 1;
	} else if (sscanf(cmd, "AT+CHLD=%u", &a) == 1) {
		if (a == 0)
			call_manage(1, 0xff);
		else if (a == 1)
			call_manage(2, 0xff);
		else if (a == 2)
			call_manage(4, 0xff);
		else if (a == 3)
			call_manage(6, 0xff);
		else if (a == 4)
			call_manage(7, 0xff);
		else if (a >= 11 && a < 20 && vc.n <= 1)
			call_send(0x03, 0x01, NULL, 0, 7);	/* the only call: RELEASE */
		else if (a >= 11 && a < 20)
			call_manage(3, a - 11);
		else if (a >= 21 && a < 30)
			call_manage(5, a - 21);
		else
			return px_reply("\r\nERROR\r\n"), 1;
	} else if (!strncmp(cmd, "AT+VTS=", 7) && cmd[7]) {
		uint8_t d[2] = { 1, (uint8_t)cmd[7] };

		call_send(0x08, 0x03, d, 2, 9);
		usleep(250000);
		d[0] = 2;
		d[1] = 0;
		call_send(0x08, 0x03, d, 2, 9);
	} else if (!strcmp(cmd, "AT+CLCC=?") || !strncmp(cmd, "AT+CLIP=", 8) ||
		   !strncmp(cmd, "AT+CRC=", 7) || !strncmp(cmd, "AT+CCWA=", 8) ||
		   !strncmp(cmd, "AT+VTD=", 7)) {
		;	/* nothing to set up: the URCs come from here */
	} else {
		return 0;
	}
	px_reply("\r\nOK\r\n");
	return 1;
}

/*
 * The CP's voice firmware (VSS) runs on the audio DSP (ABOX). cbd loads it
 * before starting the CP; at every CP power-up the ABOX driver is asked to
 * start it (its start_vss attribute, which boots the DSP first if needed).
 * Without it the CP crashes a few seconds into a call. In a child: booting
 * the DSP takes a while and the CP's start-up file requests cannot wait.
 */
static void vss_start(void)
{
	const char *dir = "/sys/bus/platform/drivers/samsung-abox";
	char path[512];
	struct dirent *e;
	DIR *d;
	int fd;

	if (fork() != 0)
		return;
	d = opendir(dir);
	while (d && (e = readdir(d))) {
		snprintf(path, sizeof(path), "%s/%s/start_vss", dir, e->d_name);
		fd = open(path, O_WRONLY);
		if (fd < 0)
			continue;
		if (write(fd, "1", 1) < 0)
			perror("mifmon: start_vss");
		else
			printf("VSS start requested\n");
		close(fd);
	}
	fflush(stdout);
	_exit(0);
}

static void dc_mm_cgact(int on, unsigned int cid);
static void dc_mm_status(char *out, size_t len);
static int dc_mm_contrdp(char *out, size_t len);

/*
 * Commands answered here instead of by the CP:
 *  - ATZ (reset to the stored profile): the CP says +CME ERROR: 100, and
 *    it is the first command of ModemManager's enable sequence.
 *  - with -M -d, the data call commands of ModemManager's shannon plugin:
 *    AT+CGACT=1/0,<cid>, AT+CGACT?, AT+CGCONTRDP[=<cid>]. The CP's own
 *    answers would describe the AT-level context, which is not the one
 *    routed to rmnet; the IPC data call is. Any <cid> maps onto the IPC
 *    call of context -c (rmnet0).
 */
static int px_local(const uint8_t *p, size_t n)
{
	char cmd[64], out[256];
	size_t i, len = 0;
	unsigned int a, b;

	for (i = 0; i < n && len < sizeof(cmd) - 1; i++)
		if (p[i] != '\r' && p[i] != '\n')
			cmd[len++] = toupper(p[i]);
	cmd[len] = 0;
	if (i < n || len == n)
		return 0;	/* too long, or not a full command line */

	if (!strcmp(cmd, "ATZ") || !strcmp(cmd, "ATZ0")) {
		px_reply("\r\nOK\r\n");
	} else if (!strcmp(cmd, "AT+CGDCONT=?")) {
		/*
		 * The CP's fixed answer. Asked by ModemManager right after a CP
		 * boot, when our AT+CGATT=1 can hold the channel for longer than
		 * its timeout; without it ModemManager sees no IP support.
		 */
		px_reply("\r\n+CGDCONT: (1-11),\"IP\",,,(0-3),(0-4)\r\n"
			 "\r\n+CGDCONT: (1-11),\"PPP\",,,(0-3),(0-4)\r\n\r\nOK\r\n");
	} else if (sms_at(cmd) || rat_at(cmd) || call_at(cmd)) {
		;
	} else if (dc.on_demand && sscanf(cmd, "AT+CGACT=%u,%u", &a, &b) == 2 && a <= 1) {
		dc_mm_cgact(a, b);	/* replies itself, maybe later */
	} else if (dc.on_demand && !strcmp(cmd, "AT+CGACT?")) {
		dc_mm_status(out, sizeof(out));
		px_reply(out);
	} else if (dc.on_demand && !strncmp(cmd, "AT+CGCONTRDP", 12)) {
		if (dc_mm_contrdp(out, sizeof(out)))
			px_reply(out);
		else
			px_reply("\r\n+CME ERROR: 3\r\n");
	} else {
		return 0;
	}
	stamp();
	printf("MM %s answered locally\n", cmd);
	return 1;
}

/*
 * The CP's own call result codes and URCs on the AT channel (it reports
 * calls there too, e.g. CONNECT when a call is answered). ModemManager gets
 * ours from the IPC call state instead; a stray CONNECT made it take the
 * port for a data call and probe the modem all over again.
 */
static int cp_call_urc(const uint8_t *l, size_t n)
{
	static const char *const urc[] = {
		"CONNECT", "RING", "NO CARRIER", "BUSY", "NO ANSWER",
		"+CLIP:", "+CRING:", "+CCWA:", "+COLP:",
	};
	size_t i, k;

	for (i = 0; i < sizeof(urc) / sizeof(urc[0]); i++) {
		k = strlen(urc[i]);
		if (n >= k && !memcmp(l, urc[i], k) &&
		    (urc[i][k - 1] == ':' || n == k || l[k] == ' '))
			return 1;
	}
	return 0;
}

/* p without the lines above; returns the new length (in place) */
static size_t cp_filter(uint8_t *p, size_t n)
{
	size_t i = 0, w = 0, ds, ls;

	while (i < n) {
		ds = i;
		while (i < n && (p[i] == '\r' || p[i] == '\n'))
			i++;
		ls = i;
		while (i < n && p[i] != '\r' && p[i] != '\n')
			i++;
		if (i > ls && cp_call_urc(p + ls, i - ls))
			continue;
		memmove(p + w, p + ds, i - ds);
		w += i - ds;
	}
	return w;
}

/* bytes from ModemManager */
static void px_mm_rx(const uint8_t *p, size_t n)
{
	if (px.prompt && (memchr(p, 0x1a, n) || memchr(p, 0x1b, n)))
		px.prompt = 0;	/* PDU sent (Ctrl-Z) or cancelled (ESC) */
	/*
	 * Answered here even while our own command holds the CP or a
	 * command of ModemManager's is still unanswered: ModemManager sends
	 * the next command only after a reply or its timeout.
	 */
	if (px_local(p, n))
		return;
	if (px.own_active || px.own_wait) {
		/* a command still queued here was given up by ModemManager */
		if (n <= sizeof(px.q)) {
			memcpy(px.q, p, n);
			px.qlen = n;
		}
		return;
	}
	if (write(px.router, p, n) < 0)
		perror("mifmon: write umts_router");
	if (!px.mm_pending)
		px.rx[0] = 0;
	px.mm_pending = 1;
	px.mm_since = 0;
}

/*
 * CP output while our own command runs: our commands (CGDCONT, CGATT)
 * print no "+..." lines except errors, so any other "+..." line is a URC
 * (+CREG/+CGREG/+CEREG, +CMTI, +CUSD, ...) and goes to ModemManager.
 */
static void px_forward_urcs(const uint8_t *p, size_t n)
{
	size_t i = 0, start, end;

	if (px.fd < 0)
		return;
	while (i < n) {
		while (i < n && (p[i] == '\r' || p[i] == '\n'))
			i++;
		start = i;
		while (i < n && p[i] != '\r' && p[i] != '\n')
			i++;
		end = i;
		if (end > start && p[start] == '+' &&
		    !(end - start >= 10 && !memcmp(p + start, "+CME ERROR", 10))) {
			if (write(px.fd, "\r\n", 2) < 0 ||
			    write(px.fd, p + start, end - start) < 0 ||
			    write(px.fd, "\r\n", 2) < 0)
				perror("mifmon: write wwan_at_ctl");
		}
	}
}

/*
 * CP output not belonging to our own command: to ModemManager. The CP
 * prompts for SMS text (AT+CMGS) with "\n> "; ModemManager waits for
 * "\r\n> ", so the prompt is passed on in that form.
 */
static void px_cp_rx(const uint8_t *p, size_t n)
{
	size_t have = strlen(px.rx);

	if (px.mm_pending && n >= 3 && !memcmp(p + n - 3, "\n> ", 3) &&
	    (n == 3 || p[n - 4] != '\r')) {
		if (px.fd >= 0 && (write(px.fd, p, n - 3) < 0 ||
				   write(px.fd, "\r\n> ", 4) < 0))
			perror("mifmon: write wwan_at_ctl");
		if (!px.prompt)
			px.prompt = now_s();
		return;
	}
	if (px.fd >= 0 && write(px.fd, p, n) < 0)
		perror("mifmon: write wwan_at_ctl");
	if (!px.mm_pending)
		return;		/* URC */
	if (have + n >= sizeof(px.rx))
		have = 0;
	memcpy(px.rx + have, p, n);
	px.rx[have + n] = 0;
	if (at_final(px.rx)) {
		px.mm_pending = 0;
		px.rx[0] = 0;
		px_idle();
	}
}

static void at_send_now(const char *cmd)
{
	char line[256];

	snprintf(line, sizeof(line), "%s\r", cmd);
	stamp();
	printf("ROUTER tx \"%s\\r\"\n", cmd);
	dc.at_rx[0] = 0;
	px.own_active = 1;
	if (write(dc.at_fd, line, strlen(line)) < 0)
		perror("mifmon: write umts_router");
}

static void at_send(const char *cmd)
{
	if (px.mm_pending) {
		/* after ModemManager's command completes */
		snprintf(px.own, sizeof(px.own), "%.150s", cmd);
		px.own_wait = 1;
		return;
	}
	at_send_now(cmd);
}

/* our own AT command outside the data call sequence; 0 if busy */
static int own_at(const char *cmd)
{
	if (dc.at_fd < 0 || px.own_active || px.own_wait)
		return 0;
	px.ctl = 1;
	px.ctlrx[0] = 0;
	at_send(cmd);
	return 1;
}

/* one line from /run/mifmon.ctl */
static void ctl_line(char *l)
{
	uint8_t f[512];
	size_t n;

	if (!strncmp(l, "fmt ", 4)) {
		n = parse_hex(l + 4, f + 4, sizeof(f) - 4);
		if (n >= 3)
			fmt_send(f, n + 4);
	} else if (!strncmp(l, "at ", 3) && own_at(l + 3)) {
		;
	} else {
		fprintf(stderr, "mifmon: ctl: bad line \"%s\"\n", l);
	}
}

static void ctl_rx(int fd)
{
	static char line[1100];
	static size_t len;
	char c;

	while (read(fd, &c, 1) == 1) {
		if (c == '\n') {
			line[len] = 0;
			if (len)
				ctl_line(line);
			len = 0;
		} else if (len < sizeof(line) - 1) {
			line[len++] = c;
		}
	}
}

static time_t now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

/*
 * The CP does not attach to LTE by itself: the stock RIL gives it an
 * initial-attach context first. That part is done over AT (CGDCONT +
 * CGATT); an AT-activated context is not routed to rmnet, so the data
 * call itself goes over Samsung IPC (define + activate).
 */
static void dc_send_define(void)
{
	uint8_t f[134] = { 0 };

	f[4] = 0x0d; f[5] = 0x01; f[6] = 3;
	f[7] = 1;
	f[8] = dc.cid;
	f[9] = 6;	/* IPv4v6 (RIL DataProtocol 3) */
	strncpy((char *)f + 10, dc.apn, 100);
	dc.step = DC_DEFINE;
	dc.deadline = now_s() + 20;
	fmt_send(f, 134);
}

static void dc_send_activate(void)
{
	uint8_t f[112] = { 0 };

	f[4] = 0x0d; f[5] = 0x04; f[6] = 3;
	f[7] = 1;
	f[8] = dc.cid;
	dc.step = DC_ACTIVATE;
	dc.deadline = now_s() + 20;
	fmt_send(f, 112);
}

static void dc_next(void)
{
	uint8_t f[10] = { 0 };
	char cmd[160];

	switch (dc.step) {
	case DC_OFF:
		if (dc.ipc_attach) {
			dc_send_define();
			break;
		}
		/* initial-attach context */
		snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%u,\"IPV4V6\",\"%s\"",
			 dc.cid, dc.apn);
		dc.step = DC_AT_CGDCONT;
		dc.deadline = now_s() + 10;
		at_send(cmd);
		break;
	case DC_AT_CGDCONT:	/* attach; OK once registered on PS */
		dc.step = DC_AT_CGATT;
		dc.deadline = now_s() + 60;
		at_send("AT+CGATT=1");
		break;
	case DC_AT_CGATT:	/* define the context over IPC */
		if (dc.on_demand && !dc.want) {
			/* ModemManager decides when to bring the call up */
			dc.step = DC_ATTACHED;
			dc.deadline = 0;
			stamp();
			printf("DATA attached, data call on request\n");
			break;
		}
		dc_send_define();
		break;
	case DC_DEFINE:
		if (!dc.ipc_attach) {
			dc_send_activate();
			break;
		}
		/* GPRS_PS attach over IPC */
		f[4] = 0x0d; f[5] = 0x03; f[6] = 3;
		f[7] = 1;
		dc.step = DC_PS_ATTACH;
		dc.deadline = now_s() + 20;
		fmt_send(f, 10);
		break;
	case DC_PS_ATTACH:
		dc_send_activate();
		break;
	default:
		break;
	}
}

/* AT replies during the attach phase */
static void dc_at_rx(const uint8_t *p, size_t n)
{
	size_t have = strlen(dc.at_rx);

	if (dc.step != DC_AT_CGDCONT && dc.step != DC_AT_CGATT)
		return;
	if (have + n >= sizeof(dc.at_rx))
		have = 0;
	memcpy(dc.at_rx + have, p, n);
	dc.at_rx[have + n] = 0;
	if (strstr(dc.at_rx, "\r\nOK\r\n")) {
		px.own_active = 0;
		dc_next();
		px_idle();
	} else if (strstr(dc.at_rx, "ERROR")) {
		px.own_active = 0;
		stamp();
		printf("DATA attach step failed, retrying in 10 s\n");
		dc.step = DC_AT_CGDCONT;	/* retry CGATT */
		dc.deadline = now_s() + 10;
		dc.at_rx[0] = 0;
		px_idle();
	}
}

/* start over (CP rebooted, or a step got no answer) after delay seconds */
static void dc_restart(int delay)
{
	if (dc.mm_act_waiting) {
		dc.mm_act_waiting = 0;
		dc.want = 0;
		px_reply("\r\n+CME ERROR: 148\r\n");	/* unspecified GPRS error */
	}
	px.own_active = 0;
	px.own_wait = 0;
	dc.step = DC_OFF;
	dc.deadline = now_s() + delay;
}

/* called on poll timeout / every loop */
static void dc_tick(void)
{
	if (!dc.deadline || now_s() < dc.deadline)
		return;
	switch (dc.step) {
	case DC_OFF:
		dc_next();
		break;
	case DC_AT_CGDCONT:
	case DC_AT_CGATT:
		px.own_active = 0;
		stamp();
		printf("DATA no attach yet, retrying\n");
		dc.step = dc.step == DC_AT_CGATT ? DC_AT_CGDCONT : DC_OFF;
		dc_next();
		break;
	case DC_PS_ATTACH:
		if (dc.ipc_attach) {
			dc_next();	/* retry the activate */
			break;
		}
		/* fall through */
	case DC_DEFINE:
	case DC_ACTIVATE:
		stamp();
		printf("DATA no answer from the CP, starting over\n");
		dc_restart(0);
		break;
	default:
		break;
	}
}

static void set_mtu(const char *iface, unsigned int mtu)
{
	struct ifreq ifr;
	int sk;

	if (!mtu)
		return;
	sk = socket(AF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
	ifr.ifr_mtu = mtu;
	if (ioctl(sk, SIOCSIFMTU, &ifr) < 0)
		perror("mifmon: SIOCSIFMTU");
	close(sk);
}

/*
 * ModemManager's AT+CGACT=<on>,<cid>. Only context <dc.cid> is routed to
 * the net port ModemManager uses, so whatever context ModemManager picked
 * (it creates a new one when the IP type differs, e.g. "IP" vs the
 * attach context's "IPV4V6") is served by the IPC call on <dc.cid>.
 */
static void dc_mm_cgact(int on, unsigned int cid)
{
	if (!on) {
		/* keep the IPC call (the last LTE PDN cannot go anyway) */
		if (cid == dc.mm_cid)
			dc.want = 0;
		px_reply("\r\nOK\r\n");
		return;
	}
	dc.mm_cid = cid;
	dc.want = 1;
	if (dc.step == DC_UP) {
		px_reply("\r\nOK\r\n");
		return;
	}
	dc.mm_act_waiting = 1;	/* OK once the CP reports the IP */
	if (dc.step == DC_ATTACHED)
		dc_send_define();
	/* otherwise the attach sequence runs on into the data call */
}

/* ModemManager's AT+CGACT? */
static void dc_mm_status(char *out, size_t len)
{
	snprintf(out, len, "\r\n+CGACT: %u,%d\r\n\r\nOK\r\n",
		 dc.mm_cid ? dc.mm_cid : dc.cid, dc.want && dc.step == DC_UP);
}

/*
 * ModemManager's AT+CGCONTRDP: the IPC call's address as a /31 with the
 * other address of the pair as gateway (rmnet is raw IP: the gateway is
 * only there to make a default route).
 */
static int dc_mm_contrdp(char *out, size_t len)
{
	unsigned int a, b, c, d;

	if (dc.step != DC_UP || !dc.want ||
	    sscanf(dc.ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return 0;
	snprintf(out, len,
		 "\r\n+CGCONTRDP: %u,5,\"\",\"%s.255.255.255.254\",\"%u.%u.%u.%u\",\"%s\",\"%s\"\r\n\r\nOK\r\n",
		 dc.mm_cid ? dc.mm_cid : dc.cid, dc.ip, a, b, c, d ^ 1, dc.dns1, dc.dns2);
	return 1;
}

static void dc_ip_config(const uint8_t *p, size_t n)
{
	char ip[16], dns1[16], dns2[16], iface[16], num[8];
	unsigned int mtu = 0;
	pid_t pid;

	if (n < 22 || p[7] != dc.cid)
		return;
	snprintf(ip, sizeof(ip), "%u.%u.%u.%u", p[10], p[11], p[12], p[13]);
	snprintf(dns1, sizeof(dns1), "%u.%u.%u.%u", p[14], p[15], p[16], p[17]);
	snprintf(dns2, sizeof(dns2), "%u.%u.%u.%u", p[18], p[19], p[20], p[21]);
	if (n >= 0xa1)
		mtu = p[0x9f] | p[0xa0] << 8;
	snprintf(iface, sizeof(iface), "rmnet%u", dc.cid - 1);
	stamp();
	printf("DATA cid=%u iface=%s ip=%s dns=%s,%s mtu=%u\n",
	       dc.cid, iface, ip, dns1, dns2, mtu);
	dc.step = DC_UP;
	dc.deadline = 0;
	snprintf(dc.ip, sizeof(dc.ip), "%s", ip);
	snprintf(dc.dns1, sizeof(dc.dns1), "%s", dns1);
	snprintf(dc.dns2, sizeof(dc.dns2), "%s", dns2);
	set_mtu(iface, mtu);
	if (dc.mm_act_waiting) {
		dc.mm_act_waiting = 0;
		px_reply("\r\nOK\r\n");
	}
	if (!dc.hook)
		return;

	snprintf(num, sizeof(num), "%u", dc.cid);
	setenv("CID", num, 1);
	setenv("IFACE", iface, 1);
	setenv("IP", ip, 1);
	setenv("DNS1", dns1, 1);
	setenv("DNS2", dns2, 1);
	snprintf(num, sizeof(num), "%u", mtu);
	setenv("MTU", num, 1);
	pid = fork();
	if (pid == 0) {
		execl(dc.hook, dc.hook, (char *)NULL);
		perror("mifmon: hook");
		_exit(127);
	}
}

/* called for every FMT frame from the CP while a data call is wanted */
static void dc_rx(const uint8_t *p, size_t n)
{
	if (n < 7)
		return;
	if (p[4] == 0x01 && p[5] == 0x01) {
		/* PWR_PHONE_PWR_UP: the CP (re)booted, its contexts are gone */
		stamp();
		printf("DATA CP powered up, setting up the data call again\n");
		if (dc.on_demand)
			dc.want = 0;	/* MM sees the call gone and reconnects */
		dc_restart(5);
		return;
	}
	if (p[4] == 0x80 && p[5] == 0x01 && n >= 12 && p[7] == 0x0d) {
		/* GEN_PHONE_RES for one of ours: group, index, type, code */
		unsigned int code = p[10] | p[11] << 8;

		stamp();
		printf("DATA 0d%02x -> 0x%04x\n", p[8], code);
		if ((p[8] == 0x01 && dc.step == DC_DEFINE) ||
		    (p[8] == 0x03 && dc.step == DC_PS_ATTACH))
			dc_next();	/* attach: 0x8005 = already attached */
		else if (p[8] == 0x04 && dc.step == DC_ACTIVATE && code != 0x8000) {
			if (dc.ipc_attach) {
				/* not attached yet: try the activate again */
				printf("DATA activate refused, not attached yet? retrying in 10 s\n");
				dc.step = DC_PS_ATTACH;
				dc.deadline = now_s() + 10;
			} else {
				printf("DATA activate refused (-c/-A right?), retrying in 30 s\n");
				dc_restart(30);
			}
		}
	} else if (p[4] == 0x0d && p[5] == 0x09) {
		dc_ip_config(p, n);
	} else if (p[4] == 0x0d && p[5] == 0x10 && n >= 9 && p[7] == dc.cid) {
		stamp();
		printf("DATA call status cid=%u state=%u%s\n", p[7], p[8],
		       p[8] == 1 ? " (active)" : "");
		if (p[8] != 1 && dc.step == DC_UP) {
			printf("DATA call dropped, setting it up again in 10 s\n");
			if (dc.on_demand)
				dc.want = 0;
			dc_restart(10);
		}
	}
}

int main(int argc, char **argv)
{
	static uint8_t buf[BUF_SIZE];
	const char *at_cmd = NULL, *fmt_hex[8];
	int nfmt = 0;
	struct pollfd pfd[5];
	int opt, i, data = 0, proxy = 0;

	dc.cid = 1;
	dc.apn = "";
	dc.mseq = 0x40;
	rfs.efs = "/var/lib/android-modem/efs";
	while ((opt = getopt(argc, argv, "a:f:dIc:A:s:re:M")) != -1) {
		switch (opt) {
		case 'a': at_cmd = optarg; break;
		case 'f':
			if (nfmt < 8)
				fmt_hex[nfmt++] = optarg;
			break;
		case 'd': data = 1; break;
		case 'I': data = 1; dc.ipc_attach = 1; break;
		case 'c': dc.cid = atoi(optarg); break;
		case 'A': dc.apn = optarg; break;
		case 's': dc.hook = optarg; break;
		case 'r': rfs.buf = malloc(RFS_MAX); break;
		case 'e': rfs.efs = optarg; break;
		case 'M': proxy = 1; break;
		default:
			fprintf(stderr, "usage: %s [-a AT-command] [-f hex-fmt-frame]"
				" [-d [-c cid] [-A apn] [-s hook]] [-r [-e efs]]\n", argv[0]);
			return 2;
		}
	}
	if (dc.cid < 1 || dc.cid > 8) {
		fprintf(stderr, "mifmon: cid must be 1..8\n");
		return 2;
	}
	signal(SIGCHLD, SIG_IGN);	/* hook and vss_start() processes */
	setvbuf(stdout, NULL, _IOLBF, 0);

	pfd[0].fd = open_dev("/dev/umts_ipc0");
	pfd[1].fd = open_dev("/dev/umts_rfs0");
	/* -I leaves the AT port to ModemManager */
	pfd[2].fd = dc.ipc_attach && !at_cmd && !proxy ? -1 : open_dev("/dev/umts_router");
	pfd[3].fd = proxy ? open_dev("/dev/wwan_at_ctl") : -1;
	px.fd = pfd[3].fd;
	px.router = pfd[2].fd;
	for (i = 0; i < 4; i++)
		pfd[i].events = POLLIN;
	if (pfd[0].fd < 0 || pfd[1].fd < 0)
		return 1;

	for (i = 0; i < nfmt; i++) {
		size_t n = parse_hex(fmt_hex[i], buf, sizeof(buf));

		show_fmt("tx", buf, n);
		if (write(pfd[0].fd, buf, n) < 0)
			perror("mifmon: write umts_ipc0");
	}
	unlink("/run/mifmon.ctl");
	if (mkfifo("/run/mifmon.ctl", 0600) == 0)
		pfd[4].fd = open("/run/mifmon.ctl", O_RDWR | O_NONBLOCK);
	else
		pfd[4].fd = -1;
	pfd[4].events = POLLIN;
	dc.fd = pfd[0].fd;
	dc.at_fd = pfd[2].fd;
	rfs.fd = pfd[1].fd;
	/* with ModemManager, it decides when the data call is up */
	dc.on_demand = proxy && data && !dc.ipc_attach && px.fd >= 0;
	if (data && !dc.ipc_attach && dc.at_fd < 0) {
		fprintf(stderr, "mifmon: -d needs /dev/umts_router\n");
		return 1;
	}
	if (data)
		dc_next();
	if (at_cmd && pfd[2].fd >= 0) {
		snprintf((char *)buf, sizeof(buf), "%s\r", at_cmd);
		stamp();
		printf("ROUTER tx \"%s\\r\"\n", at_cmd);
		if (write(pfd[2].fd, buf, strlen((char *)buf)) < 0)
			perror("mifmon: write umts_router");
	}

	for (;;) {
		if (data)
			dc_tick();
		sms_announce();
		rat_tick();
		call_tick();
		if (px.prompt && now_s() - px.prompt > 20) {
			/*
			 * MM gave up on an SMS (no PDU after the prompt): leave
			 * text entry, or the CP takes every later command as
			 * message text.
			 */
			stamp();
			printf("SMS prompt abandoned: ESC\n");
			px.prompt = 0;
			if (write(px.router, "\x1b", 1) < 0)
				perror("mifmon: write umts_router");
		}
		if (px.mm_pending && px.own_wait) {
			/* MM never got a final result: do not block our attach */
			if (!px.mm_since)
				px.mm_since = now_s();
			else if (now_s() - px.mm_since > 30) {
				px.mm_pending = 0;
				px_idle();
			}
		}
		if (poll(pfd, 5, data || proxy ? 2000 : -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("mifmon: poll");
			return 1;
		}
		for (i = 0; i < 5; i++) {
			ssize_t n;

			if (pfd[i].fd < 0 || !pfd[i].revents)
				continue;
			if (i == 4) {
				ctl_rx(pfd[i].fd);
				continue;
			}
			if (pfd[i].revents & (POLLERR | POLLHUP)) {
				stamp();
				printf("channel %d: %s (CP state change)\n", i,
				       pfd[i].revents & POLLHUP ? "HUP" : "ERR");
			}
			n = read(pfd[i].fd, buf, sizeof(buf));
			if (n <= 0)
				continue;
			if (i == 0) {
				show_fmt("rx", buf, n);
				if (n >= 7 && buf[4] == 0x04 && buf[5] == 0x02)
					sms_incoming(buf, n);
				if (n >= 7 && buf[4] == 0x01 && buf[5] == 0x01)
					vss_start();	/* PWR_PHONE_PWR_UP */
				rat_rx(buf, n);
				call_rx(buf, n);
				if (data)
					dc_rx(buf, n);
			}
			else if (i == 1) {
				if (rfs.buf)
					rfs_rx(buf, n);
				else
					show_rfs("rx", buf, n);
			} else if (i == 2) {
				show_raw("ROUTER", buf, n);
				n = cp_filter(buf, n);
				if (!n)
					continue;
				if (px.own_active && px.ctl) {
					size_t h = strlen(px.ctlrx);

					if (h + n >= sizeof(px.ctlrx))
						h = 0;
					memcpy(px.ctlrx + h, buf, n);
					px.ctlrx[h + n] = 0;
					px_forward_urcs(buf, n);
					if (at_final(px.ctlrx)) {
						px.own_active = px.ctl = 0;
						px_idle();
					}
				} else if (data && px.own_active) {
					px_forward_urcs(buf, n);
					dc_at_rx(buf, n);
				} else
					px_cp_rx(buf, n);
			} else {
				show_raw("MM", buf, n);
				px_mm_rx(buf, n);
			}
		}
	}
}
