# ESP32 HaLow Router STA

This app is the incubating ESP32 HaLow station router for the Phase 1 BACnet
router MVP.

Current milestone implemented here:

- Heltec HT-HC01P HaLow STA bring-up
- static IP on the HaLow interface
- UDP/TBX socket on the HaLow link
- copied portable `wbacnet-fw` router service configured with one UDP/TBX port
- periodic real BACnet network-layer Who-Is-Router-To-Network NPDU over TBX
- inbound TBX frames unwrapped and submitted to the router service

Planned MVP transports:

```text
interface.halow.0      HaLow STA link management/status
transport.udp_tbx.0    router-to-router backhaul over HaLow IP
transport.bip.0        local BACnet/IP over W5500 Ethernet
transport.mstp.0       local BACnet MS/TP over RS485
```

Implementation sequence:

1. HaLow STA + static IP + UDP unicast to Pi.
2. UDP/TBX between ESP32 and Pi.
3. Router service with UDP/TBX only.
4. Add W5500 BACnet/IP.
5. Add RS485 MS/TP.
6. Soak simultaneous W5500 + MS/TP with HaLow reconnect recovery.

## Build

```bash
cd /home/scott/tb/halow-esp32-test/third_party/mm-iot-esp32/examples/router_sta
source /home/scott/esp/esp-idf/export.sh
export MMIOT_ROOT=/home/scott/tb/halow-esp32-test/third_party/mm-iot-esp32

idf.py -B build-heltec-router \
  -D SDKCONFIG=build-heltec-router/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3.heltec_ht_hc01p" \
  build
```

The verified Heltec build output is:

```text
build-heltec-router/router_sta.bin
```

## Pi Test Listener

On the Pi AP:

```sh
nc -u -l -p 5000 | hexdump -C
```

The ESP32 sends periodic TBX-wrapped BACnet NPDU frames to
`192.168.50.1:5000` and listens on local UDP port `5000`. The payload is now a
BACnet network-layer Who-Is-Router-To-Network NPDU, not a text smoke payload.

Expected ESP32 log examples:

```text
ROUTER_CONFIG ok ports=1 tbx_net=65000
ROUTER_STA status link=up rssi=-20 wir_seq=3
TX_ROUTER_WIR seq=4 npdu_len=2
TX_TBX npdu_len=2 tbx_len=11 tx_frames=4
RX_TBX from=192.168.50.1:5000 bytes=... origin=AP01 npdu_len=...
RX_TBX_ROUTER rc=... accepted=...
```

For manual receive-path testing, send a TBX-wrapped BACnet NPDU from the Pi.
A plain text UDP packet is intentionally rejected as `RX_TBX_BAD`.

## Source Sharing

See `SYNC_FROM_WBACNET.md` before copying router/TBX/BACnet files from
`wbacnet-fw`.
