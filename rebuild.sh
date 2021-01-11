#!/bin/bash

sudo chmod -R 777 *
rm ./menuselect.makeopts

sudo make clean
sudo make uninstall #use "uninstall-all" for removing configure files, logs, etc.

sudo ./configure --prefix=/usr --libdir=/usr/lib64

#sudo ./configure --prefix=/usr --libdir=/usr/lib64 --enable-shared --disable-sound --disable-#resample --disable-video --disable-opencore-amr

sudo make dep
sudo make
sudo systemctl stop asterisk
sudo make install
sudo ldconfig

#ldconfig -p | grep pj
#sudo ./configure --libdir=/usr/lib64
#sudo make menuselect
#sudo make samples

#lsmod | grep dahdi #check if DAHDI is running
#/etc/init.d/dadhi start #run it for starting DAHDI

#/etc/init.d/asterisk status #check if ASTERISK is running
#/etc/init.d/asterisk start  #run it for starting ASTERISK
sudo systemctl start asterisk #run it for starting ASTERISK

#sudo asterisk -rvvvvv #connect to ASTERISK CLI (command line interface)
