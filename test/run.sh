#!/bin/bash
# Local httpd test; runs in /scratch/local/claude/httpd. Needs `make module`.
set -u
SRC=$(cd "$(dirname "$0")/.." && pwd)
ROOT=/scratch/local/claude/httpd
HTTPD=/usr/sbin/apache2
URL=http://127.0.0.1:8080
fails=0

ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
codes() { # n curl-args...
  local n=$1; shift
  for _ in $(seq "$n"); do code "$@"; echo; done | sort | uniq -c | perl -0pe 's/^\s+//; s/\s+/ /g; s/ $//'
}

rm -rf "$ROOT"
mkdir -p "$ROOT"/{logs,run,htdocs/limited,htdocs/open,htdocs/msg,htdocs/fixed,htdocs/custom,htdocs/neg}
echo hi > "$ROOT/htdocs/limited/x.txt"
echo hi > "$ROOT/htdocs/open/x.txt"
for d in msg fixed custom neg; do echo hi > "$ROOT/htdocs/$d/x.txt"; done
cat > "$ROOT/prefixes-test.yaml" <<EOF
- prefix: 127.0.0.0/8
  class: testcls
- prefix: ::1/128
  class: testcls
- prefix: 1.92.0.0/16
  class: aicrawler
EOF
perl -pe "s#\@ROOT\@#$ROOT#g; s#\@SRC\@#$SRC#g" "$SRC/test/httpd-test.conf" > "$ROOT/httpd.conf"

$HTTPD -f "$ROOT/httpd.conf" -t || { echo "config test failed"; exit 1; }
$HTTPD -f "$ROOT/httpd.conf" -k start || exit 1
trap 'kill $(cat "$ROOT/run/httpd.pid") 2>/dev/null' EXIT
sleep 1

# burst=2 at 2/s: first requests ok, rest 429
out=$(codes 10 $URL/limited/x.txt)
echo "     10 rapid: $out"
n429=$(echo "$out" | perl -ne 'print $1 if /(\d+) 429/')
[ "${n429:-0}" -ge 6 ] && ok "rapid requests limited" || bad "rapid requests limited ($out)"
ra=$(curl -s -D- -o /dev/null $URL/limited/x.txt | perl -ne 'print $1 if /^Retry-After: (\d+)/i')
[ "${ra:-0}" -ge 1 ] && ok "Retry-After=$ra" || bad "Retry-After missing"
sleep 1.2
check "refill after 1.2s" "$(code $URL/limited/x.txt)" 200

# no rule on /open/
check "open never limited" "$(codes 20 $URL/open/x.txt)" "20 200"

# unlisted client via X-Forwarded-For
check "unlisted not limited" "$(codes 20 -H 'X-Forwarded-For: 8.8.8.8' $URL/limited/x.txt)" "20 200"

# env/class header
check "class testcls" "$(curl -sI $URL/open/x.txt | perl -ne 'print $1 if /^X-Class: (\S+)/i')" testcls
check "class aicrawler" "$(curl -sI -H 'X-Forwarded-For: 1.92.5.5' $URL/open/x.txt | perl -ne 'print $1 if /^X-Class: (\S+)/i')" aicrawler
check "no class unlisted" "$(curl -sI -H 'X-Forwarded-For: 8.8.8.8' $URL/open/x.txt | perl -ne 'print $1 if /^X-Class: (\S+)/i')" ""

# separate bucket per class: aicrawler 1/m burst=1
out=$(codes 4 -H 'X-Forwarded-For: 1.92.5.5' $URL/limited/x.txt)
check "aicrawler 1 ok then 429" "$out" "1 200 3 429"

# message body + configurable Retry-After
check "msg first ok" "$(code $URL/msg/x.txt)" 200
resp=$(curl -s -D- $URL/msg/x.txt)
check "msg status" "$(echo "$resp" | perl -ne 'print $1 if /^HTTP\S* (\d+)/')" 429
check "msg content-type" "$(echo "$resp" | perl -ne 'print $1 if /^Content-Type: (.*?)\r?$/i')" "text/plain; charset=utf-8"
check "msg Retry-After fixed" "$(echo "$resp" | perl -ne 'print $1 if /^Retry-After: (\d+)/i')" 30
check "msg body" "$(echo "$resp" | perl -0ne 'print $1 if /\r?\n\r?\n(.*?)\s*$/s')" "Slow down, try later"
check "msg filters ran (X-Class)" "$(echo "$resp" | perl -ne 'print $1 if /^X-Class: (\S+)/i')" testcls
check "msg HEAD status" "$(curl -sI -o /dev/null -w '%{http_code}' $URL/msg/x.txt)" 429
check "msg other loc unaffected" "$(code $URL/open/x.txt)" 200

check "fixed first ok" "$(code $URL/fixed/x.txt)" 200
resp=$(curl -s -D- $URL/fixed/x.txt)
check "fixed status" "$(echo "$resp" | perl -ne 'print $1 if /^HTTP\S* (\d+)/')" 429
check "fixed Retry-After" "$(echo "$resp" | perl -ne 'print $1 if /^Retry-After: (\d+)/i')" 17
echo "$resp" | perl -0ne 'exit(/text\/html/i ? 0 : 1)' && ok "fixed default error page (html)" || bad "fixed default error page"

check "custom first ok" "$(code $URL/custom/x.txt)" 200
resp=$(curl -s -D- $URL/custom/x.txt)
check "custom content-type" "$(echo "$resp" | perl -ne 'print $1 if /^Content-Type: (.*?)\r?$/i')" "application/json"
check "custom body" "$(echo "$resp" | perl -0ne 'print $1 if /\r?\n\r?\n(.*?)\s*$/s')" '{"error":"slow down"}'

# negated class: !aicrawler = everyone else, incl. unlisted
check "neg: aicrawler exempt" "$(codes 10 -H 'X-Forwarded-For: 1.92.5.5' $URL/neg/x.txt)" "10 200"
sleep 1.2
check "neg: other class limited" "$(codes 4 $URL/neg/x.txt)" "1 200 3 429"
sleep 1.2
check "neg: unlisted limited" "$(codes 4 -H 'X-Forwarded-For: 8.8.8.8' $URL/neg/x.txt)" "1 200 3 429"
sleep 1.2

# ipv6
sleep 1.2
out=$(codes 8 -g 'http://[::1]:8080/limited/x.txt')
echo "     ipv6: $out"
echo "$out" | perl -ne 'exit(/429/ ? 0 : 1)' && ok "ipv6 limited" || bad "ipv6 limited ($out)"

# shared across worker processes: 100 reqs, 20 concurrent
sleep 3
ab -q -n 100 -c 20 $URL/limited/x.txt > "$ROOT/ab.out" 2>&1
non2=$(perl -ne 'print $1 if /Non-2xx responses:\s+(\d+)/' "$ROOT/ab.out")
n200=$((100 - ${non2:-0}))
echo "     ab: $n200 x 200 of 100"
[ "$n200" -le 6 ] && [ "$n200" -ge 1 ] && ok "budget shared across workers" || bad "budget shared ($n200 x 200)"

# graceful reload picks up new prefixes
check "8.8.8.8 not limited before" "$(codes 6 -H 'X-Forwarded-For: 8.8.8.8' $URL/limited/x.txt)" "6 200"
printf -- '- prefix: 8.8.8.0/24\n  class: testcls\n' >> "$ROOT/prefixes-test.yaml"
$HTTPD -f "$ROOT/httpd.conf" -k graceful
sleep 2
out=$(codes 8 -H 'X-Forwarded-For: 8.8.8.8' $URL/limited/x.txt)
echo "$out" | perl -ne 'exit(/429/ ? 0 : 1)' && ok "reload picks up prefix" || bad "reload ($out)"

# bad yaml aborts config test
printf -- '- prefix: nonsense\n' > "$ROOT/prefixes-test.yaml"
msg=$($HTTPD -f "$ROOT/httpd.conf" -t 2>&1)
echo "$msg" | perl -0ne 'exit(/prefixes-test.yaml:1: bad prefix/ ? 0 : 1)' && ok "bad yaml rejected with line" || bad "bad yaml ($msg)"

echo
[ $fails -eq 0 ] && echo "ALL OK" || echo "$fails FAILED (see $ROOT/logs/error.log)"
exit $fails
