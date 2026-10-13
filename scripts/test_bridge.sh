#!/bin/bash
cd /root/xquic/bridge
pkill -x h3_bridge 2>/dev/null; sleep 0.5
rm -f /tmp/h3_bridge.log /tmp/h3_bridge_stdout.log
setsid ./h3_bridge -l 0.0.0.0:18080 -L /tmp/h3_bridge.log -t 15 </dev/null >/tmp/h3_bridge_stdout.log 2>&1 &
sleep 1
echo "=== TEST 1: GET via proxy (absolute-form) ==="
curl -s -x http://127.0.0.1:18080 -o /tmp/br_get.out -w "code=%{http_code} ver=%{http_version} time=%{time_total}\n" --max-time 20 http://acs.m.taobao.com/gw/mtop.common.getTimestamp/1.0/
echo "--- body ---"; head -c 300 /tmp/br_get.out; echo
echo "=== TEST 2: response headers ==="
curl -sv -x http://127.0.0.1:18080 --max-time 20 http://acs.m.taobao.com/gw/mtop.common.getTimestamp/1.0/ -o /dev/null 2>&1 | grep -E "^< " | head -16
echo "=== TEST 3: POST with body ==="
curl -s -x http://127.0.0.1:18080 -X POST -d '{"test":1}' -o /tmp/br_post.out -w "code=%{http_code} time=%{time_total}\n" --max-time 20 http://acs.m.taobao.com/gw/mtop.common.getTimestamp/1.0/
head -c 300 /tmp/br_post.out; echo
echo "=== TEST 4: origin-form + Host header ==="
curl -s -o /tmp/br_of.out -w "code=%{http_code} time=%{time_total}\n" --max-time 20 --resolve acs.m.taobao.com:18080:127.0.0.1 http://acs.m.taobao.com:18080/gw/mtop.common.getTimestamp/1.0/
head -c 200 /tmp/br_of.out; echo
echo "=== TEST 5: concurrent x4 ==="
pids=""
for i in 1 2 3 4; do curl -s -x http://127.0.0.1:18080 -o /tmp/br_c$i.out -w "c$i=%{http_code}/%{time_total} " --max-time 20 http://acs.m.taobao.com/gw/mtop.common.getTimestamp/1.0/ & pids="$pids $!"; done
wait $pids
echo
echo "=== TEST 6: local health endpoint (GET /__health) ==="
curl -s -o /tmp/br_health.out -w "health code=%{http_code} time=%{time_total}\n" --max-time 5 http://127.0.0.1:18080/__health
cat /tmp/br_health.out
echo "=== TEST 7: loopback upstream guard (expect 400, fast, no hang) ==="
curl -s -o /tmp/br_loop.out -w "loop code=%{http_code} time=%{time_total}\n" --max-time 8 http://127.0.0.1:18080/
head -c 120 /tmp/br_loop.out; echo
echo "=== bridge stdout ==="
cat /tmp/h3_bridge_stdout.log | head -5
pkill -x h3_bridge 2>/dev/null
echo "=== DONE ==="
