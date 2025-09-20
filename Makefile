CC = gcc
CFLAGS = -Wall -O2
LIBS = -lX11 -lxkbfile

all: thing

thing: a.c
	$(CC) $(CFLAGS) -o  x11_status_bar  a.c  -I/usr/include/freetype2  -lX11  -lXft  -lfontconfig  -lm $(LIBS)

clean:
	rm -f thing

