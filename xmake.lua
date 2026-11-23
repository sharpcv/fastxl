set_project("fastxl")
FASTXL_PROJECT_VERSION = "1.0.0"
set_version(FASTXL_PROJECT_VERSION)
set_license("MIT")
set_languages("c++20")
add_rules("mode.debug", "mode.release")

add_requires("minizip-ng 4.2.2", {configs = {zlib = true, bzip2 = false, lzma = false, zstd = false}})
add_requires("fast_float 8.3.3")
add_requires("simdutf 8.2.0")
add_requires("fmt 12.2.1")
add_requires("nlohmann_json 3.12.0")

includes("projects/fastxl-core", "projects/fastxl-cli", "bindings")
-- Local development only; not part of the published tree.
if os.isdir("projects/bench") then includes("projects/bench") end
if os.isdir("projects/tests") then includes("projects/tests") end

if is_plat("windows") then
    -- Source files (tests, CLI) contain UTF-8 literals.
    add_cxflags("/utf-8")
end
