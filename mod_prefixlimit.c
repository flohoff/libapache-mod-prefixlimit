/* mod_prefixlimit: classify clients by CIDR list, rate limit by class. */
// clang-format off
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_mutex.h"
// clang-format on

#include "apr_file_io.h"
#include "apr_global_mutex.h"
#include "apr_shm.h"
#include "apr_strings.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "trie.h"
#include "yaml.h"

#define MUTEX_TYPE "prefixlimit-shm"
#define DEFAULT_STATUS 429
#define HANDLER "prefixlimit-reject"
#define NOTE_RETRY "prefixlimit-retry"
#define DEFAULT_CTYPE "text/plain; charset=utf-8"

module AP_MODULE_DECLARE_DATA prefixlimit_module;

typedef struct {
  const char *cls;
  int neg;     /* rule applies to clients NOT in cls */
  int id;      /* bucket index */
  double rate; /* tokens per second */
  double burst;
} rule_t;

typedef struct {
  trie_t *trie;
  apr_array_header_t *classes; /* const char * */
  apr_array_header_t *counts;  /* int, per class */
  const char *path;
  int status; /* 0 = unset */
} srv_cfg;

typedef struct {
  apr_array_header_t *rules; /* rule_t */
  int retry;                 /* Retry-After: -1 unset, 0 auto, else seconds */
  const char *message;       /* body for the limited response, or NULL */
  const char *ctype;
} dir_cfg;

typedef struct {
  double tokens;
  apr_time_t last;
  int init;
} bucket_t;

static int nrules;
static apr_shm_t *shm;
static bucket_t *buckets;
static apr_global_mutex_t *mutex;

static void *create_srv(apr_pool_t *p, server_rec *s) {
  (void)s;
  return apr_pcalloc(p, sizeof(srv_cfg));
}

static void *merge_srv(apr_pool_t *p, void *b, void *a) {
  srv_cfg *base = b, *add = a, *c = apr_pcalloc(p, sizeof(*c));
  c->trie = add->trie ? add->trie : base->trie;
  c->classes = add->trie ? add->classes : base->classes;
  c->counts = add->trie ? add->counts : base->counts;
  c->path = add->trie ? add->path : base->path;
  c->status = add->status ? add->status : base->status;
  return c;
}

static void *create_dir(apr_pool_t *p, char *path) {
  dir_cfg *c = apr_pcalloc(p, sizeof(*c));
  (void)path;
  c->retry = -1;
  return c;
}

static void *merge_dir(apr_pool_t *p, void *b, void *a) {
  dir_cfg *base = b, *add = a, *c = apr_pcalloc(p, sizeof(*c));
  int i, j;
  c->rules = apr_array_make(p, 2, sizeof(rule_t));
  c->retry = add->retry >= 0 ? add->retry : base->retry;
  c->message = add->message ? add->message : base->message;
  c->ctype = add->message ? add->ctype : base->ctype;
  if (add->rules)
    for (i = 0; i < add->rules->nelts; i++)
      *(rule_t *)apr_array_push(c->rules) = ((rule_t *)add->rules->elts)[i];
  if (base->rules)
    for (i = 0; i < base->rules->nelts; i++) {
      rule_t *r = &((rule_t *)base->rules->elts)[i];
      int dup = 0;
      for (j = 0; add->rules && j < add->rules->nelts; j++)
        if (r->neg == ((rule_t *)add->rules->elts)[j].neg &&
            !strcmp(r->cls, ((rule_t *)add->rules->elts)[j].cls))
          dup = 1;
      if (!dup)
        *(rule_t *)apr_array_push(c->rules) = *r;
    }
  return c;
}

/* ---- PrefixListFile ---- */

typedef struct {
  srv_cfg *cfg;
  apr_pool_t *pool;
  int n;
} load_ctx;

static int class_id(load_ctx *c, const char *name) {
  int i;
  for (i = 0; i < c->cfg->classes->nelts; i++)
    if (!strcmp(((const char **)c->cfg->classes->elts)[i], name))
      return i;
  *(const char **)apr_array_push(c->cfg->classes) = apr_pstrdup(c->pool, name);
  *(int *)apr_array_push(c->cfg->counts) = 0;
  return i;
}

static int on_entry(void *ud, const char *prefix, const char *cls, char *err,
                    size_t errlen) {
  load_ctx *c = ud;
  unsigned char addr[16];
  int bits;
  if (trie_parse_cidr(prefix, addr, &bits)) {
    apr_snprintf(err, errlen, "bad prefix '%s'", prefix);
    return 1;
  }
  int id = class_id(c, cls);
  if (trie_insert(c->cfg->trie, addr, bits, id)) {
    apr_snprintf(err, errlen, "out of memory");
    return 1;
  }
  ((int *)c->cfg->counts->elts)[id]++;
  c->n++;
  return 0;
}

static apr_status_t trie_cleanup(void *t) {
  trie_free(t);
  return APR_SUCCESS;
}

/* Line n (1-based) of buf, truncated, for error messages. */
static const char *nth_line(apr_pool_t *p, const char *buf, int n) {
  size_t len;
  while (--n > 0 && (buf = strchr(buf, '\n')))
    buf++;
  if (!buf)
    return "";
  len = strcspn(buf, "\r\n");
  return apr_pstrndup(p, buf, len > 100 ? 100 : len);
}

static const char *cmd_list(cmd_parms *cmd, void *dummy, const char *arg) {
  srv_cfg *cfg =
      ap_get_module_config(cmd->server->module_config, &prefixlimit_module);
  apr_pool_t *p = cmd->pool;
  const char *path = ap_server_root_relative(p, arg);
  apr_file_t *f;
  apr_finfo_t fi;
  apr_size_t len;
  apr_status_t rv;
  char *buf, err[256], msg[128];
  load_ctx ctx;
  int line;
  (void)dummy;

  if (!path)
    return apr_psprintf(p, "bad path %s", arg);
  if ((rv = apr_file_open(&f, path, APR_READ, APR_OS_DEFAULT, p))) {
    ap_log_perror(APLOG_MARK, APLOG_ERR | APLOG_STARTUP, rv, p,
                  "prefixlimit: cannot open prefix list %s", path);
    return apr_psprintf(p, "cannot open %s: %s", path,
                        apr_strerror(rv, msg, sizeof(msg)));
  }
  if ((rv = apr_file_info_get(&fi, APR_FINFO_SIZE, f))) {
    apr_file_close(f);
    ap_log_perror(APLOG_MARK, APLOG_ERR | APLOG_STARTUP, rv, p,
                  "prefixlimit: cannot stat prefix list %s", path);
    return apr_psprintf(p, "cannot stat %s", path);
  }
  len = (apr_size_t)fi.size;
  buf = apr_pcalloc(p, len + 1);
  if ((rv = apr_file_read_full(f, buf, len, NULL))) {
    apr_file_close(f);
    ap_log_perror(APLOG_MARK, APLOG_ERR | APLOG_STARTUP, rv, p,
                  "prefixlimit: cannot read prefix list %s", path);
    return apr_psprintf(p, "cannot read %s", path);
  }
  apr_file_close(f);

  cfg->trie = trie_new();
  if (!cfg->trie)
    return "out of memory";
  apr_pool_cleanup_register(p, cfg->trie, trie_cleanup, apr_pool_cleanup_null);
  cfg->classes = apr_array_make(p, 4, sizeof(const char *));
  cfg->counts = apr_array_make(p, 4, sizeof(int));
  cfg->path = path;
  ctx.cfg = cfg;
  ctx.pool = p;
  ctx.n = 0;
  line = yaml_parse_prefixes(buf, on_entry, &ctx, err, sizeof(err));
  if (line) {
    ap_log_perror(APLOG_MARK, APLOG_ERR | APLOG_STARTUP, 0, p,
                  "prefixlimit: parsing %s failed at line %d: %s ['%s'] "
                  "(%d prefixes read before the error)",
                  path, line, err, nth_line(p, buf, line), ctx.n);
    return apr_psprintf(p, "%s:%d: %s", path, line, err);
  }
  return NULL;
}

/* ---- PrefixRateLimit <class> <rate>/<s|m> [burst=N] ---- */

static const char *cmd_rate(cmd_parms *cmd, void *mconfig, const char *cls,
                            const char *rate, const char *opt) {
  dir_cfg *dc = mconfig;
  rule_t *r;
  char *end;
  double n = strtod(rate, &end), per = 1;
  int neg = 0;

  if (end == rate || n <= 0 || *end != '/' ||
      (end[1] != 's' && end[1] != 'm') || end[2])
    return "rate must be <number>/s or <number>/m";
  if (end[1] == 'm')
    per = 60;
  if (cls[0] == '!') {
    neg = 1;
    cls++;
  }
  if (!*cls || *cls == '!')
    return "class name must not be empty or start with '!'";
  if (!dc->rules)
    dc->rules = apr_array_make(cmd->pool, 2, sizeof(rule_t));
  r = apr_array_push(dc->rules);
  r->cls = apr_pstrdup(cmd->pool, cls);
  r->neg = neg;
  r->id = nrules++;
  r->rate = n / per;
  r->burst = r->rate < 1 ? 1 : ceil(r->rate);
  if (opt) {
    double b;
    if (strncmp(opt, "burst=", 6) || (b = strtod(opt + 6, &end)) < 1 || *end)
      return "third argument must be burst=N (N >= 1)";
    r->burst = b;
  }
  return NULL;
}

static const char *cmd_status(cmd_parms *cmd, void *dummy, const char *arg) {
  srv_cfg *cfg =
      ap_get_module_config(cmd->server->module_config, &prefixlimit_module);
  int v = atoi(arg);
  (void)dummy;
  if (v < 400 || v > 599)
    return "status must be 400-599";
  cfg->status = v;
  return NULL;
}

static const char *cmd_retry(cmd_parms *cmd, void *mconfig, const char *arg) {
  dir_cfg *dc = mconfig;
  char *end;
  long v;
  (void)cmd;
  if (!strcasecmp(arg, "auto")) {
    dc->retry = 0;
    return NULL;
  }
  v = strtol(arg, &end, 10);
  if (end == arg || *end || v < 1 || v > 86400)
    return "seconds (1-86400) or auto";
  dc->retry = (int)v;
  return NULL;
}

static const char *cmd_message(cmd_parms *cmd, void *mconfig, const char *text,
                               const char *ctype) {
  dir_cfg *dc = mconfig;
  (void)cmd;
  if (!*text)
    return "message must not be empty";
  dc->message = apr_pstrdup(cmd->pool, text);
  dc->ctype = ctype ? apr_pstrdup(cmd->pool, ctype) : DEFAULT_CTYPE;
  return NULL;
}

static const command_rec cmds[] = {
    AP_INIT_TAKE1("PrefixListFile", cmd_list, NULL, RSRC_CONF,
                  "YAML file with prefix/class entries"),
    AP_INIT_TAKE23("PrefixRateLimit", cmd_rate, NULL, RSRC_CONF | ACCESS_CONF,
                   "<class> <rate>/<s|m> [burst=N]"),
    AP_INIT_TAKE1("PrefixRateLimitStatus", cmd_status, NULL, RSRC_CONF,
                  "status code when limited (default 429)"),
    AP_INIT_TAKE1("PrefixRateLimitRetryAfter", cmd_retry, NULL,
                  RSRC_CONF | ACCESS_CONF,
                  "Retry-After seconds when limited, or auto (default)"),
    AP_INIT_TAKE12("PrefixRateLimitMessage", cmd_message, NULL,
                   RSRC_CONF | ACCESS_CONF,
                   "<text> [content-type] body sent with the limited response"),
    {NULL}};

/* ---- shared state ---- */

static int pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp) {
  (void)plog;
  (void)ptemp;
  nrules = 0;
  ap_mutex_register(pconf, MUTEX_TYPE, NULL, APR_LOCK_DEFAULT, 0);
  return OK;
}

static apr_status_t shm_cleanup(void *unused) {
  (void)unused;
  if (shm)
    apr_shm_destroy(shm);
  shm = NULL;
  buckets = NULL;
  return APR_SUCCESS;
}

/* Log prefix counts per class (once per start/reload). */
static void log_lists(server_rec *s) {
  srv_cfg *base = ap_get_module_config(s->module_config, &prefixlimit_module);
  server_rec *sp;
  int i;

  for (sp = s; sp; sp = sp->next) {
    srv_cfg *c = ap_get_module_config(sp->module_config, &prefixlimit_module);
    int total = 0;
    if (!c->trie || (sp != s && c->trie == base->trie))
      continue;
    for (i = 0; i < c->classes->nelts; i++)
      total += ((int *)c->counts->elts)[i];
    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, sp,
                 "prefixlimit: %s: loaded %d prefixes in %d classes from %s",
                 sp->server_hostname ? sp->server_hostname : "default", total,
                 c->classes->nelts, c->path);
    for (i = 0; i < c->classes->nelts; i++)
      ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, sp,
                   "prefixlimit: %s: class %s: %d prefixes",
                   sp->server_hostname ? sp->server_hostname : "default",
                   ((const char **)c->classes->elts)[i],
                   ((int *)c->counts->elts)[i]);
  }
}

static int post_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp,
                       server_rec *s) {
  apr_status_t rv;
  void *first;
  (void)plog;
  (void)ptemp;
  /* config runs twice at startup; log on the second pass and on reloads */
  apr_pool_userdata_get(&first, "prefixlimit_post_config", s->process->pool);
  if (first)
    log_lists(s);
  else
    apr_pool_userdata_set((void *)1, "prefixlimit_post_config",
                          apr_pool_cleanup_null, s->process->pool);
  shm = NULL;
  buckets = NULL;
  mutex = NULL;
  if (nrules == 0)
    return OK;

  rv = apr_shm_create(&shm, sizeof(bucket_t) * nrules, NULL, pconf);
  if (rv != APR_SUCCESS) {
    ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s, "prefixlimit: shm create");
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  apr_pool_cleanup_register(pconf, NULL, shm_cleanup, apr_pool_cleanup_null);
  buckets = apr_shm_baseaddr_get(shm);
  memset(buckets, 0, sizeof(bucket_t) * nrules);

  rv = ap_global_mutex_create(&mutex, NULL, MUTEX_TYPE, NULL, s, pconf, 0);
  if (rv != APR_SUCCESS)
    return HTTP_INTERNAL_SERVER_ERROR;
  return OK;
}

static void child_init(apr_pool_t *p, server_rec *s) {
  apr_status_t rv;
  if (!mutex)
    return;
  rv = apr_global_mutex_child_init(&mutex, apr_global_mutex_lockfile(mutex), p);
  if (rv != APR_SUCCESS)
    ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s, "prefixlimit: mutex child");
}

/* ---- request ---- */

/* Take a token; returns 0 ok, else seconds to wait (>=1). */
static int take(const rule_t *r) {
  bucket_t *b = &buckets[r->id];
  apr_time_t now = apr_time_now();
  double wait = 0;

  apr_global_mutex_lock(mutex);
  if (!b->init) {
    b->tokens = r->burst;
    b->init = 1;
  } else {
    double t = b->tokens + (double)(now - b->last) / 1e6 * r->rate;
    b->tokens = t > r->burst ? r->burst : t;
  }
  b->last = now;
  if (b->tokens >= 1)
    b->tokens -= 1;
  else
    wait = (1 - b->tokens) / r->rate;
  apr_global_mutex_unlock(mutex);

  return wait > 0 ? (int)ceil(wait) : 0;
}

static int check_access(request_rec *r) {
  srv_cfg *sc =
      ap_get_module_config(r->server->module_config, &prefixlimit_module);
  dir_cfg *dc = ap_get_module_config(r->per_dir_config, &prefixlimit_module);
  apr_sockaddr_t *sa = r->useragent_addr;
  unsigned char a[16];
  const char *cls;
  int id, i;

  if (!ap_is_initial_req(r) || !sc->trie || !sa)
    return DECLINED;
  if (sa->family == AF_INET) {
    memset(a, 0, 10);
    a[10] = a[11] = 0xff;
    memcpy(a + 12, &sa->sa.sin.sin_addr, 4);
  } else if (sa->family == AF_INET6) {
    memcpy(a, &sa->sa.sin6.sin6_addr, 16);
  } else {
    return DECLINED;
  }
  id = trie_lookup(sc->trie, a);
  cls = id < 0 ? NULL : ((const char **)sc->classes->elts)[id];
  if (cls)
    apr_table_setn(r->subprocess_env, "PREFIX_CLASS", cls);

  if (!dc->rules || !buckets)
    return DECLINED;
  for (i = 0; i < dc->rules->nelts; i++) {
    const rule_t *rule = &((rule_t *)dc->rules->elts)[i];
    int wait, match = cls && !strcmp(rule->cls, cls);
    if (match == rule->neg)
      continue;
    wait = take(rule);
    if (wait) {
      const char *retry = apr_itoa(r->pool, dc->retry > 0 ? dc->retry : wait);
      ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                    "prefixlimit: rule %s%s over limit (client class %s)",
                    rule->neg ? "!" : "", rule->cls, cls ? cls : "none");
      if (dc->message) {
        /* answered by reject_handler, set in fixups */
        apr_table_setn(r->notes, NOTE_RETRY, retry);
        return DECLINED;
      }
      apr_table_setn(r->err_headers_out, "Retry-After", retry);
      return sc->status ? sc->status : DEFAULT_STATUS;
    }
  }
  return DECLINED;
}

/* Route limited requests to reject_handler, whatever other modules chose. */
static int fixups(request_rec *r) {
  if (apr_table_get(r->notes, NOTE_RETRY))
    r->handler = HANDLER;
  return DECLINED;
}

static int reject_handler(request_rec *r) {
  srv_cfg *sc =
      ap_get_module_config(r->server->module_config, &prefixlimit_module);
  dir_cfg *dc = ap_get_module_config(r->per_dir_config, &prefixlimit_module);
  const char *retry = apr_table_get(r->notes, NOTE_RETRY);
  const char *body;

  if (!r->handler || strcmp(r->handler, HANDLER) || !retry || !dc->message)
    return DECLINED;
  body = apr_pstrcat(r->pool, dc->message, "\n", NULL);
  r->status = sc->status ? sc->status : DEFAULT_STATUS;
  ap_set_content_type(r, dc->ctype ? dc->ctype : DEFAULT_CTYPE);
  apr_table_setn(r->headers_out, "Retry-After", retry);
  ap_set_content_length(r, (apr_off_t)strlen(body));
  if (!r->header_only)
    ap_rputs(body, r);
  return OK;
}

static void register_hooks(apr_pool_t *p) {
  (void)p;
  ap_hook_pre_config(pre_config, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_post_config(post_config, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_child_init(child_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_access_checker(check_access, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_fixups(fixups, NULL, NULL, APR_HOOK_REALLY_LAST);
  ap_hook_handler(reject_handler, NULL, NULL, APR_HOOK_REALLY_FIRST);
}

AP_DECLARE_MODULE(prefixlimit) = {
    STANDARD20_MODULE_STUFF, create_dir, merge_dir, create_srv, merge_srv, cmds,
    register_hooks};
