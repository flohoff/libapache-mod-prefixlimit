#ifndef YAML_H
#define YAML_H

#include <stddef.h>

/* Called per entry; return 0 ok, else fill err and return nonzero. */
typedef int (*yaml_entry_cb)(void *ud, const char *prefix, const char *cls,
                             char *err, size_t errlen);

/*
 * Parse a list of maps with scalar keys "prefix" and "class".
 * Returns 0 ok, else the error line (>0) with message in err.
 */
int yaml_parse_prefixes(const char *text, yaml_entry_cb cb, void *ud, char *err,
                        size_t errlen);

#endif
