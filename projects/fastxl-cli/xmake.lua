target("fastxl-cli")
set_kind("binary")
set_targetdir("$(projectdir)/build/bin")
add_files("src/*.cpp")
add_deps("fastxl-core")
add_packages("fmt", "nlohmann_json")
add_defines('FASTXL_VERSION="' .. FASTXL_PROJECT_VERSION .. '"')
if is_plat("windows") then
    add_syslinks("shell32")
elseif is_plat("linux", "bsd", "android") then
    add_cxflags("-pthread")
    add_ldflags("-pthread")
end
set_rundir("$(projectdir)")
