# fastxl 优化记录与进度

> 日期：2026-11-26 ｜ 基线：`v1.0.0`（`main`）｜当前：`v1.2.0`（开发中）
> 范围：`fastxl-core`、`fastxl-cli`、Python 绑定、MCP 工具链、文档与测试
> 本机验证平台：Windows 10 x64（i7-14700 / MSVC 19.51）、Linux x86_64；CI 覆盖 Linux x64/arm64 与 Windows x64

---

## 1. 目标

按三阶段路线推进（原始路线图与 M7 审计稿已在 v1.1.0 发版前归档进 CHANGELOG）：

1. **语义保真**：让 `info` / `preview` / `probe` / `schema` 不再丢失 Excel 的合并、样式、命名区域、表、批注、透视表、图表、隐藏行和公式引用。
2. **吞吐与检索**：解除单 reader 限制，支持多 Sheet 并发读取、大表流式并行、`search`、单 Sheet 多区域（Data Islands）和 Join Key 提示。
3. **平台与接口**：mmap、AVX2/NEON、`Result<T>` / `std::error_code`、C++20 Ranges、Writer 并行与公式/自定义样式、MCP 常驻化与输出预算。

所有新增 JSON 字段保持 `version: 1`，只增加可选字段；现有字段类型不变。

---

## 2. 里程碑进度

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 共享原语、线程安全 `ZipArchive`、A1 坐标辅助 | ✅ 完成 |
| M1 | 元数据保真（样式、definedNames、tables/comments/pivot/chart、合并/隐藏行/筛选、公式链接、验证范围） | ✅ 完成并验证 |
| M2 | 多 Sheet 并发读取、`profile_sheets`、大表流式并行 | ✅ 完成并验证 |
| M3 | `search`、regions、join_keys（CLI / Python / MCP / JSON Schema） | ✅ 完成并验证 |
| M4 | mmap、AVX2/NEON 分派、preview/search 快路径 | ✅ 完成；Windows x64 验证，ARM/Linux 待 CI |
| M5 | `Result<T>` / `std::error_code`、`WorkSheet::rows()`、DOM 内存上限 | ✅ 完成并验证 |
| M6 | Writer 公式透传、自定义样式、并行写、安装规则 | ✅ 完成并验证 |
| M7 | `Session` 常驻对象、MCP native-first + LRU、`--max-bytes` 预算 | ✅ 完成并验证（Linux x86_64 + Windows） |
| M8 | 测试fixtures/脚本/bench、全量文档收尾 | ✅ 完成（CI 待首次运行） |
| M8.1 | M7 审计修复：MCP 契约、Windows 文件锁、预览原生化、共享公式与主题色 | ✅ 完成并验证 |
| R1.1 | v1.1.0 发版收口：仓库清理、版本号、契约校验、Sanitizer/no-exceptions 门禁、基准 | ✅ 完成 |
| M9 | v1.2.0 优化：Python 位置/惰性行流式、`Session` 画像与检索缓存、多 Sheet/大 part 纯 C++ 基准 | ✅ 完成并验证（Windows x64 本机实测） |

---

## 3. 已完成内容

### M0 — 共享原语与线程安全归档

- `column_letters`、`cell_ref`、`range_ref`、`parse_cell_ref`、`parse_range_ref` 统一到 `types.hpp` / `types.cpp`，替换 CLI、绑定和 profile 中的重复实现。
- `ZipArchive` 在打开时缓存中央目录，`has` / `entries` / `locate` 不再共享 minizip reader；`read_all` / `open_entry` 每次创建独立 reader，支持并发读取。
- STORE 条目支持基于 mmap 的零拷贝读取；本地文件头与中央目录 extra 长度不一致时以本地头为准。

### M1 — 元数据保真

- `xl/styles.xml`：解析 `numFmt`、字体颜色、填充色、删除线，`SheetStyle::info(xf)` 暴露 `CellStyleInfo`。
- 工作簿级：`definedNames`、pivot cache 关系、表关系。
- 工作表级：合并区间、隐藏行、筛选列、tables、comments、pivot tables、charts 及截断标志。
- 公式：`uncached_formulas`、`formula_links`、每列 `formula_cells`、preview 的 `needs_recalc`。
- 数据验证：解析并展开同工作簿区域引用，输出 `enum_values`（带截断标志）。
- 所有读表面同步更新：C++ JSON、CLI、Python 绑定与 `.pyi`、MCP、`docs/probe.schema.json`、文档。

### M2 — 并发与流式 Profile

- 移除 `WorkBook` 的单活跃 `WorkSheetReader` 限制；每个 reader 持有独立解压流，SST 与样式表只读共享。
- 新增 `profile_sheets`，多 Sheet 按 `jobs` 并行，结果顺序稳定。
- 超过 `in_memory_threshold` 的大表使用“解压生产者 + 行片段工作队列”的流式流水线，内存受最大行与队列预算约束。
- 串行、缓冲并行、流式并行的计数结果保持一致（大表 `jobs=1/4` 已验证）。

### M3 — 检索与复杂布局

- 新增 `fastxl::search_workbook` / `search_table`，支持文本与公式命中、上下文窗口、正则、大小写、Sheet 过滤、命中上限。
- CLI 新增 `search`，JSON 契约包含 `matches`、`match_count`、`truncated`、`searched_cells`。
- `schema` / `probe` 支持 `regions`：按空行/空列与合并边界识别单 Sheet 多区域，区域独立表头与列画像。
- `schema` 输出 `join_keys`：同名列与值域重合度提示。
- Python 与 MCP 暴露 `search`；文档与 JSON Schema 同步。

### M4 — 平台吞吐

- `MappedFile`：POSIX `mmap` / Windows `CreateFileMapping` + `MapViewOfFile`，失败自动回退文件 API。
- `ZipArchive` 支持 mmap 上的 per-call reader；STORE 条目零拷贝解析，DEFLATE 仍解压。
- SIMD 分派：x86_64 优先 AVX2，AArch64 NEON，SSE2/标量兜底；`fastxl-cli --version` 输出 `simd`。
- preview/search 先做列与范围过滤，避免无关单元格的 `to_text` 与样式解析。

### M5 — C++20 易用性

- `Error` / `Status` / `Result<T>`、`std::error_code` 分类与 `try_*` 边界入口。
- `WorkSheet::rows()` Input Range，单遍、借用生命周期；复用 `RowChannel`。
- `DomOptions::max_memory_bytes` 默认 1 GiB，超限抛 `LimitExceeded`。
- 文档说明：非异常 API 是边界适配，库内部仍使用异常。

### M6 — Writer 能力与安装

- **公式透传**：`CellView::formula` 写入 `<f>`；`Null` 公式单元格不写 `<v>`，读取侧标记 `needs_recalc`。
- **自定义样式**：`CellStyle` + `register_style` + `style_count`；基础 18 个 `cellXfs` 保留，自定义 id 从 18 开始。
  - 复用/去重数字格式（164/165 及从 166 开始的代码）
  - 字体组合（bold/italic/strike/color）与纯色填充去重
  - `write_row` 第四参数 `style_ids` 按单元格覆盖推导样式
- **并行写**：`BookWriteOptions::parallel/jobs/max_sheet_buffer_bytes`
  - 每表独立 XML 缓冲，超阈值溢写临时文件
  - 提交时并行 raw deflate，按索引顺序封包 ZIP
  - SST 在并行模式下加锁，句柄线程归属明确
- **安装规则**：`xmake install` 安装静态库、公开头文件与 CLI：

  ```text
  <prefix>/lib/fastxl-core.lib
  <prefix>/include/fastxl/*.hpp
  <prefix>/bin/fastxl-cli.exe
  ```

#### M6 关键修复

1. **`writer.commit()` 崩溃**
   - 根因：`ZipWriter` 在高层 `mz_zip_writer_*` 句柄上混用低层 `mz_zip_entry_*` API。
   - 修复：`ZipWriter` 全部切换到低层 `mz_zip_create/open/entry_*`，只保留一套 ZIP 写入实现；叠加 buffered stream。

2. **STORE 条目零拷贝偏移错误**
   - 触发：顺序 writer + `compression_level=0` 生成的 ZIP64 本地 extra 与中央目录 extra 长度不同。
   - 现象：读取 sheet XML 偏移 20 字节，报 `MalformedXml: control character in text`。
   - 修复：`mapped_store_entry` 从本地文件头读取 filename/extra 长度，再计算数据偏移；签名/边界失败时回退普通读取。

3. **回归覆盖**
   - 并行 writer：公式、合并、`0.00%`、strike、字体色、填充色、文件溢写。
   - 顺序 writer：STORE、inline string、公式、合并。
   - 并行 writer + STORE：预压缩条目的 `close_raw` 路径。

### M7 — 常驻会话与输出预算

**C++ 输出预算（`projects/fastxl-cli/src/json_budget.hpp`）**

- `--max-bytes N` 从"已解析但未生效"变为真实生效：`info` / `preview` / `schema` / `probe` / `search` 的 JSONL 输出统一经过 `apply_output_budget()`。
- 按信息损失从小到大分阶段折叠，每阶段只在仍然超预算时执行：
  1. `samples` 每列样本值
  2. `top_values` 每列值频次
  3. `empty_columns` 全空列（整列移除并记入 `folded_columns`）
  4. `enum_values` 验证枚举
  5. `metadata` 合并单元格、隐藏行、公式链接、tables/comments/pivot/charts/definedNames
  6. `constant_columns` 单值列的数值/文本统计
  7. `rows` / `matches` 尾部预览行与检索命中（置 `truncated: true`）
  8. `columns` 超宽表的尾部列
- 输出始终是合法 JSON，末尾追加 `budget` 对象：`max_bytes`、`bytes`、`applied`、`within_budget`、`stages`、`folded_columns`、`dropped_rows`、`dropped_matches`。
- 报告本身也计入预算：折叠明细反而撑爆预算时先丢 `folded_columns` 明细、再丢 `stages` 明细（保留计数）；仍不达标就如实返回 `within_budget: false`，不截断 JSON。
- 人类可读的 text/table 布局不受影响（已有行列上限）。

**Python 常驻会话（`fastxl.Session`）**

- 新增 `fastxl.Session(capacity=8)`：按路径缓存已打开的 `WorkBook`，LRU 淘汰，`stats()` 暴露 `size` / `capacity` / `hits` / `misses`。
- 失效条件是文件的 `(mtime, size)` 变化；`clear()` 手动清空；支持 `with fastxl.Session() as s:`。
- `info` / `schema` / `probe` / `search` 复用缓存 workbook（共享字符串、样式表只解析一次）；`preview` / `read_rows` 仍走流式读取，因为 reader 独占解压流。
- 模块级函数与 Session 方法都新增 `max_bytes=0`，返回同样的 `budget` 对象；Python 侧预算与 CLI 共用同一份 C++ 实现，避免两套裁剪逻辑漂移。

**MCP native-first（`projects/tools/fastxl_mcp.py`）**

- 默认优先导入编译模块（`build/python` 或已安装包），进程内常驻一个 `Session`；模块不可用时，或设置了 `FASTXL_FORCE_CLI=1` / `--cli-only`，回落到 `fastxl-cli` 子进程。
- `initialize` 的 `serverInfo.mode` 报告 `native` / `cli`，便于排查。
- `fastxl_info` / `fastxl_schema` / `fastxl_probe` / `fastxl_search` 走 native；`fastxl_preview` 保留 CLI 契约（used range、header row、合并单元格等），因为行读取接口的载荷形状不同。
- 所有工具的 `inputSchema` 新增 `max_bytes` 并原样透传；原生调用失败时打印 stderr 并回落到 CLI，错误渲染保持一致。

**契约与文档**

- `docs/probe.schema.json` 新增 `Budget` / `BudgetColumn`，并补上此前缺失的 `Search` / `SearchMatch`。
- `docs/ai.md`、`docs/library.md`、`README.md` 与 CLI `--help` 同步新增说明。

**回归与修复**

- 过时测试 `workbook_one_reader_at_a_time` 改为 `workbook_concurrent_readers`：并发 reader 是 M2 的特性，改为跨线程读两张表并与顺序读取结果比对。
- 新增 `projects/tests/test_python.py`（模块函数、预算、Session 缓存/失效/淘汰、与 CLI 合约对齐）与 `projects/tests/test_mcp.py`（native/CLI 两种模式、预算、错误路径）。
- `test_cli.py` 增加五个命令的 `--max-bytes` 断言，并修正 tool list（补上 `fastxl_search`）。

### M8 — 测试、基准与 CI

**fixtures（标准库生成，CI 可复现）**

`projects/tests/fixtures/gen_fixtures.py` 新增六个 fixture：

| fixture | 覆盖点 |
|---|---|
| `metadata.xlsx` | 样式（字体色 / 填充 / strike / 数字格式）、definedNames（工作簿级 + sheet 级）、合并单元格、隐藏行与隐藏列、autoFilter、数据验证列表、table、comments、pivot（含 cache 源）、chart（含引用） |
| `multi.xlsx` | 4 个 sheet（visible / hidden / veryHidden），跨表 `order_id` 触发 `join_keys` |
| `large.xlsx` | 15000 行 × 10 列，用于流式 profiling、行读取与 bench |
| `search.xlsx` | 文本命中、公式命中（缓存文本不匹配）、Unicode 文本、数字文本命中 |
| `regions.xlsx` | 两个由空行分隔的数据岛屿，`--header-row auto` 时各自识别表头 |
| `budget.xlsx` | 40 列宽表（22 个全空列、1 个常量列），用于预算折叠 |

生成器保持纯标准库，不引入 openpyxl，CI 与本地用同一份代码。

**测试**

- `test_cli.py`：新增 `test_feature_fixtures`，覆盖上述六个 fixture（元数据保真、sheet 可见性、join_keys、search 命中与公式命中、region 切分、预算折叠、`--max-rows` 流式上界）。
- `test_python.py`：新增 native 侧的同组断言，并验证 `Session` 复用。
- `test_mcp.py`：同一组 fixture 在 native 与 `FASTXL_FORCE_CLI=1` 两种模式下各跑一遍。
- 三个脚本在 fixture 缺失时自动调用生成器，因此干净 checkout 与 CI 无需额外步骤。

**bench**

`projects/bench/bench.py` 测 probe 层（与 `perf_suite.py` 的裸吞吐互补）：每个用例给出 median / min / max 毫秒与子进程峰值 RSS，覆盖 CLI、CLI + `--max-bytes`、native、`Session` 热缓存四种路径，`--json` 可落盘供对比。

**CI**

`.github/workflows/ci.yml`：

- Linux x64（`ubuntu-24.04`）与 Linux arm64（`ubuntu-24.04-arm`）：安装 `python3-dev` / `pybind11-dev`，构建 `fastxl-tests fastxl-cli fastxl-python`，运行 C++、CLI、Python、MCP 四组检查。
- Windows x64：构建 `fastxl-tests` / `fastxl-cli`，运行 C++ 与 CLI 检查（Python 模块依赖本机 CPython 头文件路径，保留给本地验证）。
- xmake 包缓存按平台缓存，`fail-fast: false` 让 x64 与 arm64 互不遮挡。

**入库策略**

`.gitignore` 从"整个 `projects/tests/`、`projects/bench/` 忽略"改为"测试与基准源码入库，生成的 fixtures 与 bench 数据仍然忽略"。这样 CI 和后续贡献者跑的是同一套测试，而不是各自本地的私有副本。

**skill**

`skill/scripts/fastxl_skill.py` 的 `info` / `schema` / `probe` / `preview` 增加 `--max-bytes`（native 与 CLI 两条路径都透传），`SKILL.md` 同步示例。

### M8.1 — M7 审计修复

对应 v1.1.0 发版前的 M7 审计问题清单（结论已汇总进 CHANGELOG）。

**MCP 契约（P0）**

- `fastxl_schema` / `fastxl_probe` 的 `TOOL_SCHEMAS` 补上 `regions`，`fastxl_search` 补上
  `max_cell_bytes`；CLI 回退路径同时转发 `--regions`。
- `fastxl_preview` 改为原生：新增 `fastxl.preview_sheet()` / `Session.preview_sheet()`，
  与 CLI `preview` 共享同一份 JSON 组装（`preview_json.{hpp,cpp}`）。MCP 对 xlsx 走
  native，CSV/JSONL 预览仍走 CLI。
- 确定性输入错误不再"先 native 失败再起一次 CLI"：捕获 `fastxl.Error` 后直接返回
  结构化错误对象（含 `code` / `message` / `exit_code` / `command` / `file`）。

**Windows 常驻文件锁（P0）**

- `Session` 新增 `memory_map` 参数（`None` 表示平台默认：Windows 关闭、其它开启），
  通过 `OpenOptions::memory_map` 传给 `WorkBook::open`；Windows 上常驻缓存不再持有
  阻塞 Excel 保存的映射，需要时可用 `Session(memory_map=True)` 显式打开。

**分层与 Schema 对齐（P1）**

- `json_budget.hpp` 从 `projects/fastxl-cli/src/` 下沉到
  `projects/fastxl-core/include/fastxl/detail/`，命名空间改为 `fastxl::detail`，CLI 与
  Python 绑定都包含同一份实现，绑定不再依赖 CLI 的预算源码。
- 核对 `docs/probe.schema.json`：预算字段为 `folded_columns` / `dropped_rows` /
  `dropped_matches`，与实现、文档一致（审计提到的 `omitted_columns` 不存在）。

**真实 Excel 边界（P2）**

- **共享公式**：`SheetParser` 追踪 `<f t="shared" si=...>` 主公式，按行/列偏移重写跟随
  单元格的相对引用（`$` 绝对部分保持），`preview`、`search --field formula`、
  `formula_cells` / `formula_links` 都能看到整段公式。使用共享公式的工作表由并行
  profiling 回退为单遍扫描（跟随公式可能落在不同行片段）。
- **主题色**：解析 `xl/theme/theme1.xml` 的 12 色方案，按 `theme="N"` + `tint` 解析为
  具体 RGB（HSL 亮度调整），`font_color` / `fill_color` 对 Office 主题给出
  `#4472C4`、tint 0.4 → `#8FAADC`、tint -0.25 → `#2F5597` 等结果。
- **`convert` 公式丢失**：定位到 `CopyHandler` 把 `CellView.formula`（指向解析器临时
  缓冲的 view）直接传到 writer，回调返回后失效，出现 `SUM` → `UM` 的首字符丢失；
  改为与文本一样复制到行内存储，xlsx→xlsx 现在完整保留公式文本。
- **CSV 编码诊断**：UTF-8 校验失败时明确提示"fastxl 只读 UTF-8（允许 BOM），不检测
  GBK/GB18030，请另存为 UTF-8"，而不是只说 "not valid UTF-8"。

**测试**

- 三个测试套件新增 `audit_fixes`：主题色、共享公式（含 `$` 绝对引用与区间）、
  `convert` 公式保留、GBK 诊断、`preview_sheet` 与 CLI 逐字节对齐、`Session.memory_map`、
  `Error.exit_code`、MCP 的 `regions` / `max_cell_bytes` / 原生 preview / 错误对象。
- `test_cli.py` 增加 JSON Schema 强校验：`info`、`preview`、`schema`、`probe`、`search`、
  `errors`、失败错误对象与预算输出共 8 类，使用 `docs/probe.schema.json`（未安装
  `jsonschema` 时自动跳过）。
- fixture 生成器写入 `VERSION` 标记（生成器源码哈希），测试在生成器变更后自动重建，
  避免本地使用过期 fixture。

### R1.1 — v1.1.0 发版收口

对应 v1.1.0 发版审计检查单（根目录的审计过程文件已在 v1.2.0 清理中删除，历史沉淀在
本文件与 `CHANGELOG.md`）。

**仓库清理**

- 删除根目录过程文件 `fastxl_optimization_roadmap.md`、`fastxl_m7_audit_and_m8_plan.md`，
  README 链接改为 `CHANGELOG.md`；`docs/optimization-progress.md` 中的引用改为文字说明。
- 修复 `docs/limitations.md` 指向不存在 `dom-optimization.md` 的死链。
- `.gitignore` 补上 `bindings/python/fastxl/_fastxl*.pyd` / `.so`，避免本地扩展产物误提交。
- `xmake.lua` 版本号 `1.1.0`，删除"tests/bench 不随仓库发布"的过时注释；`FASTXL_VERSION`
  同时传给 `fastxl-python`，CLI / Python 包 / MCP `serverInfo` 版本一致。
- 新增 `CHANGELOG.md`（v1.0.0 → v1.1.0 全量特性与修复）。

**质量门禁**

- **Windows CI 补全**：`windows` job 现在安装 `pybind11`，用 `FASTXL_PY_*` 指向 runner
  上的 CPython，编译 `fastxl-python` 并执行 `test_cli.py` / `test_python.py` /
  `test_mcp.py`——M8.1 的两项 Windows 专属修复（`default_memory_map()`、MCP UTF-8
  stdio）从此有 CI 覆盖。
- **契约校验真实执行**：Linux / Windows 都安装 `jsonschema`；`test_cli.py` 对 8 类载荷、
  `test_mcp.py` 对 5 个工具输出与错误对象做 `docs/probe.schema.json` 校验，不再依赖
  "本机恰好装了 jsonschema"。
- **Sanitizer 作业**（`linux-sanitizers`）：ASan + UBSan 跑完整 C++ 套件（含 leak 检测）
  0 报告；TSan 起初报出 `mktime`/`tzset` 的 libc 内部竞态——`ZipArchive::open` 增加
  进程内 `tzset()` 预热，其余报告是 glibc 私有锁对 TSan 不可见导致的误报，用带注释的
  `projects/tests/tsan.supp` 精确抑制，现在 TSan 也是 0 报告。
- **no-exceptions 门禁**：公开头文件在 `-fno-exceptions` 下可编译（`Result`/`Status`
  边界降级为 abort-on-error），CI 用
  `g++ -fno-exceptions -fsyntax-only projects/tests/no_exceptions_check.cpp` 守住。
  库本体仍以异常构建，文档明确该契约。

**基准与文档**

- 新增 `projects/bench/compare_baselines.py`（读：fastxl / `openpyxl` /
  `python-calamine`）与 `projects/bench/compare_write.py` + `write_bench_rs`（写：
  fastxl / `rust_xlsxwriter` / `xlsxwriter` / `openpyxl`），全部子进程隔离测量
  wall time、rows/s、输出大小与峰值 RSS。
- README 新增 Performance 章节：probe 层延迟（CLI/native/Session）、读对比、写对比
  与复现命令。
- 本机（Linux x86_64，Python 3.14，calamine 0.9.1，rust_xlsxwriter 0.104）实测：

  | 场景 | fastxl | 基线 |
  |---|---|---|
  | 读 200k×8（流式扫描） | 0.57 s / 352k rows/s / 133 MB | calamine 1.41 s、openpyxl 10.13 s |
  | 读 200k×8（物化全部行） | 1.26 s / 159k rows/s / 257 MB | calamine `to_python` 1.48 s / 147 MB |
  | 写 200k×8 | 1.81 s / 111k rows/s / 113 MB | rust_xlsxwriter 1.95 s / 270 MB、xlsxwriter 10.18 s、openpyxl 17.32 s |
  | 写 15k×10 | 0.16 s / 92k rows/s / 28 MB | rust_xlsxwriter 0.17 s / 26 MB、openpyxl 1.55 s |

  fastxl 的写数字包含默认的原子写 + fsync；物化全部行时内存高于 calamine（Python
  dict vs list），README 已注明适用场景。

### M9 — v1.2.0 优化：Python 行流式、Session 缓存与吞吐基准

对应 v1.2.0 审计提出的 4 项优化（P1/P2）：Python 全行读取 RSS、`Session` 热缓存、
内存说明与多 Sheet/大 part 基准。

**Python 行读取**

- `read_rows(..., as_tuples=True)` 返回按列顺序的元组行（优先于 `as_dicts`，现有调用
  不变）；新增惰性 `iter_rows()`（`RowIterator`），逐行解码、不物化整表，暴露
  `columns` / `header_row` / `sheet` / `rows_read` / `truncated`，无 `max_bytes` 预算；
  `Session.iter_rows` 提供同名接口但独立打开 reader、不复用缓存工作簿。
- 每列 dict key 复用同一个 `py::str` 对象（interned），去掉逐行重复的列名字符串。
  200k×8（`gen_fixtures.py --big --rows 200000 --cols 8`，55.9 MiB part，Windows x64，
  独立进程峰值工作集，解释器+模块基线 22.8 MiB）：`read_rows(0)` dict 188.0 MiB /
  0.71 s；`as_tuples=True` 157.1 MiB / 0.70 s；`as_dicts=False` 160.1 MiB / 0.70 s；
  `iter_rows` 惰性消费 85.6 MiB / 0.67 s（≈298k rows/s）。该 part 小于 64 MiB
  `in_memory_threshold`，`iter_rows` 的 62.6 MiB 增量主要是整块缓冲本身；更大的 part
  走 256 KiB 流式窗口，内存有界。

**Session 缓存**

- 在原有工作簿 LRU 之外新增两层 LRU：`SheetProfile`（按文件 `(mtime, size)` + sheet +
  `header_row`/`max_rows`/`sample`/`top`/`distinct_cap`/`regions`/`jobs` 键控，
  `profile_capacity` 默认 64）与 `search` JSON 载荷（按 query + 选项键控、忽略
  `max_bytes`，`search_capacity` 默认 16）。`stats()` 增加
  `profiles`/`profile_capacity`/`profile_hits`/`profile_misses` 与
  `searches`/`search_capacity`/`search_hits`/`search_misses`，原有工作簿 key 不变；
  `clear()` 清空全部缓存。
- `large.xlsx`（15k×10，Windows x64，MSVC 19.51 release，Python 3.15.0，5 次中位数）：
  native 冷调用 `schema`/`probe`/`search` 为 38.75/37.39/72.94 ms，`Session` 热调用为
  0.24/0.23/0.02 ms——热路径直接复用缓存画像/载荷，不再扫表。

**基准扩展**

- `perf_suite.py` 新增 `--sheets N`/`--sheets-rows N`（多 Sheet 组）与
  `--large-rows N`/`--large-cols N`（>64 MiB 流式组），默认 0=关闭（原有单 Sheet
  矩阵不变）；`--json`/`--write` 输出机器可读结果。`bench.cpp` 纯 C++20 路径
  （Windows 10 x64 / i7-14700 / MSVC 19.51，独立进程、1 次预热 + 7 次中位数）：

  | 工作负载 | 串行/`jobs=1` | 并行/`jobs=4` | 加速比 |
  |---|---:|---:|---:|
  | 多 Sheet read-all（12 表 × 20k × 8） | 611 ms | 185 ms | 3.30× |
  | 多 Sheet profile-all | 929 ms | 362 ms | 2.57× |
  | 多 Sheet 写（单线程产行 + commit 并行压缩） | 914 ms | 578 ms | 1.58× |
  | >64 MiB profile（1M×8，274.5 MiB 未压缩） | 3,542 ms | 1,674 ms | 2.12× |

  单 Sheet 200k×8 profile：`jobs=1` 639 ms → `jobs=4` 390 ms（1.64×）；>64 MiB 的
  jobs=4 行 `parallel=true`，确认走流式并行 profiler。多 Sheet 读/画像按表并行（表内
  强制 jobs=1），随表数扩展；`WorkBookWriter` 非线程安全，并行写仍是单线程产行、commit
  时按表并行压缩，故并行写峰值 RSS 更高（12.2 → 97.1 MiB）。Windows 峰值工作集与
  Linux `ru_maxrss` 口径不同，不跨平台比较。

复现：

```sh
xmake build -y fastxl-bench
python projects/bench/perf_suite.py --runs 7 --jobs 4 --rows 200000 --cols 8 \
  --sheets 12 --sheets-rows 20000 --large-rows 1000000 --large-cols 8 \
  --write build/reports/performance.md
```

**内存说明**

- README 补充内存说明：解压后 worksheet part ≤ `OpenOptions::in_memory_threshold`
  （默认 64 MiB）整块缓冲，超过时切换 256 KiB 流式窗口；低内存环境可调低
  `in_memory_threshold` 与 `distinct_cap`。`docs/limitations.md` 的 `Session` 条目
  同步改为缓存行为、LRU 与内存注意事项；`CHANGELOG.md` 增加 v1.2.0 Unreleased 段。

---

## 4. 验证记录

### 构建与测试

```sh
xmake f -y -m release -p windows -a x64
xmake build -y fastxl-cli fastxl-python fastxl-tests
./build/windows/x64/release/fastxl-tests.exe
```

结果：

```text
all checks passed
```

本机同时验证：

- `openpyxl` 成功读取 writer 输出；公式、`0.00%`、strike、字体色、填充色、合并范围均正确。
- `python zipfile.ZipFile.testzip()` 对 writer 输出返回 `None`。
- Python `fastxl.write_xlsx` → `fastxl.preview` 往返成功。
- `fastxl-cli convert --compress-level 0` 生成的 XLSX 可正常 `preview`。
- `xmake install -o build/install-test fastxl-core fastxl-cli` 目录布局正确。

### Linux 验证（M7）

```sh
xmake build -y fastxl-tests fastxl-cli fastxl-python
./build/linux/x86_64/release/fastxl-tests
python projects/tests/test_cli.py
python projects/tests/test_python.py
python projects/tests/test_mcp.py
```

结果：

```text
122 tests, 0 failures
CLI checks passed
Python checks passed
MCP checks passed
```

- `--max-bytes` 的五个命令输出均通过 `docs/probe.schema.json` 校验（含 `budget` 对象）。
- `test_python.py` 中 native 与 CLI 的 `info` / `probe` / `search` 输出逐字段一致。
- `build/python/fastxl/_fastxl.so` 为 CPython 3.14 构建，与 `.venv` 解释器匹配。

### M8 验证（Linux x86_64）

```sh
python projects/tests/fixtures/gen_fixtures.py --out projects/tests/fixtures
xmake build -y fastxl-tests fastxl-cli fastxl-python
./build/linux/x86_64/release/fastxl-tests
python projects/tests/test_cli.py
python projects/tests/test_python.py
python projects/tests/test_mcp.py
python projects/bench/bench.py --runs 5
```

结果：

```text
122 tests, 0 failures
CLI checks passed
Python checks passed
MCP checks passed
```

probe 层基准（`large.xlsx`，15000 行 × 10 列，5 次中位数，本机 Linux x86_64）：

| operation | CLI | CLI + budget | native | Session 热 |
|---|---:|---:|---:|---:|
| `info` | 3.4 ms | – | 0.15 ms | – |
| `preview rows=20` | 88 ms | – | 11 ms | – |
| `preview rows=2000` | 153 ms | 169 ms | 30 ms | – |
| `schema` | 86 ms | 91 ms | 80 ms | 78 ms |
| `probe` | 88 ms | 95 ms | 80 ms | 82 ms |
| `search` | 117 ms | – | 114 ms | 114 ms |

- 进程启动是 CLI 的固定成本：`info` 3.4 ms 对 native 0.15 ms；`preview rows=20` 88 ms 对 11 ms。
- `schema` / `probe` 由扫描本身主导，native 与 CLI 差距缩小，`Session` 只省掉包解析（约 2%），这与 M7 的设计预期一致。
- `--max-bytes` 的折叠成本很小：`schema` 86 → 91 ms（CLI）、80 → 80 ms（native）。

### M8.1 验证（Linux x86_64）

```sh
xmake build -y fastxl-tests fastxl-cli fastxl-python
./build/linux/x86_64/release/fastxl-tests
python projects/tests/test_cli.py
python projects/tests/test_python.py
python projects/tests/test_mcp.py
```

结果：

```text
122 tests, 0 failures
CLI checks passed
Python checks passed
MCP checks passed
pyright: 0 errors
```

- `preview` 重构前后在 19 组用例（xlsx/csv/jsonl、range/columns/auto header/budget/
  text 布局/regions）下输出逐字节一致。
- `fastxl.preview_sheet()` 与 CLI `preview --json` 在 9 组参数下完全相等；`Session`
  第二次调用命中缓存。
- 主题色：`A2` → `#4472C4`、`B3` → `#2F5597`、`B4` → `#8FAADC`。
- 共享公式：`B2/B3/B4` → `A2*2` / `A3*2` / `A4*2`，`C4` → `SUM($A4:B4)`，
  `D3` → `$A3+A$2`；`search "A3*2"` 命中 `formula` 字段。
- `convert` 后公式文本 `SUM(C2:C4)` 完整保留（修复前为 `UM(C2:C4)`）。

### 验证边界

- Windows x64（原作者 + CI）与 Linux x86_64（本次）已本地验证；Linux arm64 由新增 CI job 覆盖（首次运行结果待观察）。
- R1.1/M9 的探针延迟、读/写对比与多 Sheet/大 part 基准已记录在 `README.md` 与本文件 M9
  小节；Windows 峰值工作集与 Linux `ru_maxrss` 口径不同，跨平台数字不可直接比较。

---

## 5. 关键文件

| 领域 | 文件 |
|---|---|
| 共享 A1 / 类型 | `projects/fastxl-core/src/types.cpp`、`include/fastxl/types.hpp` |
| ZIP 读取 / mmap | `src/zip_archive.cpp`、`src/platform/mapped_*` |
| ZIP 写入 | `src/zip_writer.cpp`、`src/workbook_writer.cpp` |
| 元数据解析 | `src/xml_part_parsers.cpp`、`src/sheet_parser.cpp`、`src/formula.cpp` |
| Profile / 并行 | `src/profile.cpp`、`include/fastxl/profile.hpp` |
| 检索 / 区域 | `src/search.cpp`、`include/fastxl/search.hpp`、`src/rows.cpp` |
| Writer API | `include/fastxl/writer.hpp` |
| CLI | `projects/fastxl-cli/src/cmd_*.cpp` |
| Python / MCP | `bindings/fastxl_module.cpp`、`bindings/python/fastxl/`、`projects/tools/fastxl_mcp.py` |
| 输出预算 | `projects/fastxl-cli/src/json_budget.hpp` |
| 测试 | `projects/tests/test_cli.py`、`test_python.py`、`test_mcp.py`、`fixtures/gen_fixtures.py` |
| 基准 | `projects/bench/bench.py`、`perf_suite.py`、`bench.cpp`、`python_overhead.py` |
| CI | `.github/workflows/ci.yml` |
| 契约 | `docs/probe.schema.json`、`docs/ai.md`、`docs/library.md` |

---

## 6. 剩余工作

v1.1.0 的 P0/P1 门禁已全部落地。后续版本候选：

- **完整 `-fno-exceptions` 库构建**：当前只保证公开头文件可被禁用异常的宿主使用；
  库内 31 处 `try/catch` 边界仍需按 `__cpp_exceptions` 分块改造，才能在
  `-fno-exceptions` 下构建整库。
- **arm64 NEON 实测**：CI 的 linux-arm64 job 已能构建并跑通测试，但还没有 NEON 与
  scalar 的逐单元格等价性对比与性能数据。
- **更多基线**：LibreOffice headless 重算、`openpyxl` round-trip 的自动化对照，
  以及把 `compare_write.py` 的写入基准纳入 CI 回归门限。
- **未覆盖的格式细节**：`indexed` 调色板、数组公式（`t="array"`）展开、
  Windows 上 `Session(memory_map=True)` 的实测行为。
- **Windows Python 模块的长期验证**：CI 已覆盖，但 Windows 上的 `mmap`/文件锁行为
  建议再加一条“缓存后文件可被替换”的集成测试。

## 7. 使用与构建

```sh
xmake f -y -m release -p windows -a x64
xmake build -y fastxl-cli fastxl-python
xmake install -o <prefix> fastxl-core fastxl-cli
```

C++ 最低要求：C++20、xmake；ZIP 仅支持 STORE / DEFLATE，加密 ZIP 会返回
`UnsupportedEncryption`。详细 API 见 `docs/library.md`，JSON 契约见
`docs/probe.schema.json`。
