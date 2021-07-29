#!/bin/bash

threads=(1 2 4 8 16 32 64)
# for (( i = 0; i < 5; i++ )); do
for i in "${threads[@]}" do
	export OMP_NUM_THREADS=$i
	sh submit.sh c cpu big datasets/cpu_submission_$i.txt
	# sh submit.sh c cpu small datasets/cpu_optimised_no_openp_$i.txt
	
	# sh submit.sh c gpu big datasets/gpu_collective_optimal_nonblocking_$i.txt
done