#!/usr/bin/env bash
set -e

sudo cp m12306index.html /var/www/html/m12306index.html
sudo cp register.html /var/www/html/register.html
sudo cp query_train.html /var/www/html/query_train.html
sudo cp query_route.html /var/www/html/query_route.html
sudo cp index.html /var/www/html/index.html

echo "done"
