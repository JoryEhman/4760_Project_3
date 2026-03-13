#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>

#define BILLION 1000000000
#define QUANTUM 250000000

typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} SimClock;

typedef struct msgbuffer {
    long mtype;
    char strData[100];
    int intData;
} msgbuffer;

FILE *logfp;

void log_event(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    va_start(args, fmt);
    vfprintf(logfp, fmt, args);
    va_end(args);

    fflush(logfp);
}

void advance_clock(SimClock *clock)
{
    clock->nanoseconds += QUANTUM;

    if(clock->nanoseconds >= BILLION)
    {
        clock->seconds++;
        clock->nanoseconds -= BILLION;
    }
}

int main(int argc, char *argv[])
{
    int n = 1;
    int s = 1;
    int t = 1;
    char *logfile = "oss.log";

    int opt;

    while((opt = getopt(argc, argv, "n:s:t:f:")) != -1)
    {
        switch(opt)
        {
            case 'n': n = atoi(optarg); break;
            case 's': s = atoi(optarg); break;
            case 't': t = atoi(optarg); break;
            case 'f': logfile = optarg; break;
            default:
                fprintf(stderr,"Usage: %s [-n proc] [-s simul] [-t time] [-f logfile]\n",argv[0]);
                exit(1);
        }
    }

    logfp = fopen(logfile,"w");

    if(logfp == NULL)
    {
        perror("fopen");
        exit(1);
    }

    printf("OSS starting: n=%d s=%d t=%d logfile=%s\n",n,s,t,logfile);

    key_t shmkey = ftok(".",1);

    int shmid = shmget(shmkey,sizeof(SimClock),IPC_CREAT|0666);

    if(shmid < 0)
    {
        perror("shmget");
        exit(1);
    }

    SimClock *clock = (SimClock*) shmat(shmid,NULL,0);

    clock->seconds = 0;
    clock->nanoseconds = 0;

    key_t msgkey = ftok(".",2);

    int msqid = msgget(msgkey,IPC_CREAT|0666);

    if(msqid < 0)
    {
        perror("msgget");
        exit(1);
    }

    pid_t pid = fork();

    if(pid == 0)
    {
        char shmid_str[20];
        char msqid_str[20];
        char runtime_str[20];

        sprintf(shmid_str,"%d",shmid);
        sprintf(msqid_str,"%d",msqid);
        sprintf(runtime_str,"%d",t);

        execl("./worker","worker",shmid_str,msqid_str,runtime_str,(char*)NULL);

        perror("exec failed");
        exit(1);
    }

    msgbuffer msg;

    while(1)
    {
        msg.mtype = pid;
        msg.intData = 1;

        log_event("OSS: Sending message to worker %d at %u:%u\n",
        pid,clock->seconds,clock->nanoseconds);

        msgsnd(msqid,&msg,sizeof(msgbuffer)-sizeof(long),0);

        msgrcv(msqid,&msg,sizeof(msgbuffer)-sizeof(long),getpid(),0);

        log_event("OSS: Received message from worker %d\n",pid);

        if(msg.intData == 0)
        {
            log_event("OSS: Worker %d terminating at %u:%u\n",
            pid,clock->seconds,clock->nanoseconds);

            break;
        }

        advance_clock(clock);
    }

    wait(NULL);

    shmdt(clock);
    shmctl(shmid,IPC_RMID,NULL);

    msgctl(msqid,IPC_RMID,NULL);

    fclose(logfp);

    printf("OSS finished\n");

    return 0;
}