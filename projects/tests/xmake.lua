target("fastxl-tests")
set_kind("binary")
set_default(false)
add_files("*.cpp")
add_deps("fastxl-core")
add_includedirs(".", "../fastxl-core/src")
set_rundir("$(projectdir)")
-- Keep symbols: sanitizer reports and crash stacks are useless when stripped.
set_strip("none")
if is_plat("linux") then
    -- The concurrency tests use std::thread.
    add_cxflags("-pthread")
    add_ldflags("-pthread")
end
