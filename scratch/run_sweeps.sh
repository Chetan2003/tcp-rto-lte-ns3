#!/usr/bin/env bash
# run_sweeps.sh — MinRto and RLC-buffer sensitivity sweeps, one shared job queue so all
# cores stay busy across configurations.  Resumable (completed runs are skipped).
#
#   scratch/run_sweeps.sh [jobs=4] [simTime=300]        (run from the ns-3.46 root)
#
# Configurations (6 seeds each; the baselines, MinRto 200 ms / RLC 32 KB, already exist in
# runs/lte, runs/lte_blk and runs/wired):
#   wired    MinRto 1000, 600, 400 ms         -> runs/wired_mr<ms>
#   lte      MinRto 1000, 600, 400 ms         -> runs/lte_mr<ms>        (radio-only)
#   lte_blk  MinRto 1000, 600, 400 ms         -> runs/lte_blk_mr<ms>    (radio + blockage)
#   lte      RLC buffer 5, 10, 64, 128 KB     -> runs/lte_rlc<KB>       (radio-only)
# Order is deliberate: the fast wired runs first, then the most informative LTE points.
set -euo pipefail
jobs=${1:-4}
simtime=${2:-300}
root=$(pwd)
[ -x ./ns3 ] || { echo "run from the ns-3.46 root"; exit 1; }
fad="$root/src/lte/model/fading-traces/fading_trace_EPA_3kmph.fad"
seeds="1 2 3 4 5 6"
queue=$(mktemp)

add() { for s in $seeds; do echo "$1|$2|$3|$s" >> "$queue"; done; }
for m in 1000 600 400; do add wired   "mr$m"  "--minRto=${m}ms"; done
for m in 1000 600 400; do add lte     "mr$m"  "--minRto=${m}ms"; done
for m in 1000 600 400; do add lte_blk "mr$m"  "--minRto=${m}ms"; done
for b in 5 10 64 128;  do add lte     "rlc$b" "--rlcBufferKB=$b"; done
echo "queued $(wc -l < "$queue") runs"

run_job() {
  IFS='|' read -r model tag extra seed <<< "$1"
  case "$model" in
    lte)     prog="scratch/lte_ue_to_ue";  base="--fadingTrace=$fad" ;;
    lte_blk) prog="scratch/lte_ue_to_ue";  base="--fadingTrace=$fad --blockage=true" ;;
    wired)   prog="scratch/wired_control"; base="" ;;
  esac
  dir="$root/runs/${model}_${tag}/seed_$seed"
  mkdir -p "$dir"
  if [ -f "$dir/DONE" ]; then echo "[skip] ${model}_${tag} seed $seed"; return 0; fi
  cmd="$prog --simTime=$simtime --RngRun=$seed $base $extra"
  echo "./ns3 run --no-build \"$cmd\" --cwd=$dir" > "$dir/cmd.txt"
  echo "[start] ${model}_${tag} seed $seed"
  if ./ns3 run --no-build "$cmd" --cwd="$dir" > "$dir/stdout.txt" 2>&1; then
    touch "$dir/DONE"; echo "[done]  ${model}_${tag} seed $seed"
  else
    echo "[FAIL]  ${model}_${tag} seed $seed (see $dir/stdout.txt)"
  fi
}
export -f run_job
export root simtime fad

xargs -a "$queue" -P "$jobs" -d '\n' -I{} bash -c 'run_job "$1"' _ {}
rm -f "$queue"
echo "sweeps finished"
