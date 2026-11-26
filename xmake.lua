set_project("fastxl")
FASTXL_PROJECT_VERSION = "1.3.1"
set_version(FASTXL_PROJECT_VERSION)
set_license("MIT")
set_languages("c++20")
add_rules("mode.debug", "mode.release")

-- fastxl only reads/writes STORE and DEFLATE ZIP entries; encrypted or signed
-- ZIPs are rejected. Override the repo package so minizip-ng is rebuilt against
-- zlib-ng with every unused compression and encryption backend compiled out.
-- This keeps OpenSSL/libssl/libcrypto out of the build.
package("minizip-ng")
    set_homepage("https://github.com/zlib-ng/minizip-ng")
    set_description("Zip manipulation library (fastxl build: STORE + DEFLATE via zlib-ng).")
    set_license("zlib")

    add_urls("https://github.com/zlib-ng/minizip-ng/archive/refs/tags/$(version).tar.gz",
             "https://github.com/zlib-ng/minizip-ng.git")
    add_versions("4.2.2", "71af7b9799856d8b03619df3949e9c1be9703f8de0795af71399ba283cb27aac")

    add_deps("cmake")
    add_deps("zlib-ng", {configs = {zlib_compat = true}})
    if is_plat("windows") then
        add_syslinks("crypt32", "advapi32", "bcrypt")
    end

    on_install(function (package)
        local configs = {
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DMZ_ZLIB=ON",
            "-DMZ_ZLIB_FLAVOR=zlib",
            "-DMZ_BZIP2=OFF",
            "-DMZ_LZMA=OFF",
            "-DMZ_ZSTD=OFF",
            "-DMZ_PPMD=OFF",
            "-DMZ_LIBCOMP=OFF",
            "-DMZ_OPENSSL=OFF",
            "-DMZ_PKCRYPT=OFF",
            "-DMZ_WZAES=OFF",
            "-DMZ_FETCH_LIBS=OFF",
            "-DMZ_FORCE_FETCH_LIBS=OFF",
        }
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cincludes({"minizip/mz.h", "minizip/mz_zip.h"}))
    end)
package_end()

-- minizip-ng 4.2.2 links the CMake `ZLIB` target and zlib-ng 2.3.x only exports
-- `zlib-ng::zlib`, so build zlib-ng with its zlib-compatible API by default and
-- let find_package(ZLIB) resolve it from the package prefix.
package("zlib-ng")
    set_homepage("https://github.com/zlib-ng/zlib-ng")
    set_description("zlib replacement with optimizations for next generation systems.")
    set_license("zlib")

    add_urls("https://github.com/zlib-ng/zlib-ng/archive/refs/tags/$(version).tar.gz",
             "https://github.com/zlib-ng/zlib-ng.git")
    add_versions("2.3.3", "f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1")

    add_configs("zlib_compat", {description = "Install the zlib-compatible API", default = true, type = "boolean"})
    add_deps("cmake")

    on_install(function (package)
        local configs = {
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DZLIB_ENABLE_TESTS=OFF",
            "-DZLIBNG_ENABLE_TESTS=OFF",
            "-DWITH_GTEST=OFF",
            "-DINC_INSTALL_DIR=" .. package:installdir("include"),
            "-DLIB_INSTALL_DIR=" .. package:installdir("lib"),
            "-DZLIB_COMPAT=" .. (package:config("zlib_compat") and "ON" or "OFF"),
        }
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cincludes("zlib.h"))
    end)
package_end()

add_requires("minizip-ng 4.2.2")
add_requires("zlib-ng 2.3.3")
add_requires("fast_float 8.3.3")
add_requires("simdutf 8.2.0")
add_requires("fmt 12.2.1")
add_requires("nlohmann_json 3.12.0")

includes("projects/fastxl-core", "projects/fastxl-cli", "projects/fastxl-c", "bindings")
if os.isdir("projects/bench") then includes("projects/bench") end
if os.isdir("projects/tests") then includes("projects/tests") end

if is_plat("windows") then
    -- Source files (tests, CLI) contain UTF-8 literals.
    add_cxflags("/utf-8")
end
