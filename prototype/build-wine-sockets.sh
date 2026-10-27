# Sourced by the cross-build with the same pinned MinGW tools as the host.
winebuild=${PROBE_WINEBUILD:-winebuild}
socket_objects=()
for unit in socket protocol inaddr async; do
  "$cc32" "${flags[@]}" "${inc[@]}" -D_WS2_32_ -c "wine-11.0-source/dlls/ws2_32/$unit.c" -o "prototype/out/ws2-$unit.o"
  socket_objects+=("prototype/out/ws2-$unit.o")
done
for unit in unix_lib debug stub delay_load; do
  "$cc32" "${flags[@]}" "${inc[@]}" -c "wine-11.0-source/dlls/winecrt0/$unit.c" -o "prototype/out/ws2-crt-$unit.o"
  socket_objects+=("prototype/out/ws2-crt-$unit.o")
done
# Preserve Wine's lazy DNS/IP helper imports. These DLLs are only needed by
# APIs that actually use them, as in Wine's original ws2_32 Makefile.
cat > prototype/out/ws2-dnsapi.def <<'EOF'
LIBRARY dnsapi.dll
EXPORTS
DnsQuery_A@24
DnsRecordListFree@8
EOF
cat > prototype/out/ws2-iphlpapi.def <<'EOF'
LIBRARY iphlpapi.dll
EXPORTS
GetAdaptersInfo@8
GetIpForwardTable@12
GetBestRoute@12
GetIpAddrTable@12
EOF
cat > prototype/out/ws2-kernelbase.def <<'EOF'
LIBRARY kernelbase.dll
EXPORTS
DelayLoadFailureHook@8
ResolveDelayLoadedAPI@24
EOF
cat > prototype/out/ws2-user32.def <<'EOF'
LIBRARY user32.dll
EXPORTS
PostMessageW@16
EOF
cat > prototype/out/ws2-advapi32.def <<'EOF'
LIBRARY advapi32.dll
EXPORTS
RegGetValueW@28
EOF
for unit in dnsapi iphlpapi user32 advapi32; do
  "$dlltool32" -k -d "prototype/out/ws2-$unit.def" -y "prototype/out/libws2-$unit-delay.a"
done
"$dlltool32" -k -d prototype/out/ws2-kernelbase.def -l prototype/out/libws2-kernelbase.a
"$winebuild" --dll -b i686-w64-mingw32 --cc-cmd "$cc32" --nm-cmd "$nm32" \
  -E wine-11.0-source/dlls/ws2_32/ws2_32.spec -o prototype/out/ws2-spec.o "${socket_objects[@]}"
"$winebuild" --def -k -b i686-w64-mingw32 -E wine-11.0-source/dlls/ws2_32/ws2_32.spec -o prototype/out/ws2-exports.def
"$cc32" -shared -nostdlib prototype/out/ws2-spec.o "${socket_objects[@]}" \
  -Wl,--entry,_DllMain@12,--exclude-all-symbols,--no-gc-sections -o prototype/out/stock-wine/ws2_32.dll \
  -Wl,--start-group prototype/out/libws2-{dnsapi,iphlpapi,user32,advapi32}-delay.a prototype/out/libws2-kernelbase.a \
  -lntdll -lkernel32 -luser32 -ladvapi32 -lmsvcrt -lmingwex -lgcc -Wl,--end-group
python3 prototype/check-pe-exports.py prototype/stock-wine/i386-windows/ws2_32.dll prototype/out/stock-wine/ws2_32.dll
