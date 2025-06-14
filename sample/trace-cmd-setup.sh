#!/bin/bash

set -eu

ESC="$(printf '\033')"
NORMAL="${ESC}[0m"
YELLOW="${ESC}[1;33m"
GREEN="${ESC}[32;1m"

function spewTraceStatus() {
    echo "Tracing status:"

    OWNER="$(stat -c %U ${TRACECMD})"

    if [ -u "${TRACECMD}" ]; then
        echo "  ${TRACECMD} ${GREEN}(setuid set for '${OWNER}')${NORMAL}"
    else
        echo "  ${TRACECMD} ${YELLOW}(setuid not set)${NORMAL}"
    fi

    if egrep -q "${TRACEFS} .*tracefs" /proc/mounts; then
        echo "  ${TRACEFS} ${GREEN}(mounted)${NORMAL}"
    else
        echo "  ${TRACEFS} ${YELLOW}(not mounted)${NORMAL}"
    fi

    if [ -w "${TRACEFS}/trace_marker" ]; then
        echo "  ${TRACEFS}/trace_marker ${GREEN}(writable)${NORMAL}"
    else
        echo "  ${TRACEFS}/trace_marker ${YELLOW}(not writable)${NORMAL}"
    fi
}

ROOT_CMDS=

# note: trace-cmd mounts tracefs on /sys/kernel/tracing IFF it's not already mounted elsewhere;
# these scripts assume it's always /sys/kernel/tracing so we explicitly try to mount it
# there (it's safe to have it mounted in multiple places)
# from /proc/mounts:
#   nodev /sys/kernel/tracing tracefs rw,relatime 0 0
TRACEFS="/sys/kernel/tracing"

TRACECMD=

# Using command -v to get trace-cmd path:
#  https://stackoverflow.com/questions/592620/check-if-a-program-exists-from-a-bash-script
if [ -x "$(command -v trace-cmd)" ]; then
    TRACECMD=$(readlink -f "$(command -v trace-cmd)")
fi

if [ -z "${TRACECMD}" ]; then
    echo "ERROR: Could not locate trace-cmd binary"
    exit -1
fi

if [ -u "${TRACECMD}" ] && [ "$(stat -c %U ${TRACECMD})" == "root" ] ; then
    # trace-cmd owner and setuid set
    :
else
    # trace-cmd owner or setuid not set
    ROOT_CMDS+="# Make sure root owns trace-cmd\n"
    ROOT_CMDS+="chown root:root ${TRACECMD}\n\n"

    ROOT_CMDS+="# Add setuid bit to trace-cmd binary\n"
    ROOT_CMDS+="chmod u+s ${TRACECMD}\n\n"
fi

# Check if tracefs dir is mounted
if ! egrep -q " ${TRACEFS} .*tracefs" /proc/mounts; then
    ROOT_CMDS+="# Mounting ${TRACEFS}...\n"
    ROOT_CMDS+="mount -t tracefs nodev ${TRACEFS}\n\n"
fi

if [ -w "${TRACEFS}/trace_marker" ]; then
    # trace_marker exists and is writable
    :
else
    # trace_marker not writable or tracefs still to be mounted
    ROOT_CMDS+="# Add tracefs execute/search permissions\n"
    ROOT_CMDS+="chmod 0755 \"${TRACEFS}\"\n\n"

    ROOT_CMDS+="# Add trace_marker write permissions \n"
    ROOT_CMDS+="chmod 0222 \"${TRACEFS}/trace_marker\"\n"
fi

# Enable i915/xe perf to collect GPU data.
ROOT_CMDS+="sysctl --ignore dev.i915.perf_stream_paranoid=0"
ROOT_CMDS+="sysctl --ignore dev.xe.observation_paranoid=0"

if [ -z "${ROOT_CMDS}" ]; then
    :
else
    ROOT_CMDS_FILE="$(mktemp)"

    echo "if ! [ \"\$UID\" -eq 0 ]; then" > ${ROOT_CMDS_FILE}
    echo "  echo Enter your password to setup trace-cmd permissions:" >> ${ROOT_CMDS_FILE}
    echo "  echo sudo bash \"\$0\"" >> ${ROOT_CMDS_FILE}
    echo "  exec sudo bash \"\$0\"" >> ${ROOT_CMDS_FILE}
    echo "fi" >> ${ROOT_CMDS_FILE}
    echo "" >> ${ROOT_CMDS_FILE}

    echo -e ${ROOT_CMDS} >> ${ROOT_CMDS_FILE}

    echo ""
    echo "trace-cmd root initialization file (${ROOT_CMDS_FILE}):"
    echo "---------------------------------"
    cat -s -n "${ROOT_CMDS_FILE}"
    echo ""

    if [ "$UID" -eq 0 ]; then
        echo "bash ${ROOT_CMDS_FILE}"
        bash ${ROOT_CMDS_FILE}
    else
        # Do something ~ Baldur does in RenderDoc
        # https://github.com/baldurk/renderdoc/blob/v0.x/qrenderdoc/Code/QRDUtils.cpp#L826
        if [ -x "$(command -v pkexec)" ]; then
            echo "pkexec bash ${ROOT_CMDS_FILE}"
            pkexec bash ${ROOT_CMDS_FILE}
        elif [ -x "$(command -v kdesudo)" ]; then
            echo "kdesudo bash ${ROOT_CMDS_FILE}"
            kdesudo -c "bash ${ROOT_CMDS_FILE}"
        elif [ -x "$(command -v gksudo)" ]; then
            echo gksudo --message \"Setup trace-cmd permissions\" \"bash ${ROOT_CMDS_FILE}\"
            gksudo --message "Setup trace-cmd permissions" "bash ${ROOT_CMDS_FILE}"
        elif [ -x "$(command -v beesu)" ]; then
            echo beesu bash ${ROOT_CMDS_FILE}
            beesu bash ${ROOT_CMDS_FILE}
        elif [ -x "$(command -v xterm)" ]; then
            echo xterm -e "bash ${ROOT_CMDS_FILE}"
            xterm -e "bash ${ROOT_CMDS_FILE}; sleep 5"
        else
            echo "ERROR: Can't execute initilization file as root."
        fi
    fi

    echo
    rm ${ROOT_CMDS_FILE}
fi

spewTraceStatus
