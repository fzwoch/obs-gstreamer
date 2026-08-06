#!/bin/bash
set -e

export PKG_CONFIG_PATH=/c/gstreamer/1.0/x86_64/lib/pkgconfig/

rm -f cross.txt
{
  echo "[binaries]"
  echo "c = 'x86_64-w64-mingw32-gcc'"
  echo "cpp = 'x86_64-w64-mingw32-g++'"
  echo "ar = 'x86_64-w64-mingw32-ar'"
  echo "strip = 'x86_64-w64-mingw32-strip'"
  echo "pkgconfig = 'x86_64-w64-mingw32-pkg-config'"
  echo "windres = 'x86_64-w64-mingw32-windres'"
  echo ""
  echo "[properties]"
  echo "c_link_args = ['-static-libgcc', '-L/bin/64bit']"
  echo "pkg_config_libdir = '/c/gstreamer/1.0/mingw_x86_64/lib/pkgconfig'"
  echo ""
  echo "[host_machine]"
  echo "system = 'windows'"
  echo "cpu_family = 'x86_64'"
  echo "cpu = 'x86_64'"
  echo "endian = 'little'"
} >> cross.txt

export C_INCLUDE_PATH=/

rm -rf gst-delayimp
bash docker/gen-delayimp-libs.sh /c/gstreamer/1.0/mingw_x86_64/bin gst-delayimp

meson --buildtype release --cross-file cross.txt -Dpkg_config_path=/c/gstreamer/1.0/x86_64/lib/pkgconfig/ \
	-Dgst_delayimp_dir="$(pwd)/gst-delayimp" windows
ln -sf /c/ "windows/c:"
ninja -C windows

echo "Build complete: windows/obs-gstreamer.dll"
