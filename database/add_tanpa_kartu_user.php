<?php
require_once __DIR__ . '/../api/config.php';
$db = getDB();

try {
    // 1. Cek apakah user "Tanpa Kartu" sudah ada
    $stmt = $db->prepare("SELECT id FROM users WHERE username = 'tanpa.kartu'");
    $stmt->execute();
    $user = $stmt->fetch();

    if (!$user) {
        $hash = password_hash('tanpakartu123', PASSWORD_DEFAULT);
        $db->prepare(
            "INSERT INTO users (nama_lengkap, username, email, password, jabatan, rfid_uid, rfid_label, avatar_initials, avatar_color, is_active) 
             VALUES ('Tanpa Kartu', 'tanpa.kartu', 'tanpakartu@arsip.go.id', ?, 'Petugas Arsip', NULL, 'Keypad Only', 'TK', '#6C757D', 1)"
        )->execute([$hash]);
        $userId = $db->lastInsertId();
        echo "User 'Tanpa Kartu' berhasil dibuat dengan ID: $userId\n";
    } else {
        $userId = $user['id'];
        echo "User 'Tanpa Kartu' sudah ada dengan ID: $userId\n";
    }

    // 2. Hubungkan semua PIN aktif (atau default '5D015A' jika kosong) dengan user 'Tanpa Kartu' ini
    $pinStmt = $db->prepare("SELECT pin_code FROM pin_access WHERE is_active = 1 LIMIT 1");
    $pinStmt->execute();
    $pin = $pinStmt->fetch();
    $pinCode = $pin ? $pin['pin_code'] : '5D015A';

    // Nonaktifkan PIN lama
    $db->prepare("UPDATE pin_access SET is_active = 0")->execute();

    // Buat PIN aktif baru yang merujuk ke user 'Tanpa Kartu'
    $db->prepare(
        "INSERT INTO pin_access (pin_code, user_id, is_active) VALUES (?, ?, 1)"
    )->execute([$pinCode, $userId]);
    echo "PIN aktif '$pinCode' berhasil diset untuk user 'Tanpa Kartu'\n";

} catch (Exception $e) {
    echo "Error: " . $e->getMessage() . "\n";
}
