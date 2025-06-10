CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -pthread

all: ring_buffer_test

ring_buffer_test: main.c atomic_event_ring_buffer.c
	$(CC) $(CFLAGS) -o ring_buffer_test main.c atomic_event_ring_buffer.c

clean:
	rm -f ring_buffer_test
