"""Compile the unchanged file-1167 authored sources; never install or re-sign.
Run from an x64 MSVC developer prompt, or pass --vcvars explicitly.
Third-party Core headers are supplied by the reviewer's authorized SDK checkout.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding='utf-8-sig'))


def check_sources() -> None:
    binding = read_json(ROOT / 'source-binding.json')
    for relative, record in binding['source_files'].items():
        path = ROOT / relative
        if not path.is_file() or sha(path) != record['sha256']:
            raise RuntimeError('Bound source changed: ' + relative)


def compiler_environment(vcvars: Path | None, temporary: Path) -> dict:
    env = os.environ.copy()
    if vcvars is not None:
        if not vcvars.is_file():
            raise FileNotFoundError('MSVC activation script not found')
        # Parse activation output in memory; never print/store the environment.
        command = f'cmd.exe /d /s /c "call "{vcvars}" >nul && set"'
        result = subprocess.run(command, capture_output=True, check=True)
        for line in result.stdout.decode('mbcs', errors='replace').splitlines():
            key, separator, value = line.partition('=')
            if key and separator:
                env[key.upper()] = value
    temporary.mkdir(parents=True, exist_ok=False)
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    return env


def execute(command: list[str], cwd: Path, env: dict, log: str) -> None:
    tool = shutil.which(command[0], path=env.get('PATH', ''))
    if tool is None:
        raise RuntimeError(command[0] + ' not found; open an x64 MSVC developer prompt')
    result = subprocess.run([tool, *command[1:]], cwd=cwd, env=env, capture_output=True)
    (cwd / log).write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{command[0]} failed ({result.returncode}); see {cwd.name}/{log}')


def build_manager(name: str, config: dict, output: Path, env: dict, resource: Path) -> Path:
    work = output / name
    work.mkdir()
    framework = name == 'framework'
    entry = 'framework_sources' if framework else 'manager_sources'
    generation = ROOT / ('src/manager/gen-frame' if framework else 'src/manager/gen-manager')
    includes = [generation, *[ROOT / p for p in config['manager_includes']]]
    defines = config['framework_defines'] if framework else []
    objects = []
    for index, source in enumerate(config[entry]):
        obj = work / f'{index:02d}-{Path(source).stem}.obj'
        execute(['cl.exe', *config['manager_flags'], *['/I' + str(p) for p in includes],
                 *defines, '/c', '/sourceDependencies', str(work / f'{index:02d}.deps.json'),
                 str(ROOT / source), '/Fo' + str(obj)], work, env, f'compile-{index:02d}.log')
        objects.append(str(obj))
    target = work / ('Mod管理器.exe' if framework else 'manager.exe')
    execute(['link.exe', '/NOLOGO', '/INCREMENTAL:NO', '/MANIFEST:NO', '/SUBSYSTEM:WINDOWS',
             '/OUT:' + str(target), str(resource), *config['manager_libraries'], *objects],
            work, env, 'link.log')
    return target


def sdk_includes(sdk: Path) -> list[Path]:
    upstream = sdk / 'RE-UE4SS-527a483b63b4dd0104fe1ca1a3934a06b87fcfb2'
    unreal = sdk / ('UEPseudo-auth-36e87abeb376a3949fe658a08fe1420cb6bd95b2/'
                    'Re-UE4SS-UEPseudo-36e87abeb376a3949fe658a08fe1420cb6bd95b2')
    return [upstream / 'UE4SS/include', *sorted((upstream / 'deps/first').glob('*/include')),
            unreal / 'include', unreal / 'generated_include', unreal / 'include/Unreal',
            unreal / 'include/Unreal/Core', sdk / 'fmt-11.2.0/include', sdk / 'imgui-1.92.1',
            sdk / 'ImGuiColorTextEdit-1.2.0', sdk / 'zydis-4.1.1/include',
            sdk / 'zycore-c-0b2432ced0884fd152b471d97ecf0258ff4d859f/include']


def build_core(config: dict, output: Path, env: dict, sdk: Path) -> Path:
    for relative, expected in read_json(ROOT / 'sdk-input-hashes.json').items():
        path = sdk / relative
        if not path.is_file() or sha(path) != expected:
            raise RuntimeError('Pinned SDK input missing/different: ' + relative)
    includes = [*sdk_includes(sdk), *[ROOT / p for p in config['core_includes']]]
    work = output / 'core'
    work.mkdir()
    library = work / 'UE4SS.lib'
    execute(['lib.exe', '/nologo', '/machine:x64', '/def:' + str(ROOT / 'src/core/UE4SS.def'),
             '/out:' + str(library)], work, env, 'import-library.log')
    dependencies = work / 'dependencies'
    dependencies.mkdir()
    target = work / 'main.dll'
    execute(['cl.exe', *config['core_flags'], *['/I' + str(p) for p in includes],
             '/sourceDependencies', str(dependencies), '/LD',
             *[str(ROOT / p) for p in config['core_sources']], str(library),
             '/Fo' + str(work) + os.sep, '/Fe' + str(target), '/link',
             '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF'], work, env, 'compile-link.log')
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-root', type=Path, help='Authorized pinned header layout; see BUILD_INSTRUCTIONS.txt')
    parser.add_argument('--vcvars', type=Path, help='Optional vcvars64.bat, otherwise use the active developer prompt')
    parser.add_argument('--manager-only', action='store_true', help='Build both manager EXEs, without UE SDK')
    parser.add_argument('--label', default='review-rebuild-01')
    args = parser.parse_args()
    if not args.manager_only and args.sdk_root is None:
        parser.error('--sdk-root is required for the Core DLL; use --manager-only for the standalone EXEs')
    if not args.label or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-_' for c in args.label):
        parser.error('Use a short lowercase build label')
    output = ROOT / 'build' / args.label
    output.mkdir(parents=True, exist_ok=False)
    check_sources()
    config = read_json(ROOT / 'src/build-config.json')
    env = compiler_environment(args.vcvars, output / 'tmp')
    resources = output / 'resources'
    shutil.copytree(ROOT / 'src/manager/resources', resources)
    resource = resources / 'installer.res'
    execute(['rc.exe', '/nologo', '/fo' + str(resource), 'installer.rc'], resources, env, 'compile.log')
    targets = [build_manager('manager', config, output, env, resource),
               build_manager('framework', config, output, env, resource)]
    if not args.manager_only:
        targets.append(build_core(config, output, env, args.sdk_root.resolve()))
    check_sources()
    results = [{'file': p.relative_to(ROOT).as_posix(), 'sha256': sha(p), 'bytes': p.stat().st_size} for p in targets]
    receipt = {'built_at': datetime.now(timezone.utc).isoformat(), 'compile_link_passed': True,
               'targets': results, 'sources_unchanged': True, 'game_started': False,
               'game_installed': False, 'release_signing_performed': False,
               'scope': 'Portable reviewer compile/link check; not bit-identical production-package recreation or live-game acceptance'}
    (output / 'rebuild-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(receipt, ensure_ascii=False))


if __name__ == '__main__':
    main()
