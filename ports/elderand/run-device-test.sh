#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

export ELD_HOST="${ELD_HOST:-192.168.31.154}"
export ELD_PASS="${ELD_PASS:-nextos}"
export ELD_USER="${ELD_USER:-root}"

secs="${1:-45}"
shift || true

base_env="ELD_DEVLIB=1 ELD_NOFATAL_OFF=1 CUP_NOSIGH=1 TER_CHOREO=1 ELD_GCOFF=1 CUP_1CORE=1 TER_JOBLOG=1 TER_THRCENSUS=28 CUP_DLLOG=1"
if [ "$#" -gt 0 ]; then
  extra_env="$base_env $*"
else
  extra_env="$base_env"
fi

exec ./run_test.sh "$secs" "$extra_env"
