APXS ?= apxs
CC ?= gcc
APXS_CFLAGS ?= -Wall
APXS_LDFLAGS ?=
CFLAGS_T ?= -Wall -Wextra -g -fsanitize=address,undefined
TESTBIN = test/test_trie

all: module

module: .libs/mod_prefixlimit.so

.libs/mod_prefixlimit.so: mod_prefixlimit.c trie.c yaml.c trie.h yaml.h
	$(APXS) -c -Wc,"$(APXS_CFLAGS)" $(if $(APXS_LDFLAGS),-Wl$(comma)"$(APXS_LDFLAGS)") \
		-o mod_prefixlimit.la mod_prefixlimit.c trie.c yaml.c

comma = ,

$(TESTBIN): test/test_trie.c trie.c yaml.c trie.h yaml.h
	$(CC) $(CFLAGS_T) -o $@ test/test_trie.c trie.c yaml.c

test: $(TESTBIN)
	./$(TESTBIN) examples/prefixes.yaml

# local httpd integration test (needs apache2, curl, ab)
itest: module
	bash test/run.sh

install: module
	$(APXS) -i -n prefixlimit mod_prefixlimit.la

clean:
	rm -rf $(TESTBIN) .libs *.o *.lo *.la *.slo

.PHONY: all module test itest install clean
