"""The package itself: it imports without pytest and knows its version."""

import subprocess
import sys

import devbench


def test_import_does_not_pull_in_pytest():
    code = "import sys, devbench; sys.exit('pytest' in sys.modules)"

    assert subprocess.run([sys.executable, "-c", code], check=False).returncode == 0


def test_version_is_set():
    assert devbench.__version__
