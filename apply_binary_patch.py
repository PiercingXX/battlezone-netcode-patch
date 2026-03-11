#!/usr/bin/env python3

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_manifest(path: Path) -> dict:
    with path.open('r', encoding='utf-8') as handle:
        return json.load(handle)


def apply_patch(target_path: Path, manifest: dict, create_backup: bool) -> int:
    data = bytearray(target_path.read_bytes())
    actual_sha256 = sha256_bytes(data)
    expected_sha256 = manifest.get('expected_sha256')

    if expected_sha256 and actual_sha256.lower() != expected_sha256.lower():
        print('Refusing to patch: SHA-256 mismatch.', file=sys.stderr)
        print(f'Expected: {expected_sha256}', file=sys.stderr)
        print(f'Actual:   {actual_sha256}', file=sys.stderr)
        return 2

    patches = manifest.get('patches', [])
    if not patches:
        print('No patches defined in manifest.', file=sys.stderr)
        return 3

    for patch in patches:
        offset = int(patch['offset'], 0) if isinstance(patch['offset'], str) else int(patch['offset'])
        original = bytes.fromhex(patch['original_hex'])
        replacement = bytes.fromhex(patch['patched_hex'])

        if len(original) != len(replacement):
            print(f"Patch '{patch.get('name', '<unnamed>')}' has mismatched lengths.", file=sys.stderr)
            return 4

        current = bytes(data[offset:offset + len(original)])
        if current != original:
            print(f"Patch '{patch.get('name', '<unnamed>')}' failed original-byte check at {offset:#x}.", file=sys.stderr)
            print(f'Expected: {original.hex()}', file=sys.stderr)
            print(f'Actual:   {current.hex()}', file=sys.stderr)
            return 5

        data[offset:offset + len(replacement)] = replacement

    if create_backup:
        backup_path = target_path.with_suffix(target_path.suffix + '.bak')
        if not backup_path.exists():
            shutil.copy2(target_path, backup_path)

    target_path.write_bytes(data)

    print(f'Patched {target_path.name}')
    print(f'Old SHA-256: {actual_sha256}')
    print(f'New SHA-256: {sha256_bytes(data)}')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description='Apply a guarded byte patch to a Battlezone 98 Redux binary.')
    parser.add_argument('target', help='Path to the binary to patch')
    parser.add_argument('manifest', help='Path to the JSON patch manifest')
    parser.add_argument('--no-backup', action='store_true', help='Do not create a .bak backup file')
    args = parser.parse_args()

    target_path = Path(args.target)
    manifest_path = Path(args.manifest)

    if not target_path.is_file():
        print(f'Target not found: {target_path}', file=sys.stderr)
        return 1

    if not manifest_path.is_file():
        print(f'Manifest not found: {manifest_path}', file=sys.stderr)
        return 1

    manifest = load_manifest(manifest_path)
    return apply_patch(target_path, manifest, create_backup=not args.no_backup)


if __name__ == '__main__':
    raise SystemExit(main())