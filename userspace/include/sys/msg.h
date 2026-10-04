#pragma once
#include <sys/ipc.h>
#include <stddef.h>

#define MSG_NOERROR 010000
#define MSG_EXCEPT  020000
#define MSG_COPY    040000
#define MSG_STAT    11
#define MSG_INFO    12
#define MSG_STAT_ANY 13

typedef unsigned long msgqnum_t;
typedef unsigned long msglen_t;

struct msqid_ds {
    struct ipc_perm msg_perm;
    unsigned long msg_stime, __msg_stime_hi;
    unsigned long msg_rtime, __msg_rtime_hi;
    unsigned long msg_ctime, __msg_ctime_hi;
    unsigned long msg_cbytes;
    msgqnum_t msg_qnum;
    msglen_t msg_qbytes;
    pid_t msg_lspid, msg_lrpid;
    unsigned long __unused4, __unused5;
};

struct msginfo {
    int msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz, msgtql;
    unsigned short msgseg;
};

int msgget(key_t key, int flag);
int msgsnd(int id, const void *msg, size_t size, int flag);
ssize_t msgrcv(int id, void *msg, size_t size, long type, int flag);
int msgctl(int id, int cmd, struct msqid_ds *buf);
