-- Binary SDK: one shared library, fastxl_c.h and fastxl_cxx.hpp. The report
-- builders are the CLI sources, so the SDK and CLI output cannot drift.
target("fastxl-c")
set_kind("shared")
set_basename("fastxl")
set_targetdir("$(projectdir)/build/lib")
add_defines("FASTXL_C_EXPORTS")
add_defines('FASTXL_VERSION="' .. FASTXL_PROJECT_VERSION .. '"')
add_files("src/*.cpp")
add_files("../fastxl-cli/src/common.cpp",
          "../fastxl-cli/src/headers.cpp",
          "../fastxl-cli/src/preview_json.cpp",
          "../fastxl-cli/src/args.cpp",
          "../fastxl-cli/src/table.cpp",
          "../fastxl-cli/src/cmd_info.cpp",
          "../fastxl-cli/src/cmd_schema.cpp",
          "../fastxl-cli/src/cmd_probe.cpp",
          "../fastxl-cli/src/cmd_search.cpp",
          "../fastxl-cli/src/cmd_preview.cpp",
          "../fastxl-cli/src/cmd_convert.cpp")
add_includedirs("include", {public = true})
add_includedirs("../fastxl-cli/src")
add_deps("fastxl-core")
add_packages("fmt", "nlohmann_json", "simdutf")
add_headerfiles("include/(fastxl_c.h)", "include/(fastxl_cxx.hpp)")

if is_plat("windows") then
    add_cxflags("/utf-8")
    -- fastxl-core is built MDd in debug; a blanket MD here breaks that link.
    if is_mode("release") then
        set_runtimes("MD")
    end
else
    add_cxflags("-fvisibility=hidden", "-fvisibility-inlines-hidden")
    if is_plat("linux") then
        add_cxflags("-pthread")
        add_shflags("-pthread",
                    "-Wl,--exclude-libs,ALL",
                    "-Wl,--version-script=" .. path.join(os.projectdir(),
                                                        "projects/fastxl-c/fastxl.map"),
                    {force = true})
    end
end

after_build(function (target)
    -- The ctypes package ships next to the built library for local testing.
    local dst = path.join(os.projectdir(), "build", "lib", "fastxl_ctypes")
    os.mkdir(dst)
    os.cp(path.join(os.projectdir(), "projects", "fastxl-c", "python", "fastxl_ctypes", "*"),
          dst)
end)

on_install(function (target)
    local prefix = target:installdir()
    local lib_dir = path.join(prefix, "lib")
    local include_dir = path.join(prefix, "include")
    os.mkdir(lib_dir)
    os.mkdir(include_dir)
    os.cp(target:targetfile(), lib_dir)
    if is_plat("windows") then
        -- Without the import library (fastxl.lib / libfastxl.dll.a) an
        -- external project cannot link against the installed DLL.
        local implib = target:artifactfile("implib")
        if not implib or not os.isfile(implib) then
            raise("missing the fastxl import library; rebuild fastxl-c")
        end
        os.cp(implib, lib_dir)
    end
    for _, name in ipairs({"fastxl_c.h", "fastxl_cxx.hpp"}) do
        os.cp(path.join(os.projectdir(), "projects", "fastxl-c", "include", name), include_dir)
    end
end)
