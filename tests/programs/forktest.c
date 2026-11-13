/*
 * DOS-XC /tests/programs/forktest.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int global = 1;

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "exec")) {
        printf("exec child: pid %d\n", (int)getpid());
        return argc > 2 ? atoi(argv[2]) : 0;
    }
    int local = 2;
    char *small = malloc(64), *large = malloc(8 << 20);
    strcpy(small, "parent");
    memset(large, 'p', 8 << 20);

    pid_t pid = fork();
    if (pid == 0) {
        global = 10;
        local = 20;
        strcpy(small, "child");
        memset(large, 'c', 8 << 20);
        char *more = malloc(1 << 20);
        memset(more, 1, 1 << 20);
        free(more);
        free(small);
        _exit(global == 10 && local == 20 && large[123] == 'c' ? 7 : 1);
    }
    check(pid > 0, "fork returns the child's pid");
    int status = 0;
    check(waitpid(pid, &status, 0) == pid, "waitpid finds the child");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 7, "the child saw its own copy");
    check(global == 1 && local == 2 && !strcmp(small, "parent") && large[123] == 'p' && large[(8 << 20) - 1] == 'p',
          "the parent's memory is untouched");

    int fds[2];
    pipe(fds);
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        const char *text = "through the pipe";
        write(fds[1], text, strlen(text));
        _exit(0);
    }
    close(fds[1]);
    char buffer[64] = {0};
    size_t got = 0;
    for (ssize_t n; (n = read(fds[0], buffer + got, sizeof(buffer) - 1 - got)) > 0;) got += (size_t)n;
    close(fds[0]);
    waitpid(pid, 0, 0);
    check(!strcmp(buffer, "through the pipe"), "pipe from child to parent, then end of file");

    pid = fork();
    if (pid == 0) {
        char *args[] = {argv[0], "exec", "5", 0};
        execv(argv[0], args);
        _exit(99);
    }
    printf("forked pid %d to exec\n", (int)pid);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 5, "exec keeps the pid and passes the status");

    pid = fork();
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) _exit(global == 1 ? 3 : 1);
        int s = 0;
        waitpid(grandchild, &s, 0);
        _exit(WIFEXITED(s) ? WEXITSTATUS(s) + 1 : 1);
    }
    check(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 4, "a forked child forks again");

    pipe(fds);
    pid = fork();
    if (pid == 0) {
        pid_t orphan = fork();
        if (orphan == 0) {
            close(fds[0]);
            usleep(300000);
            memset(large, 'o', 8 << 20);
            write(fds[1], large[77] == 'o' && global == 1 ? "Y" : "N", 1);
            _exit(0);
        }
        _exit(0);
    }
    close(fds[1]);
    waitpid(pid, 0, 0);
    char answer = 0;
    read(fds[0], &answer, 1);
    close(fds[0]);
    check(answer == 'Y', "a child outlives the parent it was forked from");

    int ok = 1;
    for (int i = 0; i < 20 && ok; ++i) {
        pid = fork();
        if (pid == 0) _exit(i);
        ok = waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == i;
    }
    check(ok, "twenty forks in a row");
    printf("%s\n", failures ? "FORK TEST FAILED" : "FORK TEST PASSED");
    return failures != 0;
}
