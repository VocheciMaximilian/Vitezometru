Acest proiect a fost realizat în cadrul workshop-ului de prototipare cu Arduino, Builders 101, și implementează un vitezometru pentru bicicletă folosind un Arduino,  
un senzor Hall KY-003, un afișaj LCD 16×2, un RTC DS1302, un buzzer pasiv și butoane pentru interacțiune.  
![Untitled](https://github.com/user-attachments/assets/7267db31-6029-4fea-b97b-37e4a65dbf6c)  
![Untitled](https://github.com/user-attachments/assets/4fe0eddf-4029-42bb-b7c7-87fe03928c86)  
![image](https://github.com/user-attachments/assets/186cafee-f0b7-4b2a-96ed-ac885b091ab4)


Funcționalități principale:  
   - Măsurarea vitezei instantanee și RPM.  
   - Calculul vitezei medii "rolling" (pe ultimele 10 măsurători).  
   - Afișarea orei și datei curente.  
   - Înregistrarea distanței totale (odometru) și a timpului total de pedalare.  
   - Gestionarea și salvarea în EEPROM a până la MAX_TRIPS ture individuale,  
     incluzând viteza medie, maximă, minimă, distanța, durata și timestamp-ul.  
   - Mod de blocare (LOCK) cu PIN pentru a descuraja utilizarea neautorizată.  
   - Meniu de setări pentru: diametrul roții, setarea ceasului/datei, resetare la  
     valorile din fabrică și exportul turelor prin portul serial.  
   - Comunicare serială cu un PC pentru a exporta datele turelor la cerere.  
   - Mod standby pentru economisirea energiei afișajului LCD.  
     
Ce nu am reușit să implementez:  
   - Modul GPS pentru a salva traseul.  
   - Utilizarea unui modul pentru carduri SD pentru a stoca mai multe ture.  
   - Calcularea procentului de memorie ocupată din Arduino pentru a aproxima mai bine câte ture pot să memorez.  
   - Calcularea procentului de baterie pentru a estima timpul de viață.  
   - Utilizarea unui modul Wi-Fi pentru a transmite datele către calculator wireless.  
   - Controlarea luminozității LCD-ului pentru a economisi baterie.  
-------------------------------------------------  
Pini utilizați  

1. AFIȘAJ LCD 16×2  
 Pinii de Alimentare și Contrast:  
  - VSS (Pin 1 LCD):  -> GND (masă) Arduino  
  - VDD (Pin 2 LCD):  -> +5 V Arduino  
  - V0 (Pin 3 LCD):   -> Pin Digital 9 Arduino (LCD_CONTRAST_PIN)  
                          (pentru control PWM al contrastului)  

 Pinii de Control:  
  - RS (Pin 4 LCD):   -> Pin Digital 3 Arduino (LCD_RS)  
  - RW (Pin 5 LCD):   -> GND Arduino (mod doar scriere)  
  - E  (Pin 6 LCD):   -> Pin Digital 8 Arduino (LCD_EN)  

 Pinii de Date (mod 4-bit):  
  - D0–D3 (Pinii 7–10 LCD): neconectați  
  - D4 (Pin 11 LCD):   -> Pin Digital 4 Arduino (LCD_D4)  
  - D5 (Pin 12 LCD):   -> Pin Digital 5 Arduino (LCD_D5)  
  - D6 (Pin 13 LCD):   -> Pin Digital 6 Arduino (LCD_D6)  
  - D7 (Pin 14 LCD):   -> Pin Digital 7 Arduino (LCD_D7)  

 Pinii pentru Iluminare de Fundal (Backlight):  
  - A / LED+ (Pin 15 LCD): -> +5 V Arduino (posibil prin rezistență de 220 Ω–1 kΩ)  
  - K / LED– (Pin 16 LCD): -> GND Arduino  

 Rezumat Conexiuni LCD:  
  - LCD Pin 1 (VSS)    -> GND Arduino  
  - LCD Pin 2 (VDD)    -> +5 V Arduino  
  - LCD Pin 3 (V0)     -> Arduino Pin 9  
  - LCD Pin 4 (RS)     -> Arduino Pin 3  
  - LCD Pin 5 (RW)     -> GND Arduino  
  - LCD Pin 6 (E)      -> Arduino Pin 8  
  - LCD Pin 11 (D4)    -> Arduino Pin 4  
  - LCD Pin 12 (D5)    -> Arduino Pin 5  
  - LCD Pin 13 (D6)    -> Arduino Pin 6  
  - LCD Pin 14 (D7)    -> Arduino Pin 7  
  - LCD Pin 15 (A/LED+)-> +5 V Arduino (cu rezistență dacă e necesar)  
  - LCD Pin 16 (K/LED–)-> GND Arduino  

2. MODUL RTC DS1302 (Real-Time Clock)  
  - VCC (RTC):      -> +5 V Arduino (sau +3,3 V, verifică specificațiile modulului)  
  - GND (RTC):      -> GND Arduino  
  - CLK/SCLK (RTC): -> Pin Digital 10 Arduino (DS1302_CLK)  
  - DAT/IO (RTC):   -> Pin Digital 11 Arduino (DS1302_DAT)  
  - RST/CE (RTC):   -> Pin Digital 12 Arduino (DS1302_RST)  

3. SENZOR HALL  
  - VCC (senzor):   -> +5 V Arduino  
  - GND (senzor):   -> GND Arduino  
  - OUT (senzor):   -> Pin Digital 13 Arduino (HALL_PIN)  
                      (Pinul 13 are INPUT_PULLUP activat în cod)  

4. BUZZER PASIV  
  - Un terminal (+):  -> Pin Digital 2 Arduino (BUZZER_PIN)  
  - Celălalt terminal (–): -> GND Arduino  

5. BUTOANE (PUSH BUTTONS)  
(Toate butoanele au un terminal la pinul Arduino specificat și celălalt terminal la GND Arduino.  
Pinii Arduino sunt configurați cu INPUT_PULLUP.)  

  - Buton UP:  
    - Terminal 1: -> Pin Analogic A2 Arduino (BTN_UP)  
    - Terminal 2: -> GND Arduino  

  - Buton MODE:  
    - Terminal 1: -> Pin Analogic A1 Arduino (BTN_MODE)  
    - Terminal 2: -> GND Arduino  

  - Buton DOWN:  
    - Terminal 1: -> Pin Analogic A0 Arduino (BTN_DOWN)  
    - Terminal 2: -> GND Arduino  

  - Buton OK:  
    - Terminal 1: -> Pin Analogic A3 Arduino (BTN_OK)  
    - Terminal 2: -> GND Arduino  
