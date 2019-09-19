#!/bin/bash

CMD="trace-cmd reset"
echo $CMD
$CMD

CMD="trace-cmd snapshot -f"
echo $CMD
$CMD

if [ "${USE_I915_PERF} " ]; then
    CMD="i915-perf-control -q"
    echo $CMD
    $CMD
fi

./trace-cmd-status.sh
