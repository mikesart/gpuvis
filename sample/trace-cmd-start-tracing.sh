#/bin/bash

TRACECMD=$(which trace-cmd)

if [ -z "${TRACECMD}" ]; then
    echo "ERROR: Could not locate trace-cmd binary"
    exit -1
fi

if [ ! -u "${TRACECMD}" ]; then
    echo "ERROR: ${TRACECMD} setuid not set"
    exit -1
fi

EVENTS=

# https://github.com/mikesart/gpuvis/wiki/TechDocs-Linux-Scheduler
EVENTS+=" -e sched:sched_switch"
EVENTS+=" -e sched:sched_process_fork"
EVENTS+=" -e sched:sched_process_exec"
EVENTS+=" -e sched:sched_process_exit"

EVENTS+=" -e drm:drm_vblank_event"
EVENTS+=" -e drm:drm_vblank_event_queued"

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
EVENTS+=" -e i915:intel_engine_notify"
EVENTS+=" -e i915:i915_gem_request_wait_begin"
EVENTS+=" -e i915:i915_gem_request_wait_end"

echo
CMD="trace-cmd start -b 2000 -i ${EVENTS}"
echo $CMD
$CMD

echo
./trace-cmd-status.sh
