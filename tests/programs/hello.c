#include <stdio.h>
int main(int argc, char **argv) {
    printf("Hello from a %d-bit DOS-XC program!\n", (int)sizeof(void *) * 8);
    for (int i = 0; i < argc; ++i) printf("argv[%d] = %s\n", i, argv[i]);
    return 0;
}
