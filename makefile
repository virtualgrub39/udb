all: udb

config.h: config.def.h
	cp config.def.h config.h

udb: udb-daemon.c config.h
	cc -o udb udb-daemon.c $(UDB_LDFLAGS) $(UDB_CFLAGS)

clean:
	rm -rf udb *.o

.PHONY: all clean
