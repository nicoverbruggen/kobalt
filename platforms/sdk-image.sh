#!/bin/sh
# Immutable prepared SDK images used by local builds and CI.
# Recipes and upstream source pins: https://github.com/nicoverbruggen/kobuild
# Qt5 recipe commit: 05283b966cb99098f5b197c96a3847790227db53
# Qt6 recipe commit: 3b39cbb803cab6279ebdf7f9415da675643590b3
# Android recipe commit: b55f833f909841c3b0394912bbd628bf5804fc42
set -eu
case "${1:-}" in
    qt5) echo ghcr.io/nicoverbruggen/kobuild@sha256:cfb68715d110d02e91df589f1e8940e9781a91d88b2aaee536f5f3830876fb25 ;;
    qt6) echo ghcr.io/nicoverbruggen/kobuild@sha256:b939301323e1bed0bf6b0bee43b8791db2efdf01231d4245c1f972d57433644a ;;
    android) echo ghcr.io/nicoverbruggen/kobuild@sha256:bb32726522eec89ea849ca85916d6819d492c8dfdcf3855c868bfa96dc21964c ;;
    *) echo 'Expected qt5, qt6 or android' >&2; exit 2 ;;
esac
