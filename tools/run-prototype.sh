#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/prototype"
if [ ! -d .venv ]; then
  python -m venv .venv --system-site-packages
  . .venv/bin/activate
  pip install -r requirements.txt
else
  . .venv/bin/activate
fi
exec python -m gameframe "$@"
