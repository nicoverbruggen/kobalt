#!/bin/bash
# Tagged releases are quiet; branch, nightly and interim builds retain diagnostics.
set -euo pipefail
[[ $# == 1 ]] || { echo 'Expected the build version' >&2; exit 2; }
if [[ ${GITHUB_REF:-} == refs/tags/* && ! $1 =~ -(nightly|interim)([.-]|$) ]]; then
    printf 'OFF\n'
else
    printf 'ON\n'
fi
