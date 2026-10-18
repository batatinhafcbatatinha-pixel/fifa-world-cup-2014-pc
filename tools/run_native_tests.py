"""Compile and run the game-free tests of the native shader/vertex importers (C++20, warnings as errors)."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ('shader-metadata', 'source-shader-asset', 'shader-parameter-updates', 'source-vertex-layout')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=shutil.which('clang++') or shutil.which('g++'))
    args = parser.parse_args()
    if not args.compiler:
        parser.error('clang++ or g++ is required')
    failures = 0
    with tempfile.TemporaryDirectory() as temp:
        for case in CASES:
            source = ROOT / 'tests' / f'test-{case}.cpp'
            exe = Path(temp) / (case + ('.exe' if sys.platform == 'win32' else ''))
            build = subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(ROOT / 'project/src'), str(source), '-o', str(exe)],
                                   capture_output=True, text=True)
            if build.returncode:
                print(f'FAIL {case}: compile\n{build.stdout}{build.stderr}')
                failures += 1
                continue
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            print(('PASS ' if result.returncode == 0 else 'FAIL ') + case)
            if result.returncode:
                print(result.stdout + result.stderr)
                failures += 1
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
