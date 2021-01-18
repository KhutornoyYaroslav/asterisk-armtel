.PHONY: all clean rtp clean_rtp

#***** move to top-level makefile *****
export CC = gcc 
export CFLAGS = -pthread -pipe -Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -g3 -O3 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -march=native -fPIC
export LFLAGS = -pthread -shared -Wl,--warn-common
#**************************************

INCLUDE = ../../include

.SUFFIXES :
.SUFFIXES : .o .c

.c.o:
	$(CC) -c $< -o $@ $(CFLAGS) -I$(INCLUDE)

#ALL SECTION
all: rtp

#RTP SECTION 
rtp: res_aics_rtp_asterisk.so

res_aics_rtp_asterisk.so : res_rtp_asterisk.o
	$(CC) -o $@ $(LFLAGS),--version-script,res_rtp_asterisk.exports $^

res_rtp_asterisk.o : res_rtp_asterisk.c

clean_rtp:
	rm -f res_rtp_asterisk.o

#CLEAN SECTION
clean: clean_rtp
	rm -f *.o *.so
