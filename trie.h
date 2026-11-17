#ifndef TRIE_H
#define TRIE_H

/* Binary prefix trie over 128-bit keys; IPv4 is stored v4-mapped. */
typedef struct trie trie_t;

trie_t *trie_new(void);
void trie_free(trie_t *t);

/* Insert addr/bits (bits in 0..128, v6 space). Returns 0 or -1 (oom). */
int trie_insert(trie_t *t, const unsigned char addr[16], int bits, int cls);

/* Longest-prefix match. Returns class id or -1. */
int trie_lookup(const trie_t *t, const unsigned char addr[16]);

/* Parse "a.b.c.d[/n]" or "v6[/n]" into v6-space addr/bits. 0 ok, -1 bad. */
int trie_parse_cidr(const char *s, unsigned char addr[16], int *bits);

#endif
