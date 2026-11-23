BUNDLE  := baixo-delay.lv2
PREFIX  ?= $(HOME)/.lv2
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -std=c11 -fPIC -fvisibility=hidden -D_DEFAULT_SOURCE -I../common $(shell pkg-config --cflags lv2)

all: $(BUNDLE)/baixo_delay.so

$(BUNDLE)/baixo_delay.so: baixo_delay.c ../common/bass_split.h
	$(CC) $(CFLAGS) -shared $< -o $@ -lm

test: all test_delay
	./test_delay $(BUNDLE)/baixo_delay.so echo
	./test_delay $(BUNDLE)/baixo_delay.so stable

test_delay: test_delay.c
	$(CC) -O2 -Wall -std=c11 -D_DEFAULT_SOURCE $(shell pkg-config --cflags lv2) $< -o $@ -ldl -lm

install: all
	mkdir -p $(PREFIX)/$(BUNDLE)
	cp $(BUNDLE)/manifest.ttl $(BUNDLE)/baixo_delay.ttl $(BUNDLE)/baixo_delay.so $(PREFIX)/$(BUNDLE)/

uninstall:
	rm -rf $(PREFIX)/$(BUNDLE)

clean:
	rm -f $(BUNDLE)/baixo_delay.so test_delay

.PHONY: all test install uninstall clean
