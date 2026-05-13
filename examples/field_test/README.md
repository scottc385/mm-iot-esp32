# HaLow Field Test STA

ESP32 STA app for building survey tests. It connects to the HaLow AP using a static IP,
runs a UDP iperf server on port 5001, prints periodic link/test status, and supports a
boot-button trigger packet to the AP.

Default Heltec profile:

- FW: `mm6108.mbin`
- BCF: `bcf_mf08551.mbin`
- Static IP: `192.168.50.2/24`
- Gateway/AP: `192.168.50.1`
- Button: GPIO0, active low
- Addressable RGB LED: GPIO48

Known hardware profiles:

| Profile defaults file | Module | FW | BCF | Notes |
| --- | --- | --- | --- | --- |
| `sdkconfig.defaults.esp32s3.heltec_ht_hc01p` | Heltec HT-HC01P | `mm6108.mbin` | `bcf_mf08551.mbin` | Current best field-test profile. Morse support recommends using Heltec-supplied BCFs for Heltec modules. |
| `sdkconfig.defaults.esp32s3.heltec_ht_hc01p_vendor` | Heltec HT-HC01P | `mm6108.mbin` | `bcf_HC0P.mbin` | Heltec `driver_1_15_3` vendor BCF converted from `bcf_HC0P.bin`; added for safe A/B testing against `bcf_mf08551.mbin`. |
| `sdkconfig.defaults.esp32s3.seeed_mf16858_current` | Seeed/Quectel FGH100M-H | `mm6108.mbin` | `bcf_mf16858.mbin` | Best local Seeed ESP32 candidate so far. `MF16858` is the Morse reference design name used by Quectel-derived FGH100M-H files. |
| `sdkconfig.defaults.esp32s3.seeed_fgh100mh` | Seeed/Quectel FGH100M-H | `mm6108.mbin` | `bcf_fgh100mhaamd.mbin` | Historical candidate. Keep for comparison, but do not mix BCF/firmware versions casually. |

Build:

```bash
cd third_party/mm-iot-esp32/examples/field_test
source /home/scott/esp/esp-idf/export.sh
export MMIOT_ROOT=/home/scott/tb/halow-esp32-test/third_party/mm-iot-esp32
idf.py -B build-heltec-field \
  -D SDKCONFIG=build-heltec-field/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3.heltec_ht_hc01p" \
  build
```

Flash:

```bash
idf.py -B build-heltec-field -p /dev/ttyACM0 flash monitor
```

Serial status examples:

```text
FIELD status=IDLE_CONNECTED uptime_ms=31000 rssi=-63 umac_rssi=-63 rx_frames=0 last_kbps=0 last_rx=0 last_errors=0 seq=0
FIELD status=TEST_RUNNING uptime_ms=42000 rssi=-67 umac_rssi=-67 rx_frames=110 last_kbps=0 last_rx=0 last_errors=0 seq=0
FIELD status=OUT_OF_RANGE uptime_ms=88000 rssi=-91 umac_rssi=-91 rx_frames=110 last_kbps=502 last_rx=429 last_errors=1 seq=1
```

Button behavior:

- Short press sends a UDP trigger packet to `192.168.50.1:5010` and prints status.
- Long press currently prints a status marker only.

LED behavior:

- Dim green: connected/idle
- Cyan fast blink: test traffic running
- Green flash: recent test looked good
- Amber flash: recent test had high errors/no traffic
- Red slow blink: out of range or RSSI below threshold

The Hosyond ESP32-S3 N16R8 board appears to use an addressable RGB LED on GPIO48.
If the LED does not light, check `CONFIG_FIELD_RGB_ADDR_GPIO` first.
