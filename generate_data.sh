#!/usr/bin/bash
CURRENT_PATH=$(pwd)
BENCHMARKS=("disparity" "mser" "localization" "tracking" "sift")
INPUTS=("sim_fast" "sim" "sqcif" "cif" "vga")
#BENCHMARKS=("disparity")
#INPUTS=("vga")
TARGET_BMARKS=()
if [ "$EUID" -ne 0 ]; then
  echo "This script must be run as root to allow the tests to succeed"
  exit
fi
for bench in "${BENCHMARKS[@]}"; do
  for data in "${INPUTS[@]}"; do
    make -C "infrastructure/rt-bench/vision/benchmarks/${bench}/data/${data}" debug
    if [ $? -gt 0 ]; then
      echo "Cannot compile ${bench} with input size ${data}!"
      exit
    fi
  done
done
make -C infrastructure
if [ $? -gt 0 ]; then
  echo "Cannot compile the profiler"
  exit
fi
for bench in "${BENCHMARKS[@]}"; do
  for data in "${INPUTS[@]}"; do
    TARGET_BMARKS=("${TARGET_BMARKS[@]}" "$CURRENT_PATH/infrastructure/rt-bench/vision/benchmarks/${bench}/data/${data}/${bench}")
    BMARKS_ARGS=("${BMARKS_ARGS[@]}" "-p 1 -d 1 -t 1 -b infrastructure/rt-bench/vision/benchmarks/${bench}/data/${data}")
  done
done
OUTPUT="$CURRENT_PATH/data/10_runs_5_inputs_5_bmarks_raw.csv"
mkdir -p $CURRENT_PATH/data
for i in "${!TARGET_BMARKS[@]}"; do
  stap -vg $CURRENT_PATH/infrastructure/shm.stap ${TARGET_BMARKS[i]} &
  if [ $? -gt 0 ]; then
    echo "Cannot start systemtamp for ${TARGET_BMARKS[i]}!"
    exit
  fi
  sleep 10
  for ((j = 0; j < 10; j++)); do
    $CURRENT_PATH/infrastructure/profiler -o "$OUTPUT" "${TARGET_BMARKS[i]} ${BMARKS_ARGS[i]}"
    if [ $? -gt 0 ]; then
      echo "Cannot profile ${TARGET_BMARKS[i]}!"
      killall stap
      exit
    fi
  done
  killall stap
done
chmod +rw -R "$OUTPUT"
if [ $? -gt 0 ]; then
  echo "Cannot make  $OUTPUT accessible to all users!"
  exit
fi
