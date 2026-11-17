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
mkdir -p "$ROOT"/{logs,run,htdocs/limited,htdocs/open}
echo hi > "$ROOT/htdocs/limited/x.txt"
echo hi > "$ROOT/htdocs/open/x.txt"
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
