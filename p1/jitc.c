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

// char *object_file = "out.o";

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
        int status;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 0) {
            EXIT("subprocess execution failed");
        }
        printf("subprocess exited with code %d\n", WEXITSTATUS(status));
    }
    printf("done compiling.\n");
}

int jitc_compile(const char *input, const char *output) {
#ifdef __APPLE__
    run("/usr/bin/clang", (char *[]){"/usr/bin/clang", "-dynamiclib", (char *)input, "-o", (char *)output, NULL});
#else
    char *object_file = "tmp.o";
    // printf("compiling\n");
    run("/usr/bin/gcc", (char *[]){"/usr/bin/gcc", "-fPIC", "-c", (char *)input, "-o", object_file, NULL});
    run("/usr/bin/gcc", (char *[]){"/usr/bin/gcc", object_file, "-shared", "-o", (char *)output, NULL});
    // printf("compile complete\nBundling...");
    // run("/usr/bin/ar", (char *[]){"/usr/bin/ar", "rcs", (char *)output, object_file, NULL});
    file_delete(object_file);
#endif

    if (fopen(output, "r") == NULL) EXIT("compilation failed");
    dlopen(output, RTLD_LAZY);
    char *err = dlerror();
    if (err != NULL) {
        EXIT(err);
    }

    return 0;
}

struct jitc *jitc_open(const char *pathname) {
    printf("Trying to open JITC library: %s\n", pathname);
    dlerror();
    struct jitc *dylib = (struct jitc *)dlopen(pathname, RTLD_NOW);

    char *err = dlerror();
    if (err != NULL) {
        EXIT(err);
    }
    return dylib;
}

void jitc_close(struct jitc *jitc) {
    dlclose(jitc);
}

long jitc_lookup(struct jitc *jitc, const char *symbol) {
    void *ptr = dlsym(jitc, symbol);
    if (ptr == NULL) {
        EXIT("JITC: failed to find symbol");
    }
    return (long)ptr;
}

/* research the above Needed API and design accordingly */
