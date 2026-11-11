#!/bin/sh
# Immutable prepared SDK images used by local builds and CI.
# Recipes and upstream source pins: https://github.com/nicoverbruggen/kobuild
# Qt5 recipe commit: 05283b966cb99098f5b197c96a3847790227db53
# Qt6 recipe commit: 3b39cbb803cab6279ebdf7f9415da675643590b3
# Android recipe commit: df4a11fdebfdb8f73b1e7e91b6298191dc93f8e6
# Linux recipe commit: 1f6cfcb817382a9447797efe413b9e7cb9faad40
set -eu
case "${1:-}" in
    qt5) echo ghcr.io/nicoverbruggen/kobuild@sha256:cfb68715d110d02e91df589f1e8940e9781a91d88b2aaee536f5f3830876fb25 ;;
    qt6) echo ghcr.io/nicoverbruggen/kobuild@sha256:b939301323e1bed0bf6b0bee43b8791db2efdf01231d4245c1f972d57433644a ;;
    android) echo ghcr.io/nicoverbruggen/kobuild@sha256:d42aac6284da20c197d8ebcd9e17629e6e47842e5d00279523e3abb91cc95d5e ;;
    linux) echo ghcr.io/nicoverbruggen/kobuild@sha256:c08fdbb1a2bdb2878f32b1f985061ad2cb6bc9334872dda10296cb733f8834fe ;;
    *) echo 'Expected qt5, qt6, android or linux' >&2; exit 2 ;;
esac
