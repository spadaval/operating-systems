/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * jitc.c
 */

#include "jitc.h"

#include <dlfcn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "system.h"

#define DL_ASSERT(c)       \
    dlerror();             \
    c;                     \
    char *err = dlerror(); \
    if (err != NULL) {     \
        EXIT(err);         \
    }

/**
 * Needs:
 *   fork()
 *   execv()
 *   waitpid()
 *   WIFEXITED()
 *   WEXITSTATUS()
 *   dlopen()
 *   dlclose()
 *   dlsym()
 */

/* private */ void run(char *command, char **args) {
    printf("executing command: %s ", command);
    for (int i = 0; args[i] != NULL; i++) {
        printf("%s ", args[i]);
    }
    printf("\n");

    int pid = fork();
    if (pid == -1) {
        EXIT("fork failed");
    }
    if (pid == 0) { /* child */
        execv(command, args);
    } else {
        int status = 1;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 0) {
            EXIT("subprocess execution failed");
        }
        printf("subprocess exited with code %d\n", WEXITSTATUS(status));
    }
    printf("done compiling.\n");
}

/*private*/ void validate_library(const char *output) {
    FILE *f = fopen(output, "r");
    if (f == NULL) EXIT("file doesn't exist");
    fclose(f);

    DL_ASSERT(dlopen(output, RTLD_LAZY));
}

int jitc_compile(const char *input, const char *output) {
#ifdef __APPLE__
    run("/usr/bin/clang", (char *[]){"/usr/bin/clang", "-dynamiclib", (char *)input, "-o", (char *)output, NULL});
#else
    char *object_file = "tmp.o";
    run("/usr/bin/gcc", (char *[]){"/usr/bin/gcc", "-fPIC", "-c", (char *)input, "-o", object_file, NULL});
    run("/usr/bin/gcc", (char *[]){"/usr/bin/gcc", object_file, "-shared", "-o", (char *)output, NULL});
    file_delete(object_file);
#endif

    validate_library(output);

    return 0;
}

struct jitc *jitc_open(const char *pathname) {
    printf("Trying to open dynamic library: %s\n", pathname);
    DL_ASSERT(
        struct jitc *dylib = (struct jitc *)dlopen(pathname, RTLD_NOW);)
    struct jitc *jitc = (struct jitc *)malloc(sizeof(struct jitc));
    jitc->handle = dylib;
    return jitc;
}

void jitc_close(struct jitc *jitc) {
    printf("Closing dynamic library\n");
    dlerror();
    DL_ASSERT(dlclose(jitc->handle));
    free(jitc);
}

long jitc_lookup(struct jitc *jitc, const char *symbol) {
    void *ptr = dlsym(jitc->handle, symbol);
    if (ptr == NULL) {
        EXIT("JITC: failed to find symbol");
    }
    return (long)ptr;
}

/* research the above Needed API and design accordingly */
