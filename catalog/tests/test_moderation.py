import io
from contextlib import closing
from pathlib import Path
import re
import sqlite3
import tempfile
import unittest

import test_catalog as fixtures
from app import create_app, PACKAGE_DELETED_NOTICE, USER_DELETED_NOTICE, SCHEMA


class ModerationTest(unittest.TestCase):
    setUp = fixtures.CatalogTest.setUp
    tearDown = fixtures.CatalogTest.tearDown
    sql = fixtures.CatalogTest.sql
    post = fixtures.CatalogTest.post
    import_blob = fixtures.CatalogTest.import_blob

    def create_user(self, name, email, publisher=False, confirmed=True):
        client = self.app.test_client()
        response = self.post('/register', {'username': name, 'email': email,
                                         'password': 'a long test password'}, client=client)
        self.assertEqual(response.status_code, 302)
        if confirmed:
            token = re.search(r'/confirm/([^\s]+)', self.mail[-1].get_content()).group(1)
            self.assertEqual(self.post('/confirm/' + token, client=client).status_code, 302)
        if publisher:
            result = self.app.test_cli_runner().invoke(args=['publisher', name])
            self.assertEqual(result.exit_code, 0, result.output)
        if confirmed:
            self.assertEqual(self.post('/login', {'username': name, 'password': 'a long test password'}, client=client).status_code, 302)
        user_id = self.sql('SELECT id FROM users WHERE email=?', (email,))[0]['id']
        return client, user_id

    def root(self):
        return self.create_user('Ben Daws', 'ben@bendaws.net')

    def publish(self, client, name='curl', version='1.0'):
        response = self.post('/publish', {'package': (io.BytesIO(fixtures.dsp(name=name, version=version)), 'package.dsp')}, client=client)
        self.assertEqual(response.status_code, 302)
        return int(response.location.rsplit('/', 1)[1])

    def test_root_requires_verified_email_and_moderation_is_admin_only(self):
        root, root_id = self.create_user('Ben Daws', 'BEN@BENDAWS.NET', confirmed=False)
        self.assertEqual(root.get('/admin').status_code, 302)
        self.assertEqual(self.post('/login', {'username': 'Ben Daws', 'password': 'a long test password'}, client=root).status_code, 401)
        token = re.search(r'/confirm/([^\s]+)', self.mail[-1].get_content()).group(1)
        self.post('/confirm/' + token, client=root)
        self.post('/login', {'username': 'Ben Daws', 'password': 'a long test password'}, client=root)
        for view in ('users', 'packages', 'admins'):
            self.assertEqual(root.get('/admin?view=' + view).status_code, 200)
        self.assertIn(b'>Admin</a>', root.get('/').data)
        ordinary, identifier = self.create_user('Other', 'other@example.com')
        self.assertEqual(ordinary.get('/admin').status_code, 403)
        self.assertEqual(self.post('/admin/emails', {'email': 'other@example.com', 'action': 'appoint'}, client=ordinary).status_code, 403)
        self.assertEqual(self.post(f'/admin/users/{root_id}/delete', client=ordinary).status_code, 403)
        self.assertEqual(self.post('/admin/packages/1/delete', client=ordinary).status_code, 403)
        self.assertEqual(root.post('/admin/emails', data={'email': 'other@example.com', 'action': 'appoint'}).status_code, 400)
        self.assertNotIn(b'other@example.com', ordinary.get(f'/users/{identifier}').data)

    def test_admin_appointments_pending_verification_and_live_revocation(self):
        root, root_id = self.root()
        self.assertEqual(self.post('/admin/emails', {'email': ' MOD@example.com ', 'action': 'appoint'}, client=root).status_code, 302)
        moderator, mod_id = self.create_user('Moderator', 'mod@example.com', confirmed=False)
        self.assertEqual(moderator.get('/admin').status_code, 302)
        token = re.search(r'/confirm/([^\s]+)', self.mail[-1].get_content()).group(1)
        self.post('/confirm/' + token, client=moderator)
        self.post('/login', {'username': 'Moderator', 'password': 'a long test password'}, client=moderator)
        self.assertEqual(moderator.get('/admin').status_code, 200)
        response = self.post('/account', client=moderator)
        api_token = re.search(rb'<pre>([^<]+)</pre>', response.data).group(1).decode()
        # Appointed admins may appoint others as requested.
        self.assertEqual(self.post('/admin/emails', {'email': 'third@example.com', 'action': 'appoint'}, client=moderator).status_code, 302)
        self.assertEqual(self.post('/admin/emails', {'email': 'mod@example.com', 'action': 'revoke'}, client=root).status_code, 302)
        self.assertEqual(moderator.get('/admin').status_code, 403)
        self.assertEqual(self.post(f'/admin/users/{root_id}/delete', client=moderator).status_code, 403)
        response = moderator.post('/api/v1/packages', headers={'Authorization': 'Bearer ' + api_token},
                                  data={'package': (io.BytesIO(fixtures.dsp()), 'curl.dsp')})
        self.assertEqual(response.status_code, 401)
        self.assertEqual(self.post('/admin/emails', {'email': 'BEN@bendaws.net', 'action': 'revoke'}, client=root).status_code, 400)
        self.assertEqual(self.post(f'/admin/users/{root_id}/delete', client=root).status_code, 400)
        self.assertEqual(self.post('/admin/emails', {'email': 'broken', 'action': 'appoint'}, client=root).status_code, 400)
        restarted = create_app(dict(self.app.config))
        self.assertEqual(len(self.sql('SELECT * FROM admin_emails WHERE email=?', ('third@example.com',))), 1)
        self.assertIsNotNone(restarted)

    def test_deleted_release_keeps_page_and_blocks_all_download_methods(self):
        root, _ = self.root()
        old = self.publish(root, version='1.9')
        latest = self.publish(root, version='1.10')
        public = self.app.test_client()
        metadata = public.get(f'/api/v1/packages/{latest}').json
        download_url = metadata['download_url']
        with public.get(download_url) as download:
            etag = download.headers['ETag']
            self.assertEqual(download.headers['Cache-Control'], 'no-store')
        self.assertEqual(self.post(f'/admin/packages/{latest}/delete', client=root).status_code, 302)
        page = public.get(f'/packages/{latest}')
        self.assertEqual(page.status_code, 200)
        self.assertIn(PACKAGE_DELETED_NOTICE.encode(), page.data)
        self.assertIn(b'class="deleted-banner"', page.data)
        self.assertNotIn(b'Download .dsp', page.data)
        tombstone = public.get(f'/api/v1/packages/{latest}').json
        self.assertTrue(tombstone['deleted'])
        self.assertFalse(tombstone['installable'])
        self.assertIsNone(tombstone['download_url'])
        for method, headers in [('GET', {}), ('HEAD', {}), ('GET', {'Range': 'bytes=0-3'}), ('GET', {'If-None-Match': etag})]:
            response = public.open(download_url, method=method, headers=headers)
            self.assertEqual(response.status_code, 410)
            self.assertEqual(response.headers['Cache-Control'], 'no-store')
            if method == 'GET':
                self.assertEqual(response.json['error'], PACKAGE_DELETED_NOTICE)
        self.assertEqual(public.get('/api/v1/packages').json['total'], 1)
        self.assertEqual(public.get('/api/v1/resolve?name=curl&arch=64').json['id'], old)
        self.assertEqual(public.get('/api/v1/resolve?name=curl&arch=64&version=1.10').status_code, 404)
        self.assertEqual(self.post(f'/admin/packages/{latest}/restore', client=root).status_code, 302)
        with public.get(download_url) as download:
            self.assertEqual(download.data, fixtures.dsp(version='1.10'))
        self.assertEqual(public.get('/api/v1/resolve?name=curl&arch=64').json['id'], latest)

    def test_deleted_user_retains_page_revokes_credentials_and_blocks_packages(self):
        root, _ = self.root()
        publisher, user_id = self.create_user('Publisher', 'publisher@example.com', publisher=True)
        first = self.publish(publisher)
        second = self.publish(publisher, name='another')
        self.post(f'/admin/packages/{first}/delete', client=root)
        response = self.post('/account', client=publisher)
        token = re.search(rb'<pre>([^<]+)</pre>', response.data).group(1).decode()
        self.post('/admin/emails', {'email': 'publisher@example.com', 'action': 'appoint'}, client=root)
        self.assertEqual(self.post(f'/admin/users/{user_id}/delete', client=root).status_code, 302)
        public = self.app.test_client()
        profile = public.get(f'/users/{user_id}')
        self.assertEqual(profile.status_code, 200)
        self.assertIn(b'This user&#39;s account has been deleted', profile.data)
        self.assertNotIn(b'publisher@example.com', profile.data)
        self.assertEqual(public.get(f'/packages/{second}').status_code, 200)
        self.assertIn(b'This user&#39;s account has been deleted', public.get(f'/packages/{second}').data)
        self.assertEqual(public.get(f'/api/v1/packages/{second}/download').status_code, 410)
        self.assertEqual(public.get('/api/v1/packages').json['total'], 0)
        self.assertEqual(public.get('/api/v1/resolve?name=another&arch=64').status_code, 404)
        self.assertEqual(publisher.get('/account').status_code, 302)
        self.assertEqual(publisher.get('/admin').status_code, 302)
        self.assertEqual(self.post('/login', {'username': 'Publisher', 'password': 'a long test password'}, client=publisher).status_code, 401)
        response = publisher.post('/api/v1/packages', headers={'Authorization': 'Bearer ' + token})
        self.assertEqual(response.status_code, 401)
        row = self.sql('SELECT * FROM users WHERE id=?', (user_id,))[0]
        self.assertIsNone(row['session_key'])
        self.assertIsNone(row['api_token'])
        self.assertFalse(row['publisher'])
        self.assertFalse(self.sql('SELECT 1 FROM admin_emails WHERE email=?', ('publisher@example.com',)))
        self.assertEqual(self.post('/admin/emails', {'email': 'publisher@example.com', 'action': 'appoint'}, client=root).status_code, 400)
        self.assertEqual(self.post(f'/admin/users/{user_id}/grant-publisher', client=root).status_code, 400)
        self.post(f'/admin/users/{user_id}/restore', client=root)
        # Restoring the user leaves explicitly deleted releases and permissions alone.
        self.assertEqual(public.get(f'/api/v1/packages/{first}/download').status_code, 410)
        with public.get(f'/api/v1/packages/{second}/download') as download:
            self.assertEqual(download.status_code, 200)
        self.assertEqual(self.sql('SELECT publisher FROM users WHERE id=?', (user_id,))[0][0], 0)
        self.assertEqual(publisher.get('/account').status_code, 302)

    def test_publishing_permissions_and_invalid_moderation_targets(self):
        root, _ = self.root()
        ordinary, user_id = self.create_user('Ordinary', 'ordinary@example.com')
        self.assertEqual(ordinary.get('/publish').status_code, 403)
        self.post(f'/admin/users/{user_id}/grant-publisher', client=root)
        self.assertEqual(ordinary.get('/publish').status_code, 200)
        self.post(f'/admin/users/{user_id}/revoke-publisher', client=root)
        self.assertEqual(ordinary.get('/publish').status_code, 403)
        self.assertEqual(self.post('/admin/users/999/delete', client=root).status_code, 404)
        self.assertEqual(self.post('/admin/packages/999/delete', client=root).status_code, 404)
        self.assertEqual(self.post(f'/admin/users/{user_id}/erase', client=root).status_code, 400)
        self.assertEqual(self.post('/admin/packages/1/erase', client=root).status_code, 400)
        for view in ('users', 'packages', 'admins'):
            self.assertEqual(root.get('/admin?view=' + view + '&q=ordinary').status_code, 200)
        self.assertEqual(root.get('/admin?page=bad').status_code, 400)
        self.assertEqual(root.get('/admin?view=unknown').status_code, 400)

    def test_deleted_unconfirmed_accounts_cannot_reactivate_with_old_link(self):
        root, _ = self.root()
        pending, user_id = self.create_user('Pending', 'pending@example.com', confirmed=False)
        token = re.search(r'/confirm/([^\s]+)', self.mail[-1].get_content()).group(1)
        self.post(f'/admin/users/{user_id}/delete', client=root)
        self.assertEqual(pending.get('/confirm/' + token).status_code, 400)
        count = len(self.mail)
        self.post('/resend', {'email': 'pending@example.com'}, client=pending)
        self.assertEqual(len(self.mail), count)

    def test_existing_database_is_migrated_without_changing_releases(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / 'catalog.sqlite3'
            with closing(sqlite3.connect(database)) as connection:
                with connection:
                    connection.executescript(SCHEMA)
                    connection.execute('INSERT INTO namespaces VALUES (?,NULL)', ('legacy',))
                    connection.execute('''INSERT INTO packages(name,version,arch,metadata,sha256,size,published)
                                         VALUES ('legacy','1',64,'{}','abc',1,1)''')
            config = dict(self.app.config, DATA_DIR=directory)
            upgraded = create_app(config)
            client = upgraded.test_client()
            result = client.get('/api/v1/packages/1')
            self.assertEqual(result.status_code, 200)
            self.assertEqual(result.json['name'], 'legacy')
            self.assertTrue(result.json['installable'])
            # Running startup again must be safe as well.
            create_app(config)
