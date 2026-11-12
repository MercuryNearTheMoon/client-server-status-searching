"""Package source separately from reports/evidence; no build artifacts or .git."""
import hashlib
from pathlib import Path
import tarfile

root = Path(__file__).resolve().parent.parent
out = root/'submission'
out.mkdir(exist_ok=True)
files = []
for name in ['client', 'server', 'util', 'test', 'scripts', 'docs']:
    files.extend(path for path in (root/name).rglob('*') if path.is_file() and
                 '__pycache__' not in path.parts and path.suffix not in ('.pyc', '.o', '.so'))
files.extend(root/name for name in ['Makefile', 'Dockerfile', 'compose.yaml', 'README.md',
                                   '.gitignore', '.dockerignore'])
files = sorted(set(files))
archive = out/'source-code.tar.gz'
with tarfile.open(archive, 'w:gz') as tar:
    for path in files:
        if path.is_symlink(): raise RuntimeError(f'Unexpected symlink: {path}')
        tar.add(path, arcname=Path('midterm-source')/path.relative_to(root), recursive=False)
with tarfile.open(archive) as tar:
    assert len(tar.getmembers()) == len(files)
    for member, path in zip(tar.getmembers(), files):
        assert tar.extractfile(member).read() == path.read_bytes()
(out/'source-code.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  source-code.tar.gz\n')
(out/'source-manifest.txt').write_text(''.join(
    hashlib.sha256(path.read_bytes()).hexdigest()+'  '+str(path.relative_to(root))+'\n' for path in files))
print(f'Packaged and verified {len(files)} files: {archive}')
