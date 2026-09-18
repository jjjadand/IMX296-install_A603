CC ?= aarch64-linux-gnu-gcc
CFLAGS ?= -O3 -fopenmp -Wall

camview: camview.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f camview

.PHONY: clean
