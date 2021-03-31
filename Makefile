VPATH = dumpfloppy

CFLAGS = -I dumpfloppy

all: raw2imd imdcat

imdcat: imdcat.c imd.o util.o disk.o show.o
	$(CC) $(CFLAGS) -o $@ $^

raw2imd: raw2imd.c imd.o util.o disk.o show.o
	$(CC) $(CFLAGS) -o $@ $^
