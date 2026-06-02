# ESP32 HaLow Router STA

This app is the incubating ESP32 HaLow station router for the Phase 1 BACnet
router MVP.

Current milestone implemented here:

- Heltec HT-HC01P HaLow STA bring-up
- static IP on the HaLow interface
- UDP/TBX smoke traffic to the Pi AP/router endpoint

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

## Pi Test Listener

On the Pi AP:

```sh
nc -u -l -p 5000 | hexdump -C
```

The ESP32 sends periodic TBX-wrapped `TBX_SMOKE` payloads to
`192.168.50.1:5000` and listens on local UDP port `5000`.

Plain text UDP replies still work for manual socket testing:

```sh
printf 'AP_REPLY test\n' | nc -u -w 1 192.168.50.2 5000
```

Those replies will print as `RX_UDP_RAW`. TBX-wrapped replies print as
`RX_TBX_SMOKE`. This is only a framing smoke test; the payload is not a real
BACnet NPDU yet.

## Source Sharing

See `SYNC_FROM_WBACNET.md` before copying router/TBX/BACnet files from
`wbacnet-fw`.
