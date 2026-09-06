#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"

#define MAX_PROCS 64
#define MAX_SERVICE_LINES 32
#define LINE_MAX_LEN 256
#define SERVICE_NAME_MAX 32
#define SERVICE_CMD_MAX 180
#define PROC_NAME_MAX 32

typedef struct proc_entry {
    int pid;
    char state;
    char name[PROC_NAME_MAX];
} proc_entry_t;

typedef struct service_entry {
    int found;
    int respawn;
    int enabled;
    char type[SERVICE_NAME_MAX];
    char name[SERVICE_NAME_MAX];
    char command[SERVICE_CMD_MAX];
} service_entry_t;

typedef struct runtime_entry {
    int found;
    int pid;
    char state[16];
    char boot[16];
    char command[SERVICE_CMD_MAX];
} runtime_entry_t;

static proc_entry_t procs[MAX_PROCS];
static int proc_count;

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

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return base;
}

static void read_processes(void) {
    FILE *f = fopen("/proc/processes", "r");
    if (!f) return;

    proc_count = 0;
    char line[160];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        if (first) {
            first = 0;
            continue;
        }
        trim_line(line);

        char *p = line;
        char *pid_s = next_word(&p);
        next_word(&p); /* ppid */
        next_word(&p); /* pgrp */
        next_word(&p); /* sid */
        char *state_s = next_word(&p);
        next_word(&p); /* tty */
        char *name = next_word(&p);

        if (!pid_s || !state_s || !name || proc_count >= MAX_PROCS) continue;

        procs[proc_count].pid = atoi(pid_s);
        procs[proc_count].state = state_s[0];
        strncpy(procs[proc_count].name, name, PROC_NAME_MAX - 1);
        procs[proc_count].name[PROC_NAME_MAX - 1] = '\0';
        proc_count++;
    }

    fclose(f);
}

static proc_entry_t *find_process(const char *command) {
    char cmd[SERVICE_CMD_MAX];
    strncpy(cmd, command, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    char *p = cmd;
    char *first = next_word(&p);
    if (!first) return (proc_entry_t *)0;

    const char *want = base_name(first);
    for (int i = 0; i < proc_count; i++) {
        if (procs[i].state == 'Z') continue;
        if (strcmp(procs[i].name, want) == 0) return &procs[i];
    }
    return (proc_entry_t *)0;
}

static int parse_service_line(char *line, service_entry_t *svc) {
    memset(svc, 0, sizeof(*svc));
    svc->enabled = 1;

    char *p = skip_space(line);
    if (!*p || *p == '#') return 0;

    char *type = next_word(&p);
    char *name = next_word(&p);
    if (!type || !name) return 0;

    strncpy(svc->type, type, sizeof(svc->type) - 1);
    strncpy(svc->name, name, sizeof(svc->name) - 1);
    svc->respawn = strcmp(type, "respawn") == 0;

    if (svc->respawn) {
        char *first = next_word(&p);
        if (!first) return 0;
        char *command = first;

        if (strcmp(first, "enabled") == 0 || strcmp(first, "disabled") == 0) {
            svc->enabled = strcmp(first, "disabled") != 0;
            command = skip_space(p);
        }

        if (!*command) return 0;
        strncpy(svc->command, command, sizeof(svc->command) - 1);
    } else {
        char *command = skip_space(p);
        if (!*command) return 0;
        strncpy(svc->command, command, sizeof(svc->command) - 1);
    }

    svc->found = 1;
    return 1;
}

static int lookup_service(const char *svc_name, service_entry_t *out) {
    FILE *f = fopen("/etc/services", "r");
    if (!f) return 0;

    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        service_entry_t svc;
        if (!parse_service_line(line, &svc)) continue;
        if (svc.respawn && strcmp(svc.name, svc_name) == 0) {
            *out = svc;
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

static int lookup_runtime_status(const char *svc_name, runtime_entry_t *out) {
    FILE *f = fopen("/tmp/services.status", "r");
    if (!f) return 0;

    memset(out, 0, sizeof(*out));
    char line[LINE_MAX_LEN];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        if (first) {
            first = 0;
            continue;
        }

        trim_line(line);
        char *p = line;
        char *name = next_word(&p);
        char *type = next_word(&p);
        char *boot = next_word(&p);
        char *pid_s = next_word(&p);
        char *state = next_word(&p);
        char *command = skip_space(p);

        if (!name || !type || !boot || !pid_s || !state || !*command) continue;
        if (strcmp(type, "respawn") != 0 || strcmp(name, svc_name) != 0) continue;

        out->found = 1;
        out->pid = atoi(pid_s);
        strncpy(out->boot, boot, sizeof(out->boot) - 1);
        strncpy(out->state, state, sizeof(out->state) - 1);
        strncpy(out->command, command, sizeof(out->command) - 1);
        fclose(f);
        return 1;
    }

    fclose(f);
    return 0;
}

static int service_live(const service_entry_t *svc, int *pid_out) {
    runtime_entry_t rt;
    if (lookup_runtime_status(svc->name, &rt)) {
        if (pid_out) *pid_out = rt.pid;
        return strcmp(rt.state, "running") == 0 && rt.pid > 0;
    }

    read_processes();
    proc_entry_t *proc = find_process(svc->command);
    if (pid_out) *pid_out = proc ? proc->pid : -1;
    return proc != (proc_entry_t *)0;
}

static int send_control(const char *cmd, const char *name) {
    service_entry_t svc;
    int old_pid = -1;
    int has_service = lookup_service(name, &svc);
    int wait_restart = strcmp(cmd, "restart") == 0 && has_service;
    int wait_start = strcmp(cmd, "start") == 0 && has_service;
    int wait_stop = strcmp(cmd, "stop") == 0 && has_service;

    if (!has_service) {
        printf("svc: unknown respawn service %s\n", name);
        return 1;
    }

    if (wait_restart || wait_stop) {
        service_live(&svc, &old_pid);
    }

    int fd = open("/tmp/initctl", O_WRONLY);
    if (fd < 0) {
        puts("svc: cannot open /tmp/initctl");
        return 1;
    }

    char line[96];
    sprintf(line, "%s %s\n", cmd, name);
    write(fd, line, strlen(line));
    close(fd);

    if (wait_restart || wait_start || wait_stop) {
        for (int i = 0; i < 1000; i++) {
            int pid = -1;
            int live = service_live(&svc, &pid);
            if (wait_stop && !live) {
                printf("stopped %s\n", name);
                return 0;
            }
            if (wait_start && live) {
                printf("started %s\n", name);
                return 0;
            }
            if (wait_restart && live && pid != old_pid) {
                printf("restarted %s\n", name);
                return 0;
            }
            sched_yield();
        }
    }

    printf("%s %s\n", cmd, name);
    return 0;
}

static int rewrite_service_state(const char *name, int enabled) {
    char lines[MAX_SERVICE_LINES][LINE_MAX_LEN];
    int count = 0;
    int changed = 0;

    FILE *f = fopen("/etc/services", "r");
    if (!f) {
        puts("svc: no /etc/services");
        return 1;
    }

    while (count < MAX_SERVICE_LINES && fgets(lines[count], sizeof(lines[count]), f))
        count++;
    fclose(f);

    for (int i = 0; i < count; i++) {
        char copy[LINE_MAX_LEN];
        strncpy(copy, lines[i], sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        trim_line(copy);

        service_entry_t svc;
        if (!parse_service_line(copy, &svc)) continue;
        if (!svc.respawn || strcmp(svc.name, name) != 0) continue;

        sprintf(lines[i], "%s %s %s %s\n",
                svc.type, svc.name, enabled ? "enabled" : "disabled", svc.command);
        changed = 1;
        break;
    }

    if (!changed) {
        printf("svc: unknown respawn service %s\n", name);
        return 1;
    }

    int fd = open("/etc/services", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        puts("svc: cannot update /etc/services");
        return 1;
    }

    for (int i = 0; i < count; i++)
        write(fd, lines[i], strlen(lines[i]));
    close(fd);

    return send_control(enabled ? "start" : "stop", name);
}

int main(int argc, char *argv[]) {
    if (argc == 3 &&
        (strcmp(argv[1], "start") == 0 ||
         strcmp(argv[1], "stop") == 0 ||
         strcmp(argv[1], "restart") == 0))
        return send_control(argv[1], argv[2]);

    if (argc == 3 &&
        (strcmp(argv[1], "enable") == 0 ||
         strcmp(argv[1], "disable") == 0)) {
        int enabled = strcmp(argv[1], "enable") == 0;
        return rewrite_service_state(argv[2], enabled);
    }

    if (argc != 1) {
        puts("usage: svc [start|stop|restart|enable|disable NAME]");
        return 1;
    }

    FILE *f = fopen("/etc/services", "r");
    if (!f) {
        puts("svc: no /etc/services");
        return 1;
    }

    read_processes();
    puts("NAME TYPE BOOT PID STATE COMMAND");

    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        service_entry_t svc;
        if (!parse_service_line(line, &svc)) continue;

        if (svc.respawn) {
            runtime_entry_t rt;
            if (lookup_runtime_status(svc.name, &rt)) {
                if (rt.pid > 0) {
                    printf("%s %s %s %d %s %s\n",
                           svc.name, svc.type, rt.boot, rt.pid, rt.state, rt.command);
                } else {
                    printf("%s %s %s - %s %s\n",
                           svc.name, svc.type, rt.boot, rt.state, rt.command);
                }
                continue;
            }

            proc_entry_t *proc = find_process(svc.command);
            if (proc) {
                printf("%s %s %s %d running %s\n",
                       svc.name, svc.type, svc.enabled ? "enabled" : "disabled",
                       proc->pid, svc.command);
            } else {
                printf("%s %s %s - stopped %s\n",
                       svc.name, svc.type, svc.enabled ? "enabled" : "disabled",
                       svc.command);
            }
        } else {
            printf("%s %s - - configured %s\n", svc.name, svc.type, svc.command);
        }
    }

    fclose(f);
    return 0;
}
