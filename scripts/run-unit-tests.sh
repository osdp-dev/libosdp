#!/usr/bin/env bash
#
# Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
# SPDX-License-Identifier: Apache-2.0
#
# Parallel unit-test runner for the lean Makefile build -- the `make check`
# equivalent of `ctest -jN` for the CMake path. Each suite runs as its own
# process (so the harness's global mock state never contends across suites),
# with its output redirected to a per-suite log file; the terminal shows only a
# high-level pass/fail summary. Ctrl-C stops every suite and cleans up.
#
# Overridable via env: BIN (test binary), LOGDIR (log dir), JOBS (concurrency).
# OSDP_TEST_SEED passes through to every suite to replay a run.

set -u

BIN="${BIN:-build/unit-test}"
LOGDIR="${LOGDIR:-build/test-logs}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
# When set, each suite writes per-case JUnit XML to $JUNIT_DIR/<suite>.xml
# (for CI test reporting). Empty = no JUnit emitted.
JUNIT_DIR="${JUNIT_DIR:-}"

# Suites that assert real-time protocol timing (file-transfer completion
# windows, retransmit/link-loss budgets) are unreliable under heavy CPU
# contention -- run them alone, matching RUN_SERIAL in the CMake path.
SERIAL_SUITES="file_tx"

if [ ! -x "$BIN" ]; then
	echo "error: '$BIN' not found or not executable; run 'make unit-test' first" >&2
	exit 1
fi

mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/*.log "$LOGDIR"/*.status 2>/dev/null
if [ -n "$JUNIT_DIR" ]; then
	mkdir -p "$JUNIT_DIR"
	rm -f "$JUNIT_DIR"/*.xml 2>/dev/null
fi

# Ctrl-C / TERM: stop the suite processes (never the orchestrator itself).
# The harness treats SIGTERM/SIGINT cooperatively -- it unwinds at the next
# case boundary -- so a suite stuck in a long in-flight case would linger. Give
# it a brief grace window, then SIGKILL any survivor so Ctrl-C is prompt and
# leaves no orphans. Match by the binary's full path so only suites are hit.
cleanup() {
	trap - INT TERM
	echo >&2
	echo "Interrupted -- stopping suites..." >&2
	pkill -TERM -f "$BIN " 2>/dev/null
	sleep 1
	pkill -KILL -f "$BIN " 2>/dev/null
	wait 2>/dev/null
	exit 130
}
trap cleanup INT TERM

ALL_SUITES=$("$BIN" --list)

is_serial() {
	case " $SERIAL_SUITES " in
	*" $1 "*) return 0 ;;
	*) return 1 ;;
	esac
}

# Run one suite; record its exit code in a sidecar .status file so the parallel
# batch can be tallied after the fact, and print a one-line progress marker on
# completion (single-line writes stay atomic even from concurrent suites).
run_suite() {
	local s="$1"
	local extra=()
	[ -n "$JUNIT_DIR" ] && extra=(--junit "$JUNIT_DIR/$s.xml")
	if "$BIN" "${extra[@]}" "$s" >"$LOGDIR/$s.log" 2>&1; then
		echo 0 >"$LOGDIR/$s.status"
		printf '  [PASS] %s\n' "$s"
	else
		echo $? >"$LOGDIR/$s.status"
		printf '  [FAIL] %s (see %s/%s.log)\n' "$s" "$LOGDIR" "$s"
	fi
}

n_total=$(echo "$ALL_SUITES" | wc -w)
printf 'Running %s unit-test suites (-j%s), logs in %s ...\n' \
	"$n_total" "$JOBS" "$LOGDIR"

start=$(date +%s)

# Phase 1: timing-sensitive suites first, one at a time, while the CPU is quiet.
# Background each and `wait` for it (never foreground) so a Ctrl-C during a long
# serial suite fires the trap immediately instead of being deferred until the
# child returns.
for s in $ALL_SUITES; do
	if is_serial "$s"; then
		run_suite "$s" &
		wait $!
	fi
done

# Phase 2: the rest, capped at $JOBS concurrent processes.
running=0
for s in $ALL_SUITES; do
	is_serial "$s" && continue
	run_suite "$s" &
	running=$((running + 1))
	if [ "$running" -ge "$JOBS" ]; then
		wait -n 2>/dev/null || wait
		running=$((running - 1))
	fi
done
wait

end=$(date +%s)

# Headline summary: one row per suite, detail stays in the per-suite logs.
fails=0
printf '\n===== Unit test summary (logs: %s, -j%s) =====\n' "$LOGDIR" "$JOBS"
printf '%-16s %-6s %s\n' "SUITE" "RESULT" "PASS/FAIL/SKIP  TIME"
printf -- '-------------------------------------------------------\n'
for s in $ALL_SUITES; do
	st=$(cat "$LOGDIR/$s.status" 2>/dev/null || echo "?")
	if [ "$st" = "0" ]; then
		result="PASS"
	else
		result="FAIL"
		fails=$((fails + 1))
	fi
	pass=$(grep -m1 'Pass:'  "$LOGDIR/$s.log" 2>/dev/null | grep -oE '[0-9]+')
	fail=$(grep -m1 'Fail:'  "$LOGDIR/$s.log" 2>/dev/null | grep -oE '[0-9]+')
	skip=$(grep -m1 'Skip:'  "$LOGDIR/$s.log" 2>/dev/null | grep -oE '[0-9]+')
	time=$(grep -m1 'Time:'  "$LOGDIR/$s.log" 2>/dev/null | sed 's/.*Time: *//')
	printf '%-16s %-6s %s/%s/%s\t%s\n' \
		"$s" "$result" "${pass:-?}" "${fail:-?}" "${skip:-?}" "${time:-?}"
done
printf -- '-------------------------------------------------------\n'

if [ "$fails" -gt 0 ]; then
	echo "$fails suite(s) FAILED in $((end - start))s. Failing logs:"
	for s in $ALL_SUITES; do
		st=$(cat "$LOGDIR/$s.status" 2>/dev/null || echo "?")
		[ "$st" = "0" ] && continue
		seed=$(grep -m1 -oE 'OSDP_TEST_SEED=[0-9]+' "$LOGDIR/$s.log")
		echo "  $LOGDIR/$s.log${seed:+ ($seed)}"
	done
	exit 1
fi

echo "All $(echo "$ALL_SUITES" | wc -w) suites passed in $((end - start))s."
exit 0
