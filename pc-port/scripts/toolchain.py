# SPDX-License-Identifier: GPL-2.0-or-later
"""The Windows toolchain of the build scripts: vcvars64.bat and clang-cl.

vcvars64.bat: ISS_VCVARS, else Visual Studio 2022 Community (the original setup), else the newest
Visual Studio with the C++ tools that vswhere reports (2022, 2026, any edition).
clang-cl: ISS_CLANG, else tools/llvm/bin/clang-cl.exe, else the "C++ Clang Compiler for Windows"
component of that Visual Studio (VC/Tools/Llvm/x64/bin)."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
VS2022 = Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community')
VSWHERE = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / \
    'Microsoft Visual Studio/Installer/vswhere.exe'


def _installations():
    found = []
    if VSWHERE.exists():
        result = subprocess.run([str(VSWHERE), '-all', '-products', '*', '-sort', '-requires',
                                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                 '-property', 'installationPath'],
                                capture_output=True, text=True)
        found = [Path(line.strip()) for line in result.stdout.splitlines() if line.strip()]
    return [VS2022] + found


def vcvars():
    if os.environ.get('ISS_VCVARS'):
        return Path(os.environ['ISS_VCVARS'])
    for path in _installations():
        candidate = path / 'VC/Auxiliary/Build/vcvars64.bat'
        if candidate.exists():
            return candidate
    return VS2022 / 'VC/Auxiliary/Build/vcvars64.bat'


def clang():
    if os.environ.get('ISS_CLANG'):
        return Path(os.environ['ISS_CLANG'])
    local = WORKSPACE / 'tools/llvm/bin/clang-cl.exe'
    if local.exists():
        return local
    for path in _installations():
        candidate = path / 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
        if candidate.exists():
            return candidate
    return local


def environment(clang_path=None):
    """The vcvars64 environment with clang-cl's folder first on PATH."""
    result = subprocess.run(f'cmd.exe /d /s /c ""{vcvars()}" >nul && set"',
                            capture_output=True, text=True, check=True)
    env = os.environ.copy()
    for line in result.stdout.splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            env[key] = value
    env['PATH'] = str((clang_path or clang()).parent) + os.pathsep + env.get('Path', env.get('PATH', ''))
    env.pop('Path', None)
    return env
