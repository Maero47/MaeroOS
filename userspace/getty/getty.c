#include "../include/stdio.h"
#include "../include/unistd.h"

static char *disk_login_argv[] = { "/disk/login", "-f", "root", (char *)0 };
static char *initrd_login_argv[] = { "/login", "-f", "root", (char *)0 };
static char *disk_shell_argv[] = { "/disk/shell", (char *)0 };
static char *initrd_shell_argv[] = { "/shell", (char *)0 };

static char *disk_envp[] = {
    "PATH=/disk:/disk/bin:/:/bin",
    "HOME=/home/root",
    "TERM=linux",
    "USER=root",
    "LOGNAME=root",
    "TTY=tty0",
    (char *)0
};

static char *initrd_envp[] = {
    "PATH=/:/bin",
    "HOME=/",
    "TERM=linux",
    "USER=root",
    "LOGNAME=root",
    "TTY=tty0",
    (char *)0
};

int main(int argc, char *argv[]) {
    const char *tty = argc > 1 ? argv[1] : "tty0";
    int disk_login = access("/disk/login", X_OK) == 0;
    int initrd_login = access("/login", X_OK) == 0;
    int disk_shell = access("/disk/shell", X_OK) == 0;
    char *session_path = disk_login ? "/disk/login" : (initrd_login ? "/login" : (disk_shell ? "/disk/shell" : "/shell"));
    char **session_argv = disk_login ? disk_login_argv : (initrd_login ? initrd_login_argv : (disk_shell ? disk_shell_argv : initrd_shell_argv));
    char **envp = disk_shell ? disk_envp : initrd_envp;

    printf("MaeroOS getty on %s\n", tty);
    printf("login: root\n");

    execve(session_path, session_argv, envp);
    printf("getty: failed to exec %s\n", session_path);
    return 1;
}
