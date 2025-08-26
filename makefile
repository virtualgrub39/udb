all: udb

UDB_CFLAGS = $(CFLAGS) -Wall -Wextra -pedantic -Werror
UDB_CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200112L
UDB_CFLAGS += -ggdb

config.h: config.def.h
	cp config.def.h config.h

udb: udb-daemon.c config.h
	cc -o $@ udb-daemon.c $(UDB_LDFLAGS) $(UDB_CFLAGS)

clean:
	rm -rf udb *.o

.PHONY: all clean
