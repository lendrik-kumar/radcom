#!/bin/bash
set -e
echo "Installing radcom-master..."
apt-get install -y libsqlite3-dev
mkdir -p /var/lib/radcom
make clean && make
install -m 755 radcom-master /usr/local/bin/
install -m 644 radcom-master.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now radcom-master
echo "Done. Status:"
systemctl status radcom-master --no-pager
