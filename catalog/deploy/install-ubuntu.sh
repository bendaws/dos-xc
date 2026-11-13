#!/bin/bash
# Run from an extracted release: sudo bash deploy/install-ubuntu.sh [--apache]
set -euo pipefail
catalog_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
configure_apache=0
if [ "${1:-}" = '--apache' ]; then configure_apache=1; shift; fi
if [ "$#" -ne 0 ]; then echo 'Usage: sudo bash deploy/install-ubuntu.sh [--apache]' >&2; exit 1; fi
if [ "$(id -u)" -ne 0 ]; then echo 'Run this installer with sudo.' >&2; exit 1; fi
if [ ! -f "$catalog_root/.env" ]; then
    echo "Place your existing .env in $catalog_root/.env, then run this installer again." >&2
    exit 1
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else "Python 3.11+ is required (Ubuntu 24.04 or newer recommended).")'
apt-get update
apt-get install -y python3-venv apache2 curl
if ! getent group dosxc-catalog >/dev/null; then groupadd --system dosxc-catalog; fi
if ! id dosxc-catalog >/dev/null 2>&1; then
    useradd --system --gid dosxc-catalog --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin dosxc-catalog
fi
usermod -aG dosxc-catalog dosxc-catalog
python3 -m venv "$catalog_root/.venv"
"$catalog_root/.venv/bin/pip" install -r "$catalog_root/requirements.txt"
# Preserve the environment file's contents; give only the service group read access.
chgrp dosxc-catalog "$catalog_root/.env"
chmod 640 "$catalog_root/.env"
runuser -u dosxc-catalog -- test -r "$catalog_root/app.py" || {
    echo 'The service user cannot read this directory. Extract the ZIP under /opt/dosxc-catalog.' >&2; exit 1;
}
"$catalog_root/.venv/bin/python" "$catalog_root/deploy/install-service.py" "$catalog_root"
systemd-analyze verify /etc/systemd/system/dosxc-catalog.service
runuser -u dosxc-catalog -- "$catalog_root/.venv/bin/python" "$catalog_root/deploy/manage.py" --help >/dev/null
systemctl daemon-reload
systemctl enable dosxc-catalog
systemctl restart dosxc-catalog
if [ "$configure_apache" -eq 1 ]; then
    a2enmod proxy proxy_http headers
    catalog_site=/etc/apache2/sites-available/dosxc-catalog.conf
    if [ ! -e "$catalog_site" ]; then
        if apache2ctl -S 2>/dev/null | grep -Fq dos-packages.bendaws.net; then
            echo 'An Apache virtual host already serves dos-packages.bendaws.net.'
            echo "Add the proxy directives from $catalog_root/deploy/apache2.conf to that virtual host."
        else
            install -m 644 "$catalog_root/deploy/apache2.conf" "$catalog_site"
            a2ensite dosxc-catalog
        fi
    else
        echo "Keeping existing Apache configuration: $catalog_site"
    fi
    apache2ctl configtest
    systemctl reload apache2
fi
echo 'Catalog service installed. Check: sudo systemctl status dosxc-catalog'
echo 'For HTTPS on a new Apache host: sudo certbot --apache -d dos-packages.bendaws.net --redirect'
echo 'The installer preserved .env and all existing database/package files.'
