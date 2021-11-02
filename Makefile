CFLAGS = -g

all: froot1 bin2rom rom2bin

froot1: fake6502.o froot1.o
	gcc ${CFLAGS} -o froot1 froot1.o fake6502.o

bin2rom: bin2rom.o
	gcc ${CFLAGS} -o bin2rom bin2rom.o

rom2bin: rom2bin.o
	gcc ${CFLAGS} -o rom2bin rom2bin.o

clean:
	rm -f froot1 bin2rom rom2bin *.o

.c.o:
	gcc ${CFLAGS} -c $<
