<html><head><meta http-equiv="Content-Type" content="text/html; charset=UTF-8"><title>Ralfs Wetterstation</title>
<style>
	body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }
	svg { width: 98%; height: auto; }
</style>
</head>
<body>
<?php 
setlocale(LC_TIME, 'de_DE.utf8');
echo "<p>Aktuelle Uhrzeit".strftime('%A %e.%B %T %G (%Z)')."</p>";
$pdo = new PDO('mysql:host=localhost;dbname=weather_station', 'pi', 'geheim');

echo '<h1>Ralfs Wetterstation</h1>';
$sql = "SELECT datum, temp, humidity FROM temperature WHERE sender_id = 'Ralfs Wetterstation' ORDER BY datum DESC LIMIT 1";
	foreach ($pdo->query($sql) as $row) {
		echo "<p>Letzte Messung ".$row['datum']." Temperatur ".$row['temp']."&deg;C Luftfeuchtigkeit ".$row['humidity']."</p>";
	}
echo '
</p>
<embed src="graph.php?sensor=Ralfs%20Wetterstation" type="image/svg+xml" width="98%" height="500">


<h1>Wohnzimmer</h1>';
$sql = "SELECT datum, temp, humidity FROM temperature WHERE sender_id = 'Ralfs Wohnzimmer' ORDER BY datum DESC LIMIT 1";
	foreach ($pdo->query($sql) as $row) {
		echo "<p>Letzte Messung ".$row['datum']." Temperatur ".$row['temp']."&deg;C Luftfeuchtigkeit ".$row['humidity']."</p>";
	}
echo '
<embed src="graph.php?sensor=Ralfs%20Wohnzimmer" type="image/svg+xml" width="98%" height="500">

<h1>Testsensor</h1>';
$sql = "SELECT datum, temp, humidity FROM temperature WHERE sender_id = 'Ralfs Testsensor' ORDER BY datum DESC LIMIT 1";
	foreach ($pdo->query($sql) as $row) {
		echo "Letzte Messung: ".$row['datum']." Temperatur ".$row['temp']."&deg;C Luftfeuchtigkeit ".$row['humidity'];
	}
?>
<embed src="graph.php?sensor=Ralfs%20Testsensor" type="image/svg+xml" width="98%" height="500">
</body>
</html>
