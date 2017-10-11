#/bin/bash

EVENTS=

# https://github.com/mikesart/gpuvis/wiki/TechDocs-Linux-Scheduler
EVENTS="$EVENTS -e 'sched:sched_switch'"
EVENTS="$EVENTS -e 'sched:sched_process_fork'"
EVENTS="$EVENTS -e 'sched:sched_process_exec'"
EVENTS="$EVENTS -e 'sched:sched_process_exit'"

EVENTS="$EVENTS -e 'drm:drm_vblank_event'"
EVENTS="$EVENTS -e 'drm:drm_vblank_event_queued'"

# https://github.com/mikesart/gpuvis/wiki/TechDocs-AMDGpu
EVENTS="$EVENTS -e 'amdgpu:amdgpu_vm_flush'"
EVENTS="$EVENTS -e 'amdgpu:amdgpu_cs_ioctl'"
EVENTS="$EVENTS -e 'amdgpu:amdgpu_sched_run_job'"
EVENTS="$EVENTS -e '*fence:*fence_signaled'"

# https://github.com/mikesart/gpuvis/wiki/TechDocs-Intel
#
# NOTE: the i915_gem_request_submit, i915_gem_request_in, i915_gem_request_out
# tracepoints require the CONFIG_DRM_I915_LOW_LEVEL_TRACEPOINTS Kconfig option to
# be enabled.
EVENTS="$EVENTS -e 'i915:i915_flip_request'"
EVENTS="$EVENTS -e 'i915:i915_flip_complete'"
EVENTS="$EVENTS -e 'i915:intel_gpu_freq_change'"
EVENTS="$EVENTS -e 'i915:i915_gem_request_add'"
EVENTS="$EVENTS -e 'i915:i915_gem_request_submit'"
EVENTS="$EVENTS -e 'i915:i915_gem_request_in'"
EVENTS="$EVENTS -e 'i915:i915_gem_request_out'"
EVENTS="$EVENTS -e 'i915:intel_engine_notify'"
EVENTS="$EVENTS -e 'i915:i915_gem_request_wait_begin'"
EVENTS="$EVENTS -e 'i915:i915_gem_request_wait_end'"

CMD="trace-cmd start -b 2000 -i ${EVENTS}"
echo $CMD
$CMD

./trace-cmd-status.sh
