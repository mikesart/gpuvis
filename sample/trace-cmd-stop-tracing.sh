#!/bin/bash

CMD="trace-cmd reset"
echo $CMD
$CMD

./trace-cmd-status.sh
