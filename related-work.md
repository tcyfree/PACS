# MQSim 相关工作配置

设备参数位于 `ssdconfig.xml` 的 `Device_Parameter_Set`。每次可启用一种写入分配策略。

| 策略 | `PACS_Enabled` | `QSTR_MED_Enabled` | `LEPA_Enabled` |
| --- | --- | --- | --- |
| Baseline | false | false | false |
| PACS | true | false | false |
| QSTR-MED（PV Check） | false | true | false |
| LEPA | false | false | true |

在 MQSim 目录下运行。默认工作负载使用时间戳已归一化的 `1612_2`。

```bash
make
mkdir -p 1612_2
./MQSim -i ssdconfig.xml -w workload.xml
```

每组策略可在独立的工作目录运行。MQSim 将 XML 和采样文件写入当前目录，采样子目录以 trace 文件名去掉扩展名后命名。

QSTR-MED 和 LEPA 使用当前的 `PAGE_LEVEL`、`CWDP` 和 `PRIORITY_OUT_OF_ORDER` 配置。

## QSTR-MED

主机写入和 GC 迁移各使用自己的块组。策略按块的字线编程时延总和，在每个 plane 的空闲块池中取最快或最慢的四个候选。它选出全组最快或最慢的参考块，再用字线签名的 Hamming 距离从每个其余 plane 选出成员。每层按四个连续逻辑 WL 构建签名，最快两项记为 0。块编程完成后更新签名，模拟器为未映射的读请求预置的页也按其配置时延记入。主机写使用快组，GC 迁移使用慢组。预处理先填充初始数据，前沿块续配时启用 QSTR-MED。MQSim 逐平面推进写入和 GC，先填满的 plane 可补配一个本地块。

在 `Flash_Parameter_Set` 中加入 `Block_Latency_Profile_File` 可以给单个物理块设定 NAND 时延。文件每行用空白分隔，地址从 0 开始，时延单位为 ns。行末可以用 `#` 引入注释。

```text
channel chip die plane block tBERS tPROG_WL0 tPROG_WL1 ... tPROG_WL255
```

每行的 tPROG 数量与 `Wordline_Program_Latencies` 相同。当前 SOML Read 配置每块有 256 条模拟 WL。缺少逐块数据的物理块使用共用 WL 表和 `Block_Erase_Latency`。相同 profile 文件可输入各策略以对齐 NAND 时延。

结果 XML 内的 `QSTR_MED` 记录快组、慢组、实际分配完毕的块组和签名统计。两个 `Extra_Superblock_*` 计数是按已组装块组计算的时延差总和。编程对每个 WL 取组内最大值减最小值再求和，擦除取组内最大值减最小值。

[Tseng 等，HPCA 2024，§V](https://doi.org/10.1109/HPCA57654.2024.00055)

## LEPA

LEPA 以平均 NAND 读时延为单位，按已分配且尚未完成的读、写、擦除事务估计 die 负载。它先按 CWD 找静态目标，在达到一个编程时延的负载差时选择负载最低的可用 die，再按该 die 的写 plane 指针选 plane。

启用 `Independent_Wordline_Multiplane` 后，使用论文式 (2) 至 (5) 的按 plane 取最大值和未完成写条带估算。调度器可把同一 die 上不同 plane 的就绪事务合并成多平面命令。SOML Read 的原调度设置为 false。该设置下 LEPA 按单平面操作累加读、写、擦除负载。

结果 XML 内的 `LEPA` 记录时延比、改址次数和回放结束时的未完成事务计数。

[Wu 等，IEICE Electronics Express 2025，§3](https://doi.org/10.1587/elex.22.20250435)
