#include "yaml.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 512
#define VAL_MAX_LEN 128

typedef struct {
  char prefix[VAL_MAX_LEN], cls[VAL_MAX_LEN];
  int have_prefix, line;
} entry_t;

static int fail(char *err, size_t n, const char *msg) {
  snprintf(err, n, "%s", msg);
  return 1;
}

/* Strip comment (# at start or after whitespace, outside quotes), trim. */
static char *clean(char *s) {
  char q = 0, *p, *e;
  for (p = s; *p; p++) {
    if (q) {
      if (*p == q)
        q = 0;
    } else if (*p == '"' || *p == '\'') {
      q = *p;
    } else if (*p == '#' && (p == s || isspace((unsigned char)p[-1]))) {
      *p = '\0';
      break;
    }
  }
  e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = '\0';
  return s;
}

static char *unquote(char *v) {
  size_t n = strlen(v);
  if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
    v[n - 1] = '\0';
    return v + 1;
  }
  return v;
}

/* Handle "key: value"; returns 0 ok. */
static int set_key(entry_t *e, char *kv, char *err, size_t errlen) {
  char *colon = strchr(kv, ':'), *key = kv, *val;
  if (!colon)
    return fail(err, errlen, "expected 'key: value'");
  *colon = '\0';
  val = colon + 1;
  while (isspace((unsigned char)*val))
    val++;
  clean(key);
  val = unquote(val);
  if (!*val || strlen(val) >= VAL_MAX_LEN)
    return fail(err, errlen, "empty or too long value");
  if (!strcmp(key, "prefix")) {
    snprintf(e->prefix, sizeof(e->prefix), "%s", val);
    e->have_prefix = 1;
  } else if (!strcmp(key, "class")) {
    snprintf(e->cls, sizeof(e->cls), "%s", val);
  } else {
    return fail(err, errlen, "unknown key (want prefix/class)");
  }
  return 0;
}

/* Handle "{ key: value, key: value }" (flow mapping); returns 0 ok. */
static int set_flow(entry_t *e, char *s, char *err, size_t errlen) {
  size_t n = strlen(s);
  char *p, *start, q = 0;

  if (n < 2 || s[n - 1] != '}')
    return fail(err, errlen, "unterminated '{'");
  s[n - 1] = '\0';
  start = s + 1;
  for (p = start;; p++) {
    if (q) {
      if (*p == q)
        q = 0;
    } else if (*p == '"' || *p == '\'') {
      q = *p;
    }
    if ((!q && *p == ',') || *p == '\0') {
      int last = *p == '\0';
      char *e2;
      *p = '\0';
      while (isspace((unsigned char)*start))
        start++;
      for (e2 = start + strlen(start);
           e2 > start && isspace((unsigned char)e2[-1]);)
        *--e2 = '\0';
      if (*start && set_key(e, start, err, errlen))
        return 1;
      if (last)
        break;
      start = p + 1;
    }
  }
  return 0;
}

static int flush(entry_t *e, yaml_entry_cb cb, void *ud, char *err,
                 size_t errlen) {
  if (e->line == 0)
    return 0;
  if (!e->have_prefix)
    return fail(err, errlen, "entry without prefix");
  return cb(ud, e->prefix, e->cls[0] ? e->cls : "default", err, errlen);
}

int yaml_parse_prefixes(const char *text, yaml_entry_cb cb, void *ud, char *err,
                        size_t errlen) {
  entry_t e;
  int lineno = 0;
  const char *p = text;

  memset(&e, 0, sizeof(e));
  while (*p) {
    char line[LINE_MAX_LEN], *s;
    size_t len = strcspn(p, "\n");
    lineno++;
    if (len >= sizeof(line)) {
      fail(err, errlen, "line too long");
      return lineno;
    }
    memcpy(line, p, len);
    line[len] = '\0';
    p += len + (p[len] ? 1 : 0);

    s = clean(line);
    if (*s == '\0' || !strcmp(s, "---"))
      continue;
    if (isspace((unsigned char)line[0]) == 0 && line[0] != '-') {
      fail(err, errlen, "expected list item '- key: value'");
      return lineno;
    }
    if (line[0] == '-') {
      s++;
      while (isspace((unsigned char)*s))
        s++;
      if (flush(&e, cb, ud, err, errlen))
        return e.line;
      memset(&e, 0, sizeof(e));
      e.line = lineno;
    } else {
      while (isspace((unsigned char)*s))
        s++;
      if (e.line == 0) {
        fail(err, errlen, "key outside list item");
        return lineno;
      }
    }
    if (line[0] == '-' && *s == '{' ? set_flow(&e, s, err, errlen)
                                    : set_key(&e, s, err, errlen))
      return lineno;
  }
  if (flush(&e, cb, ud, err, errlen))
    return e.line;
  return 0;
}
