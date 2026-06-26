<?php
require_once 'config.php';
$method = $_SERVER['REQUEST_METHOD'];

if ($method === 'GET') {
    $db = getDB();
    $filter = $_GET['filter'] ?? 'all';
    $date = $_GET['date'] ?? null;
    $search = $_GET['search'] ?? '';
    $where = "1=1";
    $params = [];
    if ($filter === 'success') { $where .= " AND a.status = 'Berhasil'"; }
    elseif ($filter === 'rejected') { $where .= " AND a.status = 'Ditolak'"; }
    if ($date === 'today') { $where .= " AND a.tanggal = CURDATE()"; }
    elseif ($date === 'yesterday') { $where .= " AND a.tanggal = DATE_SUB(CURDATE(), INTERVAL 1 DAY)"; }
    if (!empty($search)) { $where .= " AND (a.nama_petugas LIKE ? OR a.rfid_label LIKE ? OR a.ruangan LIKE ?)"; $params = array_merge($params, ["%$search%","%$search%","%$search%"]); }
    $stmt = $db->prepare(
        "SELECT a.*, u.avatar_initials, u.avatar_color
         FROM access_logs a
         LEFT JOIN users u ON a.user_id = u.id
         WHERE $where
         ORDER BY a.tanggal DESC, a.waktu DESC
         LIMIT 100"
    );
    $stmt->execute($params);
    $logs = $stmt->fetchAll();
    $total = count($logs);
    $success = count(array_filter($logs, fn($l) => $l['status'] === 'Berhasil'));
    $rejected = $total - $success;
    jsonResponse(['logs' => $logs, 'stats' => ['total' => $total, 'success' => $success, 'rejected' => $rejected, 'success_pct' => $total > 0 ? round($success/$total*100) : 0]]);
}
