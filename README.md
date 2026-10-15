# Tamagotchi casero (ESP32-C3 + TFT 1.8" ST7735)

Mascota virtual (gato o perro) que nace cachorro, crece, envejece y muere en unas 2-3 semanas. Cuanto mejor la cuides, más vive.

## Material

| Cantidad | Componente |
|---|---|
| 1 | ESP32-C3 (DevKit o SuperMini) |
| 1 | Pantalla TFT 1.8" SPI 128x160 (ST7735, v1.1) |
| 2 | Pulsadores |
| 1 | Protoboard + cables dupont |
| 1 | Cable USB (alimentación y programación) |

## Conexiones

### Pantalla → ESP32-C3

| Pin pantalla | ESP32-C3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| CS | GPIO 7 |
| RESET (RST) | GPIO 10 |
| A0 (DC) | GPIO 5 |
| SDA (MOSI) | GPIO 6 |
| SCK | GPIO 4 |
| LED (retroiluminación) | 3V3 (directo, o con resistencia 100 Ω) |

> En estas pantallas **SDA = MOSI** y **A0 = DC**. Alimentar siempre a **3,3 V**, no a 5 V.

### Botones → ESP32-C3

Cada botón: un pie a su GPIO y el otro pie a **GND**. No hacen falta resistencias (se usa el pull-up interno).

| Botón | GPIO |
|---|---|
| Botón 1 · Comida | GPIO 0 |
| Botón 2 · Jugar | GPIO 1 |

Evita GPIO 8 y 9 (los usa el arranque de la placa).

### Buzzer → ESP32-C3 SuperMini

Usa un buzzer **activo** (el que suena solo con darle tensión, 3,3 V).

| Buzzer | SuperMini |
|---|---|
| + | GPIO 3 |
| − | GND |

Sonidos: 3 pitidos cortos cada 45 s mientras haya un caso extremo (comida o diversión <10 o >90) y un pitido largo al morir. Pon `SOUND_ON = false` para silenciarlo.

## Instalar y programar (Arduino IDE)

1. Instala el soporte ESP32: *Archivo → Preferencias → URLs adicionales* →
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`, y luego *Gestor de tarjetas* → "esp32 by Espressif".
2. *Gestor de librerías*: instala **Adafruit GFX Library** y **Adafruit ST7735 and ST7789 Library**.
3. Placa: **ESP32C3 Dev Module**. Activa **USB CDC On Boot: Enabled** (para ver el Serial).
4. Abre `tamagotchi.ino` y súbelo.

Si la pantalla muestra colores raros o una franja de ruido: en `setup()` cambia `INITR_BLACKTAB` por `INITR_GREENTAB` (o `INITR_REDTAB`); si los colores salen invertidos añade `tft.invertDisplay(true);`.

## Cómo se juega

- **Primer arranque:** Botón 1 = gato, Botón 2 = perro.
- **Botón 1:** pone una ración en el plato (la mascota se la come sola).
- **Botón 2:** jugar.
- Entre pulsaciones del mismo botón hay **1 minuto** de espera (sale "Espera...").
- **Mantener los dos botones >1 s:** muestra los valores internos (para el monitor / depurar).
- **Mantener los dos botones 5 s:** borra la mascota y empieza otra.

## Reglas del juego

| Variable | Rango | Qué pasa |
|---|---|---|
| Comida | 0 (desnutrido) – 100 (sobrealimentado) | Ideal ≈ 4 raciones/día (valor 50). Se calcula con la media de las últimas ~24 h. Fuera de 35–65 baja el trato. **≤5 durante 1 h = muere de hambre. 100 = muere de empacho.** |
| Diversión | 0 – 100 | Baja 3 puntos/hora. Cada partida suma `5 + 2·racha` (la racha sube al jugar seguido y se pierde con el tiempo). **0 durante 1 h = muere de pena. 100 = muere de sobreexcitación.** Ideal 40–70. |
| Trato (oculto) | 0 – 100 | Se acerca despacio (≈2 días) a la media de lo bien que está comida y diversión. |
| Edad | 0 – 17 días base | Con trato 100 envejece a ×0,6 (≈28 días de vida); con trato 0 a ×1,4 (≈12 días). Con trato medio ≈ 17 días. |

Etapas (en días base): cachorro 0–3 · joven 3–8 · adulto 8–14 · mayor 14–17 → muere de vejez.

## Probar rápido

En el código, `TIME_SCALE = 1.0f`. Ponlo a `3600.0f` (1 hora por segundo) para ver todo el ciclo de vida en unos minutos. Vuelve a 1.0 para jugar de verdad. El Serial (115200) imprime los valores cada 10 s.

## Limitaciones conocidas

- Sin batería de reloj (RTC): **el tiempo con el aparato apagado no cuenta**; la mascota "se congela". El estado se guarda cada 5 min en la flash.
- Para llevarlo encima añade una batería LiPo + módulo de carga TP4056.

## Ideas para ampliar

Zumbador pasivo con sonidos, tercer botón (limpiar / medicina), caca, sueño nocturno con LDR, mensaje en la pantalla según el estado.
