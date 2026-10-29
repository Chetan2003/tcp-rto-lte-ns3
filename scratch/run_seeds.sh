#!/usr/bin/env bash
# run_seeds.sh — run one model for a range of RNG seeds, N jobs in parallel.
#
#   scratch/run_seeds.sh <model> <first_seed> <last_seed> [simTime=300] [jobs=4]
#
#   model:  lte      LTE UE-to-UE, lossy radio (AM + HARQ + Rayleigh), no blockage
#           lte_blk  same + seconds-scale blockage episodes on UE-A (--blockage=true)
#           wired    wired control twin (Arm 1, outage-free)
#
# Sensitivity sweeps: set TAG and EXTRA in the environment, e.g.
#   TAG=mr1000 EXTRA="--minRto=1000ms" scratch/run_seeds.sh lte 1 6
# which writes to runs/lte_mr1000/ instead of runs/lte/.
#
# Output: runs/<model>[_<TAG>]/seed_<N>/  (every CSV, stdout.txt, cmd.txt).  Existing complete
# runs are skipped, so an interrupted batch can simply be restarted.
# Run from the ns-3.46 root.  Cost: roughly 3 s wall per simulated second for LTE
# (about 15 min per 300 s run), about 0.4 s per simulated second for wired.
set -euo pipefail

model=${1:?model: lte|lte_blk|wired}
first=${2:?first seed}
last=${3:?last seed}
simtime=${4:-300}
jobs=${5:-4}

root=$(pwd)
[ -x ./ns3 ] || { echo "run from the ns-3.46 root"; exit 1; }
fad="$root/src/lte/model/fading-traces/fading_trace_EPA_3kmph.fad"

case "$model" in
  lte)     prog="scratch/lte_ue_to_ue";  extra="--fadingTrace=$fad";                  done_file=throughput_lte.csv ;;
  lte_blk) prog="scratch/lte_ue_to_ue";  extra="--fadingTrace=$fad --blockage=true";  done_file=throughput_lte.csv ;;
  wired)   prog="scratch/wired_control"; extra="";                                    done_file=throughput_wired.csv ;;
  *) echo "unknown model $model"; exit 1 ;;
esac

run_one() {
  seed=$1
  dir="$root/runs/${model}${TAG:+_$TAG}/seed_$seed"
  mkdir -p "$dir"
  if [ -f "$dir/DONE" ]; then echo "[skip] $model seed $seed"; return 0; fi
  cmd="$prog --simTime=$simtime --RngRun=$seed $extra ${EXTRA:-}"
  echo "./ns3 run --no-build \"$cmd\" --cwd=$dir" > "$dir/cmd.txt"
  echo "[start] $model seed $seed"
  if ./ns3 run --no-build "$cmd" --cwd="$dir" > "$dir/stdout.txt" 2>&1; then
    touch "$dir/DONE"; echo "[done]  $model seed $seed"
  else
    echo "[FAIL]  $model seed $seed (see $dir/stdout.txt)"
  fi
}
export -f run_one
export root model prog extra simtime TAG EXTRA
TAG=${TAG:-}; EXTRA=${EXTRA:-}

seq "$first" "$last" | xargs -P "$jobs" -I{} bash -c 'run_one {}'
echo "batch finished: $model seeds $first..$last"
