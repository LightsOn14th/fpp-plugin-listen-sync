#!/usr/bin/env python3
"""
Listen Sync daemon: everything network-related for fpp-plugin-listen-sync.

Runs as its own process, outside fppd, so nothing here can affect the show.
The small C++ part inside fppd sends position datagrams to 127.0.0.1:39117
(see src/ListenSyncPlugin.cpp for the format). This process:

- keeps an outbound WebSocket to the relay and sends position samples on the
  relay clock, using an NTP-style offset measured with pings;
- looks up the media files of the running playlist through the local FPP API;
- uploads new or changed audio files from the Music folder to the relay;
- writes a status file the plugin page reads.

Started by scripts/postStart.sh through scripts/listen_sync_run.sh, which
restarts it if it ever exits.
"""
import asyncio
import json
import logging
import math
import os
import re
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from websockets.asyncio.client import connect
from websockets.exceptions import InvalidStatus

PLUGIN = 'fpp-plugin-listen-sync'
MEDIA_DIR = os.environ.get('MEDIADIR', '/home/fpp/media')
LOG_DIR = os.environ.get('LOGDIR', os.path.join(MEDIA_DIR, 'logs'))
CONFIG_FILE = os.path.join(MEDIA_DIR, 'config', 'plugin.' + PLUGIN)
DATA_DIR = os.path.join(MEDIA_DIR, 'plugindata', PLUGIN)
TOKEN_FILE = os.path.join(DATA_DIR, 'token')
STATUS_FILE = os.path.join(DATA_DIR, 'status.json')
UPLOAD_REQUEST_FILE = os.path.join(DATA_DIR, 'upload-request')
LOCAL_API = os.environ.get('LISTEN_SYNC_LOCAL_API', 'http://127.0.0.1')

UDP_PORT = 39117
DEFAULT_RELAY = 'https://listen.lightson14th.com'
AUDIO_EXTENSIONS = {'aac', 'flac', 'm4a', 'mp3', 'ogg', 'opus', 'wav'}

SAMPLE_INTERVAL_S = 0.5
PING_INTERVAL_S = 2.0
PING_BURST = 5
HEARTBEAT_INTERVAL_S = 10.0
UPLOAD_INTERVAL_S = 15 * 60
POSITION_STALE_S = 3.0
LOCAL_TIMEOUT_S = 15
RELAY_TIMEOUT_S = 300

log = logging.getLogger('listen-sync')


def monotonic_ms():
    """CLOCK_MONOTONIC in ms; the same clock the fppd side stamps with."""
    return time.monotonic_ns() / 1e6


# ---- settings -----------------------------------------------------------------

def read_settings():
    """Plugin settings from config/plugin.fpp-plugin-listen-sync (key = "value")."""
    values = {}

    try:
        with open(CONFIG_FILE, encoding='utf-8') as handle:
            for line in handle:
                match = re.match(r'^\s*([^=\s]+)\s*=\s*"?(.*?)"?\s*$', line)

                if match:
                    values[match.group(1)] = match.group(2)
    except FileNotFoundError:
        pass
    except OSError as error:
        log.warning('could not read settings: %s', error)

    relay = (values.get('ListenSyncRelayURL') or DEFAULT_RELAY).strip().rstrip('/')

    return {
        'enabled': values.get('ListenSyncEnabled', '1') != '0',
        'upload_media': values.get('ListenSyncUploadMedia', '1') != '0',
        'relay_url': relay,
    }


def read_token():
    try:
        with open(TOKEN_FILE, encoding='utf-8') as handle:
            return handle.readline().strip()
    except OSError:
        return ''


def websocket_url(relay_url):
    if relay_url.startswith('https://'):
        return 'wss://' + relay_url[len('https://'):] + '/fpp'

    if relay_url.startswith('http://'):
        return 'ws://' + relay_url[len('http://'):] + '/fpp'

    return ''


# ---- state --------------------------------------------------------------------

class ClockSync:
    """Median offset of the fastest few recent ping round trips."""

    def __init__(self, max_samples=16, best_of=3):
        self.max_samples = max_samples
        self.best_of = best_of
        self.samples = []

    def add(self, t0, ts, t1):
        rtt = t1 - t0

        if rtt < 0:
            return

        self.samples.append((ts - (t0 + t1) / 2, rtt))
        self.samples = self.samples[-self.max_samples:]

    def ready(self):
        return bool(self.samples)

    def fastest(self):
        return sorted(self.samples, key=lambda sample: sample[1])[:self.best_of]

    def offset(self):
        best = sorted(sample[0] for sample in self.fastest())

        return best[len(best) // 2] if best else 0.0

    def rtt(self):
        best = self.fastest()

        return best[0][1] if best else None

    def reset(self):
        self.samples = []


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.playing = False
        self.file = ''
        self.pos = 0.0
        self.stamp_ms = 0.0
        self.last_position_at = 0.0
        self.playlist_name = ''
        self.playlist_items = []
        self.connection = 'starting'
        self.last_error = ''
        self.clock = ClockSync()
        self.upload = {'state': 'idle', 'done': 0, 'total': 0, 'error': '', 'lastRun': 0}
        self.settings = read_settings()
        self.token = read_token()

    def snapshot(self):
        with self.lock:
            return {
                'enabled': self.settings['enabled'],
                'relayUrl': self.settings['relay_url'],
                'uploadMedia': self.settings['upload_media'],
                'tokenSet': bool(self.token),
                'connection': self.connection if self.settings['enabled'] else 'disabled',
                'lastError': self.last_error,
                'clockReady': self.clock.ready(),
                'rttMs': self.clock.rtt(),
                'playing': self.playing,
                'file': self.file if self.playing else None,
                'position': self.position_now() if self.playing else None,
                'playlist': self.playlist_name,
                'playlistItems': len(self.playlist_items),
                'upload': dict(self.upload),
                'updated': int(time.time()),
            }

    def position_now(self):
        return self.pos + (monotonic_ms() - self.stamp_ms) / 1000


# ---- the daemon ---------------------------------------------------------------

class Daemon:
    def __init__(self):
        self.state = State()
        self.socket = None
        self.wake = asyncio.Event()
        self.reconnect = asyncio.Event()
        self.upload_lock = threading.Lock()
        self.upload_wanted = asyncio.Event()

    # -- datagrams from fppd ----------------------------------------------------

    def on_datagram(self, data):
        try:
            fields = data.decode('utf-8', errors='replace').split('\t')
            kind = fields[0]

            if kind == 'M' and len(fields) >= 4:
                stamp_ms = int(fields[1]) / 1e6
                pos = float(fields[2])
                file = fields[3]

                if not math.isfinite(pos) or pos < 0 or not file:
                    return

                with self.state.lock:
                    changed = not self.state.playing or file != self.state.file
                    self.state.playing = True
                    self.state.file = file
                    self.state.pos = pos
                    self.state.stamp_ms = stamp_ms
                    self.state.last_position_at = monotonic_ms()

                if changed:
                    self.wake.set()

            elif kind == 'S' and len(fields) >= 3:
                with self.state.lock:
                    was_playing = self.state.playing
                    self.state.playing = False

                if was_playing:
                    self.wake.set()

            elif kind == 'P' and len(fields) >= 3:
                name = fields[2]
                media = fields[3] if len(fields) >= 4 else ''

                with self.state.lock:
                    if name == self.state.playlist_name:
                        return

                    self.state.playlist_name = name

                asyncio.get_running_loop().create_task(self.update_playlist(name, media))
        except Exception as error:  # noqa: BLE001 - a bad datagram must never stop the daemon
            log.warning('ignored datagram: %s', error)

    async def update_playlist(self, name, fallback):
        try:
            items = await asyncio.to_thread(read_playlist_media, name)
        except Exception as error:  # noqa: BLE001
            log.warning('playlist %s: %s', name, error)
            items = []

        if not items and fallback:
            items = [fallback]

        with self.state.lock:
            if name != self.state.playlist_name:
                return

            self.state.playlist_items = items

        await self.send_playlist()

    # -- relay connection -------------------------------------------------------

    async def relay_loop(self):
        attempt = 0

        while True:
            settings = self.state.settings
            token = self.state.token
            url = websocket_url(settings['relay_url'])

            if not settings['enabled'] or not token or not url:
                with self.state.lock:
                    self.state.connection = 'disabled' if not settings['enabled'] else ('no token' if not token else 'invalid relay URL')

                await self.wait_for_reconnect(5)
                continue

            with self.state.lock:
                self.state.connection = 'connecting'

            try:
                async with connect(
                    url,
                    additional_headers={'Authorization': 'Bearer ' + token},
                    open_timeout=15,
                    ping_interval=20,
                    ping_timeout=20,
                    max_size=1 << 20,
                ) as socket:
                    attempt = 0
                    await self.session(socket)
            except InvalidStatus as error:
                self.set_error('relay refused the connection (HTTP %d)' % error.response.status_code)
            except asyncio.CancelledError:
                raise
            except Exception as error:  # noqa: BLE001
                self.set_error(str(error) or type(error).__name__)

            self.socket = None

            with self.state.lock:
                self.state.connection = 'reconnecting'

            delay = min(30, 2 ** min(attempt, 5))
            attempt += 1
            await self.wait_for_reconnect(delay)

    async def session(self, socket):
        self.socket = socket
        self.reconnect.clear()

        with self.state.lock:
            self.state.connection = 'connected'
            self.state.last_error = ''
            self.state.clock.reset()

        log.info('connected to relay %s', self.state.settings['relay_url'])

        tasks = [
            asyncio.create_task(self.receive(socket)),
            asyncio.create_task(self.pinger()),
            asyncio.create_task(self.sampler()),
            asyncio.create_task(self.heartbeat()),
            asyncio.create_task(self.reconnect.wait()),
        ]

        await self.send_playlist()
        self.upload_wanted.set()

        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)

            for task in done:
                if task.exception():
                    raise task.exception()
        finally:
            for task in tasks:
                task.cancel()

            self.socket = None

    async def receive(self, socket):
        async for message in socket:
            received = monotonic_ms()

            try:
                data = json.loads(message)
            except (TypeError, ValueError):
                continue

            if not isinstance(data, dict) or data.get('type') != 'pong':
                continue

            t0 = data.get('t0')
            ts = data.get('ts')

            if isinstance(t0, (int, float)) and isinstance(ts, (int, float)):
                with self.state.lock:
                    self.state.clock.add(float(t0), float(ts), received)

    async def send(self, message):
        socket = self.socket

        if socket is None:
            return

        try:
            await socket.send(json.dumps(message))
        except Exception as error:  # noqa: BLE001 - the session loop handles the reconnect
            log.debug('send failed: %s', error)

    async def pinger(self):
        ping_id = 0

        for index in range(PING_BURST):
            ping_id += 1
            await self.send({'type': 'ping', 'id': ping_id, 't0': monotonic_ms()})
            await asyncio.sleep(0.1)

        while True:
            await asyncio.sleep(PING_INTERVAL_S)
            ping_id += 1
            await self.send({'type': 'ping', 'id': ping_id, 't0': monotonic_ms()})

    async def sampler(self):
        sent_stop_for = None

        while True:
            try:
                await asyncio.wait_for(self.wake.wait(), SAMPLE_INTERVAL_S)
            except asyncio.TimeoutError:
                pass

            self.wake.clear()

            with self.state.lock:
                # No position for a while means fppd stopped reporting; treat
                # the media as stopped rather than repeating an old sample.
                if self.state.playing and monotonic_ms() - self.state.last_position_at > POSITION_STALE_S * 1000:
                    self.state.playing = False

                playing = self.state.playing
                file = self.state.file
                pos = self.state.pos
                stamp_ms = self.state.stamp_ms
                ready = self.state.clock.ready()
                offset = self.state.clock.offset()

            if playing:
                sent_stop_for = None

                if ready:
                    await self.send({'type': 'media', 'state': 'playing', 'file': file, 'pos': pos, 'at': stamp_ms + offset})
            elif file and sent_stop_for != file:
                sent_stop_for = file
                await self.send({'type': 'media', 'state': 'stopped', 'file': file, 'pos': 0, 'at': 0})

    async def heartbeat(self):
        while True:
            await asyncio.sleep(HEARTBEAT_INTERVAL_S)
            await self.send({'type': 'hb'})

    async def send_playlist(self):
        with self.state.lock:
            name = self.state.playlist_name
            items = list(self.state.playlist_items)

        if name:
            await self.send({'type': 'playlist', 'name': name, 'items': items})

    async def wait_for_reconnect(self, seconds):
        try:
            await asyncio.wait_for(self.reconnect.wait(), seconds)
        except asyncio.TimeoutError:
            pass

        self.reconnect.clear()

    def set_error(self, message):
        with self.state.lock:
            self.state.last_error = message

        log.warning('relay: %s', message)

    # -- settings, uploads, status ----------------------------------------------

    async def watch_settings(self):
        """Pick up settings, token and upload requests from the plugin page."""
        last_upload = 0.0

        while True:
            await asyncio.sleep(2)

            settings = read_settings()
            token = read_token()

            with self.state.lock:
                changed = settings != self.state.settings or token != self.state.token
                self.state.settings = settings
                self.state.token = token

            if changed:
                log.info('settings changed; reconnecting')
                self.reconnect.set()

            if os.path.exists(UPLOAD_REQUEST_FILE):
                try:
                    os.remove(UPLOAD_REQUEST_FILE)
                except OSError:
                    pass

                self.upload_wanted.set()

            if time.monotonic() - last_upload > UPLOAD_INTERVAL_S:
                last_upload = time.monotonic()
                self.upload_wanted.set()

            write_status(self.state.snapshot())

    async def uploader(self):
        while True:
            await self.upload_wanted.wait()
            self.upload_wanted.clear()

            with self.state.lock:
                settings = self.state.settings
                token = self.state.token
                connected = self.state.connection == 'connected'

            if not (settings['enabled'] and settings['upload_media'] and token and connected):
                continue

            try:
                await asyncio.to_thread(upload_missing_media, self.state, settings['relay_url'], token)
            except Exception as error:  # noqa: BLE001
                with self.state.lock:
                    self.state.upload.update({'state': 'error', 'error': str(error) or type(error).__name__})

                log.warning('upload: %s', error)

    async def run(self):
        loop = asyncio.get_running_loop()
        daemon = self

        class Protocol(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                daemon.on_datagram(data)

        await loop.create_datagram_endpoint(Protocol, local_addr=('127.0.0.1', UDP_PORT))
        log.info('listening for fppd on 127.0.0.1:%d', UDP_PORT)

        await asyncio.gather(self.relay_loop(), self.watch_settings(), self.uploader())


# ---- blocking helpers (run in worker threads) ----------------------------------

def http_get_json(url, headers=None, timeout=LOCAL_TIMEOUT_S):
    request = urllib.request.Request(url, headers=headers or {})

    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode('utf-8', errors='replace'))


def read_playlist_media(name):
    """Media names in a saved playlist; [] when unreadable or unexpected."""
    if '/' in name:
        # On-the-fly playlists ("Song.fseq/Song.mp3") are not saved files.
        return []

    try:
        root = http_get_json(LOCAL_API + '/api/playlist/' + urllib.parse.quote(name, safe=''))
    except (OSError, ValueError) as error:
        log.info('playlist %s not readable: %s', name, error)
        return []

    if not isinstance(root, dict):
        return []

    items = []

    for section in ('leadIn', 'mainPlaylist', 'leadOut'):
        entries = root.get(section)

        if not isinstance(entries, list):
            continue

        for entry in entries:
            media = entry.get('mediaName') if isinstance(entry, dict) else None

            if isinstance(media, str) and media:
                items.append(media)

    return items


def to_int(value):
    if isinstance(value, bool):
        return None

    if isinstance(value, int):
        return value

    if isinstance(value, float):
        return int(value)

    if isinstance(value, str) and value.strip().isdigit():
        return int(value.strip())

    return None


def upload_missing_media(state, relay_url, token):
    def status(**fields):
        with state.lock:
            state.upload.update(fields)

    status(state='checking', done=0, total=0, error='')

    listing = http_get_json(LOCAL_API + '/api/files/Music')
    manifest = http_get_json(relay_url + '/api/media', {'Authorization': 'Bearer ' + token})

    files = listing.get('files') if isinstance(listing, dict) else None
    uploaded = manifest.get('files') if isinstance(manifest, dict) else None

    if not isinstance(files, list):
        raise ValueError('unexpected FPP music file list')

    if not isinstance(uploaded, dict):
        uploaded = {}

    pending = []

    for entry in files:
        if not isinstance(entry, dict):
            continue

        name = entry.get('name')
        size = to_int(entry.get('sizeBytes'))
        mtime = entry.get('mtime') if isinstance(entry.get('mtime'), str) else ''

        if not isinstance(name, str) or not name or '/' in name or size is None:
            continue

        if name.rsplit('.', 1)[-1].lower() not in AUDIO_EXTENSIONS:
            continue

        version = '%d-%s' % (size, mtime)

        if uploaded.get(name) != version:
            pending.append((name, version))

    status(state='uploading' if pending else 'up to date', total=len(pending))
    done = 0
    errors = []

    for name, version in pending:
        try:
            upload_one(relay_url, token, name, version)
            done += 1
            status(done=done)
            log.info('uploaded %s', name)
        except Exception as error:  # noqa: BLE001 - keep going with the other files
            errors.append('%s: %s' % (name, error))
            log.warning('upload of %s failed: %s', name, error)

    status(state='up to date' if not errors else 'incomplete', error='; '.join(errors)[:500], lastRun=int(time.time()))


def upload_one(relay_url, token, name, version):
    """Stream one file from the local FPP API to the relay via a temp file."""
    quoted = urllib.parse.quote(name, safe='')

    with tempfile.TemporaryFile(dir=DATA_DIR) as temp:
        with urllib.request.urlopen(LOCAL_API + '/api/file/Music/' + quoted, timeout=RELAY_TIMEOUT_S) as source:
            while True:
                chunk = source.read(1 << 16)

                if not chunk:
                    break

                temp.write(chunk)

        size = temp.tell()
        temp.seek(0)

        request = urllib.request.Request(
            relay_url + '/media/' + quoted + '?version=' + urllib.parse.quote(version, safe=''),
            data=temp,
            method='PUT',
            headers={
                'Authorization': 'Bearer ' + token,
                'Content-Length': str(size),
                'Content-Type': 'application/octet-stream',
            },
        )

        with urllib.request.urlopen(request, timeout=RELAY_TIMEOUT_S) as response:
            response.read()


def write_status(snapshot):
    try:
        temp = STATUS_FILE + '.tmp'

        with open(temp, 'w', encoding='utf-8') as handle:
            json.dump(snapshot, handle)

        os.chmod(temp, 0o644)
        os.replace(temp, STATUS_FILE)
    except OSError as error:
        log.debug('could not write status: %s', error)


def main():
    os.makedirs(DATA_DIR, exist_ok=True)
    logging.basicConfig(
        filename=os.path.join(LOG_DIR, 'plugin-%s.log' % PLUGIN),
        level=logging.INFO,
        format='%(asctime)s %(levelname)s %(message)s',
    )
    log.info('daemon starting')
    asyncio.run(Daemon().run())


if __name__ == '__main__':
    main()
