#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>

#define BILLION 1000000000

typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} SimClock;

typedef struct msgbuffer {
    long mtype;
    char strData[100];
    int intData;
} msgbuffer;

int main(int argc,char *argv[])
{
    int shmid = atoi(argv[1]);
    int msqid = atoi(argv[2]);
    int runtime = atoi(argv[3]);

    SimClock *clock = (SimClock*) shmat(shmid,NULL,0);

    unsigned int end_sec = clock->seconds + runtime;
    unsigned int end_ns = clock->nanoseconds;

    msgbuffer msg;

    int messages = 0;

    printf("WORKER PID:%d PPID:%d SysClockS:%u SysClockNano:%u\n",
    getpid(),getppid(),clock->seconds,clock->nanoseconds);

    printf("TermTimeS:%u TermTimeNano:%u\n",end_sec,end_ns);

    printf("--Just Starting\n");

    while(1)
    {
        msgrcv(msqid,&msg,sizeof(msgbuffer)-sizeof(long),getpid(),0);

        messages++;

        printf("WORKER PID:%d PPID:%d SysClockS:%u SysClockNano:%u\n",
        getpid(),getppid(),clock->seconds,clock->nanoseconds);

        printf("TermTimeS:%u TermTimeNano:%u\n",end_sec,end_ns);

        printf("--%d messages received from oss\n",messages);

        if(clock->seconds > end_sec ||
          (clock->seconds == end_sec && clock->nanoseconds >= end_ns))
        {
            msg.mtype = getppid();
            msg.intData = 0;

            msgsnd(msqid,&msg,sizeof(msgbuffer)-sizeof(long),0);

            printf("--Terminating after %d messages\n",messages);

            break;
        }

        msg.mtype = getppid();
        msg.intData = 1;

        msgsnd(msqid,&msg,sizeof(msgbuffer)-sizeof(long),0);
    }

    shmdt(clock);

    return 0;
}