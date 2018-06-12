<?php 
if(strcasecmp($_SERVER['REQUEST_METHOD'], 'POST') != 0){
    throw new Exception('Request method must be POST!');
}

$data = json_decode(file_get_contents('php://input'), true);
if (!is_array($data)){
    throw new Exception('Received content contained invalid JSON!');
}

$pdo = new PDO('mysql:host=localhost;dbname=weather_station', 'pi', $data['password']);
$sender_id = $data['sender_id'];
$temperature = $data['temperature'];
$humidity = $data['humidity'];
$sql = "INSERT INTO temperature VALUES (DEFAULT, '$sender_id', NOW(), $temperature, $humidity)";
$stmt = $pdo->prepare($sql);
$stmt->execute();
if ($stmt->errorCode() == 0)
{
	echo $stmt->errorInfo()[1];	
	echo $stmt->errorInfo()[2];	
	die();
}
?>
