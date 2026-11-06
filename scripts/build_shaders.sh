#!/bin/sh
# Пересборка шейдеров дымки (.qsb). Запускать после правки mist.frag / mist.vert:
# qmake не собирает шейдеры сам, а в программу попадают только .qsb из assets.qrc.
#   QSB=/путь/к/qsb scripts/build_shaders.sh
set -e
QSB="${QSB:-qsb}"
cd "$(dirname "$0")/../assets/shaders/mist"
for s in mist.vert mist.frag; do
    "$QSB" --glsl "100 es,120,150" --hlsl 50 --msl 12 -o "$s.qsb" "$s"
done
echo "OK: $(pwd)/mist.vert.qsb, mist.frag.qsb"
