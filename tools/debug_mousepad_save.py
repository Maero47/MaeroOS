#!/usr/bin/env python3
"""Debug aid: boot disk-alpinex.img, launch mousepad, Ctrl+S, then NMI the
guest so kwatch dumps where every thread sleeps.  Output: build/smoke-alpinex/
(serial.log, maerox.log, screenshots).  Uses smoke_alpinex's harness."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke_alpinex  # noqa: E402


class Dbg(smoke_alpinex.AlpineX):
    def run(self):
        self.g.boot()
        self.sh("touch /tmp/.maerox-xtrace")
        mp = self.launch("mousepad", "")
        self.click_in(mp, mp["w"] // 2, mp["h"] // 2)
        self.inp.type("hello\n")
        self.g.settle(1.0)
        self.inp.combo(["ctrl"], "s")
        self.g.settle(15.0)
        self.g.shot("after-ctrl-s")
        print(self.sh("ps"))
        self.qmp.cmd("inject-nmi")
        time.sleep(5)
        self.g.settle(2.0)
        with open(os.path.join(smoke_alpinex.OUT, "xtrace-tail.log"), "w") as f:
            f.write(self.sh("tail -n 120 /tmp/maerox.log", 30))
        return []


smoke_alpinex.AlpineX = Dbg
if __name__ == "__main__":
    try:
        smoke_alpinex.main()
    except Exception as e:
        print("debug run ended:", e)
