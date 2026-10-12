# Second step after mfc_dtb_merge.py: reserve the IOVA range below the MFC
# firmware pool (s5p-mfc puts it at 0x0e000000) for the codec, so that its
# buffers are allocated above the firmware base. iommu-addresses uses the
# codec's bus cells (soc: 1 address, 1 size), not reserved-memory's.
#   python3 mfc_iova_dt.py merged.dts out.dts
import sys, re
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
MFC, RESV = 0x1f4, 0x1f5
used = set(int(x, 16) for x in re.findall(r"phandle = <(0x[0-9a-f]+)>", s))
assert not used & {MFC, RESV}
old = '''		codec@13ce0000 {
			compatible = "samsung,exynos8895-mfc";'''
new = '''		codec@13ce0000 {
			compatible = "samsung,exynos8895-mfc";
			memory-region = <%#x>;
			phandle = <%#x>;''' % (RESV, MFC)
assert s.count(old) == 1; s = s.replace(old, new)
m = re.search(r"\n\treserved-memory \{\n\t\t#address-cells = <0x02>;\n\t\t#size-cells = <0x01>;\n\t\tranges;\n", s)
assert m
node = '''
		mfc-iova-low {
			iommu-addresses = <%#x 0x1000 0xdfff000>;
			phandle = <%#x>;
		};
''' % (MFC, RESV)
s = s[:m.end()] + node + s[m.end():]
open(dst, "w").write(s)
print("ok")
