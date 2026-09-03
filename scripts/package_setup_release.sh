#!/usr/bin/env bash
# Thin wrapper around the shared psxrecomp setup-host packager.
# Autofilled by tools/new_project_layout/setup_project.{sh,ps1}.
#
# Usage:
#   scripts/package_setup_release.sh <build-dir> <artifact-tag> [recompiler-build-dir]
#
# Writes: dist/ygofm-<VERSION>-<artifact-tag>.zip
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-}"
ARTIFACT_TAG="${2:-}"
RECOMPILER_BUILD="${3:-build-recompiler}"

if [[ -z "${BUILD_DIR}" || -z "${ARTIFACT_TAG}" ]]; then
  echo "usage: $0 <build-dir> <artifact-tag> [recompiler-build-dir]" >&2
  exit 2
fi

PACKAGER="${ROOT}/psxrecomp/tools/package_setup_host.sh"
if [[ ! -f "${PACKAGER}" ]]; then
  echo "error: missing ${PACKAGER} (psxrecomp submodule)" >&2
  exit 1
fi
chmod +x "${PACKAGER}" 2>/dev/null || true

# What the PLAYER'S build needs, beyond the obvious. Getting this list wrong
# does not fail the packaging step -- it produces a zip that unpacks and then
# breaks partway through the player's build, or builds a game quietly missing
# a feature. Both happened while this was being worked out:
#
#   src/      the title's sources, INCLUDING codegen_setup.c, which moved here
#             from the repo root. The three baked art sources are absent by
#             design -- tools/disc_assets.py remakes them from the disc.
#   tools/    disc_assets.py plus the two decoders it drives and
#             sprite_spec.json. Without them the product build cannot bake its
#             art and will not even configure.
#   assets/   the app icon CMakeLists names. A missing icon does NOT fail the
#             build, so omitting it ships the wrong icon with no error at all.
#   game_options.toml (added conditionally below)
#             the native in-game OPTION persistence declaration. Today it is
#             an all-commented template declaring nothing, so shipping it
#             changes no behaviour -- it is here because it is a tracked
#             project file a source build expects, not because anything
#             breaks without it. (An earlier version of this comment said
#             omitting it dropped "fast loading = instant". That was wrong:
#             fast loading is a menu setting in the runtime's own
#             menu_settings.ini, it defaults to OFF, and it has nothing to
#             do with this file.)
#
#   launcher_assets/
#             the recomp-ui boxart. The packager stages a copy beside the SETUP
#             exe on its own, but the player's product build is configured from
#             this tree, and CMakeLists names launcher_assets/img/boxart.tga --
#             recomp-ui silently skips a missing file, so without this dir the
#             built game's launcher shows an empty box where the art should be.
EXTRA_PROJECT=()
if [[ -f "${ROOT}/catalog_identity.json" ]]; then
  EXTRA_PROJECT+=(--project-file catalog_identity.json)
fi
if [[ -f "${ROOT}/framework_pins.txt" ]]; then
  EXTRA_PROJECT+=(--project-file framework_pins.txt)
fi
# Optional in-game options defaults (titles that POST_BUILD-copy this file).
if [[ -f "${ROOT}/game_options.toml" ]]; then
  EXTRA_PROJECT+=(--project-file game_options.toml)
fi

cd "${ROOT}"
exec bash "${PACKAGER}" \
  --root "${ROOT}" \
  --build-dir "${BUILD_DIR}" \
  --artifact "${ARTIFACT_TAG}" \
  --zip-prefix ygofm \
  --exe-name Yu_Gi_Oh_Forbidden_Memories_Recompiled \
  --display-name "Yu-Gi-Oh Forbidden Memories Recompiled" \
  --recompiler-build "${RECOMPILER_BUILD}" \
  --version-env RELEASE_VERSION \
  --disc-hint "your legally owned Yu-Gi-Oh Forbidden Memories disc" \
  --openbios-only \
  --project-file CMakeLists.txt \
  --project-file game.toml \
  --project-file VERSION \
  --project-file README.md \
  --project-file LICENSE \
  --project-file NOTICE \
  --project-file Reset-Setup.bat \
  --project-dir src \
  --project-dir tools \
  --project-dir seeds \
  --project-dir assets \
  --project-dir mods   --project-dir launcher_assets \
  --project-exclude assets/duelist_icons \
  "${EXTRA_PROJECT[@]}"
