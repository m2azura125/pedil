<?php
error_reporting(E_ALL);
ini_set('display_errors', 1);
header('Content-Type: text/plain');

echo "PHP Version: " . phpversion() . "\n";

try {
    echo "Attempting to include config.php...\n";
    require_once 'config.php';
    echo "config.php included successfully!\n";
    
    echo "Attempting to connect to database...\n";
    $db = getDB();
    echo "Database connected successfully!\n";
} catch (Throwable $e) {
    echo "Error caught: " . $e->getMessage() . "\n";
    echo "File: " . $e->getFile() . " on line " . $e->getLine() . "\n";
    echo "Stack trace:\n" . $e->getTraceAsString() . "\n";
}
