# Third-party notices and licensing boundary

The root MIT license applies to authored framework code and publication materials. It does not replace upstream terms.

## Included code

### nlohmann/json 3.11.3

- File: `src/manager/native_installer/vendor/nlohmann_json.hpp`.
- Upstream: https://github.com/nlohmann/json/tree/v3.11.3
- License: MIT; original SPDX identifiers and copyright comments are retained unchanged.
- Original accompanying license: [notices/nlohmann-json-LICENSE.txt](notices/nlohmann-json-LICENSE.txt).
- Additional authors recorded within this amalgamated header include Evan Nemerson (Hedley), Björn Hoehrmann (UTF-8 processing), Florian Loitsch (floating-point conversion) and the Abseil Authors.
- The header expressly identifies its C++11 integer-sequence fallback as originating from Google Abseil under Apache-2.0. That attribution is retained and the [Apache-2.0 license](notices/abseil-Apache-2.0.txt) is also supplied. This is not a claim to relicense that portion under the framework's MIT license.

## External dependencies, not redistributed here

### UE4SS loader and C++ API

- Source commit: `527a483b63b4dd0104fe1ca1a3934a06b87fcfb2`.
- Upstream: https://github.com/UE4SS-RE/RE-UE4SS/tree/527a483b63b4dd0104fe1ca1a3934a06b87fcfb2
- Referenced loader version: `v3.0.1-1125-g527a483b`, Win64 Game Shipping.
- The [UE4SS license notice](notices/UE4SS-LICENSE.txt) is retained for the loader referenced by the original distribution. No loader binary is in this source repository.
- UE4SS's separate dependencies retain their own upstream terms. The loader notice is not a license grant for every submodule.

### UEPseudo / Unreal headers

- Selected commit: `36e87abeb376a3949fe658a08fe1420cb6bd95b2`.
- Upstream: https://github.com/Re-UE4SS/UEPseudo
- Access requires the upstream Epic/Unreal source authorization process.
- Restricted headers, Unreal Engine source and account credentials are excluded. Hashes and version/layout instructions identify the compile inputs without redistributing their contents or granting access.

### Other pinned Core header dependencies

`fmt 11.2.0`, `imgui v1.92.1`, the **UE4SS-RE fork** of `ImGuiColorTextEdit v1.2.0` (commit `6d943aba9f7cef05da80b86dbb0253b63818f95c`), `zydis v4.1.1` and `zycore-c 0b2432ced0884fd152b471d97ecf0258ff4d859f` are acquired separately. They are not bundled or relicensed here. Sources are linked in [BUILD_INSTRUCTIONS.txt](BUILD_INSTRUCTIONS.txt); `sdk-input-hashes.json` records the exact consumed headers. The selected text-editor header was independently compared with that fork's tag and matches byte-for-byte; a same-named original repository is not a substitute.

### Optional icon-authoring tool

`src/render_installer_icon.py` uses PySide6 to render the included original SVG. PySide6 is not redistributed, and its own licensing applies if used. This optional authoring tool is not needed to compile the native managers: their exact ICO input is already included. The SVG and renderer source identify the original design rather than a game-art extraction. Renderer output is separate from the frozen resource input used by the native build.

## Game and cooked UI data

ASTRONEER and Unreal names and trademarks remain with their respective owners. The root MIT license does not grant rights to the game, Unreal Engine, proprietary SDK content, cooked art packs or third-party artwork outside this repository.

The original framework package contains a cooked support-card material/texture `.pak`. It is UI data, not a hidden native gameplay module, and is outside this native compilation recipe. Its package identity and role are described in [docs/REVIEW_SCOPE.md](docs/REVIEW_SCOPE.md). This source snapshot does not claim to rebuild the entire game distribution, loader or cooked art pack.
