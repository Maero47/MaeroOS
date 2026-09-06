#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/stdlib.h"

static const char *sig_names[] = {
    "HUP","INT","QUIT","ILL","TRAP","ABRT","BUS","FPE",
    "KILL","USR1","SEGV","USR2","PIPE","ALRM","TERM",
    (char *)0
};

static int parse_signal(const char *s) {
    /* numeric */
    if (*s >= '0' && *s <= '9') return atoi(s);
    /* SIG prefix */
    if (strncmp(s,"SIG",3)==0) s+=3;
    for (int i=0; sig_names[i]; i++)
        if (strcmp(s, sig_names[i])==0) return i+1;
    return -1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { write(2,"usage: kill [-SIGNAL] PID\n",26); exit(1); }
    int sig=15, ai=1;
    if (argv[1][0]=='-') {
        sig=parse_signal(argv[1]+1);
        if (sig<0) { write(2,"kill: unknown signal\n",21); exit(1); }
        ai=2;
    }
    if (ai>=argc) { write(2,"kill: no PID\n",13); exit(1); }
    for (int i=ai; i<argc; i++) {
        int pid=atoi(argv[i]);
        if (kill(pid,sig)<0) { write(2,"kill: failed\n",13); exit(1); }
    }
    exit(0);
    return 0;
}
