#!/bin/bash
make clean
#make menuselect
./configure --prefix=/usr --libdir=/usr/lib64
make
sudo systemctl stop asterisk
sudo make install
#sudo systemctl start asterisk
