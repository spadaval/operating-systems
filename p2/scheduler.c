/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * scheduler.c
 */

#undef _FORTIFY_SOURCE

#include "scheduler.h"

#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>

#include "system.h"

#define u64 uint64_t

/**
 * Needs:
 *   setjmp()
 *   longjmp()
 */

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>

/* Obtain a backtrace and print it to stdout. */
void print_trace(void) {
    void* array[50];
    char** strings;
    int size, i;

    size = backtrace(array, 10);
    strings = backtrace_symbols(array, size);
    if (strings != NULL) {
        printf("Obtained %d stack frames.\n", size);
        for (i = 0; i < size; i++)
            printf("%s\n", strings[i]);
    }

    free(strings);
}

#define info(...)           \
    printf("\033[31;1;4m"); \
    printf(__VA_ARGS__);    \
    printf("\033[0m\n");

typedef enum Status {
    NOT_STARTED = 123,
    STARTED,
    SUSPENDED
} Status;

char* to_str(Status status) {
    switch (status) {
        case NOT_STARTED:
            return "NOT_STARTED";
        case STARTED:
            return "STARTED";
        case SUSPENDED:
            return "SUSPENDED";
        default:
            return "WTF";
    }
}

typedef struct {
    scheduler_fnc_t fn_start;
    void* arg;
    jmp_buf env;
    Status status;
    u64 stack_ptr;
    int id;
} Thread;

typedef struct ThreadNode {
    Thread thread;
    struct ThreadNode* next;
} ThreadNode;

typedef struct {
    ThreadNode* thread_list;
    int thread_count;
    u64 system_stack;
    jmp_buf system_env;
    Thread* running_thread;
} System;

System sys = {.thread_list = NULL, .thread_count = 0, .system_stack = 0, .running_thread = NULL};

/*private*/ ThreadNode* get_thread(int thread_id) {
    ThreadNode* n = sys.thread_list;
    for (int i = 0; i < thread_id; i++) {
        n = n->next;
    }
    assert(n->thread.id == thread_id);
    return n;
}

#define STACK_SIZE 1024 * 1024

u64 allocate_stack(void) {
    char* stack = malloc(STACK_SIZE + 100);
    return (u64)(stack + STACK_SIZE);
}

u64 get_stack(void) {
    register u64 sp = 0;
    __asm__ volatile("mov %[rs], sp" : [rs] "+r"(sp) : "r"(sp) :);
    return sp;
}

#define set_stack(val) __asm__ volatile("mov sp, %[rsp]" : [rsp] "+r"(val)::);

/*private*/ void print_status(void) {
    // return;
    print_trace();
    printf("\n%d threads:\n", sys.thread_count);
    ThreadNode* n = sys.thread_list;
    int i = 0;
    while (n != NULL) {
        printf("%-30d", i++);
        n = n->next;
    }
    printf("\n");
    n = sys.thread_list;
    while (n != NULL) {
        printf("%-30s", to_str(n->thread.status));
        n = n->next;
    }
    printf("\n");
    n = sys.thread_list;
    i = 0;
    while (n != NULL) {
        printf("%-30s", i++ == sys.running_thread->id ? "running" : "not running");
        n = n->next;
    }
    printf("\n");
    n = sys.thread_list;
    i = 0;
    while (n != NULL) {
        printf("%-30llu", n->thread.stack_ptr);
        n = n->next;
    }
    printf("\nrunning=%d", sys.running_thread->id);
    printf("\n\n");
}

// systemspace
int scheduler_create(scheduler_fnc_t fnc, void* arg) {
    ThreadNode* new_node = (ThreadNode*)malloc(sizeof(ThreadNode));
    new_node->thread.stack_ptr = -14;
    new_node->thread.fn_start = fnc;
    new_node->thread.arg = arg;
    new_node->thread.status = NOT_STARTED;
    info("added a new thread");

    if (sys.thread_list == NULL) {
        new_node->thread.id = 0;
        sys.thread_list = new_node;
    } else {
        ThreadNode* tail = sys.thread_list;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        new_node->thread.id = tail->thread.id + 1;
        tail->next = new_node;
    }
    sys.thread_count++;
    return 0;
}

///////////////////
// Core functions
///////////////////

// systemspace
void launch(Thread* thread) {
    assert(thread->status == NOT_STARTED);
    sys.running_thread = thread;
    info("[LAUNCH] starting thread %d", thread->id);
    thread->stack_ptr = allocate_stack();
    info("[LAUNCH] allocating stack at %llu", thread->stack_ptr);
    thread->status = STARTED;
    print_status();

    set_stack(thread->stack_ptr);
    thread->fn_start(thread->arg);
}

// systemspace
void resume(Thread* thread) {
    sys.running_thread = thread;
    assert(thread->status == SUSPENDED);
    info("[EXEC] resuming thread %d", thread->id);
    print_status();

    printf("Saved system stack as %llu", sys.system_stack);
    set_stack(thread->stack_ptr);
    longjmp(thread->env, 14);  // should go to yield
}

// systemspace
void scheduler_execute(void) {
    setjmp(sys.system_env);
    info("[SYSTEM]Set jump point\n");

    printf("[EXECUTE] current stack: %llu\n", get_stack());
    assert(sys.thread_list != NULL);

    if (sys.running_thread == NULL) {
        sys.thread_list->thread.status = STARTED;
        sys.running_thread = &sys.thread_list->thread;
        print_status();
        sys.running_thread->fn_start(sys.running_thread->arg);
        return;
    }

    info("[SYSTEM] in main system loop");

    ThreadNode* n = get_thread(sys.running_thread->id);
    do {
        n = (n->next == NULL) ? sys.thread_list : n->next;
    } while (n->thread.status == STARTED);

    sys.running_thread = &n->thread;
    Thread* thread = &n->thread;

    if (thread->status == NOT_STARTED) {
        launch(thread);
    } else {
        resume(thread);
    }
}

// userspace
void scheduler_yield(void) {
    assert(sys.thread_list != NULL);

    sys.running_thread->stack_ptr = get_stack();
    info("Save stack for %d to %llu", sys.running_thread->id, sys.running_thread->stack_ptr);
    sys.running_thread->status = SUSPENDED;

    if (setjmp(sys.running_thread->env) == 0) {
        info("[YIELD] Thread %d yielding", sys.running_thread->id);
        longjmp(sys.system_env, 0);  // should go to execute
    } else {
        print_status();
        sys.running_thread->status = STARTED;
        info("[YIELD] Thread %d resuming", sys.running_thread->id);
        return;
    }
}
