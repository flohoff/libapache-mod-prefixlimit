#include "../trie.h"
#include "../yaml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static int lookup(trie_t *t, const char *ip) {
  unsigned char a[16];
  int bits;
  if (trie_parse_cidr(ip, a, &bits))
    return -2;
  return trie_lookup(t, a);
}

static void ins(trie_t *t, const char *cidr, int cls) {
  unsigned char a[16];
  int bits;
  CHECK(trie_parse_cidr(cidr, a, &bits) == 0);
  CHECK(trie_insert(t, a, bits, cls) == 0);
}

static void test_trie(void) {
  trie_t *t = trie_new();
  ins(t, "10.0.0.0/8", 1);
  ins(t, "10.1.0.0/16", 2);
  ins(t, "192.0.2.7", 3);
  ins(t, "2001:db8::/32", 4);
  CHECK(lookup(t, "10.2.3.4") == 1);
  CHECK(lookup(t, "10.1.3.4") == 2);
  CHECK(lookup(t, "11.0.0.1") == -1);
  CHECK(lookup(t, "192.0.2.7") == 3);
  CHECK(lookup(t, "192.0.2.8") == -1);
  CHECK(lookup(t, "2001:db8::1") == 4);
  CHECK(lookup(t, "2001:db9::1") == -1);
  CHECK(lookup(t, "::ffff:10.1.0.1") == 2);
  ins(t, "0.0.0.0/0", 9);
  CHECK(lookup(t, "8.8.8.8") == 9);
  CHECK(lookup(t, "2001:4860::1") == -1);
  trie_free(t);
}

static void test_cidr(void) {
  unsigned char a[16];
  int bits;
  CHECK(trie_parse_cidr("1.2.3.0/24", a, &bits) == 0 && bits == 120);
  CHECK(trie_parse_cidr("::1/128", a, &bits) == 0 && bits == 128);
  CHECK(trie_parse_cidr("1.2.3.0/33", a, &bits) != 0);
  CHECK(trie_parse_cidr("1.2.3/24", a, &bits) != 0);
  CHECK(trie_parse_cidr("1.2.3.0/x", a, &bits) != 0);
  CHECK(trie_parse_cidr("", a, &bits) != 0);
}

typedef struct {
  trie_t *t;
  int n;
  char last_cls[64];
} ctx_t;

static int on_entry(void *ud, const char *prefix, const char *cls, char *err,
                    size_t errlen) {
  ctx_t *c = ud;
  unsigned char a[16];
  int bits;
  if (trie_parse_cidr(prefix, a, &bits)) {
    snprintf(err, errlen, "bad prefix %s", prefix);
    return 1;
  }
  trie_insert(c->t, a, bits, 0);
  snprintf(c->last_cls, sizeof(c->last_cls), "%s", cls);
  c->n++;
  return 0;
}

static int parse(const char *text, ctx_t *c, char *err) {
  memset(c, 0, sizeof(*c));
  c->t = trie_new();
  return yaml_parse_prefixes(text, on_entry, c, err, 128);
}

static void test_yaml(void) {
  ctx_t c;
  char err[128];

  CHECK(parse("- prefix: 1.2.3.0/24\n  class: foo\n", &c, err) == 0);
  CHECK(c.n == 1 && !strcmp(c.last_cls, "foo"));
  trie_free(c.t);

  CHECK(parse("# c\n---\n- prefix: \"1.2.3.0/24\" # x\n- prefix: '::1'\n"
              "  class: bar\r\n",
              &c, err) == 0);
  CHECK(c.n == 2 && !strcmp(c.last_cls, "bar"));
  trie_free(c.t);

  CHECK(parse("- prefix: 1.2.3.0/24\n", &c, err) == 0);
  CHECK(!strcmp(c.last_cls, "default"));
  trie_free(c.t);

  CHECK(parse("- { prefix: 1.2.3.0/24, class: foo }\n", &c, err) == 0);
  CHECK(c.n == 1 && !strcmp(c.last_cls, "foo"));
  CHECK(lookup(c.t, "1.2.3.4") == 0);
  trie_free(c.t);

  CHECK(parse("- {prefix: \"::1/128\",class: 'bar'} # c\n"
              "- { prefix: 1.2.4.0/24 }\n"
              "- prefix: 1.2.5.0/24\n  class: blk\n"
              "- { class: baz, prefix: 1.2.6.0/24, }\n",
              &c, err) == 0);
  CHECK(c.n == 4 && !strcmp(c.last_cls, "baz"));
  trie_free(c.t);

  CHECK(parse("- { prefix: 1.2.3.0/24, class: foo\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("- { prefix: 1.2.3.0/24, bogus: x }\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("- { class: x }\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("- {}\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("- prefix: 1.2.3.0/24\n  { class: x }\n", &c, err) == 2);
  trie_free(c.t);

  CHECK(parse("- class: x\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("- prefix: 1.2.3.0/24\n  bogus: x\n", &c, err) == 2);
  trie_free(c.t);
  CHECK(parse("- prefix: nonsense\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("foo\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("  prefix: 1.2.3.0/24\n", &c, err) == 1);
  trie_free(c.t);
  CHECK(parse("- prefix\n", &c, err) == 1);
  trie_free(c.t);
}

static void test_file(const char *path) {
  FILE *f = fopen(path, "r");
  char *buf, err[128];
  long n;
  ctx_t c;
  if (!f) {
    printf("FAIL cannot open %s\n", path);
    fails++;
    return;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  rewind(f);
  buf = calloc(1, (size_t)n + 1);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n)
    fails++;
  fclose(f);
  CHECK(parse(buf, &c, err) == 0);
  CHECK(c.n == 3);
  CHECK(!strcmp(c.last_cls, "aicrawler"));
  CHECK(lookup(c.t, "192.0.2.5") == 0);
  CHECK(lookup(c.t, "8.8.8.8") == -1);
  trie_free(c.t);
  free(buf);
}

int main(int argc, char **argv) {
  test_trie();
  test_cidr();
  test_yaml();
  test_file(argc > 1 ? argv[1] : "examples/prefixes.yaml");
  printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
  return fails != 0;
}
