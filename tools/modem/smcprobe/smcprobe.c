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
 *   insmod smcprobe.ko kinfo=1 step=1 only query the Kinibi secure OS state
 *   insmod smcprobe.ko seh=1 step=1   only register an EL3 exception handler
 *   insmod smcprobe.ko seh=1          register it, then read CP_CTRL
 *   (with seh=1 the module stays loaded for good; reboot to remove it)
 *
 * Plain non-secure reads of CP_CTRL_NS/S (0x16480030/34) abort, confirmed on
the phone; pmu=1 tries the other status registers one by one.
 */
#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <asm/cputype.h>

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
/*
 * Samsung's kernel registers a "smart exception handler" with the EL3
 * monitor at boot (exynos-el3_mon.c, SMC_CMD_SET_SEH_ADDRESS). If the
 * monitor faults while serving a call it jumps there; without one it may
 * just spin, which looks like a silent freeze. seh=1 registers ours.
 */
#define SMC_CMD_SET_SEH_ADDRESS	(-123)

static bool seh;
module_param(seh, bool, 0444);
MODULE_PARM_DESC(seh, "register an EL3 smart exception handler first");

/*
 * Trustonic Kinibi (the secure OS on stock firmware) status query, the
 * first call Samsung's MobiCore driver makes (drivers/gud, nq_start):
 * MC_FC_INFO(0xFF000002, ext_info_id) -> x1 = ret, x2 = state, x3 = info.
 * state: 0 not initialized, 1 bad init, 2 initialized, 3 halt.
 */
#define MC_FC_INFO		0xFF000002
#define MC_EXT_INFO_ID_MCI_VERSION	0

static bool kinfo;
module_param(kinfo, bool, 0444);
MODULE_PARM_DESC(kinfo, "query the Kinibi secure OS state (MC_FC_INFO) first");

static int step = 2;
module_param(step, int, 0444);
MODULE_PARM_DESC(step, "1: only register the handler, 2: also read CP_CTRL");

static void smcprobe_seh(unsigned int id, unsigned long elr,
			 unsigned long esr, unsigned long sctlr,
			 unsigned long ttbr, unsigned long tcr,
			 unsigned long x6)
{
	pr_emerg("smcprobe: EL3 exception: id=0x%x elr=0x%016lx esr=0x%016lx\n",
		 id, elr, esr);
	pr_emerg("smcprobe: sctlr=0x%016lx ttbr=0x%016lx tcr=0x%016lx x6=0x%016lx\n",
		 sctlr, ttbr, tcr, x6);
	/* keep this CPU here; the others carry the log out */
	for (;;)
		cpu_relax();
}

/*
 * printk only queues the line; /dev/kmsg readers are woken a moment later.
 * If the next SMC never returns, the line is lost. Sleep before each SMC
 * so the message reaches a remote log stream first.
 */
#define say(fmt, ...) do { pr_info(fmt, ##__VA_ARGS__); msleep(1000); } while (0)

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

	say("smcprobe: running on CPU %d (MPIDR %llx)\n", raw_smp_processor_id(),
		read_cpuid_mpidr());
	if (kinfo) {
		struct arm_smccc_res res;

		say("smcprobe: SMC MC_FC_INFO(MCI_VERSION) ...\n");
		arm_smccc_smc(MC_FC_INFO, MC_EXT_INFO_ID_MCI_VERSION, 0, 0, 0, 0, 0, 0,
			      &res);
		pr_info("smcprobe: MC_FC_INFO -> resp=0x%lx ret=0x%lx state=0x%lx info=0x%lx\n",
			res.a0, res.a1, res.a2, res.a3);
	}
	if (seh) {
		struct arm_smccc_res res;

		say("smcprobe: SMC SET_SEH_ADDRESS ...\n");
		arm_smccc_smc((unsigned long)SMC_CMD_SET_SEH_ADDRESS,
			      (unsigned long)smcprobe_seh, 0, 0, 0, 0, 0, 0, &res);
		pr_info("smcprobe: SET_SEH_ADDRESS -> 0x%lx\n", res.a0);
	}
	if (step < 2)
		return seh ? 0 : -EAGAIN;

	say("smcprobe: SMC READ_CTRL CP_CTRL_NS ...\n");
	ret = smc_read(CP_CTRL_NS, &ns);
	pr_info("smcprobe: SMC CP_CTRL_NS: ret=%d val=%08x\n", ret, ns);

	say("smcprobe: SMC READ_CTRL CP_CTRL_S ...\n");
	ret = smc_read(CP_CTRL_S, &s);
	pr_info("smcprobe: SMC CP_CTRL_S: ret=%d val=%08x\n", ret, s);

	if (write && !ret) {
		long w = smc(WRITE_CTRL, ns, CP_CTRL_NS);

		pr_info("smcprobe: SMC WRITE_CTRL CP_CTRL_NS=%08x -> ret=%ld\n", ns, w);
	}

	if (base)
		iounmap(base);
	/*
	 * Nothing to keep loaded - unless the monitor now points at our
	 * exception handler: then stay resident (no module_exit, so it can
	 * never be unloaded) or EL3 would jump into freed memory.
	 */
	return seh ? 0 : -EAGAIN;
}
module_init(smcprobe_init);

MODULE_DESCRIPTION("Exynos CP secure-control SMC probe");
MODULE_LICENSE("GPL");
