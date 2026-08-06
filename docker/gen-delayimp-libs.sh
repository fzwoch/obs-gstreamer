#!/bin/bash
# Generates delay-import libraries for the GStreamer/GLib DLLs obs-gstreamer
# directly links against on Windows, so obs-gstreamer.dll itself doesn't need
# them resolvable at LoadLibrary time (see gstreamer.c's
# configure_gstreamer_windows_paths()).
set -e

BIN_DIR="$1"
OUT_DIR="$2"

if [ -z "$BIN_DIR" ] || [ -z "$OUT_DIR" ]; then
	echo "usage: $0 <gstreamer-bin-dir> <output-dir>" >&2
	exit 1
fi

mkdir -p "$OUT_DIR"

DLLS="libgstreamer-1.0-0 libgobject-2.0-0 libglib-2.0-0 libintl-8 libgstvideo-1.0-0 libgstbase-1.0-0 libgstaudio-1.0-0 libgsttag-1.0-0 libgstapp-1.0-0 libgstnet-1.0-0 libgio-2.0-0"

for name in $DLLS; do
	dll="$BIN_DIR/$name.dll"
	if [ ! -f "$dll" ]; then
		echo "missing $dll" >&2
		exit 1
	fi
	( cd "$OUT_DIR" && gendef "$dll" )
	x86_64-w64-mingw32-dlltool \
		-d "$OUT_DIR/$name.def" \
		--dllname "$name.dll" \
		--output-delaylib "$OUT_DIR/lib${name}_delay.a"
done
