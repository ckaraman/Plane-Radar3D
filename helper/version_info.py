"""Version info for the Windows exe (stage 9b): `python version_info.py > version.txt`,
then `pyinstaller --version-file version.txt`. Reads APP_NAME and VERSION from app.py
without importing it (app.py pulls in the GUI libraries)."""

import ast
import os


def text(name, version):
    nums = tuple(int(x) for x in version.split(".")) + (0,)
    full = ".".join(str(n) for n in nums)
    strings = [
        ("CompanyName", "Plane Radar 3D"),
        ("FileDescription", name),
        ("FileVersion", full),
        ("InternalName", "PlaneRadarDeck"),
        ("LegalCopyright", "Copyright (c) 2026 ckaraman"),
        ("OriginalFilename", "PlaneRadarDeck.exe"),
        ("ProductName", name),
        ("ProductVersion", full),
    ]
    return """VSVersionInfo(
  ffi=FixedFileInfo(filevers=%r, prodvers=%r, mask=0x3f, flags=0x0, OS=0x40004,
                    fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('041F04B0', [
%s
    ])]),
    VarFileInfo([VarStruct('Translation', [0x041F, 1200])])
  ]
)
""" % (nums, nums, ",\n".join("      StringStruct(%r, %r)" % kv for kv in strings))


def from_app(path):
    found = {}
    for node in ast.parse(open(path, encoding="utf-8").read()).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            if node.targets[0].id in ("APP_NAME", "VERSION"):
                found[node.targets[0].id] = ast.literal_eval(node.value)
    return found["APP_NAME"], found["VERSION"]


if __name__ == "__main__":
    print(text(*from_app(os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.py"))), end="")
