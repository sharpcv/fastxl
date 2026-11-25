# fastxl M7 深度审计、问题清单与 M8 优化测试方案

> 审计对象：[sharpcv/fastxl](https://github.com/sharpcv/fastxl)（`main` 分支至 commit `7b0f3ef`，对应 M0–M7 完成态）  
> 目标平台：**Linux（x86_64 / aarch64）** 与 **Windows（x64）**，主构建系统：**xmake**  
> 参考文档：[docs/optimization-progress.md](https://github.com/sharpcv/fastxl/blob/main/docs/optimization-progress.md)、[docs/ai.md](https://github.com/sharpcv/fastxl/blob/main/docs/ai.md)、[docs/limitations.md](https://github.com/sharpcv/fastxl/blob/main/docs/limitations.md)、[docs/probe.schema.json](https://github.com/sharpcv/fastxl/blob/main/docs/probe.schema.json)、[xmake.lua](https://github.com/sharpcv/fastxl/blob/main/xmake.lua)、[projects/tools/fastxl_mcp.py](https://github.com/sharpcv/fastxl/blob/main/projects/tools/fastxl_mcp.py)

## 一、总体评价

M0–M7 的推进非常高效，底层 Core 与 AI 探针的关键能力（元数据保真、多 Sheet 并发、`>64 MiB` 大表流式并行、`search`/`--regions`/`join_keys`、`mmap` + AVX2/NEON、`Result<T>`/Ranges、并行写入与公式/自定义样式、`--max-bytes` 预算与 `Session`）均已落地。当前主要问题集中在 **MCP 契约对齐、Windows 常驻文件锁风险、模块分层、真实 Excel 边界处理** 以及 **公开仓库缺失测试与 CI** 五个方面。

## 二、当前核心问题清单（M7 审计）

### 1. MCP 与常驻 `Session` 存在半打通与契约缺陷（高优先）

* **`TOOL_SCHEMAS` 漏暴露参数导致功能不可调用**：[fastxl_mcp.py](https://github.com/sharpcv/fastxl/blob/main/projects/tools/fastxl_mcp.py) 中所有工具均声明了 `additionalProperties: False`，但 `fastxl_schema` 与 `fastxl_probe` 的参数表**遗漏了 `regions`**（尽管 `NATIVE_OPTIONS` 已支持 `regions`），`fastxl_search` 也遗漏了 `max_cell_bytes`。MCP 客户端无法直接开启单 Sheet 多区域（Data Islands）切分。
* **最高频工具 `fastxl_preview` 仍走 CLI 子进程**：`fastxl_preview` 被排除在 `NATIVE_METHODS` 之外，每次调用仍需启动 `fastxl-cli`。在典型 Agent 探查链路（`info -> preview -> schema/search`）中，`preview` 是最热路径，未走 `Session` 会显著削弱常驻缓存的延迟收益。
* **异常回退机制放大业务错误开销**：`native_call()` 捕获任意 `Exception` 后都会回退执行一次 `cli_call()`。对于 `FileNotFound`、`InvalidSheetRef`、`UnsupportedEncryption` 等确定性输入错误，会无意义地重复执行并多起一次子进程。

### 2. Windows 平台 `mmap` + 常驻 `Session` 存在文件锁冲突（高优先）

* M4 默认开启了 `OpenOptions::memory_map`（Windows 侧使用 `CreateFileMapping` / `MapViewOfFile`），M7 的 `fastxl.Session(capacity=8)` 会常驻持有最多 8 个已打开的工作簿句柄。
* 在 Windows 下，只要进程保持文件映射未释放，用户通过 Excel 或其他工具保存/覆盖同一 `.xlsx` 文件时会触发共享锁冲突失败。

### 3. 架构分层倒挂与 Schema 文档漂移（中优先）

* **预算裁剪模块位置倒挂**：`json_budget.hpp` 当前位于 `projects/fastxl-cli/src/json_budget.hpp`，而 Python 绑定与 CLI 共享该实现，造成 `bindings` 反向依赖 `fastxl-cli` 内部源码。
* **Budget 字段命名需核对统一**：[docs/probe.schema.json](https://github.com/sharpcv/fastxl/blob/main/docs/probe.schema.json) 的 `Budget` 定义使用 `omitted_columns`，而 [docs/optimization-progress.md](https://github.com/sharpcv/fastxl/blob/main/docs/optimization-progress.md) 中记载了 `folded_columns`、`dropped_rows`、`dropped_matches`，需确认 CLI、Python 与 Schema 三处完全一致。
* **后置预算仅防 Token 溢出、不防内存尖峰**：`--max-bytes` 在完整 JSON 组装完成后才执行 8 阶段折叠；面对极端超宽表（如 16,384 列全非空），在折叠前仍可能产生较大内存占用。

### 4. 真实企业 Excel 边界盲区（中优先）

* **共享公式（Shared Formulas）**：Excel 常在首行写入 `<f t="shared" ref="C2:C1000" si="0">A2+B2</f>`，后续行仅记录 `<f t="shared" si="0"/>`。若 `sheet_parser` 未按 `si` 追踪主公式，后续单元格会在 `search --field formula` 与 `formula_links` 中被漏检。
* **主题色（Theme Color）与富文本单元格**：真实工作簿中大量颜色通过 `theme` + `tint` 表达而非直接 `rgb`，且共享字符串和批注常包含 `<r><t>` 富文本分段，需确认解析保真度。
* **文档与 `convert` 行为核对**：[docs/ai.md](https://github.com/sharpcv/fastxl/blob/main/docs/ai.md) 仍标注 `convert` 会丢弃公式，需核对 M6 公式透传落地后 `convert` 是否已同步更新或仍需显式开关。
* **Windows 中文环境 CSV 编码**：目前 CSV 仅支持 UTF-8（见 [docs/limitations.md](https://github.com/sharpcv/fastxl/blob/main/docs/limitations.md)），至少应对 UTF-8 BOM 无感兼容，并在遇到 GBK/GB18030 时给出明确诊断信息。

### 5. 公开仓库无测试套件、无基准脚本、无 CI（高优先）

* [xmake.lua](https://github.com/sharpcv/fastxl/blob/main/xmake.lua) 将 `projects/tests` 与 `projects/bench` 排除在公开树之外，且仓库缺少 `.github/workflows/`。
* 直接后果：Linux `aarch64` NEON 路径至今仍标记为“待 CI 验证”，外部使用者无法在本地复现验证 `fastxl-tests`。

## 三、功能完备性盘点（M1–M8）

| 模块 / 里程碑 | 当前状态 | 完备性评估与剩余缺口 |
| :--- | :--- | :--- |
| **语义元数据（M1）** | 已落地 | 覆盖 `numFmt`、颜色、删除线、合并坐标、隐藏行、筛选、Defined Names、Table、批注、透视表/图表、未缓存公式与 DV 枚举；需补验**共享公式**与**主题色**，并核对 `convert` 文档。 |
| **并发与大表吞吐（M2 / M4）** | 已落地 | 多 Sheet 并发、`>64 MiB` 流式并行、`mmap`、AVX2/NEON 分发均已完成；缺 **Linux aarch64 NEON 实测/CI** 与 **TSan/ASan 并发压测**。 |
| **检索与复杂布局（M3）** | 已落地 | `search`、`--regions`、`join_keys` 已打通 Core/CLI/Python；缺 **MCP `regions` 参数暴露**，需补充坏正则异常处理与压测。 |
| **现代 C++20 接口（M5）** | 已落地 | `Result<T>`/`std::error_code`、`WorkSheet::rows()`、DOM `max_memory_bytes` 已补齐；需验证 `-fno-exceptions` 编译兼容性。 |
| **写入与安装（M6）** | 已落地 | 公式透传、自定义样式、`STORE` 零拷贝、并行压缩、temp spill 与 `xmake install` 已完成；保留原图表/透视表的原地局部 patch 可明确列为非目标。 |
| **Agent 工程化（M7）** | 基本落地 | `--max-bytes` 与 `Session` LRU 已上线；需修 **MCP `regions` 漏暴露**、**`fastxl_preview` 未原生化**、**Windows `mmap` 常驻锁文件**。 |
| **测试、基准与 CI（M8）** | 未开始 | 公开树缺失夹具、自动化回归套件、`bench.py` 及 GitHub Actions。 |

## 四、针对性优化与修复建议

| 优先级 | 优化项 | 涉及位置 | 具体改动方案 |
| :--- | :--- | :--- | :--- |
| **P0** | 补齐 MCP Schema 参数并修正回退逻辑 | `projects/tools/fastxl_mcp.py` | 在 `fastxl_schema`/`fastxl_probe` 的 `TOOL_SCHEMAS` 中补上 `regions`，在 `fastxl_search` 中补上 `max_cell_bytes`；捕获 `fastxl.Error` 时直接返回标准 JSON 错误对象，不再重复调用 CLI。 |
| **P0** | 解决 Windows 常驻 `Session` 锁文件问题 | `bindings/fastxl_module.cpp`、`projects/fastxl-core/` | 为 `Session` 打开工作簿透传 `memory_map` 开关，在 Windows 常驻模式下默认关闭 `mmap`（或改为短生命周期映射/释放后重建），避免阻塞 Excel 保存。 |
| **P0** | `fastxl_preview` 接入原生 `Session` | `projects/fastxl-core/`、`bindings/`、`fastxl_mcp.py` | 将 `preview` 的元数据与表头候选组装逻辑统一到 Core/Python 绑定中，使 MCP 的 `fastxl_preview` 与 `info`/`schema`/`probe`/`search` 共享同一常驻 `Session`。 |
| **P1** | 解耦 `json_budget` 分层并对齐 Schema | `projects/fastxl-cli/src/json_budget.hpp`、`docs/probe.schema.json` | 将预算裁剪逻辑下沉到 `fastxl-core`（或独立内部库），统一 `Budget` 字段名与 `probe.schema.json` 定义；在 `profile` 阶段增加极端列数保护。 |
| **P1** | 补齐公开测试套件与多平台 CI | `projects/tests/`、`.github/workflows/`、`xmake.lua` | 将可复现的夹具生成脚本、C++ `fastxl-tests`、Python/MCP `pytest` 与 GitHub Actions（Linux x64、Linux arm64、Windows x64）合入公开仓库。 |
| **P2** | 真实 Excel 边界增强 | `sheet_parser`、`xml_part_parsers`、`table_csv.cpp` | 追踪 `<f t="shared" si="...">` 共享公式；完善 `theme`/`tint` 颜色与富文本 `<r><t>` 解析；确保 CSV 无感处理 UTF-8 BOM 并对非 UTF-8 编码给出清晰报错。 |

## 五、M8 功能性测试矩阵

### 1. Core 解析与边界正确性测试
* **全量元数据夹具（`metadata.xlsx`）**：覆盖多级横向/纵向合并单元格、自定义 `numFmt`（百分比/货币/千分位/日期）、RGB 与 Theme 颜色、删除线、隐藏行/隐藏列、AutoFilter 筛选条件、Defined Names、Excel Table、批注、透视表、图表、跨表 Data Validation、未缓存公式（`<f>` 无 `<v>`）、共享公式（`t="shared"`）与 `date1904=true`，逐项断言字段值与截断标志。
* **畸形与安全边界**：截断 ZIP、非 ZIP 文件、加密 `.xlsx`、bzip2/zstd 压缩条目、非法 UTF-8、越界 SST 索引、触发 `max_cell_bytes` 的超长单元格、触发 `DomOptions::max_memory_bytes` 返回 `LimitExceeded`（防 OOM）。
* **CSV / JSONL 边界**：UTF-8 BOM、空文件、ragged rows、64 位整数边界（`int_min`/`int_max`）、`NaN`/`Inf`。

### 2. 并发、SIMD 与跨平台一致性测试
* **SIMD 后端等价性**：在同一批测试文件上对比 `AVX2` vs `SSE2` vs `scalar`（x86_64）与 `NEON` vs `scalar`（Linux aarch64），断言解析结果逐单元格一致。
* **流式并行 vs 串行确定性**：调小 `in_memory_threshold`（如 `64 KiB`）强制触发大表流式并行，对比 `jobs=1` 与 `jobs=4` 的输出顺序与统计指标（`sum`/`mean`/`distinct`/`top_values`）完全一致。
* **并发安全与文件锁验证**：
  * Linux 开启 **ASan + UBSan + TSan** 跑多线程并发读同一 `WorkBook` 与多线程并行写 `WorkBookWriter`。
  * Windows 验证 `Session` 缓存打开文件后，外部进程可正常覆盖修改该 `.xlsx`，且修改后 `(mtime, size)` 缓存准确失效并读到新内容。
* **`-fno-exceptions` 编译验证**：在 `xmake` 中新增独立验证 target，确保 `Result<T>` / `try_*` API 在禁用异常模式下可正常编译与运行。

### 3. 写入与第三方互操作（Round-trip）测试
* 覆盖 **顺序写、并行写、`STORE` 无压缩、`StringStorage::Inline`、触发 `max_sheet_buffer_bytes` 临时文件 spill、超过 `max_shared_strings_bytes` 回退 inline** 六条路径。
* 写出含公式、自定义 `numFmt`、字体/填充色、删除线、合并单元格、冻结窗格与 AutoFilter 的工作簿后，依次通过 **`fastxl` 回读**、**`openpyxl` 校验** 以及 **Linux CI 下 LibreOffice headless 重算公式** 验证兼容性。

### 4. AI 探针契约与 MCP 端到端测试
* **JSON Schema 强校验**：使用 `jsonschema` 对 `info`、`preview`、`probe`、`schema`、`search`、`errors` 及失败 `error` 共 7 类输出做全量校验；在 `--max-bytes` 触发全部 8 个折叠阶段时，断言 `budget.within_budget == true` 且序列化 UTF-8 字节数 `<= max_bytes`。
* **`search` 与 `--regions` 专项**：验证大小写折叠、正则匹配、坏正则报错、命中公式 vs 缓存值、`--context 0` 与首行/首列边界不越界；验证单 Sheet 多独立数据岛加标题行的切分准确率。
* **MCP Native vs CLI 一致性**：对同一组 JSON-RPC 请求分别运行 `native` 与 `--cli-only` 模式，验证返回结构等价；验证 `Session` 连续调用 `info -> preview -> schema -> probe -> search` 的 `hits` 增长与超过 `capacity=8` 的 LRU 淘汰。

### 5. 性能基准测试（`bench.py`）
* **测试样本**：小表（1k 行）、超宽表（500 列 × 10k 行）、大表（`>64 MiB`，触发流式并行）、多 Sheet 工作簿（20 Sheets）。
* **观测指标**：吞吐（rows/s、MB/s）、首字节延迟、峰值 RSS。
* **对比基线**：同机对比 `fastxl`、`openpyxl`（`read_only=True`）与 `python-calamine`，并单独测量 MCP `native Session` 相对 `CLI` 子进程在多轮探查中的端到端延迟收益。

## 六、推荐收口顺序

1. **修复 M7 高优问题**：补齐 `fastxl_mcp.py` 缺失参数与错误映射、解决 Windows `Session` 文件锁风险、打通 `fastxl_preview` 原生 `Session`、下沉 `json_budget`。
2. **补齐 M8 公开测试与 CI**：提交夹具生成脚本、C++/Python 回归套件与 Linux x64 / Linux arm64 / Windows x64 GitHub Actions，完成 NEON 与 TSan/ASan 验证。
3. **补跑 `bench.py` 与文档收口**：同步更新 `docs/ai.md`、`docs/library.md`、`docs/probe.schema.json` 与 `README.md` 性能数据，发布 `v1.1.0` / `v2.0.0`。
