target("fastxl-cli")
set_kind("binary")
set_targetdir("$(projectdir)/build/bin")
add_files("src/*.cpp")
add_deps("fastxl-core")
add_packages("fmt", "nlohmann_json")
if is_plat("windows") then
    add_syslinks("shell32")
end
set_rundir("$(projectdir)")
