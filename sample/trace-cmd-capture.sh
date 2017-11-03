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

# Take a snapshot of the trace buffer
CMD="trace-cmd snapshot -s"
echo ${CMD}
$CMD

# Extract the snapshot
CMD="trace-cmd extract -s -k -o trace_${DATE}.dat"
echo ${CMD}
$CMD
