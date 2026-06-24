<?php
/**
 * esp32_command.php
 * GET  : ESP32 polling perintah (juga update heartbeat node)
 * POST : Web dashboard mengirim perintah ke ESP32
 */
require_once 'config.php';
$db     = getDB();
$method = $_SERVER['REQUEST_METHOD'];

// ── ESP32 POLLING (GET) ──────────────────────────────────────
if ($method === 'GET') {
    $node_id = $_GET['node_id'] ?? '';
    if (!$node_id) jsonResponse(['command' => 'noop']);

    // Update heartbeat & IP node
    $ip = $_SERVER['REMOTE_ADDR'] ?? '';
    $db->prepare("UPDATE esp32_nodes SET last_heartbeat = NOW(), is_online = 1, ip_address = ? WHERE node_id = ?")
       ->execute([$ip, $node_id]);

    // Ambil perintah pertama yang belum dieksekusi
    $stmt = $db->prepare(
        "SELECT * FROM esp32_commands
         WHERE node_id = ? AND is_executed = 0
         ORDER BY id ASC LIMIT 1"
    );
    $stmt->execute([$node_id]);
    $cmd = $stmt->fetch();

    if ($cmd) {
        $db->prepare("UPDATE esp32_commands SET is_executed = 1, executed_at = NOW() WHERE id = ?")
           ->execute([$cmd['id']]);
        jsonResponse([
            'command' => $cmd['command'],
            'params'  => json_decode($cmd['params'] ?? '{}', true),
            'id'      => $cmd['id']
        ]);
    }

    jsonResponse(['command' => 'noop']);
}

// ── WEB DASHBOARD MENGIRIM PERINTAH (POST) ──────────────────
if ($method === 'POST') {
    $input   = getJsonInput();
    $node_id = $input['node_id'] ?? '';
    $command = $input['command'] ?? '';
    $params  = $input['params']  ?? [];

    $valid_commands = ['force_lock', 'force_unlock', 'reboot', 'noop'];
    if (!$node_id || !in_array($command, $valid_commands)) {
        jsonResponse(['success' => false, 'message' => 'Parameter tidak valid'], 400);
    }

    // Hapus perintah lama yang belum dieksekusi untuk node yang sama
    $db->prepare("DELETE FROM esp32_commands WHERE node_id = ? AND is_executed = 0")
       ->execute([$node_id]);

    $db->prepare("INSERT INTO esp32_commands (node_id, command, params) VALUES (?, ?, ?)")
       ->execute([$node_id, $command, json_encode($params)]);

    jsonResponse(['success' => true, 'message' => "Perintah '$command' dikirim ke $node_id"]);
}
