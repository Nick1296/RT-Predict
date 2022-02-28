#!/usr/bin/bash
CURRENT_PATH=$(pwd)
BENCHMARKS=("disparity" "mser" "localization" "tracking" "sift")
INPUTS=("sim_fast" "sim" "sqcif" "cif" "vga")
TARGET_BMARKS=()
if [ "$EUID" -ne 0 ]; then
  echo "This script must be run as root to allow the tests to succeed"
  exit
fi
for bench in "${BENCHMARKS[@]}"; do
  for data in "${INPUTS[@]}"; do
    make -C "infrastructure/rt-bench/vision/benchmarks/${bench}/data/${data}" compile
    if [ $? -gt 0 ]; then
      echo "Cannot compile ${bench} with input size ${data}!"
      exit
    fi
  done
done
make
for bench in "${BENCHMARKS[@]}"; do
  for data in "${INPUTS[@]}"; do
    TARGET_BMARKS=("${TARGET_BMARKS[@]}" "$CURRENT_PATH/infrastructure/rt-bench/vision/benchmarks/${bench}/data/${data}/${bench} -p 1 -d 1 -t 1 -b infrastructure/rt-bench/vision/benchmarks/${bench}/data/${data}")
  done
done
OUTPUT="$CURRENT_PATH/data/1st_run.csv"
mkdir -p $CURRENT_PATH/data
for ((i = 0; i < 100; i++)); do
  for bench in "${TARGET_BMARKS[@]}"; do
    $CURRENT_PATH/infrastructure/profiler -o "$OUTPUT" "$bench"
    if [ $? -gt 0 ]; then
      echo "Cannot profile ${bench}!"
      exit
    fi
  done
done
chmod +rw -R "$OUTPUT"
if [ $? -gt 0 ]; then
  echo "Cannot make  $OUTPUT accessible to all users!"
  exit
fi
