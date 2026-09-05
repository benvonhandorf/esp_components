#!/usr/bin/env bash
#
# Build tests/consumer against the PUBLISHED tags instead of EXTRA_COMPONENT_DIRS.
#
# This is the only check that exercises the path a stranger takes. An in-tree build
# resolves every component from the working tree, so it cannot see a missing entry in
# an idf_component.yml -- the manifests are not consulted at all. Here they are the
# only thing consulted, and a component that requires a sibling it does not declare
# fails with:
#
#   Failed to resolve component 'diag' required by component 'cli': unknown
#
# It fetches from GitHub, so it tests what is pushed, not what is in the working
# tree. Run it after pushing the commit and its tags.
#
#   tests/publish_check.sh [target]        # default esp32s3
#
set -euo pipefail

TARGET="${1:-esp32s3}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_URL="https://github.com/benvonhandorf/esp_components.git"

[ -n "${IDF_PATH:-}" ] || { echo "IDF_PATH is not set; source ESP-IDF's export.sh" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -r "$ROOT/tests/consumer" "$WORK/consumer"
rm -rf "$WORK/consumer/build" "$WORK/consumer/sdkconfig"
PROJ="$WORK/consumer"

# The point of the exercise: reach the components only through the manifest.
sed -i '/EXTRA_COMPONENT_DIRS/d' "$PROJ/CMakeLists.txt"

# Every component, each at the version its own manifest declares -- which is the
# version the tag of that name points at, or this check is being run against a tag
# that was never pushed.
{
    echo "dependencies:"
    for dir in "$ROOT"/*/; do
        comp="$(basename "$dir")"
        [ -f "$dir/idf_component.yml" ] || continue
        version="$(sed -n 's/^version: *"\?\([0-9.]*\)"\?/\1/p' "$dir/idf_component.yml" | head -1)"
        [ -n "$version" ] || { echo "no version in $comp/idf_component.yml" >&2; exit 1; }
        printf '  %s:\n    git: %s\n    path: %s\n    version: %s-v%s\n' \
            "$comp" "$REPO_URL" "$comp" "$comp" "$version"
    done
} > "$PROJ/main/idf_component.yml"

# The lockfile is the in-tree build's, and pins nothing about these git dependencies.
rm -f "$PROJ/dependencies.lock"

echo "== consuming $(grep -c '    path:' "$PROJ/main/idf_component.yml") components by tag, target $TARGET"
cd "$PROJ"
idf.py set-target "$TARGET"
idf.py build
echo "== built against published tags: $TARGET"
