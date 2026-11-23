target("fastxl-core")
set_kind("static")
set_targetdir("$(projectdir)/build/lib")
after_build(function (target)
    -- Ship public headers beside the static library.
    local include_dir = path.join(os.projectdir(), "build", "lib", "include")
    os.mkdir(include_dir)
    os.cp(path.join(os.projectdir(), "projects", "fastxl-core", "include", "fastxl"),
          include_dir)
end)
add_files("src/*.cpp")
if is_plat("windows") then
    add_files("src/platform/atomic_win32.cpp")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS")
else
    add_files("src/platform/atomic_posix.cpp")
end
add_includedirs("include", {public = true})
add_includedirs("src")
add_packages("minizip-ng", "fast_float", "simdutf", "fmt", "nlohmann_json", {public = true})
add_headerfiles("include/(fastxl/*.hpp)")
if is_plat("windows") then
    add_cxflags("/W4", "/permissive-")
else
    add_cxflags("-Wall", "-Wextra", "-Wpedantic", "-fPIC")
    if is_plat("linux", "bsd", "android") then
        -- std::thread needs the POSIX thread compile/link contract on older glibc.
        add_cxflags("-pthread")
    end
end
