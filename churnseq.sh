#!/bin/bash
# Run one churn mode at a time, one pinned copy per CPU, for a fixed time each.
#
# usage: churnseq.sh <seconds-per-mode> <mode>...
#   e.g. churnseq.sh 300 clonenosig
#
# A heartbeat line goes to $LOG (and the system log) every 5 seconds and is
# synced to disk, so after a wedge and reboot `tail churn-heartbeat.log` shows
# the mode and the time of the last beat, to within about 5 seconds.
#
# Environment:
#   CPUS   CPUs to pin one copy to each (default "0 1")
#   CHURN  path to the churn binary (default: next to this script)
#   LOG    heartbeat file (default: churn-heartbeat.log next to this script)
dir=$(cd "$(dirname "$0")" && pwd)
churn=${CHURN:-$dir/churn}
cpus=${CPUS:-"0 1"}
log=${LOG:-$dir/churn-heartbeat.log}

[ -x "$churn" ] || { echo "build first: make" >&2; exit 1; }
[ $# -ge 2 ] || { sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }

dur=$1; shift
pids=()
trap 'kill "${pids[@]}" 2>/dev/null; exit 130' INT TERM

beat() {
	echo "$(date -u +%FT%TZ) $*" >> "$log"
	sync
	logger -t churnseq "$*" 2>/dev/null || true
}

for mode in "$@"; do
	pids=()
	for c in $cpus; do
		"$churn" "$c" "$mode" & pids+=($!)
	done
	end=$((SECONDS + dur))
	while [ $SECONDS -lt $end ]; do
		beat "mode=$mode t=$SECONDS"
		sleep 5
	done
	kill "${pids[@]}" 2>/dev/null
	wait "${pids[@]}" 2>/dev/null
done
beat "sequence complete"
