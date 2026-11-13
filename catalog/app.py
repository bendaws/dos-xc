"""Self-hosted DOS-XC package catalog. See README.md for configuration."""
import functools
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import smtplib
import sqlite3
import ssl
import tempfile
import time
from datetime import timedelta
from email.message import EmailMessage
from functools import cmp_to_key
from urllib.parse import urlsplit

import click
from flask import (Flask, abort, flash, g, jsonify, redirect, render_template,
                   request, send_file, session, url_for)
from werkzeug.exceptions import HTTPException, SecurityError
from werkzeug.security import check_password_hash, generate_password_hash
from werkzeug.middleware.proxy_fix import ProxyFix

from packages import compare_versions, inspect_package

ROOT_ADMIN_EMAIL = 'ben@bendaws.net'
PACKAGE_DELETED_NOTICE = 'This package has been deleted and their packages can not be installed over the network with pkg.'
USER_DELETED_NOTICE = "This user's account has been deleted and their packages can not be installed over the network with pkg."
PACKAGE_SELECT = '''SELECT p.*, n.owner AS owner_id, u.username AS owner_username,
    COALESCE(u.deleted, 0) AS owner_deleted FROM packages p
    JOIN namespaces n ON n.name=p.name LEFT JOIN users u ON u.id=n.owner'''
ACTIVE_PACKAGE = 'p.deleted=0 AND COALESCE(u.deleted, 0)=0'
# Chunks stay well below proxy request limits such as Cloudflare's 100 MB.
CHUNK_SIZE = 50 * 1024**2
UPLOAD_LIFETIME = 86400
MAX_ACTIVE_UPLOADS = 10
MAX_FORM_FILES = 20
UPLOAD_ID = re.compile(r'[0-9a-f]{32}')


def valid_email(email):
    return (len(email) <= 254 and re.fullmatch(r'[^\s@<>]+@[^\s@<>]+\.[^\s@<>]+', email)
            and not any(ord(c) < 32 for c in email))


SCHEMA = '''
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users (
 id INTEGER PRIMARY KEY, username TEXT NOT NULL COLLATE NOCASE UNIQUE,
 email TEXT NOT NULL COLLATE NOCASE UNIQUE, password TEXT NOT NULL,
 verified INTEGER NOT NULL DEFAULT 0, publisher INTEGER NOT NULL DEFAULT 0,
 confirmation TEXT, confirmation_expires INTEGER, api_token TEXT,
 session_key TEXT
);
CREATE TABLE IF NOT EXISTS namespaces (
 name TEXT PRIMARY KEY COLLATE NOCASE, owner INTEGER REFERENCES users(id)
);
CREATE TABLE IF NOT EXISTS packages (
 id INTEGER PRIMARY KEY, name TEXT NOT NULL COLLATE NOCASE REFERENCES namespaces(name),
 version TEXT NOT NULL COLLATE NOCASE, arch INTEGER NOT NULL,
 metadata TEXT NOT NULL, sha256 TEXT NOT NULL, size INTEGER NOT NULL,
 published INTEGER NOT NULL, UNIQUE(name, version, arch)
);
CREATE TABLE IF NOT EXISTS rate_limits (
 key TEXT PRIMARY KEY, started INTEGER NOT NULL, hits INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS admin_emails (
 email TEXT PRIMARY KEY COLLATE NOCASE
);
CREATE TABLE IF NOT EXISTS uploads (
 id TEXT PRIMARY KEY, owner INTEGER NOT NULL REFERENCES users(id),
 size INTEGER NOT NULL, received INTEGER NOT NULL DEFAULT 0,
 created INTEGER NOT NULL, completing INTEGER NOT NULL DEFAULT 0
);
'''


def hash_token(token):
    return hashlib.sha256(token.encode()).hexdigest()


def create_app(config=None):
    app = Flask(__name__)
    base = os.environ.get('CATALOG_BASE_URL', 'http://127.0.0.1:5000').rstrip('/')
    app.config.update(
        SECRET_KEY=os.environ.get('CATALOG_SECRET_KEY'),
        BASE_URL=base,
        DATA_DIR=os.environ.get('CATALOG_DATA_DIR', str(Path(__file__).parent / 'instance')),
        MAX_CONTENT_LENGTH=256 * 1024**2,
        MAX_PACKAGE_SIZE=256 * 1024**2,
        MAX_FORM_MEMORY_SIZE=128 * 1024,
        MAX_FORM_PARTS=MAX_FORM_FILES + 10,
        SESSION_COOKIE_HTTPONLY=True,
        SESSION_COOKIE_SECURE=os.environ.get('CATALOG_COOKIE_SECURE', '1') != '0',
        SESSION_COOKIE_SAMESITE='Lax',
        PERMANENT_SESSION_LIFETIME=timedelta(hours=12),
        SMTP_HOST=os.environ.get('SMTP_HOST', ''),
        SMTP_PORT=int(os.environ.get('SMTP_PORT', '587')),
        SMTP_SECURITY=os.environ.get('SMTP_SECURITY', 'starttls'),
        SMTP_USERNAME=os.environ.get('SMTP_USERNAME', ''),
        SMTP_PASSWORD=os.environ.get('SMTP_PASSWORD', ''),
        SMTP_FROM=os.environ.get('SMTP_FROM', ''),
        PROXY_HOPS=int(os.environ.get('CATALOG_PROXY_HOPS', '0')),
    )
    if config:
        app.config.update(config)
    if app.config['PROXY_HOPS'] not in (0, 1):
        raise RuntimeError('CATALOG_PROXY_HOPS must be 0 (direct) or 1 (one trusted reverse proxy).')
    if app.config['PROXY_HOPS']:
        app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=0, x_host=0, x_port=0, x_prefix=0)
    if not app.config['SECRET_KEY'] or len(app.config['SECRET_KEY']) < 32:
        raise RuntimeError('Set CATALOG_SECRET_KEY to a persistent random secret of at least 32 characters.')
    parsed = urlsplit(app.config['BASE_URL'])
    if parsed.scheme not in ('https', 'http') or not parsed.hostname or parsed.path or parsed.query or parsed.fragment or parsed.username:
        raise RuntimeError('CATALOG_BASE_URL must be an HTTP(S) origin without a path.')
    app.config['TRUSTED_HOSTS'] = [parsed.hostname]
    data = Path(app.config['DATA_DIR'])
    data.mkdir(parents=True, exist_ok=True)
    (data / 'packages').mkdir(exist_ok=True)
    (data / 'uploads').mkdir(exist_ok=True)
    app.config['DATABASE'] = str(data / 'catalog.sqlite3')

    def db():
        if 'db' not in g:
            g.db = sqlite3.connect(app.config['DATABASE'], timeout=30)
            g.db.row_factory = sqlite3.Row
            g.db.execute('PRAGMA foreign_keys=ON')
        return g.db

    @app.teardown_appcontext
    def close_db(_error):
        if 'db' in g:
            g.db.close()

    with app.app_context():
        db().executescript(SCHEMA)
        # Upgrade existing catalogs in place; serialize migrations across workers.
        with db():
            db().execute('BEGIN IMMEDIATE')
            for table in ('users', 'packages'):
                columns = {row['name'] for row in db().execute(f'PRAGMA table_info({table})')}
                if 'deleted' not in columns:
                    db().execute(f'ALTER TABLE {table} ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0')
            db().execute('INSERT OR IGNORE INTO admin_emails(email) VALUES (?)', (ROOT_ADMIN_EMAIL,))

    def rate_limit(action, limit, seconds=3600, identity=None):
        now = int(time.time())
        key = hash_token(f'{action}:{identity or request.remote_addr}')
        with db():
            db().execute('DELETE FROM rate_limits WHERE started < ?', (now - 86400,))
            row = db().execute('''INSERT INTO rate_limits VALUES (?, ?, 1)
                ON CONFLICT(key) DO UPDATE SET
                hits=CASE WHEN started <= ? THEN 1 ELSE hits+1 END,
                started=CASE WHEN started <= ? THEN excluded.started ELSE started END
                RETURNING hits''', (key, now, now - seconds, now - seconds)).fetchone()
        if row['hits'] > limit:
            abort(429, 'Too many attempts. Please try again later.')

    def csrf():
        if 'csrf' not in session:
            session['csrf'] = secrets.token_urlsafe(32)
        return session['csrf']

    @app.before_request
    def load_user():
        g.user = None
        g.is_admin = False
        if session.get('user_id'):
            user = db().execute('SELECT * FROM users WHERE id=?', (session['user_id'],)).fetchone()
            if user and user['verified'] and not user['deleted'] and user['session_key'] and hmac.compare_digest(user['session_key'], session.get('key', '')):
                g.user = user
                g.is_admin = db().execute('SELECT 1 FROM admin_emails WHERE email=?', (user['email'],)).fetchone() is not None
        if request.method == 'POST' and not request.path.startswith('/api/'):
            token = request.form.get('csrf', '')
            if not token or not hmac.compare_digest(token, session.get('csrf', '')):
                abort(400, 'The form expired. Reload the page and try again.')

    @app.context_processor
    def template_context():
        return {'csrf': csrf, 'user': g.user, 'is_admin': g.is_admin,
                'can_publish': bool(g.user and (g.user['publisher'] or g.is_admin)),
                'root_admin_email': ROOT_ADMIN_EMAIL}

    @app.after_request
    def headers(response):
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['X-Frame-Options'] = 'DENY'
        response.headers['Referrer-Policy'] = 'same-origin'
        response.headers['Content-Security-Policy'] = "default-src 'self'; style-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'"
        if app.config['BASE_URL'].startswith('https:'):
            response.headers['Strict-Transport-Security'] = 'max-age=31536000'
        if request.endpoint != 'static':
            response.headers['Cache-Control'] = 'no-store'
        return response

    @app.errorhandler(HTTPException)
    def error(exc):
        # Host validation can fail before Flask creates its URL adapter.
        if isinstance(exc, SecurityError):
            return 'Invalid host.', 400, {'Content-Type': 'text/plain; charset=utf-8'}
        if request.path.startswith('/api/'):
            return jsonify(error=exc.description), exc.code
        return render_template('error.html', error=exc), exc.code

    def login_required(fn):
        @functools.wraps(fn)
        def wrapped(*args, **kwargs):
            if not g.user:
                return redirect(url_for('login'))
            return fn(*args, **kwargs)
        return wrapped

    def admin_required(fn):
        @functools.wraps(fn)
        def wrapped(*args, **kwargs):
            if not g.user:
                return redirect(url_for('login'))
            if not g.is_admin:
                abort(403, 'Administrator access is required.')
            return fn(*args, **kwargs)
        return wrapped

    def confirmation(user):
        token = secrets.token_urlsafe(32)
        message = EmailMessage()
        message['Subject'] = 'Confirm your DOS-XC Package Catalog account'
        message['From'] = app.config['SMTP_FROM']
        message['To'] = user['email']
        message.set_content(
            f'Hello {user["username"]},\n\n'
            'Confirm your email address to activate your DOS-XC Package Catalog account:\n\n'
            f'{app.config["BASE_URL"]}/confirm/{token}\n\n'
            'This link expires in 24 hours and can be used once.\n'
            'If you did not create this account, you can ignore this email.\n\n'
            'DOS-XC Package Catalog\n')
        if not app.config['SMTP_HOST'] or not app.config['SMTP_FROM']:
            raise OSError('SMTP is not configured')
        mode = app.config['SMTP_SECURITY']
        if mode not in ('ssl', 'starttls', 'none'):
            raise OSError('Invalid SMTP_SECURITY')
        # Keep the transaction through sending so a link cannot race its token commit.
        with db():
            db().execute('UPDATE users SET confirmation=?, confirmation_expires=? WHERE id=?',
                         (hash_token(token), int(time.time()) + 86400, user['id']))
            cls = smtplib.SMTP_SSL if mode == 'ssl' else smtplib.SMTP
            kwargs = {'timeout': 15}
            if mode == 'ssl':
                kwargs['context'] = ssl.create_default_context()
            with cls(app.config['SMTP_HOST'], app.config['SMTP_PORT'], **kwargs) as smtp:
                if mode == 'starttls':
                    smtp.starttls(context=ssl.create_default_context())
                if app.config['SMTP_USERNAME']:
                    smtp.login(app.config['SMTP_USERNAME'], app.config['SMTP_PASSWORD'])
                smtp.send_message(message)

    @app.route('/register', methods=['GET', 'POST'])
    def register():
        if request.method == 'POST':
            rate_limit('register', 10)
            username = request.form.get('username', '').strip()
            email = request.form.get('email', '').strip().lower()
            password = request.form.get('password', '')
            if not re.fullmatch(r'[A-Za-z0-9 ]{1,40}', username):
                abort(400, 'Username must contain 1–40 letters, numbers or spaces.')
            if not valid_email(email):
                abort(400, 'Enter a valid email address.')
            if not 12 <= len(password) <= 256:
                abort(400, 'Password must contain 12–256 characters.')
            try:
                with db():
                    row = db().execute('INSERT INTO users(username,email,password) VALUES (?,?,?) RETURNING *',
                                       (username, email, generate_password_hash(password))).fetchone()
            except sqlite3.IntegrityError:
                flash('That username or email is already registered. Sign in or resend your confirmation.')
                return redirect(url_for('register'))
            try:
                confirmation(row)
            except (OSError, smtplib.SMTPException):
                app.logger.error('Confirmation delivery failed; check SMTP configuration.')
                flash('Your account was saved, but email delivery failed. Use Resend confirmation once mail is available.')
                return redirect(url_for('resend'))
            flash('Check your email for a confirmation link before signing in.')
            return redirect(url_for('login'))
        return render_template('auth.html', mode='register')

    @app.route('/resend', methods=['GET', 'POST'])
    def resend():
        if request.method == 'POST':
            rate_limit('resend', 5)
            email = request.form.get('email', '').strip().lower()
            rate_limit('resend-email', 3, identity=email)
            user = db().execute('SELECT * FROM users WHERE email=? AND verified=0 AND deleted=0', (email,)).fetchone()
            if user:
                try:
                    confirmation(user)
                except (OSError, smtplib.SMTPException):
                    app.logger.error('Confirmation delivery failed; check SMTP configuration.')
            flash('If an unconfirmed account matches, a confirmation email has been requested.')
            return redirect(url_for('resend'))
        return render_template('auth.html', mode='resend')

    @app.route('/confirm/<token>', methods=['GET', 'POST'])
    def confirm(token):
        user = db().execute('SELECT * FROM users WHERE confirmation=? AND confirmation_expires>? AND verified=0 AND deleted=0',
                            (hash_token(token), int(time.time()))).fetchone()
        if not user:
            abort(400, 'This confirmation link is invalid or expired. Request another from Resend confirmation.')
        if request.method == 'POST':
            with db():
                db().execute('UPDATE users SET verified=1, confirmation=NULL, confirmation_expires=NULL WHERE id=?', (user['id'],))
            flash('Email confirmed. You can now sign in.')
            return redirect(url_for('login'))
        return render_template('auth.html', mode='confirm')

    # A real hash keeps unknown-account checks as expensive as known-account checks.
    dummy_password = generate_password_hash(secrets.token_urlsafe(32))

    @app.route('/login', methods=['GET', 'POST'])
    def login():
        if request.method == 'POST':
            rate_limit('login', 30, seconds=900)
            name = request.form.get('username', '').strip()
            rate_limit('login-account', 15, seconds=900, identity=name.lower())
            user = db().execute('SELECT * FROM users WHERE username=?', (name,)).fetchone()
            password = request.form.get('password', '')
            valid = len(password) <= 256 and check_password_hash(user['password'] if user else dummy_password, password)
            if not user or not valid or not user['verified'] or user['deleted']:
                flash('Sign-in failed. Check your username, password and email confirmation.')
                return render_template('auth.html', mode='login'), 401
            key = secrets.token_urlsafe(32)
            with db():
                result = db().execute('UPDATE users SET session_key=? WHERE id=? AND verified=1 AND deleted=0', (key, user['id']))
                if not result.rowcount:
                    abort(403, USER_DELETED_NOTICE)
            session.clear()
            session.permanent = True
            session.update(user_id=user['id'], key=key)
            return redirect(url_for('account'))
        return render_template('auth.html', mode='login')

    @app.post('/logout')
    def logout():
        if g.user:
            with db():
                db().execute('UPDATE users SET session_key=NULL WHERE id=?', (g.user['id'],))
        session.clear()
        return redirect(url_for('index'))

    @app.route('/account', methods=['GET', 'POST'])
    @login_required
    def account():
        token = None
        if request.method == 'POST':
            if not (g.user['publisher'] or g.is_admin):
                abort(403, 'Publisher permission is required.')
            token = secrets.token_urlsafe(40)
            with db():
                result = db().execute('''UPDATE users SET api_token=? WHERE id=? AND deleted=0 AND verified=1
                    AND session_key=? AND (publisher=1 OR email IN (SELECT email FROM admin_emails))''',
                    (hash_token(token), g.user['id'], session.get('key')))
                if not result.rowcount:
                    abort(403, 'Publishing permission is required.')
        return render_template('account.html', token=token)

    def query_packages():
        q = request.args.get('q', '').strip()[:160]
        arch = request.args.get('arch', '')
        if arch and arch not in ('16', '32', '64'):
            abort(400, 'arch must be 16, 32 or 64.')
        try:
            page = int(request.args.get('page', '1'))
            if not 1 <= page <= 1000000:
                raise ValueError
        except ValueError:
            abort(400, 'page must be a positive integer.')
        where, values = [ACTIVE_PACKAGE], []
        if q:
            where.append("(instr(lower(p.name), lower(?))>0 OR instr(lower(p.metadata), lower(?))>0)")
            values += [q, q]
        if arch:
            where.append('p.arch IN (?,16)' if request.args.get('compatible') == '1' else 'p.arch=?')
            values.append(int(arch))
        condition = ' WHERE ' + ' AND '.join(where) if where else ''
        total = db().execute('SELECT count(*) FROM (' + PACKAGE_SELECT + condition + ')', values).fetchone()[0]
        rows = db().execute(PACKAGE_SELECT + condition + ' ORDER BY p.published DESC, p.id DESC LIMIT 25 OFFSET ?',
                            values + [(page - 1) * 25]).fetchall()
        return rows, total, page, q, arch

    def serialize(row):
        meta = json.loads(row['metadata'])
        installable = not row['deleted'] and not row['owner_deleted']
        return dict(id=row['id'], name=row['name'], version=row['version'], arch=row['arch'],
                    description=meta.get('description', ''), author=meta.get('author', ''),
                    requires=meta.get('requires', ''), provides=meta.get('provides', ''), metadata=meta,
                    sha256=row['sha256'], size=row['size'], published=row['published'],
                    owner_id=row['owner_id'], owner_username=row['owner_username'],
                    deleted=bool(row['deleted']), owner_deleted=bool(row['owner_deleted']), installable=installable,
                    deletion_message=PACKAGE_DELETED_NOTICE if row['deleted'] else USER_DELETED_NOTICE if row['owner_deleted'] else None,
                    download_url=app.config['BASE_URL'] + url_for('download', package_id=row['id']) if installable else None)

    def package(package_id):
        row = db().execute(PACKAGE_SELECT + ' WHERE p.id=?', (package_id,)).fetchone()
        if not row:
            abort(404, 'Package not found.')
        return row

    @app.get('/')
    def index():
        rows, total, page, q, arch = query_packages()
        return render_template('index.html', packages=[serialize(r) for r in rows], total=total, page=page, q=q, arch=arch)

    @app.get('/packages/<int:package_id>')
    def details(package_id):
        return render_template('details.html', package=serialize(package(package_id)))

    @app.get('/users/<int:user_id>')
    def user_profile(user_id):
        profile = db().execute('''SELECT u.id,u.username,u.email,u.verified,u.publisher,u.deleted,
            EXISTS(SELECT 1 FROM admin_emails a WHERE a.email=u.email) AS appointed_admin
            FROM users u WHERE u.id=?''', (user_id,)).fetchone()
        if not profile:
            abort(404, 'User not found.')
        page = page_number()
        rows = db().execute(PACKAGE_SELECT + ' WHERE n.owner=? ORDER BY p.published DESC,p.id DESC LIMIT 25 OFFSET ?',
                            (user_id, (page - 1) * 25)).fetchall()
        total = db().execute('SELECT count(*) FROM packages p JOIN namespaces n ON n.name=p.name WHERE n.owner=?', (user_id,)).fetchone()[0]
        return render_template('user.html', profile=profile, packages=[serialize(r) for r in rows],
                               deletion_message=USER_DELETED_NOTICE if profile['deleted'] else None, page=page, total=total)

    @app.get('/api/v1/packages')
    def api_packages():
        rows, total, page, _, _ = query_packages()
        if request.args.get('format') == 'inf':
            return inf_response(rows, next_page=page + 1 if page * 25 < total else 0)
        return jsonify(packages=[serialize(r) for r in rows], total=total, page=page, per_page=25,
                       next_page=page + 1 if page * 25 < total else None)

    @app.get('/api/v1/packages/<int:package_id>')
    def api_package(package_id):
        row = package(package_id)
        if request.args.get('format') == 'inf':
            if row['deleted'] or row['owner_deleted']:
                abort(410, PACKAGE_DELETED_NOTICE if row['deleted'] else USER_DELETED_NOTICE)
            return inf_response([row])
        return jsonify(serialize(row))

    @app.get('/api/v1/resolve')
    def resolve():
        name, arch = request.args.get('name', ''), request.args.get('arch', '')
        if not name or arch not in ('16', '32', '64'):
            abort(400, 'name and arch (16, 32 or 64) are required.')
        # 16-bit DOS programs can also run on either native edition.
        rows = db().execute(PACKAGE_SELECT + ' WHERE p.name=? AND p.arch IN (?,16) AND ' + ACTIVE_PACKAGE,
                            (name, int(arch))).fetchall()
        version = request.args.get('version')
        if version:
            rows = [r for r in rows if r['version'].lower() == version.lower()]
        if not rows:
            abort(404, 'No compatible package found.')
        def compare(a, b):
            return compare_versions(a['version'], b['version']) or ((a['arch'] == int(arch)) - (b['arch'] == int(arch)))
        row = max(rows, key=cmp_to_key(compare))
        if request.args.get('format') == 'inf':
            return inf_response([row])
        return jsonify(serialize(row))

    def inf_response(rows, next_page=0):
        # Native PKG reuses its existing PACKAGE.INF reader for this representation.
        text = [f'NextPage={next_page}\n']
        for row in rows:
            text.append('[Package]\n')
            for key, value in json.loads(row['metadata']).items():
                for part in value.split('\n'):
                    text.append(f'{key}={part}\n')
            text.extend([f'Id={row["id"]}\n', f'Size={row["size"]}\n', f'SHA256={row["sha256"]}\n',
                         f'Download=/api/v1/packages/{row["id"]}/download\n', '[/Package]\n'])
        return app.response_class(''.join(text), content_type='text/plain; charset=utf-8')

    @app.get('/api/v1/packages/<int:package_id>/download')
    def download(package_id):
        row = package(package_id)
        if row['deleted'] or row['owner_deleted']:
            abort(410, PACKAGE_DELETED_NOTICE if row['deleted'] else USER_DELETED_NOTICE)
        path = data / 'packages' / (row['sha256'] + '.dsp')
        if not path.is_file():
            abort(503, 'Package storage is unavailable.')
        return send_file(path, mimetype='application/octet-stream', as_attachment=True,
                         download_name=f'{row["name"]}-{row["version"]}-{row["arch"]}.dsp',
                         conditional=True, etag=row['sha256'], max_age=0)

    def store(path, owner=None):
        info, digest = inspect_package(path)
        size = Path(path).stat().st_size
        with db():
            # Serialize namespace claiming with package insertion across workers.
            db().execute('BEGIN IMMEDIATE')
            if owner is not None:
                publisher = db().execute('SELECT * FROM users WHERE id=? AND verified=1 AND deleted=0', (owner,)).fetchone()
                admin = publisher and db().execute('SELECT 1 FROM admin_emails WHERE email=?', (publisher['email'],)).fetchone()
                if not publisher or not (publisher['publisher'] or admin):
                    raise PermissionError('Publishing permission is required.')
            namespace = db().execute('SELECT owner FROM namespaces WHERE name=?', (info['name'],)).fetchone()
            if namespace and owner is not None and namespace['owner'] != owner:
                raise PermissionError('This package name belongs to another publisher or the administrator.')
            if namespace is None:
                db().execute('INSERT INTO namespaces VALUES (?,?)', (info['name'], owner))
            if db().execute('SELECT 1 FROM packages WHERE name=? AND version=? AND arch=?',
                            (info['name'], info['version'], int(info['arch']))).fetchone():
                raise FileExistsError('This release already exists. Publish a new version instead.')
            target = data / 'packages' / (digest + '.dsp')
            # Hard-link a completed temporary copy: readers never see a partial file.
            fd, temporary = tempfile.mkstemp(dir=data / 'packages', prefix='.import-')
            try:
                with os.fdopen(fd, 'wb') as output, open(path, 'rb') as source:
                    shutil.copyfileobj(source, output)
                try:
                    os.link(temporary, target)
                except FileExistsError:
                    pass
            finally:
                os.unlink(temporary)
            result = db().execute('''INSERT INTO packages(name,version,arch,metadata,sha256,size,published)
                VALUES (?,?,?,?,?,?,?) RETURNING id''',
                (info['name'], info['version'], int(info['arch']), json.dumps(info), digest, size, int(time.time()))).fetchone()
        return result['id']

    def store_upload(path, owner):
        try:
            return store(path, owner)
        except ValueError as exc:
            abort(400, str(exc))
        except PermissionError as exc:
            abort(403, str(exc))
        except FileExistsError as exc:
            abort(409, str(exc))

    def save_upload(upload, owner):
        if not upload or not upload.filename.lower().endswith('.dsp'):
            abort(400, 'Choose a .dsp package file.')
        fd, temporary = tempfile.mkstemp(dir=data, prefix='.upload-')
        try:
            with os.fdopen(fd, 'wb') as output:
                upload.save(output)
            return store_upload(temporary, owner)
        finally:
            os.unlink(temporary)

    def publish_limit(user):
        rate_limit('publish', 200, identity=f'user:{user["id"]}')

    @app.route('/publish', methods=['GET', 'POST'])
    @login_required
    def publish():
        if not (g.user['publisher'] or g.is_admin):
            abort(403, 'Ask the catalog administrator for publisher permission.')
        if request.method == 'POST':
            uploads = [upload for upload in request.files.getlist('package') if upload.filename]
            if not uploads:
                abort(400, 'Choose a .dsp package file.')
            if len(uploads) > MAX_FORM_FILES:
                abort(400, f'Publish up to {MAX_FORM_FILES} packages at once.')
            if len(uploads) == 1:
                publish_limit(g.user)
                return redirect(url_for('details', package_id=save_upload(uploads[0], g.user['id'])))
            results = []
            for upload in uploads:
                try:
                    publish_limit(g.user)
                    results.append(dict(filename=upload.filename,
                                        package=serialize(package(save_upload(upload, g.user['id'])))))
                except HTTPException as exc:
                    results.append(dict(filename=upload.filename, error=exc.description))
            status = 200 if any('package' in result for result in results) else 400
            return render_template('publish.html', results=results, **publish_limits()), status
        return render_template('publish.html', **publish_limits())

    def publish_limits():
        return dict(chunk_size=CHUNK_SIZE, max_size=app.config['MAX_PACKAGE_SIZE'], max_files=MAX_FORM_FILES)

    def api_publisher():
        authorization = request.headers.get('Authorization', '')
        if authorization.startswith('Bearer '):
            user = db().execute('''SELECT * FROM users WHERE api_token=? AND verified=1 AND deleted=0
                                AND (publisher=1 OR email IN (SELECT email FROM admin_emails))''',
                                (hash_token(authorization[7:]),)).fetchone()
            if not user:
                abort(401, 'Invalid publisher token.')
            return user
        # The publish page authenticates with its session and the form's CSRF token.
        token = request.headers.get('X-CSRF-Token', '')
        if g.user and token and hmac.compare_digest(token, session.get('csrf', '')):
            if not (g.user['publisher'] or g.is_admin):
                abort(403, 'Publishing permission is required.')
            return g.user
        abort(401, 'A publisher bearer token is required.')

    @app.post('/api/v1/packages')
    def api_publish():
        rate_limit('api-publish', 60)
        user = api_publisher()
        publish_limit(user)
        package_id = save_upload(request.files.get('package'), user['id'])
        return jsonify(serialize(package(package_id))), 201

    def upload_path(upload_id):
        return data / 'uploads' / (upload_id + '.part')

    def upload_status(row):
        return dict(id=row['id'], size=row['size'], received=row['received'], chunk_size=CHUNK_SIZE,
                    expires=row['created'] + UPLOAD_LIFETIME)

    def owned_upload(upload_id, user):
        row = None
        if UPLOAD_ID.fullmatch(upload_id):
            row = db().execute('SELECT * FROM uploads WHERE id=? AND owner=? AND created>?',
                               (upload_id, user['id'], int(time.time()) - UPLOAD_LIFETIME)).fetchone()
        if not row:
            abort(404, 'Upload not found or expired.')
        return row

    def discard_upload(upload_id):
        with db():
            db().execute('DELETE FROM uploads WHERE id=?', (upload_id,))
        upload_path(upload_id).unlink(missing_ok=True)

    @app.post('/api/v1/uploads')
    def create_upload():
        user = api_publisher()
        rate_limit('upload', 200, identity=f'user:{user["id"]}')
        size = (request.get_json(silent=True) or {}).get('size')
        if type(size) is not int or size <= 0:
            abort(400, 'size must be the package size in bytes.')
        if size > app.config['MAX_PACKAGE_SIZE']:
            abort(413, f'Packages can be up to {app.config["MAX_PACKAGE_SIZE"] // 1024**2} MiB.')
        upload_id = secrets.token_hex(16)
        now = int(time.time())
        with db():
            db().execute('BEGIN IMMEDIATE')
            stale = db().execute('DELETE FROM uploads WHERE created<=? RETURNING id', (now - UPLOAD_LIFETIME,)).fetchall()
            active = db().execute('SELECT count(*) FROM uploads WHERE owner=?', (user['id'],)).fetchone()[0]
            if active < MAX_ACTIVE_UPLOADS:
                upload_path(upload_id).touch(exist_ok=False)
                row = db().execute('INSERT INTO uploads(id,owner,size,created) VALUES (?,?,?,?) RETURNING *',
                                   (upload_id, user['id'], size, now)).fetchone()
        for old in stale:
            upload_path(old['id']).unlink(missing_ok=True)
        if active >= MAX_ACTIVE_UPLOADS:
            abort(429, 'Too many unfinished uploads. Finish or cancel one first.')
        return jsonify(upload_status(row)), 201

    @app.get('/api/v1/uploads/<upload_id>')
    def get_upload(upload_id):
        return jsonify(upload_status(owned_upload(upload_id, api_publisher())))

    @app.put('/api/v1/uploads/<upload_id>')
    def upload_chunk(upload_id):
        user = api_publisher()
        rate_limit('upload-chunk', 2000, identity=f'user:{user["id"]}')
        row = owned_upload(upload_id, user)
        try:
            offset = int(request.args.get('offset', ''))
        except ValueError:
            abort(400, 'offset is required.')
        length = request.content_length
        if length is None:
            abort(411, 'Content-Length is required.')
        if not 0 < length <= CHUNK_SIZE:
            abort(413, f'Chunks must contain 1–{CHUNK_SIZE} bytes.')
        if offset != row['received'] or row['completing']:
            return jsonify(error='This chunk does not continue the upload.', received=row['received']), 409
        if offset + length > row['size']:
            abort(400, 'This chunk extends past the declared upload size.')
        # Receive the body before taking the write lock; slow clients must not block other workers.
        fd, temporary = tempfile.mkstemp(dir=data / 'uploads', prefix='.chunk-')
        try:
            with os.fdopen(fd, 'w+b') as chunk:
                copied = 0
                while copied < length:
                    block = request.stream.read(min(1024**2, length - copied))
                    if not block:
                        abort(400, 'The chunk ended before its Content-Length.')
                    chunk.write(block)
                    copied += len(block)
                chunk.seek(0)
                with db():
                    db().execute('BEGIN IMMEDIATE')
                    current = db().execute('SELECT * FROM uploads WHERE id=?', (upload_id,)).fetchone()
                    if not current:
                        abort(404, 'Upload not found or expired.')
                    if current['received'] != offset or current['completing']:
                        return jsonify(error='This chunk does not continue the upload.', received=current['received']), 409
                    # Truncating discards bytes from an earlier attempt that never committed.
                    with open(upload_path(upload_id), 'r+b') as part:
                        part.seek(offset)
                        part.truncate()
                        shutil.copyfileobj(chunk, part)
                    row = db().execute('UPDATE uploads SET received=? WHERE id=? RETURNING *',
                                       (offset + length, upload_id)).fetchone()
        finally:
            os.unlink(temporary)
        return jsonify(upload_status(row))

    @app.post('/api/v1/uploads/<upload_id>/complete')
    def complete_upload(upload_id):
        user = api_publisher()
        publish_limit(user)
        owned_upload(upload_id, user)
        with db():
            claimed = db().execute('''UPDATE uploads SET completing=1
                WHERE id=? AND completing=0 AND received=size''', (upload_id,)).rowcount
        if not claimed:
            abort(409, 'The upload is incomplete or already being published.')
        try:
            package_id = store_upload(upload_path(upload_id), user['id'])
        finally:
            discard_upload(upload_id)
        return jsonify(serialize(package(package_id))), 201

    @app.delete('/api/v1/uploads/<upload_id>')
    def cancel_upload(upload_id):
        owned_upload(upload_id, api_publisher())
        with db():
            cancelled = db().execute('DELETE FROM uploads WHERE id=? AND completing=0', (upload_id,)).rowcount
        if not cancelled:
            abort(409, 'The upload is already being published.')
        upload_path(upload_id).unlink(missing_ok=True)
        return '', 204

    def page_number():
        try:
            page = int(request.args.get('page', '1'))
            if not 1 <= page <= 1000000:
                raise ValueError
            return page
        except ValueError:
            abort(400, 'page must be a positive integer.')

    @app.get('/admin')
    @admin_required
    def admin_panel():
        view = request.args.get('view', 'users')
        if view not in ('users', 'packages', 'admins'):
            abort(400, 'Unknown admin view.')
        q = request.args.get('q', '').strip()[:160]
        page = page_number()
        if view == 'users':
            selection = '''SELECT u.id,u.username,u.email,u.verified,u.publisher,u.deleted,
                EXISTS(SELECT 1 FROM admin_emails a WHERE a.email=u.email) AS appointed_admin FROM users u
                WHERE instr(lower(u.username),lower(?))>0 OR instr(lower(u.email),lower(?))>0'''
            values = [q, q]
            order = ' ORDER BY u.id DESC'
        elif view == 'packages':
            selection = PACKAGE_SELECT + ' WHERE instr(lower(p.name),lower(?))>0'
            values = [q]
            order = ' ORDER BY p.published DESC,p.id DESC'
        else:
            selection = '''SELECT a.email,u.username,u.id AS user_id,u.verified,u.deleted
                FROM admin_emails a LEFT JOIN users u ON u.email=a.email
                WHERE instr(lower(a.email),lower(?))>0'''
            values = [q]
            order = ' ORDER BY a.email'
        total = db().execute('SELECT count(*) FROM (' + selection + ')', values).fetchone()[0]
        rows = db().execute(selection + order + ' LIMIT 25 OFFSET ?', values + [(page - 1) * 25]).fetchall()
        if view == 'packages':
            rows = [serialize(row) for row in rows]
        return render_template('admin.html', view=view, rows=rows, total=total, page=page, q=q)

    def ensure_current_admin():
        # Recheck under the write lock, including revocations from other workers.
        active = db().execute('''SELECT 1 FROM users u JOIN admin_emails a ON a.email=u.email
                             WHERE u.id=? AND u.verified=1 AND u.deleted=0''', (g.user['id'],)).fetchone()
        if not active:
            abort(403, 'Administrator access is required.')

    @app.post('/admin/emails')
    @admin_required
    def admin_email():
        email = request.form.get('email', '').strip().lower()
        action = request.form.get('action')
        if not valid_email(email) or action not in ('appoint', 'revoke'):
            abort(400, 'Enter a valid email and action.')
        if email == ROOT_ADMIN_EMAIL and action == 'revoke':
            abort(400, 'The primary administrator cannot be removed.')
        with db():
            db().execute('BEGIN IMMEDIATE')
            ensure_current_admin()
            if action == 'appoint':
                target = db().execute('SELECT deleted FROM users WHERE email=?', (email,)).fetchone()
                if target and target['deleted']:
                    abort(400, 'Restore this account before appointing it as an administrator.')
                db().execute('INSERT OR IGNORE INTO admin_emails VALUES (?)', (email,))
            else:
                db().execute('DELETE FROM admin_emails WHERE email=?', (email,))
        flash('Administrator appointed.' if action == 'appoint' else 'Administrator removed.')
        return redirect(url_for('admin_panel', view='admins'))

    @app.post('/admin/users/<int:user_id>/<action>')
    @admin_required
    def moderate_user(user_id, action):
        if action not in ('delete', 'restore', 'grant-publisher', 'revoke-publisher'):
            abort(400, 'Unknown moderation action.')
        with db():
            db().execute('BEGIN IMMEDIATE')
            ensure_current_admin()
            target = db().execute('SELECT * FROM users WHERE id=?', (user_id,)).fetchone()
            if not target:
                abort(404, 'User not found.')
            if action == 'delete':
                if target['email'].lower() == ROOT_ADMIN_EMAIL or user_id == g.user['id']:
                    abort(400, 'You cannot delete the primary administrator or your own account.')
                db().execute('''UPDATE users SET deleted=1,publisher=0,api_token=NULL,session_key=NULL,
                             confirmation=NULL,confirmation_expires=NULL WHERE id=?''', (user_id,))
                db().execute('DELETE FROM admin_emails WHERE email=?', (target['email'],))
            elif action == 'restore':
                db().execute('UPDATE users SET deleted=0 WHERE id=?', (user_id,))
            else:
                if target['deleted'] or not target['verified']:
                    abort(400, 'Publishing access requires an active, confirmed account.')
                db().execute('UPDATE users SET publisher=?,api_token=NULL WHERE id=?',
                             (int(action == 'grant-publisher'), user_id))
        flash('Account updated.')
        return redirect(url_for('user_profile', user_id=user_id))

    @app.post('/admin/packages/<int:package_id>/<action>')
    @admin_required
    def moderate_package(package_id, action):
        if action not in ('delete', 'restore'):
            abort(400, 'Unknown moderation action.')
        with db():
            db().execute('BEGIN IMMEDIATE')
            ensure_current_admin()
            result = db().execute('UPDATE packages SET deleted=? WHERE id=?', (int(action == 'delete'), package_id))
            if not result.rowcount:
                abort(404, 'Package not found.')
        flash('Package deleted.' if action == 'delete' else 'Package restored.')
        return redirect(url_for('details', package_id=package_id))

    @app.get('/api')
    def api_help():
        return render_template('api.html', base=app.config['BASE_URL'])

    @app.cli.command('import-packages')
    @click.argument('paths', nargs=-1, required=True, type=click.Path(exists=True, dir_okay=False))
    def import_packages(paths):
        """Import trusted build artifacts; names are reserved to the administrator."""
        for path in paths:
            try:
                identifier = store(path)
                click.echo(f'Imported {Path(path).name}: package {identifier}')
            except (ValueError, PermissionError, FileExistsError) as exc:
                raise click.ClickException(f'{Path(path).name}: {exc}') from exc

    @app.cli.command('publisher')
    @click.argument('username')
    @click.option('--revoke', is_flag=True, help='Revoke publishing and invalidate the API token.')
    def publisher(username, revoke):
        """Grant publishing to an email-confirmed account (or revoke it)."""
        with db():
            result = db().execute('UPDATE users SET publisher=?, api_token=NULL WHERE username=? AND verified=1 AND deleted=0',
                                 (0 if revoke else 1, username))
            if result.rowcount != 1:
                raise click.ClickException('No email-confirmed account with that username.')
        click.echo('Publishing revoked.' if revoke else 'Publishing granted.')

    return app
