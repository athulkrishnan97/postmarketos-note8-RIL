# Modem (RIL) bring-up: test sequence

> **Status 2026-10-10: the CP boots and answers AT on mainline** (kernel with
> `exynos8895-modem.patch` + `greatlte-secure-world.patch`, uniLoader payload at
> 0x80200000). Three things were required that this document originally missed:
> 1. `clk_ignore_unused pd_ignore_unused` - otherwise every Samsung EL3 monitor
>    call (CP control, Kinibi) hard-freezes the SoC;
> 2. bionic for `cbd` from TWRP (`tools/modem/make-android-root.sh`) - the dev
>    phone's SYSTEM partition is empty;
> 3. `/dev/umts_*` and RADIO owned by uid/gid 1001 (`60-greatlte-modem.rules`):
>    `cbd` drops root to the Android radio user.
> Working run: `mifmon &` then `android-env run /vendor/bin/cbd -d -tss310 -bm -mm
> -P platform/11120000.ufs/by-name/RADIO -n /efs`; then `printf 'AT\r' >
> /dev/umts_router` (replies in mifmon's log). Details in HANDOVER.md.

Companion to [RIL-PLAN.md](RIL-PLAN.md). Kernel side: `exynos8895-modem.patch`
(Samsung's `modem_v1` interface driver, ported to 7.0) plus the modem nodes
in `exynos8895-greatlte-modem.dtsi`. Userspace: `tools/modem/`.

The kernel (Image + dtb, with the driver built in) compiles; nothing here
has run on the phone yet. Each step says what to look for and
what to send back. Stop at the first step that does not match.

## 0. Back up (once, before anything else)

```sh
sudo tools/modem/backup-modem-partitions.sh /home/user/modem-backup
# on the laptop:
scp -r user@172.16.42.1:/home/user/modem-backup ~/note8-modem-backup
```

EFS holds the IMEI and the RF calibration (`nv_data.bin`). If it is
damaged, calls and data are gone for good. Keep the copy off the phone.

Look at the firmware while at it (on the laptop):

```sh
python3 tools/modem/modem_toc.py ~/note8-modem-backup/RADIO.img
```

Expected entries: `TOC`, `BOOT`, `MAIN`, `VSS`, `NV` (names may vary).

## 1. SMC go / no-go (any kernel, module only)

All CP power control goes through the EL3 monitor (SMC 0x82000700). Two
other Samsung SMCs hang on this boot chain, so try this one alone first:

```sh
cd tools/modem/smcprobe
make -C <kernel build dir> M=$PWD modules   # or build it with the kernel
sudo insmod smcprobe.ko      # prints, then refuses to load (-EAGAIN): normal
sudo dmesg | grep smcprobe
```

- Good: `SMC CP_CTRL_NS: ret=0 val=...` and `CP_CTRL_S: ret=0`, values close
  to the plain PMU reads on the first line.
- Bad: the phone freezes, or ret is non-zero. Report it: the CP cannot be
  started on this boot chain without a different approach.

If reads are fine, `sudo insmod smcprobe.ko write=1` writes CP_CTRL_NS back
unchanged and prints the monitor's answer.

## 2. Kernel with the modem driver

Flash a boot image built from kernel r13 (with `exynos8895-modem.patch`).
After boot:

```sh
dmesg | grep -i -E "mif|mcu_ipc|shmem|modem"
ls -l /dev/umts_* ; ip link | grep rmnet
```

- Good: `mcu_ipc probe`, `shmem driver init` with `ipc_off=134217728`,
  `ss355ap ... link created`, and the devices `umts_boot0`, `umts_ipc0`,
  `umts_rfs0`, `umts_router`, `rmnet0`..`rmnet7`.
- Nothing starts the CP yet; the phone should behave exactly as before.

## 3. Boot the CP with Samsung's cbd

`cbd` comes from the phone's own SYSTEM partition (stock or LineageOS),
run in a chroot by `android-env`:

```sh
sudo install -m755 tools/modem/android-env.sh /usr/local/bin/android-env
sudo install -m755 rootfs-addons/etc-init.d/cbd rootfs-addons/etc-init.d/modem-ipc /etc/init.d/
sudo android-env up            # mounts SYSTEM (ro), EFS (rw), RADIO link
ls /var/lib/android-modem/vendor/bin/cbd /var/lib/android-modem/system/bin/cbd
```

First run in the foreground, with the IPC monitor already holding the
channels open (needed for the final handshake):

```sh
gcc -O2 -Wall -o mifmon tools/modem/mifmon.c && sudo install -m755 mifmon /usr/local/bin/
sudo mifmon > /tmp/mifmon.log &
sudo android-env run /vendor/bin/cbd -d -tss310 -bm -mm \
     -P platform/11120000.ufs/by-name/RADIO -n /efs 2>&1 | tee /tmp/cbd.log
```

(use `/system/bin/cbd` if that is where it is). In another shell:
`dmesg -w | grep -i -E "mif|cp_|shmem"`.

What a good boot looks like in dmesg, in order:

1. `IOCTL_MODEM_RESET`, `IOCTL_MODEM_BOOT_ON`, several `XMIT_BOOT`
2. `IOCTL_SECURITY_REQ` -> `mode=0 ... return_value=0`
3. `IOCTL_MODEM_ON`, `CP Power` / `CP Start` messages
4. `INIT_START <- ss355ap` then `PIF_INIT_DONE ->`
5. `CP_START <- ss355ap` then `INIT_END ->` (only while mifmon runs)
6. `STATE_ONLINE`; cbd prints that the boot is done

Send back `/tmp/cbd.log`, the dmesg lines and `/tmp/mifmon.log`.

Likely failures and what they mean:

| Symptom | Meaning |
|---|---|
| cbd does not start (linker errors) | the chroot is missing a library or the APEX mount (Android 10+): send the error |
| `return_value` non-zero after SECURITY_REQ | EL3 rejected the image or the memory layout |
| phone freezes at SECURITY_REQ / MODEM_ON | an SMC hangs, or the AP touched now-protected CP memory |
| `CP power on fail` warning | CP reset sequencer did not reach state 5 |
| no `INIT_START` within 15 s | CP firmware did not come up: CP crash or wrong NV |
| `INIT_END` missing | mifmon was not running (umts_ipc0 + umts_rfs0 must be open) |

## 4. First contact

With the CP online and mifmon logging:

```sh
sudo mifmon -a 'AT' ; sudo mifmon -a 'AT+CGMR' ; sudo mifmon -a 'AT+CPIN?'
```

(stop each with Ctrl-C after a few seconds). An `OK` on `ROUTER rx` means
the CP's AT parser is reachable from `umts_router`, which decides the
userspace route: ModemManager over AT (RIL-PLAN.md §4 option A) or a
Samsung IPC daemon (option B). Also send `/tmp/mifmon.log` from step 3:
the FMT frames the CP sends unprompted (power-up, SIM status) and the RFS
file requests show what the RIL has to answer.

## 5. Running it as services (after step 3 works by hand)

```sh
sudo rc-service modem-ipc start
sudo rc-service cbd start
# at boot:
sudo rc-update add modem-ipc default && sudo rc-update add cbd default
```
