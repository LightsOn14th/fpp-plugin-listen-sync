<?php
$pluginName = 'fpp-plugin-listen-sync';
?>
<div id="listenSync" class="settings">
    <h2>Listen Sync</h2>

    <div id="lsMultiSyncWarning" class="alert alert-warning d-none" role="alert">
        MultiSync is turned off, so FPP is not reporting the playback position. Turn on
        "Send MultiSync" in FPP Settings &gt; MultiSync.
    </div>

    <div class="row g-3">
        <div class="col-12 col-lg-6">
            <div class="card h-100">
                <div class="card-header">Status</div>
                <div class="card-body">
                    <dl class="row mb-0">
                        <dt class="col-5">Relay connection</dt>
                        <dd class="col-7" id="lsConnection">-</dd>
                        <dt class="col-5">Last error</dt>
                        <dd class="col-7" id="lsLastError">-</dd>
                        <dt class="col-5">Round trip</dt>
                        <dd class="col-7" id="lsRtt">-</dd>
                        <dt class="col-5">Now sending</dt>
                        <dd class="col-7 text-break" id="lsNow">-</dd>
                        <dt class="col-5">Playlist</dt>
                        <dd class="col-7 text-break" id="lsPlaylist">-</dd>
                        <dt class="col-5">Media upload</dt>
                        <dd class="col-7" id="lsUpload">-</dd>
                    </dl>
                    <button type="button" class="buttons btn-outline-success mt-3" id="lsUploadNow">Upload Now</button>
                </div>
            </div>
        </div>

        <div class="col-12 col-lg-6">
            <div class="card h-100">
                <div class="card-header">Settings</div>
                <div class="card-body">
                    <div class="mb-3">
                        <?php PrintSettingCheckbox('Enabled', 'ListenSyncEnabled', 0, 0, '1', '0', $pluginName, '', 1); ?>
                        <label class="form-label">Enabled</label>
                    </div>

                    <div class="mb-3">
                        <label class="form-label" for="ListenSyncRelayURL">Relay URL</label>
                        <div>
                            <?php PrintSettingTextSaved('ListenSyncRelayURL', 0, 0, 255, 40, $pluginName, 'https://listen.lightson14th.com'); ?>
                        </div>
                    </div>

                    <div class="mb-3">
                        <?php PrintSettingCheckbox('Upload Media', 'ListenSyncUploadMedia', 0, 0, '1', '0', $pluginName, '', 1); ?>
                        <label class="form-label">Upload audio files from the Music folder to the relay</label>
                    </div>

                    <div class="mb-1">
                        <label class="form-label" for="lsToken">Relay token</label>
                        <div class="input-group">
                            <input type="password" class="form-control" id="lsToken" autocomplete="off" maxlength="512">
                            <button type="button" class="buttons btn-outline-success" id="lsSaveToken">Save Token</button>
                        </div>
                        <div class="form-text" id="lsTokenState">-</div>
                    </div>
                </div>
            </div>
        </div>
    </div>
</div>

<script type="text/javascript">
( function () {
    var API = 'plugin.php?plugin=fpp-plugin-listen-sync&page=ajax.php&nopage=1&action=';

    function text( id, value ) {
        document.getElementById( id ).textContent = value;
    }

    function ms( value ) {
        return value === null || value === undefined ? '-' : Math.round( value ) + ' ms';
    }

    function render( status ) {
        text( 'lsConnection', ! status.daemonRunning ? 'daemon not running' : ( status.enabled ? status.connection : 'disabled' ) );
        text( 'lsLastError', status.lastError || '-' );
        text( 'lsRtt', ms( status.rttMs ) );
        text( 'lsNow', status.playing && status.position !== null ? status.file + ' at ' + status.position.toFixed( 1 ) + ' s' : 'Not playing' );
        text( 'lsPlaylist', status.playlist ? status.playlist + ' (' + status.playlistItems + ' audio files)' : '-' );

        var upload = status.upload || { state: '-', done: 0, total: 0, error: '' };
        var uploadText = status.uploadMedia ? upload.state : 'off';

        if ( upload.total > 0 ) {
            uploadText += ' (' + upload.done + ' of ' + upload.total + ')';
        }

        if ( upload.error ) {
            uploadText += ' - ' + upload.error;
        }

        text( 'lsUpload', uploadText );
        text( 'lsTokenState', status.tokenSet ? 'A token is saved.' : 'No token saved.' );
        document.getElementById( 'lsMultiSyncWarning' ).classList.toggle( 'd-none', status.multiSyncEnabled );
    }

    function refresh() {
        $.get( API + 'status' ).done( render ).fail( function () {
            text( 'lsConnection', 'status not available' );
        } );
    }

    document.getElementById( 'lsSaveToken' ).addEventListener( 'click', function () {
        var input = document.getElementById( 'lsToken' );
        var value = input.value.trim();

        if ( ! value ) {
            return;
        }

        $.ajax( { url: API + 'token', method: 'POST', data: value, contentType: 'text/plain', processData: false } )
            .done( function ( status ) {
                input.value = '';
                render( status );
                $.jGrowl( 'Token saved', { themeState: 'success' } );
            } )
            .fail( function () {
                $.jGrowl( 'Token could not be saved', { themeState: 'danger' } );
            } );
    } );

    document.getElementById( 'lsUploadNow' ).addEventListener( 'click', function () {
        $.post( API + 'upload' ).done( render );
    } );

    refresh();
    setInterval( refresh, 2000 );
} )();
</script>
