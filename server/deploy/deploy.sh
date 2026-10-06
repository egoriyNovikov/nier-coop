#!/usr/bin/env bash
# Uploads the current server code to the VPS, builds it there and restarts the service.
# Run from Git Bash: ./server/deploy/deploy.sh
# The VPS keeps its own /opt/nier-coop/server.json (password etc.), it is never overwritten.
set -euo pipefail

HOST="${NIER_VPS_HOST:-root@95.163.229.188}"
KEY="${NIER_VPS_KEY:-$HOME/.ssh/nier_vps}"
SSH=(ssh -i "$KEY" -o IdentitiesOnly=yes "$HOST")

cd "$(dirname "$0")/.."

echo ">> Uploading sources to $HOST"
tar -czf - automatamp go.mod go.sum main.go | "${SSH[@]}" '
    set -e
    rm -rf /opt/nier-coop/src
    mkdir -p /opt/nier-coop/src
    tar -xzf - -C /opt/nier-coop/src
'

echo ">> Updating systemd unit"
"${SSH[@]}" 'cat > /etc/systemd/system/nier-coop.service && systemctl daemon-reload' < deploy/nier-coop.service

echo ">> Building and restarting"
"${SSH[@]}" '
    set -e
    cd /opt/nier-coop/src
    go build -o /opt/nier-coop/server.new main.go
    install -m 755 /opt/nier-coop/server.new /opt/nier-coop/server
    rm -f /opt/nier-coop/server.new
    systemctl restart nier-coop
    sleep 2
    systemctl --no-pager --lines=15 status nier-coop
'
