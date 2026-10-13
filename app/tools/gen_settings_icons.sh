#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ICONS=(
  "cpu:icon_cpu.png"
  "volume-2:icon_volume.png"
  "gamepad-2:icon_gamepad.png"
  "keyboard:icon_keyboard.png"
  "activity:icon_activity.png"
  "tv:icon_tv.png"
)

for item in "${ICONS[@]}"; do
  name="${item%%:*}"
  file="${item##*:}"
  ./tools/lucide_icon.sh "${name}" "projects/evoplayer/assets/icons/${file}" 96
done

echo "All settings icons generated successfully."
