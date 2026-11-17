#include "trie.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int child[2];
  int cls;
} node_t;

struct trie {
  node_t *nodes;
  int n, cap;
};

static int new_node(trie_t *t) {
  if (t->n == t->cap) {
    int cap = t->cap ? t->cap * 2 : 256;
    node_t *p = realloc(t->nodes, (size_t)cap * sizeof(*p));
    if (!p)
      return -1;
    t->nodes = p;
    t->cap = cap;
  }
  t->nodes[t->n].child[0] = t->nodes[t->n].child[1] = -1;
  t->nodes[t->n].cls = -1;
  return t->n++;
}

trie_t *trie_new(void) {
  trie_t *t = calloc(1, sizeof(*t));
  if (t && new_node(t) < 0) {
    free(t);
    return NULL;
  }
  return t;
}

void trie_free(trie_t *t) {
  if (t) {
    free(t->nodes);
    free(t);
  }
}

static int bit(const unsigned char *a, int i) {
  return (a[i >> 3] >> (7 - (i & 7))) & 1;
}

int trie_insert(trie_t *t, const unsigned char addr[16], int bits, int cls) {
  int cur = 0;
  if (bits < 0 || bits > 128)
    return -1;
  for (int i = 0; i < bits; i++) {
    int b = bit(addr, i);
    int nx = t->nodes[cur].child[b];
    if (nx < 0) {
      nx = new_node(t);
      if (nx < 0)
        return -1;
      t->nodes[cur].child[b] = nx;
    }
    cur = nx;
  }
  t->nodes[cur].cls = cls;
  return 0;
}

int trie_lookup(const trie_t *t, const unsigned char addr[16]) {
  int cur = 0, best = t->nodes[0].cls;
  for (int i = 0; i < 128; i++) {
    cur = t->nodes[cur].child[bit(addr, i)];
    if (cur < 0)
      break;
    if (t->nodes[cur].cls >= 0)
      best = t->nodes[cur].cls;
  }
  return best;
}

int trie_parse_cidr(const char *s, unsigned char addr[16], int *bits) {
  char buf[64], *slash, *end;
  size_t len = strlen(s);
  int maxbits, n;
  struct in_addr a4;

  if (len == 0 || len >= sizeof(buf))
    return -1;
  memcpy(buf, s, len + 1);
  slash = strchr(buf, '/');
  if (slash)
    *slash = '\0';

  if (strchr(buf, ':')) {
    if (inet_pton(AF_INET6, buf, addr) != 1)
      return -1;
    maxbits = 128;
  } else {
    if (inet_pton(AF_INET, buf, &a4) != 1)
      return -1;
    memset(addr, 0, 10);
    addr[10] = addr[11] = 0xff;
    memcpy(addr + 12, &a4, 4);
    maxbits = 32;
  }
  n = maxbits;
  if (slash) {
    long v = strtol(slash + 1, &end, 10);
    if (end == slash + 1 || *end || v < 0 || v > maxbits)
      return -1;
    n = (int)v;
  }
  *bits = maxbits == 32 ? n + 96 : n;
  return 0;
}
