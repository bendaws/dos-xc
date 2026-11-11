#!/bin/sh
#
# DOS-XC /curl/package.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
for edition in $EDITIONS; do
    use_edition "$edition"
    package_port curl Version=8.11.1 Author="Daniel Stenberg and the curl contributors" \
        Description="HTTP and HTTPS transfer tool" Path=BIN Env=CURL_CA_BUNDLE=%PACKAGE%/CACERT.PEM
done
