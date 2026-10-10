// SPDX-License-Identifier: GPL-2.0
/*
 * smcprobe - go/no-go test for CP (modem) control through the EL3 monitor.
 *
 * With CP secure boot the PMU CP_CTRL_S register is only writable through
 * SMC 0x82000700 (READ_CTRL = 3, WRITE_CTRL = 4; arg3 selects CP_CTRL_S (0)
 * or CP_CTRL_NS (1); a read returns 16 bits: status in [15:0], value in
 * [31:16], arg2 picks the low (0) or high (1) half). Other SMCs hang on this
 * boot chain (UFS FMP, ABOX SMC_CMD_REG), so try this one alone first:
 *
 *   insmod smcprobe.ko            SMC reads only (default)
 *   insmod smcprobe.ko write=1    also write CP_CTRL_NS back unchanged
 *
 * Plain non-secure reads of CP_CTRL_NS/S (0x16480030/34) abort, confirmed on
the phone; pmu=1 tries the other status registers one by one.
 */
#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/module.h>

#define SMC_ID		0x82000700
#define READ_CTRL	0x3
#define WRITE_CTRL	0x4
#define CP_CTRL_S	0
#define CP_CTRL_NS	1

#define PMU_BASE	0x16480000
#define PMU_CP_CTRL_NS	0x30
#define PMU_CP_CTRL_S	0x34
#define PMU_CP_STAT	0x38
#define PMU_RESET_SEQ	0x504

static bool write;
module_param(write, bool, 0444);
MODULE_PARM_DESC(write, "write CP_CTRL_NS back with its current value");

/*
 * Plain reads of CP_CTRL_NS/S from the non-secure world take a synchronous
 * external abort on greatlte (oops in init_module), so they are off by
 * default; pmu=1 only reads CP_STAT and RESET_SEQUENCER_STATUS directly.
 */
static bool pmu;
module_param(pmu, bool, 0444);
MODULE_PARM_DESC(pmu, "also read CP_STAT/RESET_SEQ directly (non-secure)");
static long smc(unsigned long a1, unsigned long a2, unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_ID, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

static int smc_read(int reg, u32 *val)
{
	long lo, hi;

	lo = smc(READ_CTRL, 0, reg);
	if (lo & 0xffff)
		return -(int)(lo & 0xffff);
	hi = smc(READ_CTRL, 1, reg);
	if (hi & 0xffff)
		return -(int)(hi & 0xffff);
	*val = ((u32)hi & 0xffff0000) | (((u32)lo >> 16) & 0xffff);
	return 0;
}

static int __init smcprobe_init(void)
{
	void __iomem *base = pmu ? ioremap(PMU_BASE, 0x1000) : NULL;
	u32 s = 0, ns = 0;
	int ret;

	if (base) {
		pr_info("smcprobe: PMU RESET_SEQ=%08x\n", readl(base + PMU_RESET_SEQ));
		pr_info("smcprobe: PMU CP_STAT=%08x\n", readl(base + PMU_CP_STAT));
	}

	pr_info("smcprobe: SMC READ_CTRL CP_CTRL_NS ...\n");
	ret = smc_read(CP_CTRL_NS, &ns);
	pr_info("smcprobe: SMC CP_CTRL_NS: ret=%d val=%08x\n", ret, ns);

	pr_info("smcprobe: SMC READ_CTRL CP_CTRL_S ...\n");
	ret = smc_read(CP_CTRL_S, &s);
	pr_info("smcprobe: SMC CP_CTRL_S: ret=%d val=%08x\n", ret, s);

	if (write && !ret) {
		long w = smc(WRITE_CTRL, ns, CP_CTRL_NS);

		pr_info("smcprobe: SMC WRITE_CTRL CP_CTRL_NS=%08x -> ret=%ld\n", ns, w);
	}

	if (base)
		iounmap(base);
	/* nothing to keep loaded */
	return -EAGAIN;
}
module_init(smcprobe_init);

MODULE_DESCRIPTION("Exynos CP secure-control SMC probe");
MODULE_LICENSE("GPL");
