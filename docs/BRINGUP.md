================================================================================
README_AGENT.txt -- technical continuation notes for AI agents / developers
Project: postmarketOS port for Samsung Galaxy Note 8 SM-N950F (greatlte)
State date: 2026-10-03 late (all verified working unless marked otherwise)
================================================================================

This file is the dense technical state dump. Read README.txt first for the
human overview. Everything below was learned empirically this session;
verify before relying on it, but do NOT rediscover it.


--------------------------------------------------------------------------------
1. ENVIRONMENT / PATHS
--------------------------------------------------------------------------------
PC (laptop "athul-g15", Ubuntu, systemd-networkd, NO NetworkManager):
  /home/athul/postmarketos/        project dir (this file lives here)
    work/                          boot images, tools, firmware
      boot-v12.img                 last known-good pmOS boot image
      boot-backup-twrp.img         TWRP/LOS boot image
      bnep-up.c / bnep-up          BNEP tool source + static aarch64 binary
      firmware/bcm4361B0_{murata,semco}.hcd          PATCHED
      firmware/*.hcd.orig          unpatched originals
      rootfs-addons/               staged-but-unused rootfs extras
    pmaports/                      aports, branch greatlte-v2; includes
      device-samsung-greatlte, uniloader-samsung-greatlte,
      linux-postmarketos-exynos8895 (+ greatlte-dts.patch)
    uniLoader/                     patched with board-greatlte
    linux-mainline/                sparse clone (v7.3-era master checked out)
  /home/athul/bt-pan-up.sh         MASTER copy of the bring-up script
  /etc/udev/rules.d/99-bnep-mtu.rules  sets MTU 600 on laptop bnep NIC
  PC bluetooth: bluez 5.85, controller hci0 90:E8:68:42:1D:2A, name athul-g15
  PC bridge br0 = 192.168.99.1/24. PC runs a NAP server: bt-network -s nap br0

Phone ("greatlte", postmarketOS/Alpine, OpenRC, kernel 7.0.0 (7.0.0-rc1 until 2026-10-07)
       #4-postmarketos-exynos8895, musl, user athul password 147147):
  /home/athul/bt-pan-up.sh         bring-up script (idempotent, logs to
                                   /home/athul/bt-pan-up.log)
  /usr/local/bin/bnep-up           static aarch64 BNEP client tool
  /lib/firmware/BCM4347B0.hcd      <-- firmware actually flashed at boot
  /lib/firmware/bcm4347b0.hcd      (copy)
  /lib/firmware/brcm/bcm4361B0_{murata,semco}.hcd
  /etc/firmware/{BCM4347B0.hcd, BCM4347B0_semco.hcd, bcm4347b0.hcd}  (copies;
                                   /etc/firmware is hciattach's patch lookup)
  Phone bluez 5.87; openssh 10.5 (binary is /usr/sbin/sshd.pam! not sshd;
                                   found by the init script's update_command;
                                   plain 'find -name sshd' MISSES it)
  sshd enabled in runlevel; ssh key from laptop in authorized_keys
  Phone BT addr (patched firmware): 42:47:B0:00:1F:AC
  Phone br0 = 192.168.99.2/24; phone acts as BNEP PANU client

Phone <-> PC pairing: bonded both directions. Keys in
/var/lib/bluetooth/<bdaddr>/ on each side; bonds are PER-LOCAL-ADDRESS.

SD card workflow: phone off -> card to PC -> mounts at
/run/media/athul/pmOS_root (p2 rootfs; automount may lag a few seconds) ->
edit -> sync -> umount. With the PAN link up, prefer ssh/scp for /home files
(watch out: SD-installed files in /home/athul are root-owned; rm then scp).

Flashing boot image (works from TWRP): adb push img /tmp/ && adb shell
'dd if=/tmp/... of=/dev/block/sda7 bs=4096' && adb reboot. Download mode:
VolDown+Bixby+USB; hard power off: Power+VolDown 10s. heimdall also works.


--------------------------------------------------------------------------------
2. THE NETWORK BRING-UP CHAIN (phone side), as automated by bt-pan-up.sh
--------------------------------------------------------------------------------
Order matters. The script is idempotent and refuses to double-hciattach.

1. Detect hci: parse `hciconfig` output (NOT /sys/class/bluetooth/hci0/address
   -- that sysfs file DOES NOT EXIST on modern kernels, verified on 7.0-rc1
   AND on the laptop's Ubuntu kernel).
2. Only if no hci device exists at all: hciattach /dev/ttySAC0 bcm43xx
   3000000  <-- RUNNING THIS WHEN hci0 ALREADY EXISTS WEDGES THE CHIP
                (BT_EN never cycles); only a reboot recovers. Guarded.
   (Normally unnecessary: boot-time init already ran it, see boot log
   "Flash firmware /lib/firmware/BCM4347B0.hcd ... Device setup complete".)
3. Power up: rfkill unblock bluetooth; hciconfig hci0 up; verify
   "UP RUNNING".
4. rc-service bluetooth start (bluez 5.87, /usr/lib/bluetooth/bluetoothd --
   NOTE: sudo PATH lacks it; use absolute path if debugging:
   sudo /usr/lib/bluetooth/bluetoothd -nd >/tmp/bt.log 2>&1 &)
5. bnep kernel module: /sys/module/bnep exists == loaded-or-builtin (bnep is
   BUILT IN on this kernel; `lsmod|grep bnep` is the wrong check, and
   modprobe of a builtin returns 0 silently).
6. br0: ip link add br0 type bridge; ip addr add 192.168.99.2/24 dev br0;
   ip link set br0 up.
7. Kill stale bnep-up; run /usr/local/bin/bnep-up <PC-bdaddr> hci0 (setsid,
   backgrounded); wait up to 20s for /sys/class/net/bnep* to appear.
8. Configure bnep0: ip link set dev bnep0 address 42:47:B0:00:1F:AC (a NO-OP,
   see 4.3, but harmless); **ip link set dev bnep0 mtu 600** (CRITICAL, see
   4.4); ip link set dev bnep0 up; ip link set dev bnep0 master br0.
9. ping 192.168.99.1 to verify.


--------------------------------------------------------------------------------
3. bnep-up.c -- why it exists and what it does
--------------------------------------------------------------------------------
bluez CAN do PAN client itself (org.bluez.Network1.Connect) but it fails on
this device: profiles/network/bnep.c:bnep_if_up() ->
"bnep: Could not bring up bnep0: Address not available(99)".

Root cause chain (all verified):
3.1 The phone's BD address 43:47:B0:00:1F:AC came from the .hcd firmware.
    Byte 0 = 0x43 -> Ethernet multicast (I/G) bit SET. When bluez converts it
    to the bnep0 MAC, the kernel rejects the interface bring-up
    (EADDRNOTAVAIL). The laptop's 90:E8:... has the bit clear -> works.
3.2 Kernel bnep CANNOT take a different MAC: net/bluetooth/bnep/netdev.c
    bnep_net_set_mac_addr() is a no-op returning 0. `ip link set address`
    "succeeds" and changes nothing. So the MAC must be fixed at the source.
3.3 FIX APPLIED: patched the .hcd firmware. The BD address lives in the
    "CMcfgS" header at file offset 0x21, LSB-first, exactly one occurrence:
        ac 1f 00 b0 47 43  ->  ac 1f 00 b0 47 42
    (43->42 clears I/G, sets U/L = valid locally-administered unicast).
    ALL .hcd copies on the SD were byte-patched (7 files, see paths above).
    Originals kept as *.hcd.orig in work/firmware/. This survives every boot
    (hciattach reflashes the patch each boot). Side effect: phone BD address
    changed -> all bonds invalidated -> had to re-pair once.
3.4 Kernel API change (this kernel is 7.0.0-rc1): the OLD flow
    "ioctl(BNEPCONNADD) on the connected L2CAP socket with
    struct{sock,flags,role,pkt_type,src,dst,device}" is GONE.
    net/bluetooth/l2cap_sock.c l2cap_sock_ops.ioctl = bt_sock_ioctl which
    does NOT handle BNEPCONNADD -> ENOTTY ("Inappropriate ioctl for device").
    NEW flow (net/bluetooth/bnep/sock.c + bnep.h, v7.0-rc1):
      - ctrl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_BNEP /*=4*/)
      - l2   = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP /*=0*/)
      - bind l2 to local hci addr; connect l2 to dst, PSM 0x000F
      - req = struct bnep_connadd_req { int sock; __u32 flags; __u16 role;
              char device[16]; }   // NO src/dst/pkt_type; kernel takes them
              req.sock = l2 fd; role = 0x02 (PANU); flags = 0
      - ioctl(ctrl, BNEPCONNADD, &req)
        where BNEPCONNADD = _IOW('B', 200, int)  // "int", NOT the struct!
      - kernel: sockfd_lookup(req.sock) must be BT_CONNECTED, then
        bnep_add_connection(&req, nsock); req.device filled with "bnep%d".
    3.4b Same class of bug bit us twice: HCIGETDEVINFO = _IOR('H', 211, int)
    (bluez defines these ioctls with `int`; encoding the struct size gives a
    DIFFERENT number -> "Inappropriate ioctl").
    Kernel struct hci_dev_info has __u16 dev_id FIRST (bluez's userspace
    copy omits it): {__u16 dev_id; char name[8]; bdaddr_t bdaddr; __u32
    flags; __u8 type; __u8 features[8]; __u32 pkt_type,link_policy,link_mode;
    __u16 acl_mtu,acl_pkts,sco_mtu,sco_pkts; struct hci_dev_stats stat;}
    Read bdaddr at offset 10. bnep-up.c contains a working implementation.
3.5 After CONNADD, the PANU side must perform the BNEP setup handshake or
    the peer's bluez NAP server times out ("Hangup or error on BNEP socket").
    bnep-up sends on the L2CAP socket:
        01 01 02 11 16 11 15
        (BNEP_CONTROL, SETUP_CONN_REQ, uuid_size=2, dst=NAP 0x1116, src=PANU
        0x1115, big-endian) and reads the SETUP_CONN_RSP (type 0x01 ctrl 0x02,
        resp u16be; 0x0000 = success). Replies "setup rsp: SUCCESS (0x0000)".
    Then it pauses forever holding both fds open (run via setsid + &).
    l2cap sockaddr_l2 layout used: {u16 family; u16 psm; u8 bdaddr[6];
    u16 cid; u8 type; u16 chan} == kernel's (total 16 bytes).
    Tool usage: bnep-up <nap-bdaddr> [hciX]. Static aarch64, glibc.


--------------------------------------------------------------------------------
4. THE MTU PROBLEM (critical, bites anything bulk: ssh, scp)
--------------------------------------------------------------------------------
4.1 Symptom: ssh to phone connects (banner exchange completes, verified via
    sshd -ddd trace reaching "SSH2_MSG_KEXINIT sent") then hangs forever;
    ~120s later "Connection closed by 192.168.99.2" (sshd LoginGraceTime).
4.2 Measured: pings <= 600 bytes payload PASS, 700+ are 100% LOST. The bnep
    netdev gets default MTU 1500; frames beyond the real L2CAP path limit are
    SILENTLY DROPPED at TX (no IP fragmentation is triggered because the
    kernel thinks the interface can take 1500).
4.3 FIX: MTU 600 on BOTH bnep interfaces:
      phone:  ip link set dev bnep0 mtu 600          (now in bt-pan-up.sh)
      laptop: ip link set dev <bnep-nic> mtu 600     (udev rule, see 5)
    After that, 1400-byte pings pass (IP fragmentation into <=600 works) and
    ssh/scp are full speed.


--------------------------------------------------------------------------------
5. LAPTOP (PC) SIDE SETUP -- required once per bluetoothd restart
--------------------------------------------------------------------------------
- br0 must exist with 192.168.99.1/24 and be UP.
- NAP server registration: sudo setsid nohup bt-network -s nap br0 ... &
  (bt-network exits immediately after registering; the registration lives in
  bluetoothd. "AlreadyExists" on re-run = fine.)
- GOTCHA: if a previous registration was made with an EMPTY/broken bridge,
  incoming connections fail with bluetoothd logging
  "bnep_setup() Server error, bridge not initialized" /
  "BNEP server cannot be added". Fix: Unregister then re-register:
    sudo dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0 \
      org.bluez.NetworkServer1.Unregister string:"nap"
    then bt-network -s nap br0 again. "NAP server registered" = good.
- udev RENAMES the laptop's bnep interface: its MAC equals the BT adapter
  address (bluez sets bnep MAC = controller BD addr), so it appears as
  "enx90e868421d2a". Find it with: bridge link show  (look for master br0).
  /etc/udev/rules.d/99-bnep-mtu.rules:
    SUBSYSTEM=="net", ACTION=="add", ATTR{address}=="90:e8:68:42:1d:2a",
    RUN+="/usr/sbin/ip link set dev %k mtu 600"
- PC bluetoothd: bluez 5.85. If "systemctl restart bluetooth" happens:
  controller powers OFF, NAP registration DIES (must re-register), and the
  bluetoothctl screen session (screen -S bt) needs: power on, agent
  NoInputNoOutput, default-agent.
- sdp tooling caveat: `sdptool browse local` returns EMPTY on this system;
  don't trust it. `sdptool browse <remote-bdaddr>` (direct SDP query) works.
- Discovery/EIR: a phone-side NetworkServer1.Register did NOT make the NAP
  UUID visible to the PC via scan+EIR in our tests; we abandoned the
  phone-as-NAP direction entirely. Current working direction: PC = NAP
  server, phone = PANU client via bnep-up. Don't re-litigate without new
  evidence.


--------------------------------------------------------------------------------
6. SSH ON THE PHONE -- gotchas
--------------------------------------------------------------------------------
- sshd binary is /usr/sbin/sshd.pam (OpenSSH_10.5, OpenSSL 3.5.9, PAM build;
  the init script prefers *.pam). `find / -name sshd -type f` does NOT find
  it (exact-name match). service: rc-service sshd {start|stop|status}; it's
  enabled at boot (runlevel).
- Entropy is NOT a problem (entropy_avail 256; sshd banner/conn verified).
- Password auth works (147147); laptop ed25519 pubkey installed ->
  passwordless. ssh client on PC: use sshpass -p 147147 when scripting.
- On the PC, the phone's host key changes when the rootfs is re-imaged; use
  -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null in scripts
  (ksshaskpass may print a harmless warning).


--------------------------------------------------------------------------------
7. BLUETOOTH HARDWARE / FIRMWARE FACTS
--------------------------------------------------------------------------------
- BT chip: Broadcom BCM4347B0 (WiFi BCM4375 is separate, PCIe, unsupported).
- UART: dedicated UART_BT at 10830000 -> /dev/ttySAC0 (serial_1), 3 Mbaud,
  flow control on. NOT usi13 (earlier wrong turn).
- Patch lookup: bluez hciattach scans /etc/firmware RECURSIVELY for a file
  STARTING WITH THE EXACT UPPERCASE chip name "BCM4347B0" ending .hcd.
  "Patch not found for BCM4347B0" = filename problem, not content.
- hciattach: `sudo hciattach /dev/ttySAC0 bcm43xx 3000000` (see 2.2 warning).
- Phone bluez 5.87 bluetoothd: /usr/lib/bluetooth/bluetoothd (sudo PATH does
  not include it). Debug: sudo /usr/lib/bluetooth/bluetoothd -nd >/tmp/bt.log 2>&1 &
  (note trailing &, else it owns your console).
- Pairing dance when needed: BOTH ends `agent NoInputNoOutput` +
  `default-agent`; phone also `discoverable on` + `pairable on`; then from
  the PC `bluetoothctl pair <phone-bdaddr>` after a fresh `scan on` (remove
  stale device first on BOTH sides if the address changed). "Connection
  terminated by remote user" during bnep experiments = the peer bluez
  dropping; check its logs first (journalctl -u bluetooth) before suspecting
  the phone.


--------------------------------------------------------------------------------
8. DEBUG TOOLBOX
--------------------------------------------------------------------------------
- btmon on the PC: sudo btmon -w /tmp/cap.log, then `btmon -r /tmp/cap.log`
  to read. Shows BNEP/L2CAP/SDP packet flows -- how the "Operation
  Successful"/"interface bnep0 added" facts were established.
- phone sshd debug: sudo rc-service sshd stop; sudo /usr/sbin/sshd.pam -ddd
  (one connection, verbose trace on the phone console).
- phone bluez debug: see 7.
- Verify chain: hciconfig | grep 42:47 -> script -> "got interface bnep0" ->
  ping -c2 192.168.99.2 from PC -> ssh athul@192.168.99.2 'echo ok'.
- Size probe for MTU: for s in 600 700 800; do ping -c1 -s $s 192.168.99.2; done


--------------------------------------------------------------------------------
9. BOOT / IMAGE STATE
--------------------------------------------------------------------------------
- work/boot-v45.img = current good pmOS boot image (1.7/1.69GHz cpufreq) (BOOT partition,
  /dev/block/sda7). boot-backup-twrp.img = TWRP/LOS boot for recovery.
- Rootfs: microSD GPT, p1 pmOS_boot (487M), p2 pmOS_root.
- Internal UFS invisible to mainline (parked; needs UFS driver work).
  [Done 2026-10-06, see section 16.]
- Kernel source: pmaports linux-postmarketos-exynos8895 (branch greatlte-v2),
  mainline 7.0 base (7.0-rc1 until 2026-10-07). DTS patch for greatlte in the same branch.
  Local linux-mainline/ is a sparse clone for header reference; to compile
  the greatlte DTB from it:
    cpp -nostdinc -I arch/arm64/boot/dts/exynos -I include \
        -I arch/arm64/boot/dts -I /tmp/dtsinc -undef -x assembler-with-cpp \
        <dts> | dtc -I dts -O dtb -i arch/arm64/boot/dts/exynos -i include -


--------------------------------------------------------------------------------
10. WIFI (BCM4361 over PCIe) -- FULLY WORKING, auto at boot, 2026-10-03
--------------------------------------------------------------------------------
The "BCM4375" is really a Broadcom BCM4361B0 (PCI device ID 0x14e4:0x441f,
chipid 0x4361 rev 3; downstream Samsung driver dir is bcmdhd4361). Firmware
13.38.64 (B0, Network/rsdb) loads via brcmfmac. End-to-end automatic at boot:
PCIe link -> BAR assignment -> brcmfmac probe -> firmware -> wlan0 ->
NetworkManager auto-connects to "The Starry Night 2.4" (WPA2, 2.4 GHz).
5 GHz / WPA3 SSID "The Starry Night" does NOT associate (driver/firmware
limitation, not investigated). First wifi startup costs ~2 min of firmware
loader timeouts on the optional txcap_blob (missing); harmless.

Kernel/git state:
- Scratch tree: /tmp/kernel-patchwork (git repo, base b6a8f5cf5; NOTE /tmp may
  be wiped -- the full work is committed and regenerated into the pmaports
  patch, see "Build / patch regen loop" below).
- pmaports linux-postmarketos-exynos8895: pkgrel=13, patch
  exynos8895-pcie-wifi.patch (everything except the two greatlte dts files).
- CONFIG_PCI_EXYNOS=m (module iteration without reflash).

PCIe root complex bring-up (all in drivers/pci/controller/dwc/pci-exynos.c):
- 8895 needs ALL fsys1 CMU gate clocks enabled: loop of_clk_get_from_provider
  over cmu_fsys1 ("samsung,exynos8895-cmu-fsys1") ids 5..44. The 7 dts clocks
  alone hang the bus.
- Do NOT touch ELBI 0x288/0x28c/0x290 (bogus reset regs from older exynos
  code); writing 0x290 hard-hangs the SoC. Downstream never touches them.
- Order: phy_init() FIRST (releases PMU isolation at pmu 0x1648071c bit0),
  then ELBI writes, then LTSSM.
- Power/link sequence (downstream-faithful): PERST(bit2 gpj1, active-low)+
  WLAN_EN(bit3 gpj1) assert -> mdelay(100) -> sysreg 0x11421044 bit1 clear;
  sysreg+0xc lane ctrl (clear 0xf<<4,0xf<<2, set 0x3<<2, clear bit1); PMU
  0x1648071c|=1; PHY cmn/trsv cal tables (pcie-8895-tables.h) + PCS regs;
  ELBI soft core reset 0x1d0 (0,udelay,1); ELBI glue: 0xf4|=0x11,
  0x1b8=0x2, 0x2c8&=~0xfff, 0x38=1, 0x274|=1, 0x00c=0xf; start_link: PERST
  release, 0x2c=1, poll ELBI 0x74 &0x1f for 0x0d-0x14, PERST bounce x10.
- DWC resets root port MEM/LIMIT=0 -> bogus 1MB window at PCI addr 0 that
  the PCI core won't grow. Fixed by disabling it in start_link
  (MEM_BASE=0xfff0/LIMIT=0, PREF same, UPPER32=0) wrapped in
  dw_pcie_dbi_ro_wr_en/dis (outside host_init the DBI is locked and writes
  are silently dropped).

iATU (THE key discovery -- BAR reads external-aborted until this was fixed):
- The 8895 iATU registers are INLINE in DBI config space, not at
  dbi+DEFAULT_DBI_ATU_OFFSET(+0x300000): viewport dbi+0x900, CR1 0x904,
  CR2 0x908, LOWER_BASE 0x90c, UPPER_BASE 0x910, LIMIT 0x914,
  LOWER_TARGET 0x918, UPPER_TARGET 0x91c. Matches downstream pci-exynos.h.
- Fix: pci->atu_base = pci->dbi_base + 0x900 + 4 in host_init so the dw
  core's generic offsets land right. dmesg then shows
  "iATU: unroll F, 3 ob, 5 ib" (3 outbound windows, viewport-based).
- The dw core only programs outbound MEM windows (dw_pcie_iatu_setup) when
  child_ops == dw_child_pcie_ops or ecam. Our driver installs its own
  child ops, so MEM windows are programmed manually in start_link (after
  link up) with raw exynos_pcie_writel to dbi+0x900.. .

SOC ADDRESS ROUTING (second key discovery):
- The SoC only routes CPU physical 0x11800000-0x11c00000 (4 MiB) to the
  PCIe AXI slave. Proven: with an iATU window mapping CPU 0x11b00000 ->
  PCI 0x11c00000, `poke 0x11b00000` returned 0xb83ef000 (BAR2 content),
  while direct pokes at 0x11c00000/0x12000000 Bus-error even with a
  covering iATU window and MEM decode on.
- poke tool: reads/writes u32 via /dev/mem (laptop: /tmp/poke/poke, also on
  phone /tmp/poke). Userspace reads of unrouted/gated regions -> SIGBUS Bus
  error; writes to a wedged block hang the phone. NEVER poke the iATU
  viewport register (0x11700900) from userspace with junk -- wedged twice.

BAR/APERTURE LAYOUT (final, working):
- dts (exynos8895.dtsi pcie node): ONE range
  <0x82000000 0 0x11800000 0x11800000 0 0x500000> (5 MB; the PCI core
  rounds the 4MB+32KB need UP to 5MB and aligns to 4MB). CPU can't route
  the top 1MB -- the PCI core doesn't know and doesn't care.
- The dw core reserves the config aperture (pp->cfg0_base 0x11bff000,
  4KB) via devm_pci_remap_cfg_resource. That reservation punches a hole in
  the window -> "bridge window [mem size 0x500000]: can't assign; no
  space" and NO BARs get assigned. Fix in exynos8895_pcie_host_init (runs
  after the reservation, before pci_host_probe):
  release_mem_region(pp->cfg0_base, pp->cfg0_size). Config TLPs still work
  (iATU outbound idx0 at 0x11bff000 handles them); the devm double-release
  at remove only warns.
- Result: BAR2 (4MB) assigned 0x11800000-0x11bfffff; BAR0 (32KB) stays
  UNASSIGNED (BAR2 eats the space in practice) -> handled in brcmfmac.
- Manual outbound windows programmed in start_link:
    idx1: CPU 0x11800000-0x11afffff -> PCI 0x11800000  (BAR2 low 3MB;
          chip RAM/rings live below 3MB -- verified working; host CANNOT
          reach BAR2 offsets 0x300000+ and there is no room to map more)
    idx2: CPU 0x11b00000-0x11b07fff -> PCI 0x11c00000  (BAR0 alias)
    idx0: config at 0x11bff000 (dw core, automatic)

brcmfmac patches (drivers/net/wireless/broadcom/brcm80211/):
- brcm_hw_ids.h: BRCM_PCIE_4375_RAW_DEVICE_ID 0x441f (module alias);
  BRCM_CC_4361_CHIP_ID 0x4361.
- pcie.c: BRCMF_FW_CLM_DEF(4361,"brcmfmac4361-pcie") + FW_ENTRY for 4361;
  in brcmf_pcie_get_resource():
    bar0_addr==0        -> pci_write BAR0=0x11c00004/upper0 (chip decodes
                           BAR0 at 0x11c00000), regs ioremapped at alias
                           CPU 0x11b00000
    bar0_addr>=0x11c00000 -> alias only (BAR already assigned)
    bar1_addr==0        -> 0x11800000/0x400000 fallback
- chip.c: brcmf_chip_tcm_rambase: case BRCM_CC_4361 -> 0x170000
  (downstream: 4361 grouped with 4347/4357 = CR4_4347_RAM_BASE).

Firmware on phone (/lib/firmware/brcm/, persists on SD rootfs):
- brcmfmac4361-pcie.bin == work/firmware/bcmdhd_sta.bin_b0 (sha-identical;
  genuine 4361b0 fw despite "4375" naming confusion earlier).
- brcmfmac4361-pcie.txt = work/firmware/nvram.txt_murata_r033_b0 (board
  NVRAM; the generic linux-firmware 4375 nvram is WRONG for this board).
- .clm_blob = bcmdhd_clm.blob. Also copies under
  brcmfmac4361-pcie.samsung,greatlte.* (skip a firmware-path fallback hop).
- Do NOT create EMPTY txcap_blob files: zero-length firmware = -22 EINVAL,
  still slow. Missing = graceful "no txcap_blob available" after the
  fallback timeout (~60s each for board+base name at first boot).

Rootfs module install (no pmbootstrap rebuild needed):
- Modules are .ko.zst under /lib/modules/7.0.0/kernel/... Build with
  make LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION= in the scratch
  tree, zstd -f on the laptop, scp, sudo cp over the old .zst,
  depmod -a 7.0.0. modules.alias must contain
  "of:N*T*Csamsung,exynos8895-pcie pci_exynos" and
  "pci:v000014E4d0000441Fsv*sd*bc02sc80i* brcmfmac" (depmod does this).

Build / patch regen loop (fast path, driver-only changes):
  cd /tmp/kernel-patchwork
  make ARCH=arm64 LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION= -j4 \
       M=drivers/pci/controller/dwc modules        # or M=.../brcm80211
  scp .../pci-exynos.ko athul@<ip>:/tmp/
  phone: rmmod pci_exynos brcmfmac brcmutil; insmod /tmp/pci-exynos.ko; ...
Boot image (dtb/kernel change): build Image dtbs modules, cp to
uniLoader/blob/{Image,dtb}, make -s ARCH=aarch64 CROSS_COMPILE=... in
uniLoader, cp uniLoader/uniLoader work/ulpkg/boot/bootshim (DON'T forget
this cp -- a stale bootshim cost one flash cycle), then mkbootimg
--header_version 1 --kernel work/ulpkg/boot/bootshim --ramdisk
work/empty_ramdisk --pagesize 2048 --base 0 --kernel_offset 0x10008000
--ramdisk_offset 0x11000000 --second_offset 0x10f00000 --tags_offset
0x10000100 -o work/boot-vNN.img. Regenerate pmaports patch:
  git diff b6a8f5cf5 HEAD -- . ':(exclude)arch/arm64/boot/dts/exynos/Makefile'
      ':(exclude)arch/arm64/boot/dts/exynos/exynos8895-greatlte.dts' \
      > pmaports/.../exynos8895-pcie-wifi.patch
then update its sha512 + pkgrel in the APKBUILD.

Debug toolbox for wifi:
- Boot check: dmesg | grep -iE "ATU ob|BAR .mem|Firmware:|MMIO"
- BAR/cmd state: cat /sys/bus/pci/devices/0000:01:00.0/resource;
  dd if=.../config bs=1 skip=4 count=2 (cmd reg must have bit1 MEM for
  BAR reads to answer; failed assignment strips IORESOURCE_MEM so
  pci_enable_device leaves MEM off -> 0xffffffff reads / probe -19).
- Manual BAR poke (pre-auto-assign debugging): write 0x11c00004 at config
  off 0x10 (BAR0), 0x11800004 at off 0x18 (BAR2), 0x0006 at off 4 (cmd)
  via dd conv=notrunc on the sysfs config file.
- Phone wifi ssh: athul@192.168.1.187 (DHCP; find in router), or BT PAN
  fallback athul@192.168.99.2 after sudo sh /home/athul/bt-pan-up.sh.
- Phone-side power_save: `iw` not installed on phone; NM manages it.

Laptop-side quirk found while testing: laptop wifi powersave made phone->
laptop replies take ~2.5s (requests arrived fine). `iw dev wlp4s0 set
power_save off` on the laptop. Laptop->phone direction was always fine.


--------------------------------------------------------------------------------
12. DESKTOP / PLASMA MOBILE / TOUCHSCREEN (2026-10-03, working)
--------------------------------------------------------------------------------
- Graphics: simpledrm on the bootloader framebuffer (0xcc000000) -> /dev/dri/card0,
  atomic modesetting, llvmpipe GL. NO GPU acceleration. Weston 16 and
  Plasma Mobile both run on it directly. DRM master needs a seat manager:
  seatd (apk add seatd; rc-service seatd start; user in seat group).
  Weston launch (as user athul, uid 10000, XDG_RUNTIME_DIR=/run/user/1000):
    openvt -c 2 -s -- sudo -u athul XDG_RUNTIME_DIR=/run/user/1000 \
        weston --backend=drm-backend.so
  Subpackages needed: weston-backend-drm, weston-shell-desktop.
- Touchscreen: samsung,s6sy761 @ i2c 0x48 (USI hsi2c), IRQ gpa1 0, mainline
  driver drivers/input/touchscreen/s6sy761.c. Node was ALREADY in the dts;
  the driver probes a bit late (deferred). Works in weston + Plasma.
  NOTE: at low CPU clocks (455MHz A53) taps register multiple times - keep
  the performance governor if touch accuracy matters.
- Plasma Mobile: apk add postmarketos-ui-plasma-mobile (tinydm autologin
  uid 10000 = athul; session /usr/share/wayland-sessions/plasma-mobile.desktop).
  Toggle console/Plasma boot by adding/removing /etc/runlevels/default/tinydm
  on the SD rootfs.
- S Pen is wacom,w90xx @ i2c 0x56 (no mainline driver). sec_ts main touch
  is the s6sy761 (NOT synaptics rmi4 as guessed early on).
- 2GB /swapfile added on SD (fstab) - needed for big apk transactions.

--------------------------------------------------------------------------------
13. CPUFREQ (2026-10-03, partial - little cluster only)
--------------------------------------------------------------------------------
- Hardware: CMU_CPUCL0 @ 0x16800000 (A53 little), CMU_CPUCL1 @ 0x16900000
  (M2 big), size 0x8000. Register maps from Samsung CAL data (sparse clone
  /tmp/universal8895-src, drivers/soc/samsung/cal-if/exynos8895/cmucal-sfr.c):
  PLL_LOCKTIME 0x0, PLL_CON0 0x120, MUX_SWITCH_USER 0x100 (+PLL_CON2 0x108),
  MUX_CLK_CPUCLx_PLL 0x1000, DIV_CLK_CLUSTERx_ACLK 0x1800/ATCLK 0x1804,
  DIV_CLK_CPUCLx_CPU 0x180c (CL0) / 0x1814 (CL1), gate GATE_CLK_CPUCLx_CPU
  0x2020 (CL0) / 0x2018 (CL1).
- Driver: clk-exynos8895.c CMU_CPUCL0/1 sections modeled on the in-tree
  clk-exynos850.c template (new CPUCLK_LAYOUT_E8895_CL0/CL1 in clk-cpu.c).
  PLLs: pll_1051x for both (CAL says CPUCL0=1050X, CPUCL1=1051X - same
  family/layout, mainline has no 1050x). Rate tables computed as
  26MHz*M/(P*2^S), P=3 (full tables for ALL downstream OPPs are in the
  driver; raising caps later is dts-only).
- Downstream OPP lists (kHz) - from universal8895 dts cpufreq-domain nodes:
  A53: 2002000 1898000 1794000 1690000 1456000 1248000 1053000 949000 832000
       715000 598000 455000
  M2:  2808000 2704000 2652000 2574000 2496000 2314000 2158000 2002000
       1937000 1807000 1703000 1469000 1261000 1170000 1066000 962000 858000
       741000
- STATUS (2026-10-03 late, VERIFIED): full chain works - both clusters
  scale with VOLTAGE coupling via S2MPS17 over a ported SPEEDY bus driver.
  FINAL SHIPPING CONFIG (2026-10-03): big (M2) 455MHz-1.703GHz, little (A53)
  455MHz-1.690GHz (stock). [Superseded: big now to 2.314 GHz with TMU
  throttling, section 19.] 100s all-core stress PASSED at 1.703/1.690.
  Boot governor = userspace (1.066GHz); /etc/local.d/cpuspeed.start on the
  phone switches to performance after boot (booting AT >1.066 races udev
  module loading -> NULL deref oops at ~3.2s).
- THE SWITCH HANG FIX: exynos8895_cpuclk_pre_rate_change must force
  PLL_CON0_MUX_CLKCMU_CPUCLx_SWITCH_USER (base+0x100) bit4=1 (select the
  CMU_TOP switch clock as alternate) BEFORE the relock dance - the
  bootloader leaves CPUCL1's on OSCCLK (26MHz) and switching the M2 cores
  to it mid-relock hangs the SoC. (clk-cpu.c, E8895 layouts)
- FREQUENCY CEILING: >1.7GHz hangs/crashes REGARDLESS of voltage (tested
  2158@1.1V and 1.125V instant hang idle; 2002 idle-OK but crashes under
  sustained all-core load even at 1.2V = Samsung's max rail voltage).
  Root cause is NOT voltage - prime suspect is EMA (SRAM timing) which
  downstream reprograms per voltage (S5E8890: MNGS_EMA_CON = sysreg
  0x11850000+0x314: >=1.106V -> 0xE91B9, >=0.9V -> 0x1091B9, else 0x1095B9;
  8895 ACPM firmware does it invisibly). Address unverified for 8895 - a
  memremap-dump of 0x11800000-0x11900000 to find the cluster sysreg is the
  next step before poking. Bus-divider theory RULED OUT: ATCLK div is
  constant 3 (=core/4) at all rates per cmucal-vclklut.c.
- PMIC (S2MPS17) + SPEEDY: all in-tree. BUCK2=vdd_cpucl0->A53 little
  (cpu@100-103), BUCK3=vdd_cpucl1->M2 big (cpu@0-3). BUCKs 0.3V + 6.25mV
  steps, Samsung caps 1.2V. opp-microvolt MUST be exact multiples of 6250
  or _opp_supported_by_regulators rejects the OPP.
- SPEEDY driver gotcha (cost 3 rounds): the transaction is ONE fused
  command - CMD reg packs ACCESS_RANDOM|DIRECTION|DEVICE(dev)<<15|
  ADDRESS(reg)<<7, LENGTHS go in FIFO_CTRL (RX[4:0], TX[12:8]), writes do
  CMD-then-TX_DATA, reads grant NO credits, completion = INT_STATUS bit0
  TRANSFER_DONE (W1C, clear before+after). The downstream i2c-speedy.c xfer
  path is DEAD CODE (return 0) - the reference is the exynos-speedy mainline
  patch by Markuss Broks (linux-arm-kernel, Dec 2024). A malformed transfer
  wedges the FSM so hard even controller register reads stall the CPU bus.
- ECT (ASV tables) extracted from the phone: BL2 leaves it at PA 0xA0000000
  (NOT 0x90000000 like the 850), signature "PARA". Dumped via a memremap
  module (ioremap refuses RAM; /dev/mem blocked by STRICT_DEVMEM). Parser
  format per downstream ect_parser.c. Full M2/A53 OPP voltage tables parsed
  (worst-bin table0 group2/3 values + margin used for the dts).

--------------------------------------------------------------------------------
14. SUSTAINED WIFI DOWNLOAD CRASH (unfixed, reproducible 4/4)
--------------------------------------------------------------------------------
- Symptom: 5-10 min of sustained full-speed WiFi RX (apk downloads, big
  transfers) -> INSTANT reboot, no pstore/ramoops panic, no logs.
  Not memory (1GB+ free at crash, 2GB swap present), not thermal-proven.
  Suspect brcmfmac/firmware wedge. Chunked installs + rests did NOT help;
  only avoiding sustained RX helps.
- WORKAROUND that worked: generate exact package URLs on the phone
  (apk fetch --url <pkgs> > urls.txt), wget them on the laptop, scp to the
  phone /var/cache/apk in ~20-file batches with rests, then
  apk add postmarketos-ui-plasma-mobile fully offline from cache.
- Note: the USB gadget is on dummy_udc (VIRTUAL loopback!) - usb0 cannot
  carry real laptop traffic until dwc3 is brought up. Don't waste time on
  USB networking like I did.

--------------------------------------------------------------------------------
15. NATIVE DISPLAY (DECON + DSI + S6E3HA6), GPU CLOCK, CPU CLUSTERS (2026-10-05/06)
--------------------------------------------------------------------------------
Pipeline: DECON_f -> dual DSC encoders (2 slices of 720x40, 8 bpp) -> DSIM0
(4 lanes, 898 Mbps, command mode, TE-triggered) -> S6E3HA6 AMOLED (a2 panel,
ID 817143). Drivers: drivers/gpu/drm/bridge/exynos8895-{decon,dsim}.c,
drivers/gpu/drm/panel/panel-samsung-s6e3ha6.c, S2DOS03 panel PMIC regulator.
All in aports/linux-postmarketos-exynos8895/exynos8895-display.patch.

15.1 Root causes, in the order they were found
  - fbdev oops (missing drm_mode_config_reset) and fbcon freezing the first
    commit under console_lock -> DRM_FBDEV_EMULATION is OFF (see 15.3).
  - Stale bootloader DSIM/DECON interrupts -> "nobody cared" IRQ storms and
    flaky panel ID reads: mask+clear everything before request_irq.
  - IDMA fetch registers live in the DMA bank 0x128B1000, not the DPP bank
    0x12851000; the scan-out kept fetching the bootloader framebuffer.
  - THE NOISE BUG: DSC encoder SFR +0x58 holds PPS bytes 56..59; writing only
    58/59 zeroed rc_buf_thresh[12..13] (0x7d,0x7e) so the encoder rate
    control did not match the panel decoder -> structured noise. Found by
    booting with decon/dsim disabled (clk_ignore_unused) and diffing against
    the bootloader-programmed registers (tools/regdump.c).
  - DSIM: support MIPI compression-mode packets (type 0x07, downstream
    DSI_PKT_TYPE_COMP; the panel does NOT use DCS 0x9D), clear the
    PER_FRAME_READ_EN reset default, program SLICE23.
  - PRIME export used virt_to_page() on a write-combine remap -> bogus phys
    address -> every panfrost import bounced via swiotlb. Now dma_get_sgtable().
  - The DRM driver is named "exynos" so Mesa's kmsro pairs it with panfrost
    (GPU-composited Plasma, kmscube 60 fps).
  - Real vblank: the HW trigger stays armed (panel refresh 59 fps measured via
    DECON FRAME_ID @0x128602a0), flip events complete at frame start.
  - CMA 384M@0x80000000-0xbc000000 (KWin swapchain exhausted 128M; must stay
    below 4G for the 32-bit panfrost mask).

15.2 GPU clock (it ran at 26 MHz the whole time)
  panfrost fdinfo drm-cycles / drm-engine ns gave exactly 26.0 MHz: the GPU
  was on the oscillator. The real G3D PLL is CMU_G3D+0x140 (pmucal map), not
  0x120 (cmucal map, reads 0). The g3d bring-up (rootfs-addons/etc-init.d/g3d)
  runs debugfs steps 1 2 3 6 7 5: pmucal g3d_on (locks the PLL at 260 MHz),
  TOP switch as bridge clock, gates, relock to 546 MHz (stock gpu_max_clock),
  busd mux -> PLL. 546 MHz at 800 mV gave DATA_INVALID_FAULT GPU job faults;
  the phone's ECT (dumped from RAM at 0xA0000000, parsed with
  tools/ect_parse.py) asks for up to 768 mV + stock's 37.5 mV margin, so the
  OPP is now 812.5 mV. NEVER let CCF reprogram this PLL at runtime: a
  set_rate-capable PLL + a 546 MHz OPP froze the phone at boot.

15.3 CPU clusters were swapped (cause of the "freezes under load")
  CMU_CPUCL0 (0x16800000) + vdd_cpucl0/BUCK2 = Mongoose big cluster,
  CMU_CPUCL1 (0x16900000) + vdd_cpucl1/BUCK3 = A53 little cluster (downstream
  cmucal vdd_mngs -> PLL_CPUCL0; ECT PLL_CPUCL0 = 741..2808 MHz). The DT and
  PLL tables had them swapped, so the A53s ran the big table: 1703 MHz at
  1025 mV where the ECT asks for up to 1200 mV. Proven by lowering each
  cpufreq policy and timing a busy loop on each core type. Fixed DT, PLL
  tables now verbatim from the ECT, A53 top OPPs at the ECT worst case.
  (The old "big CPU above 1.7 GHz hangs" note was most likely this.)

15.4 Other findings
  - Touch (Samsung Y661, s6sy761 protocol): the first queued event is not
    always boot-complete; the driver now polls like the vendor driver.
  - work/reboot-download had no sync(): every reboot-to-download lost
    unflushed writes (0-byte /etc/xdg autostart files -> plasmashell never
    started, truncated busybox). tools/reboot-download.c has the fix.
  - microSD UHS: no vqmmc (S2MPS17 LDO2) is described, so UHS SDR104/SDR50
    signalling is not switched to 1.8 V -> command timeouts + I/O errors under
    sustained reads. SDR50 is the current setting; dropping UHS entirely
    broke boot (fixed sampling timing). Proper fix: add LDO2 + per-mode
    timings. The card itself reads clean. [2026-10-07: this was also blamed
    for the browser freezes, but those happen from UFS too - section 17.]
  - Known limits: fbdev/fbcon still crashes early boot when enabled; no
    brightness control yet (no DCS 0x51 on this panel; AOR 0xB1 + 0xF7 latch
    dims but must be sent between frames); Plasma ~24-30 fps.
  - UFS (internal storage): done, see section 16.

--------------------------------------------------------------------------------
16. INTERNAL UFS STORAGE (2026-10-06, working; rootfs moved off the microSD)
--------------------------------------------------------------------------------
Device: Toshiba THGAF4G9N4LBAIRA (64 GB, UFS 2.1, 4 KiB logical blocks).
Driver: mainline drivers/ufs/host/ufs-exynos.c + an "samsung,exynos8895-ufs"
variant, PHY stub drivers/phy/samsung/phy-exynos8895-ufs.c. All in
aports/linux-postmarketos-exynos8895/exynos8895-ufs.patch;
CONFIG_SCSI_UFS_EXYNOS=y, CONFIG_PHY_SAMSUNG_UFS=y (built in: root is on it).
Result: HS-G3 rate B x2 lanes, ~600 MB/s reads; sda1-21 (BOTA0 .. USERDATA),
sdb-sde boot/RPMB LUNs.

16.1 What it took, in the order the failures appeared
  - HCI registers read 0: the FSYS0 bus gates (AHBBR hclk, RSTNSYNC, PMU_FSYS0
    pclk, BTM aclk/pclk) must be clocked. Do NOT list the XIU clocks: when a
    failed probe turned them off, the next access hung the SoC.
  - The secure UFS protector (UFSP/FMP) is set up by the bootloader and is
    secure-only; the FMP SMC never returns, so the variant skips it
    (EXYNOS_UFS_OPT_SKIP_UFSP_SETUP).
  - M-PHY tuning: the downstream DT tables (phy-init, post-phy-init,
    calib-of-pwm, calib-of-hs-rate-a/b) are ported verbatim and replayed by a
    small interpreter (exynos8895_ufs_config). PMA is byte-addressed with a
    0x140 per-lane stride; PCS lane selectors are TX 0 / RX 4. Also: PMU 0x724
    bit0 (PHY isolation off), sysreg FSYS0 0x1150 bit0 (TCXO select),
    HCI 0x108 bit0 (MPHY refclk select).
  - Link start-up timed out until HW auto clock gating was disabled
    (HCI_UFS_ACG_DISABLE 0xFC bit0), as downstream does.
  - NOP OUT failed with the node marked dma-coherent: the controller is NOT
    coherent on this SoC.
  - Power mode change "failed" with UPMCRS 0: this UniPro reports the result
    in its own registers (0x78EC / 0x7868 / 0x7878), not in the HCI. Added an
    optional get_upmcrs vop to the UFS core (EXYNOS_UFS_OPT_UNIPRO_DIRECT_RESULT).
  - pclk: no divider on chip rev 0004 (downstream does the same).
  - VCC: regulator-fixed on gpg0-0, always-on. (The gpg0 DAT register is
    0x109801A4 - PERIC1 bank table, gpg0 at +0x1a0 - not 0x10980144.)
  - Remaining harmless noise: dme-set 0x9540 and 0x321 fail (attributes this
    UniPro does not implement).

16.2 Moving the rootfs
  The phone could not copy its own rootfs: a long sequential read from the
  microSD triggered the SD freeze (15.4). Instead, on the laptop: the SD root
  partition was copied into an 8 GB ext4 image (fstab rewritten to the UFS
  UUIDs) and the boot partition into an ext2 image, both written from TWRP
  with `adb exec-in "dd of=/dev/block/sdaNN"` (~6.5 MB/s) and checked by md5
  over the full length, then resize2fs on first boot.
  GOTCHA: the first boot failed with "EXT4-fs (sda16): bad block size 1024":
  the SD boot partition has 1 KiB blocks, below the UFS 4 KiB sector size.
  Recreated it with mkfs.ext2 -b 4096. pstore (pmsg-ramoops-0, readable from
  TWRP) held the initramfs log that showed this.
  Selection: pmos_boot_uuid= / pmos_root_uuid= in the dts bootargs (the
  initramfs otherwise picks partitions by the pmOS_boot/pmOS_root labels,
  which the microSD also carries). Labels on UFS: pmOS_ufs_boot/pmOS_ufs_root.
  No RTC driver: the clock starts at 1970 and chronyd (started before WiFi)
  never syncs -> NetworkManager dispatcher hook restarts chronyd on connect.

17. BROWSER START-UP FREEZE (2026-10-07, SOLVED: firmware-owned RAM)
--------------------------------------------------------------------------------
Symptom: opening Firefox or Angelfish hard-froze the phone 5-15 s after the
window appeared, before any page loaded. No panic, no soft/hard-lockup
report (despite softlockup_panic=1 hung_task_panic=1 panic=5), WiFi drops,
the kernel log just stops. ramoops is empty after the forced reboot.

Root cause: the greatlte DT handed firmware-owned DRAM to Linux as ordinary
RAM. The stock DT (exynos8895-greatlte-rmem.dtsi) carves out several regions
- most importantly `/memreserve/ 0xE0000000 0x1900000` (25 MB, the secure
world's memory) - and our memory nodes covered all of 0xC0000000-0xFFFFFFFF
with nothing reserved. Once the page allocator hands out one of those pages
and the CPU touches it, the interconnect locks up: every core stops at once,
so not even the oops/lockup machinery gets to print. Browsers trigger it
because they allocate and fill hundreds of MB within seconds of starting
(other apps and stress tests never pulled pages from that range).
Fix (greatlte-reserved-memory.patch): no-map reservations for dram_test
(0x80002000), seclog (0xC0000000), secure_camera (0xD0000000), the secure
/memreserve/ (0xE0000000), abox (0xEA800000), modem_if (0xF4C00000),
cp_ram_logging (0xFDC00000) and gnss_if (0xFFC00000). TIMA/RKP at
0xB1000000 is deliberately NOT reserved: RKP is only armed by the Samsung
kernel, and reserving it splits the 384M below-4G CMA window the display
needs (cma: "Failed to reserve 384 MiB" -> KWin cannot allocate scan-out
buffers -> black screen). Cost: ~230 MB of RAM (MemTotal 3.71 GB).

How it was found (the dead ends are worth keeping):
  1. Ruled out, one test each: microSD (froze from UFS too), GPU clock (260
     vs 546 MHz), the GPU entirely (llvmpipe, X11), CPU/SIMD/memory stress,
     OOM (1.2 GB free at the freeze), user+net namespace churn, reading all
     of /sys, DRM probing, headless Firefox, the OSK, the network.
  2. Kernel 7.0-rc1 -> 7.0 final (all patches apply unchanged): still froze.
  3. 7.0's new panfrost Transparent Hugepage GEM mount (on by default;
     645 MB ShmemHugePages at idle): panfrost.transparent_hugepage=0 still
     froze.
  4. strace streamed over ssh to the laptop (a log on the phone's own disk
     never survives): the last event was Firefox's "URL Classifier" thread
     `+++ killed by SIGSEGV +++` *inside* a read() into a freshly mmap'ed
     2 MB buffer, with no SIGSEGV delivered from user space - i.e. the
     kernel died while touching the destination page. Re-reading the same
     file with caches dropped was fine, so it was the page, not the file.
  5. dmesg -w streamed over ssh: nothing at all before the stop - consistent
     with a bus lock-up rather than a software crash.
  6. /proc/iomem vs the stock rmem dtsi -> the unreserved carveouts.
The earlier "microSD read freezes" were very likely the same thing.

Debug technique worth reusing: for hard freezes, stream the evidence off the
phone while it happens (`ssh phone 'strace -f -tt ... firefox' > laptop.log`,
`ssh phone 'sudo dmesg -w' > laptop.log`) - anything written locally is lost
with the unsynced page cache.

--------------------------------------------------------------------------------
18. RELEASE IMAGE (2026-10-07: kernel 7.0, prebuilt microSD image)
--------------------------------------------------------------------------------
- Kernel moved from 7.0-rc1 to 7.0 final; every patch applied unchanged.
  The release boot.img is built from the pmbootstrap package's vmlinuz + dtb
  (so its modules match the image's /lib/modules/7.0.0). Its bootargs carry
  no pmos_*_uuid, so the initramfs finds pmOS_boot/pmOS_root by label (if a
  UUID is given and missing, find_partition does NOT fall back to labels).
- pmbootstrap: postmarketos-ui-plasma-mobile has pmb:support-systemd only,
  so `ui=plasma-mobile` forces systemd. Use ui=console + service_manager=
  openrc and --add postmarketos-ui-plasma-mobile; its -openrc subpackage's
  post-install enables tinydm and sets the plasma-mobile session.
- "Not authorized to control networking" in Plasma: polkit came as
  polkit + polkit-noelogind-libs, so polkitd cannot see the elogind
  session as active and NetworkManager's plugdev rule (subject.active)
  never matches. Fix: `apk add polkit-elogind '!polkit-noelogind-libs'`.
  After the swap the old polkitd (D-Bus activated, not under OpenRC) keeps
  running until killed - restart it, then pkcheck against plasmashell's
  pid says yes.
- Bluetooth needs bluez-deprecated (hciattach) and the patchram at
  /etc/firmware/BCM4347B0.hcd (= bcm4361B0_semco.hcd; the chip reports its
  ROM as BCM4347B0). Re-running hciattach on an already-initialised chip
  times out (it is left at 3 Mbaud) - only the first start after boot works.
- Root partition growth on first boot needs `pmos.force-partition-resize`
  on the cmdline (pmbootstrap usually puts it in its own boot.img; ours is
  uniLoader, so it lives in the dtb bootargs - greatlte-grow-rootfs.patch).
  With a UUID root on UFS (/dev/sda21) it is a no-op.
- WiFi took ~2 min to come up on every boot: brcmfmac requests an optional
  txcap_blob under two names, neither exists for the BCM4361, and the legacy
  firmware sysfs fallback made each request wait 60 s for a userspace
  helper pmOS doesn't run. `sysctl.kernel.firmware_config.ignore_sysfs_fallback=1`
  in bootargs (greatlte-no-fw-fallback.patch): firmware loaded at 5.9 s,
  SSH reachable at ~28 s uptime.
- The label-based release boot.img with no microSD inserted (and no other
  pmOS_boot/pmOS_root) hangs in the initramfs waiting for the partitions:
  black screen after uniLoader, no WiFi. Not a kernel problem.
- tools/install-rootfs-addons.sh does the rest (g3d, local.d cpuspeed,
  hciattach + udev rule, firmware names, chrony resync hook).

--------------------------------------------------------------------------------
19. BIG CLUSTER AT 2.3 GHz + TMU THERMAL (2026-10-07)
--------------------------------------------------------------------------------
- Added the 1807-2314 MHz big-cluster OPPs with ECT voltages (worst ASV bin of
  table v1 + ~12.5 mV). Each step passed a 30 s hash-checked load test
  (tools/soak.sh): the section 13 "hangs above 1.7 GHz" was the cluster swap.
- But a 2-minute all-core soak at 2.3 GHz reset the phone after ~70 s, at
  1137.5 and at 1162.5 mV alike. Reading the TMU by hand showed thermal runaway:
  the die passed 150 C (sensor 0) with nothing throttling. Even the old
  1703/1690 MHz default reached 94 C in 30 s and kept rising.
- TMU access: 0x10080000 (CPU sensors 0-5; 0 = MNGS reference, 1 = little,
  2-5 big hot spots) and 0x10084000 (GPU = sensor 0), IRQs SPI 451 / 452. The
  register clock is gout_peris_busif_tmu_pclk (CMU_PERIS+0x2020, bit 21), which
  clk_disable_unused gates: /dev/mem reads give a Bus error until it is on.
  Trim: TRIMINFO + 4n per sensor, 25 C code in [8:0], 85 C code in [17:9],
  two-point flag bit 23; vref/slope in TRIMINFO0/1 [22:18]/[21:18]. Current
  codes at 0x40 (sensors 0-1) and 0x44/0x48 (three per word), 9 bits each.
  Stock reports max(sensor0, sensorN - 20) ("balance" mode).
- Driver: mainline exynos_tmu.c + SOC_ARCH_EXYNOS8895 (Exynos7 threshold,
  interrupt and emulation layout for sensor 0; per-sensor trims; multi-sensor
  read). Reference: exynos8895/android_kernel_samsung_universal8895
  drivers/thermal/samsung/exynos_tmu.c (lineage-19.1).
- Zones: cpu-thermal 83/88/93/98 C passive + 115 critical, cooling both cpufreq
  policies. A single passive trip was not enough: step_wise only adds cooling
  while the temperature is clearly rising, and a slow climb with +-1 C noise
  isn't - the die sat at ~100 C. Each extra trip forces a minimum cooling
  state. Result: 2.3 GHz big-only soak peak 92 / steady 86 C; all-core soak
  under schedutil steady 88 C, zero hash errors.
- Lesson: TMU emulation (thermal_zone/emul_temp) tests the throttling path
  without heating the chip; the hardware trip (CONTROL bit 12) is still off.

--------------------------------------------------------------------------------
20. PLASMA AT 30 FPS - DECON FRAME-START IRQ (2026-10-07)
--------------------------------------------------------------------------------
Symptom: Plasma 20-28 fps, GPU fragment engine "busy" 150-660% while scrolling.
Ruled out, one by one: CPU (kwin/plasmashell mostly <40% of a core), memory
bandwidth (CPU memcpy ~15 GB/s - NB: reading CMU_MIF PLL registers at
0x16x00100 hangs the bus), GPU clocks (G3D PLL 546 MHz), raw GPU speed
(glmark2 1440x2960 500 fps off-screen, 745 fps on-screen), render target
(a scanout dumb buffer from card0 renders as fast as Mesa's own: 3 full-screen
blended layers in 3.2 ms), Plasma's blur effects and AFBC (PAN_MESA_DEBUG=
noafbc) - none changed anything.
Two real causes:
1. KWin software brightness. With no panel brightness control KWin dims by
   recolouring every pixel ("allowSdrSoftwareBrightness"); at 29 % that was
   most of the GPU load. At 100 % KWin's GPU fragment time fell to ~9 %.
2. The 30 fps cap. KWIN_LOG_PERFORMANCE_DATA=1 writes "kwin perf statistics
   <output>.csv": KWin rendered in ~4 ms but every flip landed exactly two
   refreshes after its target. A DECON trace (commit / frame start / frame
   done / event + shadow-update state) showed only one IRQ per frame, at frame
   done: the driver requested platform_get_irq(pdev, 1) = SPI 144 frame done
   and never the frame-start line (SPI 143). The DSI transfer takes ~14.9 ms,
   so frame done comes 1.8 ms before the next TE latch; KWin got the flip
   event there, committed ~3 ms later - after the latch - and every frame
   waited a refresh. Requesting both lines by name puts vblank and the flip
   event at frame start: commit -> event 2.8 ms, 255/255 frames at 60 fps.
Tools left on the phone: ~/bin/uiprof.sh (kwin/plasmashell CPU + panfrost
fdinfo engine time, needs .../13900000.gpu/profiling=1), gpuproc.sh (GPU time
per process), membw, fbtest (render-target speed).

--------------------------------------------------------------------------------
21. USB DEVICE MODE (2026-10-08)
--------------------------------------------------------------------------------
Result: USB 2.0 high-speed peripheral; postmarketOS's configfs NCM gadget +
unudhcpd give the standard 172.16.42.1 network and SSH over the cable
(~32 MB/s laptop->phone, ~15 MB/s phone->laptop through ssh).
- Controller: Synopsys DWC3 2.80a (GSNPSID 0x5533280a) at 0x10c00000, IRQ SPI
  337, glue "samsung,exynos8895-dwusb3".
- PHY: "KC" USB 3.0 DRD PHY, vendor CAL version 01_1_1
  (exynos8895/android_kernel_samsung_universal8895, drivers/phy/
  phy-exynos-usbdrd.c + phy-samsung-usb3-cal.c). Exynos5 register layout,
  but: a version word at +0 (0x10000) shifts all registers by 4; two port
  blocks (0x10e00000, 0x10e10000); PMU 0x704 bit 0 de-isolates; reference
  clock select bit 24 at 0x10e5007c; Q-channel workaround (PHYRESUME bits +
  LINKSYSTEM soft reset); REFCLKSEL = clock core, FSEL from the 26 MHz ref;
  no VBUS pad -> VBUSVALID/BVALID forced. New variant in phy-exynos5-usbdrd.c.
- Gotcha 1 - registers read 0: gout_fsys0_usbtv_i_usbtvh_xiu_clk (the USBTV
  XIU) carries all register access to the DRD block and PHY. Nothing claimed
  it, so clk_disable_unused gated it right after probe: every DWC3/PHY read
  returned 0 and the controller was cut off. The glue now holds it (plus
  USBTV AHB, LHM_AXI_D_USBTV, US_D_FSYS0_USB).
- Gotcha 2 - the legacy CONFIG_USB_ETH (g_ether) grabbed the UDC before the
  initramfs configfs gadget ("couldn't find an available UDC") and its
  RNDIS link timed out on the host. Disabled.
- Gotcha 3 - cable attached at boot: the MAX77865 MUIC (I2C 0x25 on
  HSI2C_USI13_1, 0x10970000) leaves its D+/D- switch OPEN (CONTROL1 0x19 =
  0x00) when a cable is present at power-on; a re-plug made its own
  detection close it. drivers/usb/misc/max77865-muic.c writes 0x89
  (D+/D- -> USB AP, BC comparator off) at probe. Plug orientation turned out
  to be a red herring (gpi1-7, the Type-C orientation input, only matters
  for the USB 3 lanes).
- Laptop side: a static 172.16.42.2/24 on another interface (enp3s0 here)
  can win the route; `ip route add 172.16.42.1/32 dev enx...` or remove it.
- Don't unbind/rebind exynos-dwc3 at runtime: it crashed the kernel, and the
  next reboot hung in the USB shutdown path (forced restart needed).
MTP: postmarketOS's usb-signaller mtp_ffs_mode (ffs.mtp + umtprd) works as
is; `usb-mode mtp|developer` (rootfs-addons) switches. umtprd.conf sets
default_uid/gid 10000, else copied files are root-owned 0600. ADB not set
up: Alpine's adbd is TCP-only and SSH over USB covers it.
Not done: host mode (S2MM005 role detection + VBUS boost), USB 3 (PIPE3).

--------------------------------------------------------------------------------
22. BATTERY / FUEL GAUGE / CHARGER (2026-10-08)
--------------------------------------------------------------------------------
MAX77865 on HSI2C_USI13_1 (i2c-2): MUIC 0x25, fuel gauge 0x36, PMIC 0x66,
charger 0x69.
- Fuel gauge: ModelGauge m5, MAX17047-style registers (DevName 0x6100),
  10 mOhm sense (DesignCap 0x1886 = 3139 mAh at 0.5 mAh/LSB). Mainline
  max17042_battery + compatible "maxim,max77865-battery". Gotcha: the
  existing "maxim,max77705-battery" entry is OF-only; the I2C probe looks
  the chip type up by client name in the i2c_device_id table and returns
  -ENODEV silently. Status falls back to AvgCurrent sign when there is no
  supplier (no charger driver), else it is always Unknown.
- Charger (0x69, regs 0xb0-0xc3, MAX77705-like): bootloader leaves
  CNFG_00 = 0x15 (charger+buck on, WDTEN=1), CHGIN_ILIM ~500 mA, CHG_CC
  ~450 mA. Nobody kicks the watchdog, so ~2-3 min after boot CHG_DTLS
  (DETAILS_01 bits 3:0) = 0xB "off, watchdog expired": the battery idles
  (or drains if load > input limit). Right after boot it charges at ~1 A.
  Clearing WDTEN by hand (0x15 -> 0x05) brought charging straight back
  (CHG_DTLS 0x1, fast charge, ~1 A into the battery), confirming it.
  Fix: drivers/power/supply/max77865_charger.c keeps WDTEN on and kicks
  WDTCLR every 20 s (vendor: every 10-30 s from get_charging_health); if
  it finds CHG_DTLS 0xb with an input present it toggles CNFG_00 CHG off/on
  as the vendor does. Registers a USB power supply (CHGIN_OK) that the fuel
  gauge uses as its supplier. Not done: input/charge current control, the
  charger IRQ, USB-PD/AFC fast charging.
  Measured (v144, wall charger, MUIC CHGTYP 3 = DCP): bootloader limits
  CNFG_02 0x1e (CC 1.5 A, 50 mA/step) and CNFG_09 0x3c (CHGIN 1.5 A,
  25 mA/step); ~+1.05 A into the battery with the phone idle (SoC ~50 C).
  While busy (Plasma start-up, SoC ~76 C) the load can exceed it and the
  battery dips even though CHG_DTLS says fast charge. After a watchdog
  expiry the limits fall back to POR defaults (0x09 = 450 mA, 0x14 =
  500 mA). On a laptop port the source itself is the limit.
- USB side note: the PHY forces VBUSVALID, so DWC3 never sees an unplug
  (UDC stays "configured" on a wall charger). Harmless; needs VBUS/CC
  detection (MUIC or S2MM005) to fix.

--------------------------------------------------------------------------------
23. SCREEN OFF, MISSING LETTERS, KERNEL LOG (2026-10-08)
--------------------------------------------------------------------------------
Screen off: the power button made KWin turn the CRTC off (debugfs state
active=0, DPMS Off), but the panel driver had no unprepare, so the
command-mode DDI kept showing its GRAM: frozen last frame, touch ignored.
Fix: unprepare sends display off + sleep in, prepare sleeps out on return.
Gotcha: on the S6E3HA6 even the standard DCS 0x28/0x10/0x11 are only
taken behind the level 1 key (0x9f 0xa5 0xa5, as downstream display-off-
seq/exit-seq); without it they are ACKed by the DSIM but ignored by the
DDI. DRM calls the panel post_disable BEFORE the CRTC disable, so the
commands go out while the DECON still triggers frames - that is fine.
Touch wakes the screen via KWin's DoubleTapWakeup ([Wayland] in kwinrc).

Missing letters ("i-Fi", "Mobi e Data", "B uetooth", stray glyphs), on
some boots, until the session restarts:
- tools/scanout.c (raw DRM GETFB2 + PRIME mmap, root) dumps the buffer
  the DECON is scanning: the letters are missing IN the rendered frame, so
  the display path is not involved; background intact = glyphs not drawn,
  not a memory scribble.
- memtester 3 GB (42% in the 0x900000000 bank): 12 tests passed (stopped
  at Bit Spread). All firmware no-map regions of the vendor rmem are
  reserved (camera ION pool at 0x880000000 is not firmware).
- GPU: Mali-G71 r0p0 (05dev0); vendor kbase lists 13 errata for it, the
  kernel side of which panfrost covers; the rest are userspace (Mesa).
  Vendor runs it non-coherent too.
- Under test: QML_USE_GLYPHCACHE_WORKAROUND / QT_ENABLE_GLYPH_CACHE_
  WORKAROUND (rootfs-addons, /etc/xdg/plasma-workspace/env): Qt keeps a CPU
  copy of the glyph atlas instead of growing it with a GPU copy.
Kernel log: CONFIG_DEBUG_DRIVER was on (OPP + driver core -DDEBUG, ~10
lines/s) and pushed everything out of dmesg within minutes. Off now.
panfrost.transparent_hugepage=0 was only on the phone's local dtb (a
leftover from section 17, where it changed nothing); removed.

--------------------------------------------------------------------------------
11. TODO / KNOWN REMAINING WORK
--------------------------------------------------------------------------------
[x] WiFi (done 2026-10-03, see section 10)
[x] Big-cluster (M2) frequency switching + stock 2.314 GHz with TMU
    thermal throttling (sections 13, 19)
[ ] 5 GHz / WPA3 association (firmware/CLM? the 5GHz WPA3 AP scans at 89%
    but nmcli connect fails "network could not be found")
[ ] Sustained-WiFi-RX instant reboot (section 14)
[x] ~2 min WiFi delay: txcap_blob sysfs-fallback timeouts (2x 60 s) - fixed
    with sysctl.kernel.firmware_config.ignore_sysfs_fallback=1 (section 18)
[ ] OpenRC service (or local.d) to auto-run bt-pan-up.sh after boot
    (needs hciattach-from-boot to be finished first -- it already is; the
    boot log shows firmware flashed and hci0 present at login).
[x] USB device mode (section 21). [ ] host mode / USB 3.
[ ] GPU/display: simpledrm only (llvmpipe); panfrost/Mesa for Mali-G71
    (needs G3D power domain/clocks - CMU_G3D not modeled yet either).
[x] UFS internal storage driver (2026-10-06, section 16).
[ ] TO FIX (parked 2026-10-12): Mesa/Panfrost on Mali-G71 draws GTK4 apps
    wrong under GTK's GL and Vulkan renderers (no text, icons, rounded
    corners; video dimmed). Workaround GSK_RENDERER=cairo per app (MFC
    section) costs ~5x CPU on video. Find the Mesa bug (suspect: GTK's
    per-instance vertex data reads as zero), patch, rebuild Mesa.
[ ] TO FIX (parked 2026-10-12): call audio. Calls connect and stay up (2G;
    CP voice firmware VSS runs on the ABOX with the module from
    tools/modem/wip, gear capped at 590 MHz), but the downlink is digital
    silence and the mics are not routed. Notes: tools/modem/wip/README.md.
[ ] Spectacle's autostarted --dbus service busy-loops at ~80% CPU on this
    phone (heats it up). Autostart disabled for the user
    (~/.config/autostart/spectacle.desktop, Hidden=true).
[ ] mifmon: after a 2G -> LTE switch, if the data call dropped, the attach
    retry loop (CGDCONT every 10 s) can starve the AT+CGATT=1 it should send;
    a modem-ipc restart recovers. A mode set right before a reboot may not
    reach the CP's NV.
[x] Browser start-up freeze: firmware-owned RAM was not reserved (section 17).
[ ] Consider re-pairing resilience: a tiny phone-side script for
    discoverable/agent state if bonds break again.
[ ] bnep-up could daemonize properly + auto-reconnect instead of pause().
[ ] Upstreaming is explicitly OUT OF SCOPE for the user right now.
[ ] pmaports r13 kernel package has NOT been rebuilt via pmbootstrap --
    the running system got its modules hand-installed (see section 10);
    a future pmbootstrap build from the r13 APKBUILD should reproduce it.

--------------------------------------------------------------------------------
HISTORY: BT/PAN state reached 2026-10-01 (~8h bring-up): mainline boot ->
console UX fixes -> BT UART bring-up -> patchram filename hunt -> bluez PAN
failures -> bdaddr multicast-bit discovery -> .hcd firmware patch -> kernel
7.0 bnep API rewrite (bnep-up tool) -> BNEP handshake -> bridge setup ->
laptop NAP registration fix -> sshd binary hunt -> MTU blackhole discovery
-> MTU 600 both ends -> ssh + key auth.

WiFi state reached 2026-10-03 (~2 days bring-up): PCIe clock gating
hang -> bogus ELBI reset hang -> phy ordering -> full downstream power
sequence -> bogus bridge window discovery -> iATU inline-in-DBI discovery
(atu_base) -> dw core skipping MEM windows with custom child ops ->
manual viewport programming -> BAR reads abort despite windows ->
SoC 4MB routed-aperture discovery via remap probe -> 5MB window sizing
fight (4MB+32KB rounds to 5MB, 4MB-aligned) -> config-aperture
reservation hole discovery -> release_mem_region fix -> chip is BCM4361
not 4375 -> brcmfmac 4361 additions (rambase 0x170000 from downstream,
fw table, pci id 0x441f) -> unassigned BAR0 (chip decode written by
brcmfmac + iATU alias) -> end-to-end auto-connect at boot.
================================================================================

## Audio: ABOX + CS47L93 (kernel 7.0-r8)

The Note 8's audio path is Exynos8895 **ABOX** (DMAs, mixers and I2S ports run
by Samsung's *Calliope* firmware on a Cortex-A7 inside the block) feeding a
Cirrus Logic **CS47L93** codec on SPI3. Mainline had neither the ABOX driver nor
the codec's supplies. What it took:

- **Codec power:** DCVDD is S2MPS17 **LDO33** (1.2 V, off at boot); with it off
  the codec reads ID 0xffff. LDO32/LDO33 added to `s2mps11`. The SPI bus needs
  `samsung,spi-feedback-delay = <1>` or every read is shifted by a bit.
- **Codec clock:** MCLK1 is the 26 MHz TCXO on **PMU CLKOUT0** (6-bit mux;
  bit 0 is a *disable* bit). MCLK2 carries nothing, so the madera 32 kHz clock
  must come from SYSCLK, otherwise the volume ramps turn audio into noise.
- **Calliope boot:** firmware `calliope_{sram,dram,iva}.bin` from the stock
  vendor image. Lessons: the downstream `SMC_CMD_REG` secure call hangs; write
  the ABOX GIC directly (the AP's accesses count as secure). `exynos-iommu` only
  translates while the master is runtime-PM active; otherwise the SysMMU stays in
  bypass and the A7 scribbles on physical RAM. Reading chip ID 0x10000000 faults,
  so the firmware gets a fake ID page. Unmapped IOVAs cause an interrupt storm,
  so faults are backed with a dummy page. The firmware's CPU gear requests
  (`ABOX_CHANGE_GEAR`) must be answered or it never starts recording.
- **Streaming:** RDMA0 -> SPUS mixer -> SIFS0 -> UAIF0. The SPUS mixer must be
  **flushed** before each stream or it free-runs (~3x speed). The DMA position
  comes from the RDMA/WDMA status register (offset in 16-byte units, 13 bits, so
  buffers are capped at 64 KiB); the firmware sends no pointer IPC.
- **The noise bug:** `UAIF_CTRL0` bit 2 **set** means the UAIF *drives*
  BCLK/LRCLK, the opposite of what the downstream code suggests. With it
  inverted, both the codec and ABOX drove the clocks and each stream started at a
  random bit offset: harsh, different-every-time distortion. Found with the codec
  AEC loopback at -9 dB as an alignment detector (at 0 dB the round trip masked
  it).
- **Userspace:** ALSA UCM (`rootfs-addons/usr-share-alsa-ucm2`) gives PulseAudio
  an Earpiece (default) and a Headphones port. The earpiece distorts above about
  -6 dB, so the machine driver caps that control there.

Not done: the bottom loudspeaker (MAX98506 amp on its own UAIF, i2c bus 0 at
0x31), microphones (MICBIAS), jack detection.

## Boot freeze, scheduler capacity, panel brightness (kernel 7.0-r9)

**Boot freeze on 5 GHz.** After the audio work the phone froze 35–55 s after
boot: the whole SoC stopped (even the 0.5 s DECON heartbeat), no panic, no
pstore. Two separate things turned up while chasing it:

- The Calliope firmware writes to IOVA 0xA0500000/0xA0570000 right after it
  boots. That is the voice firmware (VSS) window downstream maps to the modem's
  memory. Unmapped, each access is a SysMMU fault; one boot logged ~8,600 of
  them in a burst, printed from IRQ context. `exynos8895-audio-vss.patch` maps
  the whole 8 MiB window to the dummy page at probe and retries the resume IPC
  after boot-done. That ended the faults but not the freeze.
- With the ABOX module blacklisted the phone still froze, so audio was not the
  trigger. A/B boots: 5 GHz + `cpuspeed` switching to schedutil at ~29 s →
  froze 3/3; 2.4 GHz → fine; 5 GHz with the switch delayed to 90 s → fine (2/2);
  5 GHz joined after boot → fine for 10+ min. The freeze needs the 5 GHz link,
  the CPU boost and the desktop/GPU start-up (~31 s) together. `cpuspeed.start`
  now backgrounds itself and waits until 90 s of uptime. Root cause still open.
- Every boot logs `panfrost: gpu sched timeout, js=1 ... head=0xa008380` at
  ~33 s, including good ones (and in logs from 2026-10-07). Harmless.

**Scheduler capacity.** Logical CPUs 0 and 5–7 are A53s (the boot CPU is
`cpu@100`), 1–4 are Mongoose M2. Without `capacity-dmips-mhz` all eight read
`cpu_capacity` 1024 and busy threads (browsers) ran on A53s as often as on M2s.
A pinned single-thread sha256 of 64 MB takes 1.55 s on an A53 at 1.69 GHz and
0.64 s on an M2 at 2.314 GHz, hence 1024 (M2) / 572 (A53) →
`cpu_capacity` 1024 / 417. Unpinned runs now land on the M2s at 0.64 s.

**Brightness.** The panel has no DCS 0x51. `greatlte-panel-backlight.patch`
registers a raw backlight (0–1023) that writes only the AOR register: key2
(`F0 5A 5A`), `B1 hi lo`, latch `F7 03`, key2 off. AOR runs from 0x000C (the
downstream value at full brightness, and what the bootloader leaves) to 0x0B75
(the downstream lowest level), on a square-law curve. Gamma stays at the
bootloader's values, so 100 % equals the old fixed brightness and nothing goes
brighter. KWin picks the device up by itself and turns software brightness off
(`allowSdrSoftwareBrightness: false` in `kwinoutputconfig.json`), removing a
full-screen GPU pass per frame. The full downstream scheme also reprograms gamma
and ELVSS per luminance from the MTP calibration, which would keep colours exact
at the dark end.

Also checked and not limiting: thermal (45–48 °C idle, trips from 83 °C), GPU
clock (the G3D PLL reads 0xA0A80411 = 546 MHz; the "clock rate = 260000000"
boot message is stale), UFS (HS-G3 ×2), and Firefox renders through panfrost.

## Bottom loudspeaker: MAX98506 on UAIF4 (kernel 7.0-r10)

The speaker sits behind a Maxim MAX98506 boosted class-D amplifier
(`maxim,max98506` at 0x31 on hsi2c_11, chip version 0x80). Downstream links it
to ABOX UAIF4 as an I2S clock consumer (I2S, NB/NF, ABOX provides BCLK/LRCLK),
16-bit stereo in a 32-BCLK frame, the amp clocked from BCLK. The stock
`N950FXXUGDZEC` boot.img has exactly the same amp node and driver build
(0.00.0010) as the reference source.

What it took:

- **Pins.** UAIF4 comes out on **gph3-0..3** ("aud-cdma-bus" downstream), not
  on gph0-5..7 ("aud-spk-bus"). The latter are wired in parallel to the codec
  AIF1 nets on gph0-1..3: driving gph0-5 high as a GPIO pulls gph0-1 high. Our
  dts only muxed the codec and "spk" groups, so UAIF4 drove nothing while the
  amp, the clock dividers and the routing all looked right. Found with a small
  static program that samples the GPIO DAT register at ~1.6 MHz from
  `/dev/mem` and counts edges per pin (`aud_amp_bus` now muxes gph3).
- **ABOX path.** Speaker playback uses its own RDMA1 straight to SIFS1
  (SPUS_CTRL0 chain out field: 0 = SIFS1, 1 = SIFS0, 2 = SIFS2; SPUS_CTRL1
  [18:16] picks the RDMA feeding SIFS1; ROUTE_CTRL0 UAIF4 = 2 for SIFS1; flush
  SIFS1 with SPUS_CTRL3 bit 0; firmware config SET_OUT1_*). UAIF4 drives its
  clocks (UAIF_CTRL0 MODE = 1 - downstream writes 0 for "codec slave", but its
  4.4 ASoC flips the provider bits for a CPU DAI that is a codec) from
  CMU_ABOX AUDIF (0x1800, 49.152 MHz) through the 5-bit UAIF divider
  (0x1828 + 4n): 1.536 MHz needs /32, so the link is 48 kHz only. Keep
  UAIF_CTRL0[27:24] (START_FIFO_DIFF_SPK) at its reset value 1.
- **DMA position.** The RDMA/WDMA status count field is in 16-byte units like
  the offset (measured: 12 units/ms at 48 kHz stereo 16-bit = 192 B/ms), not
  bytes as downstream adds it. The old pointer crawled at 1/16 speed and then
  jumped a period, and PulseAudio underran ~45 times in the first seconds of
  every stream (audible as choppy starts). Fixed for all DMAs.
- **Amp quirks.** Registers lock while `GLOBAL_ENABLE` (0x38) is set; enable
  blocks (0x36) first, then global enable, from `mute_stream` like downstream.
  On this revision 0x1A[3:0], 0x1C/0x1D and 0x36[3:2] do not take writes, and
  LIVE_STATUS1 bit 5 (SPKCURNT) reads 1 whenever the amp has no valid I2S - it
  cleared the moment UAIF4 reached the pins. The gain register (0x2D, -6..+24 dB
  in 1 dB steps) mutes the output for a moment on every change, so UCM pins it
  at the stock +13 dB and PulseAudio uses software volume.
- **No speaker protection.** Downstream runs Maxim DSM on the ABOX DSP with the
  amp's V/I sense on UAIF4 capture; that is not ported, so the gain is capped
  at the stock value.

Detour worth recording: before finding the pins, a battery-current comparison
(amp off / silence / full-scale tone, ~1 A charge current as baseline) showed
the amp's output stage never drew power - an objective "is the amp running"
test that needs no ears.

## Hardware video codec: MFC v11 (kernel 7.0-r15)

The Exynos 8895 Multi Format Codec runs under mainline's `s5p-mfc` V4L2
mem2mem driver (`exynos8895-mfc.patch`, module `s5p_mfc`). Checked against
ffmpeg's software decoders with 720p test streams (150 frames each):

| Codec | Result |
|---|---|
| H.264, HEVC, VP9, VP8 decode | bit-identical to software |
| MPEG-2 decode | 73 dB PSNR vs software (IDCT is not bit-exact by spec) |
| MPEG-4 Part 2 decode | 57 dB (IDCT drift over P-frames); needs the VOL in-band |
| H.263 decode (CIF) | 69 dB |
| H.264 encode | High profile, 40.7 dB PSNR at 4 Mb/s; input must be NV21 |
| VC-1 decode, HEVC/VP8/MPEG-4/H.263 encode | not tested |

MPEG-4 in MP4 keeps the VOL header in the container, which ffmpeg's
`mpeg4_v4l2m2m` does not send (HEADER_NOT_FOUND, 0x66); an elementary stream
or GStreamer's `mpeg4videoparse` works. Raw streams without timestamps need
`-fps_mode passthrough` in ffmpeg or it drops frames.

    s5p-mfc 13ce0000.codec: firmware 191118, hardware version 0x1100

1080p H.264 (8 Mb/s, 600 frames) decodes at ~280 fps through ffmpeg
`h264_v4l2m2m` with ~1 s of CPU (software ffmpeg: ~15 s of CPU over all
cores); 720p output is bit-identical to ffmpeg's software decoder.

Firmware: the stock `/vendor/firmware/mfc_fw.bin` (656820 bytes, a raw ARM
image) installed as `/lib/firmware/s5p-mfc-v11.fw`. It is Samsung's and is not
in this repository; take it from the phone's stock firmware (vendor/system
image). The vendor driver boots it without the secure world (SMC is DRM-only).

What the port is made of:

- **Codec.** The 8895 MFC is v11 (`ip_ver` 15 -> 0x1100). Its register map,
  host commands and codec IDs are mainline's v10; context sizes and the H.264
  MV padding (1024) are v12's. New variant `samsung,exynos8895-mfc`, version
  0xB0 (0x1100 would trip mainline's v12 checks), firmware `s5p-mfc-v11.fw`,
  clock "mfc". Decoder options as the vendor driver: parallel processing off,
  conceal colour, realloc, special parsing, SEI recovery parsing
  (`v11_dec_opts`/`v11_sei` module parameters), per-frame DEC_TIMEOUT, CPB size
  = real buffer size. The firmware is reloaded on every boot of the codec (it
  modifies its own image while running, as on v12).
- **Header buffer.** The buffer used for the header parse also holds the first
  key frame. Mainline re-submits its unconsumed tail only for H.264 and drops
  it otherwise, so every later frame failed with SYNC_POINT_NOT_RECEIVED
  (0xBE; HEVC, MPEG-2) or NO_KEY_FRAME (0x22; VP9). On v11, as the vendor
  driver: HEVC uses the H.264 consumed-bytes check, every other codec decodes
  the header buffer again from the start.
- **Firmware below every buffer.** The firmware rejects the stream header
  (SEQ_HEADER -> error 0x37, ~50 us after the command) when the stream buffer
  is below its base. The vendor kernel's IOVA allocator is bottom-up, so the
  firmware (allocated first) is always lowest; the mainline IOMMU DMA
  allocator is top-down and put every buffer below the firmware. Fix: the
  32 MiB firmware/context pool is allocated with a 28-bit mask (lands at
  0x0e000000), later allocations use a 30-bit mask, and the board dts reserves
  IOVA 0x1000..0x0e000000 for the codec (`memory-region` -> reserved-memory
  node with `iommu-addresses = <&mfc 0x1000 0xdfff000>`; the cells are those
  of the codec's bus, not of reserved-memory) so the allocator restarts above
  the pool. Buffers then land at 0x3fxxxxxx and the header parses.
- **CMU_MFC** (0x13c00000) as a runtime-PM clock controller inside the MFC
  power domain (register save/restore), with gates for the codec and both
  SysMMUs. Two power-domain handshake requirements, both found by writing the
  PMU from userspace with nothing touching the block:
  - the TOP `gout_cmu_mfc_bus` gate must stay on (CLK_IS_CRITICAL): CMU_MFC
    gates it when it runtime-suspends, which is before genpd turns the domain
    off, and the power-down then sticks half way (status 0x0010000c, not even
    a power-on request recovers it; the next access to the block hangs the
    bus);
  - the block's gates are opened when CMU_MFC suspends (`suspend_regs`), as
    for Exynos 5433; with the codec gate closed the power-down is refused.
- **Power domain** `samsung,exynos8895-pd` (PMU 0x16484160, 0xF = on): longer
  timeout, and a power-off that times out is cancelled (domain switched back
  on) instead of being left in transition while genpd still counts it as on.
- **SysMMUs** (0x13c80000, 0x13ca0000) report version 7.0 and are driven as v5
  (`samsung,exynos8895-sysmmu`); the vendor kernel programs them through the v5
  translation registers too.
- **Q-channels and the codec lifecycle.** The bootloader enables the
  Q-channels of the MFC block (QCH_CON_* 0x3000..0x3038 = 0x003f0003, CMU
  controller option 0x800 = 0x10000000); a power cycle resets them to disabled
  (0x003f0002, 0), and with them disabled every later power-down of the domain
  fails (status 0x0009000f). CMU_MFC now saves and restores them with its
  clock registers, as the vendor kernel does, and the codec follows the vendor
  lifecycle: firmware booted on the first open, `MFC_OFF` (0x7120) = 1 and
  domain off on the last close. After `MFC_OFF` the codec halts and its
  registers abort (synchronous external abort on 0x7018) until the domain has
  been cycled, and the domain goes off asynchronously (SysMMUs and CMU_MFC
  suspend through deferred runtime PM), so the last close waits, with the
  device mutex held, until genpd reports the domain off; a racing open then
  finds a cold codec. Measured: ~14 ms per open/close including the firmware
  boot and a power cycle; 50 back-to-back opens, 20 rotating-codec sessions,
  parallel sessions all clean. Never write `RISC_ON` = 0 before `MFC_OFF`:
  that write then stalls the bus.

Using it: `/dev/video0` (decoder) and `/dev/video1` (encoder), group video.
GStreamer `v4l2h264dec`, `v4l2h265dec`, `v4l2vp9dec`, ... and ffmpeg
`*_v4l2m2m`. The decoder outputs NV12M (two non-contiguous planes), which a
sink can only take without a copy if it accepts GstVideoMeta: `waylandsink`
(DMA-BUF to KWin) and `glimagesink` do, zero-copy (1080p real-time playback:
~4% of one core); plain `fakesink` does not, and then GStreamer copies every
frame out of the uncached buffers (~22 fps at 1080p, one core busy). Use
`fakevideosink` for benchmarks. `tools/mfc/mfctest.sh` has the checks used
here.

YouTube (2026-10-12): yt-dlp-resolved 1080p60 streams play in real time on
the MFC through GStreamer `playbin3` - H.264 (avc1, High@4.2) ~18% of one core,
VP9 ~10%, including HTTPS download; YouTube's VP9 1080p60 decodes bit-identical
to software. Needed the v11 stream buffer at the full CPB size: s5p-mfc's
default (`DEF_CPB_SIZE`, 256 KiB, what GStreamer gets) cannot hold a 1080p60
key frame, and a picture that fills the buffer fails with firmware error 0x41.
AV1 is offered by YouTube but not decodable by this MFC.

Apps: Clapper (`clapper-enhancers` yt-dlp plugin) plays YouTube on the MFC
(`v4l2h264dec`; YouTube's DASH adapts 240p..720p60 on this Wi-Fi, ~35% of one
core including download, audio and rendering; 3-minute soak clean). Pipeline
(YouTube subscriptions) plays through the same libclapper. Setup and the
Alpine workarounds it needs: `tools/mfc/setup-youtube-apps.sh` (missing
libgstclapper.so link for Clapper's GL importers, libpeas Python loader for
the yt-dlp plugin, curl instead of souphttpsrc, and no media-scanner enhancer:
a second adaptivedemux2 libsoup session in the process trips libsoup's thread
assertion). The yt-dlp plugin prefers H.264, then AV1 (software here).

GTK4 apps (Clapper, Pipeline, any libadwaita app) render an empty UI with GTK's
GPU renderers on Panfrost/Mali-G71: solid rectangles draw, but text, icons,
rounded corners do not, and video comes out dimmed. Shaders
compile; GTK 4.24.1 with Mesa 26.2.3 and 26.2.4 GL and with panvk Vulkan
(forced, `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`) give the same picture, and no
`GSK_GPU_DISABLE`, `GDK_GL_DISABLE`, `PAN_MESA_DEBUG` or `BIFROST_MESA_DEBUG`
switch changes it. The two paths share only the Bifrost compiler and the
hardware, so this is a Mesa-on-G71 bug; Qt/Plasma are unaffected.
Workaround: `GSK_RENDERER=cairo` in user overrides of the two apps' launchers
(setup script). Cost: Clapper at 1080p30 uses ~121% of one core vs ~26% with
the GL renderer, since cairo copies every video frame through the CPU.

Driver fixes found through GStreamer renegotiation (adaptive streams reopen the
decoder on the same file at each resolution step): REQBUFS(0) on CAPTURE now
resets the capture queue state (mainline left it "mapped", so the reopened
instance ran INIT_BUFFERS without codec memory: "Not enough memory has been
allocated", then refused the new REQBUFS), and TRY_FMT reports
V4L2_FIELD_NONE (GStreamer flags FIELD_ANY as a driver bug).

Firefox (154) does not use the MFC: its V4L2 probe (widget/gtk/GfxInfo.cpp)
requires single-planar NV12/YV12 capture (the MFC offers NM12/NM21), and its
V4L2 decoder needs ffmpeg's v4l2m2m wrapper to output DRM_PRIME, which upstream
ffmpeg (Alpine) does not do (Raspberry Pi's fork does).

Boot images: v196 (Image from the patch as committed; MFC nodes + IOVA
reservation merged into the v186 dtb with `tools/mfc/mfc_dtb_merge.py` and
`mfc_iova_dt.py`).
