/* Self-test for the DOS-XC C library and program runtime. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { ++checks; if (strcmp((got), (want))) { ++failures; printf("FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, (got), (want)); } } while (0)

static jmp_buf jump;
static void jumper(int v) { longjmp(jump, v); }
static int compare(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void test_format(void) {
    char b[256];
    snprintf(b, sizeof b, "%d|%5d|%-5d|%05d|%+d|%x|%#X|%o|%lld|%zu", 42, 42, 42, 42, 42, 255, 255, 8, -1234567890123LL, (size_t)99);
    CHECK_STR(b, "42|   42|42   |00042|+42|ff|0XFF|10|-1234567890123|99");
    snprintf(b, sizeof b, "%.3f|%e|%g|%g|%g|%.0f|%10.2f", 3.14159265, 12345.678, 0.0001, 1e20, 100.0, 2.5, -1.005);
    CHECK_STR(b, "3.142|1.234568e+04|0.0001|1e+20|100|2|     -1.00");
    snprintf(b, sizeof b, "%.17g|%.20f|%a", 0.1, 0.1, 1.0);
    CHECK_STR(b, "0.10000000000000001|0.10000000000000000555|0x1p+0");
    snprintf(b, sizeof b, "%s|%10s|%-4s|%.2s|%c|%%|%p", "abc", "right", "l", "xyz", 'Q', (void *)0);
    CHECK_STR(b, "abc|     right|l   |xy|Q|%|(nil)");
    snprintf(b, sizeof b, "%Lg|%g|%g", 1.5L, INFINITY, -0.0);
    CHECK_STR(b, "1.5|inf|-0");
    snprintf(b, sizeof b, "%.0e|%.3e|%g", 1.0e300, 9.9996e-300, 123456789.0);
    CHECK_STR(b, "1e+300|1.000e-299|1.23457e+08");
    CHECK(snprintf(b, 4, "abcdef") == 6 && !strcmp(b, "abc"));
    int x, y; char word[16]; double d;
    CHECK(sscanf("12 0x1f hello 2.5", "%d %i %15s %lf", &x, &y, word, &d) == 4 && x == 12 && y == 31 && !strcmp(word, "hello") && d == 2.5);
}

static void test_strings(void) {
    char b[64];
    CHECK(strlen("hello") == 5);
    CHECK(strcmp("a", "b") < 0 && strncmp("abc", "abd", 2) == 0);
    CHECK(!strcasecmp("HeLLo", "hello"));
    strcpy(b, "foo"); strcat(b, "bar"); CHECK_STR(b, "foobar");
    CHECK(strstr("haystack", "st") && !strcmp(strstr("haystack", "st"), "stack"));
    CHECK(strchr("abc", 'c') && !strrchr("abc", 'z'));
    char *dup = strdup("copy"); CHECK_STR(dup, "copy"); free(dup);
    CHECK(strtol("-123xyz", 0, 10) == -123 && strtoul("ff", 0, 16) == 255 && strtoll("0777", 0, 0) == 511);
    errno = 0; CHECK(strtol("99999999999999999999", 0, 10) == LONG_MAX && errno == ERANGE);
    CHECK(strtod("1e3", 0) == 1000.0 && strtod("0x1.8p1", 0) == 3.0 && strtod("-2.5e-3", 0) == -0.0025);
    CHECK(atoi(" 77") == 77 && isdigit('7') && !isalpha('7') && toupper('q') == 'Q');
    memset(b, 'z', 10); b[10] = 0; CHECK_STR(b, "zzzzzzzzzz");
    memmove(b + 1, b, 5); CHECK(b[0] == 'z');
    CHECK_STR(strerror(ENOENT), "No such file or directory");
}

static void test_memory(void) {
    void *blocks[200];
    for (int i = 0; i < 200; ++i) { blocks[i] = malloc((size_t)(i * 37 % 5000) + 1); CHECK(blocks[i] && ((size_t)blocks[i] & 15) == 0); memset(blocks[i], i, (size_t)(i * 37 % 5000) + 1); }
    for (int i = 0; i < 200; i += 2) free(blocks[i]);
    for (int i = 1; i < 200; i += 2) CHECK(((unsigned char *)blocks[i])[0] == (unsigned char)i);
    for (int i = 1; i < 200; i += 2) free(blocks[i]);
    char *big = malloc(8 << 20); CHECK(big); memset(big, 1, 8 << 20); big = realloc(big, 16 << 20); CHECK(big && big[(8 << 20) - 1] == 1); free(big);
    char *grow = 0; for (int i = 1; i <= 1000; ++i) { grow = realloc(grow, (size_t)i * 10); CHECK(grow); grow[i * 10 - 1] = 'x'; } free(grow);
    void *aligned = 0; CHECK(!posix_memalign(&aligned, 4096, 100) && ((size_t)aligned & 4095) == 0); free(aligned);
    int *zero = calloc(1000, sizeof(int)); int sum = 0; for (int i = 0; i < 1000; ++i) sum += zero[i]; CHECK(sum == 0); free(zero);
    int values[] = {5, 3, 9, 1, 7, 2, 8, 6, 4, 0, 11, 15, 13, 12, 14, 10};
    qsort(values, 16, sizeof(int), compare);
    int sorted = 1; for (int i = 0; i < 16; ++i) sorted &= values[i] == i; CHECK(sorted);
}

static void test_math(void) {
    CHECK(fabs(sqrt(2.0) - 1.4142135623730951) < 1e-15);
    CHECK(fabs(sin(M_PI / 6) - 0.5) < 1e-15 && fabs(cos(0.0) - 1) < 1e-15);
    CHECK(fabs(exp(1.0) - M_E) < 1e-15 && fabs(log(M_E) - 1) < 1e-15);
    CHECK(pow(2, 10) == 1024 && pow(10, 3) == 1000 && fabs(pow(2.0, 0.5) - M_SQRT2) < 1e-15);
    CHECK(floor(-2.5) == -3 && ceil(-2.5) == -2 && round(2.5) == 3 && trunc(-2.7) == -2 && rint(2.5) == 2);
    CHECK(fmod(10.5, 3) == 1.5 && fabs(atan2(1, 1) - M_PI / 4) < 1e-15);
    int e; CHECK(frexp(8.0, &e) == 0.5 && e == 4 && ldexp(0.5, 4) == 8.0);
    CHECK(isnan(NAN) && isinf(INFINITY) && !isfinite(INFINITY));
    CHECK(fabs(tgamma(5.0) - 24) < 1e-12 && fabs(erf(1.0) - 0.8427007929497149) < 1e-14);
}

static void test_files(void) {
    FILE *f = fopen("LIBCTEST.TMP", "w");
    CHECK(f != 0);
    if (!f) return;
    for (int i = 0; i < 1000; ++i) fprintf(f, "line %d\n", i);
    CHECK(fclose(f) == 0);
    struct stat s; CHECK(stat("LIBCTEST.TMP", &s) == 0 && S_ISREG(s.st_mode) && s.st_size > 7000);
    f = fopen("LIBCTEST.TMP", "r");
    CHECK(f != 0);
    char line[64]; int count = 0, last = -1;
    while (fgets(line, sizeof line, f)) { ++count; sscanf(line, "line %d", &last); }
    CHECK(count == 1000 && last == 999);
    CHECK(fseek(f, 5, SEEK_SET) == 0 && fgetc(f) == '0' && ftell(f) == 6);
    fclose(f);
    int fd = open("LIBCTEST.TMP", O_RDWR | O_APPEND);
    CHECK(fd >= 0 && write(fd, "end\n", 4) == 4 && lseek(fd, 0, SEEK_END) == s.st_size + 4);
    close(fd);
    CHECK(rename("LIBCTEST.TMP", "LIBCTEST.TM2") == 0 && access("LIBCTEST.TMP", F_OK) != 0);
    CHECK(unlink("LIBCTEST.TM2") == 0 && access("LIBCTEST.TM2", F_OK) != 0 && errno == ENOENT);
    CHECK(mkdir("TESTDIR", 0777) == 0 && mkdir("TESTDIR", 0777) == -1 && errno == EEXIST);
    f = fopen("TESTDIR/a long file name.txt", "w"); CHECK(f && fputs("x", f) >= 0 && fclose(f) == 0);
    DIR *d = opendir("TESTDIR"); int found = 0; struct dirent *e;
    while (d && (e = readdir(d))) found += !strcmp(e->d_name, "a long file name.txt");
    if (d) closedir(d);
    CHECK(found == 1);
    CHECK(chdir("TESTDIR") == 0);
    char cwd[PATH_MAX]; CHECK(getcwd(cwd, sizeof cwd) && strstr(cwd, "TESTDIR"));
    CHECK(unlink("a long file name.txt") == 0 && chdir("..") == 0 && rmdir("TESTDIR") == 0);
    char template[] = "/TEMP/tstXXXXXX"; fd = mkstemp(template); CHECK(fd >= 0); close(fd); unlink(template);
    f = tmpfile(); CHECK(f && fputs("temporary", f) >= 0); if (f) { rewind(f); CHECK(fgets(line, sizeof line, f) && !strcmp(line, "temporary")); fclose(f); }
    int p[2]; CHECK(pipe(p) == 0 && write(p[1], "pipe", 4) == 4 && read(p[0], line, 4) == 4 && !memcmp(line, "pipe", 4));
    close(p[0]); close(p[1]);
    CHECK(open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT);
}

static void test_process(char *self) {
    CHECK(getenv("PATH") != 0);
    CHECK(setenv("DOSXC_TEST", "yes", 1) == 0 && !strcmp(getenv("DOSXC_TEST"), "yes"));
    int v = setjmp(jump);
    if (!v) jumper(7);
    CHECK(v == 7);
    char *args[] = {self, "child", 0};
    pid_t pid; int status = -1;
    CHECK(posix_spawn(&pid, self, 0, 0, args, environ) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    CHECK(posix_spawn(&pid, "/no/such/program.exe", 0, 0, args, environ) == ENOENT);
    char *crash[] = {self, "crash", 0};
    CHECK(posix_spawn(&pid, self, 0, 0, crash, environ) == 0 && waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    time_t now = time(0); CHECK(now > 1700000000);
    struct tm tm; gmtime_r(&now, &tm); CHECK(tm.tm_year >= 124 && timegm(&tm) == now);
    char when[64]; CHECK(strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm) == 19);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return getenv("DOSXC_TEST") ? 42 : 1;
    if (argc > 1 && !strcmp(argv[1], "crash")) __builtin_trap();
    test_format();
    test_strings();
    test_memory();
    test_math();
    test_files();
    test_process(argv[0]);
    printf("libctest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
