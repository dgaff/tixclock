# Nov 2026 - Code Review Notes

(Doug notes: I asked Claude for a code review. Here's what it recommended. I manually fixed some of these things and deferred others.)

I read through [src/main.cpp](src/main.cpp), compared it line by line with the original [tixclock.ino](tixclock.ino), and built it with PlatformIO. The new time code (`configTzTime`, `getLocalTime`, `tm_isdst`) is correct, and your POSIX timezone string matches the old Timezone rules. The build succeeds. There's one crash bug from the port and one thing to fix before you push. The rest were already there in the original.

## Fix before pushing or running

(FIXED) **1. Your Wi-Fi password would be committed.** The active [.gitignore](.gitignore) (the PlatformIO-generated one) doesn't list `wifisecrets.h`. Your old rules are in `.gitignore copy`, which git ignores. If you `git init` and push, [include/wifisecrets.h](include/wifisecrets.h) goes to GitHub with your real SSID and password. Merge the two files and delete the copy. Also, my attempt to hide the password in my own terminal output didn't work, so it appeared in this session's tool output. It went nowhere else.

(FIXED) **2. Stack buffer overflow on the sync log line, [main.cpp:494-496](src/main.cpp#L494-L496). You added this in the port.** `char timestr[10]` gets `"Clock synchronized at %02d:%02d:%02d"`, which is about 31 bytes. The compiler warns about it (`-Wformat-overflow`). It runs on every sync and can corrupt the stack or crash right when the clock starts. Use `Serial.printf(...)` directly, or a 40-byte buffer with `snprintf`.

## Bugs that were already in the original

(SKIPPED -- I don't really care about this one. I did rework the init logic for night mode, so I might want to fix this at some point.) **3. Night mode wakes the clock at 2 AM on DST weekends, [main.cpp:514](src/main.cpp#L514).** The DST check sets `initClockTime = 1` whenever `tm_isdst` changes. DST changes at 2 AM, while the clock is off for the night, so it powers back on and stays on until 21:00 the next day. Only run the resync if the clock is supposed to be on.

(FIXED) **4. Night mode reacts only to exact moments, not to the current state.** Off happens only when it's exactly `nightHour:00:00`, and on only at `dayHour-1:59:00`. So:

- If the ESP reboots at night (power blip, or the `ESP.restart()` after an SNTP failure), the clock syncs and stays on all night.
- If an SNTP time step jumps past that exact second, the transition is missed.
- If a sync is still running at 21:00:00, the off check is skipped, because it's inside the `else` branch.

A more reliable approach: compute "should be on" from `tm_hour` on every loop, keep a `clockIsOn` flag, and act only when the two differ. That also fixes #3 and the reboot case.

(NOT SURE THIS IS CORRECT. I did make the change, including resetting the pulse flag, but it broke clock reset and time init. I know the clock is funny about when the pulse train needs to be active, but I didn't feel like tracking it down because the code was working as is.) **5. The pulse can stop with D4 left HIGH, [main.cpp:144-148](src/main.cpp#L144-L148).** `stopACPulse()` clears the flag, but D4 stays in whatever state it was last set to, so about half the time it's HIGH. With clock power off, that HIGH can partly power the PIC through the diode and its input protection. That could explain the dim LEDs you mention in the README, and it could stop the clock from resetting cleanly, so it doesn't come up at 12:34. Add `digitalWrite(D4, LOW)` after clearing the flag.

(MONITORING. I want to see if there's actual clock drift that's meaningful. When the clock comes out of night mode, it resyncs the time. So the pulse train is surely more accurate than the 60Hz AC signal that originally provided a reference.) **6. Likely cause of your clock drift: `TIM_SINGLE` plus re-arming inside the ISR.** Each half-period is 41667 ticks plus however long the interrupt takes to start running before `timer1_write()`, and that delay adds up. Since the 60 Hz signal is the clock's time reference, each microsecond of delay costs roughly 10 seconds a day, and Wi-Fi activity makes the delay longer. Switch to `timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP)`, set it once in `startACPulse()`, and remove the `timer1_write` from the ISR. The hardware then reloads on its own and the delay doesn't add up.

## Smaller items

(FIXED) **Wi-Fi mode:** call `WiFi.mode(WIFI_STA)` before `WiFi.begin()`. The ESP8266 remembers its last mode and may also be broadcasting an open `ESP_xxxx` access point. The connect loop also has no timeout. SNTP already has one; consider the same approach here.
**[platformio.ini](platformio.ini):**

(FIXED, although I use an external serial monitor.)Add `monitor_speed = 115200`. The serial monitor defaults to 9600, so your debug output will be garbled.

(SKIPPED for now. I compared ~/.platformio/platforms/espressif8266/boards/esp12e.json and d1_mini.json and also framework-arduinoespressif8266/variants/nodemcu and variants/d1_mini. There's no material differences. Pin defines D9 and D10 in nodemcu vs. RX and TX in d1_mini. LED_BUILTIN_AUX 16 is defined in node_mcu. And ARDUINO_ESP8266_ESP12 vs. ARDUINO_ESP8266_WEMOS_D1MINI defined.) If the board is a Wemos D1 mini (your pin comments suggest it is), `board = d1_mini` is the exact match.

(SKIPPED) Consider pinning the `platform` version so the build stays reproducible.

(FIXED) **`ICACHE_RAM_ATTR` → `IRAM_ATTR`** ([line 113](src/main.cpp#L113)). The old name is deprecated and causes the other build warning. `digitalWrite` is already in IRAM in this core, so calling it from the ISR is fine.

(SKIPPED) **Startup pin glitch (hardware, nothing to fix in code):** D3 is GPIO0, which is pulled HIGH at boot, so the power transistor is on before `setup()` runs. This is harmless because `restartClock()` power-cycles the clock anyway.

(FIXED) **Out-of-date comments:**
- "I _think_ the clock gets its time reference from the internal oscillator" ([line 68](src/main.cpp#L68)) contradicts what Guido confirmed in your README.
- "The ESP8266 is 5 MHz" ([line 100](src/main.cpp#L100)): the CPU runs at 80 MHz, and 5 MHz is the timer clock after DIV16.

(SKIPPED. Some of this is for my info.) **Code you can delete:**
- The `D0`–`D8`/`TX`/`RX` defines; the core already defines them with the same values.
- The VT100 macros; `GOTOXY` never worked because the macro can't expand `x`/`y` inside a string.
- `testloop()`, and the commented-out `Ticker` and GDB lines.
- [tixclock.ino](tixclock.ino) at the root; PlatformIO ignores it, so it's only a stale copy now.

(FIXED) **Extra hour clicks in 12-hour mode:** `if (hr < 12) hr += 24` makes 12 more clicks than needed for morning hours. It's still correct because the hour wraps every 12. `hr += 12` would be faster.

I checked the hour and minute wrap-around math for both 12- and 24-hour modes and it's correct. The worst-case setup sequence takes about 17 seconds, which fits in the 30-second window between the `:30` power-cycle and the `:00` sync.
