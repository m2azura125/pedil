-- =====================================================
-- PEDIL - Sistem Manajemen Lemari Arsip
-- Database Schema v2.4
-- =====================================================

-- Tabel pengguna/petugas
CREATE TABLE IF NOT EXISTS users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    nama_lengkap VARCHAR(100) NOT NULL,
    username VARCHAR(50) UNIQUE NOT NULL,
    email VARCHAR(100),
    password VARCHAR(255) NOT NULL,
    jabatan ENUM('Administrator', 'Petugas Arsip', 'Staf Umum') DEFAULT 'Petugas Arsip',
    rfid_uid VARCHAR(20) UNIQUE,
    rfid_label VARCHAR(20),
    avatar_initials VARCHAR(5),
    avatar_color VARCHAR(7) DEFAULT '#2D6A4F',
    is_active TINYINT(1) DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

-- Tabel ruangan/lemari
CREATE TABLE IF NOT EXISTS rooms (
    id INT AUTO_INCREMENT PRIMARY KEY,
    kode_ruangan VARCHAR(10) UNIQUE NOT NULL,
    nama_ruangan VARCHAR(100) NOT NULL,
    deskripsi VARCHAR(255),
    lantai VARCHAR(10),
    total_slot INT DEFAULT 50,
    slot_terisi INT DEFAULT 0,
    status ENUM('Normal', 'Hampir Penuh', 'Penuh', 'Maintenance') DEFAULT 'Normal',
    is_active TINYINT(1) DEFAULT 1,
    last_access TIMESTAMP NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Tabel kartu RFID terdaftar
CREATE TABLE IF NOT EXISTS rfid_cards (
    id INT AUTO_INCREMENT PRIMARY KEY,
    uid VARCHAR(20) UNIQUE NOT NULL,
    label VARCHAR(20),
    user_id INT,
    is_active TINYINT(1) DEFAULT 1,
    registered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL
);

-- Tabel PIN akses
CREATE TABLE IF NOT EXISTS pin_access (
    id INT AUTO_INCREMENT PRIMARY KEY,
    pin_code VARCHAR(10) NOT NULL,
    user_id INT,
    is_active TINYINT(1) DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL
);

-- Tabel log akses
CREATE TABLE IF NOT EXISTS access_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    tanggal DATE NOT NULL,
    waktu TIME NOT NULL,
    rfid_uid VARCHAR(20),
    rfid_label VARCHAR(20),
    user_id INT,
    nama_petugas VARCHAR(100),
    jabatan VARCHAR(50),
    room_id INT,
    ruangan VARCHAR(100),
    aksi ENUM('Buka', 'Tutup') DEFAULT 'Buka',
    metode ENUM('RFID', 'PIN', 'Manual') DEFAULT 'RFID',
    durasi_menit INT DEFAULT 0,
    status ENUM('Berhasil', 'Ditolak') DEFAULT 'Berhasil',
    keterangan TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL,
    FOREIGN KEY (room_id) REFERENCES rooms(id) ON DELETE SET NULL
);

-- Tabel sensor data (ultrasonic, vibration, door)
CREATE TABLE IF NOT EXISTS sensor_data (
    id INT AUTO_INCREMENT PRIMARY KEY,
    room_id INT,
    sensor_type ENUM('ultrasonic', 'vibration', 'door_switch') NOT NULL,
    sensor_index TINYINT DEFAULT 0,
    value FLOAT,
    unit VARCHAR(10) DEFAULT 'cm',
    status VARCHAR(50),
    recorded_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (room_id) REFERENCES rooms(id) ON DELETE CASCADE
);

-- Tabel alert/alarm
CREATE TABLE IF NOT EXISTS alerts (
    id INT AUTO_INCREMENT PRIMARY KEY,
    room_id INT,
    alert_type ENUM('vibration_danger', 'vibration_normal', 'door_forced', 'lockout', 'capacity_full', 'door_ajar') NOT NULL,
    severity ENUM('low', 'medium', 'high', 'critical') DEFAULT 'medium',
    message TEXT,
    is_resolved TINYINT(1) DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    resolved_at TIMESTAMP NULL,
    FOREIGN KEY (room_id) REFERENCES rooms(id) ON DELETE CASCADE
);

-- Tabel lockout
CREATE TABLE IF NOT EXISTS lockouts (
    id INT AUTO_INCREMENT PRIMARY KEY,
    rfid_uid VARCHAR(20),
    ip_address VARCHAR(45),
    lockout_until TIMESTAMP NOT NULL,
    reason VARCHAR(255),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Tabel konfigurasi ESP32 nodes
CREATE TABLE IF NOT EXISTS esp32_nodes (
    id INT AUTO_INCREMENT PRIMARY KEY,
    node_id VARCHAR(20) UNIQUE NOT NULL,
    ip_address VARCHAR(45),
    room_id INT,
    firmware_version VARCHAR(20),
    last_heartbeat TIMESTAMP NULL,
    is_online TINYINT(1) DEFAULT 0,
    config_json JSON,           -- menyimpan sys_state, relay_open, nano_ok dari ESP32
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (room_id) REFERENCES rooms(id) ON DELETE SET NULL
);

-- Queue perintah web → ESP32 (force_lock, force_unlock, dll)
CREATE TABLE IF NOT EXISTS esp32_commands (
    id INT AUTO_INCREMENT PRIMARY KEY,
    node_id VARCHAR(20) NOT NULL,
    command ENUM('force_lock','force_unlock','reboot','noop') DEFAULT 'noop',
    params JSON,
    is_executed TINYINT(1) DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    executed_at TIMESTAMP NULL
);

-- =====================================================
-- DATA AWAL (SEED)
-- =====================================================

-- Admin default (password: admin123)
INSERT INTO users (nama_lengkap, username, email, password, jabatan, rfid_uid, rfid_label, avatar_initials, avatar_color) VALUES
('Adm. Rahmadani', 'rahmadani.adm', 'rahmadani@arsip.go.id', '$2y$10$92IXUNpkjO0rOQ5byMi.Ye4oKoEa3Ro9llC/.og/at2.uheWG/igi', 'Administrator', 'A1B2C3D4', 'RFID-001', 'AR', '#2D6A4F'),
('Siti Aminah', 'siti.aminah', 'siti@arsip.go.id', '$2y$10$92IXUNpkjO0rOQ5byMi.Ye4oKoEa3Ro9llC/.og/at2.uheWG/igi', 'Petugas Arsip', 'E5F6G7H8', 'RFID-002', 'SA', '#1B4332'),
('Budi Santoso', 'budi.santoso', 'budi@arsip.go.id', '$2y$10$92IXUNpkjO0rOQ5byMi.Ye4oKoEa3Ro9llC/.og/at2.uheWG/igi', 'Petugas Arsip', 'I9J0K1L2', 'RFID-003', 'BS', '#D4A017'),
('Hendra Wijaya', 'hendra.wijaya', 'hendra@arsip.go.id', '$2y$10$92IXUNpkjO0rOQ5byMi.Ye4oKoEa3Ro9llC/.og/at2.uheWG/igi', 'Staf Umum', 'M3N4O5P6', 'RFID-004', 'HW', '#7B2D8B');

-- Ruangan
INSERT INTO rooms (kode_ruangan, nama_ruangan, deskripsi, lantai, total_slot, slot_terisi, status) VALUES
('R-01', 'Ruangan 1', 'Arsip Aktif', 'Lt.2', 50, 34, 'Normal'),
('R-07', 'Ruangan 2', 'Arsip Rahasia', 'Lt.2', 8, 4, 'Normal'),
('R-23', 'Ruangan 3', 'Arsip Inaktif', 'Lt.1', 94, 85, 'Hampir Penuh'),
('R-04', 'Ruangan 4', 'Arsip Vital', 'Lt.3', 100, 30, 'Normal');

-- Kartu RFID
INSERT INTO rfid_cards (uid, label, user_id, is_active) VALUES
('A1B2C3D4', 'RFID-001', 1, 1),
('E5F6G7H8', 'RFID-002', 2, 1),
('I9J0K1L2', 'RFID-003', 3, 1),
('M3N4O5P6', 'RFID-004', 4, 1);

-- PIN Akses
INSERT INTO pin_access (pin_code, user_id, is_active) VALUES
('5D015A', 1, 1);

-- Contoh log akses
INSERT INTO access_logs (tanggal, waktu, rfid_uid, rfid_label, user_id, nama_petugas, jabatan, room_id, ruangan, aksi, metode, durasi_menit, status) VALUES
('2026-06-04', '10:45:00', 'A1B2C3D4', 'RFID-001', 1, 'Adm. Rahmadani', 'Administrator', 1, 'Ruangan 1', 'Buka', 'RFID', 12, 'Berhasil'),
('2026-06-04', '10:02:00', 'I9J0K1L2', 'RFID-003', 3, 'Budi Santoso', 'Petugas Arsip', 2, 'Ruangan 2', 'Buka', 'RFID', 8, 'Berhasil'),
('2026-06-04', '09:12:00', 'I9J0K1L2', 'RFID-003', 3, 'Budi Santoso', 'Petugas Arsip', 1, 'Ruangan 1', 'Tutup', 'RFID', 0, 'Berhasil'),
('2026-06-04', '08:30:00', 'A1B2C3D4', 'RFID-001', 1, 'Adm. Rahmadani', 'Administrator', 3, 'Ruangan 3', 'Buka', 'RFID', 25, 'Berhasil'),
('2026-06-04', '07:55:00', 'UNKNOWN1', 'RFID-005', NULL, NULL, NULL, 2, 'Ruangan 2', 'Buka', 'RFID', 0, 'Ditolak'),
('2026-06-03', '16:30:00', 'A1B2C3D4', 'RFID-001', 1, 'Adm. Rahmadani', 'Administrator', 1, 'Ruangan 1', 'Buka', 'RFID', 18, 'Berhasil'),
('2026-06-03', '14:20:00', 'E5F6G7H8', 'RFID-002', 2, 'Siti Aminah', 'Petugas Arsip', 2, 'Ruangan 2', 'Buka', 'RFID', 6, 'Berhasil'),
('2026-06-03', '13:10:00', 'M3N4O5P6', 'RFID-004', 4, 'Hendra Wijaya', 'Staf Umum', 4, 'Ruangan 4', 'Buka', 'RFID', 30, 'Berhasil'),
('2026-06-03', '11:00:00', 'I9J0K1L2', 'RFID-003', 3, 'Budi Santoso', 'Petugas Arsip', 1, 'Ruangan 1', 'Buka', 'RFID', 15, 'Berhasil'),
('2026-06-03', '09:45:00', 'UNKNOWN2', 'RFID-006', NULL, NULL, NULL, 3, 'Ruangan 3', 'Buka', 'RFID', 0, 'Ditolak'),
('2026-06-02', '15:20:00', 'A1B2C3D4', 'RFID-001', 1, 'Adm. Rahmadani', 'Administrator', 2, 'Ruangan 2', 'Buka', 'RFID', 22, 'Berhasil'),
('2026-06-02', '10:00:00', 'E5F6G7H8', 'RFID-002', 2, 'Siti Aminah', 'Petugas Arsip', 1, 'Ruangan 1', 'Buka', 'RFID', 10, 'Berhasil'),
('2026-06-02', '08:15:00', 'UNKNOWN1', 'RFID-005', NULL, NULL, NULL, 1, 'Ruangan 1', 'Buka', 'RFID', 0, 'Ditolak');

-- ESP32 Node
INSERT INTO esp32_nodes (node_id, ip_address, room_id, firmware_version, is_online) VALUES
('NODE-A', '192.168.1.10', 1, 'v2.4', 1);
