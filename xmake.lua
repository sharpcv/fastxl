set_project("fastxl")
FASTXL_PROJECT_VERSION = "1.0.0"
set_version(FASTXL_PROJECT_VERSION)
set_license("MIT")
set_languages("c++20")
add_rules("mode.debug", "mode.release")

-- fastxl only reads/writes STORE and DEFLATE ZIP entries; encrypted ZIPs are
-- rejected. Building minizip-ng with OpenSSL would pull libssl/libcrypto into
-- every binary for features the library never calls, so use a local package
-- definition that turns encryption off. Keep it on the same upstream version.
package("minizip-ng-lite")
    set_homepage("https://github.com/zlib-ng/minizip-ng")
    set_description("minizip-ng trimmed to STORE + DEFLATE (no encryption, no OpenSSL).")
    set_license("zlib")

    add_urls("https://github.com/zlib-ng/minizip-ng/archive/refs/tags/$(version).tar.gz",
             "https://github.com/zlib-ng/minizip-ng.git")
    add_versions("4.2.2", "71af7b9799856d8b03619df3949e9c1be9703f8de0795af71399ba283cb27aac")

    add_deps("cmake", "zlib")
    if is_plat("windows") then
        add_syslinks("crypt32", "advapi32", "bcrypt")
    end

    on_install(function (package)
        local configs = {
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=OFF",
            "-DMZ_LIBCOMP=OFF",
            "-DMZ_FETCH_LIBS=OFF",
            "-DMZ_PPMD=OFF",
            "-DMZ_ZLIB=ON",
            "-DMZ_BZIP2=OFF",
            "-DMZ_LZMA=OFF",
            "-DMZ_ZSTD=OFF",
            "-DMZ_OPENSSL=OFF",
            "-DMZ_PKCRYPT=OFF",
            "-DMZ_WZAES=OFF",
        }
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("mz_zip_open", {includes = "minizip/mz_zip.h"}))
    end)
package_end()

add_requires("minizip-ng-lite 4.2.2")
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
