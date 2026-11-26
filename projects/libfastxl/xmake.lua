-- libfastxl: the C++20 core (static), the shared report builders (static) and
-- the C ABI shared library. The CLI and the pybind11 module call the report
-- builders too, so every surface emits the same payloads.

target("fastxl_core")
set_kind("static")
set_basename("fastxl_core")
set_targetdir("$(projectdir)/build/lib")
add_files("src/core/*.cpp")
add_files("src/core/simd/dispatch.cpp")
if is_arch("x64", "x86_64") then
    if is_plat("windows") then
        add_files("src/core/simd/avx2.cpp", {cxflags = "/arch:AVX2"})
    else
        add_files("src/core/simd/avx2.cpp", {cxflags = "-mavx2"})
    end
end
if is_arch("arm64", "aarch64") then
    add_files("src/core/simd/neon.cpp")
end
if is_plat("windows") then
    add_files("src/core/platform/atomic_win32.cpp")
    add_files("src/core/platform/mapped_win32.cpp")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS")
else
    add_files("src/core/platform/atomic_posix.cpp")
    add_files("src/core/platform/mapped_posix.cpp")
end
add_includedirs("include", {public = true})
add_includedirs("src/core")
add_packages("minizip-ng", "fast_float", "simdutf", "fmt", "nlohmann_json", "zlib-ng",
             {public = true})
add_headerfiles("include/(fastxl/*.hpp)")
if is_plat("windows") then
    add_cxflags("/W4", "/permissive-")
else
    add_cxflags("-Wall", "-Wextra", "-Wpedantic", "-fPIC")
    if is_plat("linux") then
        -- std::thread needs the POSIX thread compile/link contract on older glibc.
        add_cxflags("-pthread")
    end
end
after_build(function (target)
    -- Ship public headers beside the static library.
    local include_dir = path.join(os.projectdir(), "build", "lib", "include")
    os.mkdir(include_dir)
    os.cp(path.join(os.projectdir(), "projects", "libfastxl", "include", "fastxl"),
          include_dir)
end)
on_install(function (target)
    local prefix = target:installdir()
    local lib_dir = path.join(prefix, "lib")
    local include_dir = path.join(prefix, "include")
    os.mkdir(lib_dir)
    os.mkdir(include_dir)
    os.cp(target:targetfile(), lib_dir)
    os.cp(path.join(os.projectdir(), "projects", "libfastxl", "include", "fastxl"),
           include_dir)
end)

-- Arg parsing and JSON report builders shared by the CLI, the C ABI and the
-- pybind11 module. Not installed on its own.
target("fastxl_reports")
set_kind("static")
set_basename("fastxl_reports")
set_targetdir("$(projectdir)/build/lib")
add_files("src/reports/*.cpp")
add_deps("fastxl_core")
add_includedirs("src/reports", {public = true})
add_packages("fmt", "nlohmann_json", {public = true})
if is_plat("windows") then
    add_cxflags("/utf-8")
else
    add_cxflags("-fPIC")
    if is_plat("linux") then
        add_cxflags("-pthread")
    end
end

-- Binary SDK: libfastxl.so / fastxl.dll, fastxl_c.h and fastxl_cxx.hpp.
target("fastxl")
set_kind("shared")
set_basename("fastxl")
set_targetdir("$(projectdir)/build/lib")
add_defines("FASTXL_C_EXPORTS")
add_defines('FASTXL_VERSION="' .. FASTXL_PROJECT_VERSION .. '"')
add_files("src/c_abi/*.cpp")
add_deps("fastxl_core", "fastxl_reports")
add_packages("fmt", "nlohmann_json", "simdutf")
add_headerfiles("include/(fastxl_c.h)", "include/(fastxl_cxx.hpp)")

if is_plat("windows") then
    add_cxflags("/utf-8")
    -- fastxl_core is built MDd in debug; a blanket MD here breaks that link.
    if is_mode("release") then
        set_runtimes("MD")
    end
else
    add_cxflags("-fvisibility=hidden", "-fvisibility-inlines-hidden")
    if is_plat("linux") then
        add_cxflags("-pthread")
        -- The SDK ships as a single .so: fold libstdc++/libgcc in so the
        -- binary has no GLIBCXX_*/GCC_* symbol-version floor (the glibc floor
        -- is set by the build image, see the manylinux job in release.yml).
        add_shflags("-pthread",
                    "-static-libstdc++",
                    "-static-libgcc",
                    "-Wl,--exclude-libs,ALL",
                    "-Wl,--version-script=" ..
                        path.join(os.projectdir(),
                                  "projects/libfastxl/src/c_abi/fastxl.map"),
                    {force = true})
    end
end

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
            raise("missing the fastxl import library; rebuild libfastxl")
        end
        os.cp(implib, lib_dir)
    end
    for _, name in ipairs({"fastxl_c.h", "fastxl_cxx.hpp"}) do
        os.cp(path.join(os.projectdir(), "projects", "libfastxl", "include", name),
              include_dir)
    end
end)
