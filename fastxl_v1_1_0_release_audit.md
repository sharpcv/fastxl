# fastxl v1.1.0 发布前终审报告与仓库清理清单

> 审计对象：[sharpcv/fastxl](https://github.com/sharpcv/fastxl)（`main` 分支至 commit `a7935a6`，M0–M8.1 完成态，CI 3/3 全绿）  
> 目标版本：**`v1.1.0`** ｜ 目标平台：**Linux（x86_64 / aarch64）** + **Windows（x64）** ｜ 构建系统：**xmake**

## 一、总体结论与评分

经过 M8 / M8.1 收口，上一轮审计中的所有 P0/P1 阻塞项（MCP `regions` / `max_cell_bytes` 参数、`fastxl_preview` 原生 `Session`、Windows `Session` 默认禁用 `mmap` 防锁文件、Windows MCP UTF-8 stdio、`json_budget.hpp` 下沉 Core、共享公式与主题色修正、2D Union-Find 区域切分、公开测试套件与多平台 CI）已全部闭环。

* **AI XLSX 探针完成度**：**92 / 100**（语义保真、多区域切分、跨表搜索、8 阶段预算折叠、常驻 MCP 已形成完整闭环）
* **高吞吐 C++20 库完成度**：**90 / 100**（多 Sheet 并发、大表流式并行、`mmap`、AVX2/NEON、`Result<T>`、Ranges、并行写与公式/自定义样式均已就绪）
* **发布结论**：**具备发布 `v1.1.0` 的功能条件**；发版前只需完成下文列出的**文档/文件清理、版本号更新**以及建议补齐的 **Windows Python/MCP CI 与 Schema 契约校验**。

## 二、M8 / M8.1 闭环核验总览

| 审计项 | 状态 | 核验依据 |
| :--- | :--- | :--- |
| MCP `TOOL_SCHEMAS` 参数补齐 | 已闭环 | [fastxl_mcp.py](https://github.com/sharpcv/fastxl/blob/main/projects/tools/fastxl_mcp.py) 已为 `fastxl_schema`/`fastxl_probe` 补齐 `regions`，为 `fastxl_search` 补齐 `max_cell_bytes`。 |
| `fastxl_preview` 走原生 `Session` | 已闭环 | 原生 `preview` 支持 `range`/`columns`/`max_cell_bytes`/`max_bytes`，仅非 `.xlsx` 或无原生模块时回退 CLI。 |
| MCP 确定性报错与 Windows UTF-8 | 已闭环 | `NativeError` 直接返回标准 JSON 错误对象；`configure_stdio()` 强制 UTF-8 解决 Windows 客户端乱码。 |
| Windows 常驻 `Session` 锁文件 | 已闭环 | [bindings/fastxl_module.cpp](https://github.com/sharpcv/fastxl/blob/main/bindings/fastxl_module.cpp) 在 `#ifdef _WIN32` 下 `default_memory_map()` 默认返回 `false`。 |
| `json_budget.hpp` 分层解耦 | 已闭环 | 原 `projects/fastxl-cli/src/json_budget.hpp` 已删除，统一移至 `projects/fastxl-core/include/fastxl/detail/json_budget.hpp`。 |
| 共享公式 / 主题色 / 2D 区域切分 | 已闭环 | 共享公式按 `si` 传播、主题色修正 `0↔1`/`2↔3` 槽位与 `sysClr`、`regions` 升级为 2D Union-Find。 |
| 公开测试与三平台 CI | 已闭环 | `projects/tests/`、`projects/bench/` 与 [ci.yml](https://github.com/sharpcv/fastxl/blob/main/.github/workflows/ci.yml)（Linux x64 / arm64 / Windows x64）已入库并全绿。 |

## 三、待删除文件、过时文案与仓库清理清单（发 1.1.0 前必做）

### 1. 建议移除或归档的过程性文件

| 文件路径 | 现状 | 处理建议 |
| :--- | :--- | :--- |
| `fastxl_optimization_roadmap.md`（根目录） | M0 阶段的规划草稿，目前挂在仓库根目录并被 `README.md` 链接 | **删除**（或移入 `docs/archive/`），同时移除 `README.md` 中的对应链接，避免根目录杂乱。 |
| `fastxl_m7_audit_and_m8_plan.md`（根目录） | M7 阶段的中间审计记录，问题均已在 M8/M8.1 修复 | **删除**，把核心变更提炼进 `CHANGELOG.md` 或 GitHub Release Notes。 |
| [docs/optimization-progress.md](https://github.com/sharpcv/fastxl/blob/main/docs/optimization-progress.md) | 详细的 M0–M8.1 开发流水账 | 可保留作历史记录，或精简后转为 `CHANGELOG.md` 的 `v1.1.0` 章节。 |

### 2. 必须修正的过时文案、死链与版本号

| 文件 | 发现的过时内容 / 缺陷 | 修改方式 |
| :--- | :--- | :--- |
| [xmake.lua](https://github.com/sharpcv/fastxl/blob/main/xmake.lua)（第 2 行） | `FASTXL_PROJECT_VERSION = "1.0.0"` | 升级为 `"1.1.0"`，并核对 `projects/tools/package_release.py`、`stage_skill.py` 中是否同步读取该版本。 |
| [xmake.lua](https://github.com/sharpcv/fastxl/blob/main/xmake.lua)（尾部注释） | `-- Local development only; not part of the published tree.` | `projects/tests` 与 `projects/bench` 现已正式入库并纳入 CI，**删除该过时注释**。 |
| [skill/SKILL.md](https://github.com/sharpcv/fastxl/blob/main/skill/SKILL.md) | Frontmatter 仍写 `"producing a new value-only XLSX."`；概述仍写 `"formulas are dropped on write"` | 更新为 M8.1 实际行为：写入与 `xlsx -> xlsx` 转换已保留 `<f>` 公式文本，同时保留“无公式求值引擎”的说明。 |
| [docs/limitations.md](https://github.com/sharpcv/fastxl/blob/main/docs/limitations.md) | `Reading → DOM is opt-in for large sheets` 中引用了不存在的 `dom-optimization.md` | **删除指向 `dom-optimization.md` 的死链**或补充对应说明。 |
| [.gitignore](https://github.com/sharpcv/fastxl/blob/main/.gitignore) | 已忽略 `bindings/python/fastxl/_fastxl.lib` 和 `skill/python/fastxl/_fastxl.*`，但未忽略 `bindings/python/fastxl/` 下的 `.pyd`/`.so` | 追加 `bindings/python/fastxl/_fastxl*.pyd` 与 `bindings/python/fastxl/_fastxl*.so`，防止本地编译产物误提交。 |

### 3. 过时/冗余代码排查结论

* `projects/fastxl-cli/src/json_budget.hpp`：**已清理干净**，无残留文件。
* `projects/fastxl-cli/src/` 与 `projects/tools/`：目录整洁，未发现废弃脚本或孤立源文件。
* **后续可重构项（不阻塞 1.1.0）**：核查 `projects/fastxl-cli/src/preview_json.cpp`、`metadata_json.hpp` 与 `bindings/fastxl_module.cpp` 之间是否存在重复的 JSON 序列化代码，后续版本可进一步收拢到 `fastxl-core/include/fastxl/detail/`。

## 四、发版前建议补齐的 4 项质量门禁

### 1. Windows CI 补跑 Python 绑定与 MCP 测试（强烈建议 1.1.0 前补上）
* 当前 [ci.yml](https://github.com/sharpcv/fastxl/blob/main/.github/workflows/ci.yml) 的 `windows` job 仅编译并运行了 `fastxl-tests.exe` 和 `test_cli.py`，**未编译 `fastxl-python`，也未跑 `test_python.py` 与 `test_mcp.py`**。
* 本轮两项关键 Windows 专属修复（`default_memory_map() == false` 防锁文件、MCP `configure_stdio()` 强制 UTF-8）尚未在 Windows CI 中实际跑过。建议在 `windows` job 中增加 `pip install pybind11`、编译 `fastxl-python` 并执行 `test_python.py` 与 `test_mcp.py`。

### 2. 增加 `docs/probe.schema.json` 自动化契约校验（强烈建议 1.1.0 前补上）
* 当前 [test_cli.py](https://github.com/sharpcv/fastxl/blob/main/projects/tests/test_cli.py) 与 [test_completeness.cpp](https://github.com/sharpcv/fastxl/blob/main/projects/tests/test_completeness.cpp) 均为手写字段断言，未引入 `jsonschema` 对照 [docs/probe.schema.json](https://github.com/sharpcv/fastxl/blob/main/docs/probe.schema.json) 做全量校验。
* 建议在 `test_cli.py` 和 `test_mcp.py` 中对 7 类输出（`info`、`preview`、`probe`、`schema`、`search`、`errors`、`error`）以及 `--max-bytes` 折叠态执行 `jsonschema.validate()`，彻底杜绝文档与实现漂移。

### 3. Linux CI 补齐 Sanitizer 与 `-fno-exceptions` 门禁（1.1.0 或 1.1.1）
* 目前 CI 全线仅跑 `-m release`。针对 SIMD、并发读、流式并行 `profile` 与并行写路径，建议在 Linux x64 新增一个 `sanitizer` job：
  * 跑一轮 **ASan + UBSan** 与一轮 **TSan** 覆盖 `fastxl-tests`。
  * 跑一轮 **`-fno-exceptions` 编译检查**，确保 `Result<T>` / `try_*` 接口真正适配禁异常宿主工程。

### 4. 发布流程收尾
* 补充 `CHANGELOG.md`（汇总 `v1.0.0 -> v1.1.0` 的全部新特性与修复）。
* 运行 `projects/bench/` 并将 Linux/Windows 实测吞吐与峰值内存（对比 `openpyxl` / `python-calamine`）写入 `README.md`。
* 重新执行 `python3 projects/tools/stage_skill.py` 确保 `skill/` 打包内容与 `v1.1.0` 同步，再打 `v1.1.0` tag 发布。

## 五、v1.1.0 发版检查单

- [ ] 删除根目录 `fastxl_optimization_roadmap.md`、`fastxl_m7_audit_and_m8_plan.md` 并清理 `README.md` 链接
- [ ] 更新 `xmake.lua` 版本号至 `1.1.0` 并删除过时注释
- [ ] 更新 `skill/SKILL.md` 过时公式描述，修复 `docs/limitations.md` 中 `dom-optimization.md` 死链
- [ ] `.gitignore` 补齐 `bindings/python/fastxl/_fastxl*.pyd` / `.so`
- [ ] Windows CI 补跑 `fastxl-python`、`test_python.py`、`test_mcp.py`
- [ ] `test_cli.py` / `test_mcp.py` 接入 `docs/probe.schema.json` 的 `jsonschema` 校验
- [ ] 重新运行 `stage_skill.py` 并打 `v1.1.0` tag
