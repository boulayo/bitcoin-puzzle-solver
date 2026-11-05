#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# The two user guides kept in step, and the links of every document: docs/it/USO.md must have the sections of docs/en/USAGE.md (same
# numbering, same count, same order; the titles may differ), every option (--something) and every environment variable (PZ_*) that one
# guide mentions must be in the other, and every relative link of every .md file must point to a file that exists. Each difference is
# printed; the exit status is 1 when there is any. POSIX sh, awk and grep only.
# Usage: tools/check_docs.sh      (from anywhere: it works in the repository root)
cd "$(dirname "$0")/.." || exit 2
EN=docs/en/USAGE.md IT=docs/it/USO.md
bad=0
fail() { echo "  FAIL $*"; bad=$((bad + 1)); }

# headings outside fenced code blocks, as "level number" ("2 5", "3 7.10", "1 -" for a heading without a number)
headings() {
    awk '/^```/ { fence = !fence; next }
         !fence && /^#+ / { level = length($1); n = $2; if (n !~ /^[0-9]+(\.[0-9]+)*\.?$/) n = "-"; sub(/\.$/, "", n); print level, n }' "$1"
}
[ -f "$EN" ] && [ -f "$IT" ] || { echo "  FAIL $EN or $IT is missing"; exit 1; }
headings "$EN" > /tmp/check_docs_en.$$
headings "$IT" > /tmp/check_docs_it.$$
NE=$(wc -l < /tmp/check_docs_en.$$ | tr -d ' '); NI=$(wc -l < /tmp/check_docs_it.$$ | tr -d ' ')
if [ "$NE" != "$NI" ]; then fail "$EN has $NE headings, $IT has $NI"; fi
# the headings one guide has and the other has not (by level and number, as many times as they occur), then, when the sets agree, the
# first place where the order differs
awk -v en="$EN" -v it="$IT" 'NR == FNR { a[NR] = $0; na = NR; ca[$0]++; next }
     { b[FNR] = $0; nb = FNR; cb[$0]++ }
     END { for (k in ca) if (ca[k] > cb[k]) { printf "  FAIL heading \"%s\" (level number) is in %s %d time(s) and in %s %d\n", k, en, ca[k], it, cb[k]; d++ }
           for (k in cb) if (cb[k] > ca[k]) { printf "  FAIL heading \"%s\" (level number) is in %s %d time(s) and in %s %d\n", k, it, cb[k], en, ca[k]; d++ }
           if (!d) for (i = 1; i <= na; i++) if (a[i] != b[i]) { printf "  FAIL the order differs from heading %d: %s has \"%s\", %s has \"%s\" (level number)\n", i, en, a[i], it, b[i]; d++; break }
           exit d > 0 }' /tmp/check_docs_en.$$ /tmp/check_docs_it.$$ || bad=$((bad + 1))
rm -f /tmp/check_docs_en.$$ /tmp/check_docs_it.$$
[ $bad = 0 ] && echo "  OK   $EN and $IT: $NE headings, same numbering and order"

# the options and the environment variables: each one of a guide must appear in the other
mentions() { grep -o -e "$1" "$2" | sort -u; }
for what in "option --[a-z][a-z0-9-]*" "environment variable PZ_[A-Z0-9_]*"; do
    pat=${what##* }; name=${what% *}
    mentions "$pat" "$EN" > /tmp/check_docs_en.$$
    mentions "$pat" "$IT" > /tmp/check_docs_it.$$
    n=$bad
    for x in $(cat /tmp/check_docs_en.$$); do grep -q -F -x -- "$x" /tmp/check_docs_it.$$ || fail "$name $x is in $EN but not in $IT"; done
    for x in $(cat /tmp/check_docs_it.$$); do grep -q -F -x -- "$x" /tmp/check_docs_en.$$ || fail "$name $x is in $IT but not in $EN"; done
    [ $n = $bad ] && echo "  OK   every $name of one guide is in the other ($(wc -l < /tmp/check_docs_en.$$ | tr -d ' ') in $EN)"
    rm -f /tmp/check_docs_en.$$ /tmp/check_docs_it.$$
done

# every relative link ("[text](path)" and "![alt](path)", not http(s):, mailto: or #fragment) of every .md file resolves from the file's folder
n=$bad; nl=0
for f in $(find . -name '*.md' -not -path './build/*' -not -path './build_*' -not -path './dist-release/*' -not -path './.git/*' | sort); do
    for link in $(grep -o '\]([^)]*)' "$f" | sed 's/^](//; s/)$//; s/ .*//' | grep -v -E '^(https?:|mailto:|#)' | sed 's/#.*//' | grep -v '^$' | sort -u); do
        nl=$((nl + 1))
        case $link in /*) target=$link ;; *) target=$(dirname "$f")/$link ;; esac
        [ -e "$target" ] || fail "$f links to $link, which does not exist"
    done
done
[ $n = $bad ] && echo "  OK   every relative link of the .md files resolves ($nl links)"

[ $bad = 0 ] && echo "check_docs: OK" || echo "check_docs: $bad differences"
exit $((bad > 0))
