#!/bin/bash
E=/mnt/d/AIReverse/project/taobao/work/xquic-h3-bridge/evidence
mkdir -p "$E"
cp -f /tmp/bridge_test_run2.out "$E/"
cp -f /tmp/bridge_test_run3.out "$E/" 2>/dev/null
cp -f /tmp/br_get.out "$E/br_get.out" 2>/dev/null
cp -f /tmp/br_post.out "$E/br_post.out" 2>/dev/null
cp -f /tmp/br_of.out "$E/br_of.out" 2>/dev/null
cp -f /tmp/br_c1.out "$E/br_c1.out" 2>/dev/null
cp -f /tmp/br_health.out "$E/br_health.out" 2>/dev/null
cp -f /tmp/br_loop.out "$E/br_loop.out" 2>/dev/null
cp -f /tmp/h3_bridge.log "$E/bridge_xquic_full.log" 2>/dev/null
grep -m3 "xqc_h3_request_destroy" /tmp/h3_bridge.log 2>/dev/null | cut -c1-360 > "$E/bridge_xquic_request_report.txt"
grep -m3 "xqc_conn_destroy" /tmp/h3_bridge.log 2>/dev/null | cut -c1-360 >> "$E/bridge_xquic_request_report.txt"
echo "--- cleanup stray files at / ---"
ls -la / | grep -E "bridge_|br_" 
rm -f /bridge_test_run2.out /br_get.out /br_post.out /br_of.out /br_c1.out /bridge_xquic_request_report.txt
echo "--- evidence dir ---"
ls -la "$E"
