# callaudio

Voice call audio for the Galaxy Note 8: as soon as a call exists (ringing
in included) enables the ABOX call DMAs and raises VDD_INT for the A7; while
a call is dialing, ringing out, active or held, routes the downlink to the
earpiece and both digital mics to the uplink and keeps the ABOX/codec I2S
link (UAIF0 <-> AIF1) clocked; restores everything when the last call ends.
Works with any ModemManager voice client (plasma-dialer, gnome-calls).

Needs:

- kernel 7.0-r17 (exynos8895-call-audio.patch: the ABOX `call` attribute,
  VSS memory, gear_min, the mic widgets; CONFIG_REGULATOR_ARIZONA_MICSUPP)
- rootfs-addons/etc-modprobe.d/greatlte-audio.conf, which loads the mic
  supply before the codec (otherwise the mics read a constant -32768)
- i2c-tools (VDD_INT) and alsa-utils

Install:

    install -D -m755 callaudio /usr/local/sbin/callaudio
    install -m755 callaudio.initd /etc/init.d/callaudio
    install -m644 callaudio.confd /etc/conf.d/callaudio
    rc-update add callaudio default && rc-service callaudio start

(tools/install-rootfs-addons.sh installs it into images; enable it with
the modem.) Details: docs/BRINGUP.md, "Call audio".

Levels: /etc/conf.d/callaudio. Log: `/var/log/callaudio.log`.

Not yet: speakerphone (the dialer's speaker button has no effect) and
headset audio during calls.
