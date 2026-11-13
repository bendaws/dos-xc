# OpenSSH for DOS-XC

Builds the OpenSSH 10.0p1 client tools as the `OpenSSH` package: `SSH`, `SCP`, `SFTP`, `SSH-KEYGEN`, `SSH-ADD`, `SSH-AGENT` and `SSH-KEYSCAN`, with OpenSSL 3.5.9 linked in. There is no server (`sshd`).

- `build.sh [x86_64|i686]` compiles it into `build/ports`.
- `package.sh [32] [64]` packs that as a `.dsp`.

Keys, `known_hosts` and `config` live in `%HOME%\.ssh`. `dosxc.patch` skips the file permission checks (FAT has none, so every file looks world-readable) and leaves out SSHFP DNS lookups (`VerifyHostKeyDNS`). `scp` and `sftp` start `ssh` with `fork`, so on the x86 edition only `ssh` itself works.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). OpenSSH's licence (BSD-style) is downloaded with its source and copied into the package.
