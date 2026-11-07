#!/bin/sh
# Prints the version a build reports (issue #61): the title version from
# scripts/package-cia.sh, as it is ("0.4.0") only for a release build, from
# a commit tagged v<version> with nothing uncommitted; every other build is
# "<version>-dev", so its reports never mix with the release's in Sentry.
# Runs on the host (it needs the repository's git); without git: -dev.
set -eu
cd "$(dirname "$0")/.."
version="$(sed -n 's/^version=\([0-9.]*\).*/\1/p' scripts/package-cia.sh)"
if [ "$(git describe --exact-match --tags HEAD 2>/dev/null)" = "v$version" ] &&
    [ -z "$(git status --porcelain 2>/dev/null || echo unknown)" ]; then
    echo "$version"
else
    echo "$version-dev"
fi
