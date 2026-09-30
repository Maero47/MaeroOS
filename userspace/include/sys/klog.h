#pragma once

/* klogctl() reads the kernel log through /proc/kmsg; clearing is refused. */
int klogctl(int type, char *buf, int len);
