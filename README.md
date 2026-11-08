# DOS-XC
DOS-XC (DOS eXtended Core) is a full-fledged modern DOS operating system. It can run your 16-bit DOS programs, or new modern 32-bit and 64-bit programs. In summary, it's like DOS but awesome.

## Get it
Every version has an accompanying GitHub release, so you can start there: https://github.com/bendaws/dos-xc/releases/latest

- Download `DOS-XC-<version>-x64.iso` for 64-bit systems, or `DOS-XC-<version>-x32.iso` for 32-bit systems.

## Building
```sh
toolchain/build-toolchain.sh x86_64   # optional: GCC for the x64 edition (\GCC)
./build.sh                             # build/DOS-XC-x32.iso, build/DOS-XC-x64.iso, ...
./boot64.sh                            # run in QEMU (MEMORY=1024 MiB by default)
```

## Documentation
- [docs/PROGRAMS.md](docs/PROGRAMS.md): the 32/64-bit program format, runtime and C library
- [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md): using and building GCC on DOS-XC
