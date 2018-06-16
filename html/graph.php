<?php
setlocale(LC_TIME, 'de_DE.utf8');
if (!empty($_GET['db'])) { $db = $_GET['db']; } else { $db = 'weather_station'; }
if (!empty($_GET['sensor'])) { $sensor = $_GET['sensor']; } else { $sensor = 'Ralfs Wetterstation'; }
if (!empty($_GET['offset'])) { $hour_offset = -$_GET['offset'] + 1; } else { $hour_offset = 1; }

$endTime = new DateTime(date_create('NOW')->format('Y-m-d H:00:00'));
$endTime->add(date_interval_create_from_date_string("$hour_offset hour"));
$endHour = intval($endTime->format('H'));
$pdo = new PDO("mysql:host=localhost;dbname=$db", 'pi', 'geheim');
$right = -$hour_offset + 1;
$left = $right + 24;
$sql = "SELECT datum, temp, humidity FROM temperature WHERE sender_id = '$sensor' AND datum >= DATE_SUB(NOW(), INTERVAL $left HOUR) AND datum <= DATE_SUB(NOW(), INTERVAL $right HOUR) ORDER BY datum";
$stmt = $pdo->prepare($sql);
$stmt->execute();
$t_min = 0; $t_max = 0;
if ($stmt->errorCode() == 0)
{
	$data = $stmt->fetchAll();
	foreach( $data as $index => $row)
	{
		if ($t_min > intval(10 * $row['temp'])) {
			$t_min = intval(10 * $row['temp']);
		}
		if ($t_max < intval(10 * $row['temp'])) {
			$t_max = intval(10 * $row['temp']);
		}
	}
	$step_h = 50; $step_v = 60; $data_max = 24*60;
	$t_min = $t_min - $t_min % 50 - (($t_min < 0) ? 50 : 0);
	$t_max = $t_max - $t_max % 50 + (($t_max > 0) ? 50 : 0);
	$t_norm = $t_max - $t_min;
} else {
	echo $stmt->errorInfo()[1];	
	echo $stmt->errorInfo()[2];	
	die();
}

echo 
'<svg xmlns="http://www.w3.org/2000/svg" version="1.1" viewBox="0 0 '.($data_max + 2 * $step_v)." ".($t_norm + 100).'">
	<defs>
		<pattern id="grid" patternUnits="userSpaceOnUse" width="'.($step_v).'" height="50" x="0" y="0" '.
		'fill="NavajoWhite" stroke-width="1" stroke="grey" stroke-dasharray="2,2">
			<desc>Raster</desc>
			<path d="M0,0 v50 h'.$step_v.' v-50 z" />
		</pattern>
	</defs>
	<rect width="'.$data_max.'" height="'.$t_norm.'" fill="url(#grid)" stroke-width="1" stroke="Black" x="'.$step_v.'" y="50" />
';
	$startet = False;
	$last = 0;
	foreach( $data as $index => $row)
	{
		$t = date_create($row['datum']);
		$i = 24 * 60 - intval(($endTime->format("U") - $t->format("U")) / 60);
		if ($i < 0) { continue; }
		if ($startet && ($last + 5 < $i))
		{
			echo '" style="fill:none;stroke:MediumBlue;stroke-width:2" />';
			$startet = False;
		}
		if (!$startet) { echo '	<polyline points="'; $startet = True; }
		echo ($i + $step_v).",".($t_max - intval($row['temp'] * 10) + 50)." ";
		$last = $i;
	}
	if ($startet) { echo '" style="fill:none;stroke:MediumBlue;stroke-width:2" />'; }

	$startet = False;
	$last = 0;
	foreach( $data as $index => $row)
	{
		$t = date_create($row['datum']);
		$i = 24 * 60 - intval(($endTime->format("U") - $t->format("U")) / 60);
		if ($i < 0 || ($row['humidity'] < 0)) { continue; }
		if ($startet && ($last + 5 < $i))
		{
			echo '" style="fill:none;stroke:DarkGreen;stroke-width:2" />';
			$startet = False;
		}
		if (!$startet) { echo '	<polyline points="'; $startet = True; }
		echo ($i + $step_v).",".intval((100 - $row['humidity']) * $t_norm / 100 + 50)." ";
		$last = $i;
	}
	if ($startet) { echo '" style="fill:none;stroke:DarkGreen;stroke-width:2" />'; }
	for($x = 0; $x <= 24; $x++)
	{
		$h = $endHour - (24 - $x);
		if ($h < 0) { $h += 24; }
		echo '
	<text x="'.($step_v * ($x+1) - 5).'" y="40" fill="Black">'.$h.'</text>';
	}
	for ($x = 0; $x <= $t_norm / 50; $x++)
	{
		echo '
	<text x="'.($data_max + $step_v + 5).'" y="'.(55 + $x * 50).'" fill="MediumBlue">'.intval(($t_max - $x * 50)/10).'&deg;C</text>
	<text x="5" y="'.(55 + $x * 50).'" fill="DarkGreen">'.intval(100 - 5000 * $x / $t_norm).'%</text>
';
	}
echo '	
	<text x="'.intval(($data_max) - 120).'" y="'.($t_norm + 75).'">'.strftime("%A %e.%B", $endTime->format("U")).'</text>
	<text x="'.intval(($data_max/2)).'" y="'.($t_norm + 75).'" fill="MediumBlue">24h Temperatur '.($t_min/10).'-'.($t_max/10).'&deg;C</text>
	<text x="100" y="'.($t_norm + 75).'" fill="DarkGreen">24h Luftfeuchtigkeit 0-100%</text>
</svg>
';
?>
