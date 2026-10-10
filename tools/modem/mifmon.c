// SPDX-License-Identifier: GPL-2.0
/*
 * mifmon - watch the Shannon CP's IPC channels on postmarketOS.
 *
 * The modem driver tells the CP that the AP side is ready (CMD_INIT_END)
 * only while both /dev/umts_ipc0 (FMT: Samsung IPC) and /dev/umts_rfs0
 * (RFS: remote file system) are open, so this tool also acts as the
 * "RIL is up" placeholder during bring-up. Every frame is printed with a
 * timestamp and the decoded header; nothing is answered yet.
 *
 *   mifmon                     log umts_ipc0 + umts_rfs0 (+ umts_router)
 *   mifmon -a 'AT+CPIN?'       also send an AT command on umts_router
 *   mifmon -f <hex bytes>      also send a raw FMT frame on umts_ipc0
 *   mifmon -d [-c cid] [-A apn] [-s hook]
 *                              also bring up a data call (see below)
 *
 * Only one process may read umts_ipc0: a second reader steals the CP's
 * replies. So the data call is set up from here rather than by a separate
 * tool.
 *
 * Data call (decoded from the stock libsec-ril, IpcProtocol41):
 *   GPRS_DEFINE_PDP_CONTEXT 0d 01 SET: u8 1, u8 cid, u8 pdp_type,
 *       char apn[100], ... (134 bytes in all)
 *   GPRS_PS (attach)        0d 03 SET: u8 1, u8 0, u8 0 (10 bytes)
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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* --- data call ---------------------------------------------------------- */

enum dc_step { DC_OFF, DC_DEFINE, DC_ATTACH, DC_ACTIVATE, DC_UP };

static struct {
	enum dc_step step;
	int fd;
	uint8_t cid;
	const char *apn;
	const char *hook;
	uint8_t mseq;
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

static void dc_next(void)
{
	uint8_t f[134];

	memset(f, 0, sizeof(f));
	switch (dc.step) {
	case DC_OFF:		/* define the context */
		f[4] = 0x0d; f[5] = 0x01; f[6] = 3;
		f[7] = 1;
		f[8] = dc.cid;
		f[9] = 6;	/* IPv4v6 (RIL DataProtocol 3) */
		strncpy((char *)f + 10, dc.apn, 100);
		dc.step = DC_DEFINE;
		fmt_send(f, 134);
		break;
	case DC_DEFINE:		/* PS attach (no-op if already attached) */
		f[4] = 0x0d; f[5] = 0x03; f[6] = 3;
		f[7] = 1;
		dc.step = DC_ATTACH;
		fmt_send(f, 10);
		break;
	case DC_ATTACH:		/* activate */
		f[4] = 0x0d; f[5] = 0x04; f[6] = 3;
		f[7] = 1;
		f[8] = dc.cid;
		dc.step = DC_ACTIVATE;
		fmt_send(f, 112);
		break;
	default:
		break;
	}
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
	if (p[4] == 0x80 && p[5] == 0x01 && n >= 12 && p[7] == 0x0d) {
		/* GEN_PHONE_RES for one of ours: group, index, type, code */
		unsigned int code = p[10] | p[11] << 8;

		stamp();
		printf("DATA 0d%02x -> 0x%04x\n", p[8], code);
		if ((p[8] == 0x01 && dc.step == DC_DEFINE) ||
		    (p[8] == 0x03 && dc.step == DC_ATTACH))
			dc_next();	/* attach may fail if already attached */
		else if (p[8] == 0x04 && dc.step == DC_ACTIVATE && code != 0x8000)
			printf("DATA activate refused (registered? -c/-A right?)\n");
	} else if (p[4] == 0x0d && p[5] == 0x09) {
		dc_ip_config(p, n);
	} else if (p[4] == 0x0d && p[5] == 0x10 && n >= 9 && p[7] == dc.cid) {
		stamp();
		printf("DATA call status cid=%u state=%u%s\n", p[7], p[8],
		       p[8] == 1 ? " (active)" : "");
	}
}

int main(int argc, char **argv)
{
	static uint8_t buf[BUF_SIZE];
	const char *at_cmd = NULL, *fmt_hex = NULL;
	struct pollfd pfd[3];
	int opt, i, data = 0;

	dc.cid = 1;
	dc.apn = "";
	dc.mseq = 0x40;
	while ((opt = getopt(argc, argv, "a:f:dc:A:s:")) != -1) {
		switch (opt) {
		case 'a': at_cmd = optarg; break;
		case 'f': fmt_hex = optarg; break;
		case 'd': data = 1; break;
		case 'c': dc.cid = atoi(optarg); break;
		case 'A': dc.apn = optarg; break;
		case 's': dc.hook = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-a AT-command] [-f hex-fmt-frame]"
				" [-d [-c cid] [-A apn] [-s hook]]\n", argv[0]);
			return 2;
		}
	}
	if (dc.cid < 1 || dc.cid > 8) {
		fprintf(stderr, "mifmon: cid must be 1..8\n");
		return 2;
	}
	signal(SIGCHLD, SIG_IGN);	/* hook processes */
	setvbuf(stdout, NULL, _IOLBF, 0);

	pfd[0].fd = open_dev("/dev/umts_ipc0");
	pfd[1].fd = open_dev("/dev/umts_rfs0");
	pfd[2].fd = open_dev("/dev/umts_router");
	for (i = 0; i < 3; i++)
		pfd[i].events = POLLIN;
	if (pfd[0].fd < 0 || pfd[1].fd < 0)
		return 1;

	if (fmt_hex) {
		size_t n = parse_hex(fmt_hex, buf, sizeof(buf));

		show_fmt("tx", buf, n);
		if (write(pfd[0].fd, buf, n) < 0)
			perror("mifmon: write umts_ipc0");
	}
	dc.fd = pfd[0].fd;
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
		if (poll(pfd, 3, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("mifmon: poll");
			return 1;
		}
		for (i = 0; i < 3; i++) {
			ssize_t n;

			if (pfd[i].fd < 0 || !pfd[i].revents)
				continue;
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
				if (data)
					dc_rx(buf, n);
			}
			else if (i == 1)
				show_rfs("rx", buf, n);
			else
				show_raw("ROUTER", buf, n);
		}
	}
}
