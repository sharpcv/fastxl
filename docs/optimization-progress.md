# fastxl 优化记录与进度

> 日期：2026-11-25  
> 基线：`v1.0.0`（`main`）  
> 范围：`fastxl-core`、`fastxl-cli`、Python 绑定、MCP 工具链、文档与测试  
> 本机验证平台：Windows 10 x64（Linux / aarch64 需由 CI 补充验证）

---

## 1. 目标

按 `fastxl_optimization_roadmap.md` 的三阶段路线推进：

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
| M7 | `Session` 常驻对象、MCP native-first + LRU、`--max-bytes` 预算 | ⏳ 未开始 |
| M8 | 测试fixtures/脚本/bench、全量文档收尾 | ⏳ 未开始 |

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

### 验证边界

- 当前仅在本机 **Windows x64** 验证；Linux x86_64 / aarch64、AVX2/NEON 与 mmap 的跨平台行为需要 CI。
- 尚无正式 wall-clock benchmark 数据；不要在报告中引用未记录的加速比。

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
| 契约 | `docs/probe.schema.json`、`docs/ai.md`、`docs/library.md` |

---

## 6. 剩余工作

### M7 — MCP 常驻化与输出预算

- 新增 `fastxl.Session`，缓存已打开 `WorkBook`。
- MCP 默认 native-first；`fastxl.Session` LRU（最多 8 个），按 `(mtime_ns, size)` 失效。
- `fastxl_info` / `preview` / `schema` / `probe` / `search` 支持 `max_bytes`。
- `json_budget.hpp` / Python 侧预算裁剪：折叠空列、常量列、样本与元数据，输出 `budget` 对象。
- CSV/JSONL 走模块级函数，不建立 Session。

### M8 — 验证与文档收尾

- 生成 fixtures：`metadata.xlsx`、`multi.xlsx`、`large.xlsx`、`search.xlsx`、`regions.xlsx`、`budget.xlsx`。
- 增加 `test_cli.py`、`test_python.py`、`test_mcp.py`、`bench.py`。
- 全量跑通 CLI / Python / MCP / C++ 测试。
- 补齐 Linux / aarch64 CI 验证，并更新 README、`docs/ai.md`、`docs/library.md`、`docs/limitations.md`、skill 文档。

---

## 7. 使用与构建

```sh
xmake f -y -m release -p windows -a x64
xmake build -y fastxl-cli fastxl-python
xmake install -o <prefix> fastxl-core fastxl-cli
```

C++ 最低要求：C++20、xmake；ZIP 仅支持 STORE / DEFLATE，加密 ZIP 会返回
`UnsupportedEncryption`。详细 API 见 `docs/library.md`，JSON 契约见
`docs/probe.schema.json`。
