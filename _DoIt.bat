@ECHO OFF
D:\FICHEROS\UTIL\UPX --ultra-brute --no-lzma --overlay=strip ROLEXWINVC32.exe
D:\FICHEROS\UTIL\UPX --ultra-brute --no-lzma --overlay=strip ROLEXWINVC64.exe
DEL 4More.zip
D:\FICHEROS\UTIL\KZIP ROLEXWINVC.zip ROLEXWINVC32.exe ROLEXWINVC64.exe