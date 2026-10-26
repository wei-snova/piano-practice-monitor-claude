CFLAGS = -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE
CORE   = ../main/detector.c ../main/session.c ../main/classifier.c

all: host_test classify

host_test: host_test.c $(CORE)
	$(CC) $(CFLAGS) -o $@ host_test.c $(CORE) -lm
	./host_test

classify: classify_test.c $(CORE)
	$(CC) $(CFLAGS) -o classify_test classify_test.c $(CORE) -lm
	./classify_test

clean:
	rm -f host_test classify_test

.PHONY: all classify clean
