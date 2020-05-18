CC=gcc
CFLAGS=-O3 -Wall -Wextra
EXTRA_FLAGS=-D

all: client server

verbose: client.c client.h server.c server.h common.c common.h myUDP.h 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ client.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ server.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ common.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ client.o common.o -o client -pthread -lm 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ server.o common.o -o server -pthread -lm 

adaptive: client.c client.h server.c server.h common.c common.h myUDP.h
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ client.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ server.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ common.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ client.o common.o -o client -pthread -lm 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ server.o common.o -o server -pthread -lm 

adaptive-verbose: client.c client.h server.c server.h common.c common.h myUDP.h
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) verbose $(EXTRA_FLAGS) adaptive client.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) verbose $(EXTRA_FLAGS) adaptive server.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) verbose $(EXTRA_FLAGS) adaptive common.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) verbose $(EXTRA_FLAGS) adaptive client.o common.o -o client -pthread -lm 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) verbose $(EXTRA_FLAGS) adaptive server.o common.o -o server -pthread -lm 

debug: client.c client.h server.c server.h common.c common.h myUDP.h
	$(CC) $(CFLAGS) -g client.c -c 
	$(CC) $(CFLAGS) -g server.c -c 
	$(CC) $(CFLAGS) -g common.c -c 
	$(CC) $(CFLAGS) -g client.o common.o -o client -pthread -lm 
	$(CC) $(CFLAGS) -g server.o common.o -o server -pthread -lm 

debugadaptive: client.c client.h server.c server.h common.c common.h myUDP.h
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ -g client.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ -g server.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ -g common.c -c 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ -g client.o common.o -o client -pthread -lm 
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) $@ -g server.o common.o -o server -pthread -lm 

client.o: client.c client.h myUDP.h
	$(CC) $(CFLAGS) client.c -c

server.o: server.c server.h myUDP.h
	$(CC) $(CFLAGS) server.c -c

common.o: common.c common.h myUDP.h
	$(CC) $(CFLAGS) common.c -c

client: client.o common.o myUDP.h 
	$(CC) $(CFLAGS) client.o common.o -o $@ -pthread -lm

server: server.o common.o myUDP.h
	$(CC) $(CFLAGS) server.o common.o -o $@ -pthread -lm

clean:
	rm -f client server client.o server.o common.o files_client/*.txt
