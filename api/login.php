<?php
require_once 'config.php';
$method = $_SERVER['REQUEST_METHOD'];
$input = getJsonInput();
if ($method !== 'POST') { jsonResponse(['error' => 'Method not allowed'], 405); }
$action = $input['action'] ?? 'login';

switch ($action) {
    case 'login': handleLogin($input); break;
    case 'rfid': handleRfidLogin($input); break;
    case 'logout': session_destroy(); jsonResponse(['success' => true]); break;
    case 'check_session': checkSession(); break;
    default: jsonResponse(['error' => 'Invalid'], 400);
}

function handleLogin($input) {
    $username = $input['username'] ?? '';
    $password = $input['password'] ?? '';
    if (empty($username) || empty($password)) { jsonResponse(['error' => 'Wajib diisi'], 400); }
    $db = getDB();
    $stmt = $db->prepare("SELECT * FROM users WHERE username = ? AND is_active = 1");
    $stmt->execute([$username]);
    $user = $stmt->fetch();
    if (!$user || !password_verify($password, $user['password'])) {
        jsonResponse(['error' => 'Username atau password salah'], 401);
    }
    $_SESSION['user_id'] = $user['id'];
    $_SESSION['jabatan'] = $user['jabatan'];
    unset($user['password']);
    jsonResponse(['success' => true, 'user' => $user]);
}

function handleRfidLogin($input) {
    $rfid_uid = $input['rfid_uid'] ?? '';
    if (empty($rfid_uid)) { jsonResponse(['error' => 'RFID wajib'], 400); }
    $db = getDB();
    $stmt = $db->prepare("SELECT * FROM lockouts WHERE rfid_uid = ? AND lockout_until > NOW()");
    $stmt->execute([$rfid_uid]);
    if ($lockout = $stmt->fetch()) {
        $remaining = strtotime($lockout['lockout_until']) - time();
        jsonResponse(['error' => 'Akses dikunci', 'lockout' => true, 'remaining_seconds' => $remaining], 403);
    }
    $stmt = $db->prepare("SELECT rc.*, u.* FROM rfid_cards rc LEFT JOIN users u ON rc.user_id = u.id WHERE rc.uid = ? AND rc.is_active = 1");
    $stmt->execute([$rfid_uid]);
    $card = $stmt->fetch();
    if (!$card || !$card['user_id']) {
        $stmt = $db->prepare("SELECT COUNT(*) as c FROM access_logs WHERE rfid_uid = ? AND status = 'Ditolak' AND created_at > DATE_SUB(NOW(), INTERVAL 10 MINUTE)");
        $stmt->execute([$rfid_uid]);
        $fails = $stmt->fetch()['c'] + 1;
        $db->prepare("INSERT INTO access_logs (tanggal,waktu,rfid_uid,status,keterangan) VALUES (CURDATE(),CURTIME(),?,'Ditolak','Kartu tidak terdaftar')")->execute([$rfid_uid]);
        if ($fails >= 3) { jsonResponse(['error' => 'Kartu ditolak 3x', 'require_pin' => true, 'fail_count' => $fails], 401); }
        jsonResponse(['error' => 'Akses ditolak', 'fail_count' => $fails, 'max_attempts' => 3], 401);
    }
    $_SESSION['user_id'] = $card['user_id'];
    unset($card['password']);
    jsonResponse(['success' => true, 'user' => $card]);
}

function checkSession() {
    if (isset($_SESSION['user_id'])) {
        $db = getDB();
        $stmt = $db->prepare("SELECT * FROM users WHERE id = ?");
        $stmt->execute([$_SESSION['user_id']]);
        $user = $stmt->fetch();
        if ($user) { unset($user['password']); jsonResponse(['authenticated' => true, 'user' => $user]); }
    }
    jsonResponse(['authenticated' => false], 401);
}
