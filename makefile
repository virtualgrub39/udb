SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

UDB_INCS = $(shell pkg-config --cflags gio-2.0 glib-2.0)
UDB_LIBS = $(shell pkg-config --libs gio-2.0 glib-2.0)

UDB_CFLAGS = $(CFLAGS) $(UDB_INCS) \
	-Wall -Wextra -pedantic -Werror \
	-ggdb
UDB_LDFLAGS = $(LDFLAGS) $(UDB_LIBS)

all: udb

config.h: config.def.h
	cp config.def.h config.h

.c.o:
	$(CC) $(UDB_CFLAGS) -c $<

udb.o: config.h

udb: $(OBJ)
	$(CC) -o $@ $(OBJ) $(UDB_LDFLAGS)

clean:
	rm -f udb $(OBJ)

commands:
	make clean; bear -- make

.PHONY: all clean commands
