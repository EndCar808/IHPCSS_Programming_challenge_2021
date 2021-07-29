#!/bin/bash

sh compile_gpu_versions.sh

sh submit.sh c gpu big datasets/gpu_big_opt.txt

