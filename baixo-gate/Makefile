BUNDLE  := baixo-gate.lv2
PREFIX  ?= $(HOME)/.lv2
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -std=c11 -fPIC -fvisibility=hidden -D_DEFAULT_SOURCE $(shell pkg-config --cflags lv2)

all: $(BUNDLE)/baixo_gate.so

$(BUNDLE)/baixo_gate.so: baixo_gate.c
	$(CC) $(CFLAGS) -shared $< -o $@ -lm

test: all test_gate
	./test_gate $(BUNDLE)/baixo_gate.so

test_gate: test_gate.c
	$(CC) -O2 -Wall -std=c11 -D_DEFAULT_SOURCE $(shell pkg-config --cflags lv2) $< -o $@ -ldl -lm

install: all
	mkdir -p $(PREFIX)/$(BUNDLE)
	cp $(BUNDLE)/manifest.ttl $(BUNDLE)/baixo_gate.ttl $(BUNDLE)/baixo_gate.so $(PREFIX)/$(BUNDLE)/

uninstall:
	rm -rf $(PREFIX)/$(BUNDLE)

clean:
	rm -f $(BUNDLE)/baixo_gate.so test_gate

.PHONY: all test install uninstall clean
