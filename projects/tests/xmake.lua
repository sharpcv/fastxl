target("fastxl_tests")
set_kind("binary")
set_default(false)
add_files("cpp/*.cpp")
add_deps("fastxl_core")
add_includedirs("cpp", "../libfastxl/src/core")
set_rundir("$(projectdir)")
-- Keep symbols: sanitizer reports and crash stacks are useless when stripped.
set_strip("none")
if is_plat("linux") then
    -- The concurrency tests use std::thread.
    add_cxflags("-pthread")
    add_ldflags("-pthread")
end
