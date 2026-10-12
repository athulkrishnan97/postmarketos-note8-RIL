# sleepd

Android-style sleep for the Galaxy Note 8: suspend (s2idle) whenever the
screen is off and nothing needs the CPU; go back to sleep 10 s after any
wakeup (modem mailbox, RTC alarm) that did not turn the screen on. Needs the
r16 kernel (`exynos8895-suspend.patch`). Details: docs/BRINGUP.md, Sleep.

Install:

    install -D -m755 sleepd /usr/local/sbin/sleepd
    install -m755 sleepd.initd /etc/init.d/sleepd
    install -m644 sleepd.confd /etc/conf.d/sleepd
    rc-update add sleepd default && rc-service sleepd start

Keep it awake for a while: `touch /run/sleepd.disable` (remove to re-enable).
Log: `/var/log/sleepd.log`.
