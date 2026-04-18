#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

for f in html/*.html; do
    name=$(basename "$f")
    sudo cp "$f" "/var/www/html/$name"
done

echo "HTML deployed to /var/www/html/"
