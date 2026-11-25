-- Python bindings must match the release CPython CRT. Build them with
-- `xmake f -m release`; debug-mode Pythons are a separate ABI.
if is_mode("debug") then
    return
end

local function first_file(pattern)
    local matches = os.files(pattern)
    if matches and #matches > 0 then
        return matches[1]
    end
    return nil
end

local py_include = os.getenv("FASTXL_PY_INCLUDE")
local py_libdir = os.getenv("FASTXL_PY_LIBDIR")
local py_library = os.getenv("FASTXL_PY_LIBRARY")
local pybind_include = os.getenv("FASTXL_PYBIND11_INCLUDE")
local ext = ".so"

if is_plat("windows") then
    py_include = py_include or "C:/Apps/py/py3.15/Include"
    py_libdir = py_libdir or "C:/Apps/py/py3.15/libs"
    py_library = py_library or "python315"
    pybind_include = pybind_include or "C:/Apps/py/py3.15/Lib/site-packages/pybind11/include"
    ext = ".pyd"
else
    if not py_include then
        local header = first_file("/usr/include/python3*/Python.h") or
                       first_file("/usr/local/include/python3*/Python.h")
        if header then
            py_include = path.directory(header)
        end
    end
    if not pybind_include then
        local header = first_file(os.projectdir() ..
                                      "/.venv/lib/python3*/site-packages/pybind11/include/pybind11/pybind11.h") or
                       first_file("/usr/lib/python3*/site-packages/pybind11/include/pybind11/pybind11.h") or
                       first_file("/usr/local/lib/python3*/site-packages/pybind11/include/pybind11/pybind11.h") or
                       first_file("/usr/include/pybind11/pybind11.h")
        if header then
            pybind_include = path.directory(path.directory(header))
        end
    end
end

target("fastxl-python")
    set_kind("shared")
    set_default(false)
    set_languages("c++20")
    add_files("fastxl_module.cpp")
    add_files("../projects/fastxl-cli/src/headers.cpp")
    add_files("../projects/fastxl-cli/src/preview_json.cpp")
    add_files("../projects/fastxl-cli/src/args.cpp")
    add_files("../projects/fastxl-cli/src/common.cpp")
    if py_include then
        add_includedirs(py_include)
    end
    if pybind_include then
        add_includedirs(pybind_include)
    end
    add_includedirs("../projects/fastxl-cli/src")
    if py_libdir then
        add_linkdirs(py_libdir)
    end
    if py_library then
        add_links(py_library)
    end
    add_deps("fastxl-core")
    add_packages("fmt", "nlohmann_json")
    set_extension(ext)
    set_basename("_fastxl")
    set_prefixname("")
    set_targetdir("$(projectdir)/build/python/fastxl")
    after_build(function (target)
        -- Keep the importable package self-contained in build/python/fastxl,
        -- including PEP 561 type information.
        local src = path.join(os.projectdir(), "bindings", "python", "fastxl")
        local dst = path.join(os.projectdir(), "build", "python", "fastxl")
        for _, name in ipairs({ "__init__.py", "_fastxl.pyi", "py.typed" }) do
            os.cp(path.join(src, name), dst)
        end
    end)
    if is_plat("windows") then
        add_cxflags("/utf-8")
        set_runtimes("MD")
    else
        if is_plat("linux") then
            add_cxflags("-pthread")
            add_ldflags("-pthread")
        end
    end
