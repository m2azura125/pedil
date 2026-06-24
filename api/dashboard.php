<?php
require_once 'config.php';
$db = getDB();

// Dashboard statistics
$today = date('Y-m-d');

// Okupansi
$rooms = $db->query("SELECT * FROM rooms WHERE is_active = 1")->fetchAll();
$total_slots = array_sum(array_column($rooms, 'total_slot'));
$total_filled = array_sum(array_column($rooms, 'slot_terisi'));
$occupancy = $total_slots > 0 ? round($total_filled / $total_slots * 100) : 0;

// Pintu terbuka (simulasi)
$open_doors = $db->query("SELECT COUNT(DISTINCT room_id) as c FROM sensor_data WHERE sensor_type = 'door_switch' AND status = 'open' AND recorded_at > DATE_SUB(NOW(), INTERVAL 5 MINUTE)")->fetch()['c'];

// Akses hari ini
$today_access = $db->prepare("SELECT COUNT(*) as c FROM access_logs WHERE tanggal = ?");
$today_access->execute([$today]);
$access_today = $today_access->fetch()['c'];

// Akses user hari ini
$today_users = $db->prepare("SELECT COUNT(DISTINCT user_id) as c FROM access_logs WHERE tanggal = ? AND user_id IS NOT NULL");
$today_users->execute([$today]);

// Ditolak 7 hari
$rejected_7d = $db->query("SELECT COUNT(*) as c FROM access_logs WHERE status = 'Ditolak' AND tanggal >= DATE_SUB(CURDATE(), INTERVAL 7 DAY)")->fetch()['c'];

// Akses per pengguna (30 hari)
$user_access = $db->query("
    SELECT u.nama_lengkap, u.avatar_initials, u.avatar_color, rc.label as rfid_label,
           SUM(CASE WHEN a.status='Berhasil' THEN 1 ELSE 0 END) as berhasil,
           SUM(CASE WHEN a.status='Ditolak' THEN 1 ELSE 0 END) as ditolak,
           COUNT(*) as total
    FROM access_logs a
    LEFT JOIN users u ON a.user_id = u.id
    LEFT JOIN rfid_cards rc ON a.rfid_uid = rc.uid
    WHERE a.tanggal >= DATE_SUB(CURDATE(), INTERVAL 30 DAY)
    GROUP BY COALESCE(a.user_id, a.rfid_uid)
    ORDER BY total DESC LIMIT 6
")->fetchAll();

// Tren harian (9 hari)
$daily_trend = $db->query("
    SELECT tanggal, COUNT(*) as total FROM access_logs 
    WHERE tanggal >= DATE_SUB(CURDATE(), INTERVAL 9 DAY)
    GROUP BY tanggal ORDER BY tanggal
")->fetchAll();

// Average
$avg = count($daily_trend) > 0 ? round(array_sum(array_column($daily_trend, 'total')) / count($daily_trend), 1) : 0;

jsonResponse([
    'occupancy' => ['pct' => $occupancy, 'filled' => $total_filled, 'total' => $total_slots],
    'open_doors' => $open_doors,
    'access_today' => ['count' => $access_today, 'users' => $today_users->fetch()['c']],
    'rejected_7d' => $rejected_7d,
    'user_access' => $user_access,
    'daily_trend' => $daily_trend,
    'daily_avg' => $avg,
    'rooms' => $rooms
]);
