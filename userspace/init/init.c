#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/time.h"
#include "../include/fcntl.h"
#include "../include/sys/wait.h"
#include "../include/string.h"
#include "../include/signal.h"
#include "../include/poll.h"

static char *initrd_shell_argv[] = { "/shell", (char *)0 };
static char *initrd_desktop_argv[] = { "/desktop", (char *)0 };
static char *disk_shell_argv[] = { "/disk/shell", (char *)0 };
static char *disk_getty_argv[] = { "/disk/getty", "tty0", (char *)0 };
static char *disk_desktop_argv[] = { "/disk/desktop", (char *)0 };
static char *disk_rc_argv[] = { "/disk/shell", "-c", ". /etc/rc", (char *)0 };
static char *disk_service_argv[] = { "/disk/shell", "-c", (char *)0, (char *)0 };

#define MAX_SERVICES 8
#define SERVICE_NAME_MAX 32
#define SERVICE_CMD_MAX 180
#define MAX_SESSIONS 4
#define SESSION_ARG_MAX 8
#define SESSION_CMD_MAX 128

typedef struct service {
    int respawn;
    int enabled;
    int pid;
    char name[SERVICE_NAME_MAX];
    char command[SERVICE_CMD_MAX];
} service_t;

typedef struct session {
    int pid;
    char id[SERVICE_NAME_MAX];
    char command[SESSION_CMD_MAX];
    char command_buf[SESSION_CMD_MAX];
    char *argv[SESSION_ARG_MAX + 1];
    int  fast_exits;     /* consecutive immediate failures (for respawn backoff) */
    int  disabled;       /* parked after too many rapid failures */
    long start_time;     /* time() when last started (to detect instant failures) */
} session_t;

static service_t services[MAX_SERVICES];
static int service_count;
static int initctl_fd = -1;
static session_t sessions[MAX_SESSIONS];
static int session_count;

/* Persistent boot/service log, enabled only when a writable disk root exists. */
static int disk_logging = 0;

static void init_log(const char *line) {
    if (!disk_logging) return;
    int fd = open("/var/log/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
    close(fd);
}

static char *skip_space(char *s);
static char *next_word(char **cursor);
static int parse_service_command(char **cursor, int *enabled, char **command);
static void write_service_status(void);
static void write_session_status(void);

static char *initrd_envp[] = {
    "PATH=/:/bin",
    "LD_LIBRARY_PATH=/:/lib:/disk:/disk/lib",
    "HOME=/",
    "TERM=linux",
    "USER=root",
    "LOGNAME=root",
    (char *)0
};

static char *disk_envp[] = {
    "PATH=/disk:/disk/bin:/:/bin",
    "LD_LIBRARY_PATH=/:/lib:/disk:/disk/lib",
    "HOME=/home/root",
    "TERM=linux",
    "USER=root",
    "LOGNAME=root",
    (char *)0
};

/* The graphical session and everything it spawns run as this unprivileged
 * user (root keeps the console/getty for admin).  See /etc/passwd. */
#define SESSION_UID 1000
#define SESSION_GID 100

static char *user_envp[] = {
    "PATH=/disk:/disk/bin:/:/bin",
    "LD_LIBRARY_PATH=/:/lib:/disk:/disk/lib",
    "HOME=/home/user",
    "TERM=linux",
    "USER=user",
    "LOGNAME=user",
    (char *)0
};

static int file_available(const char *path) {
    return access(path, X_OK) == 0;
}

/* Like run_program() but drops to the unprivileged session user first. */
static void run_program_as_user(char *path, char **argv, char **envp) {
    int pid = fork();
    if (pid == 0) {
        setgid(SESSION_GID);
        setuid(SESSION_UID);       /* irreversible: euid leaves 0 */
        execve(path, argv, (char *const *)envp);
        printf("[init] Failed to exec %s as user\n", path);
        exit(1);
    }
    if (pid > 0) {
        int status, got;
        do { got = waitpid(pid, &status, 0); } while (got > 0 && got != pid);
    }
}

static int device_available(const char *path, int flags) {
    int fd = open(path, flags);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int graphics_session_available(void) {
    return device_available("/dev/fb0", O_RDWR) &&
           device_available("/dev/input/event0", O_RDONLY);
}

static void run_program(char *path, char **argv, char **envp) {
    int pid = fork();
    if (pid == 0) {
        execve(path, argv, (char *const *)envp);
        printf("[init] Failed to exec %s\n", path);
        exit(1);
    }
    if (pid > 0) {
        int status;
        int got;
        do {
            got = waitpid(pid, &status, 0);
        } while (got > 0 && got != pid);
    }
}

static int start_shell(char *shell_path, char **shell_argv, char **envp) {
    int pid = fork();
    if (pid == 0) {
        execve(shell_path, shell_argv, (char *const *)envp);
        printf("[init] Failed to exec %s\n", shell_path);
        exit(1);
    }
    return pid;
}

static int split_command(char *command, char **argv, int max_args) {
    char *p = command;
    int argc = 0;
    while (argc < max_args) {
        char *word = next_word(&p);
        if (!word) break;
        argv[argc++] = word;
    }
    argv[argc] = (char *)0;
    return argc;
}

static void set_session(session_t *session, const char *id, const char *command) {
    session->pid = -1;
    strncpy(session->id, id, SERVICE_NAME_MAX - 1);
    session->id[SERVICE_NAME_MAX - 1] = '\0';
    strncpy(session->command, command, SESSION_CMD_MAX - 1);
    session->command[SESSION_CMD_MAX - 1] = '\0';
}

static char *session_path(session_t *session) {
    strncpy(session->command_buf, session->command, SESSION_CMD_MAX - 1);
    session->command_buf[SESSION_CMD_MAX - 1] = '\0';
    if (split_command(session->command_buf, session->argv, SESSION_ARG_MAX) <= 0)
        return (char *)0;
    return session->argv[0];
}

static session_t *find_session_by_pid(int pid) {
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].pid == pid) return &sessions[i];
    }
    return (session_t *)0;
}

static int start_service(service_t *svc, char *shell_path, char **envp) {
    int pid = fork();
    if (pid == 0) {
        char cmd[SERVICE_CMD_MAX];
        char *argv[16];
        strncpy(cmd, svc->command, SERVICE_CMD_MAX - 1);
        cmd[SERVICE_CMD_MAX - 1] = '\0';

        char *p = cmd;
        int argc = 0;
        while (argc < 15) {
            char *word = next_word(&p);
            if (!word) break;
            argv[argc++] = word;
        }
        argv[argc] = (char *)0;

        if (argc > 0) execve(argv[0], argv, (char *const *)envp);
        (void)shell_path;
        printf("[init] Failed to exec service %s\n", svc->name);
        exit(1);
    }
    if (pid > 0) {
        svc->pid = pid;
        printf("[init] Starting service %s pid=%d\n", svc->name, pid);
        char lb[128];
        sprintf(lb, "service %s started pid=%d", svc->name, pid);
        init_log(lb);
    }
    return pid;
}

static service_t *find_service_by_pid(int pid) {
    for (int i = 0; i < service_count; i++) {
        if (services[i].pid == pid) return &services[i];
    }
    return (service_t *)0;
}

static int respawn_service_child(int pid, char *shell_path, char **envp) {
    service_t *svc = find_service_by_pid(pid);
    if (!svc || !svc->respawn) return 0;
    svc->pid = -1;

    if (!svc->enabled) {
        printf("[init] Service %s stopped\n", svc->name);
        write_service_status();
        return 1;
    }

    printf("[init] Respawning service %s\n", svc->name);
    start_service(svc, shell_path, envp);
    write_service_status();
    return 1;
}

static service_t *find_service_by_name(const char *name) {
    for (int i = 0; i < service_count; i++) {
        if (strcmp(services[i].name, name) == 0) return &services[i];
    }
    return (service_t *)0;
}

static char *skip_space(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void trim_line(char *s) {
    int len = (int)strlen(s);
    while (len > 0 &&
           (s[len - 1] == '\n' || s[len - 1] == '\r' ||
            s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static char *next_word(char **cursor) {
    char *s = skip_space(*cursor);
    if (!*s) {
        *cursor = s;
        return (char *)0;
    }
    char *word = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) *s++ = '\0';
    *cursor = s;
    return word;
}

static int parse_service_command(char **cursor, int *enabled, char **command) {
    char *first = next_word(cursor);
    if (!first) return 0;

    *enabled = 1;
    if (strcmp(first, "enabled") == 0 || strcmp(first, "disabled") == 0) {
        *enabled = strcmp(first, "disabled") != 0;
        *command = skip_space(*cursor);
        return **command != '\0';
    }

    *command = first;
    return 1;
}

static void write_service_status(void) {
    int fd = open("/tmp/services.status", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    char line[256];
    const char *header = "NAME TYPE BOOT PID STATE COMMAND\n";
    write(fd, header, strlen(header));
    for (int i = 0; i < service_count; i++) {
        service_t *svc = &services[i];
        sprintf(line, "%s respawn %s %d %s %s\n",
                svc->name,
                svc->enabled ? "enabled" : "disabled",
                svc->pid,
                svc->pid > 0 ? "running" : "stopped",
                svc->command);
        write(fd, line, strlen(line));
    }

    close(fd);
}

static void write_session_status(void) {
    int fd = open("/tmp/sessions.status", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    char line[256];
    const char *header = "ID ACTION PID STATE COMMAND\n";
    write(fd, header, strlen(header));
    for (int i = 0; i < session_count; i++) {
        session_t *session = &sessions[i];
        sprintf(line, "%s respawn %d %s %s\n",
                session->id,
                session->pid,
                session->pid > 0 ? "running" : "stopped",
                session->command);
        write(fd, line, strlen(line));
    }

    close(fd);
}

static void run_services(char *shell_path, char **envp) {
    FILE *f = fopen("/etc/services", "r");
    if (!f) return;

    printf("[init] Reading /etc/services\n");

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        char *p = skip_space(line);
        if (!*p || *p == '#') continue;

        char *type = next_word(&p);
        char *name = next_word(&p);
        int enabled = 1;
        char *command = (char *)0;

        if (!type || !name) {
            printf("[init] Ignoring malformed service line\n");
            continue;
        }

        if (strcmp(type, "once") != 0 && strcmp(type, "respawn") != 0) {
            printf("[init] Unsupported service type %s for %s\n", type, name);
            continue;
        }

        if (strcmp(type, "once") == 0) {
            command = skip_space(p);
            if (!*command) {
                printf("[init] Ignoring malformed service line\n");
                continue;
            }
            printf("[init] Starting service %s\n", name);
            disk_service_argv[0] = shell_path;
            disk_service_argv[2] = command;
            run_program(shell_path, disk_service_argv, envp);
            printf("[init] Service %s exited\n", name);
            continue;
        }

        if (!parse_service_command(&p, &enabled, &command)) {
            printf("[init] Ignoring malformed service line\n");
            continue;
        }

        if (service_count >= MAX_SERVICES) {
            printf("[init] Too many respawn services; skipping %s\n", name);
            continue;
        }

        service_t *svc = &services[service_count++];
        svc->respawn = 1;
        svc->enabled = enabled;
        svc->pid = -1;
        strncpy(svc->name, name, SERVICE_NAME_MAX - 1);
        svc->name[SERVICE_NAME_MAX - 1] = '\0';
        strncpy(svc->command, command, SERVICE_CMD_MAX - 1);
        svc->command[SERVICE_CMD_MAX - 1] = '\0';
        if (svc->enabled) {
            start_service(svc, shell_path, envp);
        } else {
            printf("[init] Service %s disabled\n", svc->name);
        }
        write_service_status();
    }

    fclose(f);
}

static void load_inittab(void) {
    FILE *f = fopen("/etc/inittab", "r");
    if (!f) return;

    printf("[init] Reading /etc/inittab\n");
    session_count = 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        char *p = skip_space(line);
        if (!*p || *p == '#') continue;

        char *id = next_word(&p);
        char *action = next_word(&p);
        char *command = skip_space(p);
        if (!id || !action || !*command) {
            printf("[init] Ignoring malformed inittab line\n");
            continue;
        }

        if (strcmp(action, "respawn") != 0) {
            printf("[init] Unsupported inittab action %s for %s\n", action, id);
            continue;
        }

        if (session_count >= MAX_SESSIONS) {
            printf("[init] Too many inittab sessions; skipping %s\n", id);
            continue;
        }

        session_t *session = &sessions[session_count++];
        set_session(session, id, command);
        printf("[init] Console %s respawn %s\n", session->id, session->command);
    }

    fclose(f);
}

static void setup_initctl(void) {
    mkdir("/tmp", 0755);
    mkfifo("/tmp/initctl", 0600);
    initctl_fd = open("/tmp/initctl", O_RDONLY);
    if (initctl_fd >= 0)
        printf("[init] Control FIFO ready at /tmp/initctl\n");
}

static void handle_control_line(char *line, char *shell_path, char **envp) {
    trim_line(line);
    char *p = skip_space(line);
    char *cmd = next_word(&p);
    char *name = next_word(&p);

    if (!cmd || !name) return;

    if (strcmp(cmd, "start") == 0) {
        service_t *svc = find_service_by_name(name);
        if (!svc || !svc->respawn) {
            printf("[init] Cannot start service %s\n", name);
            return;
        }

        svc->enabled = 1;
        if (svc->pid <= 0) {
            printf("[init] Starting service %s by request\n", svc->name);
            start_service(svc, shell_path, envp);
        } else {
            printf("[init] Service %s already running pid=%d\n", svc->name, svc->pid);
        }
        write_service_status();
        return;
    }

    if (strcmp(cmd, "stop") == 0) {
        service_t *svc = find_service_by_name(name);
        if (!svc || !svc->respawn) {
            printf("[init] Cannot stop service %s\n", name);
            return;
        }

        svc->enabled = 0;
        if (svc->pid <= 0) {
            printf("[init] Service %s already stopped\n", svc->name);
            write_service_status();
            return;
        }

        int old_pid = svc->pid;
        printf("[init] Stopping service %s pid=%d\n", svc->name, old_pid);
        kill(old_pid, SIGTERM);
        int status;
        if (waitpid(old_pid, &status, 0) == old_pid)
            respawn_service_child(old_pid, shell_path, envp);
        write_service_status();
        return;
    }

    if (strcmp(cmd, "restart") == 0) {
        service_t *svc = find_service_by_name(name);
        if (!svc || !svc->respawn) {
            printf("[init] Cannot restart service %s\n", name);
            return;
        }

        svc->enabled = 1;
        if (svc->pid <= 0) {
            printf("[init] Starting service %s by restart request\n", svc->name);
            start_service(svc, shell_path, envp);
            write_service_status();
            return;
        }

        int old_pid = svc->pid;
        printf("[init] Restarting service %s pid=%d\n", svc->name, svc->pid);
        kill(svc->pid, SIGTERM);
        int status;
        if (waitpid(old_pid, &status, 0) == old_pid)
            respawn_service_child(old_pid, shell_path, envp);
        write_service_status();
        return;
    }

    printf("[init] Unsupported control command %s\n", cmd);
}

static void poll_initctl(char *shell_path, char **envp) {
    if (initctl_fd < 0) return;

    struct pollfd pfd;
    pfd.fd = initctl_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0) return;
    if (!(pfd.revents & POLLIN)) return;

    char buf[128];
    int n = read(initctl_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    char *line = buf;
    while (*line) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = '\0';
        handle_control_line(line, shell_path, envp);
        line = next;
    }
}

static void settle_respawn_services(char *shell_path, char **envp) {
    if (service_count == 0) return;

    for (int i = 0; i < 64; i++) {
        int status;
        int pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            respawn_service_child(pid, shell_path, envp);
        }
        poll_initctl(shell_path, envp);
        sched_yield();
    }
}

static int start_session(session_t *session, char **envp) {
    char *path = session_path(session);
    if (!path) return -1;
    session->start_time = (long)time((time_t *)0);
    session->pid = start_shell(path, session->argv, envp);
    printf("[init] Session %s pid=%d\n", session->id, session->pid);
    char lb[128];
    sprintf(lb, "session %s started pid=%d", session->id, session->pid);
    init_log(lb);
    write_session_status();
    return session->pid;
}

static void start_sessions(char **envp) {
    for (int i = 0; i < session_count; i++)
        start_session(&sessions[i], envp);
}

static void monitor_children(char *command_shell_path, char **envp) {
    start_sessions(envp);

    while (1) {
        int status;
        int pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            poll_initctl(command_shell_path, envp);
            for (int i = 0; i < session_count; i++) {
                if (sessions[i].pid <= 0 && !sessions[i].disabled)
                    start_session(&sessions[i], envp);
            }
            sched_yield();
            continue;
        }

        session_t *session = find_session_by_pid(pid);
        if (session) {
            session->pid = -1;
            /* Respawn backoff: a session whose command exec-fails restarts
             * instantly, which used to busy-loop init at 100% CPU (tens of
             * thousands of forks) and starve the desktop/Firefox of cycles.
             * Only RAPID failures (ran < 3s) count toward backoff; a session
             * that ran a while and exited normally restarts immediately. */
            long ran = (long)time((time_t *)0) - session->start_time;
            if (ran >= 3) session->fast_exits = 0;
            else          session->fast_exits++;
            if (session->fast_exits >= 8) {
                if (!session->disabled) {
                    session->disabled = 1;
                    printf("[init] Session %s keeps failing instantly; parking it.\n",
                           session->id);
                }
                write_session_status();
                continue;   /* do not respawn — stop the CPU storm */
            }
            printf("[init] Session %s exited; restarting (try %d)\n",
                   session->id, session->fast_exits);
            write_session_status();
            for (int s = 0; s < session->fast_exits && s < 4; s++)
                usleep(250000);   /* 0.25s..1s backoff before respawn */
            start_session(session, envp);
            continue;
        }

        service_t *svc = find_service_by_pid(pid);
        if (svc && svc->respawn) {
            respawn_service_child(pid, command_shell_path, envp);
        }
    }
}

int main(void) {
    int disk_userland = file_available("/disk/shell");
    char *command_shell_path = disk_userland ? "/disk/shell" : "/shell";
    char *session_path = command_shell_path;
    char **session_argv = disk_userland ? disk_shell_argv : initrd_shell_argv;
    char **envp = disk_userland ? disk_envp : initrd_envp;
    if (disk_userland && file_available("/disk/getty")) {
        session_path = "/disk/getty";
        session_argv = disk_getty_argv;
    }

    session_count = 1;
    set_session(&sessions[0], "tty0", session_path);
    if (session_argv[1]) {
        strncat(sessions[0].command, " ", SESSION_CMD_MAX - strlen(sessions[0].command) - 1);
        strncat(sessions[0].command, session_argv[1], SESSION_CMD_MAX - strlen(sessions[0].command) - 1);
    }

    disk_logging = disk_userland;
    init_log("=== MaeroOS init boot ===");

    if (disk_userland && file_available("/etc/rc")) {
        printf("[init] Running /etc/rc\n");
        run_program(command_shell_path, disk_rc_argv, envp);
        init_log("ran /etc/rc");
    }

    if (disk_userland)
        load_inittab();

    if (disk_userland && file_available("/etc/services")) {
        setup_initctl();
        run_services(command_shell_path, envp);
        settle_respawn_services(command_shell_path, envp);
    }

    if (graphics_session_available()) {
        int disk_desktop = disk_userland && file_available("/disk/desktop");
        char *desktop_path = disk_desktop ? "/disk/desktop" : "/desktop";
        char **desktop_argv = disk_desktop ? disk_desktop_argv : initrd_desktop_argv;

        printf("[init] Starting graphical session\n");
        /* Startup chime (fire-and-forget; silent without an audio device) */
        if (file_available("/disk/chime.wav") || file_available("/chime.wav")) {
            int cpid = fork();
            if (cpid == 0) {
                char *wp = file_available("/disk/wavplay") ? "/disk/wavplay"
                                                           : "/wavplay";
                char *cw = file_available("/disk/chime.wav") ? "/disk/chime.wav"
                                                             : "/chime.wav";
                char *cargv[] = { wp, cw, 0 };
                execve(wp, cargv, (char *const *)envp);
                exit(1);
            }
        }
        /* Run the desktop as the unprivileged user (root keeps the
         * console).  envp carries USER=user / HOME=/home/user. */
        (void)envp;
        run_program_as_user(desktop_path, desktop_argv, user_envp);
        printf("[init] Graphical session ended; starting shell\n");
    } else {
        printf("[init] Graphics unavailable; starting shell\n");
    }

    monitor_children(command_shell_path, envp);
    return 0;
}
