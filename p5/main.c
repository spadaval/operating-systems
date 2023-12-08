/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * main.c
 */

#include <signal.h>
#include <stdbool.h>

#include "system.h"

#define u64 uint64_t
#define u32 uint32_t
#define u8 uint8_t

/**
 * Needs:
 *   signal()
 */

#include <unistd.h>

#define UNWRAP(x)        \
    if ((x) == -1) {     \
        perror("error"); \
        exit(-1);        \
    }

enum Mode {
    ALL = 42,
    MEM,
    CPU,
    UPTIME,
    NETWORK,
    DONE
};

const char *const PROC_STAT = "/proc/stat";
const char *const PROC_UPTIME = "/proc/uptime";
const char *const PROC_MEMINFO = "/proc/meminfo";
const char *const PROC_NET = "/proc/net/dev";

static volatile enum Mode mode = ALL;

static void install_interrupt_handler();

static void on_interrupt(int signum) {
    assert(SIGINT == signum);
    mode += 1;
    install_interrupt_handler();
}

void install_interrupt_handler() {
    if (SIG_ERR == signal(SIGINT, on_interrupt)) {
        EXIT("signal");
    }
}

double parse_cpu(const char *s) {
    static unsigned sum_, vector_[7];
    unsigned sum, vector[7];
    const char *p;
    double util;
    uint64_t i;

    /*
      user
      nice
      system
      idle
      iowait
      irq
      softirq
    */

    if (!(p = strstr(s, " ")) ||
        (7 != sscanf(p,
                     "%u %u %u %u %u %u %u",
                     &vector[0],
                     &vector[1],
                     &vector[2],
                     &vector[3],
                     &vector[4],
                     &vector[5],
                     &vector[6]))) {
        return 0;
    }
    sum = 0.0;
    for (i = 0; i < ARRAY_SIZE(vector); ++i) {
        sum += vector[i];
    }
    util = (1.0 - (vector[3] - vector_[3]) / (double)(sum - sum_)) * 100.0;
    sum_ = sum;
    for (i = 0; i < ARRAY_SIZE(vector); ++i) {
        vector_[i] = vector[i];
    }
    return util;
}

int print_cpu(char *str) {
    char line[1024];
    FILE *file;

    if (!(file = fopen(PROC_STAT, "r"))) {
        EXIT("fopen");
    }
    int bytes = 0;
    if (fgets(line, sizeof(line), file)) {
        bytes = snprintf(str, 2048, "CPU %5.1f%%", parse_cpu(line));
    }
    fclose(file);
    return bytes;
}

int print_uptime(char *str) {
    char line[1024];
    FILE *file;

    if (!(file = fopen(PROC_UPTIME, "r"))) {
        EXIT("fopen()");
    }
    int bytes = 0;
    if (fgets(line, sizeof(line), file)) {
        float time = 0;
        float idle = 0;
        sscanf(line, "%f %f", &time, &idle);
        bytes = snprintf(str, 2048, "Uptime: %.0f seconds (idle for %.0f seconds)", time, idle);
        fflush(stdout);
    }
    fclose(file);
    return bytes;
}

char sizes[3][3] = {"KB", "MB", "GB"};

u64 parse_num(char line[1024]) {
    int i = 0;
    // skip past the :
    while (line[i] != ':') i++;
    i++;
    // skip past the spaces
    while (line[i] == ' ') i++;
    i++;

    u64 val = 0;
    while (line[i] >= '0' && line[i] <= '9') {
        val = val * 10 + (line[i] - '0');
        i++;
    }
    return val;
}

int print_memory(char *str) {
    char line[1024];
    FILE *file;

    if (!(file = fopen(PROC_MEMINFO, "r"))) {
        TRACE("fopen()");
        return -1;
    }

    if (fgets(line, sizeof(line), file) == 0) exit(-1);
    u64 total = parse_num(line);
    if (fgets(line, sizeof(line), file) == 0) exit(-1);
    u64 free = parse_num(line);

    int bytes = snprintf(str, 2048, "[MEM] %lu KB free of %lu KB", free, total);

    fclose(file);
    return bytes;
}

int print_network(char *str) {
    static u64 prev_recv = 0, prev_snd = 0;

    char line[4096];
    FILE *file;

    if (!(file = fopen(PROC_NET, "r"))) {
        EXIT("fopen");
    }

    // skip the headers
    if (fgets(line, sizeof(line), file) == 0) exit(-1);
    if (fgets(line, sizeof(line), file) == 0) exit(-1);

    u64 data[16];
    u64 recv = 0, snd = 0;
    while (fgets(line, sizeof(line), file) != 0) {
        // note: there may be an arbitrary amount of spaces anywhere
        // here's the data format
        // face: bytes packets errs drop fifo frame compressed multicast bytes packets errs drop fifo colls carrier compressed
        //       ^ (pos 0)                                             ^ (pos 8)
        if (fgets(line, sizeof(line), file) == 0) exit(-1);

        int i = 0;
        // skip till past the `face:`
        while (line[i] != ':') i++;
        i++;

        while (line[i] == ' ') i++;

        // parse all 16 numbers in the line
        for (int x = 0; x < 16; x++) {
            int num = 0;
            // read a number
            while (line[i] >= '0' && line[i] <= '9') {
                num = num * 10 + (line[i] - '0');
                i++;
            }
            data[x] = num;
            // skip white space
            while (line[i] == ' ') i++;
        }

        recv += data[0];
        snd = data[8];
    }
    fclose(file);

    int bytes = snprintf(str, 2048, "[NET] send ↑%5.1f KB, rcv ↓%10.1f KB", (snd - prev_snd) / 1024.0, (recv - prev_recv) / 1024.0);
    prev_recv = recv;
    prev_snd = snd;

    return bytes;
}

int main(int argc, char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);
    char buffer[2048];

    install_interrupt_handler();

    while (true) {
        if (mode == ALL) {
            char *b = buffer;
            b += print_cpu(b);
            *b++ = '\t';
            b += print_uptime(b);
            *b++ = '\t';
            b += print_memory(b);
            *b++ = '\t';
            b += print_network(b);
        } else if (mode == CPU) {
            print_cpu(buffer);
        } else if (mode == UPTIME) {
            print_uptime(buffer);
        } else if (mode == MEM) {
            print_memory(buffer);
        } else if (mode == NETWORK) {
            print_network(buffer);
        } else
            break;
        printf("                                                                                                                                                     \r");
        fflush(stdout);
        printf("\r%s", buffer);
        fflush(stdout);
        us_sleep(500000);
    }
    printf("\rDone!   \n");
    return 0;
}
