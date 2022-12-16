#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"

/* flag to check if server is accepting clients */
int server_accept = 1; 

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
} server_control_t;

/* server control struct for whole server*/
server_control_t scontrol = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/* client control struct */
client_control_t ccontrol = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

/* initilizing thread list head and its mutex */
client_t *thread_list_head = NULL;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);

/*
client_control_wait: waits until the go mutex of client control is released by the client_control_wait function 
parameters: none
returns: none
*/
void client_control_wait() {
    int err;
    pthread_mutex_lock(&ccontrol.go_mutex);
    pthread_cleanup_push((void *) pthread_mutex_unlock, &ccontrol.go_mutex); 
    while (ccontrol.stopped == 1) {
        if ((err = pthread_cond_wait(&ccontrol.go, &ccontrol.go_mutex)) != 0) {
            handle_error_en(err, "pthread cond wait");    
        }
    }
    pthread_cleanup_pop(1);
}

/*
client_control_stop: stops all of the client threads
parameters: none
returns: none
*/
void client_control_stop() {
    pthread_mutex_lock(&ccontrol.go_mutex);
    ccontrol.stopped = 1;
    pthread_mutex_unlock(&ccontrol.go_mutex);
    printf("stopping all clients\n");
}

// Called by main thread to resume client threads
/*
client_control_release: releases all of the clients that are currentlly stopped
parameters: none
returns: none
*/
void client_control_release() {
    pthread_mutex_lock(&ccontrol.go_mutex);
    ccontrol.stopped = 0;
    int err;
    if ((err = pthread_cond_broadcast(&ccontrol.go)) != 0) {
            handle_error_en(err, "pthread cond broadcast");    
        }
    pthread_mutex_unlock(&ccontrol.go_mutex);
}

/*
client_constructor: 
parameters: the file cxstr of the client struct
returns: none
*/
void client_constructor(FILE *cxstr) {
    client_t *c;
    if ((c = (client_t *) malloc(sizeof(client_t))) == NULL) {
        printf("malloc error\n");
        return;
    } 

    c->cxstr = cxstr;
    c->prev = NULL;
    c->next = NULL;
    
    int err;
    if ((err = pthread_create(&c->thread, 0, run_client, c)) != 0) {
        handle_error_en(err, "pthread create");
    }
    
    if ((err = pthread_detach(c->thread)) != 0) {
        handle_error_en(err, "pthread detach");    
    }
}

/*
client_destructor: shutsdown and frees the client struct 
parameters: client struct
returns: none
*/
void client_destructor(client_t *client) {
    comm_shutdown(client->cxstr);
    free(client);
}

/*
run_client: runs during the whole lifetime of the client thread, recieving and sending messages to the file cxstr
parameters: void pointer argument, which is cast to a client pointer
returns: none (null pointer)
*/
void *run_client(void *arg) {
    client_t *c = (client_t *) arg;
    if (server_accept == 1) {

        pthread_mutex_lock(&thread_list_mutex);
        if (thread_list_head == NULL) {  
            thread_list_head = c;
            c->prev = c;
            c->next = c;
        } else if (thread_list_head->next == thread_list_head) {
            client_t *old = thread_list_head;
            thread_list_head = c;
            c->next = old;
            c->prev = old;
            old->next = c;
            old->prev = c;
        } else {
            client_t *old = thread_list_head;
            thread_list_head = c;
            c->prev = old->prev;
            c->next = old;
            old->prev->next = c;
            old->prev = c;   
        }
        pthread_mutex_unlock(&thread_list_mutex);

        pthread_mutex_lock(&scontrol.server_mutex);
        scontrol.num_client_threads++;
        pthread_mutex_unlock(&scontrol.server_mutex);
        
        pthread_cleanup_push(thread_cleanup, c);

        char response[512];
        char command[512];
        memset(response, 0, 512);
        memset(command, 0, 512);
        while(1) { 
            if (comm_serve(c->cxstr, response, command) == -1) { //get the command 
                break;
            }
            client_control_wait();
            interpret_command(command, response, 512); //get the response
        }
        pthread_cleanup_pop(1);
    } else {
        client_destructor(c);
    }
    return NULL;
}

/*
delete_all: deletes all of the client threads 
parameters: none
returns: none
*/
void delete_all() {
    client_t *cur = thread_list_head;
    if (scontrol.num_client_threads == 0) {
        return;
    }
    do { 
        int err; 
        if ((err = pthread_cancel(cur->thread)) != 0) { 
            handle_error_en(err, "pthread cancel"); 
        }
        cur = cur->next;
    } while (cur != thread_list_head);
}

/*
thread_cleanup: cleans up the client structs
parameters: void pointer argument, which is cast to a client pointer
returns: none
*/
void thread_cleanup(void *arg) {
    client_t *c = (client_t *) arg;
    pthread_mutex_lock(&thread_list_mutex);
    if (c->prev == c && c->next == c && c == thread_list_head) {
        thread_list_head = 0;
    } else {
        client_t *prev = c->prev;
        client_t *next = c->next;
        next->prev = prev;
        prev->next = next;
        if (thread_list_head == c) {
            thread_list_head = next;
        }
    }
    pthread_mutex_unlock(&thread_list_mutex);

    pthread_mutex_lock(&scontrol.server_mutex);
    scontrol.num_client_threads--; 
    pthread_mutex_unlock(&scontrol.server_mutex);
    int err;

    if (scontrol.num_client_threads == 0) {
        if ((err = pthread_cond_signal(&scontrol.server_cond)) != 0) {
            handle_error_en(err, "pthread cond signal");    
        }
    }
    client_destructor(c);
}

/*
monitor_signal: Code executed by the signal handler thread to do when SIGINT is recieved 
parameters: void pointer argument, which is cast to a sigset pointer
returns: none (null pointer)
*/
void *monitor_signal(void *arg) {
    sigset_t *set;
    set = (sigset_t *) arg;
    int sig;
    while(1) {
        if (sigwait(set, &sig) == 0) {
            if (sig == SIGINT) {
                if (scontrol.num_client_threads == 0) {
                    printf(" sigint received. no clients to cancel.\n");
                } else {
                    pthread_mutex_lock(&thread_list_mutex);
                    delete_all();
                    pthread_mutex_unlock(&thread_list_mutex);
                    printf(" sigint received. canceling all clients.\n");
                }
            }
        } else {
            printf("sigwait failed\n");
        }
    }
    return NULL;
}

/*
sig_handler_constructor: sets up the signal handler for SIGINT 
parameters: none
returns: sigint handler pointer
*/
sig_handler_t *sig_handler_constructor() {
    sig_handler_t *sigint_handler;
    sigint_handler = malloc(sizeof(sig_handler_t));
    if (sigint_handler == NULL) {
        printf("malloc error\n");
        return NULL;
    }
    if (sigemptyset(&sigint_handler->set) == -1) {
        printf("sigemptyset failed\n");
        return NULL;
    }
    if (sigaddset(&sigint_handler->set, SIGINT) == -1) {
        printf("sigaddset failed\n");
        return NULL;
    }
    if (pthread_sigmask(SIG_BLOCK, &sigint_handler->set, 0) != 0) {
        printf("pthread_sigmask failed\n");
        return NULL;
    }
    int err;
    if ((err = pthread_create(&sigint_handler->thread, 0, monitor_signal, &sigint_handler->set)) != 0) {
        handle_error_en(err, "pthread create");    
    } 
    return sigint_handler;
}

/*
sig_handler_destructor: destorys the sig handler by canceling its threads and freeing it
parameters: sig handler pointer
returns: none
*/
void sig_handler_destructor(sig_handler_t *sighandler) {
    int err;
    if ((err = pthread_cancel(sighandler->thread)) != 0) {
        handle_error_en(err, "pthread cancel");    
    }
    if ((err = pthread_join(sighandler->thread, 0)) != 0) {
        handle_error_en(err, "pthread join");    
    }
    free(sighandler);
}

/*
main: sets up sigint handler, starts the listener, reads commands from the STDIN until 
a signal is recieved or EOF, and then destroys the signal handler, stops accepting 
clients, deletes all the clients, cleans up the database, cancels the listener thread, 
and exits 
parameters: argc, which is the number of args in argv, and argv, which is an array of 
arguments
returns: an int 
*/
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("correct usage: ./server [port]\n");
        return 1;
    }
    sigset_t set;
    if (sigemptyset(&set) == -1) {
        printf("sigemptyset failed\n");
        return 1;
    }
    if (sigaddset(&set, SIGPIPE) == -1) {
        printf("sigaddset failed\n");
        return 1;
    }
    if (pthread_sigmask(SIG_BLOCK, &set, 0) != 0) {
        printf("pthread_sigmask failed\n");
        return 1;
    }
    sig_handler_t *sigh = sig_handler_constructor();

    pthread_t listener = start_listener(atoi(argv[1]), client_constructor);

    size_t MAX = 1024;
    char buffer[MAX];
    memset(buffer, 0, MAX);
    while (read(0, buffer, MAX) > 0) {
        if (strlen(buffer) > 1) {
            char bufz[1];
            char file[512];
            memset(file, 0, 512);
            sscanf(buffer, "%s %s", bufz, file);

            if (!strcmp(bufz, "s")) { 
                client_control_stop();
            } else if (!strcmp(bufz, "g")) {
                client_control_release();
                printf("releasing all clients\n");
            } else if (!strcmp(bufz, "p")) {
                db_print(file);
            } else {
                fprintf(stderr, "syntax error: please enter either s, g, or p\n");
            }
        }
    }
    sig_handler_destructor(sigh);
    pthread_mutex_lock(&thread_list_mutex);
    server_accept = 0;
    delete_all();
    pthread_mutex_unlock(&thread_list_mutex);

    pthread_mutex_lock(&scontrol.server_mutex);
    int err;
    while(scontrol.num_client_threads > 0) { 
        if ((err = pthread_cond_wait(&scontrol.server_cond, &scontrol.server_mutex)) != 0) {
            handle_error_en(err, "pthread cond wait"); 
        }
    } 
    pthread_mutex_unlock(&scontrol.server_mutex); 

    db_cleanup();

    if ((err = pthread_cancel(listener)) != 0) {
        handle_error_en(err, "pthread cancel"); 
    }
    if ((err = pthread_join(listener, 0)) != 0) {
        handle_error_en(err, "pthread join"); 
    }

    printf("exiting database\n");
    pthread_exit((void *) 0);

    return 0;
}