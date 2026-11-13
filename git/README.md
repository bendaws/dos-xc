# Git for DOS-XC

Builds Git 2.51.0 as the `git` package for the x64 edition (Git needs `fork()`), with libcurl 8.11.1 and OpenSSL 3.5.9 linked in for HTTPS remotes. SSH remotes use the `OpenSSH` package's `ssh`.

- `build.sh [x86_64]` compiles it into `build/ports`.
- `package.sh [64]` packs that as a `.dsp`.

Paths may start with a drive (`C:/Users/me/project`): `dosxc-paths.h` switches on Git's own Windows path helpers. Git finds its helper programs relative to `GIT.EXE` (`RUNTIME_PREFIX`), and `GIT_SSL_CAINFO` points at the bundled Mozilla certificates. FAT has no symbolic links or permission bits, so `git init` sets `core.symlinks` and `core.filemode` off by itself. Shell-script commands (`git mergetool`, `git filter-branch` ...) need Bash; the Perl and Python ones, and the server programs (`git daemon`, `git shell`, `git http-backend`), are left out. Set an editor with `git config --global core.editor nano`.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). Git is GPL-2.0; its licence is copied into the package.
