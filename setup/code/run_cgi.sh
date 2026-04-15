#!/usr/bin/env bash
set -e

make clean

make cgi_get.cgi
make practice2.cgi
make login.cgi
make register.cgi
make home.cgi
make query_train.cgi
make query_route.cgi
make book.cgi
make orders.cgi
make admin.cgi

sudo cp cgi_get.cgi /var/www/cgi-bin/cgi_get.cgi
sudo cp practice2.cgi /var/www/cgi-bin/practice2.cgi
sudo cp login.cgi /var/www/cgi-bin/login.cgi
sudo cp register.cgi /var/www/cgi-bin/register.cgi
sudo cp home.cgi /var/www/cgi-bin/home.cgi
sudo cp query_train.cgi /var/www/cgi-bin/query_train.cgi
sudo cp query_route.cgi /var/www/cgi-bin/query_route.cgi
sudo cp book.cgi /var/www/cgi-bin/book.cgi
sudo cp orders.cgi /var/www/cgi-bin/orders.cgi
sudo cp admin.cgi /var/www/cgi-bin/admin.cgi

sudo chmod +x /var/www/cgi-bin/cgi_get.cgi
sudo chmod +x /var/www/cgi-bin/practice2.cgi
sudo chmod +x /var/www/cgi-bin/login.cgi
sudo chmod +x /var/www/cgi-bin/register.cgi
sudo chmod +x /var/www/cgi-bin/home.cgi
sudo chmod +x /var/www/cgi-bin/query_train.cgi
sudo chmod +x /var/www/cgi-bin/query_route.cgi
sudo chmod +x /var/www/cgi-bin/book.cgi
sudo chmod +x /var/www/cgi-bin/orders.cgi
sudo chmod +x /var/www/cgi-bin/admin.cgi

echo "done"