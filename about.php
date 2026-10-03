<div class="settings">
    <h2>Listen Sync - About</h2>

    <p>
        Listen Sync lets visitors play the show audio on their phones, in sync with the lights.
        This plugin sends FPP's playback position to a Listen Sync relay and uploads the audio
        files from the Music folder so phones can download each song before it plays.
        Audio is not streamed from this device.
    </p>

    <h3>Setup</h3>
    <ol>
        <li>Turn on "Send MultiSync" in FPP Settings &gt; MultiSync. The plugin reads the playback position from MultiSync.</li>
        <li>Open Status/Control &gt; Listen Sync, check the relay URL and save the relay token.</li>
        <li>The status card shows "connected" once the relay accepts the token. Uploads start automatically and repeat every 15 minutes.</li>
    </ol>

    <h3>Notes</h3>
    <ul>
        <li>Only audio files in the Music folder are uploaded (mp3, m4a, aac, ogg, opus, wav, flac). Video files are not supported.</li>
        <li>The relay token is stored in plugindata/fpp-plugin-listen-sync/ and is removed when the plugin is uninstalled.</li>
        <li>Network traffic runs in a separate background process, never inside fppd. Its messages are written to logs/plugin-fpp-plugin-listen-sync.log.</li>
    </ul>

    <p>
        Source and issues: <a href="https://github.com/LightsOn14th/fpp-plugin-listen-sync" target="_blank" rel="noopener">github.com/LightsOn14th/fpp-plugin-listen-sync</a>
    </p>
</div>
