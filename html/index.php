<html><head><meta http-equiv="Content-Type" content="text/html; charset=UTF-8"><title>Ralfs Wetterstation</title>
<style>
	body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }
	svg { width: 98%; height: auto; }
</style>
</head>
<body>
<h1>Ralfs Wetterstation</h1>
<p>
</p>
<p>Uhrzeit: <?php echo date("l Y-m-d G:i:s (e)") ?></p>
<embed src="http://sensor1/graph.svg" type="image/svg+xml">
<h1>Wohnzimmer</h1>
<embed src="http://sensor2/graph.svg" type="image/svg+xml">
<h1>Testsensor</h1>
<?php
$pdo = new PDO('mysql:host=localhost;dbname=weather_station', 'pi', 'geheim');
$sql = "SELECT datum, temp, humidity FROM temperature WHERE sender_id = 'Ralfs Testsensor' ORDER BY datum DESC LIMIT 1";
	foreach ($pdo->query($sql) as $row) {
		echo "Letzter Eintrag: ".$row['datum']." Temperatur ".$row['temp']."&deg;C Luftfeuchtigkeit ".$row['humidity'];
	}
?>
<embed src="graph.php" type="image/svg+xml" width="98%" height="100%">
</body>
</html>
