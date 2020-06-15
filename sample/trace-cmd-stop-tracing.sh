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

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
${SCRIPT_DIR}/trace-cmd-status.sh
