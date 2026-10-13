#!/bin/bash
set -e
mkdir -p /root/xquic/bridge
cp -f /mnt/d/AIReverse/project/taobao/work/xquic-h3-bridge/src/bridge.c /root/xquic/bridge/bridge.c
cd /root/xquic/bridge
gcc -g -O2 -std=gnu11 -Wall -Wno-unused -Wno-pointer-sign -Wno-format-security \
  -DNDEBUG_PRINT -DNPRINT_MALLOC -DXQC_ENABLE_EVENT_LOG \
  -Wno-error=maybe-uninitialized -Wno-error=stringop-truncation -Wno-error=dangling-pointer \
  -I/root/xquic/include -I/root/xquic -I/root/xquic/third_party/boringssl/include \
  bridge.c -o h3_bridge \
  /root/xquic/build/libxquic-static.a \
  /root/xquic/third_party/boringssl/build/libssl.a \
  /root/xquic/third_party/boringssl/build/libcrypto.a \
  /usr/lib/x86_64-linux-gnu/libevent.so \
  -lm -ldl -lstdc++ -lpthread
echo "BUILD OK"; ls -la h3_bridge