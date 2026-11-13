# Ubuntu Server / Apache2

Requires Python 3.11+ (Ubuntu 24.04 or newer recommended). Extract the ZIP into
the directory where the service should run; `/opt/dosxc-catalog` is recommended.
The private deployment ZIP built with `--include-env` contains your supplied
`.env`. It contains no accounts, package binaries or database. Keep this ZIP
private. Source-only ZIPs omit `.env`.

```sh
sudo mkdir -p /opt/dosxc-catalog
sudo unzip dosxc-package-catalog-ubuntu.zip -d /opt/dosxc-catalog
```

The included `.env` is extracted to `/opt/dosxc-catalog/.env`. Keep
`CATALOG_BASE_URL=https://dos-packages.bendaws.net`,
`CATALOG_COOKIE_SECURE=1`, and `CATALOG_PROXY_HOPS=1`. Use a dedicated persistent
directory for `CATALOG_DATA_DIR`, such as `/var/lib/dosxc-catalog`.

Install the service and Apache proxy configuration:

```sh
cd /opt/dosxc-catalog
sudo bash deploy/install-ubuntu.sh --apache
```

The installer preserves `.env` contents and existing data, installs dependencies,
and starts a `dosxc-catalog` systemd service under a dedicated user. It secures
`.env` to owner/group access, so run CLI administration as the service user.
Gunicorn loads `.env` automatically and listens only on `127.0.0.1:8080`.

If Apache already has a virtual host for the domain, the installer leaves it in
place. Copy the proxy directives from `deploy/apache2.conf` into that virtual
host (including its HTTPS virtual host), then run:

```sh
sudo a2enmod proxy proxy_http headers
sudo apache2ctl configtest
sudo systemctl reload apache2
```

For a new domain, point DNS at this server and enable HTTPS:

```sh
sudo apt-get install certbot python3-certbot-apache
sudo certbot --apache -d dos-packages.bendaws.net --redirect
```

Apache proxies every request to the application; do not use the application
directory as a static `DocumentRoot`. Keep the app port private and preserve the
forwarding-header configuration in the supplied virtual host.

Check the service and website:

```sh
sudo systemctl status dosxc-catalog
sudo journalctl -u dosxc-catalog -n 50 --no-pager
curl --fail https://dos-packages.bendaws.net/api/v1/packages
```

Import your built `.dsp` files (run from the extraction directory):

```sh
sudo -u dosxc-catalog .venv/bin/python deploy/manage.py import-packages /path/to/curl-64.dsp
```

`deploy/manage.py` loads this directory's `.env` before running Flask commands.
Register and confirm `ben@bendaws.net` to access the Admin page. That page can
appoint other admin emails and moderate packages and accounts.

For an update, back up the data directory, stop the service, extract the new ZIP
into the same directory, and rerun the installer. Use `unzip -o release.zip -d
/opt/dosxc-catalog -x .env` to retain the server's current `.env` during updates.
Runtime data is never included. Existing Apache configuration is preserved.
