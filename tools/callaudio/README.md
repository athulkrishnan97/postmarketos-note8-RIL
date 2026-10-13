# callaudio

Voice call audio for the Galaxy Note 8: while a call is dialing, ringing out,
active or held, routes the downlink to the earpiece and both digital mics to
the uplink, keeps the ABOX/codec I2S link (UAIF0 <-> AIF1) clocked and enables
the ABOX call DMAs, then restores everything when the last call ends. Works
with any ModemManager voice client (plasma-dialer, gnome-calls).

Needs:

- the ABOX driver with the `call` attribute (tools/modem/wip/abox-vss.diff)
- `CONFIG_REGULATOR_ARIZONA_MICSUPP=m`: without it the codec's MICVDD is a
  dummy regulator, the mic charge pump stays off and the mics read a
  constant -32768; load it before the codec
  (`softdep snd_soc_cs47l92 pre: arizona-micsupp` in /etc/modprobe.d)
- "Main Mic" (IN1AR pins -> IN3L, MICBIAS1A) and "Sub Mic" (IN2AL pins ->
  IN2L, MICBIAS1B) widgets in the exynos8895_madera machine driver

Install:

    install -D -m755 callaudio /usr/local/sbin/callaudio
    install -m755 callaudio.initd /etc/init.d/callaudio
    install -m644 callaudio.confd /etc/conf.d/callaudio
    rc-update add callaudio default && rc-service callaudio start

Levels: /etc/conf.d/callaudio. Log: `/var/log/callaudio.log`.

Not yet: speakerphone (the dialer's speaker button has no effect) and
headset audio during calls.
