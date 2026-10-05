# ASTRONEER Mod Framework

Shared native mod framework and Windows Mod Manager by **Youtiaowei**.

This repository publishes the authored native sources for the **1.0.2 universal-manager framework** submitted as Nexus Mods **mod 234 / file 1167**. It is a corresponding-source snapshot, not a new installer upload or a claim of Nexus security approval.

## What is included

- A native Win32 universal installer entrance and persistent Mod Manager.
- The shared in-game menu, settings, numeric controls and presentation layer.
- Local file-based manager/runtime communication.
- Generic module ABI, authenticated module loading and lifecycle dispatch.
- Installation ownership, transactions, local signature verification and public trust anchors.
- Exact generated public build contracts, resource inputs and original icon authoring sources.
- A portable, standard-library-only native build driver and pinned dependency hashes.

**No standalone gameplay mod implementation is included.** Planet Flattening, Universal Slot and Wide Range are separate distributions. Names or compatibility contracts referenced by the framework are not the corresponding gameplay implementations. The framework does not provide those features by itself.

## Build

Read [BUILD_INSTRUCTIONS.txt](BUILD_INSTRUCTIONS.txt) for the full Windows toolchain, pinned SDK layout, commands, outputs and signing limitations.

### Manager only

From an **x64 Native Tools Command Prompt for Visual Studio 2022**, at this repository's root:

```cmd
python -B src/rebuild_for_review.py --manager-only --label manager-build-01
```

This builds the bootstrap `Mod管理器.exe` and the persistent `manager.exe`. No Unreal/UE4SS SDK is needed for this target. Python 3.10+ is needed only for building; the shipping managers are native applications.

### Managers and shared Core

After obtaining the pinned SDK through its authorized upstream access process:

```cmd
python -B src/rebuild_for_review.py --sdk-root "D:\ExampleSDK" --label framework-build-01
```

The Core depends on an Epic-authorized UEPseudo/Unreal header checkout. Those restricted headers are **not redistributed here**. This is not an unauthenticated, dependency-free Core build. Detailed versions and directory names are documented in the build instructions.

All build outputs stay under `build/`. Use a new `--label` for each attempt. The script refuses to overwrite existing attempts or silently compile altered frozen inputs.

## Source and review boundary

- [source-binding.json](source-binding.json) binds the unchanged native build inputs and original shipped EXE/DLL identities.
- [sdk-input-hashes.json](sdk-input-hashes.json) records the compiler-consumed external header identities.
- [src/build-config.json](src/build-config.json) records translation units, CRT selection, definitions, include order and libraries.
- [Review scope](docs/REVIEW_SCOPE.md) separates authored native code, the upstream loader and cooked UI data.
- [Third-party notices](THIRD_PARTY_NOTICES.md) identifies dependencies and retained licenses.
- [Build verification](docs/BUILD_VERIFICATION.json) records the actual isolated compile/link result for this public snapshot.

This repository does not redistribute the third-party loader DLLs, cooked `.pak` art data, game assets, private signing authority, account credentials, local pairing records, player configuration, saves or browser state. UE4SS's source and build documentation are linked separately.

A fresh native build is not automatically byte-identical to the original PE files. Original author-signed package records will not accept arbitrary newly compiled replacements. The build script neither generates release signatures nor weakens production verification. Do not substitute these outputs into a signed production package.

## License

Authored framework code, tooling, documentation and the original installer icon design in this repository are available under the [MIT License](LICENSE).

Third-party code retains its own copyrights and license terms. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This license does not grant rights to Epic/Unreal code, the game or its trademarks, nor to independently distributed gameplay mods that are not included in this repository.

MIT permits commercial use and does not itself require independently distributed gameplay modules to publish their own source. Such modules and their dependencies still have their own applicable terms.

## Community

- More mods: https://mod.cab
- QQ group: **131810277**

Not affiliated with or endorsed by System Era Softworks, Epic Games or Nexus Mods.

## 简体中文

这是 **异星探险家公共 Mod 框架与通用管理器** 的源码仓库，对应送交 Nexus 审核的 1.0.2 框架包（mod 234 / file 1167）。

公开范围：安装与管理器、共用游戏内菜单、设置、本机文件通信、模块接口与加载、事务和本地验签，以及实际编译需要的公共源码和资源。

**不包含平整星球、万能插槽、宽口范围等独立玩法 MOD 的实现。** 框架采用 MIT 许可，不会自动把这份许可扩展到未包含的独立 MOD。

详细编译步骤见 [BUILD_INSTRUCTIONS.txt](BUILD_INSTRUCTIONS.txt)。管理器可以单独编译；游戏内 Core 还需要按上游授权流程获取指定版本的 UE4SS/UEPseudo SDK。私钥、凭据、个人配置、存档、浏览器状态和受限 Unreal 头文件不在仓库中。

本仓库只公布源码与真实编译证据，不代表 Nexus 已放行；没有重新上传、替换或改动原发行包。
