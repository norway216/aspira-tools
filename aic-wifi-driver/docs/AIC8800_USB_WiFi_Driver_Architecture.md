# AIC8800 USB WiFi 模块驱动完整架构设计

> 版本：V1.0  
> 日期：2026-06-30  
> 适用对象：AIC8800 / AIC8800D80 / AIC8800DC 等 USB WiFi 模块  
> 目标平台：RK3568 / RK3588 / x86 Ubuntu / Debian / Yocto / Buildroot  
> 推荐内核范围：Linux 5.10 LTS、5.15 LTS、6.1 LTS、6.6 LTS、6.12+  
> 设计目标：高性能、高稳定性、可维护、可量产、可诊断、可自愈

---

## 1. 设计背景

AIC8800 系列 USB WiFi 模块常用于低成本 WiFi 6 USB 网卡、嵌入式板卡外接 WiFi、医疗设备网络连接等场景。现有公开驱动通常是厂商 out-of-tree 驱动，常见模块包括：

```text
aic_load_fw.ko
aic8800_fdrv.ko
```

在很多发行版或嵌入式系统中，AIC8800 驱动存在以下典型问题：

1. 内核版本适配成本高。
2. firmware 版本和驱动版本不匹配容易卡死。
3. USB 热插拔、断连、休眠恢复稳定性不足。
4. 高吞吐场景下 CPU 占用较高。
5. NetworkManager / wpa_supplicant / cfg80211 状态不一致。
6. 异常恢复路径不完整，出现假连接、无法扫描、无法重连。
7. 日志和诊断能力不足，现场问题难复现。
8. 模块加载顺序、固件加载顺序容易出错。
9. DKMS / Yocto / Buildroot / Debian 打包体系不统一。
10. 医疗设备、工业设备中需要更严格的稳定性和可追溯性。

因此，本设计不是简单“能编译、能联网”的驱动，而是面向工业级产品的完整驱动架构。

---

## 2. 总体设计目标

### 2.1 功能目标

驱动需要支持：

- USB 设备枚举。
- firmware 加载。
- WiFi STA 模式。
- 扫描 AP。
- 连接 WPA/WPA2/WPA3，具体取决于 firmware 能力。
- 断线重连。
- 热插拔。
- suspend / resume。
- runtime PM。
- NetworkManager / wpa_supplicant 兼容。
- cfg80211 / nl80211 接口。
- 日志、统计、debugfs。
- 异常检测和自恢复。
- Yocto / Buildroot / DKMS 集成。

### 2.2 性能目标

建议目标值：

| 指标 | 目标 |
|---|---|
| USB 模式 | USB 2.0 High-Speed 起步，USB 3.0 兼容 |
| 2.4G 吞吐 | 稳定 60~120 Mbps，视天线和 AP 环境 |
| 5G 吞吐 | 稳定 150~400 Mbps，视芯片、AP 和信道 |
| 断线恢复 | 5 秒内重新触发连接流程 |
| 热插拔恢复 | 3 秒内重新识别网卡 |
| 高负载丢包 | 持续 iperf3 下无驱动崩溃 |
| 连续运行 | 7x24 小时无 kernel panic |
| CPU 占用 | 高吞吐场景尽量低于单核 50% |
| 内存泄漏 | 长测 72 小时无持续增长 |

### 2.3 稳定性目标

驱动必须防止：

- USB URB 泄漏。
- workqueue 悬空。
- skb 泄漏。
- disconnect 与 TX/RX 并发导致 use-after-free。
- firmware 加载失败后继续访问设备。
- suspend 过程中仍提交 URB。
- remove 后 worker 仍访问私有结构。
- mutex / spinlock 死锁。
- cfg80211 状态和 firmware 状态不一致。
- WiFi 看似 connected，但实际无 IP 或无数据链路。

---

## 3. 推荐总体架构

### 3.1 架构分层

建议将驱动拆成 8 层：

```text
+------------------------------------------------------+
| 用户空间                                             |
| NetworkManager / wpa_supplicant / iw / ip / systemd |
+------------------------------------------------------+
                         |
                         v
+------------------------------------------------------+
| Linux Wireless Core                                  |
| cfg80211 / nl80211                                   |
+------------------------------------------------------+
                         |
                         v
+------------------------------------------------------+
| AIC8800 WiFi Core Driver                             |
| 状态机 / 扫描 / 连接 / 密钥 / 速率 / 统计             |
+------------------------------------------------------+
                         |
                         v
+------------------------------------------------------+
| AIC8800 Data Path                                    |
| TX Queue / RX Queue / QoS / skb / NAPI-like polling  |
+------------------------------------------------------+
                         |
                         v
+------------------------------------------------------+
| AIC8800 Control Path                                 |
| 命令队列 / 事件队列 / firmware message / mailbox     |
+------------------------------------------------------+
                         |
                         v
+------------------------------------------------------+
| AIC8800 Firmware Loader                              |
| firmware 下载 / patch table / 校验 / 版本匹配         |
+------------------------------------------------------+
                         |
                         v
+------------------------------------------------------+
| USB HCI Layer                                        |
| bulk in / bulk out / control urb / usb anchor         |
+------------------------------------------------------+
                         |
                         v
+------------------------------------------------------+
| USB Core / xHCI / EHCI / PHY / Power Management      |
+------------------------------------------------------+
```

---

## 4. 关键设计原则

### 4.1 不建议一开始重写成 mac80211 驱动

AIC8800 现有公开驱动通常更接近 FullMAC 模式，也就是 firmware 负责大量 802.11 MAC 管理逻辑，Linux 驱动主要对接 cfg80211，并通过私有命令和 firmware 通信。

因此推荐路线是：

```text
第一阶段：保留 FullMAC + cfg80211 架构，重构稳定性和数据路径
第二阶段：抽象 HCI / firmware / cfg80211 层，提升可维护性
第三阶段：如果厂商开放足够 firmware 能力，再考虑 mac80211 化
```

不要在没有 firmware 细节的情况下强行改造成 mac80211 驱动，否则开发成本和风险会非常高。

### 4.2 驱动内必须有明确状态机

不要用一堆 bool 变量判断状态。需要统一状态机。

推荐状态：

```c
enum aic_dev_state {
    AIC_STATE_UNINIT = 0,
    AIC_STATE_USB_PROBED,
    AIC_STATE_FW_LOADING,
    AIC_STATE_FW_READY,
    AIC_STATE_HW_READY,
    AIC_STATE_NETDEV_REGISTERED,
    AIC_STATE_SCANNING,
    AIC_STATE_CONNECTING,
    AIC_STATE_CONNECTED,
    AIC_STATE_DISCONNECTING,
    AIC_STATE_SUSPENDING,
    AIC_STATE_SUSPENDED,
    AIC_STATE_RECOVERING,
    AIC_STATE_REMOVING,
    AIC_STATE_DEAD,
};
```

状态转换必须集中管理：

```c
int aic_state_transition(struct aic_dev *adev,
                         enum aic_dev_state from,
                         enum aic_dev_state to,
                         const char *reason);
```

所有关键路径都要检查状态：

```c
if (!aic_state_can_tx(adev))
    return NETDEV_TX_BUSY;
```

---

## 5. 模块划分

### 5.1 推荐目录结构

```text
drivers/net/wireless/aic8800/
├── Kconfig
├── Makefile
├── aic_main.c              # module init/exit
├── aic_usb.c               # USB probe/disconnect/suspend/resume
├── aic_usb.h
├── aic_fw.c                # firmware 加载、校验、版本匹配
├── aic_fw.h
├── aic_hci.c               # host-controller interface
├── aic_hci.h
├── aic_cmd.c               # 控制命令
├── aic_cmd.h
├── aic_event.c             # firmware event 处理
├── aic_event.h
├── aic_tx.c                # TX 数据路径
├── aic_tx.h
├── aic_rx.c                # RX 数据路径
├── aic_rx.h
├── aic_cfg80211.c          # cfg80211_ops
├── aic_cfg80211.h
├── aic_netdev.c            # net_device 兼容层，如果需要
├── aic_pm.c                # runtime PM / suspend / resume
├── aic_pm.h
├── aic_recovery.c          # 自愈与复位
├── aic_recovery.h
├── aic_debugfs.c           # debugfs
├── aic_debugfs.h
├── aic_trace.h             # tracepoints
├── aic_stats.c             # 统计
├── aic_stats.h
├── aic_compat.h            # 内核版本兼容封装
└── firmware/
    └── README.md
```

如果仍然沿用厂商模块结构，也建议逻辑上拆成：

```text
aic_load_fw.ko
    - 只负责 firmware 下载、USB patch、校验

aic8800_fdrv.ko
    - WiFi 主驱动
    - cfg80211
    - data path
    - recovery
```

但从长期维护角度，建议逐渐合并成一个主模块，或者至少让两个模块共享统一的接口定义和版本检查。

---

## 6. 核心对象设计

### 6.1 全局设备结构

```c
struct aic_dev {
    struct usb_device       *udev;
    struct usb_interface    *intf;
    struct device           *dev;

    struct wiphy            *wiphy;
    struct wireless_dev     *wdev;
    struct net_device       *ndev;

    enum aic_dev_state       state;
    spinlock_t               state_lock;
    struct mutex             op_mutex;

    struct aic_usb           usb;
    struct aic_fw            fw;
    struct aic_txq           txq;
    struct aic_rxq           rxq;
    struct aic_cmd_mgr       cmd;
    struct aic_event_mgr     event;
    struct aic_pm            pm;
    struct aic_recovery      recovery;
    struct aic_stats         stats;

    struct workqueue_struct *wq;
    struct work_struct       event_work;
    struct work_struct       recovery_work;
    struct delayed_work      link_watch_work;
    struct delayed_work      health_check_work;

    atomic_t                 refcnt;
    atomic_t                 tx_pending;
    atomic_t                 rx_pending;
    atomic_t                 reset_pending;

    bool                     removing;
    bool                     fw_ready;
    bool                     surprise_removed;
};
```

### 6.2 USB 层结构

```c
struct aic_usb {
    unsigned int bulk_in_pipe;
    unsigned int bulk_out_pipe;
    unsigned int intr_in_pipe;
    unsigned int ctrl_pipe;

    u16 bulk_in_maxp;
    u16 bulk_out_maxp;

    struct usb_anchor rx_anchor;
    struct usb_anchor tx_anchor;
    struct usb_anchor ctrl_anchor;

    struct sk_buff_head rx_free;
    struct sk_buff_head rx_done;
    struct sk_buff_head tx_pending;
    struct sk_buff_head tx_done;

    int rx_urb_num;
    int tx_urb_num;
    int rx_buf_size;
    int tx_buf_size;

    atomic_t rx_urb_inflight;
    atomic_t tx_urb_inflight;

    bool usb_online;
};
```

### 6.3 TX 队列结构

```c
#define AIC_TXQ_NUM 4

struct aic_txq {
    struct sk_buff_head ac[AIC_TXQ_NUM];   /* BK/BE/VI/VO */
    spinlock_t lock;

    int high_watermark;
    int low_watermark;

    atomic_t stopped;
    atomic_t dropped;
    atomic_t completed;

    struct work_struct tx_work;
};
```

### 6.4 RX 队列结构

```c
struct aic_rxq {
    struct sk_buff_head pending;
    spinlock_t lock;

    struct napi_struct napi;       /* 可选：如果走 netdev NAPI */
    int budget;

    atomic_t received;
    atomic_t dropped;
    atomic_t errors;
};
```

---

## 7. USB HCI 层设计

### 7.1 USB endpoint 识别

probe 阶段需要解析 interface descriptor：

```text
bulk in     -> RX data / firmware event
bulk out    -> TX data / command
interrupt   -> optional event
control     -> firmware / register / vendor command
```

probe 流程：

```text
usb_register()
    |
    v
aic_usb_probe()
    |
    +-- usb_get_dev()
    +-- usb_set_intfdata()
    +-- parse endpoint
    +-- allocate aic_dev
    +-- init locks / queues / anchors
    +-- load firmware
    +-- init HCI
    +-- register cfg80211/wiphy
    +-- start RX URBs
    +-- enable device
```

### 7.2 URB 管理原则

所有已提交 URB 必须被 anchor 管理：

```c
usb_anchor_urb(urb, &adev->usb.rx_anchor);
ret = usb_submit_urb(urb, GFP_ATOMIC);
if (ret) {
    usb_unanchor_urb(urb);
    return ret;
}
```

disconnect / suspend / reset 时统一停止：

```c
usb_kill_anchored_urbs(&adev->usb.rx_anchor);
usb_kill_anchored_urbs(&adev->usb.tx_anchor);
usb_kill_anchored_urbs(&adev->usb.ctrl_anchor);
```

### 7.3 RX URB 预提交

高性能 USB WiFi 驱动不能收到包才分配 URB，而应该预先提交多个 RX URB：

```text
RX_URB_NUM = 16 / 32
RX_BUF_SIZE = 4096 / 8192 / 16384
```

策略：

```text
probe 完成后预提交 N 个 RX URB
RX complete 后：
    1. 检查状态
    2. 解析数据包
    3. 投递 skb 到协议栈
    4. 重新提交 URB
```

### 7.4 TX URB 池

TX 不建议每个包临时 usb_alloc_urb / usb_free_urb。推荐使用 URB 池：

```text
TX_URB_NUM = 16 / 32
每个 TX URB 绑定一个 skb
发送完成后归还 URB
```

优势：

- 减少内存分配开销。
- 降低碎片。
- 高吞吐更稳定。
- 容易统计 inflight。

### 7.5 USB 错误处理

需要分类处理 URB status：

| status | 含义 | 处理 |
|---|---|---|
| 0 | 成功 | 正常处理 |
| -ENOENT | urb 被 unlink | remove/suspend 时忽略 |
| -ECONNRESET | urb killed | remove/suspend 时忽略 |
| -ESHUTDOWN | 设备断开 | 标记 surprise_removed |
| -EPROTO | USB 协议错误 | 计数，触发恢复 |
| -EPIPE | endpoint halt | clear halt + reset |
| -ETIMEDOUT | 超时 | 触发 HCI 复位 |
| -ENOMEM | 内存不足 | 降速、延迟重试 |

示例：

```c
static void aic_rx_complete(struct urb *urb)
{
    struct aic_rx_ctx *ctx = urb->context;
    struct aic_dev *adev = ctx->adev;

    usb_unanchor_urb(urb);

    if (adev->removing || adev->surprise_removed)
        return;

    switch (urb->status) {
    case 0:
        aic_rx_handle(adev, urb->transfer_buffer, urb->actual_length);
        break;
    case -ENOENT:
    case -ECONNRESET:
    case -ESHUTDOWN:
        return;
    case -EPIPE:
        aic_schedule_usb_recovery(adev, "rx epipe");
        return;
    default:
        aic_stats_inc(&adev->stats.rx_urb_error);
        break;
    }

    aic_usb_submit_rx_urb(adev, ctx);
}
```

---

## 8. Firmware 加载架构

### 8.1 Firmware 目录设计

建议 firmware 存放：

```text
/lib/firmware/aic8800/
├── manifest.json
├── aic8800d80/
│   ├── fw_patch.bin
│   ├── wifi_fw.bin
│   ├── rf_config.bin
│   └── cali_config.bin
├── aic8800dc/
│   ├── fw_patch.bin
│   ├── wifi_fw.bin
│   └── rf_config.bin
└── common/
    └── country_power_table.bin
```

### 8.2 Firmware manifest

需要引入 manifest，解决“驱动和 firmware 不匹配导致系统卡死”的问题。

```json
{
  "chip": "AIC8800D80",
  "vendor_id": "0x2357",
  "product_id": "0x014e",
  "driver_abi": "1.2",
  "firmware_version": "2026.06.30",
  "min_kernel": "5.10",
  "sha256": {
    "wifi_fw.bin": "xxxx",
    "rf_config.bin": "xxxx"
  }
}
```

### 8.3 加载流程

```text
aic_fw_load()
    |
    +-- identify chip id
    +-- identify USB VID/PID
    +-- select firmware directory
    +-- read manifest
    +-- verify driver ABI
    +-- verify sha256
    +-- download patch
    +-- download WiFi firmware
    +-- download RF config
    +-- wait firmware ready event
    +-- query firmware version
    +-- mark fw_ready
```

### 8.4 失败处理

firmware 加载失败不能继续注册 WiFi 设备。

```text
firmware load failed
    -> stop all URBs
    -> release USB resources
    -> return probe error
    -> dmesg 明确提示 firmware 缺失/版本不匹配
```

错误日志必须清晰：

```text
aic8800: firmware version mismatch: driver ABI=1.2, fw ABI=1.0
aic8800: please install /lib/firmware/aic8800/aic8800d80/2026.06 package
```

---

## 9. cfg80211 层设计

### 9.1 为什么选择 cfg80211

推荐实现 cfg80211_ops，原因：

- 兼容 NetworkManager。
- 兼容 wpa_supplicant。
- 兼容 iw / nl80211。
- 现代 Linux WiFi 驱动都需要通过 cfg80211 暴露标准接口。
- 避免私有 ioctl 过多导致维护困难。

### 9.2 关键接口

```c
static struct cfg80211_ops aic_cfg80211_ops = {
    .scan               = aic_cfg80211_scan,
    .connect            = aic_cfg80211_connect,
    .disconnect         = aic_cfg80211_disconnect,
    .add_key            = aic_cfg80211_add_key,
    .del_key            = aic_cfg80211_del_key,
    .set_default_key    = aic_cfg80211_set_default_key,
    .set_power_mgmt     = aic_cfg80211_set_power_mgmt,
    .get_station        = aic_cfg80211_get_station,
    .change_virtual_intf= aic_cfg80211_change_iface,
};
```

### 9.3 扫描流程

```text
用户空间 iw/NetworkManager 发起 scan
    |
    v
cfg80211_ops.scan()
    |
    +-- 检查状态
    +-- 检查是否已有 scan in progress
    +-- 发送 firmware scan command
    +-- 启动 scan timeout delayed_work
    |
firmware 返回 scan result event
    |
    +-- cfg80211_inform_bss_data()
    |
firmware 返回 scan complete event
    |
    +-- cfg80211_scan_done()
```

必须设置扫描超时：

```text
SCAN_TIMEOUT = 8s / 10s
```

否则 firmware 没返回事件时，用户空间会一直认为正在扫描。

### 9.4 连接流程

```text
connect request
    |
    +-- stop scan if scanning
    +-- validate SSID/BSSID/auth/cipher
    +-- send connect command to firmware
    +-- start connect timeout
    |
firmware connect event
    |
    +-- cfg80211_connect_result()
    +-- netif_carrier_on()
    +-- start link_watch_work
```

### 9.5 断开流程

```text
disconnect request
    |
    +-- send disconnect command
    +-- flush TX queue
    +-- netif_carrier_off()
    +-- cfg80211_disconnected()
    +-- clear connection context
```

### 9.6 状态一致性

驱动必须维护三种状态：

```text
Linux cfg80211 状态
NetworkManager/wpa_supplicant 认知状态
firmware 实际连接状态
```

如果三者不一致，需要由 health_check_work 纠正。

---

## 10. 数据路径设计

### 10.1 TX 路径

```text
ndo_start_xmit()
    |
    +-- check device state
    +-- classify skb priority
    +-- enqueue to AC queue
    +-- if queue high watermark: netif_stop_queue()
    +-- schedule tx_work
        |
        +-- get free tx urb
        +-- build AIC HCI header
        +-- usb_submit_urb()
        +-- tx completion
             |
             +-- free skb
             +-- update stats
             +-- if queue below low watermark: netif_wake_queue()
```

### 10.2 RX 路径

```text
RX URB complete
    |
    +-- validate length
    +-- parse AIC HCI header
    +-- distinguish data/event
    |
    +-- data frame:
    |       +-- create skb
    |       +-- set protocol
    |       +-- checksum hint
    |       +-- push to net stack
    |
    +-- event frame:
            +-- enqueue event
            +-- schedule event_work
```

### 10.3 队列水位控制

推荐参数：

```c
#define AIC_TX_HIGH_WATERMARK 512
#define AIC_TX_LOW_WATERMARK  128
#define AIC_TX_TIMEOUT_MS     5000
#define AIC_RX_URB_NUM        32
#define AIC_TX_URB_NUM        32
```

### 10.4 QoS 队列

按照 802.11e/WMM 映射：

| Linux skb priority | WMM AC |
|---|---|
| 0,3 | BE |
| 1,2 | BK |
| 4,5 | VI |
| 6,7 | VO |

高优先级队列需要防止饿死低优先级队列：

```text
VO : VI : BE : BK = 4 : 3 : 2 : 1
```

### 10.5 skb 生命周期

必须明确：

```text
TX:
    skb 进入驱动后，如果成功排队，驱动拥有 skb。
    USB completion 后释放 skb。
    出错未排队则立即 dev_kfree_skb_any()。

RX:
    URB buffer 不直接交给协议栈长期持有。
    解析后复制或构造 skb。
    RX skb 交给 netif_rx / napi_gro_receive 后由协议栈拥有。
```

---

## 11. 性能优化设计

### 11.1 URB 批量化

高吞吐关键是减少单包提交开销：

```text
TX 合并：
    多个小包合并到一个 USB bulk transfer

RX 聚合：
    一个 bulk in 包中解析多个 WiFi frame
```

如果 firmware 支持 AMSDU/AMPDU，应优先让 firmware 处理 802.11 聚合，host driver 只处理以太网帧。

### 11.2 减少内存分配

推荐：

- TX URB 池。
- RX buffer 池。
- skb recycle 池，谨慎使用。
- 预分配 command buffer。
- 避免高频 kzalloc/kfree。
- 对小控制命令使用 kmem_cache。

### 11.3 中断与轮询

USB WiFi 不一定像 PCIe NIC 一样使用硬中断，但仍然会有大量 URB completion。高负载下建议：

```text
URB completion:
    只做最小处理
    将 skb/event 放入队列
    schedule NAPI / workqueue
```

不要在 complete callback 中做复杂解析、连接状态切换、长时间持锁。

### 11.4 CPU 亲和性

在 RK3568/RK3588 上建议：

```text
USB IRQ / kworker / WiFi workqueue 避免全部挤在 CPU0
```

可提供调试脚本：

```bash
cat /proc/interrupts | grep -i xhci
ps -eLo pid,tid,psr,comm | grep aic
```

### 11.5 低延迟模式和省电模式分离

驱动参数：

```text
aic8800.low_latency=1
aic8800.power_save=0
aic8800.rx_urb_num=32
aic8800.tx_urb_num=32
```

医疗设备、工业设备推荐默认：

```text
low_latency=1
power_save=0
usb_autosuspend=off
```

消费设备推荐：

```text
low_latency=0
power_save=1
usb_autosuspend=on
```

---

## 12. 高稳定性设计

### 12.1 生命周期管理

必须严格处理：

```text
probe
open
start_xmit
stop
suspend
resume
disconnect
remove
module_exit
```

核心原则：

```text
remove/disconnect 先标记 removing
再停止 netif queue
再取消 delayed_work
再 kill anchored urbs
再 unregister wiphy/netdev
最后 free 私有结构
```

### 12.2 disconnect 安全流程

```c
static void aic_usb_disconnect(struct usb_interface *intf)
{
    struct aic_dev *adev = usb_get_intfdata(intf);

    usb_set_intfdata(intf, NULL);

    if (!adev)
        return;

    mutex_lock(&adev->op_mutex);

    adev->removing = true;
    adev->surprise_removed = true;
    aic_state_set(adev, AIC_STATE_REMOVING);

    netif_carrier_off(adev->ndev);
    netif_stop_queue(adev->ndev);

    cancel_delayed_work_sync(&adev->health_check_work);
    cancel_delayed_work_sync(&adev->link_watch_work);
    cancel_work_sync(&adev->event_work);
    cancel_work_sync(&adev->recovery_work);

    usb_kill_anchored_urbs(&adev->usb.rx_anchor);
    usb_kill_anchored_urbs(&adev->usb.tx_anchor);
    usb_kill_anchored_urbs(&adev->usb.ctrl_anchor);

    aic_cfg80211_unregister(adev);
    aic_free_resources(adev);

    mutex_unlock(&adev->op_mutex);
}
```

### 12.3 TX timeout 恢复

实现 `ndo_tx_timeout`：

```c
static void aic_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
    struct aic_dev *adev = netdev_priv(ndev);

    aic_stats_inc(&adev->stats.tx_timeout);
    aic_schedule_recovery(adev, AIC_RECOVERY_TX_TIMEOUT);
}
```

### 12.4 Firmware watchdog

定时发送 heartbeat：

```text
每 5 秒查询 firmware alive counter
如果连续 3 次无响应：
    -> 触发 firmware reset
    -> 重新下载 firmware
    -> 重新注册/恢复 WiFi 状态
```

### 12.5 链路健康检查

不仅检查 `iw dev wlan0 link`，还要检查数据链路：

```text
link state == connected
    + carrier on
    + firmware reports associated
    + rx/tx counters moving
    + DHCP/IP 存在
```

驱动层只能检查前三个，IP 层建议交给系统服务检查。

---

## 13. 自愈架构

### 13.1 自愈目标

当出现以下情况时，驱动自动恢复：

- URB 连续错误。
- TX timeout。
- firmware heartbeat 超时。
- scan 卡住。
- connect 卡住。
- USB endpoint halt。
- suspend/resume 后设备无响应。
- firmware event 异常。
- cfg80211 状态和 firmware 状态不一致。

### 13.2 恢复等级

```text
Level 0: 忽略单次错误，只计数
Level 1: 重启 TX/RX 队列
Level 2: clear halt + 重提 URB
Level 3: firmware soft reset
Level 4: USB device reset
Level 5: 驱动 remove/probe 级别重初始化
Level 6: 上报用户空间，需要人工处理
```

### 13.3 恢复状态机

```text
正常运行
    |
    v
检测到异常
    |
    v
RECOVERING
    |
    +-- stop queues
    +-- kill URBs
    +-- flush command queue
    +-- reset firmware/HCI
    +-- restart RX URBs
    +-- wake queues
    +-- notify cfg80211 if disconnected
    |
    v
恢复成功 -> RUNNING
恢复失败 -> DEAD / retry later
```

### 13.4 防止恢复风暴

必须限频：

```text
1 分钟内最多恢复 3 次
10 分钟内最多 USB reset 2 次
超过阈值后停止自动恢复，进入 degraded 状态
```

---

## 14. 电源管理设计

### 14.1 Runtime PM

USB WiFi 在嵌入式设备上容易因为 autosuspend 导致假死，推荐默认保守策略：

```text
医疗设备 / 工业设备：
    runtime PM 默认关闭
    USB autosuspend 默认关闭

普通消费设备：
    runtime PM 可开启
```

### 14.2 suspend 流程

```text
aic_suspend()
    |
    +-- mark SUSPENDING
    +-- stop netif queue
    +-- pause scan/connect
    +-- send firmware sleep command
    +-- kill RX/TX URBs
    +-- mark SUSPENDED
```

### 14.3 resume 流程

```text
aic_resume()
    |
    +-- mark RESUMING
    +-- resume USB interface
    +-- check firmware alive
    +-- if alive: restart URBs
    +-- if not alive: reload firmware
    +-- restore WiFi state
    +-- mark RUNNING
```

### 14.4 USB autosuspend 参数

模块参数：

```c
static bool disable_usb_autosuspend = true;
module_param(disable_usb_autosuspend, bool, 0644);
```

probe 时：

```c
if (disable_usb_autosuspend)
    usb_disable_autosuspend(udev);
```

---

## 15. 日志与诊断设计

### 15.1 日志等级

```text
AIC_LOG_ERR
AIC_LOG_WARN
AIC_LOG_INFO
AIC_LOG_DEBUG
AIC_LOG_TRACE
```

模块参数：

```text
aic8800.log_level=3
aic8800.trace_mask=0x0
```

### 15.2 日志格式

统一格式：

```text
aic8800 wlan0: [USB] rx urb error status=-71 count=5
aic8800 wlan0: [FW] heartbeat timeout 3/3, schedule recovery
aic8800 wlan0: [CFG] connect result ssid=xxx status=0
aic8800 wlan0: [RECOVERY] level=3 reason=tx_timeout
```

### 15.3 debugfs

推荐 debugfs 路径：

```text
/sys/kernel/debug/aic8800/wlan0/
├── state
├── stats
├── fw_version
├── usb
├── txq
├── rxq
├── recovery
├── last_events
├── trigger_recovery
└── log_level
```

### 15.4 stats 示例

```text
state: CONNECTED
fw_ready: 1
usb_online: 1
tx_packets: 123456
rx_packets: 234567
tx_errors: 3
rx_errors: 2
tx_timeout: 0
urb_rx_errors: 1
urb_tx_errors: 0
fw_heartbeat_timeout: 0
recovery_count: 1
last_recovery_reason: rx_epipe
```

---

## 16. 内核版本兼容层

由于 AIC8800 驱动经常需要适配 5.10、5.15、6.1、6.6、6.12+，建议集中管理兼容差异。

### 16.1 aic_compat.h

```c
#ifndef __AIC_COMPAT_H__
#define __AIC_COMPAT_H__

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#define AIC_CFG80211_SCAN_DONE(wiphy, req, aborted) \
    cfg80211_scan_done(req, aborted)
#else
#define AIC_CFG80211_SCAN_DONE(wiphy, req, aborted) \
    do { struct cfg80211_scan_info info = { .aborted = aborted }; \
         cfg80211_scan_done(req, &info); } while (0)
#endif

#endif
```

所有版本差异都放在 compat 层，业务代码不要散落大量 `#if LINUX_VERSION_CODE`。

---

## 17. 打包和部署架构

### 17.1 DKMS

适合 Ubuntu / Debian 现场维护：

```text
/usr/src/aic8800-1.0.0/
├── dkms.conf
├── Makefile
├── src/
└── firmware/
```

dkms.conf：

```text
PACKAGE_NAME="aic8800"
PACKAGE_VERSION="1.0.0"
BUILT_MODULE_NAME[0]="aic8800_fdrv"
DEST_MODULE_LOCATION[0]="/kernel/drivers/net/wireless/aic8800"
AUTOINSTALL="yes"
```

### 17.2 Yocto

推荐 recipe：

```text
recipes-kernel/aic8800/
├── aic8800_git.bb
├── files/
│   ├── aic8800.conf
│   ├── aic8800.rules
│   └── firmware/
```

关键点：

```bitbake
inherit module

SRC_URI = "git://xxx/aic8800.git;protocol=https;branch=main"
S = "${WORKDIR}/git"

RPROVIDES:${PN} += "kernel-module-aic8800-fdrv"
FILES:${PN} += "${nonarch_base_libdir}/firmware/aic8800"
```

### 17.3 Buildroot

推荐 package：

```text
package/aic8800/
├── Config.in
├── aic8800.mk
└── firmware/
```

### 17.4 systemd 辅助服务

可以提供一个辅助服务做链路自愈，但驱动本身仍要具备恢复能力。

```ini
[Unit]
Description=AIC8800 WiFi Health Monitor
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/aic8800-healthd
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

---

## 18. udev 规则设计

### 18.1 USB modeswitch

部分 AIC8800 USB 网卡可能需要 usb_modeswitch 切换模式。udev 示例：

```text
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="2357", ATTR{idProduct}=="014e", RUN+="/sbin/modprobe aic8800_fdrv"
```

如果有 storage mode，需要：

```text
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="a69c", ATTR{idProduct}=="5723", RUN+="/usr/sbin/usb_modeswitch -KQ -v a69c -p 5721"
```

实际 VID/PID 必须以 `lsusb` 为准。

### 18.2 模块加载顺序

建议：

```text
softdep aic8800_fdrv pre: cfg80211
```

如果保留 aic_load_fw：

```text
softdep aic8800_fdrv pre: aic_load_fw cfg80211
```

---

## 19. 安全设计

### 19.1 输入校验

firmware event、USB RX buffer、私有命令都必须校验长度：

```c
if (len < sizeof(struct aic_hci_hdr))
    return -EINVAL;

if (hdr->payload_len > len - sizeof(*hdr))
    return -EINVAL;
```

### 19.2 防止越界

禁止直接信任 firmware 返回：

```text
event id
payload length
station id
queue id
channel id
rate index
```

所有 index 都要检查范围。

### 19.3 ioctl 控制面

尽量避免新增私有 ioctl。如果必须有：

- 只允许 CAP_NET_ADMIN。
- 严格 copy_from_user 长度检查。
- 不允许任意寄存器读写默认开放。
- debug 功能默认关闭。

---

## 20. 高稳定性锁设计

### 20.1 锁分类

| 锁 | 用途 |
|---|---|
| op_mutex | open/stop/scan/connect/recovery 大操作串行化 |
| state_lock | 状态机短临界区 |
| txq.lock | TX 队列 |
| rxq.lock | RX 队列 |
| cmd.lock | 命令队列 |
| stats.seq | 统计读取一致性 |

### 20.2 锁顺序

必须固定：

```text
op_mutex
  -> state_lock
      -> txq.lock
      -> rxq.lock
      -> cmd.lock
```

禁止反向加锁。

### 20.3 completion callback 中禁止做的事

URB complete 中不要：

- 长时间持 mutex。
- 等待 firmware 命令完成。
- 调用可能睡眠的函数。
- 同步 unregister netdev。
- 递归触发 recovery。

---

## 21. 网络管理兼容设计

### 21.1 NetworkManager 兼容

需要保证：

```text
wiphy 正确注册
interface type 正确
scan_done 必须调用
connect_result 必须调用
disconnect event 必须上报
carrier 状态正确
```

否则会出现：

```text
iw dev wlan0 link 显示 connected
nmcli dev status 显示 unavailable/disconnected
```

### 21.2 wpa_supplicant 兼容

wpa_supplicant 依赖 nl80211 事件。需要保证：

```text
认证失败 -> cfg80211_connect_result(status != 0)
AP 断开 -> cfg80211_disconnected()
扫描失败 -> cfg80211_scan_done(aborted = true)
```

### 21.3 避免手动 wpa_supplicant 和 NetworkManager 冲突

产品系统中应明确：

```text
方案 A：完全交给 NetworkManager
方案 B：完全交给 wpa_supplicant + dhclient
```

不要同时两个服务抢 wlan0。

---

## 22. 测试验证体系

### 22.1 编译测试

目标内核：

```text
5.10.160  RK3568/RK3588 BSP
5.15 LTS   Ubuntu 22.04
6.1 LTS    Debian 12
6.6 LTS    新版嵌入式
6.12+      新发行版验证
```

测试：

```bash
make clean
make
modinfo aic8800_fdrv.ko
modprobe aic8800_fdrv
dmesg | grep -i aic
```

### 22.2 基础功能测试

```bash
lsusb
lsmod | grep aic
ip link
iw dev
iw dev wlan0 scan
nmcli dev wifi list
nmcli dev wifi connect "SSID" password "PASSWORD"
iw dev wlan0 link
ping -c 20 8.8.8.8
```

### 22.3 吞吐测试

AP 端：

```bash
iperf3 -s
```

设备端：

```bash
iperf3 -c <AP_IP> -t 300
iperf3 -c <AP_IP> -t 300 -R
iperf3 -c <AP_IP> -t 300 -P 4
```

记录：

```text
平均吞吐
P95 抖动
重传
CPU 占用
驱动错误计数
dmesg 错误
```

### 22.4 长稳测试

```bash
while true; do
    date
    iw dev wlan0 link
    ping -c 10 gateway
    iperf3 -c server -t 60
    sleep 30
done
```

至少运行：

```text
24 小时基础
72 小时稳定
168 小时量产前验证
```

### 22.5 热插拔测试

```bash
for i in $(seq 1 100); do
    echo "Round $i"
    # 人工或 USB relay 断开
    sleep 5
    # 人工或 USB relay 插入
    sleep 10
    iw dev
    dmesg | tail -n 50
done
```

### 22.6 suspend/resume 测试

```bash
for i in $(seq 1 100); do
    echo mem | sudo tee /sys/power/state
    sleep 10
    iw dev wlan0 link
    ping -c 5 gateway
done
```

### 22.7 异常注入测试

建议覆盖：

```text
firmware 缺失
firmware 版本错误
USB 拔出
USB endpoint halt
AP 断电
弱信号
DHCP 失败
高吞吐时拔出
scan 过程中拔出
connect 过程中拔出
suspend 中拔出
```

---

## 23. 量产配置建议

### 23.1 医疗设备 / 工业设备推荐参数

```text
power_save=0
disable_usb_autosuspend=1
rx_urb_num=32
tx_urb_num=32
log_level=2
recovery_enable=1
firmware_verify=1
```

### 23.2 普通消费设备推荐参数

```text
power_save=1
disable_usb_autosuspend=0
rx_urb_num=16
tx_urb_num=16
log_level=1
recovery_enable=1
firmware_verify=1
```

### 23.3 `/etc/modprobe.d/aic8800.conf`

```text
options aic8800_fdrv power_save=0 disable_usb_autosuspend=1 rx_urb_num=32 tx_urb_num=32 recovery_enable=1
softdep aic8800_fdrv pre: cfg80211 aic_load_fw
```

---

## 24. 推荐开发路线

### Phase 1：现有驱动稳定化

目标：解决能用、稳定、可诊断。

任务：

- 整理模块加载顺序。
- 固定 firmware 版本。
- 增加 firmware 校验。
- 修复 disconnect/remove race。
- 增加 URB anchor。
- 增加 TX timeout recovery。
- 增加 debugfs stats。
- 增加 scan/connect timeout。
- 增加 suspend/resume 基础支持。

### Phase 2：数据路径性能优化

目标：降低 CPU，提高吞吐。

任务：

- TX/RX URB 池。
- RX 预提交。
- TX 批量提交。
- skb 生命周期梳理。
- workqueue/NAPI-like 处理。
- 队列水位控制。
- QoS 队列。
- 高负载压测。

### Phase 3：自愈和产品化

目标：现场问题自动恢复。

任务：

- firmware heartbeat。
- link health check。
- recovery level。
- 防恢复风暴。
- systemd healthd。
- 日志持久化。
- 现场诊断包导出。

### Phase 4：主线化/长期维护

目标：可长期跟随内核演进。

任务：

- 统一 compat 层。
- 消除私有 ioctl。
- 标准 cfg80211 行为。
- 代码风格 kernel checkpatch。
- CI 编译多内核。
- DKMS / Yocto / Buildroot 同步维护。
- 评估是否有条件走 mac80211。

---

## 25. 验收标准

### 25.1 功能验收

| 项目 | 标准 |
|---|---|
| 模块加载 | 无 warning / oops |
| WiFi 扫描 | 连续 100 次无卡死 |
| WiFi 连接 | 连续 100 次连接/断开成功 |
| DHCP | 自动获取 IP |
| ping | 1000 包无异常中断 |
| iperf3 | 5 分钟双向稳定 |
| 热插拔 | 100 次无 crash |
| suspend/resume | 100 次无假死 |
| 弱信号 | 不崩溃，可恢复 |
| AP 断电 | 可重新连接 |

### 25.2 稳定性验收

```text
72 小时连续运行：
    - 无 kernel panic
    - 无 use-after-free
    - 无 slab 泄漏
    - 无不可恢复断网
    - recovery 次数可解释
```

### 25.3 性能验收

```text
5G AP，近距离：
    TCP download >= 150 Mbps
    TCP upload >= 100 Mbps

2.4G AP，近距离：
    TCP download >= 50 Mbps
    TCP upload >= 30 Mbps
```

实际指标受模块、天线、AP、信道、USB 控制器影响，需要按产品环境确认。

---

## 26. 现场诊断命令

### 26.1 基础信息

```bash
uname -a
lsusb
lsmod | grep -E "aic|cfg80211"
modinfo aic8800_fdrv
ip link
iw dev
iw dev wlan0 link
nmcli dev status
```

### 26.2 日志

```bash
dmesg -T | grep -iE "aic|wlan|usb|cfg80211|firmware"
journalctl -k -b | grep -iE "aic|wlan|usb|firmware"
```

### 26.3 USB 状态

```bash
lsusb -t
cat /sys/kernel/debug/usb/devices 2>/dev/null
cat /sys/bus/usb/devices/*/power/control 2>/dev/null
```

### 26.4 WiFi 状态

```bash
iw dev wlan0 info
iw dev wlan0 link
iw dev wlan0 station dump
iw reg get
nmcli dev wifi list
```

### 26.5 驱动 debugfs

```bash
cat /sys/kernel/debug/aic8800/wlan0/state
cat /sys/kernel/debug/aic8800/wlan0/stats
cat /sys/kernel/debug/aic8800/wlan0/fw_version
cat /sys/kernel/debug/aic8800/wlan0/recovery
```

---

## 27. 推荐健康监控脚本

这是用户空间辅助监控，不替代驱动内部 recovery。

```bash
#!/bin/bash

IFACE=wlan0
GW=$(ip route | awk '/default/ && $5=="wlan0" {print $3; exit}')

LOG_TAG="[aic8800-healthd]"

while true; do
    if ! ip link show "$IFACE" >/dev/null 2>&1; then
        logger "$LOG_TAG $IFACE not found"
        sleep 5
        continue
    fi

    if ! iw dev "$IFACE" link | grep -q "Connected"; then
        logger "$LOG_TAG $IFACE not connected"
        nmcli dev connect "$IFACE" >/dev/null 2>&1 || true
        sleep 5
        continue
    fi

    if [ -n "$GW" ]; then
        if ! ping -I "$IFACE" -c 3 -W 2 "$GW" >/dev/null 2>&1; then
            logger "$LOG_TAG link seems dead, reconnect"
            nmcli dev disconnect "$IFACE" >/dev/null 2>&1 || true
            sleep 2
            nmcli dev connect "$IFACE" >/dev/null 2>&1 || true
        fi
    fi

    sleep 10
done
```

---

## 28. 风险点

### 28.1 最大风险：firmware 不开放

如果 firmware 协议不公开，驱动只能围绕厂商接口做稳定化，不能完全自主实现 MAC 层。

### 28.2 第二风险：内核 API 频繁变化

out-of-tree WiFi 驱动在新内核上经常因为 cfg80211、netdev、timer、workqueue API 变化而编译失败。必须建立 CI 编译矩阵。

### 28.3 第三风险：USB 控制器兼容性

不同平台 xHCI/EHCI/USB PHY 的稳定性不同。RK 平台还要关注：

```text
USB PHY 供电
VBUS
OTG/host 模式
autosuspend
hub 质量
线材质量
EMI
```

### 28.4 第四风险：NetworkManager 状态不同步

如果 cfg80211 事件上报不完整，就会出现：

```text
iw 显示 connected
nmcli 显示 unavailable
系统无法自动重连
```

---

## 29. 推荐最终形态

面向工业级产品，最终建议形成：

```text
内核态：
    aic8800_fdrv.ko
        - USB HCI
        - firmware loader
        - cfg80211
        - TX/RX
        - recovery
        - debugfs

用户态：
    aic8800-healthd
        - IP 层健康监控
        - NetworkManager/wpa_supplicant 协调
        - 日志采集

构建：
    DKMS
    Yocto recipe
    Buildroot package
    Debian package

验证：
    CI 编译矩阵
    iperf3 自动化
    热插拔测试
    suspend/resume 测试
    长稳测试
```

---

## 30. 一句话总结

AIC8800 USB WiFi 驱动要做成工业级，不是简单地让 `aic8800_fdrv.ko` 能加载，而是要围绕：

```text
USB URB 生命周期
firmware 版本匹配
cfg80211 状态一致性
TX/RX 高性能队列
异常自愈
热插拔安全
suspend/resume 安全
可诊断日志
多内核构建
```

建立完整工程体系。

---

## 31. 参考资料

1. Linux Kernel Documentation - cfg80211 subsystem  
   https://www.kernel.org/doc/html/latest/driver-api/80211/cfg80211.html

2. Linux Wireless Documentation - cfg80211  
   https://wireless.docs.kernel.org/en/latest/en/developers/documentation/cfg80211.html

3. Linux Wireless Documentation - mac80211  
   https://wireless.docs.kernel.org/en/latest/en/developers/documentation/mac80211.html

4. Linux Kernel Documentation - USB Anchors  
   https://docs.kernel.org/driver-api/usb/anchors.html

5. Linux Kernel Documentation - USB Host-Side API  
   https://www.kernel.org/doc/html/latest/driver-api/usb/index.html

6. Linux Kernel Documentation - NAPI  
   https://docs.kernel.org/networking/napi.html

7. Linux Kernel Documentation - Device Power Management  
   https://docs.kernel.org/driver-api/pm/devices.html

8. radxa-pkg/aic8800  
   https://github.com/radxa-pkg/aic8800

9. shenmintao/aic8800d80  
   https://github.com/shenmintao/aic8800d80

10. Gentoo net-wireless/aic8800 package metadata  
    https://packages.gentoo.org/packages/net-wireless/aic8800
