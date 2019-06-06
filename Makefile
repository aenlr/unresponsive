

all: unresponsive

unresponsive: unresponsive.c
	gcc -Wall -g -o $@ $<

