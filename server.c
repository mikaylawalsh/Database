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

int server_accept = 1; //make thread safe! lock before altering 
pthread_mutex_t server_accept_mutex = PTHREAD_MUTEX_INITIALIZER;

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

server_control_t scontrol = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
// scontrol->server_mutex = PTHREAD_MUTEX_INITIALIZER;
// scontrol->server_cond = PTHREAD_COND_INITIALIZER;
// scontrol->num_client_threads = 0;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

client_control_t ccontrol = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
// ccontrol->go_mutex = PTHREAD_MUTEX_INITIALIZER;
// ccontrol->go = PTHREAD_COND_INITIALIZER;
// cclient->stopped = 0; c0 when not stopped, 1 when stopped

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

client_t *thread_list_head = NULL; //make list circular and doubly-linked 
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);

// Called by client threads to wait until progress is permitted
void client_control_wait() {
    // TODO: Block the calling thread until the main thread calls
    // client_control_release(). See the client_control_t struct.
    pthread_mutex_lock(&ccontrol.go_mutex);
    pthread_cleanup_push((void *) pthread_mutex_unlock, &ccontrol.go_mutex);
    while (ccontrol.stopped == 1) {
        pthread_cond_wait(&ccontrol.go, &ccontrol.go_mutex); //when to lock
    }
    pthread_cleanup_pop(1);
}

// Called by main thread to stop client threads
void client_control_stop() {
    // TODO: Ensure that the next time client threads call client_control_wait()
    // at the top of the event loop in run_client, they will block.

    pthread_mutex_lock(&ccontrol.go_mutex);
    ccontrol.stopped = 1;
    pthread_mutex_unlock(&ccontrol.go_mutex);
}

// Called by main thread to resume client threads
void client_control_release() {
    // TODO: Allow clients that are blocked within client_control_wait()
    // to continue. See the client_control_t struct.

    //need to lock? 
    pthread_mutex_lock(&ccontrol.go_mutex);
    ccontrol.stopped = 0;
    pthread_cond_broadcast(&ccontrol.go);
    pthread_mutex_unlock(&ccontrol.go_mutex);
}

// Called by listener (in comm.c) to create a new client thread
void client_constructor(FILE *cxstr) {
    // You should create a new client_t struct here and initialize ALL
    // of its fields. Remember that these initializations should be
    // error-checked.

    // TODO:
    // Step 1: Allocate memory for a new client and set its connection stream
    // to the input argument.
    // Step 2: Create the new client thread running the run_client routine.
    // Step 3: Detach the new client thread
    
    client_t *c;
    if ((c = (client_t *) malloc(sizeof(client_t))) == NULL) {
        printf("malloc error");
    } 
    
    int err;
    if ((err = pthread_create(&c->thread, 0, run_client, c)) != 0) {
        handle_error_en(err, "pthread create");
    }
    
    c->cxstr = cxstr;
    c->prev = NULL;
    c->next = NULL;
    if ((err = pthread_detach(c->thread)) != 0) {
        handle_error_en(err, "pthread detach");    
    }
}

void client_destructor(client_t *client) {
    // TODO: Free and close all resources associated with a client.
    // Whatever was malloc'd in client_constructor should
    // be freed here!

    comm_shutdown(client->cxstr);

    free(client);
}

// Code executed by a client thread
void *run_client(void *arg) {
    // TODO:
    // Step 1: Make sure that the server is still accepting clients. This will 
    //         will make sense when handling EOF for the server. 
    // Step 2: Add client to the client list and push thread_cleanup to remove
    //       it if the thread is canceled.
    // Step 3: Loop comm_serve (in comm.c) to receive commands and output
    //       responses. Execute commands using interpret_command (in db.c)   
    // Step 4: When the client is done sending commands, exit the thread
    //       cleanly.
    //
    // You will need to modify this when implementing functionality for stop and go!

    //make sure server is still accepting clients
    client_t *c = (client_t *) arg;
    if (server_accept == 1) {

        pthread_mutex_lock(&thread_list_mutex);
        if (thread_list_head == NULL) { //empty list 
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
            interpret_command(command, response, 512); //gets the response
        }

        pthread_cleanup_pop(1); //not sure what to pass in

        //return c; //what to return 
    } else {
        client_destructor(c);
    }
    return NULL;
}

void delete_all() {
    // TODO: Cancel every thread in the client thread list with the
    // pthread_cancel function.
    client_t *cur = thread_list_head;
    if (cur == NULL) {
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

// Cleanup routine for client threads, called on cancels and exit.
void thread_cleanup(void *arg) {
    // TODO: Remove the client object from thread list and call
    // client_destructor. This function must be thread safe! The client must
    // be in the list before this routine is ever run.

    client_t *c = (client_t *) arg;

    //remove 
    pthread_mutex_lock(&thread_list_mutex); //already locked? 
    if (c->prev == c && c->next == c && c == thread_list_head) {
        thread_list_head = 0;
    } else {
        client_t *prev = c->prev;
        client_t *next = c->next;
        next->prev = prev;
        prev->next = next;
    }
    pthread_mutex_unlock(&thread_list_mutex);

    //decrement 
    pthread_mutex_lock(&scontrol.server_mutex);
    scontrol.num_client_threads--; 
    pthread_mutex_unlock(&scontrol.server_mutex);

    //check if this is the last client 
    if (scontrol.num_client_threads == 0) {
        pthread_cond_signal(&scontrol.server_cond);
    }
    client_destructor(c);
}

// Code executed by the signal handler thread. For the purpose of this
// assignment, there are two reasonable ways to implement this.
// The one you choose will depend on logic in sig_handler_constructor.
// 'man 7 signal' and 'man sigwait' are both helpful for making this
// decision. One way or another, all of the server's client threads
// should terminate on SIGINT. The server (this includes the listener
// thread) should not, however, terminate on SIGINT!
void *monitor_signal(void *arg) {
    // TODO: Wait for a SIGINT to be sent to the server process and cancel
    // all client threads when one arrives.
    sigset_t *set;
    set = (sigset_t *) arg;
    int sig;
    while(1) {
        if (sigwait(set, &sig) == 0) {
            if (sig == SIGINT) {
                if (scontrol.num_client_threads == 0) {
                    printf(" sigint received. no clients to terminate.\n");
                } else {
                    printf(" sigint received. terminating all clients.\n");
                    pthread_mutex_lock(&thread_list_mutex);
                    delete_all();
                    pthread_mutex_unlock(&thread_list_mutex);
                }
            }
        } else { 
            //error check
        }
    }
    return NULL;
}

sig_handler_t *sig_handler_constructor() {
    // TODO: Create a thread to handle SIGINT. The thread that this function
    // creates should be the ONLY thread that ever responds to SIGINT.

    sig_handler_t *sigint_handler;
    sigint_handler = malloc(sizeof(sig_handler_t));

    sigemptyset(&sigint_handler->set);
    sigaddset(&sigint_handler->set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigint_handler->set, 0);
    pthread_create(&sigint_handler->thread, 0, monitor_signal, &sigint_handler->set); //error check 

    return sigint_handler;
}

void sig_handler_destructor(sig_handler_t *sighandler) {
    // TODO: Free any resources allocated in sig_handler_constructor.
    // Cancel and join with the signal handler's thread. 

    pthread_cancel(sighandler->thread); //error check 
    pthread_join(sighandler->thread, 0); //use 0? 
    free(sighandler);
}

// The arguments to the server should be the port number.
int main(int argc, char *argv[]) {
    // TODO:
    // Step 1: Set up the signal handler for handling SIGINT. 
    // Step 2: block SIGPIPE so that the server does not abort when a client disocnnects
    // Step 3: Start a listener thread for clients (see start_listener in
    //       comm.c).
    // Step 4: Loop for command line input and handle accordingly until EOF.
    // Step 5: Destroy the signal handler, delete all clients, cleanup the
    //       database, cancel and join with the listener thread
    //
    // You should ensure that the thread list is empty before cleaning up the
    // database and canceling the listener thread. Think carefully about what
    // happens in a call to delete_all() and ensure that there is no way for a
    // thread to add itself to the thread list after the server's final
    // delete_all().

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, 0);

    sig_handler_t *sigh = sig_handler_constructor();

    // if (signal(SIGPIPE, SIG_BLOCK) == SIG_ERR) {
    //     printf("sig_ign error");
    // } //this is wrong i think

    pthread_t listener = start_listener(atoi(argv[1]), client_constructor);

    size_t MAX = 1024;
    char buffer[MAX];
    memset(buffer, 0, MAX);
    while (read(0, buffer, MAX) > 0) {
        char bufz[1];
        char file[512];
        memset(file, 0, 512);
        sscanf(buffer, "%s %s", bufz, file);

        if (!strcmp(bufz, "s")) { 
            printf("stopped\n");
            client_control_stop();
        } else if (!strcmp(bufz, "g")) {
            printf("resumed\n");
            client_control_release();
        } else if (!strcmp(bufz, "p")) {
            db_print(file);
        } else {
            fprintf(stderr, "syntax error: please enter either s, g, or p\n");
        }
    }

    sig_handler_destructor(sigh);
    pthread_mutex_lock(&server_accept_mutex);
    server_accept = 0;
    delete_all();
    pthread_mutex_unlock(&server_accept_mutex);

    pthread_mutex_lock(&scontrol.server_mutex);
    while(scontrol.num_client_threads > 0) {
        pthread_cond_wait(&scontrol.server_cond, &scontrol.server_mutex); 
    } 
    pthread_mutex_unlock(&scontrol.server_mutex); 
    db_cleanup();

    pthread_cancel(listener);
    pthread_join(listener, 0);

    printf("exiting database\n");
    pthread_exit((void *) 0);

    return 0;
}

/*
issues:
 - sigint not doing anything 
 - stop, wait, resume check

 questions:
  - when to call client_control_wait? 
*/