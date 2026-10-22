#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: bash $0 CONFIG.yaml INPUT.pcap [OUTPUT_DIRECTORY]" >&2
  echo "Optional env: ROUNDS=3 BATCH_SIZE=64 FEATURE_THREADS=4 INFERENCE_THREADS=2 WARMUP=1000 ITERATIONS=10000 PACKET_DETAILS=0 DEMO=/path/ktag_demo" >&2
  exit 2
fi
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
demo=${DEMO:-"$repo/output/ktag_demo"}
config=$(realpath -- "$1")
pcap=$(realpath -- "$2")
out=${3:-"$repo/result/latency-$(date +%Y%m%d-%H%M%S)"}
rounds=${ROUNDS:-3}
batch=${BATCH_SIZE:-64}
features=${FEATURE_THREADS:-4}
inference=${INFERENCE_THREADS:-2}
warmup=${WARMUP:-1000}
iterations=${ITERATIONS:-10000}
for number in "$rounds" "$batch" "$features" "$inference" "$iterations"; do
  if [[ ! "$number" =~ ^[1-9][0-9]*$ ]]; then echo "Counts must be positive integers" >&2; exit 2; fi
done
[[ "$warmup" =~ ^[0-9]+$ ]] || { echo "WARMUP must be a nonnegative integer" >&2; exit 2; }
(( batch > 1 && batch <= 65536 )) || { echo "BATCH_SIZE must be in 2..65536" >&2; exit 2; }
[[ -x "$demo" ]] || { echo "Missing executable: $demo; run sh build.sh first" >&2; exit 2; }
[[ -f "$config" && -f "$pcap" ]] || { echo "CONFIG and PCAP must exist" >&2; exit 2; }
mkdir -p -- "$out"
out=$(realpath -- "$out")
# Each command must finish before the next starts; no competing benchmark jobs.
for ((round=1; round<=rounds; ++round)); do
  for topology in single_thread full_parallel; do
    workers=(-n 1 --inference-threads 1)
    if [[ "$topology" == full_parallel ]]; then workers=(-n "$features" --inference-threads "$inference"); fi
    for mode in inference e2e; do
      for size in 1 "$batch"; do
        prefix="$out/r${round}-${topology}-${mode}-b${size}"
        extra=()
        if [[ "$mode" == e2e && ${PACKET_DETAILS:-0} == 1 ]]; then extra+=(--latency-packet-details); fi
        printf '\nRunning: %s\n' "$prefix"
        "$demo" -r "$config" -p "$pcap" --pipeline-mode "$topology" "${workers[@]}" \
          --latency-mode "$mode" --latency-output "$prefix" --latency-batch-size "$size" \
          --latency-warmup "$warmup" --latency-iterations "$iterations" "${extra[@]}" \
          >"$prefix.log" 2>&1 || { cat -- "$prefix.log" >&2; exit 1; }
        cat -- "$prefix.summary.csv"
      done
    done
  done
done
printf '\nAll runs completed: %s\n' "$out"
