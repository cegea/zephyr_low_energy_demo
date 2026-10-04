# Demo de energia con Zephyr

Demo para la `NUCLEO-WL55JC1` y un fotoresistor `KY-018`. Lee la luz por el
ADC, muestra el resultado por el puerto serie y permite cambiar la politica de
energia con los tres botones de la placa.

No utiliza BLE, LoRa, radio ni LED. En todos los modos se realizan las mismas
ocho conversiones ADC cada 1000 ms. Lo unico que cambia es la restriccion que
la aplicacion entrega a la politica de energia de Zephyr.

## Cableado

El KY-018 contiene una LDR y una resistencia `103` de 10 kohm. Su salida `S`
es analogica y no necesita componentes externos adicionales.

<img src="docs/images/KY-018.svg" alt="Modulo KY-018" width="200">


| KY-018 | NUCLEO-WL55JC1 |
|---|---|
| `-` | `GND` |
| `+` | `3V3` |
| `S` | `A0`, que corresponde a PB1/ADC1_IN5 |

Con la LDR hacia arriba, como en la imagen, los pines son `S`, `+` y `-` de
izquierda a derecha. Como existen copias con serigrafia incorrecta, conviene
comprobarlo antes con un multimetro. Alimente siempre el modulo con 3.3 V y
verifique que `S` permanece entre 0 y 3.3 V. No use 5 V.

## Modos

El firmware arranca en ahorro y cambia de modo sin reiniciar ni volver a
flashear:

| Boton | Modo | Comportamiento |
|---|---|---|
| `B1/SW1` | referencia | bloquea los estados de energia gestionados por PM |
| `B2/SW2` | respuesta rapida | solicita una latencia maxima de salida de 5 us |
| `B3/SW3` | ahorro | deja la seleccion de estado a la politica de Zephyr |

Al cambiar de modo se retira primero la restriccion anterior. Pulsar de nuevo
el boton del modo actual no acumula restricciones.

Los valores de latencia de STOP0, STOP1 y STOP2 incluidos en el overlay son
metadata experimental para esta comparacion. No representan una medicion de
latencia extremo a extremo.

## Compilar y flashear

Entorno utilizado:

- Zephyr `4.4.2`.
- Board `nucleo_wl55jc`.
- Zephyr SDK `1.0.1`.

Desde PowerShell, en la raiz del proyecto:

```powershell
.\scripts\build.ps1
.\scripts\flash.ps1
```

Tambien estan disponibles las tareas `Demo: Build`, `Demo: Flash` y
`Serial Monitor` en VS Code.

El monitor serie utiliza `115200 8N1`. Una ejecucion normal se ve asi:

```text
Modo: ahorro
B1: referencia; B2: respuesta rapida; B3: ahorro
Intervalo: 1000 ms; ADC: A0/PB1
Muestra 1: 602
```

Al iluminar y tapar la LDR, los valores deben cambiar. Cada pulsacion imprime
el nombre del nuevo modo y las muestras continúan apareciendo.

## Medicion de corriente

El puente `JP1`, rotulado `I_SoC`, permite medir en serie la corriente del
STM32WL. Esa medida no incluye toda la placa, el ST-LINK ni el KY-018.

Con la placa sin alimentacion:

1. Retire el jumper JP1.
2. Configure el multimetro para corriente continua.
3. Conecte el amperimetro entre los dos pines de JP1.
4. Vuelva a alimentar la placa.

El amperimetro sustituye al jumper y debe permanecer conectado. Nunca lo
conecte directamente entre 3V3 y GND.

Para comparar modos, pulse y suelte el boton correspondiente y espere unos
segundos antes de leer el multimetro. La consola permanece activa en todos los
modos, por lo que los resultados son comparaciones relativas y no el consumo
minimo absoluto del microcontrolador.

Un multimetro convencional muestra principalmente un promedio y puede no
capturar los pulsos breves de actividad del ADC y la UART.

## Archivos principales

- `src/main.c`: lectura ADC, botones y restricciones de energia.
- `prj.conf`: configuracion de Zephyr.
- `boards/nucleo_wl55jc.overlay`: canal ADC y latencias de los estados PM.
- `scripts/build.ps1`: compilacion.
- `scripts/flash.ps1`: flasheo.
