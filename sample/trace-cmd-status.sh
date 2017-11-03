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

awk '{
        if ( ( $2 > 0 ) && ( $0 ~ /^(entries\:|overrun\:)/ ) )
        {
            printf "%s: %s\n", FILENAME, $0
        }
     }' ${STAT_FILES[@]}
