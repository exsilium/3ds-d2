#!/usr/bin/env bash
# Creates the Sentry project that problem reports go to (issue #26) and
# writes its DSN to packaging/cia/report.dsn, which package-cia builds in.
# Run once, or again to pick up a rotated key; an existing team or project
# is reused. Usage:
#
#   SENTRY_AUTH_TOKEN=<token> scripts/sentry-setup.sh [<org> [<project>]]
#
# The token needs the org:read, team:write and project:write scopes (a
# personal token: Sentry > User Settings > Personal Tokens). It is only used
# here; the DSN that goes into the build is a public key that can only send
# events. Defaults: org exsilium, project 3ds-d2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORG="${1:-exsilium}"
PROJECT="${2:-3ds-d2}"
TEAM="${SENTRY_TEAM:-$PROJECT}"
OUT="$ROOT/packaging/cia/report.dsn"

die() { echo "sentry-setup.sh: $*" >&2; exit 1; }
[ -n "${SENTRY_AUTH_TOKEN:-}" ] || die "set SENTRY_AUTH_TOKEN (org:read, team:write, project:write)"
PY="$(command -v python3 || command -v python)" || die "needs python3 for the JSON"

# api METHOD URL [JSON]: the body on stdout, the HTTP status in $status.
api() {
    local tmp; tmp="$(mktemp)"
    status=$(curl -sS -o "$tmp" -w '%{http_code}' -X "$1" -H "Authorization: Bearer $SENTRY_AUTH_TOKEN" \
        -H 'Content-Type: application/json' ${3:+--data "$3"} "$2")
    cat "$tmp"; rm -f "$tmp"
}
json() { "$PY" -c "import json,sys; d=json.load(sys.stdin); print($1)"; }

# The organization lives in one region (us.sentry.io, de.sentry.io); its API is there.
body=$(api GET "https://sentry.io/api/0/organizations/$ORG/")
[ "$status" = 200 ] || die "organization $ORG: HTTP $status: $body"
BASE=$(printf '%s' "$body" | json "d.get('links',{}).get('regionUrl') or 'https://sentry.io'")
echo "organization $ORG at $BASE"

# A team to own the project: the named one, made if the org does not have it.
body=$(api GET "$BASE/api/0/organizations/$ORG/teams/")
[ "$status" = 200 ] || die "teams: HTTP $status: $body"
if printf '%s' "$body" | json "any(t['slug']=='$TEAM' for t in d)" | grep -q True; then
    echo "team $TEAM: there"
else
    body=$(api POST "$BASE/api/0/organizations/$ORG/teams/" "{\"name\":\"$TEAM\",\"slug\":\"$TEAM\"}")
    [ "$status" = 201 ] || die "creating team $TEAM: HTTP $status: $body"
    echo "team $TEAM: created"
fi

# The project: native (C/C++), so stack-less crash events and attachments read as such.
body=$(api GET "$BASE/api/0/projects/$ORG/$PROJECT/")
if [ "$status" = 200 ]; then
    echo "project $PROJECT: there"
else
    body=$(api POST "$BASE/api/0/teams/$ORG/$TEAM/projects/" \
        "{\"name\":\"$PROJECT\",\"slug\":\"$PROJECT\",\"platform\":\"native\",\"default_rules\":true}")
    [ "$status" = 201 ] || die "creating project $PROJECT: HTTP $status: $body"
    echo "project $PROJECT: created"
fi

# Its first active client key's DSN.
body=$(api GET "$BASE/api/0/projects/$ORG/$PROJECT/keys/")
[ "$status" = 200 ] || die "keys: HTTP $status: $body"
DSN=$(printf '%s' "$body" | json "next((k['dsn']['public'] for k in d if k.get('isActive', True)), '')")
[ -n "$DSN" ] || die "project $PROJECT has no active client key"
printf '%s\n' "$DSN" > "$OUT"
echo "DSN: $DSN"
echo "written to ${OUT#$ROOT/}; package-cia builds it in"
