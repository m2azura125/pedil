<?php
/**
 * manage_credential.php
 * GET: Fetch current RFID, PIN, and last unregistered scan
 * POST: Update (inject) RFID or PIN for currently logged-in user
 */
require_once 'config.php';
$db = getDB();

// Cek autentikasi session
if (!isset($_SESSION['user_id'])) {
    jsonResponse(['success' => false, 'message' => 'Silakan login terlebih dahulu'], 401);
}
$user_id = $_SESSION['user_id'];
$method = $_SERVER['REQUEST_METHOD'];

// ── GET INFO KREDENSIAL ───────────────────────────────────────
if ($method === 'GET') {
    // 1. RFID Aktif saat ini
    $rfidStmt = $db->prepare("SELECT uid, label FROM rfid_cards WHERE user_id = ? AND is_active = 1 LIMIT 1");
    $rfidStmt->execute([$user_id]);
    $current_rfid = $rfidStmt->fetch();

    // 2. PIN Aktif saat ini (secara global)
    $pinStmt = $db->prepare("SELECT pin_code FROM pin_access WHERE is_active = 1 LIMIT 1");
    $pinStmt->execute();
    $current_pin = $pinStmt->fetch();

    // 3. Scan RFID Terakhir yang Ditolak (unregistered) dari ESP32
    $lastScanStmt = $db->query(
        "SELECT rfid_uid, tanggal, waktu 
         FROM access_logs 
         WHERE status = 'Ditolak' 
           AND rfid_uid IS NOT NULL 
           AND rfid_uid != 'PIN_LOCKOUT' 
           AND rfid_uid != ''
         ORDER BY id DESC LIMIT 1"
    );
    $last_scanned = $lastScanStmt->fetch();

    jsonResponse([
        'success' => true,
        'current_rfid' => $current_rfid ? [
            'uid' => $current_rfid['uid'],
            'label' => $current_rfid['label']
        ] : null,
        'current_pin' => $current_pin ? $current_pin['pin_code'] : null,
        'last_scanned_rfid' => $last_scanned ? [
            'uid' => $last_scanned['rfid_uid'],
            'timestamp' => $last_scanned['tanggal'] . ' ' . $last_scanned['waktu']
        ] : null
    ]);
}

// ── UPDATE KREDENSIAL (POST) ──────────────────────────────────
if ($method === 'POST') {
    $input = getJsonInput();
    $action = $input['action'] ?? '';

    // A. UPDATE RFID
    if ($action === 'update_rfid') {
        $rfid_uid = trim($input['rfid_uid'] ?? '');
        $rfid_label = trim($input['rfid_label'] ?? '');

        if (empty($rfid_uid)) {
            jsonResponse(['success' => false, 'message' => 'UID RFID tidak boleh kosong'], 400);
        }

        if (empty($rfid_label)) {
            $rfid_label = 'RFID-' . substr($rfid_uid, 0, 4);
        }

        // Mulai transaksi database
        $db->beginTransaction();
        try {
            // Bebaskan kartu UID ini dari user lain terlebih dahulu untuk menghindari UNIQUE constraint violation
            $db->prepare("UPDATE users SET rfid_uid = NULL, rfid_label = NULL WHERE rfid_uid = ?")->execute([$rfid_uid]);

            // Nonaktifkan kartu lama milik user ini
            $db->prepare("UPDATE rfid_cards SET user_id = NULL, is_active = 0 WHERE user_id = ?")->execute([$user_id]);

            // Cek apakah kartu dengan UID ini sudah terdaftar sebelumnya di sistem
            $chk = $db->prepare("SELECT id FROM rfid_cards WHERE uid = ?");
            $chk->execute([$rfid_uid]);
            if ($chk->fetch()) {
                // Update kepemilikan kartu yang ada ke user ini
                $db->prepare(
                    "UPDATE rfid_cards 
                     SET user_id = ?, label = ?, is_active = 1 
                     WHERE uid = ?"
                )->execute([$user_id, $rfid_label, $rfid_uid]);
            } else {
                // Insert kartu baru
                $db->prepare(
                    "INSERT INTO rfid_cards (uid, label, user_id, is_active) 
                     VALUES (?, ?, ?, 1)"
                )->execute([$rfid_uid, $rfid_label, $user_id]);
            }

            // Sinkronisasi data ke tabel users untuk backward compatibility
            $db->prepare(
                "UPDATE users 
                 SET rfid_uid = ?, rfid_label = ? 
                 WHERE id = ?"
            )->execute([$rfid_uid, $rfid_label, $user_id]);

            $db->commit();
            jsonResponse(['success' => true, 'message' => 'Kartu RFID berhasil di-inject ke akun Anda']);
        } catch (Throwable $e) {
            $db->rollBack();
            jsonResponse(['success' => false, 'message' => 'Gagal menyimpan kartu RFID: ' . $e->getMessage()], 500);
        }
    }

    // B. UPDATE PIN
    if ($action === 'update_pin') {
        $pin_code = trim($input['pin_code'] ?? '');

        if (empty($pin_code)) {
            jsonResponse(['success' => false, 'message' => 'PIN tidak boleh kosong'], 400);
        }

        if (!preg_match('/^[0-9A-F]{4,10}$/i', $pin_code)) {
            jsonResponse(['success' => false, 'message' => 'PIN harus berupa 4-10 karakter alfanumerik / angka hex (0-9, A-F)'], 400);
        }

        $db->beginTransaction();
        try {
            // Cari ID user 'Tanpa Kartu'
            $uStmt = $db->prepare("SELECT id FROM users WHERE username = 'tanpa.kartu'");
            $uStmt->execute();
            $tanpa_kartu_user = $uStmt->fetch();
            $target_user_id = $tanpa_kartu_user ? $tanpa_kartu_user['id'] : $user_id;

            // Nonaktifkan PIN lama secara global
            $db->prepare("UPDATE pin_access SET is_active = 0")->execute();

            // Tambahkan PIN baru yang diasosiasikan dengan user 'Tanpa Kartu'
            $db->prepare(
                "INSERT INTO pin_access (pin_code, user_id, is_active) 
                 VALUES (?, ?, 1)"
            )->execute([$pin_code, $target_user_id]);

            $db->commit();
            jsonResponse(['success' => true, 'message' => 'PIN akses darurat berhasil di-update secara global untuk semua akun']);
        } catch (Throwable $e) {
            $db->rollBack();
            jsonResponse(['success' => false, 'message' => 'Gagal menyimpan PIN: ' . $e->getMessage()], 500);
        }
    }

    // C. UPDATE PROFIL (nama, email, username)
    if ($action === 'update_profile') {
        $nama_lengkap = trim($input['nama_lengkap'] ?? '');
        $email = trim($input['email'] ?? '');
        $username = trim($input['username'] ?? '');

        if (empty($nama_lengkap)) {
            jsonResponse(['success' => false, 'message' => 'Nama lengkap tidak boleh kosong'], 400);
        }
        if (empty($username)) {
            jsonResponse(['success' => false, 'message' => 'Nama pengguna tidak boleh kosong'], 400);
        }
        if (!preg_match('/^[a-zA-Z0-9._-]{3,50}$/', $username)) {
            jsonResponse(['success' => false, 'message' => 'Nama pengguna hanya boleh huruf, angka, titik, garis bawah/hubung (3-50 karakter)'], 400);
        }
        if ($email !== '' && !filter_var($email, FILTER_VALIDATE_EMAIL)) {
            jsonResponse(['success' => false, 'message' => 'Format email tidak valid'], 400);
        }

        $chk = $db->prepare("SELECT id FROM users WHERE username = ? AND id != ?");
        $chk->execute([$username, $user_id]);
        if ($chk->fetch()) {
            jsonResponse(['success' => false, 'message' => 'Nama pengguna sudah dipakai akun lain'], 409);
        }

        $db->prepare("UPDATE users SET nama_lengkap = ?, email = ?, username = ? WHERE id = ?")
           ->execute([$nama_lengkap, $email, $username, $user_id]);

        $stmt = $db->prepare("SELECT * FROM users WHERE id = ?");
        $stmt->execute([$user_id]);
        $user = $stmt->fetch();
        unset($user['password']);

        jsonResponse(['success' => true, 'message' => 'Profil berhasil disimpan', 'user' => $user]);
    }

    // D. UPDATE PASSWORD
    if ($action === 'update_password') {
        $current_password = $input['current_password'] ?? '';
        $new_password = $input['new_password'] ?? '';

        if (empty($current_password) || empty($new_password)) {
            jsonResponse(['success' => false, 'message' => 'Password lama dan baru wajib diisi'], 400);
        }
        if (strlen($new_password) < 6) {
            jsonResponse(['success' => false, 'message' => 'Password baru minimal 6 karakter'], 400);
        }

        $stmt = $db->prepare("SELECT password FROM users WHERE id = ?");
        $stmt->execute([$user_id]);
        $user = $stmt->fetch();

        if (!$user || !password_verify($current_password, $user['password'])) {
            jsonResponse(['success' => false, 'message' => 'Password lama salah'], 401);
        }

        $hash = password_hash($new_password, PASSWORD_DEFAULT);
        $db->prepare("UPDATE users SET password = ? WHERE id = ?")->execute([$hash, $user_id]);

        jsonResponse(['success' => true, 'message' => 'Password berhasil diubah']);
    }

    jsonResponse(['success' => false, 'message' => 'Aksi tidak valid'], 400);
}
