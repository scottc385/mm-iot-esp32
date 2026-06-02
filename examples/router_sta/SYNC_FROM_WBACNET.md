# Sync From wbacnet-fw

This file records the intended copy-based MVP dependency boundary from
`wbacnet-fw` into this `router_sta` app.

Reference handoff document:

```text
/home/scott/tb/wbacnet-fw/docs/esp32-halow-router-dependency-handoff.md
```

Reference `wbacnet-fw` commit observed when this app was scaffolded:

```text
9709292
```

Current state:

- The portable TBX wrapper has been copied from `wbacnet-fw`.
- No router core or BACnet-stack NPDU helper files have been copied yet.
- This app currently implements only the HaLow STA + UDP/TBX framing smoke
  milestone. The payload is not a real BACnet NPDU yet.
- The next milestone should copy the UDP/TBX-only router file set listed in the
  handoff and update this file with exact source paths and commit SHA.

Copied files:

```text
/home/scott/tb/wbacnet-fw/components/bacnet_stack/tbx.c
  -> main/wbacnet/tbx.c
/home/scott/tb/wbacnet-fw/components/bacnet_stack/include/tbx.h
  -> main/wbacnet/tbx.h
```

Copy policy:

- Prefer practical copy-based MVP while the Morse/MM-IoT build environment is
  still isolated from `wbacnet-fw`.
- Do not modify copied core files unless necessary.
- Keep Morse/MM-IoT calls in app-local adapter files.
- Record every copied file and every local divergence here.
