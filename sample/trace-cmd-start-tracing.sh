#!/bin/bash

TRACECMD=$(which trace-cmd)

if [ -z "${TRACECMD}" ]; then
    echo "ERROR: Could not locate trace-cmd binary"
    exit -1
fi

if [ ! -u "${TRACECMD}" ]; then
    echo "ERROR: ${TRACECMD} setuid bit not set. Run trace-cmd-setup.sh?"
    exit -1
fi

if [ "${USE_I915_PERF}" ]; then
    if [ -z "${I915_PERF_METRIC}" ]; then
        echo "WARNING: Missing I915_PERF_METRIC value. Using default value 'RenderBasic'."
        I915_PERF_METRIC="RenderBasic"
    fi
fi

EVENTS=

# https://github.com/mikesart/gpuvis/wiki/TechDocs-Linux-Scheduler
EVENTS+=" -e sched:sched_switch"
EVENTS+=" -e sched:sched_process_fork"
EVENTS+=" -e sched:sched_process_exec"
EVENTS+=" -e sched:sched_process_exit"

EVENTS+=" -e drm:drm_vblank_event"
EVENTS+=" -e drm:drm_vblank_event_queued"
EVENTS+=" -e drm:drm_vblank_event_delivered"

# https://github.com/mikesart/gpuvis/wiki/TechDocs-AMDGpu
EVENTS+=" -e amdgpu:amdgpu_vm_flush"
EVENTS+=" -e amdgpu:amdgpu_cs_ioctl"
EVENTS+=" -e amdgpu:amdgpu_sched_run_job"
EVENTS+=" -e *fence:*fence_signaled"

# https://github.com/mikesart/gpuvis/wiki/TechDocs-Intel
#
# NOTE: the i915_gem_request_submit, i915_gem_request_in, i915_gem_request_out
# tracepoints require the CONFIG_DRM_I915_LOW_LEVEL_TRACEPOINTS Kconfig option to
# be enabled.
EVENTS+=" -e i915:i915_flip_request"
EVENTS+=" -e i915:i915_flip_complete"
EVENTS+=" -e i915:intel_gpu_freq_change"
EVENTS+=" -e i915:i915_gem_request_add"
EVENTS+=" -e i915:i915_gem_request_submit"
EVENTS+=" -e i915:i915_gem_request_in"
EVENTS+=" -e i915:i915_gem_request_out"
EVENTS+=" -e i915:i915_gem_request_queue"
EVENTS+=" -e i915:intel_engine_notify"
EVENTS+=" -e i915:i915_gem_request_wait_begin"
EVENTS+=" -e i915:i915_gem_request_wait_end"

EVENTS+=" -e i915:i915_request_add"
EVENTS+=" -e i915:i915_request_submit"
EVENTS+=" -e i915:i915_request_in"
EVENTS+=" -e i915:i915_request_out"
EVENTS+=" -e i915:i915_request_queue"
EVENTS+=" -e i915:i915_request_wait_begin"
EVENTS+=" -e i915:i915_request_wait_end"
EVENTS+=" -e i915:i915_request_retire"
EVENTS+=" -e i915:i915_request_execute"

EVENTS+=" -e i915:i915_pipe_update_vblank_evaded"

CMD="trace-cmd reset"
echo $CMD
$CMD

CLOCK=mono

echo
CMD="trace-cmd start -C ${CLOCK} -b 8000 -D -i ${EVENTS}"
echo $CMD
$CMD

if [ -e /tmp/.i915-perf-record ]; then
   CMD="i915-perf-control -q"
   echo $CMD
   $CMD
fi

if [ "${USE_I915_PERF}" ]; then
    CMD="i915-perf-recorder -m ${I915_PERF_METRIC} -s 8000 -k ${CLOCK}"
    echo $CMD
    $CMD &
fi

echo

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
${SCRIPT_DIR}/trace-cmd-status.sh
