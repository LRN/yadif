@echo off

REM Usage:
REM   to build the plugin:
REM     makewin32 [DEBUG=1]
REM   to install the plugin:
REM     (in cmd) makewin32 install PREFIX=d:\progs\gstreamer\0.10\lib\avsynth-0.10
REM     or
REM     (in msys) makewin32 install PREFIX=d:/progs/gstreamer/0.10/lib/avsynth-0.10
REM   to remove garbage:
REM     makewin32 clean
REM Get MinGW and GNU Make from http://sourceforge.net/projects/mingw/files/ , GTK from http://www.gtk.org/download-windows.html in one bundle
REM
REM Get GstAVSynth header (gstavsynth_sdk.h or gstavsynth_sdk_cpp.h) from somewhere (git should be fine) and drop it into %GST_DIR%\include\gst\avsynth
REM Or you can download complete GStreamer package (or build it yourself) and build GstAVSynth aggainst it yourself (or download it pre-compiled too) and get that header automatically in the right place
REM Either way you don't need anything from GStreamer except that header to compile the plugin. But you WILL need both GStreamer and GstAVSynth binaries to actually USE the plugin.
REM
REM Adjust GST_DIR to point at your GStreamer directory (where lib and include subdirectories are)
REM Adjust GTK_DIR to point at your GTK directory
REM Adjust MINGW_DIR to point at your MinGW directory
REM Adjust MAKE_DIR to pointer at your GNU Make directory
REM
REM This script should be used to call MinGW makefile from cmd ONLY. If you have a configured msys environment, call make from msys directly, like this:
REM   PKG_CONFIG_PATH=/usr/local/lib/pkgconfig CFLAGS=-I/usr/local/gstreamer/include make -f Makefile.mingw32 [parameters]
REM If you are on *nix, you're looking the wrong way (there should be other makefiles)
REM If you are on *nix and trying to cross-compile - patches to Makefile.win32 are welcome

set GST_DIR=d:\progs\gstreamer\0.10
set GTK_DIR=d:\progs\gtk
set MAKE_DIR=c:\gnu
set MINGW_DIR=c:\mingw

set PKG_CONFIG_PATH=%GTK_DIR%\lib\pkgconfig;%GST_DIR%\lib\pkgconfig
set CFLAGS=-Wall
if defined MODIFIED_PATH goto callmake
set PATH=%MAKE_DIR%\bin;%MINGW_DIR%\bin;%PATH%
set MODIFIED_PATH=1

:callmake
@echo on
mingw32-make -f Makefile.mingw32 %*