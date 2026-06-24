<?php
/**
 * sensor_live.php
 * Mengembalikan snapshot sensor terbaru dari ESP32 untuk dashboard live.
 * Data diambil dari sensor_data (5 menit terakhir) dan status node.
 */
require_once 'config.php';
$db      = getDB();
$node_id = $_GET['node_id'] ?? 'NODE-A';

// Info node
$nodeStmt = $db->prepare("SELECT * FROM esp32_nodes WHERE node_id = ?");
$nodeStmt->execute([$node_id]);
$node = $nodeStmt->fetch();

$room_id = $node['room_id'] ?? 1;

// Online jika heartbeat < 30 detik
$isOnline = false;
if ($node && $node['last_heartbeat']) {
    $isOnline = (time() - strtotime($node['last_heartbeat'])) < 30;
}

// Sensor terbaru (5 menit terakhir)
$sStmt = $db->prepare(
    "SELECT sensor_type, sensor_index, value, unit, status, recorded_at
     FROM sensor_data
     WHERE room_id = ? AND recorded_at > DATE_SUB(NOW(), INTERVAL 5 MINUTE)
     ORDER BY recorded_at DESC
     LIMIT 40"
);
$sStmt->execute([$room_id]);
$rows = $sStmt->fetchAll();

// Susun ke array indeks
$stok  = [null, null, null, null];
$doors = [null, null];
$vibs  = [null, null];

foreach ($rows as $r) {
    $idx = (int)$r['sensor_index'];
    if ($r['sensor_type'] === 'ultrasonic' && $idx < 4 && $stok[$idx] === null) {
        $stok[$idx] = [
            'percent' => round((float)$r['value'], 1),
            'status'  => $r['status'],
            'at'      => $r['recorded_at']
        ];
    } elseif ($r['sensor_type'] === 'door_switch' && $idx < 2 && $doors[$idx] === null) {
        $doors[$idx] = [
            'closed' => ((int)$r['value'] === 1),
            'status' => $r['status'],
            'at'     => $r['recorded_at']
        ];
    } elseif ($r['sensor_type'] === 'vibration' && $idx < 2 && $vibs[$idx] === null) {
        $vibs[$idx] = [
            'level' => $r['status'],
            'at'    => $r['recorded_at']
        ];
    }
}

// Alert aktif (belum resolved)
$aStmt = $db->prepare(
    "SELECT * FROM alerts WHERE room_id = ? AND is_resolved = 0 ORDER BY created_at DESC LIMIT 5"
);
$aStmt->execute([$room_id]);
$alerts = $aStmt->fetchAll();

// State dari config_json node
$cfgRaw = $node['config_json'] ?? '{}';
$cfg    = json_decode($cfgRaw, true) ?? [];

jsonResponse([
    'online'     => $isOnline,
    'node_id'    => $node_id,
    'ip'         => $node['ip_address'] ?? null,
    'last_seen'  => $node['last_heartbeat'] ?? null,
    'sys_state'  => $cfg['sys_state']  ?? 'UNKNOWN',
    'relay_open' => $cfg['relay_open'] ?? false,
    'nano_ok'    => $cfg['nano_ok']    ?? false,
    'stok'       => $stok,
    'doors'      => $doors,
    'vibration'  => $vibs,
    'alerts'     => $alerts,
    'ts'         => date('H:i:s')
]);
