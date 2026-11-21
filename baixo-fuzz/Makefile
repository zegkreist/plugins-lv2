BUNDLE  := baixo-fuzz.lv2
PREFIX  ?= $(HOME)/.lv2
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -std=c11 -fPIC -fvisibility=hidden -D_DEFAULT_SOURCE -I../common $(shell pkg-config --cflags lv2)

all: $(BUNDLE)/baixo_fuzz.so

$(BUNDLE)/baixo_fuzz.so: baixo_fuzz.c ../common/bass_split.h
	$(CC) $(CFLAGS) -shared $< -o $@ -lm

test: all test_fuzz
	./test_fuzz $(BUNDLE)/baixo_fuzz.so align
	./test_fuzz $(BUNDLE)/baixo_fuzz.so alias

test_fuzz: test_fuzz.c
	$(CC) -O2 -Wall -std=c11 -D_DEFAULT_SOURCE $(shell pkg-config --cflags lv2) $< -o $@ -ldl -lm

install: all
	mkdir -p $(PREFIX)/$(BUNDLE)
	cp $(BUNDLE)/manifest.ttl $(BUNDLE)/baixo_fuzz.ttl $(BUNDLE)/baixo_fuzz.so $(PREFIX)/$(BUNDLE)/

uninstall:
	rm -rf $(PREFIX)/$(BUNDLE)

clean:
	rm -f $(BUNDLE)/baixo_fuzz.so test_fuzz

.PHONY: all test install uninstall clean
