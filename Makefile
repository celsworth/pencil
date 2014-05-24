CFLAGS=-O2 -g -pipe -pedantic -std=iso9899:1999
CFLAGS+= -Wsystem-headers -Wall -Wno-format-y2k -W -Wno-unused-parameter\
	-Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wreturn-type\
	-Wcast-qual -Wwrite-strings -Wswitch -Wshadow -Wcast-align -Winline \
	-Wunused-parameter -Wchar-subscripts -Wnested-externs -Wredundant-decls
LDFLAGS=

CC=gcc

SOURCES=pencil.c
OBJECTS=pencil.o

all:	pencil

pencil:	$(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@

pencil.o: pencil.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f pencil pencil.gmon pencil.core $(OBJECTS)
