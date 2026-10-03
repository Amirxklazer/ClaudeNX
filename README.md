# ClaudeNX (phase 1)
Claude-style chat for Nintendo Switch homebrew, using your Puter account (Claude models via Puter's OpenAI-compatible API).

## Build
Push to GitHub -> Actions tab -> "Build NRO" -> download the `ClaudeNX` artifact.
Or locally with devkitPro: `dkp-pacman -S switch-sdl2 switch-sdl2_ttf switch-curl switch-mbedtls switch-freetype switch-zlib switch-bzip2 switch-libpng` then `make`.

## Install
Copy `ClaudeNX.nro` to `sd:/switch/`. Launch from hbmenu in title-takeover mode (not Album applet mode).

## Pair
First launch shows a QR code. Scan it with a phone on the same Wi-Fi, tap "Sign in with Puter". Token is saved to `sd:/switch/ClaudeNX/config.txt`.

## Controls
A / tap box = type | Y = new chat | X = re-pair | ZL/ZR = model | right stick / D-pad / drag = scroll | + = quit

## Optional
- `sd:/switch/ClaudeNX/models.txt`: one Puter model id per line.
- `sd:/switch/ClaudeNX/cacert.pem`: from https://curl.se/ca/cacert.pem. Without it TLS is NOT verified. Add it.

## Notes
- Pairing is plain HTTP on your LAN (port 8080) with a one-time random key in the QR. Use trusted Wi-Fi.
- config.txt holds your Puter token. Treat it like a password.
