CC=gcc
CFLAGS=-O3
CPPFLAGS=-D debug

all: client server

debug: client.c client.h server.c server.h common.c common.h myUDP.h 
	$(CC) $(CFLAGS) $(CPPFLAGS) client.c -c 
	$(CC) $(CFLAGS) $(CPPFLAGS) server.c -c 
	$(CC) $(CFLAGS) $(CPPFLAGS) common.c -c 
	$(CC) $(CFLAGS) $(CPPFLAGS) client.o common.o -o client -pthread -lm 
	$(CC) $(CFLAGS) $(CPPFLAGS) server.o common.o -o server -pthread -lm 

client.o: client.c client.h 
	$(CC) $(CFLAGS) client.c -c

server.o: server.c server.h 
	$(CC) $(CFLAGS) server.c -c

common.o: common.c common.h
	$(CC) $(CFLAGS) common.c -c

client: client.o common.o myUDP.h
	$(CC) $(CFLAGS) client.o common.o -o $@ -pthread -lm

server: server.o common.o myUDP.h
	$(CC) $(CFLAGS) server.o common.o -o $@ -pthread -lm

clean:
	rm -f client server client.o server.o common.o client_files/*
