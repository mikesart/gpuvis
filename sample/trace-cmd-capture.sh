#!/bin/bash

TRACE_ENABLED=0

if [ -r /sys/kernel/tracing/tracing_on ]; then
    read -N 1 TRACE_ENABLED < /sys/kernel/tracing/tracing_on
fi

if [ ${TRACE_ENABLED} -eq 0 ]; then
    echo -e "ERROR: Tracing is disabled\n"
    ./trace-cmd-status.sh
    exit -1
fi

DATE=$(date +%m-%d-%Y_%H-%M-%S)

# Stop tracer from recording more data
CMD="trace-cmd stop"
echo ${CMD}
$CMD

# Extract i915-perf trace
CMD="i915-perf-control -d trace_${DATE}.i915-dat"
echo ${CMD}
$CMD

# Extract trace
CMD="trace-cmd extract -k -o trace_${DATE}.dat"
echo ${CMD}
$CMD

# Restart recording
CMD="trace-cmd restart"
echo ${CMD}
$CMD
