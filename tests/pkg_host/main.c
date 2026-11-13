#include "fs.h"
#include "../../os/pkg/sha256.h"
#include <string.h>
int pkg_main(int,char **);
int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--sha256")) {
        FILE *f=fopen(argv[2],"rb"); if (!f) return 1;
        struct pkg_sha256 s; pkg_sha256_init(&s); char buffer[8192],hex[65]; size_t n;
        while ((n=fread(buffer,1,sizeof(buffer),f))) pkg_sha256_update(&s,buffer,n);
        int failed=ferror(f); fclose(f); pkg_sha256_final(&s,hex); puts(hex); return failed;
    }
    return pkg_main(argc,argv);
}
