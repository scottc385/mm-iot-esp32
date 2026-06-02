# Sync From wbacnet-fw

This file records the copy-based MVP dependency boundary from `wbacnet-fw` into
this `router_sta` app.

Reference handoff document:

```text
/home/scott/tb/wbacnet-fw/docs/esp32-halow-router-dependency-handoff.md
```

Reference `wbacnet-fw` commit observed when this router-service scaffold was
copied:

```text
9709292
```

Current state:

- Portable TBX wrapper copied from `wbacnet-fw`.
- Portable router core/service/policy files copied from `wbacnet-fw`.
- Minimal BACnet-stack NPDU/helper file set copied from `wbacnet-fw`.
- App-local UDP/TBX adapter lives under `main/ports` and contains all ESP32/lwIP
  socket integration.
- Current milestone is UDP/TBX router-service scaffold: the ESP32 sends a real
  BACnet network-layer Who-Is-Router-To-Network NPDU over TBX and submits inbound
  TBX NPDU frames to the copied router service.

Copied router/TBX files:

```text
/home/scott/tb/wbacnet-fw/components/bacnet_stack/tbx.c
  -> main/wbacnet/tbx.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/include/tbx.h
  -> main/wbacnet/tbx.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/router/router_core.c
  -> main/wbacnet/router/router_core.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/include/router_core.h
  -> main/wbacnet/router/router_core.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/router/router_policy.c
  -> main/wbacnet/router/router_policy.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/include/router_policy.h
  -> main/wbacnet/router/router_policy.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/router/router_service.c
  -> main/wbacnet/router/router_service.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/include/router_service.h
  -> main/wbacnet/router/router_service.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/router/router_mgmt_command.c
  -> main/wbacnet/router/router_mgmt_command.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/include/router_mgmt_command.h
  -> main/wbacnet/router/router_mgmt_command.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/include/router_transport.h
  -> main/wbacnet/router/router_transport.h
```

Copied BACnet-stack files:

```text
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/*.h
  -> main/wbacnet/bacnet/*.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/basic/sys/*.h
  -> main/wbacnet/bacnet/basic/sys/*.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/datalink/*.h
  -> main/wbacnet/bacnet/datalink/*.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/datalink/bsc/*.h
  -> main/wbacnet/bacnet/datalink/bsc/*.h
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/npdu.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bacaddr.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bacapp.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bacdcode.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bacint.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bactext.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bacstr.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bactimevalue.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bacreal.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/indtext.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/hostnport.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/iam.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/whois.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/rp.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/wp.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/reject.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/abort.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/external/src/bacnet/bacerror.c
  -> matching main/wbacnet/bacnet/*.c paths
```

App-local files:

```text
main/ports/udp_tbx_port.c
main/ports/udp_tbx_port.h
main/src/router_sta.c
```

Copy policy:

- Prefer practical copy-based MVP while the Morse/MM-IoT build environment is
  still isolated from `wbacnet-fw`.
- Do not modify copied core files unless necessary.
- Keep Morse/MM-IoT/lwIP calls in app-local adapter files.
- Record every copied file and every local divergence here.
