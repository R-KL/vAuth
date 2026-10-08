#!/usr/bin/env bash
set -euo pipefail

repository_root=$1
fixture=$(mktemp -d /tmp/vauth-debian-builder-test.XXXXXX)
cleanup() {
    rm -rf -- "$fixture"
}
trap cleanup EXIT

package_path=packaging/debian/vauth-0.1.0
mkdir -p "$fixture/repository/$package_path" "$fixture/bin" "$fixture/slint" "$fixture/work"
cp "$repository_root/$package_path/build-local.sh" \
    "$repository_root/$package_path/changelog" \
    "$fixture/repository/$package_path/"
printf '' > "$fixture/slint/SlintConfig.cmake"
printf 'set(PACKAGE_VERSION "1.19.0")\n' > "$fixture/slint/SlintConfigVersion.cmake"

# Package construction is stubbed; real Git and dpkg-parsechangelog exercise
# clean archive staging and the version actually handed to Debian tools.
cat > "$fixture/bin/dpkg-buildpackage" <<'SH'
#!/bin/sh
set -eu
test "$*" = '--build=binary --no-sign'
test "$(dpkg-parsechangelog -ldebian/changelog -SVersion)" = "$EXPECTED_VERSION"
test "${PWD##*/}" = "vauth-$EXPECTED_UPSTREAM"
test "$(dpkg-parsechangelog -lpackaging/debian/vauth-0.1.0/changelog -SVersion)" = "$ORIGINAL_VERSION"
version_without_epoch=${EXPECTED_VERSION#*:}
touch "../vauth_${version_without_epoch}_all.deb" \
    "../vauth-dbgsym_${version_without_epoch}_all.deb" \
    "../vauth_${version_without_epoch}_all.buildinfo" \
    "../vauth_${version_without_epoch}_all.changes"
SH
cat > "$fixture/bin/lintian" <<'SH'
#!/bin/sh
set -eu
test "$#" -eq 1
test -f "$1"
SH
chmod +x "$fixture/bin/dpkg-buildpackage" "$fixture/bin/lintian"
export PATH="$fixture/bin:$PATH"
export VAUTH_SLINT_DIR="$fixture/slint"
export TMPDIR="$fixture/work"
export ORIGINAL_VERSION
ORIGINAL_VERSION=$(dpkg-parsechangelog -l"$fixture/repository/$package_path/changelog" -SVersion)

git -C "$fixture/repository" init --quiet
git -C "$fixture/repository" add packaging
git -C "$fixture/repository" -c user.name='vAuth packaging test' \
    -c user.email='test@example.invalid' commit --quiet --no-gpg-sign -m fixture

build_case() {
    local name=$1
    export EXPECTED_VERSION=$2
    export EXPECTED_UPSTREAM=${EXPECTED_VERSION#*:}
    EXPECTED_UPSTREAM=${EXPECTED_UPSTREAM%-*}
    export VAUTH_DEB_OUTPUT_DIR="$fixture/out-$name"
    sh "$fixture/repository/$package_path/build-local.sh"
    local version_without_epoch=${EXPECTED_VERSION#*:}
    for artifact in \
        "vauth_${version_without_epoch}_all.deb" \
        "vauth-dbgsym_${version_without_epoch}_all.deb" \
        "vauth_${version_without_epoch}_all.buildinfo" \
        "vauth_${version_without_epoch}_all.changes"; do
        test -f "$VAUTH_DEB_OUTPUT_DIR/$artifact"
    done
    cmp "$repository_root/$package_path/changelog" \
        "$fixture/repository/$package_path/changelog"
    test -z "$(git -C "$fixture/repository" status --porcelain)"
    test -z "$(find "$fixture/work" -mindepth 1 -print -quit)"
}

unset VAUTH_DEB_VERSION
build_case default "$ORIGINAL_VERSION"
export VAUTH_DEB_VERSION=''
build_case empty "$ORIGINAL_VERSION"
export VAUTH_DEB_VERSION='0.2.0~beta1-2+local1'
build_case override "$VAUTH_DEB_VERSION"
export VAUTH_DEB_VERSION='1:0.2.0-3+local2'
build_case epoch "$VAUTH_DEB_VERSION"

for invalid_version in 'invalid' '0.2.0/bad' '0.2.0 bad'; do
    export VAUTH_DEB_VERSION=$invalid_version
    export VAUTH_DEB_OUTPUT_DIR="$fixture/out-invalid"
    if sh "$fixture/repository/$package_path/build-local.sh" > "$fixture/error" 2>&1; then
        echo "Builder accepted invalid version: $invalid_version" >&2
        exit 1
    else
        test "$?" -eq 2
    fi
    grep -q 'VAUTH_DEB_VERSION must be a valid Debian package version' "$fixture/error"
    test ! -e "$VAUTH_DEB_OUTPUT_DIR"
    test -z "$(find "$fixture/work" -mindepth 1 -print -quit)"
done
