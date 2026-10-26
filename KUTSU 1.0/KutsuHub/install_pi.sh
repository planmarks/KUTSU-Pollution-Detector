#!/bin/sh
# Install KUTSU Hub as a service that starts on boot (Raspberry Pi OS / Debian / Ubuntu).
# Run from inside the KutsuHub folder:   sh install_pi.sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="$(id -un)"
PY="$(command -v python3 || true)"
[ -n "$PY" ] || { echo "python3 not found: sudo apt install python3"; exit 1; }
"$PY" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 7) else 1)' \
  || { echo "Python 3.7 or newer is required"; exit 1; }

sed -e "s|^User=.*|User=$USER_NAME|" \
    -e "s|^WorkingDirectory=.*|WorkingDirectory=$DIR|" \
    -e "s|^ExecStart=.*|ExecStart=$PY $DIR/kutsu_hub.py --db $DIR/kutsu_hub.db|" \
    "$DIR/kutsu-hub.service" | sudo tee /etc/systemd/system/kutsu-hub.service >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now kutsu-hub
sleep 2
systemctl --no-pager --lines=5 status kutsu-hub || true
IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo
echo "KUTSU Hub is running. Dashboard: http://${IP:-<this-pi>}:8080/"
echo "Logs: journalctl -u kutsu-hub -f    Stop: sudo systemctl stop kutsu-hub"
