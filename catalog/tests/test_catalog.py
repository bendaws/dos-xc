import hashlib
from contextlib import closing
import io
import json
import lzma
from pathlib import Path
import re
import secrets
import sqlite3
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app import create_app
from packages import compare_versions, inspect_package


def dsp(name='curl', version='1.0', arch=64, filename='BIN/HELLO.EXE', body=b'hello', fmt=2):
    metadata = f'Name={name}\r\nVersion={version}\r\nArch={arch}\r\nDescription=A test package\r\n'.encode()
    header = b'DSPK' + struct.pack('<HHI', fmt, 0, len(metadata)) + metadata
    if fmt == 1:
        stream = io.BytesIO(header)
        stream.seek(0, 2)
        with zipfile.ZipFile(stream, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(filename, body)
        return stream.getvalue()
    encoded = filename.encode()
    listing = struct.pack('<IHII', 1, len(encoded), len(body), zlib.crc32(body)) + encoded
    return header + listing + lzma.compress(body, check=lzma.CHECK_CRC32)


class CatalogTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.app = create_app(dict(TESTING=True, SECRET_KEY='test-secret-' * 4,
                                  DATA_DIR=self.directory.name, BASE_URL='https://catalog.test',
                                  SERVER_NAME='catalog.test', SESSION_COOKIE_SECURE=False,
                                  SMTP_HOST='mail.test', SMTP_FROM='catalog@example.com'))
        self.client = self.app.test_client()
        self.mail = []
        test = self

        class SMTP:
            def __init__(self, *args, **kwargs):
                pass
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def starttls(self, **kwargs):
                pass
            def send_message(self, message):
                test.mail.append(message)

        self.smtp = patch('app.smtplib.SMTP', SMTP)
        self.smtp.start()

    def tearDown(self):
        self.smtp.stop()
        self.directory.cleanup()

    def sql(self, statement, values=()):
        with closing(sqlite3.connect(self.app.config['DATABASE'])) as db:
            with db:
                db.row_factory = sqlite3.Row
                return db.execute(statement, values).fetchall()

    def post(self, path, data=None, client=None):
        client = client or self.client
        client.get('/login')
        with client.session_transaction() as session:
            token = session['csrf']
        return client.post(path, data={**(data or {}), 'csrf': token})

    def register(self, name='Ben Daws', email='ben@example.com'):
        return self.post('/register', dict(username=name, email=email, password='a long test password'))

    def confirm(self):
        message = self.mail[-1]
        token = re.search(r'/confirm/([^\s]+)', message.get_content()).group(1)
        return token, self.post('/confirm/' + token)

    def publisher(self, name='Ben Daws', email='ben@example.com'):
        self.register(name, email)
        self.confirm()
        runner = self.app.test_cli_runner()
        result = runner.invoke(args=['publisher', name])
        self.assertEqual(result.exit_code, 0, result.output)
        result = self.post('/login', dict(username=name, password='a long test password'))
        self.assertEqual(result.status_code, 302)

    def upload(self, blob=None):
        return self.post('/publish', {'package': (io.BytesIO(blob or dsp()), 'package.dsp')})

    def import_blob(self, blob):
        path = Path(self.directory.name) / 'build.dsp'
        path.write_bytes(blob)
        result = self.app.test_cli_runner().invoke(args=['import-packages', str(path)])
        self.assertEqual(result.exit_code, 0, result.output)

    def test_empty_catalog_and_pages(self):
        for path in ['/', '/login', '/register', '/resend', '/api']:
            self.assertEqual(self.client.get(path).status_code, 200)
        self.assertIn(b'No packages yet.', self.client.get('/').data)
        self.assertEqual(self.client.get('/api/v1/packages').json['packages'], [])
        self.assertEqual(self.client.get('/api/v1/packages?arch=128').status_code, 400)
        self.assertEqual(self.client.get('/?page=bad').status_code, 400)
        self.assertEqual(self.client.get('/?page=-1').status_code, 400)

    def test_registration_confirmation_and_login(self):
        self.assertEqual(self.register().status_code, 302)
        self.assertEqual(len(self.mail), 1)
        message = self.mail[0]
        self.assertEqual(message.get_content_type(), 'text/plain')
        self.assertFalse(message.is_multipart())
        self.assertIn('https://catalog.test/confirm/', message.get_content())
        self.assertNotIn('a long test password', str(message))
        token = re.search(r'/confirm/([^\s]+)', message.get_content()).group(1)
        row = self.sql('SELECT * FROM users')[0]
        self.assertNotEqual(row['password'], 'a long test password')
        self.assertNotEqual(row['confirmation'], token)
        self.assertEqual(self.client.get('/confirm/' + token).status_code, 200)
        self.assertEqual(self.sql('SELECT verified FROM users')[0][0], 0)
        self.assertEqual(self.post('/login', dict(username='Ben Daws', password='a long test password')).status_code, 401)
        self.assertEqual(self.post('/confirm/' + token).status_code, 302)
        self.assertEqual(self.post('/confirm/' + token).status_code, 400)
        self.assertEqual(self.post('/login', dict(username='ben daws', password='a long test password')).status_code, 302)
        self.assertEqual(self.client.get('/account').status_code, 200)
        self.assertEqual(self.client.get('/publish').status_code, 403)
        self.post('/logout')
        self.assertEqual(self.client.get('/account').status_code, 302)

    def test_invalid_registration_csrf_duplicates_and_token_expiry(self):
        self.assertEqual(self.client.post('/register', data={}).status_code, 400)
        self.assertEqual(self.register('Bad_Name').status_code, 400)
        self.assertEqual(self.register(email='broken').status_code, 400)
        self.register()
        self.register('BEN DAWS', 'different@example.com')
        self.assertEqual(len(self.sql('SELECT * FROM users')), 1)
        old_token = re.search(r'/confirm/([^\s]+)', self.mail[-1].get_content()).group(1)
        self.post('/resend', {'email': 'ben@example.com'})
        self.assertEqual(self.client.get('/confirm/' + old_token).status_code, 400)
        self.sql('UPDATE users SET confirmation_expires=?', (int(time.time()) - 1,))
        token = re.search(r'/confirm/([^\s]+)', self.mail[-1].get_content()).group(1)
        self.assertEqual(self.client.get('/confirm/' + token).status_code, 400)

    def test_smtp_failure_keeps_recoverable_account(self):
        with patch('app.smtplib.SMTP', side_effect=OSError('offline')):
            response = self.register()
        self.assertTrue(response.location.endswith('/resend'))
        self.assertEqual(len(self.sql('SELECT * FROM users')), 1)
        self.assertEqual(self.sql('SELECT verified FROM users')[0][0], 0)
        self.post('/resend', dict(email='ben@example.com'))
        self.assertEqual(len(self.mail), 1)
        self.confirm()

    def test_publish_download_metadata_immutable_and_pagination(self):
        self.publisher()
        content = dsp(body=b'hello' * 100)
        response = self.upload(content)
        self.assertEqual(response.status_code, 302)
        self.assertEqual(self.client.get(response.location).status_code, 200)
        listing = self.client.get('/api/v1/packages?q=curl&arch=64').json
        self.assertEqual(listing['total'], 1)
        package = listing['packages'][0]
        self.assertEqual(package['sha256'], hashlib.sha256(content).hexdigest())
        download = self.client.get(package['download_url'])
        self.assertEqual(download.data, content)
        self.assertEqual(download.headers['Content-Type'], 'application/octet-stream')
        ranged = self.client.get(package['download_url'], headers={'Range': 'bytes=0-3'})
        self.assertEqual(ranged.status_code, 206)
        self.assertEqual(ranged.data, b'DSPK')
        cached = self.client.get(package['download_url'], headers={'If-None-Match': download.headers['ETag']})
        self.assertEqual(cached.status_code, 304)
        cached.close()
        ranged.close()
        download.close()
        self.assertEqual(self.upload(content).status_code, 409)
        self.assertEqual(self.client.get('/api/v1/packages?q=unknown').json['total'], 0)
        self.assertEqual(self.client.get('/api/v1/packages/999').status_code, 404)
        for i in range(26):
            self.import_blob(dsp(name=f'Package{i}'))
        first = self.client.get('/api/v1/packages').json
        second = self.client.get('/api/v1/packages?page=2').json
        self.assertEqual((len(first['packages']), first['next_page']), (25, 2))
        self.assertEqual((len(second['packages']), second['next_page']), (2, None))

    def test_namespace_isolation_and_admin_reservation(self):
        self.import_blob(dsp(name='system'))
        self.publisher()
        self.assertEqual(self.upload(dsp(name='system', version='2')).status_code, 403)
        self.assertEqual(self.upload().status_code, 302)
        self.post('/logout')
        self.publisher('Other Person', 'other@example.com')
        self.assertEqual(self.upload(dsp(name='CURL', version='2')).status_code, 403)
        self.assertEqual(self.upload(dsp(name='other')).status_code, 302)

    def test_bearer_token_rotation_and_revocation(self):
        self.publisher()
        response = self.post('/account')
        token = re.search(rb'<pre>([^<]+)</pre>', response.data).group(1).decode()
        self.assertNotIn(token.encode(), self.client.get('/account').data)
        self.assertNotEqual(self.sql('SELECT api_token FROM users')[0][0], token)
        def api_upload(credential):
            return self.client.post('/api/v1/packages', data={'package': (io.BytesIO(dsp()), 'curl.dsp')},
                                    headers={'Authorization': 'Bearer ' + credential})
        self.assertEqual(api_upload('wrong').status_code, 401)
        self.assertEqual(api_upload(token).status_code, 201)
        self.post('/account')
        self.assertEqual(api_upload(token).status_code, 401)
        result = self.app.test_cli_runner().invoke(args=['publisher', '--revoke', 'Ben Daws'])
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(self.client.get('/publish').status_code, 403)

    def test_resolution_matches_pkg_and_architecture(self):
        for version, arch in [('1.9', 64), ('1.10', 64), ('8.0', 32), ('1.8', 16)]:
            self.import_blob(dsp(version=version, arch=arch))
        self.assertEqual(self.client.get('/api/v1/resolve?name=CURL&arch=64').json['version'], '1.10')
        self.assertEqual(self.client.get('/api/v1/resolve?name=curl&arch=16').json['version'], '1.8')
        self.assertEqual(self.client.get('/api/v1/resolve?name=curl&arch=64&version=1.9').json['version'], '1.9')
        self.assertEqual(self.client.get('/api/v1/resolve?name=missing&arch=64').status_code, 404)
        self.assertEqual(self.client.get('/api/v1/resolve?name=curl').status_code, 400)
        self.assertGreater(compare_versions('1.10', '1.9'), 0)
        self.assertEqual(compare_versions('1.01', '1.1'), 0)
        self.assertLess(compare_versions('1', '1.0'), 0)

    def test_bad_archives_are_rejected(self):
        self.publisher()
        for content in [b'junk', dsp()[:-10], dsp(filename='../escape'), dsp(filename='/absolute'),
                        dsp(filename='C:/FILE'), dsp(filename='foo\\bar'), dsp(body=b'a' * 100)[:-3]]:
            self.assertEqual(self.upload(content).status_code, 400)
        self.assertEqual(self.sql('SELECT count(*) FROM packages')[0][0], 0)
        self.assertEqual(list(Path(self.directory.name).glob('.upload-*')), [])
        self.assertEqual(self.upload(dsp(fmt=1)).status_code, 302)

    def test_headers_host_validation_and_rate_limits(self):
        response = self.client.get('/')
        self.assertEqual(response.headers['X-Content-Type-Options'], 'nosniff')
        self.assertEqual(response.headers['X-Frame-Options'], 'DENY')
        self.assertEqual(self.client.get('/', headers={'Host': 'evil.test'}).status_code, 400)
        for _ in range(6):
            result = self.post('/resend', dict(email='missing@example.com'))
        self.assertEqual(result.status_code, 429)
        self.app.config['MAX_CONTENT_LENGTH'] = 1024
        result = self.client.post('/api/v1/packages', data=b'x' * 2048)
        # An unauthenticated request is rejected before its body is read.
        self.assertEqual(result.status_code, 401)

    def test_oversized_and_unauthorized_uploads(self):
        self.publisher()
        self.app.config['MAX_CONTENT_LENGTH'] = 1024
        result = self.upload(dsp(body=secrets.token_bytes(2048)))
        # Multipart overhead plus the archive exceeds 1 KiB.
        self.assertEqual(result.status_code, 413)
        self.app.config['MAX_CONTENT_LENGTH'] = 256 * 1024**2
        self.assertEqual(self.client.post('/publish', data={'package': (io.BytesIO(dsp()), 'a.dsp')}).status_code, 400)

    def test_payload_integrity_and_escaped_metadata(self):
        self.publisher()
        blob = bytearray(dsp())
        length, = struct.unpack_from('<I', blob, 8)
        blob[12 + length + 4 + 2 + 4] ^= 1  # Corrupt the listed CRC, leave XZ valid.
        self.assertEqual(self.upload(bytes(blob)).status_code, 400)
        original = dsp()
        length, = struct.unpack_from('<I', original, 8)
        metadata = original[12:12 + length].replace(b'A test package', b'<script>alert(1)</script>')
        malicious = original[:8] + struct.pack('<I', len(metadata)) + metadata + original[12 + length:]
        self.assertEqual(self.upload(malicious).status_code, 302)
        response = self.client.get('/')
        self.assertIn(b'&lt;script&gt;', response.data)
        self.assertNotIn(b'<script>', response.data)

    def test_forwarded_address_only_trusted_when_configured(self):
        for hops, expected in [(0, '127.0.0.1'), (1, '192.0.2.123')]:
            config = dict(self.app.config)
            config['PROXY_HOPS'] = hops
            app = create_app(config)
            client = app.test_client()
            client.get('/resend')
            with client.session_transaction() as session:
                csrf = session['csrf']
            response = client.post('/resend', data={'email': 'not-registered@example.com', 'csrf': csrf},
                                   headers={'X-Forwarded-For': '192.0.2.123'})
            self.assertEqual(response.status_code, 302)
            key = hashlib.sha256(f'resend:{expected}'.encode()).hexdigest()
            self.assertEqual(len(self.sql('SELECT * FROM rate_limits WHERE key=?', (key,))), 1)


    def csrf_headers(self, client=None):
        client = client or self.client
        client.get('/publish')
        with client.session_transaction() as session:
            return {'X-CSRF-Token': session['csrf']}

    def chunked(self, blob, headers, chunk=7):
        created = self.client.post('/api/v1/uploads', json={'size': len(blob)}, headers=headers)
        self.assertEqual(created.status_code, 201, created.json)
        url = '/api/v1/uploads/' + created.json['id']
        for offset in range(0, len(blob), chunk):
            response = self.client.put(f'{url}?offset={offset}', data=blob[offset:offset + chunk], headers=headers)
            self.assertEqual(response.status_code, 200, response.json)
        return url, self.client.post(url + '/complete', headers=headers)

    def test_chunked_upload_with_session_and_token(self):
        self.publisher()
        content = dsp(body=secrets.token_bytes(300))
        with patch('app.CHUNK_SIZE', 64):
            self.assertEqual(self.client.post('/api/v1/uploads', json={'size': 10}).status_code, 401)
            self.assertEqual(self.client.post('/api/v1/uploads', json={'size': 10},
                                              headers={'X-CSRF-Token': 'wrong'}).status_code, 401)
            headers = self.csrf_headers()
            url, response = self.chunked(content, headers, chunk=64)
            self.assertEqual(response.status_code, 201, response.json)
            with self.client.get(response.json['download_url']) as download:
                self.assertEqual(download.data, content)
            self.assertEqual(self.client.get(url, headers=headers).status_code, 404)
            token = re.search(rb'<pre>([^<]+)</pre>', self.post('/account').data).group(1).decode()
            bearer = {'Authorization': 'Bearer ' + token}
            _, response = self.chunked(dsp(version='2', body=secrets.token_bytes(200)), bearer, chunk=50)
            self.assertEqual(response.status_code, 201)
            _, response = self.chunked(content, bearer)
            self.assertEqual(response.status_code, 409)
            _, response = self.chunked(b'junk' * 20, bearer)
            self.assertEqual(response.status_code, 400)
        self.assertEqual(self.sql('SELECT count(*) FROM uploads')[0][0], 0)
        self.assertEqual(list((Path(self.directory.name) / 'uploads').iterdir()), [])

    def test_chunked_upload_offsets_limits_and_ownership(self):
        self.publisher()
        headers = self.csrf_headers()
        content = dsp(body=secrets.token_bytes(100))
        with patch('app.CHUNK_SIZE', 32):
            self.assertEqual(self.client.post('/api/v1/uploads', json={'size': 0}, headers=headers).status_code, 400)
            self.app.config['MAX_PACKAGE_SIZE'] = 1024
            self.assertEqual(self.client.post('/api/v1/uploads', json={'size': 1025}, headers=headers).status_code, 413)
            created = self.client.post('/api/v1/uploads', json={'size': len(content)}, headers=headers).json
            url = '/api/v1/uploads/' + created['id']
            self.assertEqual(created['chunk_size'], 32)
            self.assertEqual(self.client.put(url + '?offset=0', data=b'x' * 33, headers=headers).status_code, 413)
            self.assertEqual(self.client.put(url + '?offset=0', data=content[:32], headers=headers).json['received'], 32)
            retry = self.client.put(url + '?offset=0', data=content[:32], headers=headers)
            self.assertEqual((retry.status_code, retry.json['received']), (409, 32))
            self.assertEqual(self.client.post(url + '/complete', headers=headers).status_code, 409)
            self.post('/logout')
            self.publisher('Other Person', 'other@example.com')
            other = self.csrf_headers()
            self.assertEqual(self.client.get(url, headers=other).status_code, 404)
            self.assertEqual(self.client.delete(url, headers=other).status_code, 404)
            self.assertEqual(self.client.get('/api/v1/uploads/../../x', headers=other).status_code, 404)
            ids = [self.client.post('/api/v1/uploads', json={'size': 5}, headers=other).json.get('id') for _ in range(10)]
            self.assertTrue(all(ids))
            self.assertEqual(self.client.post('/api/v1/uploads', json={'size': 5}, headers=other).status_code, 429)
            self.assertEqual(self.client.delete('/api/v1/uploads/' + ids[0], headers=other).status_code, 204)
            self.assertFalse((Path(self.directory.name) / 'uploads' / (ids[0] + '.part')).exists())
            self.sql('UPDATE uploads SET created=created-86400')
            self.assertEqual(self.client.post('/api/v1/uploads', json={'size': 5}, headers=other).status_code, 201)
            self.assertEqual(self.sql('SELECT count(*) FROM uploads')[0][0], 1)

    def test_form_publishes_multiple_packages(self):
        self.publisher()
        response = self.post('/publish', {'package': [(io.BytesIO(dsp(name='one')), 'one.dsp'),
                                                      (io.BytesIO(b'junk'), 'bad.dsp'),
                                                      (io.BytesIO(dsp(name='two')), 'two.dsp')]})
        self.assertEqual(response.status_code, 200)
        self.assertIn(b'Published', response.data)
        self.assertIn(b'bad.dsp', response.data)
        self.assertEqual(sorted(r[0] for r in self.sql('SELECT name FROM packages')), ['one', 'two'])
        response = self.post('/publish', {'package': [(io.BytesIO(dsp(name='one')), 'one.dsp'),
                                                      (io.BytesIO(dsp(name='two')), 'two.dsp')]})
        self.assertEqual(response.status_code, 400)
        page = self.client.get('/publish')
        self.assertIn(b'multiple', page.data)
        self.assertIn(b'publish.js', page.data)
        with self.client.get('/static/publish.js') as script:
            self.assertEqual(script.status_code, 200)

if __name__ == '__main__':
    unittest.main()
