#!/usr/bin/env python3
"""Boot the ISO+disk-ff, wait for the shell, and run a few `ls`/diagnostic
commands to check getdents/readdir on the large icon directories (does GTK
actually see the 736 installed icons?)."""
import os, selectors, subprocess, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
CMDS = [
    "ls /usr/share/icons/hicolor/16x16/actions | wc -l\n",
    "ls /usr/share/icons/hicolor/16x16/actions | head -5\n",
    "ls /usr/share/icons/hicolor | wc -l\n",
    "cat /usr/share/icons/hicolor/index.theme | head -2\n",
]
QEMU = ["qemu-system-i386","-cdrom","maeros.iso","-drive","file=disk-ff.img,format=raw,if=ide",
        "-smp","2","-m","2048M","-display","none","-serial","stdio","-no-reboot","-no-shutdown"]
def main():
    p = subprocess.Popen(QEMU, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector(); sel.register(p.stdout, selectors.EVENT_READ)
    log=[]
    def pump(dl):
        while time.time()<dl:
            for k,_ in sel.select(0.2):
                c=os.read(k.fd,65536).decode("latin1","replace")
                if c: log.append(c); sys.stdout.write(c); sys.stdout.flush()
            if p.poll() is not None: return False
        return True
    dl=time.time()+120
    while time.time()<dl:
        pump(time.time()+0.5)
        if PROMPT in "".join(log): break
    if PROMPT not in "".join(log):
        print("\n!!! no prompt"); p.terminate(); return
    for c in CMDS:
        p.stdin.write(c.encode()); p.stdin.flush(); pump(time.time()+6)
    p.terminate()
if __name__=="__main__": main()
