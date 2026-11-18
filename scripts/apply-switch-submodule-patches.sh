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
# dynarmic-switch2.patch (the Switch 2's kaiHEN JIT: RX views published from RW
# staging, guarded by __SWITCH2__) applies on top of dynarmic-switch.patch. Once
# both are in, only the newer one reverse-applies; it carries the older one's
# lines as context, so it reverse-applying means both are applied.
DYNARMIC_HEAD="86458a0bd369d63ba4c2ef812cacbb6c9080c065"
if [[ "$(git -C "$ROOT/external/dynarmic" rev-parse HEAD)" == "$DYNARMIC_HEAD" ]] &&
    git -C "$ROOT/external/dynarmic" apply --reverse --check \
        "$PATCH_DIR/dynarmic-switch2.patch" >/dev/null 2>&1; then
    printf 'already applied: external/dynarmic (dynarmic-switch.patch, dynarmic-switch2.patch)\n'
else
    apply_one "external/dynarmic" \
        "dynarmic-switch.patch" \
        "$DYNARMIC_HEAD"
    git -C "$ROOT/external/dynarmic" apply --check "$PATCH_DIR/dynarmic-switch2.patch"
    git -C "$ROOT/external/dynarmic" apply "$PATCH_DIR/dynarmic-switch2.patch"
    printf 'applied: external/dynarmic (dynarmic-switch2.patch)\n'
fi
apply_one "external/ffmpeg" \
    "ffmpeg-switch.patch" \
    "eaf918c97e76d3fa7ec5bfea63a307fced559e76"
apply_one "external/LibAtrac9" \
    "libatrac9-switch.patch" \
    "82767fe38823c32536726ea798f392b0b49e66b9"
apply_one "external/sdl" \
    "sdl-switch.patch" \
    "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"
apply_one "external/psvpfstools/psvpfsparser" \
    "psvpfsparser-switch.patch" \
    "05d43a06a6189302d31091c65e4fc8707476eef3"

printf 'All Switch submodule patches are applied.\n'
