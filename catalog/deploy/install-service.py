"""Write a systemd unit with paths for this extraction. Never print env values."""
import os
from pathlib import Path
import pwd
import sys

from dotenv import load_dotenv


def quote(value, command=False):
    value = str(value)
    if any(c in value for c in '\r\n\x00'):
        raise SystemExit('Installation paths cannot contain newlines or NUL.')
    value = value.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%')
    if command:
        value = value.replace('$', '$$')
    return '"' + value + '"'


def working_directory(value):
    # Unlike ExecStart/ReadWritePaths, systemd does not unquote this setting.
    value = str(value)
    if not Path(value).is_absolute() or any(ord(c) < 32 or ord(c) == 127 for c in value) or value.endswith((' ', '\\')):
        raise SystemExit('The working directory must be an absolute path without control characters or trailing spaces/backslashes.')
    return value.replace('%', '%%')


def render_unit(root, data):
    return f'''[Unit]
Description=DOS-XC Package Catalog
After=network.target

[Service]
Type=simple
User=dosxc-catalog
Group=dosxc-catalog
WorkingDirectory={working_directory(root)}
Environment=PYTHONDONTWRITEBYTECODE=1
ExecStart={quote(root / '.venv/bin/gunicorn', command=True)} --config {quote(root / 'gunicorn.conf.py', command=True)} app:create_app()
Restart=on-failure
RestartSec=3
UMask=0027
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths={quote(data)}

[Install]
WantedBy=multi-user.target
'''


def main():
    root = Path(sys.argv[1]).resolve()
    load_dotenv(root / '.env', override=False)
    data = Path(os.environ.get('CATALOG_DATA_DIR', root / 'instance')).resolve()
    if data == root or data in (Path('/'), Path('/etc'), Path('/usr'), Path('/var'), Path('/opt'), Path('/home'), Path('/root')):
        raise SystemExit('Choose a dedicated directory for CATALOG_DATA_DIR.')
    unit = render_unit(root, data)
    account = pwd.getpwnam('dosxc-catalog')
    data.mkdir(parents=True, exist_ok=True)
    # Only the configured catalog data tree is assigned to the service user.
    os.chown(data, account.pw_uid, account.pw_gid)
    for base, directories, files in os.walk(data):
        for name in directories + files:
            path = Path(base) / name
            if not path.is_symlink():
                os.chown(path, account.pw_uid, account.pw_gid)
    Path('/etc/systemd/system/dosxc-catalog.service').write_text(unit)


if __name__ == '__main__':
    main()
