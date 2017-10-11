#!/bin/bash

DATE=$(date +%m-%d-%Y_%H-%M-%S)

CMD="trace-cmd extract -o trace_${DATE}.dat"
echo ${CMD}
$CMD
