# ESP8266-Wetter-Server
Temperatur- und Luftfeuchtigkeit mit DHT22

Die Projekte wurden auf Grundlage der  Wetterstations-Projekte für den Raspberry gestartet, dann allerdings schnell 
in zwei Richtungen umgebaut:

- wetter_sensor.ino: Eine stromsparende Variante für den Batteriebetrieb, die nur Daten an einen Server überträgt. Der Server kümmert sich 
  dann um die Präsentation. Ziel ist es einen langristigen Baterriebetrieb mit einem esp8266-01 zu implementieren.

- wetter_server.ino: Hier werden die Daten über einen langen Zeitraum (z.B. 24h mit 1min Intervall) erfaßt und auch mit einem einfachen web-Service
  insbesondere auch grafisch aufbereitet. Mit einem port-Forwarding im Router kann man so mit jedem Handy die Kurven des Sensors auslesen, sofern er
  lange genug lief. Momentan gibt es keine Optimierung für stromsparenden Betrieb.

