#!/usr/bin/env python3
"""Run real PKG code against a real local catalog, adapting only OS calls/paths."""
import hashlib
import io
import os
from pathlib import Path
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'catalog'), str(ROOT / 'tools')]
from app import create_app
import dsp
from flask import request
from werkzeug.serving import make_server, WSGIRequestHandler


class QuietHandler(WSGIRequestHandler):
    def log_request(self, *args):
        pass


class NetworkPackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='dosxc-pkg-host-')
        cls.binary = Path(cls.build.name) / 'pkg'
        common = ['clang', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                  '-fsanitize=address,undefined', '-I' + str(ROOT / 'tests/pkg_host'),
                  '-include', str(ROOT / 'tests/pkg_host/fs.h')]
        obj = Path(cls.build.name) / 'pkg.o'
        subprocess.run(common + ['-Dmain=pkg_main', '-DCATALOG_CHUNK=4096UL', '-c', str(ROOT / 'os/pkg/pkg.c'), '-o', str(obj)], check=True)
        sources = ['tests/pkg_host/adapter.c', 'tests/pkg_host/main.c', 'os/pkg/repo.c',
                   'os/pkg/sha256.c', 'os/pkg/dsp.c', 'os/pkg/inflate.c', 'os/pkg/deflate.c', 'os/pkg/unxz.c']
        subprocess.run(common + [str(obj)] + [str(ROOT / s) for s in sources] + ['-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dosxc-network-test-')
        self.root = Path(self.temp.name)
        self.drive = self.root / 'drive'
        for directory in ['SYSTEM', 'Programs/curl/BIN', 'TEMP']:
            (self.drive / directory).mkdir(parents=True, exist_ok=True)
        (self.drive / 'Programs/curl/BIN/CURL.EXE').write_bytes(b'host curl adapter')
        (self.drive / 'Programs/curl/CACERT.PEM').touch()
        self.app = create_app({'TESTING': True, 'SECRET_KEY': secrets.token_hex(32),
                               'DATA_DIR': str(self.root / 'data'), 'BASE_URL': 'http://127.0.0.1',
                               'SESSION_COOKIE_SECURE': False, 'PROXY_HOPS': 0})
        self.mode = None
        self.ranges = []

        @self.app.before_request
        def intercept():
            if request.endpoint == 'download':
                self.ranges.append(request.headers.get('Range'))
                if self.mode == 'no-range':
                    request.environ.pop('HTTP_RANGE', None)
                if self.mode == 'short-chunk' and request.headers.get('Range', '').startswith('bytes=4096-'):
                    return b'short', 206
            if request.endpoint == 'resolve' and self.mode == 'bad-index':
                return 'NextPage=0\n[Package]\nName=bad\nVersion=1\nArch=64\n[/Package]\n'
            if request.endpoint == 'download' and self.mode == 'deleted-after-resolve':
                return {'error': 'This package has been deleted.'}, 410

        self.server = make_server('127.0.0.1', 0, self.app, threaded=True, request_handler=QuietHandler)
        self.origin = f'http://127.0.0.1:{self.server.server_port}'
        self.app.config['BASE_URL'] = self.origin
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.env = dict(os.environ, DOSXC_TEST_ROOT=str(self.drive), TMPDIR=str(self.drive / 'TEMP'),
                        PKG_REPOSITORY=self.origin, PKG_ALLOW_HTTP='1')

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def publish(self, name='hello', version='1.0', arch=64, content=b'first release', **metadata):
        source = self.root / 'source'
        source.mkdir(exist_ok=True)
        (source / 'HELLO.TXT').write_bytes(content)
        package = self.root / 'build.dsp'
        dsp.pack(source, package, dict(Name=name, Version=version, Arch=str(arch), Description='Network test', **metadata))
        result = self.app.test_cli_runner().invoke(args=['import-packages', str(package)])
        self.assertEqual(result.exit_code, 0, result.output)
        return package.read_bytes()

    def run_pkg(self, *args, ok=True, env=None):
        result = subprocess.run([str(self.binary), *args], cwd=self.drive, env=env or self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertNotIn('AddressSanitizer', result.stderr)
        self.assertNotIn('runtime error:', result.stderr)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(list((self.drive / 'TEMP').iterdir()), 'Download temporary files leaked')
        return result

    def sql(self, query, args=()):
        connection = sqlite3.connect(self.app.config['DATABASE'])
        try:
            with connection:
                return connection.execute(query, args).fetchall()
        finally:
            connection.close()

    def test_sha256_known_and_boundary_vectors(self):
        for size in (0, 1, 3, 55, 56, 63, 64, 65, 511, 8192, 8193, 1000000):
            content = b'abc' if size == 3 else secrets.token_bytes(size)
            file = self.drive / 'hash.bin'
            file.write_bytes(content)
            result = self.run_pkg('--sha256', str(file))
            self.assertEqual(result.stdout.strip(), hashlib.sha256(content).hexdigest())

    def test_fetch_search_show_install_and_update_real_files(self):
        original = self.publish(name='Hello World')
        self.assertIn('Hello World', self.run_pkg('SEARCH', 'Hello').stdout)
        self.assertIn('Version:     1.0', self.run_pkg('SHOW', 'Hello World').stdout)
        destination = self.drive / 'download.dsp'
        self.run_pkg('FETCH', 'Hello World', str(destination))
        self.assertEqual(destination.read_bytes(), original)
        self.run_pkg('FETCH', 'Hello World', str(destination), ok=False)
        self.assertEqual(destination.read_bytes(), original)
        self.run_pkg('INSTALL', 'Hello World', '/Y')
        installed = self.drive / 'Programs/Hello World'
        self.assertEqual((installed / 'HELLO.TXT').read_bytes(), b'first release')
        self.publish(name='Hello World', version='1.10', content=b'new version')
        self.run_pkg('UPDATE', 'Hello World', '/Y')
        self.assertEqual((installed / 'HELLO.TXT').read_bytes(), b'new version')
        self.assertIn('Version=1.10', (installed / 'PACKAGE.INF').read_text())
        self.assertFalse((self.drive / 'Programs/Hello World.old').exists())
        self.run_pkg('UPDATE', 'Hello World', '/Y', ok=False)

    def test_large_packages_download_in_ranged_chunks(self):
        original = self.publish(name='Large', content=secrets.token_bytes(10000))
        self.assertGreater(len(original), 8192)
        destination = self.drive / 'large.dsp'
        self.run_pkg('FETCH', 'Large', str(destination))
        self.assertEqual(destination.read_bytes(), original)
        self.assertEqual(self.ranges, ['bytes=0-4095', 'bytes=4096-8191', f'bytes=8192-{len(original) - 1}'])
        destination.unlink()
        self.mode = 'no-range'
        self.run_pkg('FETCH', 'Large', str(destination))
        self.assertEqual(destination.read_bytes(), original)
        destination.unlink()
        self.mode = 'short-chunk'
        self.assertIn('incomplete download', self.run_pkg('FETCH', 'Large', str(destination), ok=False).stdout)
        self.assertFalse(destination.exists())

    def test_compatible_architectures_pagination_and_gui_index(self):
        for i in range(27):
            self.publish(name=f'program{i}', arch=16 if i % 2 else 64)
        self.publish(name='wrongarch', arch=32)
        result = self.run_pkg('SEARCH')
        self.assertIn('27 package(s)', result.stdout)
        self.assertNotIn('wrongarch', result.stdout)
        index = self.drive / 'catalog.inf'
        self.run_pkg('CATALOG', str(index))
        self.assertEqual(index.read_text().count('[Package]'), 27)
        self.run_pkg('INSTALL', 'program1', '/Y')  # 16-bit on the 64-bit edition
        self.assertTrue((self.drive / 'Programs/program1/HELLO.TXT').exists())
        self.run_pkg('INSTALL', 'wrongarch', '/Y', ok=False)

    def test_deleted_release_user_and_deletion_after_resolution(self):
        self.publish()
        self.mode = 'deleted-after-resolve'
        result = self.run_pkg('INSTALL', 'hello', '/Y', ok=False)
        self.assertIn('deleted', result.stdout)
        self.assertFalse((self.drive / 'Programs/hello').exists())
        self.mode = None
        self.sql('UPDATE packages SET deleted=1')
        self.run_pkg('INSTALL', 'hello', '/Y', ok=False)
        self.sql('UPDATE packages SET deleted=0')
        self.sql("INSERT INTO users(username,email,password,verified,deleted) VALUES ('Deleted','gone@example.com','unused',1,1)")
        self.sql("UPDATE namespaces SET owner=(SELECT id FROM users WHERE username='Deleted')")
        self.run_pkg('FETCH', 'hello', ok=False)
        self.assertFalse((self.drive / 'Programs/hello').exists())

    def test_corrupt_download_and_invalid_catalog_never_install(self):
        self.publish()
        digest = self.sql('SELECT sha256 FROM packages')[0][0]
        artifact = self.root / 'data/packages' / (digest + '.dsp')
        content = bytearray(artifact.read_bytes())
        content[-10] ^= 1
        artifact.write_bytes(content)
        result = self.run_pkg('INSTALL', 'hello', '/Y', ok=False)
        self.assertIn('SHA-256', result.stdout)
        self.assertFalse((self.drive / 'Programs/hello').exists())
        self.mode = 'bad-index'
        self.run_pkg('INSTALL', 'bad', '/Y', ok=False)
        self.assertFalse((self.drive / 'Programs/bad').exists())

    def test_network_config_and_existing_dependency_checks(self):
        self.publish(Requires='missing>=1')
        self.run_pkg('INSTALL', 'hello', '/Y', ok=False)
        self.assertFalse((self.drive / 'Programs/hello').exists())
        self.run_pkg('SEARCH', ok=False, env=dict(self.env, PKG_ALLOW_HTTP='0'))
        self.run_pkg('SEARCH', ok=False, env=dict(self.env, PKG_REPOSITORY=self.origin + '/unexpected'))
        self.run_pkg('FETCH', 'bad;name', ok=False)
        (self.drive / 'Programs/curl/BIN/CURL.EXE').unlink()
        self.assertIn('need curl', self.run_pkg('SEARCH', ok=False).stdout)

    def test_https_certificate_verification(self):
        self.publish()
        key, cert, config = (self.root / name for name in ('key.pem', 'cert.pem', 'openssl.conf'))
        config.write_text('[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n[dn]\nCN=127.0.0.1\n[ext]\nsubjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\n')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-config', str(config), '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
        secure = make_server('127.0.0.1', 0, self.app, threaded=True, request_handler=QuietHandler, ssl_context=(cert, key))
        thread = threading.Thread(target=secure.serve_forever, daemon=True)
        thread.start()
        try:
            env = dict(self.env, PKG_REPOSITORY=f'https://127.0.0.1:{secure.server_port}', PKG_ALLOW_HTTP='0')
            self.run_pkg('FETCH', 'hello', ok=False, env=env)
            shutil.copyfile(cert, self.drive / 'Programs/curl/CACERT.PEM')
            self.run_pkg('FETCH', 'hello', env=env)
            self.assertTrue((self.drive / 'hello-1.0-64.dsp').is_file())
        finally:
            secure.shutdown()
            secure.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main(verbosity=2)
