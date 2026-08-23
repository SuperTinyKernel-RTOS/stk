# STK Wi-Fi httpd + mDNS Example — Raspberry Pi Pico 2 W (RP2350)

**SuperTinyKernel RTOS** (STK) — Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.

This example demonstrates STK-driven Wi-Fi networking on the **Raspberry Pi Pico 2 W** (RP2350, Arm Cortex-M33), serving a small web UI over lwIP's `httpd`/SSI/CGI application and advertising itself over mDNS — all wired into STK's threading and event model instead of the original SDK example's single superloop.

It is a direct STK port of the Pico SDK's stock `pico_w` Wi-Fi httpd example: the same page content and CGI/SSI/POST handlers, but restructured so that Wi-Fi connect, server setup, and request handling run inside a proper STK task, driven by events rather than polled in a `while (true)`.

---

## Example Project

This example's project is **not** hosted in the core `stk` repository — it lives directly in the `build/example` tree:

- **Source & content:** [`stk/tree/main/build/example/httpd`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/httpd)
- **Eclipse project:** [`stk/tree/main/build/example/project/eclipse/rpi/httpd-rp2350w`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/rpi/httpd-rp2350w)

Clone the `stk` repository and open the `httpd-rp2350w` project folder in Eclipse CDT to build and flash this example, its board support, and its linker/pre-build configuration.

---

## What This Demonstrates

- **STK-hosted lwIP networking**: a single `WifiHttpdTask` owns Wi-Fi connect and httpd/mDNS server setup, driven entirely by `stk::sync::EventFlags` (`FLAG_CONNECT_WIFI`, `FLAG_LED_UPDATE`, `FLAG_INIT_NET`, `FLAG_SHUTDOWN_NET`) instead of the original example's `while (true)` polling loop.
- **`cyw43_arch_stk`'s task-based background model** — the STK analogue of upstream's FreeRTOS backend (rather than `POLL` or `THREADSAFE_BACKGROUND`). Runs under `NO_SYS=0`: `cyw43_arch_init()` spins up `async_context_stk`'s own worker task to drive the Wi-Fi driver, and — since `CYW43_LWIP` is set — also spawns lwIP's `tcpip` thread via `sys_thread_new()`, sharing its core lock with the async context through `sys_arch`'s `LWIP_STK_CUSTOM_CORE_LOCKING` hook.
- **Correct task-context sequencing for `cyw43_arch_init()`**: it's called from inside `WifiHttpdTask::Run()`, not from one-time bring-up in `RunExample()`, because the driver's locking/ownership checks call `stk::GetTid()`, which only identifies a calling task once the kernel is actually running (i.e. after `kernel.Start()`).
- **End-to-end event-driven idle**: `WifiHttpdTask` blocks on `EventFlags::Wait()` rather than spinning, and so does everything beneath it — `async_context_stk`'s worker task and lwIP's `tcpip` thread block on their own queues/semaphores when idle rather than polling. With nothing runnable, the STK scheduler puts the CPU to sleep (WFI, or tickless idle via `KERNEL_TICKLESS` / `STK_TICKLESS_IDLE`) until Wi-Fi/lwIP activity, a POST request, or a timer wakes something up — with no explicit power-management code in the example itself.
- **mDNS responder**: advertises the board as `<hostname>.local` (hostname suffixed with 4 hex digits of the Wi-Fi MAC) with an `_http._tcp` service on port 80.
- **SSI tag substitution**: `status`, `welcome`, `uptime`, `ledstate`, `ledinv`, and a `table` tag that demonstrates `LWIP_HTTPD_SSI_MULTIPART`, streaming 10 generated rows across multiple SSI callback invocations.
- **CGI and POST handling**: CGI handlers for `/`, `/index.shtml`, and `/restart`; a POST handler on `/led.cgi` that parses a `led_state` form field out of the request body and toggles the onboard LED via `cyw43_gpio_set()`, returning `/ledpass.shtml` or `/ledfail.shtml` depending on success.
- **Priority-based scheduling**: the kernel is configured with `KERNEL_DYNAMIC | KERNEL_SYNC` (plus `KERNEL_TICKLESS` where the platform supports it) and `SwitchStrategyFP32`. Task capacity is sized as `1 (WifiHttpdTask) + TimerHost::TASK_COUNT + ASYNC_CONTEXT_STK_TASKS + LWIP_STK_THREAD_POOL_SIZE`, covering the app task, STK's timer host, `async_context_stk`'s worker task(s), and lwIP's `tcpip` thread(s).

---

## Web Content & `fsdata.c` Generation

The served pages (`index.shtml`, `test.shtml`, `ledpass.shtml`, `ledfail.shtml`, etc.) live as plain files under:

```
build/example/httpd/content
```

Rather than linking a filesystem, lwIP's `httpd` serves pages baked directly into flash as C arrays. The Eclipse project runs [`makefsdata.py`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/httpd) as a **pre-build step**, which walks `build/example/httpd/content`, wraps each file with its HTTP response header (status line, `Server:`, `Content-Length`, `Content-Type`/`Content-Encoding`) per `FS_FILE_FLAGS_HEADER_INCLUDED`, and emits the generated `fsdata.c` — the `fsdata_file` table lwIP's `fs.c` uses to resolve URIs. `.shtml`/`.shtm`/`.ssi`/`.xml`/`.json` files are flagged `FS_FILE_FLAGS_SSI` so lwIP scans them for SSI tags at serve time instead of treating them as static content.

The script is idempotent: it compares an MD5 of the freshly generated content against the existing `fsdata.c` and leaves the file untouched if nothing changed, so incremental builds don't needlessly relink.

If you add, remove, or edit any file under `content/`, just rebuild the project — the pre-build step regenerates `fsdata.c` automatically; there's nothing to run by hand.

---

## Building & Running (Eclipse CDT)

1. Clone the `stk` repository (this example's project and sources live inside it, unlike the NXP examples which sit in a separate repo).
2. Clone the [lwIP](https://savannah.nongnu.org/projects/lwip/) library into the `stk` repository's `deps` folder (e.g. `deps/lwip`) — it isn't bundled with `stk` and the project expects to find it there.
3. Make sure the Raspberry Pi Pico SDK and its toolchain (`arm-none-eabi-gcc`, CMake, Ninja) are installed and discoverable, as required by the Pico W / `cyw43` build.
4. Before building, set your Wi-Fi credentials — the example reads them from `ENV_WIFI_SSID` and `ENV_WIFI_PASSWORD` (configure these as build-time definitions/environment variables in the project settings rather than hardcoding them in source).
5. In Eclipse CDT (with the [Eclipse Embedded CDT](https://eclipse-embed-cdt.github.io) plugins), go to **File → Import → General → Existing Projects into Workspace** and select `build/example/project/eclipse/rpi/httpd-rp2350w`.
6. Connect your Pico 2 W board via its SWD debug probe (e.g. Picoprobe/CMSIS-DAP) or another supported OpenOCD-compatible probe.
7. Select the **Debug** build configuration.
8. Build and start the debug session. The pre-build step regenerates `fsdata.c` from `content/`, the project compiles and links against the core STK sources, and the board is flashed and launched.

Once running, the board connects to Wi-Fi, starts the httpd/mDNS server, and prints its IP address (and `<hostname>.local` mDNS name) over the USB serial console. Point a browser at either address to reach the served page, toggle the onboard LED via the form on the page, and watch the `uptime`/`ledstate` SSI tags update.

---

## See Also

- [Main STK README](https://github.com/SuperTinyKernel-RTOS/stk#readme) — project overview, features, and general Quick Start.
- [`stk/sync`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/stk/include/sync) — STK's synchronization primitives, including the `EventFlags` API used to drive this example.
- Raspberry Pi Pico SDK: `pico_w` `httpd` [Wi-Fi example](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/httpd) — the original single-superloop example this port is based on.
- `build/example/httpd` — this example's sources, `content/` web assets, and `makefsdata.py`.
