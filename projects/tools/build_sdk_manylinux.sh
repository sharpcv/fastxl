#!/usr/bin/env bash
# Build, verify and package the Linux C ABI SDK inside
# quay.io/pypa/manylinux_2_28_<arch>. Building on a current distro would raise
# the glibc symbol floor (Ubuntu 24.04 needs GLIBC_2.38) and the SDK would stop
# loading on RHEL 8 (2.28), Debian 12 (2.36) and slim Docker images.
set -euo pipefail

case "${AUDITWHEEL_PLAT:-}" in
  manylinux_2_28_*) ;;
  *)
    echo "run inside quay.io/pypa/manylinux_2_28_<arch> (AUDITWHEEL_PLAT is '${AUDITWHEEL_PLAT:-unset}')" >&2
    exit 1
    ;;
esac

# xmake refuses to run as root without this; manylinux containers run as root.
export XMAKE_ROOT=y

if ! command -v xmake >/dev/null 2>&1; then
  curl -fsSL https://xmake.io/install.sh | bash
  export PATH="$HOME/.local/bin:$PATH"
fi

# Newest bundled CPython: the packaging and fixture tools are stdlib-only.
PYTHON="${PYTHON:-$(ls -d /opt/python/cp3*-cp3*/bin/python3 | sort -V | tail -1)}"
echo "cc: $(cc --version | head -1)"
echo "python: $("$PYTHON" --version)"

xmake f -y -m release
xmake build -y fastxl-c

# Strip before packaging: the .so is dlopen'ed by ctypes, not debugged.
strip --strip-unneeded build/lib/libfastxl.so
"$PYTHON" projects/tools/check_sdk_symbols.py build/lib/libfastxl.so --max-glibc 2.28

"$PYTHON" projects/tests/fixtures/gen_fixtures.py --out projects/tests/fixtures
mkdir -p build/c-abi-tmp
cc -std=c11 -Wall -Wextra -Iprojects/fastxl-c/include \
  projects/fastxl-c/tests/test_c_abi.c \
  -Lbuild/lib -lfastxl -Wl,-rpath,"$PWD/build/lib" -o build/test_c_abi
build/test_c_abi projects/tests/fixtures/basic.xlsx build/c-abi-tmp

"$PYTHON" projects/tools/package_sdk.py
"$PYTHON" projects/tools/package_wheel.py --plat-name "$AUDITWHEEL_PLAT" --out build/wheelhouse

# Prove the wheel is self-contained: install it away from the checkout and
# import it only from the wheel target directory.
FIXTURE="$PWD/projects/tests/fixtures/basic.xlsx"
WHEEL_TEST=$(mktemp -d)
"$PYTHON" -m pip install --quiet --no-deps --target "$WHEEL_TEST/pkg" build/wheelhouse/fastxl_ctypes-*.whl
(
  cd "$WHEEL_TEST"
  PYTHONPATH="$WHEEL_TEST/pkg" "$PYTHON" -c "
import fastxl_ctypes, sys
schema = fastxl_ctypes.schema(sys.argv[1])
assert schema['ok'] is True, schema
print('wheel ok:', fastxl_ctypes.version(), fastxl_ctypes.__file__)
" "$FIXTURE"
)
rm -rf "$WHEEL_TEST"
