# cpp-netdev-watch — Linux network interface monitor

Monitor de interfaces de red de Linux en **C++23** puro (solo biblioteca estándar). Lee `/proc/net/dev` y reporta los contadores RX/TX por interfaz — bytes, paquetes, errores y drops — con totales formateados (`34.6M`, `648.0M`), umbrales de color y modo watch con tasas de transferencia en vivo.

## Características

- **Contadores por interfaz**: `/proc/net/dev` con los 16 campos RX/TX (bytes, paquetes, errores, drops, fifo, frame, compressed, multicast)
- **Formato humano**: totales legibles (`34.6M`, `648.0M`, `88.2K`) y separador de miles para paquetes (`1,515,100`)
- **Umbrales de color**: verde = interfaz limpia, amarillo = errores o drops presentes
- **Modo watch**: `--watch N` refresca cada N segundos y calcula tasas en vivo (RX rate, TX rate, pkt/s) como delta entre muestras
- **Modo JSON**: `--json` emite la estructura completa en JSON válido para scripting
- **Contexto del sistema**: load average (`/proc/loadavg`) y uptime (`/proc/uptime`)
- **Self-test**: `--selftest` con 35 casos (parser de líneas, formatos, tasas, lectura real de `/proc/net/dev`)
- **Parsing estricto**: líneas malformadas o truncadas se rechazan (sin silenciar errores)
- **Cero dependencias**: compilación directa con `g++` sin CMake, sin librerías externas

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o netdev-watch
```

Requiere GCC 14+ (usa `std::println`, `std::from_chars`). Solo biblioteca estándar.

## Uso

```bash
# Una lectura
./netdev-watch

# Modo watch: refrescar cada 2 segundos con tasas en vivo (Ctrl+C para salir)
./netdev-watch --watch 2

# Salida JSON para scripting
./netdev-watch --json

# Sin colores (pipes, scripts, logs)
./netdev-watch --no-color

# Self-tests (35 casos)
./netdev-watch --selftest

# Ayuda
./netdev-watch --help
```

## Ejemplo

```
$ ./netdev-watch
netdev-watch — Linux network interface monitor (C++23)

  INTERFACE        RX    RX pkts  errs  drop        TX    TX pkts  errs  drop
  lo             34.6M      53,188      0      0      34.6M      53,188      0      0
  eth0          386.3M   1,515,100      0      0     654.8M     753,656      0      0

  load   0.64  0.30  0.12     source /proc/net/dev
  uptime 21d 08:32:35
```

## Formato de /proc/net/dev

Cada línea de datos tiene el nombre de la interfaz (terminado en `:`) seguido de 16 contadores: 8 de recepción (bytes, paquetes, errores, drops, fifo, frame, compressed, multicast) y 8 de transmisión (bytes, paquetes, errores, drops, colls, carrier, compressed). Las líneas de cabecera (con `|`) se ignoran.

## Exit codes

- `0` — ok (lectura correcta, selftest pasado)
- `2` — no se encontraron interfaces en `/proc/net/dev` o argumentos inválidos

## Estructura

```
src/main.cpp          # todo el programa (parser + render + selftest)
```

## Verificación

```bash
bash /opt/data/skills/software-development/cpp-modern-dev/scripts/verify-cpp-cli.sh
# (BINARY=./netdev-watch SLUG=netdev-watch) → 9 checks, 0 fallos
```
