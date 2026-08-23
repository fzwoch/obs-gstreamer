:: Move to your plugin project folder
d:
cd D:\obs-gstreamer-master

:: Wipe any old broken manual inclusion folders to start fresh
rmdir /s /q include

:: Create a clean base inclusion folder
mkdir include

:: Create a native Windows Junction shortcut mapping your OBS source files
mklink /J D:\obs-gstreamer-master\include\obs D:\obs-studio-32.2.2\libobs


:: 1. Force load your global MSYS2 GCC compiler path
set PATH=D:\msys64\mingw64\bin;%PATH%
set NINJA=D:\obs-gstreamer-master\ninja.exe

:: 2. Apply the synchronized multi-layered D: drive tracking directories (Semicolon-separated)
set C_INCLUDE_PATH=D:\obs-gstreamer-master\include;D:\obs-gstreamer-master\include\obs;D:\msys64\mingw64\include\gstreamer-1.0;D:\msys64\mingw64\include\glib-2.0;D:\msys64\mingw64\lib\glib-2.0\include
set CPLUS_INCLUDE_PATH=D:\obs-gstreamer-master\include;D:\obs-gstreamer-master\include\obs;D:\msys64\mingw64\include\gstreamer-1.0;D:\msys64\mingw64\include\glib-2.0;D:\msys64\mingw64\lib\glib-2.0\include

:: 3. Pass your live OBS Studio binary and synchronized GStreamer library folders for linking
set LIBRARY_PATH=C:\Program Files\obs-studio\bin\64bit;D:\msys64\mingw64\lib

:: 4. Wipe out Meson's caching layers completely to avoid old path conflicts
rmdir /s /q build

:: 5. Run Meson setup, explicitly forcing it to pass the core and sub-module link definitions
C:\Users\r84364811\AppData\Local\Python\pythoncore-3.14-64\Scripts\meson.exe setup build ^
  -Dc_link_args="-lobs -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 -lgstnet-1.0"

:: 6. Launch the final code compilation loop
D:\obs-gstreamer-master\ninja.exe -C build
