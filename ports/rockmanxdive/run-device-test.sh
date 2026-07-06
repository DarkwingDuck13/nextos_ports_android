#!/bin/bash
# Build, deploy and run ROCKMAN X DiVE Offline on the NextOS device.
set -e

DEVICE=${DEVICE:-root@192.168.31.90}
REMOTE=${REMOTE:-/storage/roms/ports/rockmanxdive}
SSH_OPTS=(-F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

cd "$(dirname "$0")"
./build.sh

ssh "${SSH_OPTS[@]}" "$DEVICE" "mkdir -p '$REMOTE'"
rsync -avP --no-owner --no-group --no-perms -e "ssh -F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
  rockmanxdive run.sh payload/lib/*.so "$DEVICE:$REMOTE/"
rsync -avP --no-owner --no-group --no-perms -e "ssh -F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
  payload/assets/ "$DEVICE:$REMOTE/"

ssh "${SSH_OPTS[@]}" "$DEVICE" "chmod +x '$REMOTE/rockmanxdive' '$REMOTE/run.sh'"
ssh "${SSH_OPTS[@]}" "$DEVICE" "cd '$REMOTE' && ./run.sh"
