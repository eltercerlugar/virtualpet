// =====================================================================
//  TAMAGOTCHI casero - ESP32-C3 + pantalla TFT 1.8" ST7735 (128x160)
//  Librerias (Gestor de librerias de Arduino IDE):
//    - Adafruit GFX Library
//    - Adafruit ST7735 and ST7789 Library
//  Placa: "ESP32C3 Dev Module"  (activar "USB CDC On Boot" para ver Serial)
// =====================================================================
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ------------------------- PINES -------------------------------------
#define PIN_SCK       4   // pantalla: SCK
#define PIN_MOSI      6   // pantalla: SDA / MOSI
#define PIN_CS        7   // pantalla: CS
#define PIN_DC        5   // pantalla: A0 / DC
#define PIN_RST      10   // pantalla: RESET
#define PIN_BTN_FEED  0   // boton 1: comida   (al otro lado a GND)
#define PIN_BTN_PLAY  1   // boton 2: jugar    (al otro lado a GND)
#define PIN_BUZZER    3   // buzzer ACTIVO: + a GPIO3, - a GND
const bool SOUND_ON = true;   // false = silencio total

// ------------------------- AJUSTES DEL JUEGO --------------------------
// 1 = tiempo real. 3600 = una hora por segundo (para probar rapido).
const float    TIME_SCALE          = 1.0f;
const float    LIFE_DAYS           = 17.0f;   // vida "base" con cuidado medio
const float    STAGE_END_DAYS[3]   = {3, 8, 14};  // fin de cachorro, joven, adulto
const float    FOOD_NEED_PER_DAY   = 4.0f;    // raciones/dia ideales
const float    FOOD_MAX_PER_DAY    = 8.0f;    // raciones/dia = 100 (sobrealimentado)
const float    FUN_DECAY_PER_HOUR  = 3.0f;    // puntos de diversion que baja por hora
const float    STREAK_DECAY_MIN    = 10.0f;   // minutos para perder 1 de racha de juego
const float    CARE_TAU_DAYS       = 2.0f;    // lo rapido que cambia el "trato" oculto
const float    ZERO_GRACE_SEC      = 3600.0f; // tiempo con diversion=0 antes de morir
const uint32_t COOLDOWN_MS         = 60000;   // espera entre pulsaciones
const int      BOWL_MAX            = 3;
const uint32_t RESET_HOLD_MS       = 10000;   // tiempo con los 2 botones para reiniciar
const float    STARVE_LEVEL        = 5.0f;    // nutricion (0-100) por debajo = desnutricion extrema
const uint32_t ALERT_EVERY_MS      = 45000;   // pitidos de aviso en casos extremos

// ------------------------- WIFI Y HORA --------------------------------
// >>> CAMBIA ESTOS DOS VALORES POR LOS DE TU WIFI (red de 2,4 GHz) <<<
const char* WIFI_SSID = "TU_WIFI";
const char* WIFI_PASS = "TU_CONTRASENA";
// true = detecta tu zona horaria por internet; false = usa TZ_FALLBACK siempre
const bool  AUTO_TIMEZONE = true;
const char* TZ_FALLBACK   = "CET-1CEST,M3.5.0,M10.5.0/3";   // Espana peninsular
// Horario de sueno (hora local, 24 h): duerme de 22:00 a 06:59
const uint8_t SLEEP_START_H = 22;
const uint8_t SLEEP_END_H   = 7;
const float   REST_DECAY_PER_HOUR = 5.0f;    // descanso que pierde despierta
const float   REST_GAIN_PER_HOUR  = 12.0f;   // descanso que gana durmiendo

// ------------------------- OBJETOS -----------------------------------
Adafruit_ST7735 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16     canvas(128, 160);   // dibujamos aqui y lo volcamos de golpe (sin parpadeo)
Preferences     prefs;

#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

enum Species : uint8_t { CAT = 0, DOG = 1, NONE = 255 };
enum Mood : uint8_t { M_NEUTRAL, M_HAPPY, M_EATING, M_PLAYING, M_HUNGRY,
                      M_STUFFED, M_SAD, M_HYPER, M_SLEEPING, M_DEAD };

struct Pet {
  uint8_t species;
  bool    alive;
  float   ageDays;   // edad en "dias base"
  float   foodAcc;   // raciones/dia (media movil)  -> 0..FOOD_MAX
  float   fun;       // 0..100
  float   care;      // 0..100  VALOR OCULTO: como le trata el dueno
  float   streak;    // racha de juego (0..10)
  float   zeroSec;   // segundos seguidos con diversion 0
  float   starveSec; // segundos seguidos desnutrido
  uint8_t cause;     // 0 vejez, 1 tristeza, 2 sobreexcitacion, 3 hambre, 4 sobrealimentacion
  float   rest;      // 0..100 descanso (solo si hay hora)
} pet;

// (estos tipos van arriba del todo: el IDE de Arduino genera prototipos
//  de las funciones y necesita conocerlos antes)
struct Palette { uint16_t body, belly, dark, ear; };
struct Btn { uint8_t pin; bool down; uint32_t changed; };

int      bowl        = 0;      // raciones en el plato
uint32_t eatingUntil = 0;
uint32_t happyUntil  = 0;
uint32_t playUntil   = 0;
uint32_t toastUntil  = 0;
const char* toastMsg = "";
uint32_t lastFeedMs  = 0, lastPlayMs = 0;
bool     feedUsed    = false, playUsed = false;

// reloj / red
bool     timeValid   = false;   // ya tenemos la hora real
bool     caughtUp    = false;   // ya se simulo el tiempo apagado
bool     sleeping    = false;   // la mascota duerme ahora
uint32_t awakeUntil  = 0;       // despertada a la fuerza hasta...
double   stateEpoch  = 0;       // instante (epoch) al que corresponde el estado guardado; 0 = desconocido
int      hourNow = -1, minNow = 0;
uint8_t  netState    = 0;       // 0 libre, 1 conectando, 2 esperando NTP
uint32_t netStarted  = 0, nextNet = 0;
char     absMsg[24];
int      lastWifiStatus = -1;   // diagnostico WiFi (se ve en la pantalla de valores)
uint8_t  wifiFails = 0;
bool     ssidSeen  = false;

// ------------------------- BUZZER (sin bloquear) ---------------------
uint8_t  beepsLeft = 0;
uint16_t beepLen   = 0;
uint32_t beepEnd   = 0, beepNext = 0;
bool     beepOn    = false;

void beep(uint8_t n, uint16_t ms) { if (SOUND_ON) { beepsLeft = n; beepLen = ms; beepNext = 0; } }

void updateBuzzer(uint32_t now) {
  if (beepOn && now >= beepEnd) { digitalWrite(PIN_BUZZER, LOW); beepOn = false; beepNext = now + beepLen; }
  if (!beepOn && beepsLeft > 0 && now >= beepNext) {
    digitalWrite(PIN_BUZZER, HIGH); beepOn = true; beepEnd = now + beepLen; beepsLeft--;
  }
}

// ------------------------- PERSISTENCIA ------------------------------
void savePet() {
  prefs.putUChar("sp", pet.species);
  prefs.putBool("alive", pet.alive);
  prefs.putFloat("age", pet.ageDays);
  prefs.putFloat("food", pet.foodAcc);
  prefs.putFloat("fun", pet.fun);
  prefs.putFloat("care", pet.care);
  prefs.putFloat("streak", pet.streak);
  prefs.putFloat("zero", pet.zeroSec);
  prefs.putFloat("starve", pet.starveSec);
  prefs.putUChar("cause", pet.cause);
  prefs.putFloat("rest", pet.rest);
  prefs.putULong("epoch", (uint32_t)stateEpoch);
}

void loadPet() {
  pet.species = prefs.getUChar("sp", NONE);
  pet.alive   = prefs.getBool("alive", true);
  pet.ageDays = prefs.getFloat("age", 0);
  pet.foodAcc = prefs.getFloat("food", FOOD_NEED_PER_DAY);
  pet.fun     = prefs.getFloat("fun", 50);
  pet.care    = prefs.getFloat("care", 60);
  pet.streak  = prefs.getFloat("streak", 0);
  pet.zeroSec = prefs.getFloat("zero", 0);
  pet.starveSec = prefs.getFloat("starve", 0);
  pet.cause   = prefs.getUChar("cause", 0);
  pet.rest    = prefs.getFloat("rest", 80);
  stateEpoch  = prefs.getULong("epoch", 0);
}

void newPet(uint8_t sp) {
  pet = {sp, true, 0, FOOD_NEED_PER_DAY, 50, 60, 0, 0, 0, 0, 80};
  bowl = 0; eatingUntil = happyUntil = playUntil = 0;
  stateEpoch = timeValid ? (double)time(nullptr) : 0;
  savePet();
}

// ------------------------- LOGICA ------------------------------------
float nutrition() { return constrain(pet.foodAcc / FOOD_MAX_PER_DAY * 100.0f, 0, 100); }

// 100 dentro de [lo,hi], baja linealmente hasta 0 en mn y mx
float band(float x, float lo, float hi, float mn, float mx) {
  if (x >= lo && x <= hi) return 100;
  if (x < lo) return constrain((x - mn) / (lo - mn), 0, 1) * 100;
  return constrain((mx - x) / (mx - hi), 0, 1) * 100;
}

uint8_t stageOf() {
  for (int i = 0; i < 3; i++) if (pet.ageDays < STAGE_END_DAYS[i]) return i;
  return 3;
}

void die(uint8_t cause) {
  pet.alive = false;
  pet.cause = cause;
  savePet();
  beep(1, 1500);
}

bool isNightHour(int h) { return h >= SLEEP_START_H || h < SLEEP_END_H; }

// dtReal: segundos reales; clockOk: hay hora real; asleep: es de noche
void updatePet(float dtReal, bool clockOk, bool asleep) {
  float s = dtReal * TIME_SCALE;          // segundos simulados
  float d = s / 86400.0f;                 // dias simulados

  // buen trato -> envejece mas despacio (x0.6); mal trato -> mas deprisa (x1.4)
  pet.ageDays += d * (1.4f - 0.8f * pet.care / 100.0f);

  pet.foodAcc *= expf(-d);                // la "comida del dia" se va olvidando
  pet.fun     -= FUN_DECAY_PER_HOUR * (asleep ? 0.25f : 1.0f) * s / 3600.0f;
  if (clockOk) {
    pet.rest += (asleep ? REST_GAIN_PER_HOUR : -REST_DECAY_PER_HOUR) * s / 3600.0f;
    pet.rest = constrain(pet.rest, 0, 100);
  }
  pet.streak  -= s / (STREAK_DECAY_MIN * 60.0f);
  if (pet.fun < 0) pet.fun = 0;
  if (pet.streak < 0) pet.streak = 0;

  // trato oculto: se acerca despacio a lo bien que esta comida + diversion
  float foodS = band(nutrition(), 35, 65, 0, 100);
  float funS  = band(pet.fun, 40, 70, 0, 100);
  float target = clockOk ? 0.4f * foodS + 0.4f * funS + 0.2f * band(pet.rest, 50, 100, 0, 100)
                         : 0.5f * foodS + 0.5f * funS;
  pet.care += (target - pet.care) * (1.0f - expf(-d / CARE_TAU_DAYS));

  if (pet.fun <= 0) { pet.zeroSec += s; if (pet.zeroSec >= ZERO_GRACE_SEC) die(1); }
  else pet.zeroSec = 0;

  // hambre extrema: nutricion <= 5 durante 1 h seguida
  if (nutrition() <= STARVE_LEVEL) { pet.starveSec += s; if (pet.starveSec >= ZERO_GRACE_SEC) die(3); }
  else pet.starveSec = 0;

  if (pet.ageDays >= LIFE_DAYS) die(0);
}

void onFeed(uint32_t now) {
  if (!pet.alive) return;
  if (feedUsed && now - lastFeedMs < COOLDOWN_MS) { toastMsg = "Espera..."; toastUntil = now + 1500; return; }
  if (bowl >= BOWL_MAX) return;
  bowl++; feedUsed = true; lastFeedMs = now;
}

void onPlay(uint32_t now) {
  if (!pet.alive) return;
  if (sleeping) {   // despertarla de noche: se enfada y pierde descanso
    pet.rest = max(0.0f, pet.rest - 10.0f);
    awakeUntil = now + 8000;
    toastMsg = "Shh, duerme!"; toastUntil = now + 2500;
    return;
  }
  if (playUsed && now - lastPlayMs < COOLDOWN_MS) { toastMsg = "Espera..."; toastUntil = now + 1500; return; }
  playUsed = true; lastPlayMs = now;
  pet.streak = min(pet.streak + 1.0f, 10.0f);
  pet.fun   += 5.0f + 2.0f * pet.streak;   // cuanto mas juegas seguido, mas sube
  playUntil  = now + 4000;
  if (pet.fun >= 100) { pet.fun = 100; die(2); }
}

Mood moodOf(uint32_t now) {
  if (!pet.alive) return M_DEAD;
  if (sleeping) return (now < awakeUntil) ? M_SAD : M_SLEEPING;
  if (eatingUntil) return M_EATING;
  if (now < playUntil) return M_PLAYING;
  if (pet.fun >= 80) return M_HYPER;
  if (pet.fun < 25) return M_SAD;
  if (nutrition() < 20) return M_HUNGRY;
  if (nutrition() > 80) return M_STUFFED;
  if (now < happyUntil || pet.care >= 60) return M_HAPPY;
  return M_NEUTRAL;
}

// ------------------------- DIBUJO ------------------------------------
Palette palette(uint8_t sp, uint8_t stage) {
  int r, g, b, br, bg, bb;
  if (sp == CAT) { r = 255; g = 160; b = 50;  br = 255; bg = 228; bb = 175; }
  else           { r = 205; g = 145; b = 85;  br = 255; bg = 238; bb = 205; }
  if (stage == 3) { r = (r + 170) / 2; g = (g + 170) / 2; b = (b + 170) / 2;   // viejo: color apagado
                    br = 245; bg = 245; bb = 240; }
  Palette p;
  p.body  = C565(r, g, b);
  p.belly = C565(br, bg, bb);
  p.dark  = C565(r * 6 / 10, g * 6 / 10, b * 6 / 10);
  p.ear   = (sp == CAT) ? C565(255, 150, 170) : p.dark;
  return p;
}

void heart(int x, int y, int s, uint16_t c) {
  canvas.fillCircle(x - s / 2, y, s / 2 + 1, c);
  canvas.fillCircle(x + s / 2, y, s / 2 + 1, c);
  canvas.fillTriangle(x - s, y + 1, x + s, y + 1, x, y + s + 2, c);
}

void drawPet(int cx, int groundY, int R, uint8_t sp, uint8_t stage, Mood mood, uint32_t t) {
  Palette p = palette(sp, stage);
  const uint16_t BLACK = C565(40, 25, 25), WHITE = 0xFFFF, PINK = C565(255, 140, 160);
  float bob = 0; int shake = 0;
  switch (mood) {
    case M_DEAD:  break;
    case M_HYPER: bob = sinf(t * 1.2f) * 4; shake = (t & 1) ? 2 : -2; break;
    case M_SAD:   bob = sinf(t * 0.25f); break;
    case M_SLEEPING: bob = sinf(t * 0.2f); break;
    default:      bob = sinf(t * 0.5f) * 2;
  }
  if (stage == 3) bob *= 0.5f;
  cx += shake;
  int cy = groundY - R - 4 + (int)bob;

  // sombra
  canvas.fillRoundRect(cx - R, groundY - 3, 2 * R, 6, 3, C565(60, 140, 60));

  // cola
  int wag = (mood == M_DEAD) ? 0 : (int)(sinf(t * 0.7f) * 5);
  if (sp == CAT) {
    for (int i = 0; i < 3; i++) canvas.drawLine(cx + R - 2 + i, cy + R - 6, cx + R + 12 + wag + i, cy - R / 2, p.dark);
    canvas.fillCircle(cx + R + 12 + wag, cy - R / 2, 3, p.dark);
  } else {
    canvas.fillTriangle(cx + R - 4, cy + R - 4, cx + R + 9 + wag, cy + R - 16, cx + R, cy + R - 14, p.dark);
  }

  // patas
  canvas.fillCircle(cx - R / 2, cy + R - 1, 6, p.dark);
  canvas.fillCircle(cx + R / 2, cy + R - 1, 6, p.dark);

  // orejas
  if (sp == CAT) {
    for (int s = -1; s <= 1; s += 2) {
      canvas.fillTriangle(cx + s * R, cy - R / 5, cx + s * (R * 4 / 5), cy - R - 7, cx + s * (R / 6), cy - R * 9 / 10, p.dark);
      canvas.fillTriangle(cx + s * (R * 8 / 10), cy - R / 3, cx + s * (R * 3 / 4), cy - R - 1, cx + s * (R / 3), cy - R * 8 / 10, p.ear);
    }
  } else {
    canvas.fillRoundRect(cx - R - 5, cy - R * 7 / 10, 11, R, 5, p.dark);
    canvas.fillRoundRect(cx + R - 6, cy - R * 7 / 10, 11, R, 5, p.dark);
  }

  // cuerpo / cabeza
  canvas.fillCircle(cx, cy, R + 1, p.dark);
  canvas.fillCircle(cx, cy, R, p.body);
  canvas.fillCircle(cx, cy + R * 3 / 10, R * 45 / 100, p.belly);   // hocico
  if (sp == CAT) {
    for (int i = -1; i <= 1; i++) canvas.drawLine(cx + i * 4, cy - R + 3, cx + i * 4, cy - R + 9, p.dark);
  } else {
    canvas.fillCircle(cx + R / 2, cy - R * 4 / 10, R / 4, p.dark);   // mancha
  }

  // ojos
  int ex = R * 42 / 100, ey = cy - R * 15 / 100;
  int er = (stage == 0) ? 6 : (stage == 3 ? 4 : 5);
  for (int s = -1; s <= 1; s += 2) {
    int x = cx + s * ex;
    switch (mood) {
      case M_HAPPY: case M_PLAYING: case M_EATING:
        for (int k = 0; k < 2; k++) {
          canvas.drawLine(x - er, ey + er / 2 + k, x, ey - er / 2 + k, BLACK);
          canvas.drawLine(x, ey - er / 2 + k, x + er, ey + er / 2 + k, BLACK);
        }
        break;
      case M_SLEEPING:
        canvas.drawLine(x - er, ey, x + er, ey, BLACK);
        canvas.drawLine(x - er, ey + 1, x + er, ey + 1, BLACK);
        break;
      case M_DEAD:
        canvas.drawLine(x - er, ey - er, x + er, ey + er, BLACK);
        canvas.drawLine(x - er, ey + er, x + er, ey - er, BLACK);
        break;
      case M_HYPER:
        canvas.fillCircle(x, ey, er + 2, WHITE);
        canvas.drawCircle(x, ey, er + 2, BLACK);
        canvas.fillCircle(x + ((t & 1) ? 2 : -2), ey + ((t & 2) ? 1 : -1), 2, BLACK);
        break;
      case M_STUFFED:
        canvas.drawCircle(x, ey, er, BLACK);
        canvas.drawCircle(x, ey, er / 2, BLACK);
        break;
      default:
        canvas.fillCircle(x, ey, er, BLACK);
        canvas.fillCircle(x + 1, ey - 2, max(1, er / 3), WHITE);
    }
    if (mood == M_SAD)   // cejas tristes
      canvas.drawLine(x - s * er, ey - er - 2, x + s * er, ey - er - 5 + 0, BLACK),
      canvas.drawLine(x - s * er, ey - er - 3, x + s * er, ey - er - 6, BLACK);
    if (stage == 3 && mood != M_DEAD)   // viejo: cejas blancas caidas y parpado
      canvas.drawLine(x - er, ey - er - 3, x + er, ey - er - 1, WHITE),
      canvas.drawLine(x - er, ey - er - 2, x + er, ey - er, WHITE);
  }
  if (mood == M_SAD) {   // lagrima
    int ty = ey + er + 2 + (t % 6);
    canvas.fillCircle(cx - ex, ty, 2, C565(100, 180, 255));
  }

  // nariz
  int ny = cy + R * 15 / 100;
  if (sp == CAT) canvas.fillTriangle(cx - 3, ny, cx + 3, ny, cx, ny + 3, PINK);
  else           canvas.fillRoundRect(cx - 4, ny - 1, 8, 5, 2, BLACK);

  // boca
  int my = cy + R * 35 / 100;
  switch (mood) {
    case M_HAPPY: case M_PLAYING: case M_HYPER:
      canvas.drawLine(cx - 6, my, cx - 3, my + 3, BLACK);
      canvas.drawLine(cx - 3, my + 3, cx + 3, my + 3, BLACK);
      canvas.drawLine(cx + 3, my + 3, cx + 6, my, BLACK);
      if (sp == DOG) canvas.fillRoundRect(cx - 2, my + 3, 5, 6, 2, PINK);
      break;
    case M_EATING:
      canvas.fillCircle(cx, my + 3, 3 + (t & 1) * 2, C565(170, 40, 50));
      break;
    case M_HUNGRY:
      canvas.fillCircle(cx, my + 3, 3, C565(170, 40, 50));
      break;
    case M_SAD: case M_STUFFED: case M_DEAD:
      canvas.drawLine(cx - 5, my + 4, cx - 2, my + 1, BLACK);
      canvas.drawLine(cx - 2, my + 1, cx + 2, my + 1, BLACK);
      canvas.drawLine(cx + 2, my + 1, cx + 5, my + 4, BLACK);
      break;
    default:
      canvas.drawLine(cx - 4, my + 2, cx + 4, my + 2, BLACK);
  }

  // bigotes
  if (sp == CAT) {
    for (int s = -1; s <= 1; s += 2) {
      canvas.drawLine(cx + s * (R / 2), cy + R / 3, cx + s * (R + 8), cy + R / 5, BLACK);
      canvas.drawLine(cx + s * (R / 2), cy + R / 2, cx + s * (R + 8), cy + R / 2 + 2, BLACK);
    }
  }

  // mejillas / extras segun estado
  if (mood == M_HAPPY || mood == M_PLAYING || mood == M_EATING) {
    canvas.fillCircle(cx - R * 62 / 100, cy + R * 30 / 100, 3, PINK);
    canvas.fillCircle(cx + R * 62 / 100, cy + R * 30 / 100, 3, PINK);
  }
  if (mood == M_STUFFED) {   // cara verdosa + gota de sudor
    canvas.fillCircle(cx - R * 62 / 100, cy + R * 30 / 100, 4, C565(150, 210, 120));
    canvas.fillCircle(cx + R * 62 / 100, cy + R * 30 / 100, 4, C565(150, 210, 120));
    canvas.fillCircle(cx + R * 7 / 10, cy - R * 6 / 10, 2, C565(100, 180, 255));
  }
  if (mood == M_HUNGRY) {   // globo con comida
    int bx = cx - R - 4, by = cy - R - 8;
    canvas.fillCircle(bx, by, 10, WHITE);
    canvas.fillCircle(bx, by, 4, C565(200, 70, 40));
    canvas.drawLine(bx + 3, by + 3, bx + 8, by + 8, C565(230, 230, 210));
  }
  if (mood == M_HYPER) {    // chispas
    uint16_t Y = C565(255, 220, 0);
    int a = (t & 1) ? 0 : 5;
    canvas.drawLine(cx - R - 10 + a, cy - R, cx - R - 4 + a, cy - R, Y);
    canvas.drawLine(cx - R - 7 + a, cy - R - 3, cx - R - 7 + a, cy - R + 3, Y);
    canvas.drawLine(cx + R + 4 - a, cy - R + 8, cx + R + 10 - a, cy - R + 8, Y);
    canvas.drawLine(cx + R + 7 - a, cy - R + 5, cx + R + 7 - a, cy - R + 11, Y);
  }
  if (mood == M_SLEEPING) {   // zzz
    canvas.setTextColor(WHITE);
    canvas.setTextSize(1); canvas.setCursor(cx + R / 2, cy - R - 4 - (t % 12)); canvas.print("z");
    canvas.setTextSize(2); canvas.setCursor(cx + R, cy - R - 18 - (t % 12));    canvas.print("Z");
    canvas.setTextSize(1);
  }
  if (mood == M_HAPPY || mood == M_EATING) {   // corazoncito flotando
    int hy = cy - R - 10 - (t % 8);
    heart(cx + R / 2, hy, 4, C565(255, 60, 90));
  }
}

void drawScene(uint32_t now, uint32_t t) {
  // fondo (de dia o de noche segun la hora real)
  bool night = hourNow >= 0 && isNightHour(hourNow);
  if (night) {
    uint16_t sky = C565(20, 25, 70);
    canvas.fillScreen(sky);
    canvas.fillRect(0, 112, 128, 48, C565(30, 70, 40));
    canvas.fillCircle(106, 20, 9, C565(240, 240, 200));      // luna creciente
    canvas.fillCircle(110, 17, 8, sky);
    for (int i = 0; i < 6; i++) {                            // estrellas que parpadean
      int sx = 10 + i * 19, sy = 10 + (i * 37) % 60;
      if (((t / 4) + i) % 3) canvas.drawPixel(sx, sy, 0xFFFF);
      else canvas.drawFastHLine(sx - 1, sy, 3, 0xFFFF);
    }
  } else {
    canvas.fillScreen(C565(150, 215, 255));
    canvas.fillRect(0, 112, 128, 48, C565(110, 200, 90));
    canvas.fillCircle(106, 20, 10, C565(255, 225, 60));
    canvas.fillCircle(24, 22, 6, 0xFFFF); canvas.fillCircle(32, 20, 8, 0xFFFF); canvas.fillCircle(41, 23, 6, 0xFFFF);
  }

  uint8_t stage = stageOf();
  Mood mood = moodOf(now);

  if (mood == M_DEAD) {
    canvas.fillRoundRect(46, 62, 36, 54, 14, C565(150, 150, 160));
    canvas.setTextColor(C565(60, 60, 70)); canvas.setTextSize(1);
    canvas.setCursor(54, 76); canvas.print("RIP");
    canvas.fillRect(62, 88, 4, 14, C565(60, 60, 70)); canvas.fillRect(57, 92, 14, 4, C565(60, 60, 70));
    canvas.setTextColor(0xFFFF);
    const char* msg[] = {"murio de vejez", "murio de pena", "murio de tanta emocion",
                         "murio de hambre", "murio de empacho"};
    canvas.setCursor(4, 128); canvas.print(msg[pet.cause]);
    canvas.setCursor(4, 140); canvas.print("Dias vividos: "); canvas.print((int)pet.ageDays);
    canvas.setCursor(4, 150); canvas.print("A+B 10s = reiniciar");
    return;
  }

  // plato
  canvas.fillRoundRect(92, 128, 28, 10, 4, C565(200, 60, 60));
  for (int i = 0; i < bowl; i++) canvas.fillCircle(99 + i * 8, 126, 5, C565(150, 90, 40));

  // pelota al jugar
  if (mood == M_PLAYING) {
    int by = 108 - (int)(fabsf(sinf(t * 0.6f)) * 28);
    canvas.fillCircle(24, by, 6, C565(230, 50, 50));
    canvas.drawLine(19, by, 29, by, 0xFFFF);
  }

  static const int R_BY_STAGE[4] = {20, 25, 30, 29};
  drawPet(58, 118, R_BY_STAGE[stage], pet.species, stage, mood, t);

  // texto de etapa
  static const char* STAGE_NAME[4] = {"Cachorro", "Joven", "Adulto", "Mayor"};
  canvas.setTextSize(1); canvas.setTextColor(0xFFFF);
  canvas.setCursor(3, 148);
  canvas.print(STAGE_NAME[stage]); canvas.print(" - dia "); canvas.print((int)pet.ageDays + 1);
  if (hourNow >= 0) {   // reloj arriba a la izquierda
    canvas.setTextColor(night ? 0xFFFF : C565(40, 40, 80));
    canvas.setCursor(3, 3);
    if (hourNow < 10) canvas.print('0');
    canvas.print(hourNow); canvas.print(':');
    if (minNow < 10) canvas.print('0');
    canvas.print(minNow);
  }

  // frase de la mascota (el font del display usa CP437: \xAD = "¡", \x82 = "é")
  const char* say = nullptr;
  if (mood == M_EATING)       say = "\xAD" "Qu\x82 rico!";
  else if (mood == M_PLAYING) say = "\xAD" "Qu\x82 divertido!";
  if (say && now >= toastUntil) {
    int w = strlen(say) * 6 + 10, x = (128 - w) / 2;
    canvas.fillRoundRect(x, 22, w, 14, 5, 0xFFFF);
    canvas.drawRoundRect(x, 22, w, 14, 5, C565(40, 25, 25));
    canvas.setTextColor(C565(40, 25, 25)); canvas.setCursor(x + 5, 26); canvas.print(say);
  }

  if (now < toastUntil) {
    canvas.fillRoundRect(14, 4, 100, 14, 4, 0xFFFF);
    canvas.setTextColor(C565(60, 60, 60)); canvas.setCursor(20, 8); canvas.print(toastMsg);
  }
}

void drawSelect(uint32_t t) {
  canvas.fillScreen(C565(150, 215, 255));
  canvas.fillRect(0, 112, 128, 48, C565(110, 200, 90));
  canvas.setTextColor(C565(40, 40, 80)); canvas.setTextSize(1);
  canvas.setCursor(14, 8);  canvas.print("ELIGE TU MASCOTA");
  drawPet(32, 118, 18, CAT, 1, M_HAPPY, t);
  drawPet(96, 118, 18, DOG, 1, M_HAPPY, t);
  canvas.setTextColor(0xFFFF);
  canvas.setCursor(8, 130);  canvas.print("Boton 1");
  canvas.setCursor(8, 142);  canvas.print("= GATO");
  canvas.setCursor(72, 130); canvas.print("Boton 2");
  canvas.setCursor(72, 142); canvas.print("= PERRO");
}

void drawStats() {   // pantalla para el monitor: mantener A+B > 1s
  canvas.fillScreen(C565(20, 20, 40));
  canvas.setTextColor(0xFFFF); canvas.setTextSize(1);
  int y = 8;
  auto line = [&](const char* label, float v, const char* unit) {
    canvas.setCursor(6, y); canvas.print(label); canvas.print(": "); canvas.print(v, 1); canvas.print(unit); y += 14;
  };
  line("Edad", pet.ageDays, " d");
  line("Vida max", LIFE_DAYS, " d");
  line("Comida", nutrition(), "/100");
  line("Diversion", pet.fun, "/100");
  line("Trato", pet.care, "/100");
  line("Descanso", pet.rest, "/100");
  line("Racha", pet.streak, "");
  line("Ritmo vejez", 1.4f - 0.8f * pet.care / 100.0f, "x");
  canvas.setCursor(6, 118);
  if (!wifiConfigured())        canvas.print("WiFi: sin config.");
  else if (timeValid)           canvas.print("WiFi: OK (hora ok)");
  else if (netState != 0)       canvas.print("WiFi: conectando..");
  else if (wifiFails == 0)      canvas.print("WiFi: esperando");
  else if (!ssidSeen)           canvas.print("WiFi: no veo la red");
  else if (lastWifiStatus == 4 || lastWifiStatus == 6) canvas.print("WiFi: clave mala?");
  else { canvas.print("WiFi: fallo cod "); canvas.print(lastWifiStatus); }
  canvas.setCursor(6, 132);
  canvas.print("Hora: ");
  if (hourNow >= 0) {
    if (hourNow < 10) canvas.print('0');
    canvas.print(hourNow); canvas.print(':');
    if (minNow < 10) canvas.print('0');
    canvas.print(minNow);
  } else {
    canvas.print(wifiConfigured() ? "sin sincronizar" : "WiFi sin config.");
  }
  canvas.setCursor(6, 146); canvas.print("A+B 10s: reiniciar");
}

// ------------------------- BOTONES -----------------------------------
Btn bFeed = {PIN_BTN_FEED, false, 0}, bPlay = {PIN_BTN_PLAY, false, 0};
bool     comboLatched = false;
uint32_t comboSince   = 0;
bool     comboDone    = false;

bool pollBtn(Btn& b, uint32_t now) {   // true cuando se acaba de soltar
  bool d = digitalRead(b.pin) == LOW;
  if (d != b.down && now - b.changed > 30) {
    b.changed = now; b.down = d;
    return !d;
  }
  return false;
}

void handleButtons(uint32_t now) {
  bool relF = pollBtn(bFeed, now);
  bool relP = pollBtn(bPlay, now);

  if (bFeed.down && bPlay.down && !comboLatched) { comboLatched = true; comboSince = now; comboDone = false; }
  if (comboLatched) {
    if (!bFeed.down && !bPlay.down) comboLatched = false;
    else if (!comboDone && now - comboSince > RESET_HOLD_MS) {   // reinicio
      comboDone = true;
      pet.species = NONE; savePet();
    }
    return;   // mientras hay combo no se procesan pulsaciones sueltas
  }

  if (pet.species == NONE) {
    if (relF) newPet(CAT);
    if (relP) newPet(DOG);
    return;
  }
  if (relF) onFeed(now);
  if (relP) onPlay(now);
}

// ------------------------- WIFI / NTP / TIEMPO APAGADO ---------------
bool wifiConfigured() { return strcmp(WIFI_SSID, "TU_WIFI") != 0; }

// Pide a internet la diferencia horaria actual (ya incluye horario de verano).
bool fetchUtcOffset(int& offsetSec) {
  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin("http://ip-api.com/json/?fields=status,offset")) return false;
  bool ok = false;
  if (http.GET() == 200) {
    String b = http.getString();
    int i = b.indexOf("\"offset\":");
    if (i >= 0 && b.indexOf("success") >= 0) {
      offsetSec = b.substring(i + 9).toInt();
      ok = abs(offsetSec) <= 14 * 3600;
    }
  }
  http.end();
  return ok;
}

bool sleepingAtEpoch(uint32_t e) {
  time_t tt = e; struct tm tmv; localtime_r(&tt, &tmv);
  return isNightHour(tmv.tm_hour);
}

// Simula el tiempo que estuvo apagado, en trozos de 5 min, respetando dia/noche.
void catchUp(uint32_t fromE, uint32_t toE) {
  for (uint32_t t = fromE; t < toE && pet.alive; ) {
    uint32_t step = min<uint32_t>(300, toE - t);
    updatePet((float)step, true, sleepingAtEpoch(t + step / 2));
    t += step;
  }
}

void onTimeSynced() {
  uint32_t nowE = (uint32_t)time(nullptr);
  if (!caughtUp) {
    caughtUp = true;
    if (stateEpoch > 0 && pet.species != NONE && pet.alive && nowE > (uint32_t)stateEpoch + 10) {
      uint32_t gap = nowE - (uint32_t)stateEpoch;
      catchUp((uint32_t)stateEpoch, nowE);
      uint32_t h = gap / 3600;
      if (h >= 48)     snprintf(absMsg, sizeof absMsg, "Ausente %lud", (unsigned long)(h / 24));
      else if (h >= 1) snprintf(absMsg, sizeof absMsg, "Ausente %luh", (unsigned long)h);
      else             snprintf(absMsg, sizeof absMsg, "Ausente %lum", (unsigned long)(gap / 60));
      toastMsg = absMsg; toastUntil = millis() + 6000;
      Serial.printf("Tiempo apagado simulado: %lu s\n", (unsigned long)gap);
    }
  }
  stateEpoch = nowE;
  savePet();
}

void wifiOff() { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }

// Maquina de estados no bloqueante: conecta, sincroniza la hora y apaga el WiFi.
void netLoop(uint32_t now) {
  if (!wifiConfigured()) return;
  switch (netState) {
    case 0:
      if (now >= nextNet) {
        WiFi.persistent(false);                // no guardar credenciales en la flash
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);   // ANTES de begin: ayuda al SuperMini a conectar
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        Serial.println("WiFi: conectando...");
        netState = 1; netStarted = now;
      }
      break;
    case 1:
      if (WiFi.status() == WL_CONNECTED) {
        String tz = TZ_FALLBACK;
        int off;
        if (AUTO_TIMEZONE && fetchUtcOffset(off)) {
          int a = abs(off);
          char buf[24];
          snprintf(buf, sizeof buf, "UTC%c%d:%02d", off >= 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
          tz = buf;      // formato POSIX: el signo va al reves
        }
        configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
        Serial.printf("Zona horaria: %s\n", tz.c_str());
        netState = 2; netStarted = millis();
      } else if (now - netStarted > 20000) {
        // estado: 1 = no encuentra la red, 4 = fallo de clave, 6 = desconectado
        lastWifiStatus = (int)WiFi.status();
        if (wifiFails < 255) wifiFails++;
        Serial.printf("WiFi: no conecta (estado %d, intento %u)\n", lastWifiStatus, wifiFails);
        if (wifiFails <= 3) {   // lista las redes que ve, para saber si ve la tuya
          int n = WiFi.scanNetworks();
          ssidSeen = false;
          Serial.printf("Redes visibles: %d\n", n);
          for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) == WIFI_SSID) ssidSeen = true;
            Serial.printf("  - \"%s\"  %d dBm  canal %d\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i));
          }
          WiFi.scanDelete();
          Serial.printf("Tu red \"%s\": %s\n", WIFI_SSID, ssidSeen ? "VISIBLE (revisa la clave)" : "NO visible (revisa nombre / 2,4 GHz / distancia)");
        }
        wifiOff(); netState = 0;
        nextNet = now + (timeValid ? 300000UL : 30000UL);   // sin hora: reintenta en 30 s
      }
      break;
    case 2:
      if (time(nullptr) > 1700000000 && (!timeValid || now - netStarted > 4000)) {
        bool first = !timeValid;
        timeValid = true;
        wifiOff(); netState = 0; nextNet = now + 6UL * 3600 * 1000;   // resincroniza cada 6 h
        if (first) onTimeSynced();
      } else if (now - netStarted > 15000) {
        Serial.println("WiFi: conectado pero sin respuesta NTP");
        wifiOff(); netState = 0;
        nextNet = now + (timeValid ? 300000UL : 30000UL);
      }
      break;
  }
}

// ------------------------- SETUP / LOOP ------------------------------
uint32_t lastAlert = 0;
uint32_t lastMs = 0, lastFrame = 0, lastSave = 0, lastLog = 0, frameNo = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_FEED, INPUT_PULLUP);
  pinMode(PIN_BTN_PLAY, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  tft.initR(INITR_BLACKTAB);      // si ves 2px de ruido o colores raros prueba INITR_GREENTAB
  tft.setRotation(0);             // vertical 128x160
  tft.fillScreen(ST77XX_BLACK);

  prefs.begin("pet", false);
  loadPet();
  if (!wifiConfigured()) Serial.println("WiFi sin configurar: edita WIFI_SSID y WIFI_PASS. Funciona sin ciclo dia/noche.");
  lastMs = millis();
}

void loop() {
  uint32_t now = millis();
  float dt = (now - lastMs) / 1000.0f;
  lastMs = now;

  netLoop(now);

  // hora local y estado de sueno
  if (timeValid) {
    time_t e = time(nullptr);
    struct tm tmv; localtime_r(&e, &tmv);
    hourNow = tmv.tm_hour; minNow = tmv.tm_min;
    sleeping = isNightHour(hourNow);
    stateEpoch = (double)e;
  } else {
    hourNow = -1; sleeping = false;
    if (stateEpoch > 0) stateEpoch += dt;   // sin hora: avanza el reloj "estimado"
  }

  handleButtons(now);
  updateBuzzer(now);

  if (pet.species != NONE && pet.alive) {
    updatePet(dt, timeValid, sleeping);
    if (bowl > 0 && eatingUntil == 0 && !sleeping) eatingUntil = now + 3000;      // se pone a comer
    if (eatingUntil && now >= eatingUntil) {
      bowl--; eatingUntil = 0;
      pet.foodAcc = min(pet.foodAcc + 1.0f, 20.0f);
      happyUntil = now + 4000;
      if (nutrition() >= 100) die(4);      // sobrealimentacion
    }
    // aviso sonoro en casos extremos
    if (pet.alive && !sleeping && now - lastAlert > ALERT_EVERY_MS &&
        (nutrition() < 10 || nutrition() > 90 || pet.fun < 10 || pet.fun > 90)) {
      lastAlert = now; beep(3, 120);
    }
    if (now - lastSave > 300000) { savePet(); lastSave = now; }      // cada 5 min
  }

  if (now - lastFrame >= 120) {
    lastFrame = now; frameNo++;
    if (pet.species == NONE)                              drawSelect(frameNo);
    else if (comboLatched && now - comboSince > 1000)     drawStats();
    else                                                  drawScene(now, frameNo);
    tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 128, 160);
  }

  if (now - lastLog > 10000) {    // depuracion por Serial
    lastLog = now;
    Serial.printf("hora=%02d:%02d  edad=%.2f d  comida=%.0f  fun=%.0f  descanso=%.0f  trato=%.0f  racha=%.1f\n",
                  hourNow, minNow, pet.ageDays, nutrition(), pet.fun, pet.rest, pet.care, pet.streak);
  }
}
