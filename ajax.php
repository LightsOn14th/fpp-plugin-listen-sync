<?php
// Small JSON endpoints for status.php. Loaded through
// plugin.php?plugin=fpp-plugin-listen-sync&page=ajax.php&nopage=1&action=...
// Talks to the daemon only through files in plugindata/.

$dataDir = $settings['mediaDirectory'] . '/plugindata/fpp-plugin-listen-sync';
$action = isset($_GET['action']) ? $_GET['action'] : 'status';

header('Content-Type: application/json');
header('Cache-Control: no-store');

function listenSyncStatus($dataDir)
{
    global $settings;

    $status = array();
    $file = $dataDir . '/status.json';

    if (is_readable($file)) {
        $decoded = json_decode(file_get_contents($file), true);

        if (is_array($decoded)) {
            $status = $decoded;
        }
    }

    $status['daemonRunning'] = isset($status['updated']) && (time() - intval($status['updated'])) < 10;
    $status['multiSyncEnabled'] = isset($settings['MultiSyncEnabled']) && $settings['MultiSyncEnabled'] == '1';

    return $status;
}

if (!is_dir($dataDir)) {
    @mkdir($dataDir, 0775, true);
}

if ($action === 'token' && $_SERVER['REQUEST_METHOD'] === 'POST') {
    $token = trim(file_get_contents('php://input'));

    if ($token === '' || strlen($token) > 512 || preg_match('/\s/', $token)) {
        http_response_code(400);
        echo json_encode(array('error' => 'Token is empty or not valid.'));

        return;
    }

    $path = $dataDir . '/token';

    if (file_put_contents($path, $token . "\n", LOCK_EX) === false) {
        http_response_code(500);
        echo json_encode(array('error' => 'Token could not be written.'));

        return;
    }

    @chmod($path, 0640);
    echo json_encode(listenSyncStatus($dataDir));

    return;
}

if ($action === 'upload' && $_SERVER['REQUEST_METHOD'] === 'POST') {
    @touch($dataDir . '/upload-request');
    echo json_encode(listenSyncStatus($dataDir));

    return;
}

echo json_encode(listenSyncStatus($dataDir));
