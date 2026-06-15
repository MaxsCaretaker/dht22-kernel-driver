# DHT22 Kernel Driver for Raspberry Pi 4

A Linux kernel module that reads temperature and humidity from a DHT22 sensor via GPIO4, exposing the data through `/dev/dht22`.

## Tested on
- Raspberry Pi 4
- Kernel 6.12.75+rpt-rpi-v8 (64-bit)
- Debian GNU/Linux 13 (Trixie)

## Wiring
| DHT22 | Pi 4 |
|-------|------|
| VCC   | Pin 1 (3.3V) |
| DATA  | Pin 7 (GPIO4) + 4.7kΩ to 3.3V |
| GND   | Pin 9 (GND) |

## Build & load
```bash
cd driver
make
make load
```

## Read sensor
```bash
sudo cat /dev/dht22
sudo python3 Dht22_read.py
sudo python3 Dht22_read.py --loop 5 --csv readings.csv
```

## How it works
The DHT22 encodes data as pulse widths on a single wire. A ~26µs HIGH pulse is a 0 bit, a ~70µs HIGH pulse is a 1 bit. The kernel driver measures these with interrupts disabled for reliable microsecond timing, then exposes the result through /dev/dht22 as a character device.
EOF
