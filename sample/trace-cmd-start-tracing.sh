#/bin/bash

DEBUGFS=$(grep -w ^debugfs /proc/mounts | awk '{ print $2; }')
TRACEFS=${DEBUGFS}/tracing

TRACECMD=$(which trace-cmd)
# TRACECMD=/usr/bin/trace-cmd

ROOT_CMDS=

# User name of owner: stat -c %U /sys/kernel/debug

if [ -z "${DEBUGFS}" ]; then
    echo "ERROR: Could not locate debugfs directory"
    exit -1
fi

if [ -z "${TRACECMD}" ]; then
    echo "ERROR: Could not locate trace-cmd binary"
    exit -1
fi

echo "debugfs: ${DEBUGFS} (permissions:$(stat -c %a ${DEBUGFS}))"
echo "tracefs: ${TRACEFS}"

if [ -w "${TRACEFS}/trace_marker" ]; then
    echo "trace_marker: ${TRACEFS}/trace_marker (writable)"
else
    echo "trace_marker: ${TRACEFS}/trace_marker (not writable)"

    ROOT_CMDS="${ROOT_CMDS}chmod 0755 \"${DEBUGFS}\"\n"
    ROOT_CMDS="${ROOT_CMDS}ls \"${TRACEFS}\" > /dev/null 2>&1\n"
    ROOT_CMDS="${ROOT_CMDS}chmod 0755 \"${TRACEFS}\"\n"
    ROOT_CMDS="${ROOT_CMDS}chmod 0222 \"${TRACEFS}/trace_marker\"\n"
fi

if [ -u "${TRACECMD}" ]; then
    echo "${TRACECMD}: (setuid set)"
else
    echo "${TRACECMD}: (setuid not set)"

    ROOT_CMDS="${ROOT_CMDS}chmod u+s ${TRACECMD}\n"
fi

if [ -z "${ROOT_CMDS}" ]; then
    :
else
    ROOT_CMDS_FILE="$(mktemp)"
    echo -e ${ROOT_CMDS} > ${ROOT_CMDS_FILE}
    echo ROOT_CMDS_FILE is ${ROOT_CMDS_FILE}

    cat "${ROOT_CMDS_FILE}"
    xterm -e "echo Enter your password to setup SteamVR GPU profiling:; sudo bash ${ROOT_CMDS_FILE}"
fi

EVENTS=

# https://github.com/mikesart/gpuvis/wiki/TechDocs-Linux-Scheduler
EVENTS="$EVENTS -e sched:sched_switch"
EVENTS="$EVENTS -e sched:sched_process_fork"
EVENTS="$EVENTS -e sched:sched_process_exec"
EVENTS="$EVENTS -e sched:sched_process_exit"

EVENTS="$EVENTS -e drm:drm_vblank_event"
EVENTS="$EVENTS -e drm:drm_vblank_event_queued"

# https://github.com/mikesart/gpuvis/wiki/TechDocs-AMDGpu
EVENTS="$EVENTS -e amdgpu:amdgpu_vm_flush"
EVENTS="$EVENTS -e amdgpu:amdgpu_cs_ioctl"
EVENTS="$EVENTS -e amdgpu:amdgpu_sched_run_job"
EVENTS="$EVENTS -e *fence:*fence_signaled"

# https://github.com/mikesart/gpuvis/wiki/TechDocs-Intel
#
# NOTE: the i915_gem_request_submit, i915_gem_request_in, i915_gem_request_out
# tracepoints require the CONFIG_DRM_I915_LOW_LEVEL_TRACEPOINTS Kconfig option to
# be enabled.
EVENTS="$EVENTS -e i915:i915_flip_request"
EVENTS="$EVENTS -e i915:i915_flip_complete"
EVENTS="$EVENTS -e i915:intel_gpu_freq_change"
EVENTS="$EVENTS -e i915:i915_gem_request_add"
EVENTS="$EVENTS -e i915:i915_gem_request_submit"
EVENTS="$EVENTS -e i915:i915_gem_request_in"
EVENTS="$EVENTS -e i915:i915_gem_request_out"
EVENTS="$EVENTS -e i915:intel_engine_notify"
EVENTS="$EVENTS -e i915:i915_gem_request_wait_begin"
EVENTS="$EVENTS -e i915:i915_gem_request_wait_end"

echo
CMD="trace-cmd start -b 2000 -i ${EVENTS}"
echo $CMD
$CMD

echo
./trace-cmd-status.sh
