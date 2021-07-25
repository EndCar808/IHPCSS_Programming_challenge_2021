#!/bin/bash

## This script is for running the programme after requesting 'interact -gpu' - for debugging etc.

export OMP_NUM_THREADS=1
export OMP_PROC_BIND=true
export OMP_PLACES=cores

module load nvhpc
mpirun -np 1 --map-by socket -bind-to socket -report-bindings ./bin/c/gpu_small 