# fastxl 库使用指南

fastxl 是一个 C++20 表格库，用统一的读取/写入接口处理 **XLSX、CSV、JSONL**。
它面向两类场景：

1. **值读取与转换**：快速、低内存地扫描工作表，把单元格转成统一类型或规范文本。
2. **AI/数据探针**：输出列类型、空值、distinct、top values、数值统计、枚举、
   元数据和行级预览，供模型或代码生成器消费。

本文只描述公开头文件 `projects/fastxl-core/include/fastxl/*.hpp` 中的 API。CLI 的完整用法
见 [README](../README.md) 和 [AI integration](ai.md)。

---

## 目录

1. 设计目标与非目标
2. 构建与集成
3. 数据模型
4. 读取 XLSX：WorkBook / SAX
5. 读取 XLSX：DOM
6. 表格式读写：Reader / Writer
7. 写入 XLSX：WorkBookWriter
8. 概览分析：Profile
9. 值、日期与文本工具
10. 错误处理
11. 生命周期、线程与内存
12. 支持范围与限制
13. CLI 速览
14. 最小示例

---

## 1. 设计目标与非目标

**目标**

- 流式读取大工作表，内存不随行数增长。
- 一个 `CellView` 表示所有格式的值，混合类型列不需要分支。
- 每个值都能转成规范文本，便于 CSV/JSONL 输出和模型消费。
- 写入原子化：先写临时文件，成功后替换目标。
- XLSX 输出支持 ZIP64、共享字符串去重、冻结行、自动筛选、合并单元格。

**非目标**

- 不计算公式。读取缓存值，写入时丢弃公式。
- 不支持旧版 `.xls`（CFB）、加密工作簿、ODS、Parquet。
- 不实现完整 Excel 样式系统：颜色、边框、图片、图表、批注、数据透视表等不写入。
- CSV/JSONL 只处理 UTF-8。

---

## 2. 构建与集成

支持平台：Windows x64 与 Linux（x86_64/aarch64），其他平台不在支持范围内。

依赖：C++20 编译器、xmake；首次配置需要网络拉取 minizip-ng（仅 STORE/DEFLATE，
加密与签名关闭，链接 zlib-ng）与 zlib-ng、fast_float、simdutf、fmt、nlohmann_json。

```sh
xmake f -y -m release
xmake build -y fastxl-cli fastxl-python
```

Windows 上第一条命令改为 `xmake f -y -m release -p windows -a x64`。Python 绑定
要求 release 模式并需要本机 CPython 头文件，路径覆盖方式见
[`bindings/README.md`](../bindings/README.md)。

本地开发树额外带有 `fastxl-tests` 目标（`projects/tests/` 不随仓库发布）：

```sh
xmake build -y fastxl-tests
xmake run fastxl-tests
```

Linux 下库内部使用 `std::thread`（`profile` 并行分析与 XLSX 写出），编译和最终
链接都需要 `-pthread`；仓库内的 xmake 目标已自动添加，自行用 CMake/手工编译时
需显式传入。Windows 不需要 `-pthread`。

在自己的 xmake 工程中：

```lua
add_deps("fastxl-core")
```

或在 CMake 中自行编译 `projects/fastxl-core/src/*.cpp`、添加
`projects/fastxl-core/include` 和 `projects/fastxl-core/src` 到 include 路径，并链接上述依赖。

头文件入口：

```cpp
#include "fastxl/fastxl.hpp"
```

### 内部命名空间

`fastxl` 顶层只放公开 API（`projects/fastxl-core/include/fastxl/*.hpp`）。实现全部位于
`fastxl::detail` 之下，并按子系统隔离：

| 命名空间 | 内容 |
|---|---|
| `fastxl::detail::text` | UTF-8、Excel 转义、数值解析/格式化 |
| `fastxl::detail::io` | `ByteSink`/文件汇、`AtomicFile`；平台钩子在 `io::platform` |
| `fastxl::detail::xml` | `XmlCursor`、`XmlWriter`；扫描器走 `fastxl::detail::simd` |
| `fastxl::detail::ooxml` | 共享字符串、关系、样式、工作簿元数据解析 |
| `fastxl::detail::zip` | ZIP 读写 |
| `fastxl::detail::sheet` | 工作表扫描与 `SheetSink` |
| `fastxl::detail::table` | 表格式后端的输入/输出目标与工厂 |
| `fastxl::detail::simd` | SSE2 分隔符扫描、ASCII 快速检查、XML 文本单遍校验 |
| `fastxl::detail` | `FlatMap`/`FlatSet`、工作簿/写入器 pimpl、`WorkbookAccessor` |

内部头文件在 `projects/fastxl-core/src/`，不随库安装，调用方不应包含。

### 实现要点

- XML 扫描由独立的 `simd_scan.hpp` 提供：SSE2 每次比较 16 字节，查找
  `>`/`"`/`'` 等分隔符；短 token 先走标量前缀，避免 SIMD 启动开销。
  文本校验在一次遍历中同时检查控制字符、非字符、实体和 CR。
- 核心库不再使用 `std::unordered_map`/`std::unordered_set`。`FlatMap` 是
  开放寻址 + 稠密 entry 数组，bucket 只存 32 位索引，字符串 key 支持
  `string_view` 异构查找，避免 node 分配和临时字符串。

---

## 3. 数据模型

### 3.1 三种读取方式

| 方式 | 入口 | 内存 | 适用 |
|---|---|---|---|
| SAX 回调 | `WorkBook` + `SaxHandler`/`SaxCallbacks` | 有界；大 part 流式 | 大工作簿扫描、转换、统计 |
| 表格式 Reader | `open_reader` | 有界 | CSV/JSONL/XLSX 统一行接口 |
| DOM | `DomWorkBook::load` | 与单元格数成正比 | 小工作簿随机访问 |

`CellView` 是**借用视图**：`text`/`formula` 只在当前回调或当前行有效。
需要保留时复制到 `std::string` 或转换成 `Value`/`Cell`。

### 3.2 `CellType`

```cpp
enum class CellType : uint8_t {
  Number, SharedString, Text, Boolean, Error,
  Date, DateTime, Time, Invalid, Int, Null
};
```

| 值 | 含义 | 出现位置 |
|---|---|---|
| `Number` | IEEE-754 浮点 | XLSX 数值、CSV/JSONL 非整数 |
| `SharedString` | XLSX 共享字符串 | XLSX 读取，text 借用共享表 |
| `Text` | UTF-8 文本 | 所有格式 |
| `Boolean` | `true`/`false` | 所有格式 |
| `Error` | Excel 错误值 | XLSX 读取 |
| `Date` | 仅日期 | XLSX 日期样式、CSV/JSONL ISO 日期 |
| `DateTime` | 日期+时间 | 同上 |
| `Time` | 一天内时间；`number` 为秒 | XLSX 时间样式、表 reader |
| `Invalid` | 解析失败的单元格 | 宽松模式或坏数据 |
| `Int` | int64 整数 | 表 reader（CSV/JSONL/XLSX 整数可选） |
| `Null` | 空值 | 表 reader 补齐、显式空 |

核心函数 `to_string(CellType)` 返回 `"number"`、`"shared_string"`、`"text"`、
`"bool"`、`"error"`、`"date"`、`"datetime"`、`"time"`、`"invalid"`、`"int"`、
`"null"`。CLI `schema.type` 使用另一套名字（`integer`、`boolean`、`mixed` 等），
见第 8 节。

### 3.3 `CellIssue`

解析问题，不会抛异常，而是记录在 `CellView::issue`：

| 值 | 含义 |
|---|---|
| `None` | 正常 |
| `BadNumber` | `<v>` 不是合法数字 |
| `BadBool` | 布尔值不是 `0/1/true/false` |
| `BadDate` | 日期序列非法（例如 serial 60） |
| `SstOutOfRange` | 共享字符串索引越界 |
| `MissingValue` | 需要的值缺失 |

`profile` 的 `issues` 文本会包含数字形式的 issue（如 `cell r3c1: issue 3`）。

### 3.4 `ErrorValue`

`Null`、`Div0`、`Value`、`Ref`、`Name`、`Num`、`NA`、`GettingData`、`Spill`、
`Calc`、`Unknown`。`CellView::error_text()` 优先返回单元格原始错误文本，否则返回
规范文本（如 `#DIV/0!`）。

### 3.5 `DateTime` 与 `Date`

```cpp
struct Date { int32_t year; uint8_t month; uint8_t day; };
struct DateTime { int32_t year; uint8_t month; uint8_t day;
                  uint8_t hour, minute, second; uint16_t millisecond; };
```

所有坐标和日期字段都是 1-based 的自然值，不使用 Excel serial。

### 3.6 borrowed 与 owned

| 类型 | 所有权 | 用途 |
|---|---|---|
| `CellView` | 借用 | 读回调、写 `write_row` |
| `Value` | 拥有 `text` | 构造标量、复制后保存 |
| `Cell` | 拥有 `text`/`formula` | DOM 单元格 |
| `RowView` | 借用 cells | 表 reader 当前行 |
| `CellRowView` | 借用 cells | SAX 当前行（稀疏） |
| `Row` | 拥有 cells | DOM 行 |

---

## 4. 读取 XLSX：WorkBook / SAX

### 4.1 打开工作簿

```cpp
fastxl::WorkBook book = fastxl::WorkBook::open("book.xlsx", options);
```

| 参数 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `path` | `const std::filesystem::path&` | 必填 | XLSX/ZIP 路径；支持 Windows 长路径和 UTF-8 文件名 |
| `options` | `const OpenOptions&` | `{}` | 打开选项，见下表 |

`OpenOptions` 字段：

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `shared_strings` | `SharedStringMode` | `Load` | `Load` 加载文本；`Count` 只计数；`Skip` 完全跳过。读取单元格文本时必须用 `Load` |
| `max_shared_strings_bytes` | `size_t` | 512 MiB | `Load` 时共享字符串总字节上限，超出抛 `SharedStringsTooLarge` |
| `in_memory_threshold` | `size_t` | 64 MiB | part 解压后不超过此值时整块缓冲；更大时流式解析。并行 profile 只对缓冲 part 生效 |
| `max_metadata_bytes` | `size_t` | 256 MiB | workbook/rels/styles 等元数据 part 的上限 |
| `lenient` | `bool` | `true` | 宽松处理非法单元格引用/值（记录 `CellIssue`）；XML 结构错误仍然抛 `MalformedXml` |

`SharedStringMode`：

| 值 | 行为 |
|---|---|
| `Load` | 加载共享字符串，可读取 `SharedString` 单元格 |
| `Count` | 只统计数量，不保留文本；适合 `info` 类元数据 |
| `Skip` | 不读取 sharedStrings.xml；读取文本单元格会产生越界/缺失问题 |

### 4.2 `WorkBook` 方法

| 方法 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `open(path, options)` | 见上 | `WorkBook` | 静态工厂；失败抛 `Exception` |
| `sheet_count()` | — | `size_t` | 工作表/图表页总数 |
| `sheet(index)` | `size_t index`，0-based | `WorkSheet` | 越界抛 `InvalidSheetRef` |
| `find_sheet(name)` | `std::string_view` | `std::optional<WorkSheet>` | 精确匹配名称 |
| `sheets()` | — | `std::span<const WorkSheet>` | 按工作簿顺序 |
| `date_1904()` | — | `bool` | 工作簿使用 1904 日期系统 |
| `style()` | — | `const SheetStyle&` | 样式表 |
| `shared_string_count()` | — | `size_t` | 共享字符串总数（取决于打开模式） |
| `shared_string(index)` | `uint32_t index` | `std::string_view` | 需要 `Load`；否则抛 `SharedStringsNotLoaded`/`SharedStringOutOfRange` |
| `open_sheet(index)` | `size_t index` | `WorkSheetReader` | 打开一个活动 reader；同一 `WorkBook` 同时只能有一个活动 reader |
| `read_sheet(index, handler)` | `SaxHandler&` 或 `SaxCallbacks&` | `ReadStats` | 同步扫描一张表 |
| `read_all(handler)` | 同上 | `std::vector<ReadStats>` | 按顺序读取所有 worksheet，跳过 chartsheet |
| `for_each_cell(sheet, cb)` | `size_t`、`bool(const CellView&)` | `uint64_t` | 返回访问的单元格数；`cb` 返回 false 停止 |
| `for_each_row(sheet, cb)` | `size_t`、`bool(const CellRowView&)` | `uint64_t` | 返回访问的行数；`cb` 返回 false 停止 |
| `path()` | — | `const std::filesystem::path&` | 打开时的路径 |

`WorkBook` 是 move-only。`WorkSheet` 是轻量句柄，只要其 `WorkBook` 存活就有效。

### 4.3 `WorkSheet`

| 方法 | 返回 | 说明 |
|---|---|---|
| `name()` | `std::string_view` | 工作表名（UTF-8） |
| `sheet_id()` | `uint32_t` | workbook.xml 中的 `sheetId` |
| `rel_id()` | `std::string_view` | 关系 id，通常为 `rIdN` |
| `part_name()` | `std::string_view` | ZIP 内路径，如 `xl/worksheets/sheet1.xml` |
| `uncompressed_bytes()` | `uint64_t` | 解压后大小；用于判断缓冲/流式 |
| `state()` | `SheetState` | `Visible`/`Hidden`/`VeryHidden` |
| `kind()` | `SheetKind` | `Worksheet`/`Chartsheet` |
| `index()` | `size_t` | 0-based 顺序 |
| `is_worksheet()` | `bool` | `kind()==Worksheet` |
| `relationship(rel_id)` | `std::string_view` | 解析 sheet rels 中的目标；未知返回空 |
| `read(handler)` | `ReadStats` | 直接扫描本表 |

### 4.4 SAX 回调

`SaxHandler` 的每个虚函数都有空实现；按需覆盖。`SaxCallbacks` 是
`std::function` 版本，所有字段都可为空。

```cpp
fastxl::SaxCallbacks callbacks;
callbacks.cell = [](const fastxl::CellView& cell) {
  // cell.text 只在本次回调有效
  return true; // false 停止
};
sheet.read(callbacks);
```

回调顺序由文件内容决定，常见顺序是：`sheet_begin` → `dimension` →
`column`/`pane` → 每一行的 `row_info`/`row_begin`/`cell`/`row_end` →
`auto_filter`/`merge_cell`/`data_validation` → `sheet_end`。不要假设元数据一定在
数据行之前：`autoFilter`、合并区域和数据验证通常在 `</sheetData>` 之后。

| 回调 | 参数 | 说明 |
|---|---|---|
| `on_sheet_begin` | `const WorkSheet&` | 开始解析一张表 |
| `on_dimension` | `std::string_view ref` | `<dimension ref="A1:F100">` 原始引用 |
| `on_column` | `const ColumnView&` | `<col>` 的列宽/隐藏/样式 |
| `on_row_info` | `uint32_t row, const RowInfo&` | 行号（1-based）与行属性 |
| `on_pane` | `const PaneView&` | 冻结/拆分窗格 |
| `on_auto_filter` | `std::string_view ref` | 自动筛选范围，如 `A1:F100` |
| `on_hyperlink` | `const HyperlinkView&` | 超链接；target 已通过 sheet rels 解析 |
| `on_data_validation` | `const DataValidationView&` | 数据验证；含 formula1/formula2 |
| `on_row_begin` | `uint32_t row` | 返回 false 停止扫描 |
| `on_cell` | `const CellView&` | 返回 false 停止扫描 |
| `on_row_end` | — | 当前行结束 |
| `on_merge_cell` | `const MergeRange&` | 合并区域，1-based 闭区间 |
| `on_sheet_end` | — | 正常结束（停止时不会触发） |

`ReadStats`：

| 字段 | 含义 |
|---|---|
| `rows` | 访问的行数（含空 `<row>`） |
| `cells` | 访问的单元格元素数 |
| `bytes` | 扫描的 XML 字节数 |
| `stopped` | 是否因回调返回 false 提前停止 |

### 4.5 回调结构体

**`CellView`**

| 字段 | 类型 | 含义 |
|---|---|---|
| `text` | `std::string_view` | Text/SharedString 的 UTF-8 文本；Error 可携带原始错误文本；其他类型通常为空 |
| `formula` | `std::string_view` | 公式文本；仅在 `has_formula` 时非空 |
| `int_value` | `int64_t` | `CellType::Int` 的值 |
| `number` | `double` | Number 的值；Time 时为秒；Date/DateTime 不使用 |
| `datetime` | `DateTime` | Date/DateTime 的分解值 |
| `row` | `uint32_t` | 1-based 行号 |
| `col` | `uint32_t` | 1-based 列号 |
| `style` | `uint32_t` | 单元格样式索引（`xf`），可传给 `SheetStyle` |
| `type` | `CellType` | 值类型 |
| `issue` | `CellIssue` | 解析问题 |
| `error` | `ErrorValue` | Error 类型的具体错误 |
| `boolean` | `bool` | Boolean 值 |
| `has_formula` | `bool` | 是否存在公式 |
| `format` | `NumberFormat` | 写入提示；读取时通常为 `General` |

`CellView` 还提供静态工厂：

```cpp
CellView::null()
CellView::integer(int64_t)            // type = Int
CellView::real(double, NumberFormat)  // type = Number
CellView::boolean_value(bool)
CellView::string(std::string_view)    // 不复制，借用
CellView::date(Date)
CellView::datetime_value(DateTime)
CellView::time(double seconds)
CellView::error_value(ErrorValue, std::string_view raw = {})
CellView(uint32_t column, CellView value) // 复制值并设置 col
```

**`CellRowView`**：稀疏行。`row[0]` 是第一个存在的单元格，`row[0].col` 不一定为 1；
用 `find(col)`（1-based）查找。支持 `size`、`empty`、范围 for。

**`ColumnView`**

| 字段 | 含义 |
|---|---|
| `first_col` / `last_col` | 1-based 列范围 |
| `width` | Excel 列宽（字符单位，0 表示未指定） |
| `hidden` | 是否隐藏 |
| `custom_width` | 是否有自定义宽度 |
| `style` | 默认样式索引 |

**`RowInfo`**：`hidden`、`height`（点，0=未指定）、`custom_height`、`style`。

**`PaneView`**：`x_split`、`y_split`、`top_left_row`、`top_left_col`、`state`
（原始字符串，如 `frozen`/`split`）。

**`HyperlinkView`**：`ref`、`rel_id`、`target`（已解析）、`location`、`display`。

**`DataValidationView`**

| 字段 | 含义 |
|---|---|
| `type` | 验证类型，如 `list`、`whole`、`decimal` |
| `sqref` | 应用范围，如 `F3:F100` |
| `formula1` / `formula2` | 子元素文本；内联列表形如 `"a,b,c"` |
| `allow_blank` | `allowBlank="1"` |

`formula1`/`formula2` 是借用视图，只在回调期间有效。

### 4.6 `SheetStyle`

`CellView::style` 是 `cellXfs` 索引。通过 `book.style()`：

| 方法 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `xf_count()` | — | `size_t` | 样式数量 |
| `format_code(xf)` | `uint32_t xf` | `std::string_view` | 原始格式代码；越界返回空 |
| `is_date(xf)` | `uint32_t xf` | `bool` | 该样式是否日期 |
| `is_datetime(xf)` | `uint32_t xf` | `bool` | 该样式是否日期时间 |
| `is_time(xf)` | `uint32_t xf` | `bool` | 该样式是否时间 |

读取时 `CellView::format` 不填具体格式，使用 `style` + `SheetStyle` 判断。

---

## 5. 读取 XLSX：DOM

```cpp
fastxl::DomOptions options;
options.max_cells = 1'000'000;
options.max_bytes = 256ull << 20;
fastxl::DomWorkBook book = fastxl::DomWorkBook::load("small.xlsx", options);
```

`DomOptions`：

| 字段 | 默认 | 含义 |
|---|---|---|
| `max_cells` | 0（不限） | 所有表累计单元格上限，超出抛 `LimitExceeded` |
| `max_bytes` | 0（不限） | 估算内存上限：`text + formula + sizeof(Cell) + 32` 每格 |

`DomWorkBook`：

| 方法 | 说明 |
|---|---|
| `load(path, options)` | 物化所有 worksheet（chartsheet 保留为空表） |
| `sheets()` | `const std::vector<DomWorkSheet>&`，顺序与工作簿一致 |
| `date_1904()` | 日期系统 |

`DomWorkSheet`：

| 方法 | 说明 |
|---|---|
| `info()` | 对应 `WorkSheet` 句柄 |
| `rows()` | `const std::vector<Row>&`；行稀疏，按行号递增 |
| `merges()` | 合并区域 |

`Row` 的 `size/empty/operator[]/begin/end/find(col)` 与 `CellRowView` 类似，但
`Row` 拥有 `Cell`。

`Cell` 拥有 `text/formula`，字段与 `CellView` 对应；`Cell::view()` 转换成借用
`CellView`，`Cell::value()` 转换成 `Value`。

---

## 6. 表格式读写：Reader / Writer

表 API 用同一套 `Reader`/`Writer` 处理 XLSX、CSV、JSONL。

### 6.1 打开

```cpp
auto reader = fastxl::open_reader("data.csv", options);
auto writer = fastxl::open_writer("out.xlsx", reader->columns(), write_options);
```

| 函数 | 参数 | 说明 |
|---|---|---|
| `open_reader(path, options)` | 路径、`ReadOptions` | 扩展名决定格式；支持 `.xlsx`/`.xlsm`/`.csv`/`.tsv`/`.jsonl`/`.ndjson` |
| `open_reader(std::istream&, Format, options)` | 流、显式格式、选项 | xlsx 流会先 spool 到临时文件；CSV/JSONL 流式 |
| `open_writer(path, columns, options)` | 路径、列名 span、`WriteOptions` | 原子写入目标 |
| `open_writer(std::ostream&, Format, columns, options)` | 流、显式格式、列名、选项 | xlsx 流先写临时文件再复制到流 |

### 6.2 `ReadOptions`

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `format` | `Format` | `Auto` | 显式格式；`Auto` 按扩展名 |
| `sheet` | `std::string` | 空 | XLSX 工作表名或 1-based 索引；空=第一个 worksheet |
| `header_row` | `uint32_t` | 1 | 1-based 表头行；0=无表头。JSONL 始终按 0 处理 |
| `infer_types` | `bool` | `true` | 仅 CSV：是否推断数字/布尔/ISO 日期 |
| `delimiter` | `char` | `'\0'` | CSV 分隔符；`'\0'` 时 `.tsv` 用 `\t`，否则 `,` |
| `fill_gaps` | `bool` | `false` | 仅 XLSX：为跳过的行号生成 `Null` 行 |
| `lenient_columns` | `bool` | `false` | 仅 CSV：多余字段截断而不是抛 `ColumnCountMismatch` |
| `max_columns` | `uint32_t` | 16384 | 所有表格式的列数上限 |
| `column_count` | `uint32_t` | 0 | 仅 CSV：覆盖列数（标题行比数据窄时有用） |
| `max_field_bytes` | `size_t` | 8 MiB | CSV 单字段/JSONL 单行字节上限 |
| `max_rows` | `size_t` | 0 | 数据行上限；0=不限 |
| `integral_numbers_as_int` | `bool` | `true` | CSV/XLSX 表 reader：整数值用 `Int` 而不是 `Number` |
| `xlsx` | `OpenOptions` | `{}` | XLSX 打开选项 |

### 6.3 `ReadOptions` 字段对应关系

| 字段 | XLSX | CSV | JSONL |
|---|---|---|---|
| `sheet` | 是 | 否 | 否 |
| `header_row` | 是 | 是 | 忽略（始终 0） |
| `infer_types` | 否 | 是 | 否（类型来自 JSON） |
| `delimiter` | 否 | 是 | 否 |
| `fill_gaps` | 是 | 否 | 否 |
| `lenient_columns` | 否 | 是 | 否 |
| `column_count` | 否 | 是 | 否 |
| `max_field_bytes` | 否 | 是 | 是（单行） |
| `max_columns` | 是 | 是 | 是 |
| `max_rows` | 是 | 是 | 是 |

### 6.4 `Reader` 接口

| 方法 | 返回 | 说明 |
|---|---|---|
| `format()` | `Format` | 实际格式 |
| `columns()` | `std::span<const std::string_view>` | 列名，reader 存活期间稳定 |
| `header_row()` | `uint32_t` | 生效的表头行；0=无表头/JSONL |
| `next(row)` | `bool` | 读取下一数据行；false=结束 |
| `rows_read()` | `uint64_t` | 已读取数据行数 |
| `extra_cells()` | `uint64_t` | CSV 被 `lenient_columns` 截断的多余字段数 |

`RowView`：

| 字段/方法 | 含义 |
|---|---|
| `number` | 1-based 行号 |
| `cells` | 密集 span；`row[i]` 对应第 `i+1` 列 |
| `size()/empty()` | 列数/是否为空 |
| `find(col)` | 按 1-based 列号查找；不存在返回 `nullptr` |

表 reader 的行是**密集**的，会补齐到 `columns().size()`；空列为 `CellType::Null`。

### 6.5 `WriteOptions`

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `format` | `Format` | `Auto` | 输出格式；流输出必须显式 |
| `header` | `bool` | `true` | CSV 是否写表头；XLSX 是否写表头行；JSONL 忽略 |
| `delimiter` | `char` | `'\0'` | CSV 分隔符；`'\0'` 自动 |
| `crlf` | `bool` | `false` | CSV 行尾用 `\r\n` |
| `bom` | `bool` | `false` | CSV 写 UTF-8 BOM |
| `fsync` | `bool` | `true` | 提交前 fsync |
| `compression_level` | `int` | 6 | XLSX ZIP 压缩 0..9；0=不压缩 |
| `sheet_name` | `std::string` | `"Sheet1"` | XLSX 工作表名 |
| `temp_dir` | `optional<path>` | 空 | 临时文件目录；必须与目标同卷 |

### 6.6 `Writer` 接口

| 方法 | 参数 | 说明 |
|---|---|---|
| `format()` | — | 实际输出格式 |
| `write_row(cells)` | `std::span<const CellView>` | 单元格数必须等于列数；调用期间消费 cells |
| `finish()` | — | 原子发布；第二次调用无操作；之后 `write_row` 抛 `Closed` |

析构前未 `finish()` 会丢弃临时文件。XLSX 的 `CellView.col` 若为 0，按 span 顺序填
1..N；若显式设置，按列稀疏写入，但 span 长度仍需等于列数。

---

## 7. 写入 XLSX：WorkBookWriter

### 7.1 `BookWriteOptions`

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `date_1904` | `bool` | false | 使用 1904 日期系统 |
| `strings` | `StringStorage` | `Shared` | `Shared` 去重共享字符串；`Inline` 全部内联 |
| `max_shared_strings_bytes` | `size_t` | 512 MiB | 共享字符串内存上限；超出后新字符串回退为内联 |
| `fsync` | `bool` | true | 提交前 fsync |
| `compression_level` | `int` | 6 | ZIP 压缩 0..9 |
| `temp_dir` | `optional<path>` | 空 | 临时目录；必须与目标同卷 |

`StringStorage`：`Shared` / `Inline`。

### 7.2 `WorkBookWriter`

```cpp
std::vector<std::string_view> names = {"Data", "Params"};
auto writer = fastxl::WorkBookWriter::create("out.xlsx", names, options);
auto& sheet = writer.sheet(0);
// ... 写数据 ...
writer.commit();
```

| 方法 | 参数 | 说明 |
|---|---|---|
| `create(target, sheet_names, options)` | 路径、名称 span、`BookWriteOptions` | 名称需非空、≤31 个 UTF-16 单元、唯一、不含 `[]:*?/\\`、不以 `'` 开头/结尾、不能是保留名 `History` |
| `sheet(index)` | `size_t index` | 必须按 0,1,2… 顺序打开；返回可写句柄 |
| `commit()` | — | 关闭所有未完成的表、写 sharedStrings、关闭 ZIP、原子替换 |
| `abort()` | — | 丢弃临时文件；可重复调用 |

`WorkBookWriter` 是 move-only。`commit()` 会自动完成尚未打开的表。

### 7.3 `WorkSheetWriter`

| 方法 | 参数 | 说明 |
|---|---|---|
| `set_column_width(col, chars)` | `col` 1..16384，`chars`>0 | 设置列宽（字符单位） |
| `set_freeze_rows(rows)` | `rows` 0..1048576 | 冻结前 N 行；0 取消 |
| `set_auto_filter(enable)` | `bool` | 是否写 `autoFilter` |
| `merge_cells(range)` | `MergeRange`，1-based 闭区间 | 合并范围不能越界或重叠 |
| `write_row(row, cells, options)` | `row` 严格递增，从 1 开始；`cells` 按 col 递增；`RowOptions` | 写入一行；空单元格可省略 |

`RowOptions`：

| 字段 | 默认 | 含义 |
|---|---|---|
| `bold` | false | 该行所有单元格使用粗体样式 |

`write_row` 规则：

- `row` 必须严格大于上一行，范围 1..1048576。
- 每个 `cell.col` 必须在 1..16384，且按递增顺序。
- `CellView::text` 只在调用期间借用；调用返回后 writer 不再引用。
- 文本超过 32767 个 UTF-16 单元抛 `LimitExceeded`。
- 非有限浮点（NaN/Inf）抛 `InvalidValue`。
- 文本必须是合法 UTF-8，否则抛 `InvalidUtf8`。
- 公式文本不会写入；`CellView::formula` 仅作为读取信息。

### 7.4 样式与数字格式

写入器的样式表固定 18 个 `cellXfs`：9 种数字格式 + 对应粗体。`CellView::format`
使用 `NumberFormat`：

| `NumberFormat` | 写入样式 | 显示 |
|---|---|---|
| `General` | 0 | 常规 |
| `Integer` | 1 | 整数 |
| `Decimal2` | 2 | 两位小数 |
| `Percent` | 3 | 百分比 |
| `Percent2` | 4 | 两位小数百分比 |
| `Time` | 5 | `h:mm:ss` |
| `Date` | 6 | `yyyy-mm-dd` |
| `DateTime` | 7 | `yyyy-mm-dd hh:mm:ss` |
| `Text` | 8 | 文本 |

`CellType::Date/DateTime/Time` 会自动使用对应样式；`Number`/`Int` 使用
`CellView::format`。样式索引 ≥9 会回退到 `General`。

---

## 8. 概览分析：Profile

### 8.1 `ProfileOptions`

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `header_row` | `uint32_t` | 1 | 1-based 表头行；0=无表头 |
| `max_rows` | `size_t` | 0 | 最多扫描的数据行数；0=不限。表头不计入 |
| `distinct_cap` | `size_t` | 10000 | 每列 distinct/频率映射上限；0 表示不做 distinct |
| `samples` | `size_t` | 5 | 每列 sample 数；0 关闭 |
| `top_values` | `size_t` | 5 | 每列高频值数；0 关闭 |
| `jobs` | `unsigned` | 0 | 0=硬件并发（上限 8）；1=串行；>1=缓冲 part 上并行 |

并行条件：`jobs>1`、`max_rows==0`、工作表 part ≤ `in_memory_threshold`。否则自动串行，
`SheetProfile::parallel` 为 false。

### 8.2 `profile_sheet` / `profile_table`

```cpp
SheetProfile p = profile_sheet(book, 0, options);
SheetProfile t = profile_table(*reader, options);
```

| 参数 | 含义 |
|---|---|
| `book` | 已打开的 `WorkBook`；应使用默认 `SharedStringMode::Load` |
| `sheet` | 0-based 工作表索引；越界抛 `InvalidSheetRef` |
| `options` | 见上 |
| `reader` | 任意表格式 `Reader`；`profile_table` 串行 |

`profile_table` 使用 reader 的 `columns()` 作为表头名，`header_row()` 作为表头行；
`--max-rows` 语义与 `profile_sheet` 一致。

### 8.3 `SheetProfile`

| 字段 | 含义 |
|---|---|
| `file` | 文件路径（CLI 传 stdin 时为 `-`） |
| `sheet_name` / `sheet_index` | 表名、0-based 索引 |
| `header_row` / `header_rows` | 生效表头行；`header_rows` 为 0 或 1 |
| `rows_scanned` | 实际检查的数据行数，**不含表头** |
| `data_rows` | 由行号推出的数据行数：`max_row - header_row` |
| `max_row` / `max_col` | 最大行号/列号（1-based） |
| `empty_rows` | 空行元素数 |
| `ragged_rows` | 单元格数与最大列不一致的行数 |
| `merged_ranges` | 合并区域数 |
| `error_cells` | Error 类型单元格数 |
| `has_formulas` | 是否出现公式 |
| `parallel` | 是否走了并行 profile |
| `used_range` | `<dimension ref>` 原值（若有） |
| `auto_filter` | `autoFilter ref`（若有） |
| `pane_top_row` | 冻结窗格 `topLeftCell` 行号；0=无 |
| `hidden_columns` | 隐藏列索引（最多 256 个） |
| `columns` | 列统计 |
| `issues` | 解析问题文本，最多 100 条 |

### 8.4 `Column`

| 字段 | 含义 |
|---|---|
| `index` | 1-based 列号 |
| `header` | 表头文本；无表头时为空 |
| `non_null` | 非空单元格数（空文本按空处理） |
| `nulls` | `data_rows - non_null`（不小于 0） |
| `type_counts` | 11 个 `CellType` 桶的计数；SharedString 计入 Text，Int 计入 Number；Null 通常不进入统计（空值由 `non_null`/`nulls` 体现） |
| `numeric` | 数值统计 |
| `distinct` | canonical text 的 distinct 数；capped 时不超过 `distinct_cap` |
| `distinct_capped` | 是否触达 cap |
| `min_text_len` / `max_text_len` | Text 值的 UTF-8 码点长度范围 |
| `samples` | 按 (row,col) 最小顺序的前几个 distinct canonical text，单值截断到 128 字节 |
| `sample_truncated` | 是否有 sample 被截断 |
| `top_values` | canonical text 的频率列表，按 count 降序、value 升序 |
| `enum_values` | 内联 data validation list 的值，最多 64 个 |
| `flags` | 见下 |

`NumericProfile`：

| 字段 | 含义 |
|---|---|
| `any` | 是否出现有限数值 |
| `min` / `max` / `mean` / `sum` | double 统计；`sum/mean` 并行时可能有末位差异 |
| `non_finite` | NaN/Inf 数量 |
| `integers` | 整数值数量（Int 单元格 + 落在 9e15 内的 integral double） |
| `int_any` | 是否出现 Int 单元格 |
| `int_min` / `int_max` | Int 单元格的精确 int64 范围 |

`ValueCount`：`value`（canonical text）、`count`。

`flags` 可能值：

| flag | 条件 |
|---|---|
| `mixed_types` | 非 Invalid 桶超过 1 个 |
| `has_error` | 出现 Error |
| `has_formula` | 该列出现公式 |
| `whitespace_edges` | Text 前后有空白/NBSP |
| `blank_like_text` | Text 只含空白/NBSP |
| `empty_header` | 有表头但该列表头为空 |
| `duplicate_header` | 表头与其他列重复 |
| `mostly_null` | 超过一半行为空 |
| `constant` | 只有一个 distinct 值 |
| `unique` | 无 null、未 capped、distinct 等于 non_null |

### 8.5 类型推断

CLI `schema` 把 `Column` 映射为：

| 条件 | `type` |
|---|---|
| `non_null==0` | `null` |
| 只有 Invalid | `invalid` |
| 多个非 Invalid 桶 | `mixed` |
| Number | 所有值 integral → `integer`，否则 `number` |
| Text/SharedString | `text`；若值是 ISO 8601 文本，profile 已计入 `date`/`datetime` |
| Boolean | `boolean` |
| Error/Date/DateTime/Time | 同名 |
| Int | `integer` |

文本日期只在 profile 中分类，不会改变 `CellView.type`；非 ISO 写法（如
`03/04/2024`）仍是 text/mixed。

### 8.6 示例

```cpp
fastxl::WorkBook book = fastxl::WorkBook::open("book.xlsx");
fastxl::ProfileOptions options;
options.header_row = 2;       // 标题行在第 1 行
options.top_values = 5;
options.jobs = 4;
fastxl::SheetProfile profile = fastxl::profile_sheet(book, 0, options);
for (const auto& column : profile.columns) {
  std::cout << column.header << " type=" << column.distinct << "\n";
}
```

---

## 9. 值、日期与文本工具

### 9.1 `to_text`

```cpp
std::string owned = fastxl::to_text(value);       // Value
std::string owned2 = fastxl::to_text(cell);       // CellView
std::string scratch;
std::string_view borrowed = fastxl::to_text(cell, scratch); // 无分配（文本直接借用）
```

规范形式：

| 类型 | 文本 |
|---|---|
| `Null` | `""` |
| `Int` | 十进制整数，如 `-5` |
| `Number` | 最短可回读表示，如 `2.5`；非有限为 `nan`/`inf`/`-inf` |
| `Boolean` | `true`/`false` |
| `Text`/`SharedString` | 原样 UTF-8 |
| `Date` | `YYYY-MM-DD` |
| `DateTime` | `YYYY-MM-DDTHH:MM:SS`，毫秒非零时加 `.mmm` |
| `Time` | `HH:MM:SS`，小时可超过 23 或为负 |
| `Error` | 原始错误文本，否则 `#DIV/0!` 等 |

### 9.2 `normalize_value`

```cpp
fastxl::CellView normalized = fastxl::normalize_value(cell);
fastxl::Value normalized_value = fastxl::normalize_value(value);
```

- Text/SharedString：去掉 ASCII 空白和 U+00A0 两端；空结果变成 `Null`。
- Error：变成 `Null`。
- 其他类型原样返回。
- `noexcept`，不分配（文本 view 指向原缓冲区）。

### 9.3 `Value`

`Value` 拥有 `text`，字段与 `CellView` 对应。工厂：

```cpp
Value::null()
Value::integer(int64_t)
Value::real(double, NumberFormat = General)
Value::boolean_value(bool)
Value::string(std::string_view)       // 复制
Value::date(Date)
Value::datetime_value(DateTime)
Value::time(double seconds)
Value::error_value(ErrorValue, std::string_view raw = {})
```

`Value::view()` 返回借用 `CellView`；`Value::error_text()` 返回错误文本。

### 9.4 日期函数

| 函数 | 参数 | 说明 |
|---|---|---|
| `from_serial(serial, date1904, out, date_only)` | serial、日期系统、输出 | Excel serial → `DateTime`；serial 60 或越界返回 false |
| `to_serial(dt, date1904)` | `DateTime`、日期系统 | 反向转换；不可表示抛 `InvalidDate` |
| `time_seconds_from_serial(serial)` | serial | 取小数部分为秒，整天不 wrap |
| `format_iso8601(dt)` | `DateTime` | 规范 ISO 文本 |
| `format_time(seconds)` | `double` | `HH:MM:SS`，小时可 >23 |
| `parse_iso8601(text, out, date_only)` | 文本、输出 | 接受 `YYYY-MM-DD` 和 `YYYY-MM-DDTHH:MM[:SS[.fff]]`；校验时区但不转换 |

### 9.5 格式辅助

| 函数 | 说明 |
|---|---|
| `format_from_string(name)` | 大小写不敏感，忽略 `.`；`xlsx/xlsm`→Xlsx，`csv/tsv`→Csv，`jsonl/ndjson/json`→Jsonl，其他→Auto |
| `format_from_path(path)` | 按扩展名调用上者 |
| `to_string(Format)` | `"auto"`/`"xlsx"`/`"csv"`/`"jsonl"` |
| `to_string(CellType)` | 核心类型名（见 3.2） |

---

## 10. 错误处理

所有库错误都通过 `fastxl::Exception` 抛出：

```cpp
try {
  auto book = fastxl::WorkBook::open("book.xlsx");
} catch (const fastxl::Exception& e) {
  fastxl::ErrorCode code = e.code();
  const char* message = e.what();
}
```

`ErrorCode`：

| 值 | 典型原因 |
|---|---|
| `FileNotFound` | 文件不存在/无法打开 |
| `NotAZip` | 不是 ZIP/OPC 容器（例如把 CSV 当 xlsx） |
| `UnsupportedEncryption` | 加密条目 |
| `UnsupportedCompression` | bzip2/lzma/zstd 等不支持的 ZIP 压缩 |
| `UnsupportedFormat` | 无法从扩展名判断格式 |
| `MissingPart` | OPC 必需 part 缺失 |
| `MalformedXml` | XML 结构错误、标签不匹配 |
| `InvalidUtf8` | 非法 UTF-8 |
| `UnsupportedEncoding` | UTF-16 XML 等 |
| `InvalidSheetRef` | 工作表索引/名称错误 |
| `MissingRelationship` | 关系缺失/无法解析 |
| `SharedStringOutOfRange` | 共享字符串索引越界 |
| `SharedStringsNotLoaded` | 打开时跳过/只计数，却调用 `shared_string` |
| `SharedStringsTooLarge` | 共享字符串超过 `max_shared_strings_bytes` |
| `InvalidNumber` / `InvalidDate` / `InvalidValue` | 值/日期/参数非法 |
| `ZipReadError` | ZIP 损坏/截断 |
| `IoError` | 读写失败 |
| `SheetNameInvalid` / `SheetNameDuplicate` | 输出表名不符合 Excel 规则/重复 |
| `SheetOrderViolation` | writer 未按索引顺序打开表 |
| `WorkbookAlreadyStarted` | 工作簿已开始写入 |
| `CellOrderViolation` | `write_row` 行/列顺序错误 |
| `LimitExceeded` | 行列/文本/字段/列数等超限 |
| `Closed` | writer 已 finish/关闭 |
| `AtomicReplaceFailed` | 临时文件无法替换目标（跨卷等） |
| `Aborted` | 操作被中止 |
| `ColumnCountMismatch` | 表读写列数不一致 |
| `JsonSchemaMismatch` | JSONL 新对象 key 与首行不一致 |
| `EmptyTable` | 空表/无列/无表名 |
| `Internal` | 内部状态错误 |

CLI 退出码：

| 码 | 含义 |
|---|---|
| 0 | 成功（含不带参数和 `--help` 的用法输出） |
| 1 | 用法错误（`UsageError`：未知命令、参数非法） |
| 2 | 输入/格式错误（含 `InvalidUtf8`、`MalformedXml`） |
| 3 | 输出/原子替换错误（`IoError`、`AtomicReplaceFailed`） |
| 4 | 内部错误（`Internal`、`Closed`、顺序错误等） |

JSON 失败对象包含 `error.code`、`error.message` 和 `error.exit_code`。
`fastxl-cli errors [--json]` 列出全部错误码、对应的退出码和一行解释。

---

## 11. 生命周期、线程与内存

**借用生命周期**

- SAX：`CellView::text`/`formula` 只在本次 `on_cell` 回调内有效。
- 表 reader：`RowView`/`CellView` 有效到下一次 `next()`。
- `columns()` 在 reader 析构前稳定。
- `Value`/`Cell` 拥有文本，可长期保存。

**线程**

- `WorkBook` 只读，但同一时刻只允许一个活动 `WorkSheetReader`（`open_sheet` 会检查）。
- `read_sheet`/`WorkSheet::read` 是同步调用；并行 profile 在库内部开线程。
- `WorkBookWriter` 不是线程安全的；单线程写入。
- `DomWorkBook` 通过 `shared_ptr` 持有数据，读取是 const。

**内存**

- XLSX part ≤ `in_memory_threshold` 整块缓冲；更大时 256 KiB 窗口流式。
- 共享字符串默认上限 512 MiB。
- DOM 内存随单元格数线性增长；大表请用 SAX/Profile。
- 写入共享字符串达到上限后，新唯一字符串回退 inline，仍能完成输出。

---

## 12. 支持范围与限制

**支持**

- XLSX 读取：值、共享字符串、日期样式、合并、冻结窗格、自动筛选、超链接、
  数据验证（type/sqref/formula1/formula2）、行列隐藏/宽度/高度、公式文本。
- XLSX 写入：单/多工作表、共享字符串去重、ZIP64、冻结行、自动筛选、合并、
  18 个固定样式。
- CSV：多行字段、BOM、可配置分隔符、类型推断、列投影、CRLF。
- JSONL：每行一个对象，嵌套值保持紧凑 JSON 文本。
- 表格式互转；stdin/stdout；原子写入。

**不支持/限制**

- 无 `.xls`/CFB、加密、ODS、Parquet、bzip2/lzma/zstd ZIP。
- 不计算公式；写入丢弃公式。
- 样式固定：无自定义颜色、边框、字体、图片、图表、批注、透视表、条件格式。
- 数据验证只暴露 type/sqref/formula；不解析区域/跨表引用。
- 并行 profile 只在缓冲 part 上生效；流式 part 串行。
- CSV/JSONL 只支持 UTF-8。
- 文本日期只识别 ISO 8601。
- DOM 无默认上限，但大表会占大量内存。

---

## 13. CLI 速览

```sh
fastxl-cli info <file.xlsx> [--json]
fastxl-cli preview <file|-> [--sheet ...] [--header-row N|auto|--no-header]
                          [--rows N] [--range A1:F20] [--columns 1,3-5]
                          [--max-cell-bytes N]
fastxl-cli probe <file|-> [--sheet ...] [--header-row N|auto|--no-header]
                          [--max-rows N] [--sample N] [--top N]
                          [--distinct-cap N] [--jobs N] [--json]
fastxl-cli schema <file|-> [--sheet ...] [--header-row N|auto|--no-header]
                          [--max-rows N] [--sample N] [--top N]
                          [--distinct-cap N] [--jobs N]
fastxl-cli convert <in|-> --out <out|-> [--from ...] [--to ...]
                          [--sheet ...] [--header-row N|--no-header]
                          [--delimiter C] [--infer|--no-infer]
                          [--normalize] [--fill-gaps] [--columns 1,3-5]
                          [--limit N] [--crlf] [--bom]
                          [--sheet-name NAME] [--compress-level N]
fastxl-cli errors [--json]
fastxl-cli --version
fastxl-cli --help
fastxl-cli <command> --help
```

约定：

- `fastxl-cli --help` 打印命令概览，`fastxl-cli <command> --help` 打印该命令的
  全部选项、默认值和示例；不带任何参数等价于 `--help`，退出码为 0。
- `--sheet` 接受名称或 1-based 索引；JSON 的 `sheet_index` 是 0-based。
- `--version` 输出 fastxl 版本、ZIP 栈（minizip-ng + zlib-ng）和编译时关闭的后端。
- `probe/schema/preview` 接受 `-`，但必须用 `--from` 指定格式；stdin 会 spool 到临时文件。
- 机器可读 JSON 成功时带 `"version":1,"ok":true`；失败时 stderr 为
  `{"version":1,"ok":false,"error":{"code":...,"message":...,"exit_code":...}}`。
- `schema`/`preview` 始终 JSON；`info`/`probe` 需要 `--json`。
- MCP 包装：`python projects/tools/fastxl_mcp.py --cli <fastxl-cli>`；工具名为
  `fastxl_info`、`fastxl_preview`、`fastxl_schema`、`fastxl_probe`。

---

## 14. 最小示例

**SAX 读取**

```cpp
#include "fastxl/fastxl.hpp"
#include <iostream>

int main() {
  auto book = fastxl::WorkBook::open("book.xlsx");
  auto sheet = book.sheet(0);
  fastxl::SaxCallbacks callbacks;
  callbacks.cell = [](const fastxl::CellView& cell) {
    std::string scratch;
    std::cout << cell.row << "," << cell.col << "="
              << fastxl::to_text(cell, scratch) << "\n";
    return true;
  };
  sheet.read(callbacks);
}
```

**表格式转换**

```cpp
#include "fastxl/fastxl.hpp"
#include <vector>

int main() {
  auto reader = fastxl::open_reader("in.csv");
  auto writer = fastxl::open_writer("out.xlsx", reader->columns());
  std::vector<fastxl::CellView> cells;
  fastxl::RowView row;
  while (reader->next(row)) {
    cells.assign(row.cells.begin(), row.cells.end());
    writer->write_row(cells);
  }
  writer->finish();
}
```

**写多表工作簿**

```cpp
#include "fastxl/fastxl.hpp"
#include <vector>

int main() {
  std::vector<std::string_view> names = {"Data"};
  auto writer = fastxl::WorkBookWriter::create("out.xlsx", names);
  auto& sheet = writer.sheet(0);
  sheet.set_freeze_rows(1);
  sheet.set_auto_filter(true);
  sheet.merge_cells({1, 1, 1, 2});
  std::vector<fastxl::CellView> row = {
      {1, fastxl::CellView::integer(1)},
      {2, fastxl::CellView::date({2024, 1, 2})},
      {3, fastxl::CellView::string("ok")},
  };
  sheet.write_row(1, row);
  writer.commit();
}
```

**Profile**

```cpp
#include "fastxl/fastxl.hpp"
#include <iostream>

int main() {
  auto book = fastxl::WorkBook::open("book.xlsx");
  fastxl::ProfileOptions options;
  options.header_row = 1;
  options.jobs = 4;
  options.top_values = 5;
  auto profile = fastxl::profile_sheet(book, 0, options);
  for (const auto& column : profile.columns) {
    std::cout << column.header << " distinct=" << column.distinct
              << " nulls=" << column.nulls << "\n";
  }
}
```
