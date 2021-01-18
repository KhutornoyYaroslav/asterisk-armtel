.PHONY: all clean aics clean_aics

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
all: aics

#RTP SECTION 
aics: aics.so

aics.so : aics.o
	$(CC) -o $@ $(LFLAGS) $^

aics.o : aics.c

clean_aics:
	rm -f aics.o aics.so

#CLEAN SECTION
clean: clean_aics
	rm -f *.o *.so
