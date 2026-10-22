# SPDX-License-Identifier: MIT
"""Shared build helpers; upstream checkouts remain unmodified."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    subprocess.run([str(value) for value in args], check=True, **kwargs)


def prepare_sdrpp(build):
    source = build / 'sdrpp-src'
    upstream = ROOT / 'external/sdrplusplus'
    patch = ROOT / 'sdrpp/patches/lr1121.patch'
    revision = subprocess.check_output(
        ['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True
    ).strip()
    stamp = dict(
        staging_version=2,
        upstream=revision,
        patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
    )
    manifest = source / '.lr1121-build.json'
    if source.exists():
        if not manifest.exists() or json.loads(manifest.read_text()) != stamp:
            raise RuntimeError('Staging directory differs; choose a new --build directory')
    else:
        build.mkdir(parents=True, exist_ok=True)
        archive = build / 'sdrpp-source.tar'
        with archive.open('wb') as output:
            run('git', '-C', upstream, 'archive', 'HEAD', stdout=output)
        source.mkdir()
        with tarfile.open(archive) as contents:
            contents.extractall(source, filter='data')
        archive.unlink()
        # Establish a separate Git root: otherwise git apply can silently skip
        # paths when this staging directory is nested inside the project repo.
        run('git', 'init', '--quiet', source)
        run('git', 'apply', '--check', patch, cwd=source)
        run('git', 'apply', patch, cwd=source)
        manifest.write_text(json.dumps(stamp) + '\n')
    shutil.copytree(
        ROOT / 'sdrpp/lr1121_source', source / 'source_modules/lr1121_source',
        dirs_exist_ok=True,
    )
    return source
