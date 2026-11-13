"""Flask CLI with the extracted release's .env loaded first."""
import os
from pathlib import Path
import sys

from dotenv import load_dotenv

root = Path(__file__).resolve().parents[1]
load_dotenv(os.environ.get('CATALOG_ENV_FILE', root / '.env'), override=False)
sys.path.insert(0, str(root))
from app import create_app

app = create_app()
with app.app_context():
    app.cli.main(args=sys.argv[1:], prog_name='catalog')
