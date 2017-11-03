#!/bin/bash

CMD="trace-cmd reset"
echo $CMD
$CMD

CMD="trace-cmd snapshot -f"
echo $CMD
$CMD

./trace-cmd-status.sh
