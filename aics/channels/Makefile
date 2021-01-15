.PHONY: all clean radiohyt clean_radiohyt

#***** move to top-level makefile *****
export CC = gcc 
export CFLAGS = -pthread -pipe -Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -g3 -O3 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -march=native -fPIC
export LFLAGS = -pthread -shared -Wl,--warn-common
#**************************************

INCLUDE = ../../include
INCLUDE2 = /usr/include/libxml2

.SUFFIXES :
.SUFFIXES : .o .c

.c.o:
	$(CC) -c $< -o $@ $(CFLAGS) -I$(INCLUDE) -I$(INCLUDE2)

#ALL SECTION
all: radiohyt

#RADIOHYT SECTION 
OBJS_RADIOHYT = chan_radiohyt.o radiohyt/chan_radiohyt_functions.o radiohyt/monitor.o radiohyt/msg_parser.o

radiohyt: chan_radiohyt.so

chan_radiohyt.so : $(OBJS_RADIOHYT)
	$(CC) -o $@ $(LFLAGS),--version-script,chan_radiohyt.exports $^

chan_radiohyt.o : chan_radiohyt.c
radiohyt/chan_radiohyt_functions.o : radiohyt/chan_radiohyt_functions.c
radiohyt/monitor.o : radiohyt/monitor.c
radiohyt/msg_parser.o : radiohyt/msg_parser.c

clean_radiohyt:
	rm -f $(OBJS_RADIOHYT)

#CLEAN SECTION
clean: clean_radiohyt
	rm -f *.o *.so
