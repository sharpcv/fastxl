# fastxl 优化方向与演进路线图

> 适用范围：仅支持 **Linux（x86_64 / aarch64）** 与 **Windows（x64）**，继续以 **xmake** 作为主构建系统。  
> 核心思路：AI 探针（`info` / `preview` / `probe` / `schema` / `search`）的能力上限取决于底层 `fastxl-core` 的解析保真度与并发吞吐，因此采用 **Core 能力升级与探针协议扩展同步推进** 的三阶段路线。

---

## 一、现状基线（v1.0.0）

基于 [sharpcv/fastxl](https://github.com/sharpcv/fastxl) 当前实现（参考 [docs/library.md](https://github.com/sharpcv/fastxl/blob/main/docs/library.md)、[docs/ai.md](https://github.com/sharpcv/fastxl/blob/main/docs/ai.md)、[docs/limitations.md](https://github.com/sharpcv/fastxl/blob/main/docs/limitations.md) 与 [docs/probe.schema.json](https://github.com/sharpcv/fastxl/blob/main/docs/probe.schema.json)）：

* **已有优势**：C++20 核心、SAX + 有界 DOM 混合读取（默认 `64 MiB` 阈值 + `256 KiB` 流式窗口）、`CellView` 零拷贝视图、SSE2 `simd_scan`、自研 `FlatMap`、原子写入、规范化错误码与 JSON Schema 契约、`pybind11` 绑定及 MCP 封装。
* **核心短板**：
  1. **AI 语义层**：缺少 `numFmt`、合并单元格具体坐标、隐藏行、命名区域 / Table / 批注等 Excel 原生锚点；不支持单 Sheet 多表切分与跨 Sheet 检索。
  2. **Core 吞吐与接口层**：`WorkBook` 仅允许单个活跃 `WorkSheetReader`，超过 `64 MiB` 的大表 `profile()` 退化为串行；`WorkBookWriter` 单线程且丢弃公式、锁定 18 种预设样式；缺少 `mmap` 只读映射、AVX2/NEON 及无异常/Ranges 接口。

---

## 二、阶段一：Core 元数据与语义保真（高性价比首选）

本阶段**不改动流式并发架构**，聚焦让 `fastxl-core` 在解析 XML 时不再丢弃关键业务上下文，直接提升 AI 探针对真实业务表的理解准确率。

### 1. `fastxl-core` 底层改动

* **完整解析数字格式与基础异常样式（`xml_part_parsers` / `types.hpp`）**：
  * 从 `xl/styles.xml` 提取 `numFmt` 格式串与基础视觉标记（字体颜色、填充背景色、删除线），挂载到 `CellView` / `SheetStyle` 或按列聚合。
  * 让上层能区分普通小数 `0.15` 与百分比 `15.0%`、货币符号（`¥`/`$`/`S$`）、千/万缩放格式，以及人工标红/删除线的作废行。
* **保留工作表结构元数据（`workbook.cpp` / `sheet_parser`）**：
  * `merged_ranges`：除计数外，保留具体 `A1` 区间列表（如 `A1:D1`、`A3:A10`），支撑多级复合表头与纵向合并分组识别。
  * `hidden_rows` 与 `auto_filter` 条件：记录隐藏行索引及筛选条件值，避免 AI 将隐藏/过滤掉的历史数据纳入统计。
* **提取 Excel 原生语义锚点**：
  * 解析工作簿级 `definedNames`（命名区域）与 `xl/tables/table*.xml`（`ListObject` 结构化表边界与列名）。
  * 解析 `xl/comments*.xml` / 批注摘要，保留人类填写的业务口径说明。
  * 解析透视表（PivotTable）与图表（Chart）的数据源引用范围，在元数据中标注其存在性。
  * 扩展 Data Validation 解析，支持展开同工作簿区域引用（如 `=Dict!$A$2:$A$20`），补全 `enum_values`。

### 2. AI 探针与协议扩展（`docs/probe.schema.json` / `fastxl-cli`）

* `PreviewCell` 增加可选 `num_fmt` 与 `style_flags`（如 `strike`、`has_fill`）。
* `SheetMeta` 增加 `merged_cells`（区间数组）、`hidden_rows`、`tables`、`defined_names`、`comments_summary`、`pivot_tables` 字段。
* 对未缓存公式（`<f>...</f><v/>`）明确标注 `needs_recalc` 提示，避免静默误判为普通空值。

---

## 三、阶段二：并发流式与复杂布局引擎

本阶段重点解决 **Linux / Windows 大文件高吞吐** 与 **非规整复杂表格的结构化识别**。

### 1. `fastxl-core` 底层改动

* **平台层只读内存映射（`src/platform/`）**：
  * 基于现有 `atomic_posix.cpp` / `atomic_win32.cpp` 分层，补齐 Linux `mmap` 与 Windows `CreateFileMapping` / `MapViewOfFile`。
  * 支持直接从 `std::span<const std::byte>` 零拷贝读取 ZIP 与 XML，减少大文件与 `stdin` 临时文件拷贝开销。
* **解除并发限制与大表流水线**：
  * 解除 `WorkBook::open_sheet` 只能存在单个活跃 `WorkSheetReader` 的限制：各线程持有独立 deflate 解压流，只读共享 Shared Strings 表，实现**多 Sheet 真正并发读取**。
  * 优化 `profile()`：对超过 `in_memory_threshold`（`64 MiB`）的大表引入“解压线程 + SIMD 扫描与统计线程”的流水线并行，消除大表退化为串行的问题。
* **SIMD 多架构扩展（`simd_scan.hpp` + `xmake.lua`）**：
  * 在 `xmake.lua` 中按 `is_arch("x86_64")` 与 `is_arch("arm64")` 条件启用 **AVX2**（Linux/Windows x64）与 **NEON**（Linux aarch64）分隔符扫描，保留 SSE2 作为兜底。
* **复杂布局切分与快速检索**：
  * 在 `profile.cpp` 中增加基于空行/空列与合并边界的连通域检测（**Data Islands**），识别单 Sheet 内并存的多张独立子表。
  * 扫描 `<f>` 公式文本提取跨 Sheet 引用拓扑（如 `Sheet2!B2:B50`）与“原始输入列 vs 派生公式列”边界。
  * 新增低开销字符串直扫通道：先扫描 SST 与 Sheet XML 文本命中关键字，命中后再解码具体 `A1` 坐标与类型；优化 `preview --range` 靠后切片时跳过未请求列的值转换。

### 2. AI 探针与协议扩展

* 新增 **`fastxl-cli search <file> --query <pattern>`** 命令：支持跨全工作簿关键词/正则定位，直接返回命中的 `sheet`、`A1` 坐标及周围上下文窗口。
* `schema` / `probe` 支持单 Sheet 多区域（`regions`）独立输出表头与列画像。
* 利用 `distinct` 与 `top_values` 统计，输出跨 Sheet 同名列或值域重合度提示（Join Key 候选）。

---

## 四、阶段三：现代 C++ 易用性、写入增强与 Agent 工程化

### 1. `fastxl-core` C++20 库体验升级

* **无异常错误处理选项（`error.hpp`）**：保留 `fastxl::Exception` 的同时，为关键入口补充 `std::error_code` / `Result<T>` 非异常重载，适配禁用 `-fno-exceptions` 的 C++ 工程。
* **C++20 Ranges 行迭代视图**：在现有 `while (reader.next())` 拉取接口之上封装 Input Range（支持 `for (auto&& row : sheet.rows())` 与 `<ranges>` 管道组合）。
* **`DomWorkBook` 内存上限保护**：为 `DomOptions` 增加 `max_memory_bytes` 默认安全阈值，防止误用导致 OOM。
* **`WorkBookWriter` 并发与能力解锁**：
  * 支持多线程并行将不同 `WorkSheet` 序列化并压缩为独立缓冲块，再由写入线程顺序封包进 ZIP。
  * 放开 18 种预设样式限制，允许注册自定义 `numFmt` 与基础颜色/字体；支持透传写入 `<f>` 公式文本而非直接丢弃。
* **xmake 分发优化**：完善 `xmake.lua` 的安装规则与 `xrepo` 包定义；如需对接非 xmake 宿主工程，可通过 `xmake project -k cmakelists` 或导出预编译静态库+头文件包提供支持。

### 2. AI 工具链常驻化与上下文预算（`projects/tools/fastxl_mcp.py`）

* **MCP Server 常驻化**：将 `fastxl_mcp.py` 从“每次调用 shell out 起 `fastxl-cli` 子进程”重构为直接调用 `pybind11` 编译出的 `fastxl` 模块常驻运行，并缓存最近打开文件的句柄与 SST 表，降低连续探测延迟。
* **Token / 字节预算控制**：为 `preview` 与 `schema` 增加 `--max-bytes` 全局输出预算参数，遇到数百列的超宽表时自动折叠低信息量列，防止撑爆 LLM 上下文窗口。

---

## 五、实施优先级建议

| 优先级 | 所属阶段 | 关键任务 | 核心收益 |
| :--- | :--- | :--- | :--- |
| **P0** | 阶段一 | `merged_ranges` 坐标化、`numFmt` 提取、`hidden_rows`、`definedNames` / `tables` / `comments` | 显著降低 AI 面对真实业务表时的表头与口径误判 |
| **P1** | 阶段二 | 多 Sheet 并发读取、大表流式并行、`search` 原语、单 Sheet 多区域切分 | 解决大工作簿探测慢、盲目 `preview` 和一页多表问题 |
| **P2** | 阶段二/三 | Linux `mmap` / Win32 文件映射、AVX2 + NEON、`fastxl_mcp.py` 常驻化、`--max-bytes` | 榨干硬件吞吐，降低 Agent 多轮调用延迟与 Token 消耗 |
| **P3** | 阶段三 | 无异常 API、C++20 Ranges、`WorkBookWriter` 并行写与公式/自定义格式支持 | 提升外部 C++20 工程集成度与高吞吐导出能力 |
