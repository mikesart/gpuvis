#!/bin/bash

# Get path our script is running in
SCRIPT=$(realpath -s $0)
SCRIPTPATH=$(dirname $SCRIPT)

# Dirs to search for gpuvis binary in
GPUVISDIRS=(../_release ../_debug ../build)

COMMAND="$@"
if [ -z "${COMMAND}" ]; then
    echo "USAGE: ./trace-cmd-trace-command.sh [command ...]"
    exit 0
fi

TRACECMD=$(which trace-cmd)
if [ -z "${TRACECMD}" ]; then
    echo "ERROR: Could not locate trace-cmd binary"
    exit -1
fi

if [ ! -u "${TRACECMD}" ]; then
    echo "ERROR: ${TRACECMD} setuid bit not set. Run trace-cmd-setup.sh?"
    exit -1
fi

# Try to find a gpuvis binary
GPUVISBIN=$(which gpuvis)
if [ -z "${GPUVISBIN}" ]; then
  for i in ${GPUVISDIRS[@]}; do
    if [ -x "${SCRIPTPATH}/${i}/gpuvis" ]; then
      GPUVISBIN="${SCRIPTPATH}/${i}/gpuvis"
      break
    fi
  done
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

DATE=$(date +%m-%d-%Y_%H-%M-%S)
TRACEFILE=trace_${DATE}.dat

echo
CMD="trace-cmd record -b 8000 -D -o ${TRACEFILE} -i ${EVENTS} ${COMMAND}"
echo $CMD
$CMD

# Open trace in gpuvis if we found it, otherwise trace-cmd report
if [ -x "${GPUVISBIN}" ]; then
  CMD="${GPUVISBIN} ${TRACEFILE}"
else
  CMD="trace-cmd report -l ${TRACEFILE} | less"
fi

echo
echo $CMD
$CMD
