#!/bin/bash

CMD="trace-cmd stat"
echo $CMD
$CMD

#
# cat /sys/kernel/tracing/per_cpu/cpu0/stats
#   entries: 5
#   overrun: 0
#   commit overrun: 0
#   bytes: 224
#   oldest event ts: 96955.082688
#   now ts: 97157.747457
#   dropped events: 0
#   read events: 0
#
# Ring buffer stats:
#   Entries: The number of events that are still in the buffer.
#   Overrun: The number of lost events due to overwriting when the buffer was full.
#   Commit overrun: Should always be zero.
#     This gets set if so many events happened within a nested event (ring buffer is re-entrant),
#     that it fills the buffer and starts dropping events.
#   Bytes: Bytes actually read (not overwritten).
#   Oldest event ts: The oldest timestamp in the buffer.
#   Now ts: The current timestamp.
#   Dropped events: Events lost due to overwrite option being off.
#   Read events: The number of events read.
#
if [ -r "/sys/kernel/tracing/per_cpu/cpu0/stats" ]; then

STAT_FILES=()

for (( i = 0 ; i <= 256 ; i++ )) ; do
    FILE="/sys/kernel/tracing/per_cpu/cpu${i}/stats"
    if [ ! -r ${FILE} ]; then
        break
    fi

    STAT_FILES+=($FILE)
done

awk 'BEGIN { time = 0; entries = 0; overrun = 0 }
    {
       if ( $0 ~ /^entries\:/ )
           entries = $2
       if ( $0 ~ /^overrun\:/ )
           overrun = $2
       if ( $0 ~ /^oldest event ts\:/ )
           time = (float)$4
       if ( $0 ~ /^now ts\:/ ) {
           time = (float)$3 - time
           if ( entries == 0 )
               printf "%s entries:%u\n", FILENAME, entries
           else
               printf "%s entries:%u overrun:%u time:%f sec\n", FILENAME, entries, overrun, time
           time = 0;
           entries = 0;
           overrun = 0;
       }
    }' ${STAT_FILES[@]}

fi
