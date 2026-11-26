-- The CLI shell: argument parsing, help, console rendering and dispatching to
-- the shared report builders in libfastxl. Product name: `fastxl`.
target("fastxl_cli")
set_kind("binary")
set_basename("fastxl")
set_targetdir("$(projectdir)/build/bin")
add_files("src/*.cpp")
add_deps("fastxl_reports")
add_packages("fmt", "nlohmann_json")
add_defines('FASTXL_VERSION="' .. FASTXL_PROJECT_VERSION .. '"')
if is_plat("windows") then
    add_syslinks("shell32")
elseif is_plat("linux") then
    add_cxflags("-pthread")
    add_ldflags("-pthread")
end
set_rundir("$(projectdir)")
on_install(function (target)
    local bin_dir = path.join(target:installdir(), "bin")
    os.mkdir(bin_dir)
    os.cp(target:targetfile(), bin_dir)
end)
