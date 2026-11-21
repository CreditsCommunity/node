#!/bin/sh
# Healthy while the last written block advances: fails when it has not changed for 10 minutes.
# Unhealthy while the node reports FORK HELD (fork-held.json with "held": "true"): it refused a
# competing chain and waits for the operator.
# Prints the latest PEERS summary and the official-nodes state from peer-status.json (information only:
# official nodes gone never makes the container unhealthy).
peers=/data/peer-status.json
if [ -f "$peers" ]; then
    sed -nE 's/.*"summary": "([^"]*)".*/\1/p' "$peers" | head -1
    if grep -q '"official_gone": "true"' "$peers"; then
        why=$(sed -nE 's/.*"official_gone_reason": "([^"]*)".*/\1/p' "$peers" | head -1)
        echo "OFFICIAL NODES GONE: $why; community nodes keep the network going (information, not a fault)"
    fi
    if grep -q '"community_only": "true"' "$peers"; then
        echo "TRUSTED SEATS COMMUNITY-ONLY: no official node among the trusted of the last rounds"
    fi
fi
fork=/data/fork-held.json
if [ -f "$fork" ] && grep -q '"held": "true"' "$fork"; then
    at=$(sed -nE 's/.*"fork_point": "([0-9]+)".*/\1/p' "$fork")
    since=$(sed -nE 's/.*"since": "([^"]+)".*/\1/p' "$fork")
    echo "FORK HELD since $since: the node refused a competing chain (fork point #$at) and keeps its own."
    echo "Check with other nodes or the monitor first. Our chain is right: docker compose exec node touch /data/dismiss-fork."
    echo "The other chain is right: docker compose stop && docker compose run --rm node resolve-fork && docker compose up -d"
    exit 1
fi
# the two newest log files, the older first: with the newest first, a rotated log made the last line
# come from the old file, a block of hours ago (unhealthy while the node was at the head)
seq=$(cat $(ls -tr /data/log/log_*.txt 2>/dev/null | tail -2) 2>/dev/null \
      | grep -a 'Last written sequence = ' | tail -1 | sed -E "s/.*Last written sequence = ([0-9']+).*/\1/" | tr -d "'")
[ -n "$seq" ] || exit 0  # still starting
state=/tmp/credits-health
now=$(date +%s)
if [ -f "$state" ] && [ "$(cut -d' ' -f1 "$state")" = "$seq" ]; then
    [ $((now - $(cut -d' ' -f2 "$state"))) -lt 600 ] || { echo "block $seq for 10 minutes"; exit 1; }
else
    echo "$seq $now" > "$state"
fi
exit 0
