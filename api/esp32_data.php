<?php
/**
 * esp32_data.php
 * POST : ESP32 mengirim snapshot sensor (stok, pintu, getaran, state)
 * GET  : Dashboard mengambil data sensor terbaru
 */
require_once 'config.php';
$db     = getDB();
$method = $_SERVER['REQUEST_METHOD'];

// ── TERIMA DATA DARI ESP32 (POST) ────────────────────────────
if ($method === 'POST') {
    $input       = getJsonInput();
    $node_id     = $input['node_id']     ?? '';
    $sensors     = $input['sensors']     ?? [];
    $door_status = $input['door_status'] ?? [];
    $vibration   = $input['vibration']   ?? [];
    $room_id     = (int)($input['room_id'] ?? 1);
    $sys_state   = $input['sys_state']   ?? 'IDLE';
    $relay_open  = $input['relay_open']  ?? false;
    $nano_ok     = $input['nano_ok']     ?? false;

    // Update node: heartbeat + IP + state snapshot ke config_json
    $ip  = $_SERVER['REMOTE_ADDR'] ?? '';
    $cfg = json_encode([
        'sys_state'  => $sys_state,
        'relay_open' => $relay_open,
        'nano_ok'    => $nano_ok,
    ]);
    $db->prepare(
        "UPDATE esp32_nodes
         SET last_heartbeat = NOW(), is_online = 1, ip_address = ?, config_json = ?
         WHERE node_id = ?"
    )->execute([$ip, $cfg, $node_id]);

    // ── Sensor ultrasonik (stok kotak) ───────────────────────
    foreach ($sensors as $i => $val) {
        $value  = $val['percent'] ?? $val['distance'] ?? 0;
        $unit   = isset($val['percent']) ? '%' : ($val['unit'] ?? 'cm');
        $status = $val['status'] ?? 'normal';
        $db->prepare(
            "INSERT INTO sensor_data (room_id, sensor_type, sensor_index, value, unit, status)
             VALUES (?, 'ultrasonic', ?, ?, ?, ?)"
        )->execute([$room_id, $i, $value, $unit, $status]);
    }

    // ── Getaran ───────────────────────────────────────────────
    foreach ($vibration as $i => $val) {
        $level = $val['level'] ?? 'normal';
        $db->prepare(
            "INSERT INTO sensor_data (room_id, sensor_type, sensor_index, value, status)
             VALUES (?, 'vibration', ?, ?, ?)"
        )->execute([$room_id, $i, $val['value'] ?? 0, $level]);

        if ($level === 'danger') {
            // Cek apakah alert getaran aktif sudah ada (hindari duplikat)
            $chk = $db->prepare(
                "SELECT id FROM alerts
                 WHERE room_id = ? AND alert_type = 'vibration_danger'
                   AND is_resolved = 0
                   AND created_at > DATE_SUB(NOW(), INTERVAL 1 MINUTE)"
            );
            $chk->execute([$room_id]);
            if (!$chk->fetch()) {
                $db->prepare(
                    "INSERT INTO alerts (room_id, alert_type, severity, message)
                     VALUES (?, 'vibration_danger', 'critical', ?)"
                )->execute([$room_id, 'Getaran bahaya terdeteksi pada sensor ' . ($i+1)]);
            }
        }
    }

    // ── Door switch ───────────────────────────────────────────
    foreach ($door_status as $i => $val) {
        $closed = isset($val['closed']) ? (bool)$val['closed'] : true;
        $status = $closed ? 'closed' : 'open';
        $db->prepare(
            "INSERT INTO sensor_data (room_id, sensor_type, sensor_index, value, status)
             VALUES (?, 'door_switch', ?, ?, ?)"
        )->execute([$room_id, $i, $closed ? 1 : 0, $status]);

        // Alert pintu terbuka saat relay tidak dibuka (paksa)
        if (!$closed && !$relay_open) {
            $chk = $db->prepare(
                "SELECT id FROM alerts
                 WHERE room_id = ? AND alert_type = 'door_forced'
                   AND is_resolved = 0
                   AND created_at > DATE_SUB(NOW(), INTERVAL 5 MINUTE)"
            );
            $chk->execute([$room_id]);
            if (!$chk->fetch()) {
                $db->prepare(
                    "INSERT INTO alerts (room_id, alert_type, severity, message)
                     VALUES (?, 'door_forced', 'high', ?)"
                )->execute([$room_id, 'Pintu ' . ($i+1) . ' terbuka tanpa otorisasi!']);
            }
        }
    }

    // ── Update kapasitas ruangan berdasarkan rata-rata stok ───
    if (!empty($sensors)) {
        $pcts = array_filter(array_map(fn($s) => $s['percent'] ?? -1, $sensors), fn($v) => $v >= 0);
        if ($pcts) {
            $avgPct = array_sum($pcts) / count($pcts);
            $roomStmt = $db->prepare("SELECT total_slot FROM rooms WHERE id = ?");
            $roomStmt->execute([$room_id]);
            $room = $roomStmt->fetch();
            if ($room) {
                $slotTerisi = (int)round($avgPct / 100 * $room['total_slot']);
                $statusRoom = $avgPct >= 90 ? 'Penuh' : ($avgPct >= 75 ? 'Hampir Penuh' : 'Normal');
                $db->prepare(
                    "UPDATE rooms SET slot_terisi = ?, status = ? WHERE id = ?"
                )->execute([$slotTerisi, $statusRoom, $room_id]);
            }
        }
    }

    // Resolve alert vibration jika sudah aman
    $allSafe = true;
    foreach ($vibration as $val) {
        if (($val['level'] ?? 'normal') === 'danger') { $allSafe = false; break; }
    }
    if ($allSafe) {
        $db->prepare(
            "UPDATE alerts SET is_resolved = 1, resolved_at = NOW()
             WHERE room_id = ? AND alert_type = 'vibration_danger' AND is_resolved = 0"
        )->execute([$room_id]);
    }

    jsonResponse(['success' => true]);
}

// ── BACA DATA SENSOR (GET) ───────────────────────────────────
if ($method === 'GET') {
    $room_id = (int)($_GET['room_id'] ?? 1);

    $latest = $db->prepare(
        "SELECT * FROM sensor_data WHERE room_id = ? ORDER BY recorded_at DESC LIMIT 24"
    );
    $latest->execute([$room_id]);

    $alerts = $db->prepare(
        "SELECT * FROM alerts WHERE room_id = ? AND is_resolved = 0 ORDER BY created_at DESC LIMIT 10"
    );
    $alerts->execute([$room_id]);

    jsonResponse(['sensors' => $latest->fetchAll(), 'alerts' => $alerts->fetchAll()]);
}
