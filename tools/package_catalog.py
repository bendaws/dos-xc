#!/usr/bin/env python3
"""Build a deployment ZIP; include the operator's .env only when requested."""
import argparse
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILES = (
    'app.py', 'packages.py', 'requirements.txt', 'gunicorn.conf.py',
    'README.md', 'DEPLOY-UBUNTU.md', '.env.example',
    'deploy/apache2.conf', 'deploy/install-ubuntu.sh', 'deploy/install-service.py',
    'deploy/manage.py', 'tests/test_catalog.py', 'tests/test_moderation.py', 'tests/test_service_unit.py',
)


def build(output, include_env=False):
    catalog = ROOT / 'catalog'
    paths = [catalog / name for name in FILES]
    if include_env:
        paths.append(catalog / '.env')
    paths += sorted((catalog / 'templates').glob('*.html'))
    paths += sorted((catalog / 'static').glob('*.css'))
    paths += sorted((catalog / 'static').glob('*.svg'))
    paths += sorted((catalog / 'static').glob('*.js'))
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if include_env:
        # Do not leave an existing public archive readable while rewriting it.
        output.touch(mode=0o600, exist_ok=True)
        output.chmod(0o600)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in paths:
            if path.is_symlink() or not path.is_file():
                raise ValueError(f'Not a regular source file: {path.name}')
            name = path.relative_to(catalog).as_posix()
            if name == '.env':
                entry = zipfile.ZipInfo.from_file(path, name)
                entry.external_attr = 0o100600 << 16
                entry.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(entry, path.read_bytes())
            else:
                archive.write(path, name)
        archive.write(ROOT / 'LICENSE.txt', 'LICENSE.txt')
    with zipfile.ZipFile(output) as archive:
        if archive.testzip():
            raise ValueError('Archive integrity check failed.')
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', nargs='?', default=ROOT / 'build/releases/dosxc-package-catalog-ubuntu.zip')
    parser.add_argument('--include-env', action='store_true', help='Include the existing catalog/.env in this private deployment ZIP.')
    args = parser.parse_args()
    print(build(args.output, include_env=args.include_env))
