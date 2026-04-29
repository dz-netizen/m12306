#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

for f in html/*.html; do
    name=$(basename "$f")
    sudo cp "$f" "/var/www/html/$name"
done

sudo cp html/style.css /var/www/html/style.css
sudo cp assets/logo.png /var/www/html/logo.png

echo "HTML and assets deployed to /var/www/html/"
