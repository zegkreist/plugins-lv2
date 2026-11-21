PLUGINS := baixo-gate baixo-fuzz

all install uninstall clean test:
	@for p in $(PLUGINS); do $(MAKE) -C $$p $@ || exit 1; done

.PHONY: all install uninstall clean test
