#!/usr/bin/env bash
set -euo pipefail
MCONNECT_PACKAGE="$(cd "$(dirname "$0")" && pwd)"
MCONNECT_COMPONENTS="$HOME/Library/Audio/Plug-Ins/Components"
MCONNECT_VST3="$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$MCONNECT_COMPONENTS" "$MCONNECT_VST3"
for MCONNECT_KIND in component vst3; do
  if [[ "$MCONNECT_KIND" == component ]]; then MCONNECT_DEST="$MCONNECT_COMPONENTS"; else MCONNECT_DEST="$MCONNECT_VST3"; fi
  MCONNECT_BUNDLE="MConnect.$MCONNECT_KIND"
  if [[ -e "$MCONNECT_DEST/$MCONNECT_BUNDLE" ]]; then
    mv "$MCONNECT_DEST/$MCONNECT_BUNDLE" "$MCONNECT_DEST/$MCONNECT_BUNDLE.backup-$(date +%Y%m%d-%H%M%S)"
  fi
  ditto "$MCONNECT_PACKAGE/$MCONNECT_BUNDLE" "$MCONNECT_DEST/$MCONNECT_BUNDLE"
done
printf 'Installed MConnect AU and VST3 for this user. Restart your DAW and rescan plugins.\n'
