/* Run on DOS-XC: exercise real CMOS writes and restore the original clock. */
#include <dosxc/syscall.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    struct dosxc_clock original, value, readback;
    if (dosxc_sys_clock(DOSXC_CLOCK_GET, &original) < 0) return 1;
    unsigned long long start = dosxc_sys_nanoseconds();
    const long long dates[] = {0, 951825600LL, 1709251199LL, 2147483648LL, 4102444799LL};
    int failed = 0;
    for (unsigned i = 0; i < sizeof(dates) / sizeof(*dates); ++i) {
        value = original; value.seconds = dates[i];
        int result = dosxc_sys_clock(DOSXC_CLOCK_SET, &value);
        dosxc_sys_clock(DOSXC_CLOCK_GET, &readback);
        if (result || readback.seconds < dates[i] || readback.seconds > dates[i] + 1) {
            printf("FAIL RTC %lld: result %d, read %lld\n", dates[i], result, readback.seconds); failed = 1;
        }
    }
    value = original;
    value.seconds += (dosxc_sys_nanoseconds() - start) / 1000000000ULL;
    if (dosxc_sys_clock(DOSXC_CLOCK_SET, &value)) failed = 1;
    printf("%s hardware clock round trips (1970, leap day, 2038, 2099)\n", failed ? "FAIL" : "PASS");
    return failed;
}
