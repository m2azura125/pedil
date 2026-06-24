<?php
/**
 * rfid_auth.php
 * action=verify     : ESP32 verifikasi kartu RFID
 * action=verify_pin : Verifikasi PIN
 * action=enroll     : ESP32 mendaftarkan kartu baru (tanpa user, admin assign nanti)
 */
require_once 'config.php';
$db     = getDB();
$input  = getJsonInput();
$action = $input['action'] ?? 'verify';

// ── VERIFY RFID ──────────────────────────────────────────────
if ($action === 'verify') {
    $rfid_uid = $input['rfid_uid'] ?? '';
    $room_id  = (int)($input['room_id'] ?? 1);

    $stmt = $db->prepare(
        "SELECT rc.*, u.nama_lengkap, u.jabatan
         FROM rfid_cards rc
         LEFT JOIN users u ON rc.user_id = u.id
         WHERE rc.uid = ? AND rc.is_active = 1"
    );
    $stmt->execute([$rfid_uid]);
    $card = $stmt->fetch();

    $rstmt = $db->prepare("SELECT nama_ruangan FROM rooms WHERE id = ?");
    $rstmt->execute([$room_id]);
    $ruangan = $rstmt->fetch()['nama_ruangan'] ?? null;

    if ($card) {
        $db->prepare(
            "INSERT INTO access_logs
             (tanggal, waktu, rfid_uid, rfid_label, user_id, nama_petugas, jabatan, room_id, ruangan, aksi, metode, status)
             VALUES (CURDATE(), CURTIME(), ?, ?, ?, ?, ?, ?, ?, 'Buka', 'RFID', 'Berhasil')"
        )->execute([
            $rfid_uid, $card['label'], $card['user_id'],
            $card['nama_lengkap'], $card['jabatan'],
            $room_id, $ruangan
        ]);
        $db->prepare("UPDATE rooms SET last_access = NOW() WHERE id = ?")->execute([$room_id]);
        jsonResponse([
            'authorized' => true,
            'user'       => $card['nama_lengkap'],
            'jabatan'    => $card['jabatan']
        ]);
    } else {
        $db->prepare(
            "INSERT INTO access_logs
             (tanggal, waktu, rfid_uid, room_id, ruangan, aksi, metode, status, keterangan)
             VALUES (CURDATE(), CURTIME(), ?, ?, ?, 'Buka', 'RFID', 'Ditolak', 'Kartu tidak terdaftar')"
        )->execute([$rfid_uid, $room_id, $ruangan]);
        jsonResponse(['authorized' => false, 'message' => 'Kartu tidak terdaftar'], 401);
    }
}

// ── VERIFY PIN ───────────────────────────────────────────────
if ($action === 'verify_pin') {
    $pin     = $input['pin']     ?? '';
    $room_id = (int)($input['room_id'] ?? 1);

    $stmt = $db->prepare(
        "SELECT pa.*, u.nama_lengkap, u.jabatan
         FROM pin_access pa
         LEFT JOIN users u ON pa.user_id = u.id
         WHERE pa.pin_code = ? AND pa.is_active = 1"
    );
    $stmt->execute([$pin]);
    $result = $stmt->fetch();

    if ($result) {
        $rstmt = $db->prepare("SELECT nama_ruangan FROM rooms WHERE id = ?");
        $rstmt->execute([$room_id]);
        $ruangan = $rstmt->fetch()['nama_ruangan'] ?? null;
        $db->prepare(
            "INSERT INTO access_logs
             (tanggal, waktu, user_id, nama_petugas, jabatan, room_id, ruangan, aksi, metode, status)
             VALUES (CURDATE(), CURTIME(), ?, ?, ?, ?, ?, 'Buka', 'PIN', 'Berhasil')"
        )->execute([$result['user_id'], $result['nama_lengkap'], $result['jabatan'], $room_id, $ruangan]);
        $db->prepare("UPDATE rooms SET last_access = NOW() WHERE id = ?")->execute([$room_id]);
        jsonResponse(['authorized' => true, 'user' => $result['nama_lengkap']]);
    } else {
        $stmt = $db->prepare(
            "SELECT COUNT(*) as c FROM access_logs
             WHERE metode = 'PIN' AND status = 'Ditolak'
               AND created_at > DATE_SUB(NOW(), INTERVAL 10 MINUTE)"
        );
        $stmt->execute();
        $fails = $stmt->fetch()['c'] + 1;
        $db->prepare(
            "INSERT INTO access_logs (tanggal, waktu, metode, status, keterangan)
             VALUES (CURDATE(), CURTIME(), 'PIN', 'Ditolak', 'PIN salah')"
        )->execute();
        if ($fails >= 3) {
            $db->prepare(
                "INSERT INTO lockouts (rfid_uid, lockout_until, reason)
                 VALUES ('PIN_LOCKOUT', DATE_ADD(NOW(), INTERVAL 5 MINUTE), 'PIN salah 3x')"
            )->execute();
            jsonResponse([
                'authorized'       => false,
                'lockout'          => true,
                'remaining_seconds'=> 300,
                'message'          => 'Akses dikunci 5 menit'
            ], 403);
        }
        jsonResponse(['authorized' => false, 'fail_count' => $fails, 'message' => 'PIN salah'], 401);
    }
}

// ── ENROLL KARTU BARU (dari ESP32) ───────────────────────────
if ($action === 'enroll') {
    $rfid_uid = $input['rfid_uid'] ?? '';
    $label    = $input['label']    ?? 'ENROLL-ESP';

    if (!$rfid_uid) jsonResponse(['success' => false, 'message' => 'UID kosong'], 400);

    // Cek apakah sudah ada
    $chk = $db->prepare("SELECT id FROM rfid_cards WHERE uid = ?");
    $chk->execute([$rfid_uid]);
    if ($chk->fetch()) {
        jsonResponse(['success' => false, 'message' => 'Kartu sudah terdaftar']);
    }

    $db->prepare(
        "INSERT INTO rfid_cards (uid, label, is_active) VALUES (?, ?, 1)"
    )->execute([$rfid_uid, $label]);

    jsonResponse([
        'success' => true,
        'message' => 'Kartu terdaftar, assign user via dashboard'
    ]);
}
