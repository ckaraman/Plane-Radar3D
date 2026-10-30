"""Windows exe version info (stage 9b). Plain Python: runs on Windows too."""

import ast
import os
import re
import subprocess
import sys

HELPER = os.path.join(os.path.dirname(__file__), "..")
sys.path.insert(0, HELPER)

import version_info  # noqa: E402

VERSION_FROM_APP = re.search(r'^VERSION = "(.+)"$', open(os.path.join(HELPER, "app.py"), encoding="utf-8").read(), re.M).group(1)


def test_version_text():
    t = version_info.text("Plane Radar Deck", "1.10.0")
    ast.parse(t)  # PyInstaller eval()s it
    assert "filevers=(1, 10, 0, 0)" in t and "prodvers=(1, 10, 0, 0)" in t
    for s in ["'ProductName', 'Plane Radar Deck'", "'FileDescription', 'Plane Radar Deck'",
              "'CompanyName', 'Plane Radar 3D'", "'OriginalFilename', 'PlaneRadarDeck.exe'",
              "'InternalName', 'PlaneRadarDeck'", "'LegalCopyright', 'Copyright (c) 2026 ckaraman'",
              "'FileVersion', '1.10.0.0'", "'ProductVersion', '1.10.0.0'"]:
        assert s in t, s


def test_reads_app():
    out = subprocess.run([sys.executable, "version_info.py"], cwd=HELPER, capture_output=True, text=True, check=True).stdout
    assert "'ProductVersion', '%s.0'" % VERSION_FROM_APP in out
    assert "'ProductName', 'Plane Radar Deck'" in out


if __name__ == "__main__":
    test_version_text()
    test_reads_app()
    print("tests ok")
