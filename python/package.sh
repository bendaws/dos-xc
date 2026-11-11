#!/bin/sh
#
# DOS-XC /python/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port python3 Version=3.14.6 Author="Python Software Foundation" \
        Description="Python programming language" Path=BIN Env=PYTHONHOME=%PACKAGE% Env=SSL_CERT_FILE=%PACKAGE%/CACERT.PEM
done
