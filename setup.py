"""
setup.py — custom build step for the pzp Python package.

The C library (libpzp.so) is compiled via the project Makefile and then
copied into the package source tree so that it is included in the wheel
and found at runtime next to pzp/__init__.py.

For an editable install (pip install -e .) the library is left at the repo
root, and pzp/_find_lib() falls back to searching there.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py
from setuptools.command.develop import develop

# Platform-specific library filename
_LIB_NAME = {
    "linux":  "libpzp.so",
    "darwin": "libpzp.dylib",
    "win32":  "pzp.dll",
}.get(sys.platform, "libpzp.so")

_ROOT     = Path(__file__).parent.resolve()
_PKG_DIR  = _ROOT / "src" / "pzp"
_LIB_SRC  = _ROOT / _LIB_NAME       # built by make
_LIB_DST  = _PKG_DIR / _LIB_NAME    # inside the package (for wheels)


def _build_c_library():
    """Run make to produce libpzp.so in the repo root."""
    print(f"[pzp] Building {_LIB_NAME} via Makefile …")
    subprocess.run(
        ["make", _LIB_NAME],
        cwd=str(_ROOT),
        check=True,
    )


def _copy_lib_into_package():
    """Copy the compiled library into src/pzp/ for wheel inclusion."""
    if not _LIB_SRC.exists():
        raise FileNotFoundError(
            f"{_LIB_SRC} not found after make. "
            "Ensure gcc and libzstd-dev are installed."
        )
    shutil.copy2(str(_LIB_SRC), str(_LIB_DST))
    print(f"[pzp] Copied {_LIB_NAME} → {_LIB_DST}")


class BuildPy(build_py):
    """Compile libpzp.so then run the normal build_py step."""

    def run(self):
        _build_c_library()
        _copy_lib_into_package()
        super().run()


class Develop(develop):
    """
    For editable installs (pip install -e .) compile the library in-place.
    The .so stays at the repo root; _find_lib() in __init__.py will find it.
    """

    def run(self):
        _build_c_library()
        super().run()


setup(
    cmdclass={
        "build_py": BuildPy,
        "develop":  Develop,
    },
)
