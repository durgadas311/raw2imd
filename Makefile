imdcat: imdcat.c imd.o util.o disk.o show.o
	$(CC) $(CFLAGS) -o $@ $^

dumpfloppy: dumpfloppy.c imd.o util.o disk.o
	$(CC) $(CFLAGS) -o $@ $^

raw2imd: raw2imd.c imd.o util.o disk.o
	$(CC) $(CFLAGS) -o $@ $^
