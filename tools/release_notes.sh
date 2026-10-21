#!/bin/sh
# Prints the GitHub release notes for a version: download instructions plus that version's section of
# CHANGELOG.md. Fails when the changelog has no section for it, so a release never ships without notes.
# usage: tools/release_notes.sh <version, e.g. 0.2.1 or v0.2.1>
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1#v}"
[ -n "$VER" ] || { echo "usage: $0 <version>" >&2; exit 2; }

# the section runs from "## [VER]" to the next "## [" heading; GitHub shows every newline in release
# notes as a line break, so the changelog's wrapped lines (indented continuations) are joined
BODY=$(awk -v v="$VER" '
  /^## \[/ { if (f) exit; if (index($0, "## [" v "]") == 1) { f = 1; next } }
  !f { next }
  /^  [^ ]/ && cur != "" { sub(/^ +/, ""); cur = cur " " $0; next }
  { if (started || cur != "") print cur; started = 1; cur = $0 }
  END { if (f) print cur }
' "$P/CHANGELOG.md" | sed '/./,$!d')
[ -n "$(printf '%s' "$BODY" | tr -d '[:space:]')" ] || { echo "CHANGELOG.md has no entries for $VER" >&2; exit 1; }

cat <<EOF
**Download \`OpenPopTB-MacOS-$VER.zip\` below**, unzip it and open \`Populous TB.app\` (\`Populous UW.app\` for Undiscovered Worlds, \`Populous TB (Direct3D).app\` for hardware rendering).

**Requirements**
- Apple Silicon Mac, macOS 14 or later
- Apple's Command Line Tools (the app offers to install them)
- Your own copy of the game: the GOG offline installer or an installed game folder. The download contains no game code or data; the first launch recompiles your copy (about a minute), later launches take seconds.

$BODY
EOF
