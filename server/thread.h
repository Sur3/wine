/*
 * Wine server threads
 *
 * Copyright (C) 1998 Alexandre Julliard
 */

#ifndef __WINE_SERVER_THREAD_H
#define __WINE_SERVER_THREAD_H

#ifndef __WINE_SERVER__
#error This file can only be used in the Wine server
#endif

#include "object.h"

/* thread structure */

struct process;
struct thread_wait;
struct thread_apc;
struct mutex;
struct debug_ctx;
struct debug_event;
struct startup_info;
struct msg_queue;

enum run_state
{
    RUNNING,    /* running normally */
    TERMINATED  /* terminated */
};

struct apc_queue
{
    struct thread_apc *head;
    struct thread_apc *tail;
};

/* descriptor for fds currently in flight from client to server */
struct inflight_fd
{
    int client;  /* fd on the client side (or -1 if entry is free) */
    int server;  /* fd on the server side */
};
#define MAX_INFLIGHT_FDS 16  /* max number of fds in flight per thread */

struct thread
{
    struct object       obj;       /* object header */
    struct thread      *next;      /* system-wide thread list */
    struct thread      *prev;
    struct thread      *proc_next; /* per-process thread list */
    struct thread      *proc_prev;
    struct process     *process;
    struct mutex       *mutex;       /* list of currently owned mutexes */
    struct debug_ctx   *debug_ctx;   /* debugger context if this thread is a debugger */
    struct debug_event *debug_event; /* debug event being sent to debugger */
    struct msg_queue   *queue;       /* message queue */
    struct startup_info*info;        /* startup info for child process */
    struct thread_wait *wait;        /* current wait condition if sleeping */
    struct apc_queue    system_apc;  /* queue of system async procedure calls */
    struct apc_queue    user_apc;    /* queue of user async procedure calls */
    struct inflight_fd  inflight[MAX_INFLIGHT_FDS];  /* fds currently in flight */
    unsigned int        error;       /* current error code */
    struct object      *request_fd;  /* fd for receiving client requests */
    int                 reply_fd;    /* fd to send a reply to a client */
    int                 wait_fd;     /* fd to use to wake a sleeping client */
    enum run_state      state;     /* running state */
    int                 attached;  /* is thread attached with ptrace? */
    int                 exit_code; /* thread exit code */
    int                 unix_pid;  /* Unix pid of client */
    CONTEXT            *context;   /* current context if in an exception handler */
    void               *teb;       /* TEB address (in client address space) */
    int                 priority;  /* priority level */
    int                 affinity;  /* affinity mask */
    int                 suspend;   /* suspend count */
    void               *buffer;    /* buffer for communication with the client */
};

struct thread_snapshot
{
    struct thread  *thread;    /* thread ptr */
    int             count;     /* thread refcount */
    int             priority;  /* priority class */
};

extern struct thread *current;

/* thread functions */

extern struct thread *create_thread( int fd, struct process *process );
extern struct thread *get_thread_from_id( void *id );
extern struct thread *get_thread_from_handle( handle_t handle, unsigned int access );
extern struct thread *get_thread_from_pid( int pid );
extern int suspend_thread( struct thread *thread, int check_limit );
extern int resume_thread( struct thread *thread );
extern void suspend_all_threads( void );
extern void resume_all_threads( void );
extern int add_queue( struct object *obj, struct wait_queue_entry *entry );
extern void remove_queue( struct object *obj, struct wait_queue_entry *entry );
extern void kill_thread( struct thread *thread, int violent_death );
extern void wake_up( struct object *obj, int max );
extern int thread_queue_apc( struct thread *thread, struct object *owner, void *func,
                             enum apc_type type, int system, int nb_args, ... );
extern void thread_cancel_apc( struct thread *thread, struct object *owner, int system );
extern int thread_add_inflight_fd( struct thread *thread, int client, int server );
extern int thread_get_inflight_fd( struct thread *thread, int client );
extern struct thread_snapshot *thread_snap( int *count );

/* ptrace functions */

extern void sigchld_handler();
extern void wait4_thread( struct thread *thread, int signal );
extern void stop_thread( struct thread *thread );
extern void continue_thread( struct thread *thread );
extern void detach_thread( struct thread *thread, int sig );
extern int suspend_for_ptrace( struct thread *thread );
extern int read_thread_int( struct thread *thread, const int *addr, int *data );
extern int write_thread_int( struct thread *thread, int *addr, int data, unsigned int mask );
extern void *get_thread_ip( struct thread *thread );

extern unsigned int global_error;  /* global error code for when no thread is current */

static inline unsigned int get_error(void)       { return current ? current->error : global_error; }
static inline void set_error( unsigned int err ) { global_error = err; if (current) current->error = err; }
static inline void clear_error(void)    { set_error(0); }

static inline void *get_thread_id( struct thread *thread ) { return thread; }

#endif  /* __WINE_SERVER_THREAD_H */
