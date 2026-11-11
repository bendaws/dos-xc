# curl for DOS-XC

Builds curl 8.11.1 with OpenSSL as the `curl` package, for HTTP and HTTPS, with Mozilla's root certificates as `CACERT.PEM`.

- `build.sh [x86_64|i686]` compiles it into `build/ports`.
- `package.sh [32] [64]` packs that as a `.dsp`.

## Licence

The build scripts in this folder are BSD 3-Clause ([LICENSE.txt](LICENSE.txt)). curl has its own licence, which is downloaded with its source and copied into the package.
