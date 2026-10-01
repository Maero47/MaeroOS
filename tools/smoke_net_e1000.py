#!/usr/bin/env python3
"""smoke-net-e1000: tools/smoke_net.py on an Intel e1000 with the disk
attached, plus DHCP's /etc/resolv.conf and DNS lookups (see smoke_net.py)."""
import sys

import smoke_net

if __name__ == "__main__":
    sys.argv = [sys.argv[0], "--nic", "e1000"] + sys.argv[1:]
    sys.exit(smoke_net.main())
