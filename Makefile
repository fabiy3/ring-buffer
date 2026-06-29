CC     = gcc
CFLAGS = -Wall -Wextra -pedantic -Iinclude
LIBS   = -lpthread

# -Wall     = enable common warnings
# -Wextra   = enable extra warnings
# -pedantic = enforce strict C standard compliance
# -Iinclude = look in the include/ folder for .h files
# -lpthread = link the POSIX threads library (needed for mutex)

all: sanity stress_test

# The sanity check: ring_buffer.c (the library) + sanity.c (the program)
sanity: src/ring_buffer.c src/sanity.c
	$(CC) $(CFLAGS) -o sanity $^ $(LIBS)

# The stress test: ring_buffer.c (the library) + stress_test.c (the test)
stress_test: src/ring_buffer.c tests/stress_test.c
	$(CC) $(CFLAGS) -o stress_test $^ $(LIBS)

clean:
	rm -f sanity stress_test
