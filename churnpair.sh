#!/bin/bash
# Like churnseq.sh, but run a DIFFERENT churn mode on each of two CPUs.
#
# usage: churnpair.sh <seconds-per-pair> <modeCPU0:modeCPU1>...
#   e.g. churnpair.sh 60 clonenosig:syscall clonenosig:fault clonenosig:thread
#
# Uses CPU 0 and CPU 1 (override with CPU_A and CPU_B). Same heartbeat file,
# CHURN and LOG variables as churnseq.sh.
dir=$(cd "$(dirname "$0")" && pwd)
churn=${CHURN:-$dir/churn}
log=${LOG:-$dir/churn-heartbeat.log}
cpu_a=${CPU_A:-0}
cpu_b=${CPU_B:-1}

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

for pair in "$@"; do
	m0=${pair%%:*}
	m1=${pair##*:}
	"$churn" "$cpu_a" "$m0" & p0=$!
	"$churn" "$cpu_b" "$m1" & p1=$!
	pids=($p0 $p1)
	end=$((SECONDS + dur))
	while [ $SECONDS -lt $end ]; do
		beat "pair=$pair t=$SECONDS"
		sleep 5
	done
	kill "${pids[@]}" 2>/dev/null
	wait "${pids[@]}" 2>/dev/null
done
beat "sequence complete"
