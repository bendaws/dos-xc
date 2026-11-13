import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('install_service', Path(__file__).resolve().parents[1] / 'deploy/install-service.py')
service = importlib.util.module_from_spec(spec)
spec.loader.exec_module(service)


class ServiceUnitTest(unittest.TestCase):
    def test_var_www_working_directory_is_an_unquoted_absolute_path(self):
        unit = service.render_unit(Path('/var/www'), Path('/var/lib/dosxc-catalog'))
        self.assertIn('\nWorkingDirectory=/var/www\n', unit)
        self.assertNotIn('WorkingDirectory="', unit)
        self.assertIn('ExecStart="/var/www/.venv/bin/gunicorn"', unit)
        self.assertIn('ReadWritePaths="/var/lib/dosxc-catalog"', unit)

    def test_paths_with_spaces_and_literal_systemd_specifiers(self):
        unit = service.render_unit(Path('/srv/My Catalog 100%'), Path('/var/lib/My Catalog'))
        self.assertIn('\nWorkingDirectory=/srv/My Catalog 100%%\n', unit)
        self.assertIn('ExecStart="/srv/My Catalog 100%%/.venv/bin/gunicorn"', unit)
        self.assertIn('ReadWritePaths="/var/lib/My Catalog"', unit)

    def test_invalid_working_directories_are_rejected(self):
        for path in ('relative/path', '/srv/bad\npath', '/srv/bad\x00path', '/srv/trailing ', '/srv/trailing\\'):
            with self.subTest(path=repr(path)), self.assertRaises(SystemExit):
                service.working_directory(path)

    @unittest.skipUnless(shutil.which('systemd-analyze'), 'Requires systemd on Linux')
    def test_native_systemd_verifies_generated_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / '.venv/bin/gunicorn'
            executable.parent.mkdir(parents=True)
            executable.write_text('#!/bin/sh\nexit 0\n')
            executable.chmod(0o755)
            data = root / 'data'
            data.mkdir()
            unit = root / 'dosxc-catalog.service'
            unit.write_text(service.render_unit(root, data))
            result = subprocess.run(['systemd-analyze', 'verify', str(unit)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
