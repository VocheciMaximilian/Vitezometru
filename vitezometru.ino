/**************************************************************************************
 *                      VITEZOMETRU BICICLETĂ CU ARDUINO
 *
 * Acest proiect implementează un vitezometru pentru bicicletă folosind un Arduino,
 * un senzor Hall, un afișaj LCD 16x2, un RTC DS1302 și butoane pentru interacțiune.
 * Funcționalități principale:
 *  - Măsurarea vitezei instantanee și RPM.
 *  - Calculul vitezei medii "rolling" (pe ultimele 10 măsurători).
 *  - Afișarea orei și datei curente.
 *  - Înregistrarea distanței totale (odometru) și a timpului total de pedalare.
 *  - Gestionarea și salvarea în EEPROM a până la MAX_TRIPS ture individuale,
 *    incluzând viteza medie, maximă, minimă, distanța, durata și timestamp-ul.
 *  - Mod de blocare (LOCK) cu PIN pentru a descuraja utilizarea neautorizată.
 *  - Meniu de setări pentru: diametrul roții, setarea ceasului/datei, resetare la
 *    valorile din fabrică și exportul turelor prin portul serial.
 *  - Comunicare serială cu un PC pentru a exporta datele turelor la cerere.
 *  - Mod standby pentru economisirea energiei afișajului LCD.
 *
 **************************************************************************************/

// --- 0. ANTETE ȘI GLOBALE ---

// Biblioteci necesare
#include <LiquidCrystal.h> // Pentru controlul afișajului LCD
#include <ThreeWire.h>     // Pentru comunicarea cu RTC DS1302 (protocol specific)
#include <RtcDS1302.h>   // Pentru gestionarea ceasului Real-Time Clock DS1302
#include <EEPROM.h>        // Pentru stocarea persistentă a datelor

// Definirea constantei PI dacă nu este deja definită de compilator
#ifndef M_PI
#define M_PI 3.1415926535
#endif

// --- COMANDA DE LA PC ---
// Stringul comenzii pe care aplicația Python o va trimite pentru a solicita exportul turelor
#define CMD_EXPORT_TRIPS_NOW_STR "EXPORT_TRIPS_NOW"

// --- DEFINIȚII ADRESE EEPROM ---
// Adresele de start în memoria EEPROM pentru diverse date salvate.
// Acest lucru ajută la organizarea datelor și la evitarea suprascrierilor accidentale.
#define EEPROM_ADDR_MAGIC_NUM    0  // 2 bytes pentru un număr "magic" de verificare a inițializării EEPROM
#define EEPROM_ADDR_WHEEL_DIAM   (EEPROM_ADDR_MAGIC_NUM + 2) // float (4 bytes) pentru diametrul roții
#define EEPROM_ADDR_ODOMETER     (EEPROM_ADDR_WHEEL_DIAM + sizeof(float)) // float (4 bytes) pentru odometru
#define EEPROM_ADDR_TOTAL_RIDE_MS (EEPROM_ADDR_ODOMETER + sizeof(float)) // unsigned long (4 bytes) pentru timpul total de pedalare
#define EEPROM_ADDR_ACTIVE_TRIP_IDX (EEPROM_ADDR_TOTAL_RIDE_MS + sizeof(unsigned long)) // byte (1 byte) pentru indexul turei active/ultimei ture salvate
#define EEPROM_ADDR_TRIPS_START  (EEPROM_ADDR_ACTIVE_TRIP_IDX + sizeof(byte)) // Adresa de început a zonei pentru stocarea datelor turelor
#define EEPROM_MAGIC_VALUE       0xABBA // Valoarea numărului magic. Dacă această valoare nu se găsește la adresa specificată,
                                     // EEPROM-ul este considerat neinițializat sau corupt.

// --- PINI ---
// Pinii utilizați pentru diverse componente hardware.

// Pini LCD
const byte LCD_CONTRAST_PIN = 9; // Pin pentru controlul contrastului LCD (prin PWM)
const byte LCD_RS  = 3;          // Pin Register Select
const byte LCD_EN  = 8;          // Pin Enable
const byte LCD_D4  = 4;          // Pinii de date (mod 4-bit)
const byte LCD_D5  = 5;
const byte LCD_D6  = 6;
const byte LCD_D7  = 7;

// Pini senzori și actuatori
const byte HALL_PIN   = 13; // Pinul senzorului Hall. Trebuie să corespundă cu PCINT5 pentru întreruperi PinChange.
const byte BUZZER_PIN = 2;  // Pinul pentru buzzer.

// Pini butoane (conectați la pini analogici, utilizați ca digitali cu rezistențe pull-up interne)
const byte BTN_UP   = A2;   // Buton Sus / "DA"
const byte BTN_MODE = A1;   // Buton MOD
const byte BTN_DOWN = A0;   // Buton Jos / "NU"
const byte BTN_OK   = A3;   // Buton OK / Confirmare

// --- RTC Makuna DS1302 ---
// Pinii pentru modulul RTC DS1302
#define DS1302_DAT 11 // Pin Data
#define DS1302_CLK 10 // Pin Clock
#define DS1302_RST 12 // Pin Reset (Chip Enable)
ThreeWire      myWire(DS1302_DAT, DS1302_CLK, DS1302_RST); // Inițializează interfața ThreeWire
RtcDS1302<ThreeWire> rtc(myWire);                          // Inițializează obiectul RTC

// --- CODURI PENTRU MODUL LOCK ---
// Enumerare pentru a asocia coduri numerice butoanelor, utilizate în logica PIN-ului de blocare.
enum BtnCode : byte { CODE_DN = 0, CODE_MD = 1, CODE_UP = 2, CODE_OK = 3 };

// --- VARIABILE LOCK ---
byte  lockPin[4]   = {0,0,0,0}; // Array pentru a stoca secvența PIN setată de utilizator
byte  pinIndex     = 0;         // Indexul curent la introducerea/setarea PIN-ului
byte  attemptsLeft = 3;         // Numărul de încercări rămase pentru introducerea PIN-ului corect
bool  lockArmed    = false;       // Starea sistemului de blocare (armat/dezarmat)
bool  alarmActive  = false;       // Starea alarmei (activă/inactivă)
unsigned long lockArmedTime = 0; // Timpul (millis) la care s-a armat sistemul
const unsigned long lockGracePeriodMillis = 2000; // Perioada de grație (ms) după armare înainte ca mișcarea să declanșeze alarma

// --- PAGINA GLOBALĂ (Normal Mode) ---
byte page = 0; // Indexul paginii curente afișate în modul normal

// --- LCD ---
// Inițializează obiectul LiquidCrystal cu pinii corespunzători
LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// --- STARE GENERALĂ ---
// Enumerare pentru modurile de operare ale vitezometrului
enum Mode { MODE_NORMAL, MODE_TRIP, MODE_LOCK };
Mode currentMode = MODE_NORMAL; // Modul curent de operare, inițializat ca NORMAL

// --- DATE ROTAȚII ---
// Variabile volatile, deoarece sunt modificate în ISR (Interrupt Service Routine)
volatile unsigned long lastPulseMicros   = 0; // Timpul (micros) al ultimului impuls de la senzorul Hall
volatile unsigned long pulseCount        = 0; // Numărul total de impulsuri de la pornire/resetare
volatile bool          newPulseDetected  = false; // Flag setat de ISR când un nou impuls este detectat

// --- UTILITARE ---
float wheelDiameterIn   = 26.0; // Diametrul roții în inci (valoare default, poate fi modificată din meniu)
float wheelCircum_mm    = wheelDiameterIn * 25.4 * M_PI; // Circumferința roții în milimetri, calculată pe baza diametrului

unsigned long standbyTimeout    = 5UL * 60UL * 1000UL; // Timeout pentru intrarea în standby (5 minute)
unsigned long lastMovementMillis = 0; // Timpul (millis) ultimei mișcări detectate (ultimul impuls)
unsigned long lastEepromSaveMillis = 0; // Timpul (millis) ultimei salvări automate în EEPROM
const unsigned long eepromSaveIntervalMoving = 30000UL;  // Interval de salvare în EEPROM când bicicleta e în mișcare (30 secunde)
const unsigned long eepromSaveIntervalStopped = 5000UL; // Interval de salvare în EEPROM când bicicleta e oprită (5 secunde)

// --- STRUCTURĂ TURĂ ---
#define MAX_TRIPS 5 // Numărul maxim de ture ce pot fi stocate
struct Trip {
  float avgSpeed;           // Viteza medie a turei (km/h)
  float maxSpeed;           // Viteza maximă atinsă în tură (km/h)
  float minSpeed;           // Viteza minimă (peste un prag) în tură (km/h)
  float distance_km;        // Distanța parcursă în tură (km)
  unsigned long duration_s; // Durata turei (secunde)
  RtcDateTime stamp;        // Timestamp-ul (data și ora) de început al turei
  unsigned long tripStartTimeMillis; // Timpul (millis) de start al turei, folosit pentru calculul duratei curente
} trips[MAX_TRIPS]; // Array pentru a stoca datele turelor

byte  activeTripIndex = 0; // Indexul curent în array-ul `trips` pentru tura activă sau ultima salvată
bool  tripRunning     = false; // Flag care indică dacă o tură este în desfășurare

// --- ALTE VARIABILE ---
float instantSpeed   = 0;       // Viteza instantanee calculată (km/h)
float rpm            = 0;       // Rotații pe minut ale roții
float rollingSpeedBuffer[10]; // Buffer pentru calculul vitezei medii "rolling"
byte  rollingIdx = 0;         // Index pentru bufferul `rollingSpeedBuffer`

// --- ODOMETRU GLOBAL ȘI TIMP TOTAL ---
float odometer_km = 0;                // Distanța totală parcursă (km)
unsigned long totalActiveRideMillis = 0; // Timpul total de pedalare activă (ms)
bool dataChangedSinceLastSave = false; // Flag care indică dacă datele (odometru, timp) s-au modificat de la ultima salvare EEPROM

/*************************************************************************
   FUNCȚIE UTILĂ:  printFloatFix()
   Afișează un număr float pe LCD cu un număr fix de zecimale.
   dtostrf (double to string float) convertește un float într-un string.
   Parametri:
     val: valoarea float de afișat
     width: lățimea totală minimă a câmpului (incluzând punctul și semnul)
     prec: numărul de zecimale de afișat
**************************************************************************/
void printFloatFix(float val, byte width, byte prec) {
  char buf[12]; // Buffer suficient de mare pentru string-ul rezultat
  dtostrf(val, width, prec, buf);
  lcd.print(buf);
}

// --- PROTOTIPURI FUNCȚII ---
// Declararea funcțiilor definite ulterior în cod pentru a permite apelarea lor înainte de definiția completă.
void loadFromEEPROM();
void saveSettingsToEEPROM();
void saveOdometerDataToEEPROM();
void saveTripToEEPROM(byte tripIdx);
bool readButtons(bool&, bool&, bool&, bool&); // Referințe pentru a modifica direct variabilele de stare ale butoanelor
float calculateAverageSpeed();
void handleStandby();
void updateOdoAndTripData();
void drawNormalPages(byte);
void runSettingsMenu();
bool confirmStartTrip();
bool confirmStartLock();
void lockModeRunner(bool,bool,bool,bool,bool);
void tripModeRunner(bool,bool,bool,bool);
void startStopTrip(bool);
void setWheelDiameter();
void setClockTime();
void setClockDate();
void factoryReset();
void exportTripsToSerial(bool triggeredByPC = false); // Parametru pentru a diferenția apelul de la PC
unsigned long getTotalRideTimeDays();


/*************************************************************************
 * 1. setup() - Funcția de inițializare, rulează o singură dată la pornire.
*************************************************************************/
void setup() {
  Serial.begin(9600); // Inițializează comunicarea serială la 9600 baud

  // Inițializare LCD
  pinMode(LCD_CONTRAST_PIN, OUTPUT);    // Setează pinul de contrast ca ieșire
  analogWrite(LCD_CONTRAST_PIN, 120); // Setează contrastul LCD (valoare experimentală, poate necesita ajustare)
  lcd.begin(16,2); lcd.clear();         // Inițializează LCD 16x2 și curăță ecranul
  lcd.print(F("Vitezometru..."));      // Mesaj de pornire

  // Inițializare pini butoane ca intrări cu rezistențe pull-up interne activate
  pinMode(BTN_UP  , INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_OK  , INPUT_PULLUP);

  // Inițializare pin buzzer ca ieșire, starea inițială LOW (oprit)
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

  // Inițializare senzor Hall și întreruperi PinChange
  pinMode(HALL_PIN, INPUT_PULLUP); // Pinul senzorului Hall ca intrare cu pull-up
  PCICR  |= (1 << PCIE0);          // Activează întreruperile PinChange pentru grupul PCINT0-PCINT7 (Port B)
  PCMSK0 |= (1 << PCINT5);        // Activează întreruperea specifică pentru PCINT5 (care corespunde pinului digital 13 pe Arduino Uno/Nano)

  // Inițializare RTC
  rtc.Begin(); // Pornește comunicarea cu RTC-ul
  // Verifică dacă data/ora din RTC este validă sau dacă anul este < 2023 (semn de neinițializare)
  if (!rtc.IsDateTimeValid() || rtc.GetDateTime().Year() < 2023) {
      RtcDateTime compiledTime(__DATE__, __TIME__); // Obține data și ora compilării codului
      rtc.SetDateTime(compiledTime);                 // Setează RTC-ul cu această valoare
      // Dezactivează protecția la scriere, setează ora, reactivează protecția (dacă nu e deja activă)
      if (!rtc.GetIsWriteProtected()) {
          rtc.SetIsWriteProtected(true);
      }
  }

  loadFromEEPROM(); // Încarcă setările și datele salvate din EEPROM

  wheelCircum_mm = wheelDiameterIn * 25.4 * M_PI; // Calculează circumferința roții pe baza diametrului încărcat/default

  lastMovementMillis = millis(); // Inițializează timpul ultimei mișcări
  lastEepromSaveMillis = millis(); // Inițializează timpul ultimei salvări EEPROM
  delay(1500); // Pauză pentru afișarea mesajului de pornire
  lcd.clear(); // Curăță ecranul
}

/*************************************************************************
 * 2. ISR Hall - Interrupt Service Routine pentru senzorul Hall (PinChange)
 *   Rulează automat la fiecare tranziție a semnalului pe pinul HALL_PIN.
 *   Utilizată pentru a detecta rotațiile roții.
*************************************************************************/
ISR(PCINT0_vect) {
  // Verifică dacă pinul HALL_PIN este LOW.
  // Presupunem că senzorul Hall trage pinul la LOW când magnetul e prezent și folosim INPUT_PULLUP.
  if (digitalRead(HALL_PIN) == HIGH) return; // Ignoră dacă e HIGH (așteaptă să devină LOW)

  unsigned long now = micros(); // Timpul curent în microsecunde
  unsigned long dT  = now - lastPulseMicros; // Diferența de timp de la ultimul impuls

  // Debounce software simplu: ignoră impulsurile prea rapide (mai puțin de 10ms)
  if (dT < 10000) return;

  lastPulseMicros   = now; // Actualizează timpul ultimului impuls
  pulseCount++;            // Incrementează contorul de impulsuri
  newPulseDetected = true; // Setează flag-ul pentru bucla principală
}

/*************************************************************************
 * 3. readButtons() - Citește starea butoanelor cu debounce.
 *   Returnează true dacă un buton nou a fost apăsat de la ultima citire.
 *   Actualizează variabilele de stare ale butoanelor (up, down, mode, ok) prin referință.
*************************************************************************/
bool readButtons(bool &up, bool &down, bool &mode, bool &ok) {
  static unsigned long lastReadTime = 0;  // Timpul ultimei citiri valide a butoanelor
  static byte lastButtonState = 0xFF;     // Starea combinată a butoanelor la ultima citire (bitmask)
  const unsigned int DEBOUNCE_DELAY = 50; // Timp de debounce în milisecunde

  up = false; down = false; mode = false; ok = false;

  if (millis() - lastReadTime < DEBOUNCE_DELAY) {
    return false;
  }
  lastReadTime = millis();

  byte currentButtonState = 0;
  if (digitalRead(BTN_UP)   == LOW) currentButtonState |= 0b1000;
  if (digitalRead(BTN_DOWN) == LOW) currentButtonState |= 0b0100;
  if (digitalRead(BTN_MODE) == LOW) currentButtonState |= 0b0010;
  if (digitalRead(BTN_OK)   == LOW) currentButtonState |= 0b0001;

  byte pressedButtons = (currentButtonState & ~lastButtonState);
  lastButtonState = currentButtonState;

  if (pressedButtons > 0) {
    if (pressedButtons & 0b1000) up = true;
    if (pressedButtons & 0b0100) down = true;
    if (pressedButtons & 0b0010) mode = true;
    if (pressedButtons & 0b0001) ok = true;
    return true;
  }
  return false;
}

/*************************************************************************
 * 4. calculateAverageSpeed() - Calculează viteza medie "rolling".
 *   Media vitezelor stocate în `rollingSpeedBuffer`.
*************************************************************************/
float calculateAverageSpeed() {
  float sum = 0;
  byte count = 0;
  for (byte i=0; i<10; i++) {
    sum += rollingSpeedBuffer[i];
    if (rollingSpeedBuffer[i] > 0.01f || i < rollingIdx) {
        count++;
    }
  }
  return (count > 0) ? (sum / count) : 0.0;
}

/*************************************************************************
 * 5. updateOdoAndTripData() - Actualizează odometrul și datele turei curente.
 *   Procesează impulsurile noi detectate de ISR.
*************************************************************************/
void updateOdoAndTripData() {
  static unsigned long localPulseCountCopy = 0;
  unsigned long currentPulseCountSnapshot;

  noInterrupts();
  currentPulseCountSnapshot = pulseCount;
  interrupts();

  if (currentPulseCountSnapshot == localPulseCountCopy) return;

  unsigned long deltaPulses = currentPulseCountSnapshot - localPulseCountCopy;
  localPulseCountCopy = currentPulseCountSnapshot;

  float delta_km = (wheelCircum_mm * deltaPulses) / 1000000.0;

  odometer_km += delta_km;
  dataChangedSinceLastSave = true;

  if (tripRunning && activeTripIndex < MAX_TRIPS) {
      Trip &t = trips[activeTripIndex];
      t.distance_km += delta_km;
      if (instantSpeed > t.maxSpeed) {
          t.maxSpeed = instantSpeed;
      }
      if (instantSpeed > 0.1f && (instantSpeed < t.minSpeed || t.minSpeed > 999.0f) ) {
          t.minSpeed = instantSpeed;
      }
  }
}

/*************************************************************************
 * 6. handleStandby() - Gestionează intrarea și ieșirea din modul standby al LCD-ului.
*************************************************************************/
void handleStandby() {
  static bool sleeping = false;
  if (!sleeping && (millis() - lastMovementMillis > standbyTimeout)) {
    lcd.noDisplay();
    sleeping = true;
    Serial.println(F("Intrat in standby"));
  }
  else if (sleeping && newPulseDetected) { // Iese din standby la detectarea unui nou impuls
    lcd.display();
    sleeping = false;
    page = 255; // Forțează redesenarea paginii
    Serial.println(F("Iesit din standby"));
  }
}

/*************************************************************************
 * 7. drawNormalPages() - Desenează paginile de informații în modul NORMAL.
 *   Optimizată pentru a redesena doar părțile ecranului care s-au schimbat.
*************************************************************************/
void drawNormalPages(byte newPage) {
  static byte lastPageDrawn = 255;
  static float prevAvgSpeed = -1.0f, prevRpm = -1.0f;
  static uint8_t prevHour = 255, prevMinute = 255, prevDay = 0;
  static float prevOdo = -1.0f;
  static unsigned long prevRideDays = 9999;

  if (newPage == 255 || newPage != lastPageDrawn) {
    lcd.clear();
    lastPageDrawn = (newPage == 255) ? page : newPage;
    prevAvgSpeed = -1.0f; prevRpm = -1.0f;
    prevHour = 255; prevMinute = 255; prevDay = 0;
    prevOdo = -1.0f; prevRideDays = 9999;
  }

  switch(lastPageDrawn) {
    case 0: {
      float currentAvgSpeed = calculateAverageSpeed();
      float currentRpmVal = rpm;
      if (abs(currentAvgSpeed - prevAvgSpeed) > 0.05 || prevAvgSpeed == -1.0f) {
        lcd.setCursor(0, 0); lcd.print(F("spd:")); printFloatFix(currentAvgSpeed, 5, 1); lcd.print(F("km/h "));
        prevAvgSpeed = currentAvgSpeed;
      }
      if (abs(currentRpmVal - prevRpm) > 0.5 || prevRpm == -1.0f) {
        lcd.setCursor(0, 1); lcd.print(F("rpm:")); printFloatFix(currentRpmVal, 4, 0); lcd.print(F("       "));
        prevRpm = currentRpmVal;
      }
      break;
    }
    case 1: {
      RtcDateTime now = rtc.GetDateTime();
      char buf[17];
      if (now.Minute() != prevMinute || now.Hour() != prevHour || prevHour == 255) {
        lcd.setCursor(0, 0); snprintf_P(buf, sizeof(buf), PSTR("ora: %02u:%02u    "), now.Hour(), now.Minute()); lcd.print(buf);
        prevHour = now.Hour(); prevMinute = now.Minute();
      }
      if (now.Day() != prevDay || prevDay == 0) {
        lcd.setCursor(0, 1); snprintf_P(buf, sizeof(buf), PSTR("data:%02u/%02u/%04u"), now.Day(), now.Month(), now.Year()); lcd.print(buf);
        prevDay = now.Day();
      }
    } break;
    case 2: {
      float currentOdo = odometer_km;
      unsigned long currentRideDays = getTotalRideTimeDays();
      if (abs(currentOdo - prevOdo) > 0.05 || prevOdo == -1.0f) {
        lcd.setCursor(0, 0); lcd.print(F("odo:")); printFloatFix(currentOdo, 7, 1); lcd.print(F("km  "));
        prevOdo = currentOdo;
      }
      if (currentRideDays != prevRideDays || prevRideDays == 9999) {
        lcd.setCursor(0, 1); lcd.print(F("timp:")); lcd.print(currentRideDays); lcd.print(F("zile   "));
        prevRideDays = currentRideDays;
      }
      break;
    }
    case 3: {
      lcd.setCursor(0,0); lcd.print(F(">Setari     OK"));
      lcd.setCursor(0,1); lcd.print(F("U/D Nav, OK Sel"));
    } break;
  }
}

/*************************************************************************
 * 8. runSettingsMenu() - Gestionează logica meniului de setări.
*************************************************************************/
void runSettingsMenu() {
  byte subMenuIndex = 0;
  bool up, down, mode, ok;
  byte lastSubMenuDrawn = 255;
  bool exitMenu = false;

  while (!exitMenu) {
    if (subMenuIndex != lastSubMenuDrawn) {
      lcd.clear(); lcd.setCursor(0,0);
      switch (subMenuIndex) {
        case 0: lcd.print(F("Diam. rotii")); break;
        case 1: lcd.print(F("Seteaza ora")); break;
        case 2: lcd.print(F("Seteaza data")); break;
        case 3: lcd.print(F("RESET TOTAL")); break;
        case 4: lcd.print(F("Export TureCSV")); break;
      }
      lcd.setCursor(0, 1); lcd.print(F("U/D Nav OK M-Exit"));
      lastSubMenuDrawn = subMenuIndex;
    }

    bool buttonPressed = false;
    do {
      buttonPressed = readButtons(up, down, mode, ok);
    } while (!buttonPressed && !exitMenu);

    if (ok) {
      if      (subMenuIndex == 0) setWheelDiameter();
      else if (subMenuIndex == 1) setClockTime();
      else if (subMenuIndex == 2) setClockDate();
      else if (subMenuIndex == 3) factoryReset();
      else if (subMenuIndex == 4) exportTripsToSerial(false);
      lastSubMenuDrawn = 255;
    } else if (up) {
      subMenuIndex = (subMenuIndex == 0) ? 4 : subMenuIndex - 1;
    } else if (down) {
      subMenuIndex = (subMenuIndex + 1) % 5;
    } else if (mode) {
      exitMenu = true;
    }
  }
  page = 0;
}

/*************************************************************************
 * 9. startStopTrip() & confirmStartTrip() - Gestionează pornirea/oprirea unei ture.
*************************************************************************/
void startStopTrip(bool start) {
  if (start) {
    if (tripRunning) return;
    tripRunning = true;
    activeTripIndex = (activeTripIndex + 1) % MAX_TRIPS;
    Trip &t = trips[activeTripIndex];
    t = {}; t.minSpeed = 999.9f; t.maxSpeed = 0.0f;
    t.stamp = rtc.GetDateTime();
    t.tripStartTimeMillis = millis();
  } else {
    if (!tripRunning || activeTripIndex >= MAX_TRIPS) return;
    tripRunning = false;
    Trip &t = trips[activeTripIndex];
    t.duration_s = (millis() - t.tripStartTimeMillis) / 1000UL;
    if (t.duration_s > 0 && t.distance_km > 0.001f) {
      t.avgSpeed = (t.distance_km / (float)t.duration_s) * 3600.0f;
    } else {
      t.avgSpeed = 0.0f;
    }
    if (t.minSpeed > 999.0f) t.minSpeed = 0.0f;
    if (t.distance_km < 0.001f) {
        t.maxSpeed = 0.0f; t.minSpeed = 0.0f;
    }
    saveTripToEEPROM(activeTripIndex);
  }
}

bool confirmStartTrip() {
  lcd.clear(); lcd.print(F("Pornesc tura?"));
  lcd.setCursor(0,1); lcd.print(F("DA(A2) NU(A0)"));
  bool up,down,mode_btn,ok_btn;
  while(true){
    if (readButtons(up,down,mode_btn,ok_btn)) {
        if(up){
          startStopTrip(true);
          lcd.clear(); lcd.print(F("Tura START!")); delay(1000); return true;
        }
        if(down){
          lcd.clear(); lcd.print(F("Anulat")); delay(700); return false;
        }
    }
  }
}

/*************************************************************************
 * 10. confirmStartLock() - Gestionează confirmarea și setarea PIN-ului pentru modul LOCK.
*************************************************************************/
bool confirmStartLock() {
  lcd.clear(); lcd.print(F("Blochez bicla?"));
  lcd.setCursor(0,1); lcd.print(F("DA(A2) NU(A0)"));
  bool up,down,mode_btn,ok_btn;
  while(true){
    if(readButtons(up,down,mode_btn,ok_btn)){
        if(up) break;
        if(down) return false;
    }
  }

  lcd.clear(); lcd.print(F("PIN:    (A0-A3)"));
  pinIndex=0;
  byte displayPinCursor = 5;
  for(byte i=0; i<4; ++i) lockPin[i] = 0;

  while(pinIndex < 4){
    bool btn_up_local, btn_down_local, btn_mode_local, btn_ok_local;
    if(readButtons(btn_up_local, btn_down_local, btn_mode_local, btn_ok_local)){
        BtnCode b = (BtnCode)255;
        if(btn_down_local) b = CODE_DN; else if(btn_mode_local) b = CODE_MD;
        else if(btn_up_local) b = CODE_UP;   else if(btn_ok_local) b = CODE_OK;
        if(b != 255) {
            lockPin[pinIndex] = b;
            lcd.setCursor(displayPinCursor + pinIndex, 0); lcd.print('*');
            pinIndex++;
            tone(BUZZER_PIN, 3000, 50);
        }
    }
  }
  pinIndex = 0;
  lockArmed = true; alarmActive = false; attemptsLeft = 3;
  lockArmedTime = millis();
  lcd.clear(); lcd.print(F("BLOCAT!")); delay(800);
  return true;
}

/*************************************************************************
 * 11. lockModeRunner() - Gestionează logica modului LOCK.
 *    MODIFICAT pentru a permite oprirea alarmei prin PIN.
*************************************************************************/
void lockModeRunner(bool up, bool down, bool mode_btn_val, bool ok, bool wasButtonPressed) {
  static bool redrawScreen = true;
  static byte currentPinDisplayIndex = 0;

  if (alarmActive) {
    if (millis() % 1000 < 500) { tone(BUZZER_PIN, 2000, 450); }
    else { noTone(BUZZER_PIN); }
  } else { noTone(BUZZER_PIN); }

  if (redrawScreen) {
    lcd.clear();
    if (alarmActive) { lcd.print(F("!!! ALARMA !!!")); }
    else if (lockArmed) { lcd.print(F("BLOCAT")); }
    else { lcd.print(F("DEBLOCARE...")); }

    lcd.setCursor(0, 1);
    if (!lockArmed) { lcd.print(F("-> Mod Normal")); }
    else {
        lcd.print(F("PIN: "));
        for (byte i = 0; i < 4; ++i) {
            lcd.setCursor(5 + i, 1);
            if (i < currentPinDisplayIndex) { lcd.print('*'); }
            else { lcd.print(' '); }
        }
    }
    redrawScreen = false;
  }

  if (!lockArmed) {
    noTone(BUZZER_PIN); currentMode = MODE_NORMAL; page = 0;
    pinIndex = 0; currentPinDisplayIndex = 0; redrawScreen = true;
    return;
  }

  if (wasButtonPressed && lockArmed) {
    BtnCode b = (BtnCode)255;
    if (down) b = CODE_DN; else if (mode_btn_val) b = CODE_MD;
    else if (up) b = CODE_UP; else if (ok) b = CODE_OK;

    if (b != 255 && pinIndex < 4) {
      lcd.setCursor(5 + pinIndex, 1); lcd.print('*');
      currentPinDisplayIndex = pinIndex + 1;
      tone(BUZZER_PIN, 3000, 50);

      if (b == lockPin[pinIndex]) {
        pinIndex++;
        if (pinIndex == 4) {
          lockArmed = false; alarmActive = false; noTone(BUZZER_PIN);
          lcd.clear(); lcd.print(F("Deblocat!")); delay(1000);
          redrawScreen = true; return;
        }
      } else {
        pinIndex = 0; currentPinDisplayIndex = 0;
        if (!alarmActive) {
          attemptsLeft--; lcd.clear();
          if (attemptsLeft > 0) {
            lcd.print(F("GRESIT! Ramas:")); lcd.setCursor(0, 1);
            lcd.print(attemptsLeft); lcd.print(F(" incercari"));
            tone(BUZZER_PIN, 500, 300); delay(300); tone(BUZZER_PIN, 500, 300);
          } else {
            lcd.print(F("!!! ALARMA !!!")); lcd.setCursor(0, 1);
            lcd.print(F("Sistem blocat.")); alarmActive = true;
          }
        } else {
          lcd.clear(); lcd.print(F("!!! ALARMA !!!"));
          lcd.setCursor(0,1); lcd.print(F("PIN GRESIT"));
        }
        delay(1500); redrawScreen = true;
      }
    }
  }
}

/*************************************************************************
 * 12. tripModeRunner() - Gestionează afișarea datelor în modul TURĂ.
*************************************************************************/
void tripModeRunner(bool up, bool down, bool ok_btn, bool newButtonPress) {
  static byte tripPageDisplay = 0;
  static byte lastTripPageDrawn = 255;
  if (activeTripIndex >= MAX_TRIPS) return;
  Trip &t = trips[activeTripIndex];

  static float lastAvgSpeedTrip = -1.0f, lastRpmTrip = -1.0f;
  static float lastDistanceTrip = -1.0f, lastMaxSpeedTrip = -1.0f, lastMinSpeedTrip = -1.0f;
  static unsigned long lastDurationSecsTrip = 999999;
  static RtcDateTime lastClockTimeTrip;

  if (tripRunning) {
      t.duration_s = (millis() - t.tripStartTimeMillis) / 1000UL;
  }

  bool pageChangeRequest = false;
  if (newButtonPress) {
    if (up)   { tripPageDisplay = (tripPageDisplay == 0) ? 3 : tripPageDisplay - 1; pageChangeRequest = true; }
    if (down) { tripPageDisplay = (tripPageDisplay + 1) % 4; pageChangeRequest = true; }
  }

  if (pageChangeRequest || tripPageDisplay != lastTripPageDrawn) {
    lcd.clear(); lastTripPageDrawn = tripPageDisplay;
    lastAvgSpeedTrip = -1.0f; lastRpmTrip = -1.0f;
    lastDistanceTrip = -1.0f; lastDurationSecsTrip = 999999;
    lastMaxSpeedTrip = -1.0f; lastMinSpeedTrip = -1.0f;
    lastClockTimeTrip = RtcDateTime(0);
  }

  float currentAvgSpeedVal = calculateAverageSpeed();
  float currentRpmValTrip = rpm;
  float currentDistanceVal = t.distance_km;
  unsigned long currentDurationSecsVal = t.duration_s;
  char timeBuf[10]; // Buffer pentru formatarea timpului

  switch (tripPageDisplay) {
    case 0: // Viteză și RPM
      if (abs(currentAvgSpeedVal - lastAvgSpeedTrip) > 0.05f || lastAvgSpeedTrip == -1.0f) {
        lcd.setCursor(0, 0); lcd.print(F("spd:")); printFloatFix(currentAvgSpeedVal, 5, 1); lcd.print(F("km/h "));
        lastAvgSpeedTrip = currentAvgSpeedVal;
      }
      if (abs(currentRpmValTrip - lastRpmTrip) > 0.5f || lastRpmTrip == -1.0f) {
        lcd.setCursor(0, 1); lcd.print(F("rpm:")); printFloatFix(currentRpmValTrip, 4, 0); lcd.print(F("       "));
        lastRpmTrip = currentRpmValTrip;
      }
      break;
    case 1: // Distanță și Timp
      if (abs(currentDistanceVal - lastDistanceTrip) > 0.01f || lastDistanceTrip == -1.0f) {
        lcd.setCursor(0, 0); lcd.print(F("Dist:")); printFloatFix(currentDistanceVal, 6, 2); lcd.print(F("km "));
        lastDistanceTrip = currentDistanceVal;
      }
      if (currentDurationSecsVal != lastDurationSecsTrip || lastDurationSecsTrip == 999999) {
        lcd.setCursor(0, 1);
        unsigned int hours = currentDurationSecsVal / 3600;
        unsigned int minutes = (currentDurationSecsVal % 3600) / 60;
        unsigned int seconds = currentDurationSecsVal % 60;
        if (hours > 0) snprintf_P(timeBuf, sizeof(timeBuf), PSTR("%u:%02u:%02u"), hours, minutes, seconds);
        else snprintf_P(timeBuf, sizeof(timeBuf), PSTR("%02u:%02u"), minutes, seconds);
        lcd.print(F("Timp:")); lcd.print(timeBuf); lcd.print(F("   "));
        lastDurationSecsTrip = currentDurationSecsVal;
      }
      break;
    case 2: { // Viteză Max și Min
        float currentMaxSpeed = t.maxSpeed;
        float currentMinSpeed = (t.minSpeed > 999.0f && t.distance_km < 0.001f) ? 0.0f : t.minSpeed;
        if(abs(currentMaxSpeed - lastMaxSpeedTrip) > 0.05f || lastMaxSpeedTrip == -1.0f){
            lcd.setCursor(0,0); lcd.print(F("MaxSp:")); printFloatFix(currentMaxSpeed, 5, 1); lcd.print(F("km/h"));
            lastMaxSpeedTrip = currentMaxSpeed;
        }
        if(abs(currentMinSpeed - lastMinSpeedTrip) > 0.05f || lastMinSpeedTrip == -1.0f){
            lcd.setCursor(0,1); lcd.print(F("MinSp:")); printFloatFix(currentMinSpeed, 5, 1); lcd.print(F("km/h"));
            lastMinSpeedTrip = currentMinSpeed;
        }
    } break;
    case 3: { // Oră și Dată curente
      RtcDateTime now = rtc.GetDateTime();
      char buf[17];
      if (now.Minute() != lastClockTimeTrip.Minute() || now.Hour() != lastClockTimeTrip.Hour() || lastClockTimeTrip.Year()==0) {
        lcd.setCursor(0, 0); snprintf_P(buf, sizeof(buf), PSTR("Ora: %02u:%02u:%02u  "), now.Hour(), now.Minute(), now.Second()); lcd.print(buf);
      }
      if (now.Day() != lastClockTimeTrip.Day() || lastClockTimeTrip.Year()==0) {
        lcd.setCursor(0, 1); snprintf_P(buf, sizeof(buf), PSTR("Data:%02u/%02u/%02u "), now.Day(), now.Month(), now.Year()%100); lcd.print(buf);
      }
      if(lastClockTimeTrip.Year()==0 || now.Minute() != lastClockTimeTrip.Minute()) lastClockTimeTrip = now;
    } break;
  }

  if (ok_btn && newButtonPress) {
    if (tripRunning) {
        lcd.clear(); lcd.print(F("Opreste tura?"));
        lcd.setCursor(0, 1); lcd.print(F("DA(A2) NU(A0)"));
        bool up2, down2, mode2, ok2;
        while (true) {
          if (readButtons(up2, down2, mode2, ok2)) {
            if (up2) {
                startStopTrip(false); currentMode = MODE_NORMAL; page = 0;
                lcd.clear(); lcd.print(F("Tura salvata!")); delay(1000);
                lastTripPageDrawn = 255; return;
            }
            if (down2) { lastTripPageDrawn = 255; return; }
          }
        }
    } else {
        currentMode = MODE_NORMAL; page = 0; lastTripPageDrawn = 255; return;
    }
  }
}

/*************************************************************************
 * 13. loop() - Bucla principală a programului, rulează continuu.
*************************************************************************/
void loop() {
  static Mode pendingMode = MODE_NORMAL;
  static bool selectingMode = false;
  static Mode lastShownModeInSelection = MODE_NORMAL;
  static bool firstDrawSelect = true;
  static unsigned long lastActiveTimeUpdate = 0;

  unsigned long currentTime = millis();

  // --- Procesare comenzi de la PC ---
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.equals(CMD_EXPORT_TRIPS_NOW_STR)) {
        Serial.println(F("INFO: Comanda de export primita de la PC."));
        exportTripsToSerial(true);
    } else {
        Serial.print(F("INFO: Comanda necunoscuta primita: ")); Serial.println(command);
    }
  }

  // --- Procesare impulsuri senzor Hall ---
  if (newPulseDetected) {
    noInterrupts(); newPulseDetected = false; interrupts(); // Resetează flag-ul atomic
    lastMovementMillis = currentTime;
    dataChangedSinceLastSave = true;

    unsigned long currentPulseTimeMicros;
    noInterrupts(); currentPulseTimeMicros = lastPulseMicros; interrupts();

    static unsigned long previousPulseMicrosForCalc = 0;
    unsigned long dT_micros = currentPulseTimeMicros - previousPulseMicrosForCalc;

    if (previousPulseMicrosForCalc > 0 && dT_micros > 5000) { // dT_micros > 5ms (evită împărțirea la zero / RPM extrem)
      instantSpeed = (wheelCircum_mm * 3600.0) / dT_micros; // km/h
      rpm = 60000000.0 / dT_micros;                        // Rotații pe minut
    }
    previousPulseMicrosForCalc = currentPulseTimeMicros;

    rollingSpeedBuffer[rollingIdx] = instantSpeed;
    rollingIdx = (rollingIdx + 1) % 10; // Buffer circular
    updateOdoAndTripData();
  }

  // --- Resetare viteză/RPM la 0 dacă nu se detectează mișcare pentru un interval ---
  if (currentTime - lastMovementMillis > 3000 && (instantSpeed > 0.05 || rpm > 0.05)) {
    instantSpeed = 0; rpm = 0;
    for(byte i=0; i<10; ++i) rollingSpeedBuffer[i] = 0; // Golește și bufferul rolling
    rollingIdx = 0;
    dataChangedSinceLastSave = true;
  }

  // --- Actualizare timp total de pedalare activă ---
  if (instantSpeed > 0.1f) { // Consideră mișcare activă
    if (lastActiveTimeUpdate > 0 && currentTime > lastActiveTimeUpdate) {
        totalActiveRideMillis += (currentTime - lastActiveTimeUpdate);
    }
    lastActiveTimeUpdate = currentTime;
    dataChangedSinceLastSave = true;
  } else {
      lastActiveTimeUpdate = 0; // Resetează dacă nu e mișcare, pentru a calcula corect la următorul start
  }

  handleStandby(); // Gestionează modul standby al LCD-ului

  // --- Citire butoane ---
  bool up_btn = false, down_btn = false, mode_btn_press = false, ok_btn = false;
  bool newButtonPress = readButtons(up_btn, down_btn, mode_btn_press, ok_btn);

  // --- Logică alarmă (se activează dacă e mișcare după perioada de grație) ---
  if (currentMode == MODE_LOCK && lockArmed && !alarmActive) {
    if (currentTime - lockArmedTime > lockGracePeriodMillis) {
        if (lastMovementMillis > (lockArmedTime + lockGracePeriodMillis)) { // Mișcare detectată DUPĂ armare + grație
            alarmActive = true;
            // redrawScreen din lockModeRunner va fi setat la true data viitoare
        }
    }
  }

  // --- Selectare Mod de Operare ---
  if (currentMode == MODE_NORMAL && !selectingMode && mode_btn_press && newButtonPress) {
      selectingMode = true; pendingMode = MODE_NORMAL;
      firstDrawSelect = true; lastShownModeInSelection = MODE_NORMAL;
  }

  if (selectingMode) {
    if (mode_btn_press && newButtonPress) {
      pendingMode = static_cast<Mode>((pendingMode + 1) % 3); // Ciclează prin moduri
      firstDrawSelect = true; // Forțează redesenarea numelui modului
    }

    if (firstDrawSelect || pendingMode != lastShownModeInSelection) {
      lcd.clear(); lcd.print(F("Mod?: ")); lcd.setCursor(6,0);
      switch(pendingMode) {
          case MODE_NORMAL: lcd.print(F("NORMAL  ")); break;
          case MODE_TRIP:   lcd.print(F("TURA    ")); break;
          case MODE_LOCK:   lcd.print(F("BLOCARE ")); break;
      }
      lcd.setCursor(0, 1); lcd.print(F("OK Sel, Mod Next"));
      lastShownModeInSelection = pendingMode; firstDrawSelect = false;
    }

    if (ok_btn && newButtonPress) { // Confirma selecția modului
      bool selectionSuccessful = true;
      if (pendingMode == MODE_TRIP) {
        if (!tripRunning && !confirmStartTrip()) { selectionSuccessful = false; }
      } else if (pendingMode == MODE_LOCK) {
        if (!confirmStartLock()) { selectionSuccessful = false; }
      }

      if (selectionSuccessful) { currentMode = pendingMode; }
      selectingMode = false; page = 0; // Resetează pagina pentru noul mod
      if (currentMode == MODE_NORMAL) drawNormalPages(255); // Redesenează pagina modului normal
      // Pentru TRIP și LOCK, funcțiile lor `runner` vor gestiona afișajul inițial
    }
    return; // Nu executa logica modului curent cât timp se selectează un nou mod
  }

  // --- Execuție logică specifică modului curent ---
  switch(currentMode){
    case MODE_NORMAL:
      if(ok_btn && page == 3 && newButtonPress) { // Pagina "Setări"
          runSettingsMenu(); page = 0; drawNormalPages(255);
      } else if (newButtonPress) {
          if(up_btn)   page = (page == 0) ? 3 : page - 1;
          if(down_btn) page = (page + 1) % 4; // 4 pagini în modul normal (0-3)
      }
      drawNormalPages(page); // Desenează pagina curentă a modului normal
      break;
    case MODE_TRIP:
      tripModeRunner(up_btn, down_btn, ok_btn, newButtonPress);
      break;
    case MODE_LOCK:
      lockModeRunner(up_btn, down_btn, mode_btn_press, ok_btn, newButtonPress);
      break;
  }

  // --- Salvare automată în EEPROM ---
  bool shouldSave = false;
  if (dataChangedSinceLastSave) {
    // Salvează mai repede dacă bicicleta s-a oprit
    if (instantSpeed < 0.05f && currentTime - lastMovementMillis > eepromSaveIntervalStopped) {
      shouldSave = true;
    }
    // Sau salvează la intervale regulate dacă e în mișcare
    else if (instantSpeed > 0.1f && currentTime - lastEepromSaveMillis > eepromSaveIntervalMoving) {
      shouldSave = true;
    }
  }

  if (shouldSave) {
    saveOdometerDataToEEPROM(); // Salvează odometrul și timpul total
    dataChangedSinceLastSave = false; // Resetează flag-ul
    lastEepromSaveMillis = currentTime; // Actualizează timpul ultimei salvări
    Serial.println(F("Date odometru salvate in EEPROM."));
  }
}

/*************************************************************************
 * 14.  SETĂRI ȘI RESET - Funcții pentru meniul de setări.
*************************************************************************/
// Setează diametrul roții
void setWheelDiameter() {
  float d_edit = wheelDiameterIn;
  float lastDrawnD = -1.0f;
  bool up, down, mode_btn, ok_btn;
  lcd.clear(); lcd.print(F("Diametru roata:"));

  while (true) {
    if (abs(d_edit - lastDrawnD) > 0.01f || lastDrawnD == -1.0f) {
      lcd.setCursor(0, 1); printFloatFix(d_edit, 4, 1); lcd.print(F("\" U/D OK=S M=X"));
      lastDrawnD = d_edit;
    }
    bool buttonPressed = false;
    do { buttonPressed = readButtons(up, down, mode_btn, ok_btn); } while (!buttonPressed);

    if      (up)   { d_edit = min(39.0f, d_edit + 0.5f); } // Limite rezonabile pentru diametru
    else if (down) { d_edit = max(10.0f, d_edit - 0.5f); }
    else if (ok_btn) {
      wheelDiameterIn = d_edit;
      wheelCircum_mm = d_edit * 25.4 * M_PI; // Recalculează circumferința
      saveSettingsToEEPROM(); // Salvează noua valoare
      lcd.clear(); lcd.print(F("Salvat!")); delay(700); return;
    } else if (mode_btn) { return; } // Ieșire fără salvare
  }
}

// Setează ora ceasului RTC
void setClockTime() {
  RtcDateTime dt_edit = rtc.GetDateTime();
  byte h = dt_edit.Hour(), m = dt_edit.Minute();
  byte cursor_pos = 0; // 0 pentru ore, 1 pentru minute
  byte lastH_drawn = 255, lastM_drawn = 255, lastCursor_drawn = 255;
  bool up, down, mode_btn, ok_btn;
  lcd.clear(); lcd.print(F("Seteaza Ora:"));

  while (true) {
    if (h != lastH_drawn || m != lastM_drawn || cursor_pos != lastCursor_drawn || lastH_drawn == 255) {
      lcd.setCursor(0, 1); char buf[17];
      snprintf_P(buf, sizeof(buf), PSTR("%c%02u:%c%02u   OK M"),
               (cursor_pos == 0 ? '>' : ' '), h, (cursor_pos == 1 ? '>' : ' '), m);
      lcd.print(buf);
      lastH_drawn = h; lastM_drawn = m; lastCursor_drawn = cursor_pos;
    }
    bool buttonPressed = false;
    do { buttonPressed = readButtons(up,down,mode_btn,ok_btn); } while (!buttonPressed);

    if      (up)   { if (cursor_pos == 0) h = (h + 1) % 24; else m = (m + 1) % 60; }
    else if (down) { if (cursor_pos == 0) h = (h + 23) % 24; else m = (m + 59) % 60; }
    else if (mode_btn) { cursor_pos = (cursor_pos + 1) % 2; } // Comută între ore și minute
    else if (ok_btn) {
      if (rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(false);
      rtc.SetDateTime(RtcDateTime(dt_edit.Year(), dt_edit.Month(), dt_edit.Day(), h, m, 0)); // Setează doar ora/minutul, secunde la 0
      if (!rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(true);
      lcd.clear(); lcd.print(F("Ora salvata!")); delay(700); return;
    }
  }
}

// Setează data ceasului RTC
void setClockDate() {
  RtcDateTime dt_edit = rtc.GetDateTime();
  uint16_t y_edit = dt_edit.Year();
  byte mo_edit = dt_edit.Month(), d_edit = dt_edit.Day();
  byte cursor_pos = 0; // 0 pentru zi, 1 pentru lună, 2 pentru an
  uint16_t lastY_drawn = 0; byte lastMo_drawn = 0, lastD_drawn = 0, lastCursor_drawn = 255;
  bool up, down, mode_btn, ok_btn;

  // Lambda pentru a calcula numărul de zile dintr-o lună (ține cont de anii bisecți)
  auto daysInMonth = [](byte month_val, uint16_t year_val) {
    if (month_val < 1 || month_val > 12) month_val = 1;
    const byte month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    byte num_days = month_days[month_val - 1];
    if (month_val == 2 && RtcDateTime::IsLeapYear(year_val)) { num_days = 29; }
    return num_days;
  };

  lcd.clear(); lcd.print(F("Seteaza Data:"));
  while (true) {
    byte max_days_current = daysInMonth(mo_edit, y_edit); // Asigură că ziua e validă pentru luna/anul curent
    if (d_edit > max_days_current) d_edit = max_days_current;

    if (d_edit != lastD_drawn || mo_edit != lastMo_drawn || y_edit != lastY_drawn || cursor_pos != lastCursor_drawn || lastD_drawn == 0) {
      lcd.setCursor(0, 1); char buf[17];
      snprintf_P(buf, sizeof(buf), PSTR("%c%02u/%c%02u/%c%04u OKM"),
               (cursor_pos == 0 ? '>' : ' '), d_edit, (cursor_pos == 1 ? '>' : ' '), mo_edit, (cursor_pos == 2 ? '>' : ' '), y_edit);
      lcd.print(buf);
      lastD_drawn = d_edit; lastMo_drawn = mo_edit; lastY_drawn = y_edit; lastCursor_drawn = cursor_pos;
    }
    bool buttonPressed = false;
    do { buttonPressed = readButtons(up,down,mode_btn,ok_btn); } while (!buttonPressed);

    byte max_days = daysInMonth(mo_edit, y_edit);
    if (up) {
      if      (cursor_pos == 0) { d_edit = (d_edit % max_days) + 1; }
      else if (cursor_pos == 1) { mo_edit = (mo_edit % 12) + 1; }
      else                      { y_edit = min((uint16_t)2099, (uint16_t)(y_edit + 1)); }
    } else if (down) {
      if      (cursor_pos == 0) { d_edit = (d_edit == 1 ? max_days : d_edit - 1); }
      else if (cursor_pos == 1) { mo_edit = (mo_edit == 1 ? 12 : mo_edit - 1); }
      else                      { y_edit = max((uint16_t)2023, (uint16_t)(y_edit - 1)); } // Limită minimă an
    } else if (mode_btn) { cursor_pos = (cursor_pos + 1) % 3; } // Comută între zi/lună/an
    else if (ok_btn) {
      if (rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(false);
      rtc.SetDateTime(RtcDateTime(y_edit, mo_edit, d_edit, dt_edit.Hour(), dt_edit.Minute(), dt_edit.Second()));
      if (!rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(true);
      lcd.clear(); lcd.print(F("Data salvata!")); delay(700); return;
    }
  }
}

// Resetează toate setările și datele la valorile din fabrică
void factoryReset() {
  lcd.clear(); lcd.print(F("Sigur RESET?"));
  lcd.setCursor(0,1); lcd.print(F("DA(OK) NU(Mod)"));
  bool up_b, down_b, mode_b, ok_b;
  while(true) {
    if(readButtons(up_b, down_b, mode_b, ok_b)) {
        if (ok_b) break; // Confirmare
        if (mode_b) return; // Anulare
    }
  }

  lcd.clear(); lcd.print(F("RESET IN CURS..."));

  // Invalidează numărul magic pentru a forța o reinițializare completă la următorul loadFromEEPROM (deși aici suprascriem tot)
  EEPROM.put(EEPROM_ADDR_MAGIC_NUM, (uint16_t)0xFFFF);

  // Resetează variabilele globale
  pulseCount = 0; odometer_km = 0.0f; totalActiveRideMillis = 0;
  activeTripIndex = 0; wheelDiameterIn = 26.0f; // Valori default

  // Salvează noile valori default în EEPROM
  EEPROM.put(EEPROM_ADDR_MAGIC_NUM, EEPROM_MAGIC_VALUE);
  EEPROM.put(EEPROM_ADDR_WHEEL_DIAM, wheelDiameterIn);
  EEPROM.put(EEPROM_ADDR_ODOMETER, odometer_km);
  EEPROM.put(EEPROM_ADDR_TOTAL_RIDE_MS, totalActiveRideMillis);
  EEPROM.put(EEPROM_ADDR_ACTIVE_TRIP_IDX, activeTripIndex);

  wheelCircum_mm = wheelDiameterIn * 25.4 * M_PI; // Recalculează circumferința

  // Șterge toate turele din EEPROM și din memorie
  for(int i=0; i < MAX_TRIPS; ++i) {
    trips[i] = {}; // Resetează structura
    EEPROM.put(EEPROM_ADDR_TRIPS_START + i * sizeof(Trip), trips[i]);
  }

  // Resetează RTC-ul la data și ora compilării
  if (rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(false);
  rtc.SetDateTime(RtcDateTime(__DATE__, __TIME__));
  if (!rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(true);

  // Resetează stările modurilor
  lockArmed = false; alarmActive = false;
  tripRunning = false;
  currentMode = MODE_NORMAL; page = 0;

  lcd.clear(); lcd.print(F("RESET EFECTUAT!"));
  delay(1500);
}

/*************************************************************************
 * 15.  FUNCȚII EEPROM și ACCESORI
*************************************************************************/
// Încarcă datele salvate din EEPROM la pornire
void loadFromEEPROM() {
  uint16_t magic;
  EEPROM.get(EEPROM_ADDR_MAGIC_NUM, magic); // Citește numărul magic

  if (magic == EEPROM_MAGIC_VALUE) { // Dacă numărul magic e corect, EEPROM-ul a fost inițializat
    Serial.println(F("EEPROM magic OK. Incarcare date."));
    EEPROM.get(EEPROM_ADDR_WHEEL_DIAM, wheelDiameterIn);
    // Validează diametrul citit, resetează la default dacă e invalid
    if (isnan(wheelDiameterIn) || wheelDiameterIn < 10.0f || wheelDiameterIn > 39.0f) {
        wheelDiameterIn = 26.0f;
    }
    EEPROM.get(EEPROM_ADDR_ODOMETER, odometer_km);
    if (isnan(odometer_km) || odometer_km < 0) odometer_km = 0.0f;

    EEPROM.get(EEPROM_ADDR_TOTAL_RIDE_MS, totalActiveRideMillis);
    EEPROM.get(EEPROM_ADDR_ACTIVE_TRIP_IDX, activeTripIndex);
    if (activeTripIndex >= MAX_TRIPS) activeTripIndex = 0; // Validare index

    // Încarcă toate turele salvate
    for (byte i = 0; i < MAX_TRIPS; ++i) {
      EEPROM.get(EEPROM_ADDR_TRIPS_START + i * sizeof(Trip), trips[i]);
    }
  } else { // Număr magic incorect sau prima rulare
    Serial.println(F("EEPROM magic invalid sau prima rulare. Initializare."));
    // Setează valorile default
    wheelDiameterIn = 26.0f; odometer_km = 0.0f; totalActiveRideMillis = 0; activeTripIndex = 0;

    // Scrie valorile default și noul număr magic în EEPROM
    EEPROM.put(EEPROM_ADDR_MAGIC_NUM, EEPROM_MAGIC_VALUE);
    saveSettingsToEEPROM();
    saveOdometerDataToEEPROM();
    EEPROM.put(EEPROM_ADDR_ACTIVE_TRIP_IDX, activeTripIndex);

    // Inițializează și salvează turele goale
    for (byte i = 0; i < MAX_TRIPS; ++i) {
      trips[i] = {};
      EEPROM.put(EEPROM_ADDR_TRIPS_START + i * sizeof(Trip), trips[i]);
    }
  }
}

// Salvează setările (diametrul roții) în EEPROM
void saveSettingsToEEPROM() {
  EEPROM.put(EEPROM_ADDR_WHEEL_DIAM, wheelDiameterIn);
  Serial.println(F("Setari salvate in EEPROM."));
}

// Salvează datele odometrului și timpul total de pedalare în EEPROM
void saveOdometerDataToEEPROM() {
  EEPROM.put(EEPROM_ADDR_ODOMETER, odometer_km);
  EEPROM.put(EEPROM_ADDR_TOTAL_RIDE_MS, totalActiveRideMillis);
  Serial.println(F("Date odometru salvate in EEPROM."));
}

// Salvează o tură specifică și indexul turei active în EEPROM
void saveTripToEEPROM(byte tripIdx) {
  if (tripIdx < MAX_TRIPS) {
    EEPROM.put(EEPROM_ADDR_TRIPS_START + tripIdx * sizeof(Trip), trips[tripIdx]);
    EEPROM.put(EEPROM_ADDR_ACTIVE_TRIP_IDX, activeTripIndex); // Salvează și indexul ultimei ture active
    Serial.print(F("Tura ")); Serial.print(tripIdx); Serial.println(F(" salvata in EEPROM."));
  }
}

// Returnează timpul total de pedalare în zile
unsigned long getTotalRideTimeDays() {
    if (totalActiveRideMillis == 0) return 0;
    return totalActiveRideMillis / (1000UL * 60UL * 60UL * 24UL); // ms -> sec -> min -> ore -> zile
}

/*************************************************************************
   FUNCȚIE EXPORT TURE PRIN SERIAL (CSV)
   Exportă datele turelor stocate în EEPROM prin portul serial în format CSV.
   Parametru:
     triggeredByPC: true dacă exportul a fost cerut de PC (nu afișează mesaje pe LCD).
**************************************************************************/
void exportTripsToSerial(bool triggeredByPC /*= false*/) {
  if (!triggeredByPC) { // Doar dacă NU e declanșat de PC, afișează pe LCD
    lcd.clear();
    lcd.print(F("Export Ture..."));
    lcd.setCursor(0,1);
    lcd.print(F("Vezi Serial Mon."));
  }

  // Antet CSV
  Serial.println(F("--- EXPORT DATE TURE (CSV) ---")); // Delimitator de început pentru aplicația PC
  Serial.println(F("ID_Tura,VitezaMedie,VitezaMax,VitezaMin,Distanta_km,Durata_s,An,Luna,Zi,Ora,Minut,Secunda"));

  Trip tempTrip; // Variabilă temporară pentru a citi fiecare tură din EEPROM
  for (byte i = 0; i < MAX_TRIPS; ++i) {
    EEPROM.get(EEPROM_ADDR_TRIPS_START + i * sizeof(Trip), tempTrip);

    // Exportă tura doar dacă are date valide (an >= 2023 sau distanță/durată > 0)
    // Acest lucru evită exportul turelor goale sau neinițializate.
    if (tempTrip.stamp.Year() >= 2023 || tempTrip.distance_km > 0.001f || tempTrip.duration_s > 0) {
      Serial.print(i); Serial.print(F(","));
      Serial.print(tempTrip.avgSpeed, 2); Serial.print(F(",")); // Afișează cu 2 zecimale
      Serial.print(tempTrip.maxSpeed, 2); Serial.print(F(","));
      Serial.print(tempTrip.minSpeed, 2); Serial.print(F(","));
      Serial.print(tempTrip.distance_km, 3); Serial.print(F(",")); // Distanța cu 3 zecimale
      Serial.print(tempTrip.duration_s); Serial.print(F(","));
      Serial.print(tempTrip.stamp.Year()); Serial.print(F(","));
      Serial.print(tempTrip.stamp.Month()); Serial.print(F(","));
      Serial.print(tempTrip.stamp.Day()); Serial.print(F(","));
      Serial.print(tempTrip.stamp.Hour()); Serial.print(F(","));
      Serial.print(tempTrip.stamp.Minute()); Serial.print(F(","));
      Serial.println(tempTrip.stamp.Second()); // `println` pentru ultima coloană (adaugă newline)
    }
  }
  Serial.println(F("--- SFARSIT EXPORT ---")); // Delimitator de sfârșit

  if (!triggeredByPC) { // Dacă exportul a fost inițiat local, lasă mesajul pe LCD un timp
    delay(2000);
  }
}