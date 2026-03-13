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

#define BILLION 1000000000ULL
#define HALF_SECOND 500000000ULL
#define MAX_PROCS 20
#define QUANTUM 250000000U

typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} SimClock;

typedef struct msgbuffer {
    long mtype;
    int intData;
} msgbuffer;

typedef struct {
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNano;
    int endingTimeSeconds;
    int endingTimeNano;
    int messagesSent;
} PCB;

PCB processTable[MAX_PROCS];

SimClock *simClock = NULL;
int shmid = -1;
int msqid = -1;
FILE *logfp = NULL;

unsigned int lastPrintSec = 0;
unsigned int lastPrintNano = 0;

int totalProcessesLaunched = 0;
int totalMessagesSent = 0;

/* NEW FOR -i */
unsigned long long launchIntervalNs = 0;
unsigned long long lastLaunchTimeNs = 0;

static unsigned long long sim_time_ns(void)
{
    return ((unsigned long long)simClock->seconds * BILLION) +
           (unsigned long long)simClock->nanoseconds;
}

static void advance_clock(int runningChildren)
{
    if (runningChildren <= 0)
        runningChildren = 1;

    unsigned int increment = QUANTUM / (unsigned int)runningChildren;

    simClock->nanoseconds += increment;

    while (simClock->nanoseconds >= BILLION) {
        simClock->seconds++;
        simClock->nanoseconds -= BILLION;
    }
}

static void log_event(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (logfp != NULL) {
        va_start(args, fmt);
        vfprintf(logfp, fmt, args);
        va_end(args);
        fflush(logfp);
    }
}

static void cleanup(int sig)
{
    (void)sig;

    for (int i = 0; i < MAX_PROCS; i++) {
        if (processTable[i].occupied)
            kill(processTable[i].pid, SIGKILL);
    }

    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    if (simClock)
        shmdt(simClock);

    if (shmid > 0)
        shmctl(shmid, IPC_RMID, NULL);

    if (msqid > 0)
        msgctl(msqid, IPC_RMID, NULL);

    if (logfp)
        fclose(logfp);

    exit(0);
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!processTable[i].occupied)
            return i;
    }

    return -1;
}

static void clear_pcb(int i)
{
    processTable[i].occupied = 0;
    processTable[i].pid = 0;
    processTable[i].startSeconds = 0;
    processTable[i].startNano = 0;
    processTable[i].endingTimeSeconds = 0;
    processTable[i].endingTimeNano = 0;
    processTable[i].messagesSent = 0;
}

static void print_process_table(void)
{
    log_event("\nOSS PID:%d SysClockS:%u SysClockNano:%u\n",
              getpid(), simClock->seconds, simClock->nanoseconds);

    log_event("%-5s %-8s %-8s %-10s %-12s %-10s %-12s %-10s\n",
              "Entry", "Occupied", "PID", "StartS", "StartN", "EndS", "EndN", "MsgSent");

    for (int i = 0; i < MAX_PROCS; i++) {
        PCB *p = &processTable[i];

        log_event("%-5d %-8d %-8d %-10d %-12d %-10d %-12d %-10d\n",
                  i,
                  p->occupied,
                  p->pid,
                  p->startSeconds,
                  p->startNano,
                  p->endingTimeSeconds,
                  p->endingTimeNano,
                  p->messagesSent);
    }
}

static int half_second_passed(void)
{
    unsigned long long now =
        ((unsigned long long)simClock->seconds * BILLION) +
        (unsigned long long)simClock->nanoseconds;

    unsigned long long last =
        ((unsigned long long)lastPrintSec * BILLION) +
        (unsigned long long)lastPrintNano;

    if (now - last >= HALF_SECOND) {
        lastPrintSec = simClock->seconds;
        lastPrintNano = simClock->nanoseconds;
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int n = 1;
    int s = 1;
    int t = 1;
    double i_value = 0.0;
    char *logfile = "oss.log";

    srand((unsigned int)getpid());

    int opt;
    int launched = 0;
    int running = 0;

    ssize_t msgsz = sizeof(msgbuffer) - sizeof(long);

    while ((opt = getopt(argc, argv, "n:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'n': n = atoi(optarg); break;
            case 's': s = atoi(optarg); break;
            case 't': t = atoi(optarg); break;
            case 'i': i_value = atof(optarg); break;
            case 'f': logfile = optarg; break;
            default:
                fprintf(stderr,"Usage: %s [-n proc] [-s simul] [-t timelimit] [-i launchInterval] [-f logfile]\n",argv[0]);
                return 1;
        }
    }

    if (n < 1) n = 1;
    if (s < 1) s = 1;
    if (s > MAX_PROCS) s = MAX_PROCS;
    if (t < 1) t = 1;
    if (i_value < 0.0) i_value = 0.0;

    launchIntervalNs = (unsigned long long)(i_value * (double)BILLION);

    for (int i = 0; i < MAX_PROCS; i++)
        clear_pcb(i);

    logfp = fopen(logfile,"w");
    if (!logfp) {
        perror("fopen");
        return 1;
    }

    signal(SIGINT, cleanup);
    signal(SIGALRM, cleanup);
    alarm(60);

    key_t shmkey = ftok(".",1);
    shmid = shmget(shmkey,sizeof(SimClock),IPC_CREAT|0666);
    simClock = (SimClock*)shmat(shmid,NULL,0);

    simClock->seconds = 0;
    simClock->nanoseconds = 0;

    key_t msgkey = ftok(".",2);
    msqid = msgget(msgkey,IPC_CREAT|0666);

    while (launched < n || running > 0)
    {
        if (running > 0)
            advance_clock(running);

        if (half_second_passed())
            print_process_table();

        if (launched < n && running < s)
        {
            unsigned long long nowNs = sim_time_ns();
            int canLaunch = 0;

            if (launched == 0)
                canLaunch = 1;
            else if (nowNs - lastLaunchTimeNs >= launchIntervalNs)
                canLaunch = 1;

            if (canLaunch)
            {
                int slot = find_free_slot();

                if (slot != -1)
                {
                    int randSeconds = (rand() % t) + 1;
                    int randNano = rand() % (int)BILLION;

                    pid_t pid = fork();

                    if (pid == 0)
                    {
                        char shmid_str[20];
                        char msqid_str[20];
                        char sec_str[20];
                        char nano_str[20];

                        sprintf(shmid_str,"%d",shmid);
                        sprintf(msqid_str,"%d",msqid);
                        sprintf(sec_str,"%d",randSeconds);
                        sprintf(nano_str,"%d",randNano);

                        execl("./worker","worker",
                              shmid_str,msqid_str,sec_str,nano_str,(char*)NULL);

                        exit(1);
                    }

                    PCB *p = &processTable[slot];

                    p->occupied = 1;
                    p->pid = pid;
                    p->startSeconds = simClock->seconds;
                    p->startNano = simClock->nanoseconds;

                    p->endingTimeSeconds = simClock->seconds + randSeconds;
                    p->endingTimeNano = simClock->nanoseconds + randNano;

                    if (p->endingTimeNano >= BILLION) {
                        p->endingTimeSeconds++;
                        p->endingTimeNano -= BILLION;
                    }

                    p->messagesSent = 0;

                    launched++;
                    running++;
                    totalProcessesLaunched++;
                    lastLaunchTimeNs = sim_time_ns();

                    print_process_table();
                }
            }
        }

        for (int i = 0; i < MAX_PROCS; i++)
        {
            PCB *p = &processTable[i];
            msgbuffer msg;

            if (!p->occupied)
                continue;

            memset(&msg,0,sizeof(msg));
            msg.mtype = p->pid;
            msg.intData = 1;

            log_event("OSS: Sending message to worker entry %d PID %d at time %u:%u\n",
                      i, p->pid, simClock->seconds, simClock->nanoseconds);

            msgsnd(msqid,&msg,msgsz,0);

            msgrcv(msqid,&msg,msgsz,getpid(),0);

            log_event("OSS: Receiving message from worker entry %d PID %d at time %u:%u\n",
                      i, p->pid, simClock->seconds, simClock->nanoseconds);

            p->messagesSent++;
            totalMessagesSent++;

            if (msg.intData == 0)
            {
                log_event("OSS: Worker entry %d PID %d is planning to terminate\n",
                          i, p->pid);

                waitpid(p->pid,NULL,0);

                clear_pcb(i);
                running--;

                break;
            }
        }
    }

    log_event("\nOSS Summary Report\n");
    log_event("Total processes launched: %d\n", totalProcessesLaunched);
    log_event("Total messages sent: %d\n", totalMessagesSent);

    cleanup(0);
}