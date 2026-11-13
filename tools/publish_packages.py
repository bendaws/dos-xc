#!/usr/bin/env python3
#
# DOS-XC /tools/publish_packages.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import argparse
import json
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request

DEFAULT_CATALOG = 'https://dos-packages.bendaws.net'


class CatalogError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def call(method, url, token, body=None, content_type=None):
    headers = {'Authorization': 'Bearer ' + token, 'Accept': 'application/json', 'User-Agent': 'dosxc-publish'}
    if content_type:
        headers['Content-Type'] = content_type
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=600) as response:
            text = response.read()
    except urllib.error.HTTPError as exc:
        try:
            message = json.loads(exc.read()).get('error')
        except ValueError:
            message = None
        raise CatalogError(exc.code, message or f'HTTP {exc.code}') from exc
    except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
        raise CatalogError(None, str(exc)) from exc
    return json.loads(text) if text else {}


# Chunks that fail on the network resume from the offset the catalog committed.
def publish(path, catalog, token):
    size = path.stat().st_size
    upload = call('POST', catalog + '/api/v1/uploads', token, json.dumps({'size': size}).encode(), 'application/json')
    url = f'{catalog}/api/v1/uploads/{upload["id"]}'
    try:
        with path.open('rb') as source:
            offset, failures = 0, 0
            while offset < size:
                source.seek(offset)
                chunk = source.read(upload['chunk_size'])
                try:
                    offset = call('PUT', f'{url}?offset={offset}', token, chunk, 'application/octet-stream')['received']
                    failures = 0
                except CatalogError as exc:
                    if (exc.status and exc.status != 409 and exc.status < 500) or failures >= 5:
                        raise
                    failures += 1
                    time.sleep(2 ** failures)
                    offset = call('GET', url, token)['received']
                print(f'\r  {path.name}: {offset * 100 // size}%', end='', file=sys.stderr, flush=True)
        print(file=sys.stderr)
        return call('POST', url + '/complete', token)
    except BaseException:
        try:
            call('DELETE', url, token)
        except CatalogError:
            pass
        raise


def main():
    parser = argparse.ArgumentParser(description='Publish .dsp packages to a DOS-XC package catalog in chunks.')
    parser.add_argument('packages', nargs='+', type=Path)
    parser.add_argument('--catalog', default=os.environ.get('CATALOG_URL', DEFAULT_CATALOG),
                        help='Catalog origin (default: $CATALOG_URL or %(default)s)')
    parser.add_argument('--fail-existing', action='store_true', help='Treat already published releases as failures.')
    args = parser.parse_args()
    token = os.environ.get('CATALOG_TOKEN')
    if not token:
        parser.error('Set CATALOG_TOKEN to a publisher API token from your catalog account page.')
    catalog = args.catalog.rstrip('/')
    failed = 0
    for path in args.packages:
        try:
            result = publish(path, catalog, token)
            print(f'{path.name}: published {result["name"]} {result["version"]} ({result["arch"]}-bit), package {result["id"]}')
        except CatalogError as exc:
            existing = exc.status == 409 and not args.fail_existing
            print(f'{path.name}: {"skipped" if existing else "failed"}: {exc}', file=sys.stderr)
            failed += not existing
        except OSError as exc:
            print(f'{path.name}: failed: {exc}', file=sys.stderr)
            failed += 1
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
