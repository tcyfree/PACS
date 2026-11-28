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

## 请求级 PACS

在 `Device_Parameter_Set` 中开启以下选项可使用跨通道的混合分配。

```xml
<PACS_Enabled>true</PACS_Enabled>
<PACS_Request_Level_Enabled>true</PACS_Request_Level_Enabled>
<PACS_Cross_Channel_Enabled>true</PACS_Cross_Channel_Enabled>
```

每笔跨页写先生成同通道放置方案。对该方案中预计最晚完成的一页，尝试将它放到其他通道的可写候选上，再用同一个模型估算整笔请求的完工时间。只有整笔请求预计缩短的时间达到该页在同通道方案中的预计编程时延，才采用跨通道方案。每笔请求最多跨通道重定向一页。

`PACS_Cross_Channel_Enabled` 默认为 `false`。启用时，结果 XML 中的 `Cross_Channel_Multipage_Write_Requests` 记录实际提交了跨通道选址的请求数。

### 自适应关键页

在混合分配的配置上加上这三项可运行自适应模式。搜索预算表示每笔请求最多预测的跨通道组合数。

```xml
<PACS_Cross_Channel_Adaptive_Enabled>true</PACS_Cross_Channel_Adaptive_Enabled>
<PACS_Cross_Channel_Gain_Beta>1</PACS_Cross_Channel_Gain_Beta>
<PACS_Cross_Channel_Search_Budget>256</PACS_Cross_Channel_Search_Budget>
```

单页写和跨页写都使用请求级预测。先在原通道搜索各个可写 Plane，预计整笔请求能提前完成时才调整地址。以 β 乘以同通道方案中最晚完成页的预计编程时延作为最低收益门槛 θ，把超过目标时间 `T_local - θ` 的页纳入联合搜索。β 为 0 时从并列最晚的页开始搜索。每个关键页可沿用原方案，也可尝试其他通道中各个可写 Plane。整笔请求预计完成时间缩短且收益达到 θ 时才采用跨通道方案。预计完成时间相同时优先选跨通道页数较少的方案。

可用 `PACS_Cross_Channel_Screening_Beta` 为关键页筛选设置独立系数。显式配置后，筛选目标为 `T_local - Screening_Beta × tPROG_critical`，并列最晚的页始终纳入搜索。省略时筛选系数沿用收益门槛的 β。

自适应模式从配置的 `Wordline_Program_Latencies` 取 tPROG，TLC 按预计写入的物理 `PageID / 3` 查表。缺少 WL 表时取配置的页类型编程时延。候选 Plane 使用各自的写前沿，按同请求内的分配顺序推进 PageID。候选页允许不同 TLC 页类型。同一 Die 上的各 Plane 共用 Die 就绪时间。预测逐页累加排队写入各自的 tPROG，并计入在途操作的剩余时间、读擦负载、通道传输和部分页覆盖写的读依赖。结果 XML 中自适应模式的 `Program_Latency_Source` 为 `CONFIGURED`，在线编程样本数为 0。

结果 XML 的 `Adaptive_Search` 记录跨页写的关键页数分布、组合预测次数分布和预算耗尽次数。`Single_Page_Cross_Channel_Writes` 记录单页写的跨通道次数。`Single_Page_Cross_Plane_Writes` 和 `Cross_Plane_Multipage_Write_Pages` 分别记录单页写和跨页写中 Plane ID 相对 CWDP 初始地址发生变化的页数。预测误差取预计时长减去从分配决策到最后一个 NAND program 完成的实际时长。统计样本为按所选地址完成分配的跨页写请求。

### WL 感知消融

在自适应模式中启用 `PACS_Use_Average_Program_Latency` 可运行 PACS-NoWL。等待的编程、在途编程的预计剩余时间、候选页编程和收益门槛都使用统一的平均 tPROG。均值取设备时延表，NAND 仍按物理 WL 执行。默认使用 PACS-Full 的逐 WL 预测。

启用 `PACS_WL_Decision_Stats` 后，在主策略分配前，对同一资源状态比较 CWDP、PACS-NoWL 和 PACS-Full。两个优化器的物理候选池、β 和搜索预算一致。关键页集合和搜索顺序按各自的预测时间计算。以 Plane 和该 Plane 内的写入序号比较页的目标位置，可识别同一 Plane 上因请求内前序页改址而改变的目标页。结果 XML 的 `WL_Decision_Stats` 分别记录单页和跨页请求、与实际选址一致的计划数、五类选址结果、改变页数分布和同 Die 改 Plane 数。

候选差值按 CWDP 的请求内前序分配推进写前沿。每页在所有可写的候选地址上计算最大值减最小值。编程差来自候选物理页的逐 WL tPROG，负载差来自候选 Channel 和 Die 就绪时间两者的较晚值。两种差值均用上界包含的直方图区间。已评估的跨通道方案有正收益且收益低于门槛时计为 `Rejected_By_Beta`。整笔请求的反事实收益用同一逐 WL 模型重估三种方案的完工时间。对单页改址进一步拆分编程时间差和开始编程前的时间差。

### 负载强度实验

负载实验沿用无缓存的 16 KiB TLC 设备和逐 WL NAND 时延表。预处理仅将 trace 第一个字段变换为 `t0 + k × (t − t0)`，其中 `k` 为 1、2、5、10、20 或 50。相同时间戳和其余四列按原顺序复制。每个倍率回放 Baseline、LEPA、WL-aware LEPA、PACS-NoWL 和 PACS-Full。

实验配置启用 `Workload_Intensity_Stats_Enabled`。Die 利用率取 NAND 阵列执行时间占完整回放时间的比例。平均队列长度取 TSU 中等待派发的事务数的时间平均值。写排队时延从提交 TSU 算至派发 NAND 命令。候选 Die 空闲率及负载差在完整主机写请求进入 FTL 的地址分配前采样。各策略使用共同的可写候选 Die 池。空闲条件为 Die 无活跃命令且队列为空，负载差取候选 Die 预计就绪时间的最大值减最小值。跨通道比例以实际置页相对 CWDP 的通道变化计算。WL 决策改变率取 PACS-Full 的同快照对照。

在 MQSim 目录可生成 trace 并回放该批实验。首次回放使用当前已编译的 MQSim，或以 `--simulator` 指定二进制。延迟样本按 gzip 无损保存，脚本用 `pigz` 压缩并校验。

```bash
python3 -B outputs/mqsim-soml-read-wl178-no-cache-intensity/run_experiment.py --jobs 8
```

### 全部 16 条 trace 的长间隔回放

按全量 trace 清单回放 1×、100×、1000× 和 10000×，每条 trace 对比 Baseline、LEPA、WL-aware LEPA、PACS-NoWL 和 PACS-Full。使用上面负载实验的二进制和设备模板。沿用已验证的 20 组 1× 对照，其余组合使用相同的开环回放和统计配置。输出写入 `outputs/mqsim-soml-read-wl178-no-cache-intensity-all-traces`。

```bash
python3 -B outputs/mqsim-soml-read-wl178-no-cache-intensity/run_all_traces.py --jobs 16
python3 -B outputs/mqsim-soml-read-wl178-no-cache-intensity/report_all_traces.py
```

### 七种请求间隔的全量对照

补跑 12 条 trace 在 5×、10× 和 50× 的 180 组，合并同配置归档中的 380 组。完整对照覆盖 16 条 trace、五种策略和 1×、5×、10×、50×、100×、1000×、10000×，共 560 组。图中按同一条 trace 和同一倍率的 Baseline 归一化。数据和图表位于 `outputs/mqsim-soml-read-wl178-no-cache-intensity-seven-factors`。

```bash
python3 -B outputs/mqsim-soml-read-wl178-no-cache-intensity/run_missing_factors.py --jobs 16
python3 -B outputs/mqsim-soml-read-wl178-no-cache-intensity/report_missing_factors.py
```

### 50× 门槛扫描与串行回放

门槛扫描使用 `1612_2`、`src2_2`、`hm_0` 和 `web_0` 的 50× trace。PACS-Full 的关键页筛选系数固定为 1，每笔请求的搜索预算为 256，跨通道接受门槛 β 取 0、0.05、0.1、0.25、0.5 和 1。β=1 的控制组逐样本核对上一轮 50× 实验。

串行回放在单个 trace 流上启用 `<Completion_Driven_Replay>true</Completion_Driven_Replay>`。首笔请求按 trace 的首个时间戳提交，随后在主机收到上一笔完成通知的同一仿真时刻提交下一笔。实验回放 Baseline、LEPA、WL-aware LEPA、PACS-NoWL 和 PACS-Full。两组 PACS 的筛选和接受系数均为 1。每组核对请求总数、最大在途请求数、结束时的未完成数以及完成到下次提交的最大间隔。

在 MQSim 目录执行以下命令，结果汇总至脚本所在目录的 `metrics.json`。

```bash
python3 -B outputs/mqsim-soml-read-wl178-no-cache-beta-serial/run_experiments.py --jobs 8
```

QD1 补充实验在同一设备和相同 trace 序列上回放 PACS-Full，接受系数 β 取 0、0.05、0.1 和 0.25，关键页筛选系数为 1。与上面的 QD1 β=1 和 LEPA 结果对比。结果汇总到 `qd1-beta-sweep/metrics.json`。

```bash
python3 -B outputs/mqsim-soml-read-wl178-no-cache-beta-serial/run_qd1_beta_sweep.py --jobs 8
```

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

### WL-aware LEPA

在单平面配置中启用下面两项，即可按 WL 查表估计编程时延。

```xml
<LEPA_Enabled>true</LEPA_Enabled>
<LEPA_WL_Aware_Enabled>true</LEPA_WL_Aware_Enabled>
```

每个 die 按写 plane 指针取得候选页。估计成本包含按原权重计算的读和擦除负载、所有未完成写各自的 tPROG，以及候选 plane 当前写指针所指页的 tPROG。TLC 页按 `PageID / 3` 查询配置的 `Wordline_Program_Latencies`。当最优目标的估计成本比原目标至少低一次原目标候选页的编程时延时，选择最优目标。

`LEPA_WL_Aware_Enabled` 默认为 `false`。结果 XML 用 `WL_Aware` 标记此模式，并用 `Outstanding_Program_Time_ns` 记录回放结束时的未完成编程负载。

[Wu 等，IEICE Electronics Express 2025，§3](https://doi.org/10.1587/elex.22.20250435)
