# fpp-plugin-listen-sync

FPP 10 plugin for [Listen Sync](https://github.com/LightsOn14th/cf-listen-sync). It lets visitors play the show audio on their phones in sync with the lights.

- Sends the playback position from FPP MultiSync to the relay over an outbound WebSocket. No ports are opened on the FPP device.
- All networking runs in a separate background process (`listen_sync_daemon.py`), never inside fppd. The part inside fppd only hands position updates to that process as local UDP datagrams, so a network problem cannot stop the show.
- Stamps each position on the relay clock using an NTP-style offset, so the Pi wall clock does not need to be accurate.
- Uploads new and changed audio files from the Music folder to the relay so phones can download them ahead of time.

Requires FPP 10.0 or later, which corrects the reported media position for audio output latency.

## Install

1. In FPP, open Content Setup > Plugin Manager and install from this URL:
   `https://raw.githubusercontent.com/LightsOn14th/fpp-plugin-listen-sync/main/pluginInfo.json`
2. Turn on "Send MultiSync" in FPP Settings > MultiSync.
3. Open Status/Control > Listen Sync, check the relay URL and save the relay token.

The install builds the plugin on the device and loads it without restarting fppd.

## Logs and status

- Daemon log: `logs/plugin-fpp-plugin-listen-sync.log`
- Status file: `plugindata/fpp-plugin-listen-sync/status.json`

## Development

`src/ListenSyncPlugin.cpp` is the in-fppd part, built with FPP shared makefiles (`make SRCDIR=/opt/fpp/src`). It starts no threads and parses nothing. `listen_sync_daemon.py` (Python 3, `python3-websockets`) does everything else and is started by `scripts/postStart.sh` and stopped by `scripts/preStop.sh`.
