#!/usr/bin/env bash
# Run on the VM in a real terminal: store the Gmail app password the rollout emails with.
# The password is read without echo and written straight to a file only zach can read.
set -euo pipefail
umask 077
read -rs -p "Gmail app password (16 letters, spaces are fine): " p
echo
[[ -n "$p" ]] || { echo "nothing entered; nothing saved"; exit 1; }
printf '%s' "$p" > ~/.config/ota-rollout/gmail-app-password
echo "saved"
