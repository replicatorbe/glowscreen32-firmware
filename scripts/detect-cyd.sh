#!/usr/bin/env bash
# Identifie la carte ESP32-2432S028 (CYD) branchée en USB sur macOS.
set -u

echo "=== Ports série USB ==="
ports=$(ls /dev/cu.* 2>/dev/null | grep -Ev 'Bluetooth|debug-console' || true)
if [ -z "$ports" ]; then
  echo "(aucun) -> carte non détectée."
  echo "    1. câble USB de DONNÉES, pas de charge seule (cause n°1)"
  echo "    2. sur la révision USB-C : un câble C<->C ne marche pas (pas de résistances CC)"
  echo "    3. essayer un autre port, sans hub passif"
else
  echo "$ports"
fi

echo
echo "=== Puce USB-série ==="
# CH340 = 1a86:7523 (6790:29987)  CH9102 = 1a86:55d4  CP210x = 10c4:ea60 (4292:60000)
ioreg -p IOUSB -l -w 0 2>/dev/null \
  | grep -E '"(USB Vendor Name|USB Product Name|idVendor|idProduct|USB Serial Number)"' \
  | sed 's/^ *//' || true

echo
echo "=== Pilote ayant pris la main ==="
ioreg -c IOUserSerial -r -l 2>/dev/null \
  | grep -E '"(IOTTYDevice|IOUserClass|CFBundleIdentifier)"' | sed 's/^ *//' || true
echo "  (attendu : AppleUSBCHCOM = pilote Apple natif. Si 'wch' apparaît, le pilote tiers"
echo "   WCH est installé : à désinstaller, il casse le reset auto DTR/RTS sur Apple Silicon.)"

echo
echo "=== Identification de la puce ESP32 ==="
port=$(echo "$ports" | grep -E 'usbserial|wchusbserial|SLAB' | head -1)
if [ -n "$port" ]; then
  echo "Port retenu : $port"
  pio pkg exec -p tool-esptoolpy -- esptool.py --port "$port" flash_id 2>&1 | tail -20
else
  echo "(pas de port USB-série à interroger)"
fi
