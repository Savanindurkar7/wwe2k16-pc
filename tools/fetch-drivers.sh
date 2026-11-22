#!/usr/bin/env bash
# Built-in GPU drivers for the APK (r17): downloads the open-source Vulkan driver packages (adrenotools zips) from their
# GitHub releases and stages them as assets/drivers/ + assets/drivers/index.txt, which the launcher unpacks on first
# start (android/java/.../Drivers.java). Only open-source drivers are bundled (Mesa: MIT); Qualcomm's own drivers are
# not redistributable, so players import those themselves.
#
#   tools/fetch-drivers.sh <assets dir>
#
#   Mesa Turnip (Adreno), The412Banner/Banners-Turnip  three builds of one release:
#       A6xx / A7xx standard, A8xx (Snapdragon 8 Elite / Adreno 8xx incl. 810), A710 / A720 / A722 test
#   malisx2 (Mali), bmdhacks/malisx2                    Mesa PanVK on Arm's Mali kernel driver (experimental)
#
# Env: TURNIP_TAG / MALISX2_TAG = a release tag (default: the latest release), SKIP_DRIVERS=1 = none.
# Downloads are cached in out/drivers-cache/ (a release is fetched once). A failed download is a warning only: the APK
# is built without that driver.
set -uo pipefail
ASSETS="${1:?assets dir}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$ROOT/out/drivers-cache"
OUT="$ASSETS/drivers"
rm -rf "$OUT"
[ "${SKIP_DRIVERS:-0}" = 1 ] && { echo "built-in drivers: skipped (SKIP_DRIVERS=1)"; exit 0; }
mkdir -p "$OUT" "$CACHE"
INDEX="$OUT/index.txt"
echo "# file|folder name|family|gens|note (tools/fetch-drivers.sh, $(date -u +%Y-%m-%d))" > "$INDEX"

api() {  # api <path>  -> JSON on stdout
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        gh api "$1" 2>/dev/null
    else
        curl -fsSL ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} "https://api.github.com/$1"
    fi
}

# release_json <repo> <tag or empty>
release_json() {
    if [ -n "$2" ]; then api "repos/$1/releases/tags/$2"; else api "repos/$1/releases/latest"; fi
}

# pick <json> <python regex to match> [<python regex to exclude>]  -> "name url" of the first matching asset
pick() {
    REL_JSON="$1" python3 - "$2" "${3:-}" <<'PY'
import json, os, re, sys
want, skip = sys.argv[1], sys.argv[2]
rel = json.loads(os.environ["REL_JSON"])
for a in rel.get("assets", []):
    n = a["name"]
    if re.search(want, n) and not (skip and re.search(skip, n)):
        print(n, a["browser_download_url"])
        break
PY
}

# fetch <repo> <tag> <asset name> <url>  -> path in the cache
fetch() {
    local dst="$CACHE/${1//\//_}/$2/$3"
    if [ ! -s "$dst" ]; then
        mkdir -p "$(dirname "$dst")"
        curl -fsSL -o "$dst.part" "$4" && mv "$dst.part" "$dst" || { rm -f "$dst.part"; return 1; }
    fi
    # A real adrenotools package: a zip with a Vulkan driver library in it.
    unzip -l "$dst" 2>/dev/null | grep -q 'libvulkan.*\.so' || { echo "  WARNING: $3 has no libvulkan*.so" >&2; return 1; }
    echo "$dst"
}

# add <repo> <tag> <asset regex> <exclude regex> <folder name> <family> <gens> <note>
add() {
    local json="$1" repo="$2" tag="$3"
    local hit name url path
    hit="$(pick "$json" "$4" "$5")"
    if [ -z "$hit" ]; then echo "  WARNING: $repo $tag: no asset matching /$4/"; return; fi
    name="${hit%% *}"; url="${hit#* }"
    if path="$(fetch "$repo" "$tag" "$name" "$url")"; then
        cp "$path" "$OUT/$name"
        echo "$name|$6|$7|$8|$9 ($tag)" >> "$INDEX"
        echo "  + $6: $name ($(du -h "$path" | cut -f1))"
    else
        echo "  WARNING: could not download $name"
    fi
}

echo "built-in drivers:"
REPO=The412Banner/Banners-Turnip
if J="$(release_json "$REPO" "${TURNIP_TAG:-}")" && [ -n "$J" ]; then
    T="$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("tag_name",""))' <<<"$J")"
    add "$J" "$REPO" "$T" 'A8xx.*\.zip$' '' "Turnip A8xx (built-in)" adreno 8 \
        "Mesa Turnip for Adreno 8xx (8 Elite, 810 / 825 / 829 / 830 / 840)"
    add "$J" "$REPO" "$T" '^Turnip-.*\.zip$' '(A8xx|710|720|Test)' "Turnip A6xx-A7xx (built-in)" adreno "6,7" \
        "Mesa Turnip for Adreno 6xx / 7xx (Snapdragon 600-800 series, 7 / 8 Gen 1-3)"
    add "$J" "$REPO" "$T" '(710|720).*\.zip$' '' "Turnip A710-A722 (built-in)" adreno 710 \
        "Mesa Turnip test build for Adreno 710 / 720 / 722"
else
    echo "  WARNING: could not read $REPO releases (no network or rate limit)"
fi
REPO=bmdhacks/malisx2
if J="$(release_json "$REPO" "${MALISX2_TAG:-}")" && [ -n "$J" ]; then
    T="$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("tag_name",""))' <<<"$J")"
    add "$J" "$REPO" "$T" '^malisx2-.*\.zip$' '' "malisx2 Mali (built-in experimental)" mali "" \
        "Mesa PanVK for Mali (G52-G715, Arm kernel driver), experimental"
else
    echo "  WARNING: could not read $REPO releases"
fi

n=$(grep -vc '^#' "$INDEX" || true)
cat > "$ASSETS/LICENSE-drivers.txt" <<'EOF'
Built-in GPU drivers (assets/drivers):
- Mesa Turnip (freedreno Vulkan), builds by The412Banner/Banners-Turnip: Mesa 3D, MIT license (https://mesa3d.org).
- malisx2 (bmdhacks/malisx2): MIT license; built from Mesa 3D (PanVK), MIT license.
Each driver package is unchanged from its GitHub release.
EOF
echo "built-in drivers: $n staged in $OUT"
