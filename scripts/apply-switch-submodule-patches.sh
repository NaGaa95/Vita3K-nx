#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$ROOT/patches/submodules"

git -C "$ROOT" submodule update --init --recursive

apply_one() {
    local relative_repo="$1"
    local patch_name="$2"
    local expected_head="$3"
    local repo="$ROOT/$relative_repo"
    local patch="$PATCH_DIR/$patch_name"
    local actual_head

    actual_head="$(git -C "$repo" rev-parse HEAD)"
    if [[ "$actual_head" != "$expected_head" ]]; then
        printf 'error: %s is at %s; expected %s\n' \
            "$relative_repo" "$actual_head" "$expected_head" >&2
        exit 1
    fi

    if git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
        printf 'already applied: %s\n' "$relative_repo"
        return
    fi

    if ! git -C "$repo" diff --quiet || ! git -C "$repo" diff --cached --quiet; then
        printf 'error: %s has unrelated tracked changes\n' "$relative_repo" >&2
        exit 1
    fi

    git -C "$repo" apply --check "$patch"
    git -C "$repo" apply "$patch"
    printf 'applied: %s\n' "$relative_repo"
}

apply_one "external/concurrentqueue" \
    "concurrentqueue-switch.patch" \
    "9afb99746f0f5fc94ac8aef737053ae0481ba8d1"
apply_one "external/dynarmic" \
    "dynarmic-switch.patch" \
    "86458a0bd369d63ba4c2ef812cacbb6c9080c065"
apply_one "external/ffmpeg" \
    "ffmpeg-switch.patch" \
    "02f4f2691b0efffff8923235baf146a87fc37263"
apply_one "external/LibAtrac9" \
    "libatrac9-switch.patch" \
    "82767fe38823c32536726ea798f392b0b49e66b9"
apply_one "external/sdl" \
    "sdl-switch.patch" \
    "f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc"
apply_one "external/psvpfstools/psvpfsparser" \
    "psvpfsparser-switch.patch" \
    "d14381f871a69009bd18b2aaec2213a6738bebba"

printf 'All Switch submodule patches are applied.\n'
