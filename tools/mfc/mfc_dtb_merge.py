# Insert the MFC nodes into a decompiled greatlte dtb (dtc -I dtb -O dts).
# Phandles: 0x19 oscclk, 0x1a cmu_top, 0x01 gic (checked against the dtb).
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
used = set(int(x, 16) for x in re.findall(r"phandle = <(0x[0-9a-f]+)>", s))
PD, CMU, MMU0, MMU1 = 0x1f0, 0x1f1, 0x1f2, 0x1f3
assert not used & {PD, CMU, MMU0, MMU1}
for ph, path in ((0x19, "oscclk"), (0x1a, "clock-controller@15a80000")):
    i = s.index("phandle = <%#04x>;" % ph)
    assert path in s[s.rindex("{", 0, i) - 60:i], path
assert "samsung,exynos8895-mfc" not in s
soc = '''
		clock-controller@13c00000 {
			compatible = "samsung,exynos8895-cmu-mfc";
			reg = <0x13c00000 0x8000>;
			#clock-cells = <0x01>;
			clocks = <0x19 0x1a 0x72>;
			clock-names = "oscclk", "bus";
			power-domains = <PD>;
			phandle = <CMU>;
		};

		iommu@13c80000 {
			compatible = "samsung,exynos8895-sysmmu";
			reg = <0x13c80000 0x3000>;
			interrupts = <0x00 0xc6 0x04>;
			clocks = <CMU 0x05 CMU 0x06>;
			clock-names = "aclk", "pclk";
			power-domains = <PD>;
			#iommu-cells = <0x00>;
			phandle = <MMU0>;
		};

		iommu@13ca0000 {
			compatible = "samsung,exynos8895-sysmmu";
			reg = <0x13ca0000 0x3000>;
			interrupts = <0x00 0xc9 0x04>;
			clocks = <CMU 0x07 CMU 0x08>;
			clock-names = "aclk", "pclk";
			power-domains = <PD>;
			#iommu-cells = <0x00>;
			phandle = <MMU1>;
		};

		codec@13ce0000 {
			compatible = "samsung,exynos8895-mfc";
			reg = <0x13ce0000 0x10000>;
			interrupts = <0x00 0xd0 0x04>;
			clocks = <CMU 0x04>;
			clock-names = "mfc";
			iommus = <MMU0 MMU1>;
			power-domains = <PD>;
		};

		power-domain@16484160 {
			compatible = "samsung,exynos8895-pd";
			reg = <0x16484160 0x20>;
			#power-domain-cells = <0x00>;
			label = "MFC";
			phandle = <PD>;
		};
'''
for k, v in (("PD", PD), ("CMU", CMU), ("MMU0", MMU0), ("MMU1", MMU1)):
    soc = re.sub(r"\b%s\b" % k, "%#x" % v, soc)
m = re.search(r"\n\tsoc@0 \{\n", s)
assert m
end = s.index("\n\t};\n", m.end())
s = s[:end] + "\n" + soc.rstrip("\n") + s[end:]
open(dst, "w").write(s)
print("merged")
