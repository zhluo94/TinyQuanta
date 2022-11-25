#!/bin/bash

for cpu_id in {28..32}
do
	echo "performance" | sudo tee /sys/devices/system/cpu/cpu${cpu_id}/cpufreq/scaling_governor
done