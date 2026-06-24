<?php
require_once 'config.php';
$db = getDB();
if ($_SERVER['REQUEST_METHOD'] === 'GET') {
    $rooms = $db->query("SELECT * FROM rooms WHERE is_active = 1 ORDER BY id")->fetchAll();
    $total_slots = array_sum(array_column($rooms, 'total_slot'));
    $total_filled = array_sum(array_column($rooms, 'slot_terisi'));
    $occupancy = $total_slots > 0 ? round($total_filled / $total_slots * 100) : 0;
    jsonResponse(['rooms' => $rooms, 'summary' => ['total_slots' => $total_slots, 'total_filled' => $total_filled, 'occupancy_pct' => $occupancy]]);
}
