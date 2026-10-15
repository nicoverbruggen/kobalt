#!/usr/bin/env python3
"""Verify CI artifacts and attach them to a draft GitHub release."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

from changelog import section
from release_sources import checkout, digest
from version import validate as validate_version

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = {'qt5': 'Kobalt-qt5.zip', 'qt6': 'Kobalt-qt6.zip',
             'android': 'Kobalt-android-arm64-debug.apk', 'source': 'Kobalt-source.tar.gz'}


def release_artifacts(tag, artifacts=ARTIFACTS):
    # Preserve the attachment sets of published betas. Android starts at Beta 4.
    excluded = {'qt6'} if tag == 'v1.0-beta.1' else set()
    if tag in ('v1.0-beta.1', 'v1.0-beta.2', 'v1.0-beta.3'):
        excluded.add('android')
    return {key: name for key, name in artifacts.items() if key not in excluded}


def release_notes(root, tag, repository):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Expected an owner/repository name')
    notes = section(root, tag)
    artifacts = release_artifacts(tag)
    release_url = f'https://github.com/{repository}/releases/tag/{quote(tag, safe="")}'
    download_url = f'https://github.com/{repository}/releases/download/{quote(tag, safe="")}'
    fields = {'tag': tag, 'repository': repository, 'release_url': release_url, 'download_url': download_url,
              'source_url': download_url + '/' + ARTIFACTS['source'],
              'checksums_url': download_url + '/SHA256SUMS', 'build_info_url': download_url + '/BUILD-INFO.json'}
    firmware = {'qt5': 'firmware 4.38 and later', 'qt6': 'firmware 5.18 and later 5.x, and 6.x'}
    packages = []
    for key, name in artifacts.items():
        if key in firmware:
            fields[key + '_url'] = download_url + '/' + name
            packages.append(f'- [{name}]({fields[key + "_url"]}) for {firmware[key]}.')
    fields['package_links'] = '\n'.join(packages)
    fields['android_installation'] = ''
    if 'android' in artifacts:
        fields['android_url'] = download_url + '/' + artifacts['android']
        fields['android_installation'] = (
            '### Android debug build\n\n'
            f'Download [{artifacts["android"]}]({fields["android_url"]}) for ARM64 devices running Android 9 or later. '
            'Allow installation from your browser or file manager when Android asks.\n\n'
            'This is an experimental, debuggable build signed with Kobalt\'s shared public test key. '
            'Obtain it directly from this GitHub repository. Anyone with the public key can sign a modified update; '
            'the signature does not prove that an APK came from the maintainer. '
            'It installs separately from release builds and keeps its own books, settings and reading progress.')
    installation = (root / 'docs/release-installation.md').read_text(encoding='utf-8').strip()
    if not installation:
        raise ValueError('Empty release installation template')

    def replace(match):
        name = match.group(1).strip()
        if name not in fields:
            raise ValueError('Unknown or unavailable release installation field: ' + name)
        return fields[name]

    installation = re.sub(r'\{\{(.*?)\}\}', replace, installation, flags=re.DOTALL)
    if '{{' in installation or '}}' in installation:
        raise ValueError('Malformed release installation field')
    return notes + '\n\n' + installation + '\n\n' + (
        f'[Source code and dependencies]({fields["source_url"]})\n')


def validate(root, directory, tag, artifacts=ARTIFACTS):
    validate_version(tag)
    artifacts = release_artifacts(tag, artifacts)
    provenance = checkout(root)
    tagged = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'refs/tags/' + tag + '^{commit}'], text=True).strip()
    if tagged != provenance['commit']:
        raise ValueError('Release tag does not point at the checked-out commit')
    records = {}
    for key, name in artifacts.items():
        record = json.loads((directory / (key + '.json')).read_text())
        path = directory / name
        if (record.get('commit') != provenance['commit'] or record.get('dirty') is not False
                or record.get('version') != tag or record.get('file') != name or path.is_symlink() or not path.is_file()
                or record.get('sha256') != digest(path)):
            raise ValueError('Artifact does not match the clean release commit: ' + name)
        if key != 'source' and record.get('target') != key:
            raise ValueError('Artifact target mismatch: ' + name)
        records[key] = record
    return records


def draft(root, directory, tag, repository):
    notes_text = release_notes(root, tag, repository)
    artifacts = release_artifacts(tag)
    records = validate(root, directory, tag, artifacts)
    commit = records['source']['commit']
    remote = subprocess.check_output(['gh', 'api', f'repos/{repository}/commits/{tag}', '--jq', '.sha'], text=True).strip()
    if remote != commit:
        raise ValueError('Remote tag differs from the validated source commit')
    sums = directory / 'SHA256SUMS'
    sums.write_text(''.join(records[key]['sha256'] + '  ' + name + '\n' for key, name in artifacts.items()))
    info = directory / 'BUILD-INFO.json'
    info.write_text(json.dumps({'tag': tag, 'commit': commit, 'artifacts': records}, indent=2) + '\n')
    assets = [str(directory / name) for name in artifacts.values()] + [str(sums), str(info)]
    existing = subprocess.run(['gh', 'release', 'view', tag, '--repo', repository, '--json', 'isDraft,tagName,assets'],
                              capture_output=True, text=True)
    if existing.returncode == 0:
        release = json.loads(existing.stdout)
        if release.get('isDraft') is not True or release.get('tagName') != tag:
            raise ValueError('Refusing to change a published release')
        excluded = set(ARTIFACTS.values()) - set(artifacts.values())
        for asset in release.get('assets', []):
            if asset['name'] in excluded:
                current = subprocess.run(['gh', 'release', 'view', tag, '--repo', repository,
                                          '--json', 'isDraft,tagName'], check=True, capture_output=True, text=True)
                current = json.loads(current.stdout)
                if current.get('isDraft') is not True or current.get('tagName') != tag:
                    raise ValueError('Release was published while uploading; stopped')
                subprocess.run(['gh', 'release', 'delete-asset', tag, asset['name'], '--repo', repository, '--yes'],
                               check=True)
        notes = directory / 'release-notes.md'
        notes.write_text(notes_text, encoding='utf-8')
        subprocess.run(['gh', 'release', 'edit', tag, '--repo', repository, '--notes-file', str(notes)], check=True)
        subprocess.run(['gh', 'release', 'upload', tag, '--repo', repository, '--clobber', *assets], check=True)
    else:
        notes = directory / 'release-notes.md'
        notes.write_text(notes_text, encoding='utf-8')
        subprocess.run(['gh', 'release', 'create', tag, '--repo', repository, '--verify-tag', '--draft',
                        *(['--prerelease'] if '-' in tag else []),
                        '--title', 'Kobalt ' + tag, '--notes-file', str(notes), *assets], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--artifacts', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--notes-only', action='store_true', help='Print release notes without building or contacting GitHub')
    args = parser.parse_args()
    if args.notes_only:
        print(release_notes(ROOT, args.tag, args.repository), end='')
    else:
        draft(ROOT, args.artifacts, args.tag, args.repository)
