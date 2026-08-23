 ////DEV_BOARD_3.07_!!!!!!!!!!!!!!!!!!!!//////////////
//screens.h linia 82     lv_obj_t *qr1;//TYLKO TO!!!!
//wszystkie ui_image_   //TO NIE
     /*.header.always_zero = 0,
       .header.w = 480,
       .header.h = 480,
       .data_size = 460800,
       .header.cf = LV_IMG_CF_TRUE_COLOR,*/
//img_t_map://TO TEŻ NIE
/*const lv_img_dsc_t img_t = {
   .header.always_zero = 0,
  .header.w = 60,
  .header.h = 60,
  .data_size = 7200,
  .header.cf = LV_IMG_CF_TRUE_COLOR,
  .data = img_t_map,
};*/
//wszystkie images lvgl/lvgl.h zmiana na lvgl.h// TO TEŻ NIE
//FlashSize 16MB//
//PartitionScheme 8MB with SPIFFS//
//PSRAM OPI enabled//

#include <Arduino.h>
#include <ESP32Time.h>
#include <esp_sntp.h> 
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <ESP_Panel_Library.h>
#include <PubSubClient.h>
#include <lvgl.h>
#include "lvgl_port_v8.h"
#include "ui.h"
#include "actions.h"
#include "screens.h"
#include <ESP_IOExpander_Library.h>
#include "HWCDC.h"
#include <NetworkClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPUpdateServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cJSON.h>
//#include <string.h>

HWCDC USBSerial;

//////////////////////////////////FLIGHT
#define MAX_TRACKED_PLANES 10 

// Współrzędne dla miejscowości Kryry (43-265, Polska)
const float HOME_LAT = 50.0167f; 
const float HOME_LON = 18.8057f;

// Parametry Twojego ekranu radarowego (Wyświetlacz Waveshare 4" 480x480)
const int SCREEN_CENTER_X = 240;
const int SCREEN_CENTER_Y = 240;
const float MAP_RADIUS_KM = 48.0f; 
const float MAP_RADIUS_PX = 240.0f; 

// SZYBKA MATEMATYKA Z FORUM - STAŁE GEODEZYJNE POLICZONE RAZ DLA DOMU
const float km_per_deg_lat = 111.23f; 
const float km_per_deg_lon = 111.32f * cosf(HOME_LAT * 3.14159265f / 180.0f); 
const float px_per_km = MAP_RADIUS_PX / MAP_RADIUS_KM; 

// STRUKTURA Z PRAWIDŁOWYMI ROZMIARAMI TABLIC ZNAKÓW
struct TrackedPlane {
    char callsign[12]; 
    char type[8];      
    float lat;         
    float lon;         
    int altitude;
    int speed;
    int track;         
    bool active;
    char route[32]; 
    char airline[32];
};

// Globalne zmienne współdzielone bezpiecznie przez Mutex
volatile TrackedPlane trackedPlanesList[MAX_TRACKED_PLANES];
SemaphoreHandle_t flightDataMutex = NULL;

void loadFlightDetails(lv_obj_t* obj) {
    if (obj == NULL) return;

    // 1. Czas na spokojne ukończenie animacji z EEZ Studio (1.2 sekundy)
    for (int i = 0; i < 12; i++) {
        lv_timer_handler();
        delay(100); 
    }

    uintptr_t m = (uintptr_t)lv_obj_get_user_data(obj);
    
    char infoBuffer[384]; 
    char altStr[48]   = "brak";
    char speedStr[48] = "brak";
    char trackStr[32] = "brak";
    char callsignClean[16] = "";
    char typeClean[12] = "---";

    // 2. Pobieramy sygnał wywoławczy i typ pod osłoną Mutexu (zasilane przez adsb.fi)
    if (flightDataMutex != NULL && xSemaphoreTake(flightDataMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        if (trackedPlanesList[m].active) {
            strlcpy(callsignClean, (char*)trackedPlanesList[m].callsign, sizeof(callsignClean));
            strlcpy(typeClean, (char*)trackedPlanesList[m].type, sizeof(typeClean));
        }
        xSemaphoreGive(flightDataMutex);
    }

    String callsignStr = String(callsignClean);
    callsignStr.trim();

    String currentRoute = "Brak danych o trasie";
    String currentAirline = "Nieznana linia / Prywatny";
    
    USBSerial.println("\n--- DEBUG ENRICHMENT: Odpytywanie bazy adsbdb.com ---");
    USBSerial.print("Pobrany z radaru CALLSIGN: '");
    USBSerial.print(callsignStr);
    USBSerial.println("'");

    // 3. ODPOWIEDŹ NA STATUS 200: Zapytanie TLS z wymuszonym SNI (Konstrukcja bezpieczna przed ucinaniem)
    if (WiFi.status() == WL_CONNECTED && callsignStr.length() > 2 && callsignStr != "UNK") {
        WiFiClientSecure routeClient;
        routeClient.setInsecure(); // Pomijamy bazę CA dla oszczędności pamięci operacyjnej RAM
        routeClient.setHandshakeTimeout(4000 / 1000); // Wydłużony czas na uścisk TLS przy jednoczesnym ruchu MQTT

        HTTPClient routeHttp;
        
        // ROZWIĄZANIE: Łączymy bezpieczne człony tekstowe, aby przeglądarka nie sformatowała tego jako linku
        String p1 = "https:";
        String p2 = "//api.";
        String p3 = "adsbdb.com/v0/callsign/";
        String routeUrl = p1 + p2 + p3 + callsignStr;
        
        USBSerial.print("Wysylam zapytanie HTTPS pod adres: ");
        USBSerial.println(routeUrl);
        
        // WYMUSZENIE SNI: Podanie pełnego stringa URL jako jedynego parametru begin()
        routeHttp.begin(routeClient, routeUrl);
        
        // Nagłówki wymagane przez Cloudflare do autoryzacji klienta IoT
        routeHttp.addHeader("Host", "api.adsbdb.com");
        routeHttp.addHeader("User-Agent", "Mozilla/5.0 (ESP32S3; SuplaRadar)");
        routeHttp.addHeader("Accept", "application/json");
        routeHttp.setTimeout(4000); 
        
        int rCode = routeHttp.GET();
        
        USBSerial.print("Kod odpowiedzi HTTP z serwera adsbdb: ");
        USBSerial.println(rCode);
        
        if (rCode == HTTP_CODE_OK) {
            String payload = routeHttp.getString();
            
            USBSerial.println("SUROWA ODPOWIEDZ BAZY ADSDB (PAYLOAD):");
            USBSerial.println(payload);
            USBSerial.println("--- KONIEC PAYLOADU ---");
            
            // --- PARSOWANIE STRUKTURY ZGODNIE Z PAYLOADEM RADARU ---
            cJSON *root = cJSON_Parse(payload.c_str());
            if (root != NULL) {
                cJSON *responseObj = cJSON_GetObjectItem(root, "response");
                if (responseObj != NULL) {
                    
                    // DOPASOWANE: Wejście w obiekt "flightroute"
                    cJSON *flightrouteObj = cJSON_GetObjectItem(responseObj, "flightroute");
                    if (flightrouteObj != NULL) {
                        
                        // A. Odczyt linii: flightroute -> airline -> name
                        cJSON *airlineObj = cJSON_GetObjectItem(flightrouteObj, "airline");
                        if (airlineObj != NULL) {
                            cJSON *nameObj = cJSON_GetObjectItem(airlineObj, "name");
                            if (nameObj != NULL && cJSON_IsString(nameObj)) {
                                currentAirline = String(nameObj->valuestring);
                                currentAirline.trim();
                            }
                        }

                        // B. Odczyt lotnisk: flightroute -> origin/destination -> icao_code
                        cJSON *originObj = cJSON_GetObjectItem(flightrouteObj, "origin");
                        cJSON *destObj = cJSON_GetObjectItem(flightrouteObj, "destination");
                        
                        if (originObj != NULL && destObj != NULL) {
                            cJSON *origIcao = cJSON_GetObjectItem(originObj, "icao_code"); // ZMIANA: icao_code zamiast icao
                            cJSON *destIcao = cJSON_GetObjectItem(destObj, "icao_code");   // ZMIANA: icao_code zamiast icao
                            
                            if (origIcao != NULL && cJSON_IsString(origIcao) && destIcao != NULL && cJSON_IsString(destIcao)) {
                                String fromAir = String(origIcao->valuestring);
                                String toAir = String(destIcao->valuestring);
                                fromAir.trim();
                                toAir.trim();
                                
                                if (fromAir.length() > 0 && toAir.length() > 0) {
                                    currentRoute = fromAir + " -> " + toAir;
                                }
                            }
                        }
                    } else {
                        USBSerial.println("Diagnostyka cJSON: Brak obiektu 'flightroute' dla tej maszyny.");
                    }
                }
                cJSON_Delete(root); // Czyszczenie pamięci sterty RAM
            } else {
                USBSerial.println("Blad: Nie udalo sie sparsowac struktury JSON.");
            }
            
            USBSerial.print("Wynik ostateczny -> Linia: ");
            USBSerial.print(currentAirline);
            USBSerial.print(" | Trasa: ");
            USBSerial.println(currentRoute);
            
        } else {
            USBSerial.print("Brak informacji o trasie dla tego lotu. Powod: ");
            USBSerial.println(routeHttp.errorToString(rCode).c_str());
        }
        routeHttp.end();
    }
    USBSerial.println("--- KONIEC DEBUGA ENRICHMENT ---\n");

    // Zapisujemy pobrane uzupełniające detale do globalnej listy pod osłoną Mutexu
    if (flightDataMutex != NULL && xSemaphoreTake(flightDataMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        if (trackedPlanesList[m].active) {
            strlcpy((char*)trackedPlanesList[m].route, currentRoute.c_str(), sizeof(trackedPlanesList[m].route));
            strlcpy((char*)trackedPlanesList[m].airline, currentAirline.c_str(), sizeof(trackedPlanesList[m].airline));
        }
        xSemaphoreGive(flightDataMutex);
    }

    // 4. GENEROWANIE KOŃCOWEGO TEKSTU FORMATOWANEGO DLA ETYKIETY LVGL (Montserrat 20)
    if (flightDataMutex != NULL && xSemaphoreTake(flightDataMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        if (trackedPlanesList[m].active) {
            if (trackedPlanesList[m].altitude > 0) {
                snprintf(altStr, sizeof(altStr), "%d ft (%d m)", trackedPlanesList[m].altitude, (int)(trackedPlanesList[m].altitude * 0.3048f));
            }
            if (trackedPlanesList[m].speed > 0) {
                snprintf(speedStr, sizeof(speedStr), "%d kt (%d km/h)", trackedPlanesList[m].speed, (int)(trackedPlanesList[m].speed * 1.852f));
            }
            if (trackedPlanesList[m].track >= 0) {
                snprintf(trackStr, sizeof(trackStr), "%d deg", trackedPlanesList[m].track); 
            }

            snprintf(infoBuffer, sizeof(infoBuffer),
                "CALLSIGN: %s\n\n"
                "LINIA: %s\n\n"
                "TRASA: %s\n\n"
                "TYP: %s\n\n"
                "ALT: %s\n\n"
                "GS: %s\n\n"
                "KURS: %s",
                callsignStr.c_str(),
                currentAirline.c_str(),
                currentRoute.c_str(),
                typeClean,
                altStr,
                speedStr,
                trackStr
            );
        } else {
            strlcpy(infoBuffer, "Brak danych lotu.", sizeof(infoBuffer));
        }
        xSemaphoreGive(flightDataMutex);
    }

    // 5. MODYFIKACJA STYLU I AKTUALIZACJA ETYKIETY NA WYŚWIETLACZU WAVESHARE
    if (objects.lbl_11_dane != NULL) {
        lv_obj_set_style_text_font(objects.lbl_11_dane, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(objects.lbl_11_dane, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(objects.lbl_11_dane);
        lv_label_set_text(objects.lbl_11_dane, infoBuffer);
    }
}
void vFlightRadarTask(void * pvParameters) {
    // Zachowujemy pełny promień 26 mil morskich dla Twojej mapy tła 48 km
    String host = "https://opendata.adsb.fi";
    String path = "/api/v3/lat/" + String(HOME_LAT, 4) + "/lon/" + String(HOME_LON, 4) + "/dist/26";
    String url = host + path;

    // Alokacja tablicy na stercie dokładnie na 10 maszyn
    TrackedPlane* tempArray = (TrackedPlane*)malloc(10 * sizeof(TrackedPlane));

    if (tempArray == NULL) {
        USBSerial.println("BLAD FATALNY: Brak pamięci RAM dla radaru samolotów!");
        vTaskDelete(NULL);
        return;
    }

    for(;;) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFiClientSecure client;
            client.setInsecure(); // Pomijamy weryfikację certyfikatu SSL dla stabilności

            HTTPClient http;
            http.begin(client, url);
            
            USBSerial.print("Odpytywanie serwera ADSB.FI (Zasięg 48km): ");
            USBSerial.println(url);
            
            http.setUserAgent("ESP32-S3/SuplaRadarSoyer");
            
            // --- PEWNE ROZWIĄZANIE ---
            // Rezygnujemy z GZIP. Chcemy czysty tekst, aby czytać go bezpiecznie, strumieniowo.
            http.addHeader("Accept", "application/json");

            int httpCode = http.GET();
            
            USBSerial.print("Status odpowiedzi HTTP: ");
            USBSerial.println(httpCode); 
            
            if (httpCode == HTTP_CODE_OK) {
                USBSerial.println("Połączenie udane. Rozpoczynam parsowanie strumieniowe w locie...");
                
                // Tworzymy standardowy dokument ArduinoJson v7.x. 
                // W wersji v7 pamięć rośnie automatycznie w miarę potrzeb.
                JsonDocument doc;
                
                // ZMIANA KRYTYCZNA: Przekazujemy strumień http.getStream() bezpośrednio do parsera.
                // Biblioteka czyta dane prosto z sieci i nie zapycha pamięci RAM mikrokontrolera!
                DeserializationError error = deserializeJson(doc, http.getStream());
                
                if (!error && doc.containsKey("ac")) {
                    JsonArray acArray = doc["ac"].as<JsonArray>();
                    int rawCount = 0;

                    for (JsonObject ac : acArray) {
                        if (rawCount >= 10) break; // Sztywny limit na dokładnie 10 ikon wyświetlacza

                        if (ac.containsKey("lat") && ac.containsKey("lon")) {
                            float acLat = ac["lat"].as<float>();
                            float acLon = ac["lon"].as<float>();
                            
                            // Matematyka odległości (lokalna płaska ziemia) z radarów z projektu Forbota
                            float dx_km = (acLon - HOME_LON) * km_per_deg_lon;
                            float dy_km = (acLat - HOME_LAT) * km_per_deg_lat;
                            float dist_km = sqrtf((dx_km * dx_km) + (dy_km * dy_km));
                            
                            if (dist_km > MAP_RADIUS_KM) continue;

                            tempArray[rawCount].lat = acLat;
                            tempArray[rawCount].lon = acLon;
                            tempArray[rawCount].track = ac.containsKey("track") ? ac["track"].as<int>() : 0;
                            
                            String flight = ac.containsKey("flight") ? ac["flight"].as<String>() : "UNK";
                            flight.trim();
                            strlcpy((char*)tempArray[rawCount].callsign, flight.c_str(), sizeof(tempArray[rawCount].callsign));
                            
                            String type = ac.containsKey("t") ? ac["t"].as<String>() : "---";
                            strlcpy((char*)tempArray[rawCount].type, type.c_str(), sizeof(tempArray[rawCount].type));
                            
                            tempArray[rawCount].altitude = ac.containsKey("alt_baro") ? ac["alt_baro"].as<int>() : 0;
                            tempArray[rawCount].speed = ac.containsKey("gs") ? ac["gs"].as<int>() : 0;
                            tempArray[rawCount].active = true;
                            
                            rawCount++;
                        }
                    }

                    // Bezpieczna synchronizacja i wpisywanie danych pod Mutexem dla wątku ekranu LVGL
                    if (flightDataMutex != NULL && xSemaphoreTake(flightDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        for (int k = 0; k < MAX_TRACKED_PLANES; k++) {
                            if (k < rawCount) {
                                memcpy((void*)&trackedPlanesList[k], &tempArray[k], sizeof(TrackedPlane));
                            } else {
                                trackedPlanesList[k].active = false;
                            }
                        }
                        xSemaphoreGive(flightDataMutex); 
                    }
                    USBSerial.print("Radar Supli zaktualizowany. Samolotów na ekranie: ");
                    USBSerial.println(rawCount);
                } else if (error) {
                    USBSerial.print("Błąd struktury strumienia JSON: ");
                    USBSerial.println(error.c_str());
                }
            } else {
                USBSerial.println("Serwer odrzucił zapytanie (brak statusu HTTP 200)");
            }
            http.end();
        } else {
            USBSerial.println("Zadanie radaru czeka na aktywne połączenie Wi-Fi...");
        }
        vTaskDelay(pdMS_TO_TICKS(8000)); // Pętla odpytywania serwera co 8 sekund
    }
}

void updateFlightRadarUI() {
    // Sprawdzamy czy aktywny jest ekran scr10 z EEZ Studio
    if (lv_scr_act() != objects.scr10) return; 

    // Tablica obiektów PRZYCISKÓW z EEZ Studio
    lv_obj_t* planeButtons[MAX_TRACKED_PLANES] = {
        objects.plane1, objects.plane2, objects.plane3, objects.plane4, objects.plane5,
        objects.plane6, objects.plane7, objects.plane8, objects.plane9, objects.plane10
    };

    // Tablica obiektów OBRAZKÓW z EEZ Studio
    lv_obj_t* planeImages[MAX_TRACKED_PLANES] = {
        objects.plane1_1, objects.plane2_1, objects.plane3_1, objects.plane4_1, objects.plane5_1,
        objects.plane6_1, objects.plane7_1, objects.plane8_1, objects.plane9_1, objects.plane10_1
    };

    // Odczyt danych z tablicy globalnej pod osłoną TYLKO JEDNEGO Mutexu
    if (flightDataMutex != NULL && xSemaphoreTake(flightDataMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        
        for (int m = 0; m < MAX_TRACKED_PLANES; m++) {
            if (planeButtons[m] == NULL) continue;

            if (trackedPlanesList[m].active) {
                float dx_km = (trackedPlanesList[m].lon - HOME_LON) * km_per_deg_lon;
                float dy_km = (trackedPlanesList[m].lat - HOME_LAT) * km_per_deg_lat;
                
                // MAPOWANIE NA PIKSELE: Wyśrodkowanie przycisku samolotu na ekranie
                int targetX = SCREEN_CENTER_X + (int)(dx_km * px_per_km) - 16;
                int targetY = SCREEN_CENTER_Y - (int)(dy_km * px_per_km) - 16; 
                
                // 1. Ustawienie pozycji PRZYCISKU na ekranie radaru
                lv_obj_set_pos(planeButtons[m], targetX, targetY);

                // 2. BEZPOŚREDNIE POBRANIE OBRAZKA Z TABLICY (Zamiast lv_obj_get_child)
                lv_obj_t* planeImage = planeImages[m];

                if (planeImage != NULL) {
                    // Ustawienie punktu obrotu dokładnie na środku ikony 32x32 px
                    lv_img_set_pivot(planeImage, 16, 16);

                    // Przeliczenie kąta dla LVGL v8 (wartość * 10)
                    int lvglAngle = (int)(trackedPlanesList[m].track * 10);
                    
                    // Normalizacja kąta do zakresu 0 - 3600
                    if (lvglAngle < 0) lvglAngle += 3600;
                    lvglAngle = lvglAngle % 3600;

                    // 3. Obracamy tylko wewnętrzny OBRAZEK
                    lv_img_set_angle(planeImage, lvglAngle);
                }

                // Przypisanie ID indeksu dla struktury kliknięć przycisku
                lv_obj_set_user_data(planeButtons[m], (void*)(uintptr_t)m);

                // Pokazanie przycisku wraz z zawartością na ekranie
                lv_obj_clear_flag(planeButtons[m], LV_OBJ_FLAG_HIDDEN);
            } else {
                // Ukrycie całego przycisku, gdy samolot znika z radaru
                lv_obj_add_flag(planeButtons[m], LV_OBJ_FLAG_HIDDEN);
            }
        }
        // Oddanie Mutexu po zakończeniu całej operacji
        xSemaphoreGive(flightDataMutex); 
    }
}
///////////////////////////////////////////////FLIGHTend


const char *host = "esp32";
WebServer httpServer(80);
HTTPUpdateServer httpUpdater;


#define EXAMPLE_CHIP_NAME TCA95xx_8bit
#define EXAMPLE_I2C_NUM (1)
#define EXAMPLE_I2C_SDA_PIN (8)
#define EXAMPLE_I2C_SCL_PIN (9)

#define _EXAMPLE_CHIP_CLASS(name, ...) ESP_IOExpander_##name(__VA_ARGS__)
#define EXAMPLE_CHIP_CLASS(name, ...) _EXAMPLE_CHIP_CLASS(name, ##__VA_ARGS__)

ESP_IOExpander *expander = NULL;

WiFiMulti wifiMulti;
WiFiClientSecure client1;
PubSubClient client(client1);

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  25200          /* Time ESP32 will go to sleep (in seconds) */
String jsonBuffer;
ESP32Time rtc;
String tim;
//String dat;
unsigned long pmillisTime = 0;
unsigned long pmillisEvent = 0;
unsigned long pmillisAlert = 0;
int alert=0;
int setAlert=0;
boolean lPlacState = 0;
boolean lWjazdState = 0;
boolean setMain = false;
boolean updateButton=0;
boolean updating = 0;
boolean switchOn=1;
int pinPozycja=1;
int PINkey =0;
int pinCyfra1 = 8;
int pinCyfra2 = 1;
int pinCyfra3 = 0;
int pinCyfra4 = 7;
int cfr=0;
int alertRestart=0;
int slideShow=0;
const char* mqtt_server = "mqtt73.supla.org";
const char* mqtt_user = "8d0f304814108ff2d4058b86ce19a956";
const char* mqtt_pass = "cQ)8FNphgcI(7TBpkCa1jqZQInDypoWP";
float tempGrunt;
String top1="supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21874/state/temperature";//TemGrunt
String top2="supla/8d0f304814108ff2d4058b86ce19a956/devices/7294/channels/21896/state/value";//wiatr
String top3="supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21880/state/hi";//krancowka wjazd true->zamk
String top4="supla/8d0f304814108ff2d4058b86ce19a956/devices/7014/channels/19840/state/hi";//krancowka garaz true->zamk
String top5="supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21881/state/on";//LAMPY PODJAZD
String top6="supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21877/state/on";//lampy plac
String top7="supla/8d0f304814108ff2d4058b86ce19a956/devices/6861/channels/19337/state/value";//PM2.5
String top8="supla/8d0f304814108ff2d4058b86ce19a956/devices/7653/channels/23086/state/phases/1/power_active";//aktualna moc
String top9="supla/8d0f304814108ff2d4058b86ce19a956/devices/7653/channels/23087/state/value";//produkcja dzienna
String week[7] = {"Niedziela", "Poniedziałek", "Wtorek", "Środa", "Czwartek", "Piątek", "Sobota"};
String month[12] ={
  "Styczeń", "Luty", "Marzec", "Kwiecień", "Maj", "Czerwiec", "Lipiec", "Sierpień", "Wrzesień", "Październik", "Listopad", "Grudzień"};

lv_timer_t * timer;
void my_time(lv_timer_t * ti){  
  LV_UNUSED(ti);
  if (lv_scr_act()==objects.main){
   tim = rtc.getTime();
   lv_label_set_text(objects.lbl_0_time, tim.c_str());
  }
}
 lv_timer_t * timer1;
void screenSaver(lv_timer_t * ti1){
  LV_UNUSED(ti1);
  slideShow++;
  switch(slideShow){
    case 1:
     lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false);
     break;
    case 2:
     lv_scr_load_anim(objects.scr9, LV_SCR_LOAD_ANIM_OUT_TOP, 1000, 100, false);
     break;
    case 3:
     lv_scr_load_anim(objects.scr7, LV_SCR_LOAD_ANIM_OUT_TOP, 1000, 100, false);
     break;
    case 4:
     lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false);
     break;
    case 5:
     lv_scr_load_anim(objects.scr9, LV_SCR_LOAD_ANIM_OUT_TOP, 1000, 100, false);
     break;     
    case 6:
     lv_scr_load_anim(objects.scr8, LV_SCR_LOAD_ANIM_OUT_TOP, 1000, 100, false);
     break;
    case 7:
     lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false);
     break; 
    case 8:
     lv_scr_load_anim(objects.scr9, LV_SCR_LOAD_ANIM_OUT_TOP, 1000, 100, false);
     slideShow=0;
     break;     
  }
}

void setup() {
  USBSerial.begin(9600);
  expander = new EXAMPLE_CHIP_CLASS(EXAMPLE_CHIP_NAME,
                                    (i2c_port_t)EXAMPLE_I2C_NUM, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                    EXAMPLE_I2C_SCL_PIN, EXAMPLE_I2C_SDA_PIN);

  expander->init();
  esp_err_t initStatus = expander->begin();
delay(100);
  if (initStatus == ESP_OK) {
    USBSerial.println("Expander initialized successfully.");
  } else {
    expander = new EXAMPLE_CHIP_CLASS(EXAMPLE_CHIP_NAME,
                                      (i2c_port_t)1, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                      7, 15);
    expander->init();
    expander->begin();
  }
  pinMode(16, OUTPUT);
  digitalWrite(16, LOW);

  USBSerial.println("Original status:");
  expander->printStatus();
  //expander->pinMode(5, OUTPUT);
  //expander->digitalWrite(5, HIGH);
  expander->pinMode(0, OUTPUT);
  expander->digitalWrite(0, LOW);
  expander->pinMode(2, OUTPUT);
  expander->digitalWrite(2, LOW);
  //expander->printStatus();
  delay(200);
  //expander->digitalWrite(5, LOW);
  expander->digitalWrite(2, HIGH);
  expander->digitalWrite(0, HIGH);
  //expander->printStatus();

  
  USBSerial.println("Initialize panel device");
  ESP_Panel *panel = new ESP_Panel();
  
  panel->init();

 #if LVGL_PORT_AVOID_TEAR
  // When avoid tearing function is enabled, configure the RGB bus according to the LVGL configuration
  ESP_PanelBus_RGB *rgb_bus = static_cast<ESP_PanelBus_RGB *>(panel->getLcd()->getBus());
  rgb_bus->configRgbFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
  rgb_bus->configRgbBounceBufferSize(LVGL_PORT_RGB_BOUNCE_BUFFER_SIZE);
 #endif

  panel->begin();
   
  USBSerial.println("Initialize LVGL");
  lvgl_port_init(panel->getLcd(), panel->getTouch());

  USBSerial.println("Create UI");
  /* Lock the mutex due to the LVGL APIs are not thread-safe */
  lvgl_port_lock(-1);
  lvgl_port_unlock();

  wifiConfig();
  timeConfig();
  mqttConfig();
  uiConfig();
 
timer = lv_timer_create(my_time, 1000,  NULL);
timer1 = lv_timer_create(screenSaver, 600000,  NULL);

  updating=0;
 pmillisTime = millis();
 esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
 USBSerial.println("LET'S START...");

   flightDataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(vFlightRadarTask, "FlightTask", 8192, NULL, 3, NULL, 0);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  if(updating){
    httpServer.handleClient();
  }
  dateTime();
  mainScr();
static unsigned long lastRadarUiTime = 0;
if (millis() - lastRadarUiTime > 2000) {
    updateFlightRadarUI();
    lastRadarUiTime = millis();
}
}

void uiConfig(){
  USBSerial.println("UI config...");
  ui_init();
  lv_label_set_text(objects.lbl_0_time, tim.c_str());
  date();
}
void mqttConfig(){
 USBSerial.println("MQTT config...");
 client1.setInsecure();
 client.setServer(mqtt_server, 8883);
 client.setCallback(callback);
}
void timeConfig(){
  USBSerial.println("TIME config...");
  USBSerial.println("..............");
  configTzTime("CET-1CEST-2,M3.5.0/2,M10.5.0/3", "tempus2.gum.gov.pl");//automatyczna zmiana stref czasowych
 // configTime(3600, 3600, "tempus2.gum.gov.pl");//3600, 3600, "tempus2.gum.gov.pl" letni
  delay(100);
  sntp_sync_status_t syncStatus;
  syncStatus = sntp_get_sync_status();
    while (syncStatus != SNTP_SYNC_STATUS_COMPLETED) {
        syncStatus = sntp_get_sync_status();
        delay(100); // Adjust the delay time as per your requirements
    }
  tim = rtc.getTime();
  USBSerial.println(tim);
  USBSerial.println("..............");
}
void wifiConfig(){
  USBSerial.println("WIFI config...");
  wifiMulti.addAP("WWW.MULTI-TELEKOM.PL_23g", "7jimefle");
  wifiMulti.addAP("TP-LINK_B6CF2F", "98316410");
  wifiMulti.addAP("TPMlotekPiwnica", "51526948");
  wifiMulti.addAP("WIFi Claas", "47847812claas");
  wifiMulti.addAP("GUEST", "12345678");
  while(wifiMulti.run() != WL_CONNECTED) {
    USBSerial.println(".");
    delay(500);
  }
}
void mainScr(){
  if(((millis()-pmillisEvent)>60000) && (setMain) && (lv_scr_act()!=objects.scr10)){
    setMain=false;
    lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false);
    lv_timer_reset(timer1);
  }
}

void showAlert(){
  if((setAlert==0)&&((millis()-pmillisAlert)>30000)){
    lv_label_set_text(objects.lbl_0_alert, "");
    setAlert=1;
  }
}
void dateTime(){
  if((millis()-pmillisTime)>2000){
   pmillisTime=millis();
    if((rtc.getHour(true)<1) && (rtc.getMinute()<1)){
     String dat = "          Witam, dzisiaj mamy:  "+ week[rtc.getDayofWeek()] +"   "+ rtc.getDay() +" "+ month[rtc.getMonth()] +"   "+ rtc.getYear() +" roku                    ";
     lv_label_set_text(objects.lbl_0_date, dat.c_str());
    }
    else{
      showAlert();
    }
  }
 }
void date(){
  String dat = "          Witam, dzisiaj mamy:  "+ week[rtc.getDayofWeek()] +"   "+ rtc.getDay() +" "+ month[rtc.getMonth()] +"   "+ rtc.getYear() +" roku                    ";
     lv_label_set_text(objects.lbl_0_date, dat.c_str());
}
void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    USBSerial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      USBSerial.println("connected");
      client.subscribe(top1.c_str());
      client.subscribe(top2.c_str());
      client.subscribe(top3.c_str());
      client.subscribe(top4.c_str());
      client.subscribe(top5.c_str());
      client.subscribe(top6.c_str());
      client.subscribe(top7.c_str());
      client.subscribe(top8.c_str());
      client.subscribe(top9.c_str());
    } else {
      USBSerial.print("failed, rc=");
      USBSerial.print(client.state());
      USBSerial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}
void callback(char* topic, byte* payload, unsigned int length) {
  USBSerial.print("Message arrived [");
  USBSerial.print(topic);
  USBSerial.print("] ");
  for (int i = 0; i < length; i++) {
    USBSerial.print((char)payload[i]);
  }
  USBSerial.println();
 if (strcmp(topic,top1.c_str())==0){
    payload[length] = '\0';
    float temporaryGrunt=atof((char*)payload);
    if(temporaryGrunt > -270){
      tempGrunt=temporaryGrunt;
    }
    lv_label_set_text_fmt(objects.lbl_0_temp, "%.1f °C", tempGrunt);
    if(tempGrunt<-9){
      lv_obj_set_style_bg_color(objects.cont_0_temp, lv_color_hex(0xff1453ff), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if((tempGrunt>-10)&&(tempGrunt<0.1)){
      lv_obj_set_style_bg_color(objects.cont_0_temp, lv_color_hex(0xff2196f3), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if((tempGrunt>0)&&(tempGrunt<16)){
      lv_obj_set_style_bg_color(objects.cont_0_temp, lv_color_hex(0xff04e1b8), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if((tempGrunt>15)&&(tempGrunt<26)){
      lv_obj_set_style_bg_color(objects.cont_0_temp, lv_color_hex(0xff14ff78), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if((tempGrunt>25)&&(tempGrunt<34)){
      lv_obj_set_style_bg_color(objects.cont_0_temp, lv_color_hex(0xffffaf2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if(tempGrunt>33){
      lv_obj_set_style_bg_color(objects.cont_0_temp, lv_color_hex(0xffff4a2e), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
  }//      °
  if (strcmp(topic,top2.c_str())==0){//wind
    payload[length] = '\0';
    float W=atof((char*)payload);
    lv_label_set_text_fmt(objects.lbl_0_wind, "%.0f km/h", W);
    if(W<46){
      lv_obj_set_style_bg_color(objects.cont_0_wind, lv_color_hex(0xff14ff78), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if((W>45)&&(W<61)){
      lv_obj_set_style_bg_color(objects.cont_0_wind, lv_color_hex(0xffF3AA21), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if(W>60){
      lv_obj_set_style_bg_color(objects.cont_0_wind, lv_color_hex(0xfff32124), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
  }
  if (strcmp(topic,top3.c_str())==0){//kranc wjazd
    alertRestart++;
    payload[length] = '\0';
    if(strcmp((char *)payload, "true") == 0){    
     lv_obj_set_style_bg_color(objects.rly_1_brama1, lv_color_hex(0xff14ff78), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_label_set_text(objects.lbl_1_brama1, "ZAMKNIĘTA");
     if(alertRestart>1){
     lv_label_set_text(objects.lbl_0_alert, "! Zamknięto bramę wjazdową !          ");
     }
     setAlert=0;
     pmillisAlert=millis();
    }
    else{
      lv_obj_set_style_bg_color(objects.rly_1_brama1, lv_color_hex(0xfff73906), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_1_brama1, "OTWARTA");
      if(alertRestart>1){
       lv_label_set_text(objects.lbl_0_alert, "! Otwarto bramę wjazdową !          ");
      }
      setAlert=0;
      pmillisAlert=millis();
    }
    showAlert();
  }
  if (strcmp(topic,top4.c_str())==0){//kranc garaz
    if(alertRestart==1){
      lv_label_set_text(objects.lbl_0_alert, "");
    }
    payload[length] = '\0';
    if(strcmp((char *)payload, "true") == 0){  
      lv_obj_set_style_bg_color(objects.rly_1_brama2, lv_color_hex(0xff14ff78), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_1_brama2, "ZAMKNIĘTA");
      if(alertRestart>1){
       lv_label_set_text(objects.lbl_0_alert, "! Zamknięto bramę garażową !          ");
      }
      setAlert=0;
      pmillisAlert=millis();
    }
    else{
      lv_obj_set_style_bg_color(objects.rly_1_brama2, lv_color_hex(0xfff73906), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_1_brama2, "OTWARTA");
      if(alertRestart>1){
      lv_label_set_text(objects.lbl_0_alert, "! Otwarto bramę garażową !          ");
      }
      setAlert=0;
      pmillisAlert=millis();
    }
    showAlert();
  }
  if (strcmp(topic,top5.c_str())==0){//lampy podjazd
    payload[length] = '\0';
    if(strcmp((char *)payload, "false") == 0){
      lWjazdState = 0;
      lv_obj_set_style_bg_color(objects.rly_2_lampa1, lv_color_hex(0xff14ff78), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_2_lampa1, "WYŁĄCZONE");
    }
    else{
      lWjazdState = 1;
      lv_obj_set_style_bg_color(objects.rly_2_lampa1, lv_color_hex(0xfff73906), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_2_lampa1, "WŁĄCZONE");
    }
  }
  if (strcmp(topic,top6.c_str())==0){//lampy plac
    payload[length] = '\0';
    if(strcmp((char *)payload, "false") == 0){
      lPlacState = 0;
      lv_obj_set_style_bg_color(objects.rly_2_lampa2, lv_color_hex(0xff14ff78), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_2_lampa2, "WYŁĄCZONE");
    }
    else{
      lPlacState = 1;
      lv_obj_set_style_bg_color(objects.rly_2_lampa2, lv_color_hex(0xfff73906), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_2_lampa2, "WŁĄCZONE");
    }
  }
  if (strcmp(topic,top7.c_str())==0){//PM2.5
    payload[length] = '\0';
    String ai=String((char*)payload);
    int air = ai.toInt();
    if(air<14) {
      lv_obj_set_style_bg_color(objects.cont_0_air, lv_color_hex(0xff12b43b), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_0_air, "SUPER");
    }
    else if((air>13) && (air<36)){
      lv_obj_set_style_bg_color(objects.cont_0_air, lv_color_hex(0xff14ff78), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_0_air, "OK");
    }
    else if((air>35) && (air<56)){
      lv_obj_set_style_bg_color(objects.cont_0_air, lv_color_hex(0xffd2e110), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_0_air, "TAKA SOBIE");
    }
    else if((air>55) && (air<151)){
      lv_obj_set_style_bg_color(objects.cont_0_air, lv_color_hex(0xfff68e02), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_0_air, "ZŁA");
    }
    else if((air>150) && (air<256)){
      lv_obj_set_style_bg_color(objects.cont_0_air, lv_color_hex(0xffe1102f), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_0_air, "BARDZO ZŁA");
    }
    else if(air>255){
      lv_obj_set_style_bg_color(objects.cont_0_air, lv_color_hex(0xff782222), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.lbl_0_air, "TRAGICZNA");
    }
  }
  if (strcmp(topic,top8.c_str())==0){
    payload[length] = '\0';
    float pvTemp=atof((char*)payload);
    lv_label_set_text_fmt(objects.lbl_9_power, "%.1f Watt", pvTemp);
  }
  if (strcmp(topic,top9.c_str())==0){
    payload[length] = '\0';
    float pvDaily=atof((char*)payload);
    lv_label_set_text_fmt(objects.lbl_9_day, "%.2f kWh", pvDaily);
  }
}
void action_akcja(lv_event_t * e){
  USBSerial.println("EVENT              ..................:");
  pmillisEvent = millis();
  lv_timer_reset(timer1);
  setMain = true;
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    USBSerial.print("GESTURE..................:");
  switch(dir) {
    case LV_DIR_LEFT:
      if (lv_scr_act()==objects.main){
       lv_scr_load_anim(objects.scr9, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr9){
       lv_scr_load_anim(objects.scr1, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr1){
        lv_indev_wait_release(lv_indev_get_act());
       lv_scr_load_anim(objects.scr2, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr2){
        lv_indev_wait_release(lv_indev_get_act());
       lv_scr_load_anim(objects.scr4, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr7){
       lv_scr_load_anim(objects.scr8, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr8){
       lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      break;
    case LV_DIR_RIGHT:
      if (lv_scr_act()==objects.scr4){
       lv_scr_load_anim(objects.scr2, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr2){
       lv_scr_load_anim(objects.scr1, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr1){
       lv_scr_load_anim(objects.scr9, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr9){
       lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr7){
       lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      else if (lv_scr_act()==objects.scr8){
       lv_scr_load_anim(objects.scr7, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);
      }
      break;
    case LV_DIR_TOP:
       lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_TOP, 1000, 100, false);
      break;
    case LV_DIR_BOTTOM:
      lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false);
      break;
  }
  ///////////////////////////////////////flightSTART
       if (obj == objects.btn_0_flight)
    {
      lv_scr_load_anim(objects.scr10, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);
    }
    if (obj == objects.btn_10_back)
    {
      lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OUT_RIGHT, 1000, 100, false);
    }
    if (obj == objects.btn_11_back)
    {
      lv_scr_load_anim(objects.scr10, LV_SCR_LOAD_ANIM_OUT_RIGHT, 1000, 100, false);
    }
if ((obj == objects.plane1)||(obj == objects.plane2)||(obj == objects.plane3)||(obj == objects.plane4)||(obj == objects.plane5)||(obj == objects.plane6)||(obj == objects.plane7)||(obj == objects.plane8)||(obj == objects.plane9)||(obj == objects.plane10))
{
    // 1. Wpisujemy tekst oczekiwania na ekranie scr11
    if (objects.lbl_11_dane != NULL) {
        lv_label_set_text(objects.lbl_11_dane, "Pobieranie danych o locie...");
    }

    // 2. Płynnie i spokojnie przechodzimy na ekran scr11 (Animacja trwa 1000 ms + 100 ms opóźnienia)
    lv_scr_load_anim(objects.scr11, LV_SCR_LOAD_ANIM_OUT_RIGHT, 1000, 100, false);

    // 3. Wywołujemy funkcję, która wewnątrz siebie poczeka na zakończenie tego ruchu
    loadFlightDetails(obj);
}
  ///////////////////////////////////////flightEND
    if (obj == objects.btn_0_next)
    {
      lv_scr_load_anim(objects.scr9, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);
    }
    else if (obj == objects.btn_9_back)
    {
      lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);     
    }
    else if (obj == objects.btn_9_next)
    {
      lv_scr_load_anim(objects.scr1, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);     
    }
    else if (obj == objects.btn_1_back)
    {
      lv_scr_load_anim(objects.scr9, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);     
    }
    else if (obj == objects.btn_1_next)
    {
      lv_scr_load_anim(objects.scr2, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);     
    }
    else if (obj == objects.btn_2_back)
    {
      lv_scr_load_anim(objects.scr1, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);     
    }
    else if (obj == objects.btn_2_next)
    {
      lv_scr_load_anim(objects.scr4, LV_SCR_LOAD_ANIM_OUT_LEFT, 1000, 100, false);     
    }
    else if (obj == objects.btn_3_back)
    {
      lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false);    
    }
    else if (obj == objects.btn_4_back)
    {
      lv_scr_load_anim(objects.scr2, LV_SCR_LOAD_ANIM_OVER_RIGHT, 1000, 100, false);    
    }
    else if (obj == objects.btn_0_sleep)
    {
      lv_scr_load_anim(objects.scr5, LV_SCR_LOAD_ANIM_OVER_TOP, 1000, 100, false);    
    }
    else if (obj == objects.btn_5_sleep)
    {
      esp_deep_sleep_start();   
    }
    else if (obj == objects.btn_7_main)
    {
      lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false);
    }
    else if (obj == objects.btn_8_main)
    {
      lv_scr_load_anim(objects.main, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 1000, 100, false); 
    }
    else if (obj == objects.rly_1_brama1)
    {
       client.publish("supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21878/execute_action", "OPEN_CLOSE");
    } 
    else if (obj == objects.rly_1_brama2)
    {
       client.publish("supla/8d0f304814108ff2d4058b86ce19a956/devices/7014/channels/19835/execute_action", "OPEN_CLOSE");
    } 
    else if (obj == objects.rly_2_lampa1)
    {
      if(!lWjazdState){
       client.publish("supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21881/set/on", "true");
       lWjazdState = 1;
      } 
      else if(lWjazdState){
       client.publish("supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21881/set/on", "false");
       lWjazdState = 0;
      } 
    }
    else if (obj == objects.rly_2_lampa2)
    {
      if(!lPlacState){
       client.publish("supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21877/set/on","true");
       lPlacState = 1;
      } 
      else if(lPlacState){
       client.publish("supla/8d0f304814108ff2d4058b86ce19a956/devices/7293/channels/21877/set/on","false");
       lPlacState = 0;
      } 
    }
    else if (obj == objects.btn_3_update)
    {
     qrcodeLink();
     lv_label_set_recolor(objects.lbl_3_alert, true);
     lv_label_set_text(objects.lbl_3_alert, "HTTPUpdateServer gotowy! Otwórz w przeglądarce #fa0404 http://esp32.local/update# lub #2196f3 zeskanuj kod QR#, następnie, pod 'firmare', w okienku 'wybierz plik' podaj ścieżkę do pliku .bin, kliknij 'update firmware' i poczekaj na załadowanie nowego oprogramowania...");
     lv_label_set_text(objects.lbl_3_update, "UPDATE ready");
     lv_obj_set_style_bg_color(objects.btn_3_update, lv_color_hex(0xfff3a521), LV_PART_MAIN | LV_STATE_DEFAULT);
     
     if (MDNS.begin(host)) {
      Serial.println("mDNS responder started");
     }
    httpUpdater.setup(&httpServer);
    httpServer.begin();
    MDNS.addService("http", "tcp", 80);
    updating=1;
     } 
     else if (obj == objects.btn_3_reset)
     {
       ESP.restart();
       updating=0;
     }
    else if (obj == objects.btn_4_1)
    {
      cfr=1;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_2)
    {
      cfr=2;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_3)
    {
      cfr=3;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_4)
    {
      cfr=4;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_5)
    {
      cfr=5;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_6)
    {
      cfr=6;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_7)
    {
      cfr=7;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_8)
    {
      cfr=8;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_9)
    {
      cfr=9;
      getPin(cfr);
    }
    else if (obj == objects.btn_4_0)
    {
      cfr=0;
      getPin(cfr);
    }
} 

void getPin(int cyfra){
if (pinPozycja == 1) { //Jesli sprawdzamy 1 pozycje PINu
  if (cyfra == pinCyfra1) {
   PINkey++;
  }
 pinPozycja++;
 lv_label_set_text(objects.lbl_4_pin, "* ");
}
else if (pinPozycja == 2) { //Jesli sprawdzamy 2 pozycje PINu
  if (cyfra == pinCyfra2) {
   PINkey++;
  }
 pinPozycja++;
 lv_label_set_text(objects.lbl_4_pin, "* * ");
}
else if (pinPozycja == 3) { //Jesli sprawdzamy 3 pozycje PINu
  if (cyfra == pinCyfra3) {
   PINkey++;
  }
 pinPozycja++;
 lv_label_set_text(objects.lbl_4_pin, "* * * ");
}
else if (pinPozycja == 4) { //Jesli sprawdzamy 4 pozycje PINu
  if (cyfra == pinCyfra4) {
   PINkey++;
  }
  pinPozycja = 1;
  lv_label_set_text(objects.lbl_4_pin, "* * * *");
   if(PINkey==4) {
    lv_scr_load_anim(objects.scr3, LV_SCR_LOAD_ANIM_FADE_ON, 1500, 100, false);
    PINkey=0;
   }  
   else{
    lv_label_set_text(objects.lbl_4_pin, "");
    PINkey=0;
   }
 }      
}
void qrcodeLink(void)
{
  lv_obj_set_pos(objects.btn_3_update, 60, 63);
  lv_obj_t * obj = lv_qrcode_create(objects.scr3, 125, lv_color_hex(0xff3f3a3a), lv_color_hex(0xffcdc8c8));
  objects.qr1 = obj;
  lv_obj_set_pos(obj, 290, 60);
  const char * data = "http://esp32.local/update";
  lv_qrcode_update(obj, data, strlen(data));
  lv_obj_set_style_border_color(obj, lv_color_hex(0xffcdc8c8), 0);
  lv_obj_set_style_border_width(obj, 5, 0);
}
