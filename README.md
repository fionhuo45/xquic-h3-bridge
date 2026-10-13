# xqc-h3-bridge — 本地 HTTP/1.1 → 上游 HTTP/3 (XQUIC) 桥接进程

独立桥接进程: 把本地 HTTP/1.1 请求转成上游 HTTP/3 (QUIC) 发出。为复刻淘宝 mtop 网关的
QUIC(iQUIC) 链路而做——boot 侧通过配置切换 `direct`(旧通讯) / `h3bridge`(新通讯) 两种请求方式,
`h3bridge` 模式下由 boot 自己拉起本服务。

仓库: https://github.com/fionhuo45/xquic-h3-bridge

## 状态 (2026-10-13 验证)

- 上游: 淘宝 mtop 网关 HTTP/3, 客户端栈 = 与 APK `libxquic.so` 同版本区间一致的 upstream XQUIC **v1.9.4** (静态链 BoringSSL)。
- 本机链路全绿: WSL 内 curl / **Windows 侧 curl 跨边界** (`127.0.0.1:18080`, WSL localhost 转发) → 桥 → 淘宝 h3。
- 结果: GET / POST / origin-form / 并发 x4 全部 `200` + mtop `SUCCESS::接口调用成功`, 单请求 ~90–110ms (含 QUIC 握手 ~46–50ms)。
- 上游响应头原样透传, 可见 `x-protocol: HTTP/3.0`、`server: Tengine/3.0.46_20260920152855`。
- 本地健康检查 `GET /__health` (0.2ms, 不进上游); 回环上游地址直接 `400` (防自环挂起)。

## 构建

- 源码: `src/bridge.c` (xquic 公开 API + libevent)
- 构建: `scripts/build_bridge.sh` (在 WSL2 `AndroidLab-Ubuntu-24.04` 内执行; gcc 静态链接 `/root/xquic/build/libxquic-static.a` + BoringSSL)
- 产物: `/root/xquic/bridge/h3_bridge`

## 运行

```
/root/xquic/bridge/h3_bridge [-l ip:port] [-L logfile] [-t timeout_sec] [-d]
# 默认: -l 0.0.0.0:18080 -L /tmp/xqc_h3_bridge.log -t 15
# 后台:  setsid ./h3_bridge -l 0.0.0.0:18080 -L /root/xquic/bridge/h3_bridge.log </dev/null >/tmp/h3_bridge_stdout.log 2>&1 &
```

## 入口协议 (boot 侧对接面)

本地 HTTP/1.1 简单子集, 支持两种请求形式:

1. 绝对形式 (curl `-x` / .NET HttpClient Proxy 模式):
   `GET http://acs.m.taobao.com/gw/mtop.common.getTimestamp/1.0/ HTTP/1.1`
2. 原始形式 + Host 头:
   `POST /gw/mtop.common.getTimestamp/1.0/ HTTP/1.1` + `Host: acs.m.taobao.com`

规则:

- 请求体仅 `Content-Length` 定长; **不支持** CONNECT / chunked / keep-alive (每请求一连接)。
- 上游一律按 `https + HTTP/3` 处理: URL/Host 中显式端口生效, 否则 443 (`http://` scheme 只影响入口形式, 不影响上游)。
- 响应: `HTTP/1.1 + Content-Length + Connection: close`; 上游头透传 (剔除 hop-by-hop)。
- `GET /__health` → 本地 `200 xqc-h3-bridge ok`, 不进上游, 用于 boot 就绪探测与巡检。
- 回环上游地址 (localhost, 127.x, ::1, 0.0.0.0) → `400`, 防止把桥自身当上游自环。

## boot 侧对接 (已实现, 2026-10-13)

boot (tb-order-boot) 默认 `direct`(旧通讯, 行为不变); 配置切到 `h3bridge`(新通讯) 后:

- 启动时探测桥; 未就绪则通过 `wsl.exe -d <distro>` 拉起本服务 (setsid 常驻), 轮询 `/__health` 直到就绪;
- 就绪后, hosts 白名单内的 mtop 网关请求改写为 `http://<桥>/<path>` + `Host: <原域名>` 发出, 桥转 HTTP/3;
- 桥不可用且允许时回落到直连 (fallback-to-direct); 运行期 30s 健康巡检, 桥掉线自动切回直连。

配置 (打包 `application.yml` 默认值, 部署目录 `application.properties` 可覆盖, 改后重启 boot 生效):

```yaml
transport:
  mode: direct            # direct = 旧通讯(直连 HTTPS, 默认); h3bridge = 新通讯(经本桥走 HTTP/3)
  h3bridge:
    endpoint: ''          # 留空 = 自动: 优先 http://127.0.0.1:18080, 不通再取 WSL IP
    port: 18080
    auto-start: true      # h3bridge 模式下 boot 启动时自动拉起本服务
    wsl-distro: AndroidLab-Ubuntu-24.04
    bridge-path: /root/xquic/bridge/h3_bridge
    start-timeout-ms: 45000
    fallback-to-direct: true
    hosts: acs.m.taobao.com,acs.m.tmall.com,guide-acs.m.taobao.com,trade-acs.m.taobao.com,h5api.m.taobao.com,h5api.m.tmall.com
```

说明: 改写只影响 `hosts` 白名单内的域名; 支付宝/页面/图片等其它域名始终直连。
`hosts` 默认集 = 复刻验收中 `:status=200 + x-protocol=HTTP/3.0` 全绿的六个 acs 网关。

## 测试

- `scripts/test_bridge.sh` — 7 项用例 (GET / 响应头 / POST / origin-form / 并发 x4 / 健康检查 / 回环防护)
- 证据: `evidence/bridge_test_run3.out` (最新完整输出)、`evidence/br_*.out`、
  `evidence/bridge_xquic_full.log` (xquic 侧日志)、`evidence/bridge_xquic_request_report.txt`

## 限制 / TODO

- 每请求独立 QUIC 连接 (握手 ~46ms)。TODO: 按 host 连接池 + 0-RTT (session ticket 目前未持久化)。
- 不做证书校验 (与上游 demo 行为一致; 桥内网使用)。
- DNS 解析优先 IPv4; 不支持 CONNECT (https 直通) 与 chunked 请求体。
- 当前为 Linux ELF (WSL2 承载, Windows 侧经 `127.0.0.1` 转发访问); 需要 Windows 原生常驻时再交叉编译。

## 文件

```
xquic-h3-bridge/
├─ src/bridge.c                 # 源码
├─ scripts/build_bridge.sh      # 构建 (WSL 内执行)
├─ scripts/test_bridge.sh       # 功能测试 x7
├─ scripts/copy_evidence.sh     # 证据归档
├─ evidence/                    # 测试输出 + xquic 日志 + 响应体
└─ README.md
```
