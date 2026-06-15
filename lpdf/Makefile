CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic
CFLAGS  += $(shell pkg-config --cflags lexicon-pdf)
LDFLAGS := $(shell pkg-config --libs   lexicon-pdf -static)

lpdf: lpdf.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f lpdf

.PHONY: clean