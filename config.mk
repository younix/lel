VERSION = 0.1

# customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

# includes and libs
INCS =
LIBS = -lc -lX11

# debug
CFLAGS = -O0 -g -std=c99 -Wall -pedantic -DVERSION=\"${VERSION}\"
LDFLAGS = ${LIBS}

# optimized
#CFLAGS = -O2 -std=c99 -DVERSION=\"${VERSION}\"
#LDFLAGS = -s ${LIBS}

# tcc
#CC = tcc
#CFLAGS = -DVERSION=\"${VERSION}\"
#LDFLAGS = -s ${LIBS}

# compiler and linker
CC = cc
