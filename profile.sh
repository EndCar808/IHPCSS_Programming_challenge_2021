#!/bin/bash

for (( i = 0; i < 10; i++ )); do
	# sh submit.sh c cpu big datasets/cpu_big_optimized_no_openmp_$i.txt
	# sh submit.sh c cpu small datasets/cpu_optimised_no_openp_$i.txt
	
	sh submit.sh c gpu big datasets/gpu_collective_optimalupdate_$i.txt
done