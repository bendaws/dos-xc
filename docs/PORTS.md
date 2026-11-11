## Building programs for DOS-XC
DOS-XC has a script for automatically creating a GCC cross-compiler for building programs for DOS-XC. If your programs work on Linux when compiled with GCC, they will most likely work on DOS-XC, except for pathing, as DOS doesn't share a common heirarchy with Linux (so your /usr/bin, /home/<user>, etc.).

Programs that fuly rely on you setting the paths will work great however, as you can simply supply a DOS path to the program in its arguments, config, or in some other way, for example Vim:

```bat
vim C:\Home\Testing.txt
REM Which will also work with relative paths...

cd C:\Home
vim Testing.txt
```

## Get a Cross-Compiler
The easiest way is to run our battle-tested cross-compiler build script at `gcc/build.sh` as so:
```
./gcc/build.sh
```

Which produces the following binaries for a host system that compiles binaries for DOS-XC:
```
addr2line                   gcc-<version number here>   objdump
ar                          gcc-ar                      ranlib
as                          gcc-nm                      readelf
c++                         gcc-ranlib                  size
c++filt                     gprof                       strings
cpp                         ld                          strip
elfedit                     ld.bfd                      
g++                         nm                          
gcc                         objcopy                     
```

Prefix any program name with `i686-dosxc-` for 32-bit and `x86_64-dosxc-` for 64-bit and you're all set.

## Hello World
It's a good idea to test the cross compiler before porting programs. Here's a "Hello" to test it out:

```c
#include <stdio.h>

int main(int argc, char** argv) {
    printf("Hello world\n");
}
```

Build with:
```bat
gcc -s hello.c -o hello
```

Out should come a `HELLO.EXE` which you can run on DOS:
```bat
.\HELLO
REM or .\HELLO.EXE
```