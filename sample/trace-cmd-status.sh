#!/bin/bash

CMD="trace-cmd stat"
echo $CMD
$CMD

STAT_FILES=()

# /sys/kernel/tracing/per_cpu/cpu0/stats
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
           printf "%s entries:%u overrun:%u time:%f sec\n", FILENAME, entries, overrun, time
           time = 0;
           entries = 0;
           overrun = 0;
       }
    }' ${STAT_FILES[@]}
