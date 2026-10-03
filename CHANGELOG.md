# Changelog

## [NEXT_VERSION] - [UNRELEASED]
* FEA: Sync - Send the FPP playback position from MultiSync to the Listen Sync relay over an outbound WebSocket, stamped on the relay clock with an NTP-style offset.
* FEA: Playlist - Send the audio files of the running playlist so listeners can download the next song early.
* FEA: Media - Upload new and changed audio files from the Music folder to the relay, checked on connect and every 15 minutes.
* FEA: Status page - Show the relay connection, round trip, current position and upload progress, and save the relay token to plugindata.
* BUG: Stability - Read playlist, media list and relay JSON with type checks, and catch errors on every plugin thread, so an unexpected response (an on-the-fly playlist, or `sizeBytes` sent as a string) no longer aborts fppd.
* BUG: Settings - Skip playlist lookups and uploads while the plugin is disabled.
* BUG: Stability - Move all networking (relay WebSocket, playlist lookups, uploads) out of fppd into a separate daemon process that restarts itself, so a network, TLS or HTTP error can no longer abort fppd. The part inside fppd only sends local UDP datagrams and starts no threads.
