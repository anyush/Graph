#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

#define USR_MSG_LEN 10
#define CONN_WAIT 1 // time in seconds for conn command to find connection

volatile sig_atomic_t last_signal = 0;

typedef struct timespec timespec_t;

typedef struct msg
{
    timespec_t creation_time;
    char command;
    int sender;
    int receiver;
} msg_t;

typedef struct checker_data
{
    msg_t msg;
    int descr;
} checker_data_t;

int sethandler(void (*)(int), int);
void sig_handler(int);
void sigchld_handler(int);
void usage(char*);
void read_args(int, char**, int*);
void parent_work(int, int, int*);
void* check_con(void*);
void child_work(int, int, int, int*, int*);
void create_children_and_pipes(int, int*);
int main(int, char**);

int sethandler( void (*f)(int), int sigNo) {
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	if (-1==sigaction(sigNo, &act, NULL))
		return -1;
	return 0;
}

void sig_handler(int sig) {
	last_signal = sig;
}

void sigchld_handler(int sig) {
	pid_t pid;
	for(;;){
		pid=waitpid(0, NULL, WNOHANG);
		if(0==pid) return;
		if(0>=pid) {
			if(ECHILD==errno) return;
			ERR("waitpid1:");
		}
	}
}

void usage(char *name){
	fprintf(stderr,"USAGE: %s [-n liczba_wierzcholkow]\n", name);
	exit(EXIT_FAILURE);
}

void read_args(int argc, char** argv, int* n)
{
	int buf;
    *n = -1;
	
	while ((buf = getopt (argc, argv, "n:")) != -1)
	{
		switch(buf)
		{
			case 'n':
                *n = atoi(optarg);
				break;
			default:
				usage(argv[0]);
		}
	}

    if (*n < 0)
        usage(argv[0]);	
}

void parent_work(int fifo, int n, int* to_vertices){
	char buf[USR_MSG_LEN];
    int64_t count;
    msg_t msg;
    memset(buf, 0, USR_MSG_LEN);

    if(sethandler(sig_handler,SIGINT)) ERR("Setting SIGINT");

	do{
        if (last_signal == SIGINT)
            break;
        count = read(fifo, buf + USR_MSG_LEN - 1, sizeof(char));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) ERR("read");

        if (strstr(buf, "print") == buf)
        {
            printf("Krawedzie:\n");
            msg.command = 'p';
            for (int i = 0; i < n; i++)
                if (TEMP_FAILURE_RETRY(write(to_vertices[i], &msg, sizeof(msg_t))) < 0) ERR("write");
        }
        else if (strstr(buf, "add") == buf)
        {
            int x = atoi(buf + 3);
            int y = atoi(strstr(buf + 4, " "));

            msg.command = 'a';
            msg.sender = x;
            msg.receiver = y;

            if (TEMP_FAILURE_RETRY(write(to_vertices[x], &msg, sizeof(msg_t))) < 0) ERR("write");
        }
        else if (strstr(buf, "conn") == buf)
        {
            int x = atoi(buf + 4);
            int y = atoi(strstr(buf + 5, " "));

            clock_gettime(CLOCK_REALTIME, &msg.creation_time);
            msg.command = 'c';
            msg.sender = x;
            msg.receiver = y;

            if (TEMP_FAILURE_RETRY(write(to_vertices[x], &msg, sizeof(msg_t))) < 0) ERR("write");

            checker_data_t* chkd = (checker_data_t*) malloc(sizeof(checker_data_t));
            chkd->msg.creation_time.tv_sec = msg.creation_time.tv_sec;
            chkd->msg.creation_time.tv_nsec = msg.creation_time.tv_nsec;
            chkd->msg.command = 'n';
            chkd->msg.sender = x;
            chkd->descr = to_vertices[y];
            
            pthread_t thread_id;
            pthread_attr_t thread_attr;
	        if (pthread_attr_init(&thread_attr)) ERR("pthread_attr_init");
	        if (pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED)) ERR("pthread_attr_setdetach_state");
            pthread_create(&thread_id, &thread_attr, check_con, chkd);
        }
        memcpy(buf, buf + 1, USR_MSG_LEN - 1);
	} while(count > 0);
}

void* check_con(void* void_chkd_data)
{
    checker_data_t* chkd = (checker_data_t*) void_chkd_data;
    sleep(CONN_WAIT);
    if (TEMP_FAILURE_RETRY(write(chkd->descr, &(chkd->msg), sizeof(msg_t))) < 0) ERR("write");
    free(chkd);
    return NULL;
}

void child_work(int id, int n, int in_pipe, int* to_vertices, int* connected)
{
    int64_t count;
    int alr = 0;
    msg_t msg;
    if(sethandler(sig_handler,SIGINT)) ERR("Setting SIGINT");
    timespec_t* last_conn = (timespec_t*) malloc(n*sizeof(timespec_t));
    memset(last_conn, 0, n*sizeof(timespec_t));

    while (1)
    {
        if (last_signal == SIGINT)
            break;		
        count = read(in_pipe, (void*)&msg + alr, sizeof(msg_t) - alr);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) ERR("read");
        alr += count;
        if (alr < sizeof(msg_t)) continue;

        switch (msg.command)
        {
            case 'p': // print
                for (int i = 0; i < n; i++)
                    if (connected[i])
                        printf("[%i] -> [%i]\n", id, i);
                break;
            
            case 'a': // add
                if (msg.receiver != id)
                {
                    if (connected[msg.receiver] == 0)
                    {
                        connected[msg.receiver] = 1;
                        printf("[%i] -> [%i] created\n", msg.sender, id);
                    }
                    else
                        printf("[%i] -> [%i] already created\n", msg.sender, id);
                }
                else
                    printf("node can't be connected with itself\n");
                    
                break;

            case 'c': // conn
                if (msg.creation_time.tv_sec == last_conn[msg.sender].tv_sec &&
                    msg.creation_time.tv_nsec == last_conn[msg.sender].tv_nsec)
                    break;
                last_conn[msg.sender] = msg.creation_time;

                if (msg.receiver == id && (msg.sender != id || connected[id]))
                {
                    printf("[%i] is connected to [%i]!\n", msg.sender, id);
                    break;
                }

                for (int i = 0; i < n; i++)
                    if (connected[i])
                        if (TEMP_FAILURE_RETRY(write(to_vertices[i], &msg, sizeof(msg_t))) < 0) ERR("write");
                break;

            case 'n': // not connected
                if (msg.creation_time.tv_sec != last_conn[msg.sender].tv_sec ||
                    msg.creation_time.tv_nsec != last_conn[msg.sender].tv_nsec)
                    printf("[%i] is not connected to [%i]!\n", msg.sender, id);
                break;
        }

        alr = 0;
    }
}

void create_children_and_pipes(int n,int* to_vertices)
{
    int tmpfd[n][2];
    pid_t id;
    int* connected = (int*) malloc(n*sizeof(int));;

    for (int i = 0; i < n; i++)
    {
        if (pipe(tmpfd[i])) ERR("pipe");
    }

    for (int i = 0; i < n; i++)
    {
        switch (id = fork())
        {
            case 0:
                for (int j = 0; j < i; j++)
                {
                    if (TEMP_FAILURE_RETRY(close(tmpfd[j][0]))) ERR("close");
                    to_vertices[j] = tmpfd[j][1];
                    connected[j] = rand()%2;
                }
                if (TEMP_FAILURE_RETRY(close(tmpfd[i][1]))) ERR("close");
                connected[i] = 0;
                for (int j = i + 1; j < n; j++)
                {
                    if (TEMP_FAILURE_RETRY(close(tmpfd[j][0]))) ERR("close");
                    to_vertices[j] = tmpfd[j][1];
                    connected[j] = 0;
                }

                child_work(i, n, tmpfd[i][0], to_vertices, connected);

                for (int j = 0; j < i; j++)
                    if (TEMP_FAILURE_RETRY(close(tmpfd[j][1]))) ERR("close");
                if (TEMP_FAILURE_RETRY(close(tmpfd[i][0]))) ERR("close");
                for (int j = i + 1; j < n; j++)
                    if (TEMP_FAILURE_RETRY(close(tmpfd[j][1]))) ERR("close");
                free(to_vertices);
                free(connected);
                exit(EXIT_SUCCESS);
            
            case -1:
                ERR("fork");
        }
    }

    free(connected);
    for (int i = 0; i < n; i++)
        to_vertices[i] = tmpfd[i][1];
}

int main(int argc, char** argv) {
	int fifo, n;
    int* to_vertices;
	read_args(argc, argv, &n);
    to_vertices = (int*)malloc(n*sizeof(int));
	if(sethandler(SIG_IGN,SIGPIPE)) ERR("Setting SIGPIPE handler");
	if(sethandler(sigchld_handler,SIGCHLD)) ERR("Setting parent SIGCHLD:");
    if (access("graph.fifo", F_OK) == 0)
    {
        if (unlink("graph.fifo") < 0) ERR("rm fifo");
    }
	if (mkfifo("graph.fifo", S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) < 0) ERR("create fifo");
    if ((fifo = open("graph.fifo",O_RDONLY)) < 0) ERR("open");
    create_children_and_pipes(n, to_vertices);
    parent_work(fifo, n, to_vertices);
    kill(0, SIGINT);
	for (int i = 0; i < n; i++)
        if(close(to_vertices[i])<0) ERR("close");
    free(to_vertices);
	if(close(fifo)<0) ERR("close fifo:");
    pid_t pid;
    while(1)
    {
		pid=waitpid(0, NULL, 0);
		if(0>=pid) {
			if(ECHILD==errno) break;
			ERR("waitpid2:");
		}
	}
	return EXIT_SUCCESS;
}
