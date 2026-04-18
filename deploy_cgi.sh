#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

make

for f in build/*.cgi; do
    name=$(basename "$f")
    sudo cp "$f" "/var/www/cgi-bin/$name"
    sudo chmod +x "/var/www/cgi-bin/$name"
done

echo "CGI deployed to /var/www/cgi-bin/"
