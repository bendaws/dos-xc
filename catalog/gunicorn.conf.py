"""Gunicorn configuration for an extracted, self-hosted release."""
import os
from pathlib import Path

from dotenv import load_dotenv

load_dotenv(os.environ.get('CATALOG_ENV_FILE', Path(__file__).with_name('.env')), override=False)
bind = '127.0.0.1:8080'
workers = 2
timeout = 180
accesslog = None
errorlog = '-'
