#pragma once
#include "esphome.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"
#include "esphome/components/number/number.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include <map>
#include <string>
#include <cstring>
#include <cstdio>
#include <esp_system.h>
#include <esp_netif.h>
using namespace esphome;

// Emulazione modulo FC41D come MACCHINA A STATI response-gated.
// INIT -> heartbeat finche' il mainboard risponde.
// HB_OK -> product-info (0x01).
// ONLINE -> stato rete 0x04 (cloud) + query 0x08 (intervallo dello slider) -> report 0x07.
// Legge i 0x07 (una serie completa, batteria compresa, ogni 6-7 s con l'alta frequenza attiva) e li
// spacchetta in sensori. Scrive i datapoint col comando 0x06 (switch/number/select da HA).
class PowerstationModule : public Component, public uart::UARTDevice {
 public:
  PowerstationModule(uart::UARTComponent *parent, time::RealTimeClock *rtc)
      : uart::UARTDevice(parent), rtc_(rtc) {}

  void reg(uint16_t key, sensor::Sensor *s, float scale = 1.0f) { num_[key] = Reg{s, scale}; }
  void reg_sw(uint16_t key, switch_::Switch *s) { sw_[key] = s; }
  void reg_sel(uint16_t key, select::Select *s) { sel_[key] = s; }
  void reg_num(uint16_t key, number::Number *s) { numctl_[key] = s; }
  void set_status_txt(text_sensor::TextSensor *s) { status_txt_ = s; }
  void set_fault_txt(text_sensor::TextSensor *s) { fault_txt_ = s; }
  void set_hfr(uint8_t m) { hfr_mode_ = m; t_hfr_ = millis(); }           // modo HFR scelto dall'utente (lo scrive subito chi chiama: riparto a contare da qui)
  void set_q_ms(uint32_t ms) { if (ms >= 1000 && ms <= 120000) q_ms_ = ms; }  // intervallo query (slider)
  void set_screen_sel(select::Select *s) { screen_sel_ = s; }
  void set_auton_txt(text_sensor::TextSensor *s) { auton_txt_ = s; }
  // ENERGIA (27/11): DP 16 "Power Generation" (pannelli) e DP 17 "Load Power Consumption" (uscita), in kWh.
  // Il mainboard li manda da solo ogni 10 minuti con l'energia di quei 10 minuti (0,00-0,06): qui si sommano
  // in due totali che lo yaml tiene in flash (globali con restore_value), cosi' reggono ai riavvii dell'ESP.
  void set_energia(sensor::Sensor *pv, float *tot_pv, sensor::Sensor *out, float *tot_out) {
    en_pv_ = pv; en_pv_tot_ = tot_pv; en_out_ = out; en_out_tot_ = tot_out;
    if (en_pv_ != nullptr && en_pv_tot_ != nullptr) en_pv_->publish_state(*en_pv_tot_);
    if (en_out_ != nullptr && en_out_tot_ != nullptr) en_out_->publish_state(*en_out_tot_);
  }
  // "Chiedi i dati" (18/11, scelta di Gabriele): spento = l'ESP la lascia IN PACE: niente query 0x08, niente
  // cane da guardia / finto WiFi del watchdog (resta la cura della pausa 0x2B, che senza non finisce);
  // risponde alle sue domande (+ heartbeat) e tiene viva la frequenza alta (DP19, scritta solo quando
  // scade). Tiene solo i rapporti che il mainboard manda da solo (come il modulo vero, che la chiedeva
  // quasi solo all'avvio). Meta' del traffico del mainboard era la risposta alle nostre 0x08 (~1,2 kB).
  void set_chiedi(bool on) { chiedi_ = on; t_q_ = millis(); if (on) { last_wake_ms_ = millis(); hfr_due_ = true; } ESP_LOGW("ps", "chiedi i dati (query 0x08) %s", on ? "ACCESO" : "SPENTO: solo i rapporti del mainboard"); }
  void query_() { if (chiedi_) send_(0x08, nullptr, 0); }
  // REGISTRO SERIALE COMPLETO (interruttore in HA): scrive in esadecimale ogni frame in arrivo e in
  // partenza e ogni byte buttato via (rumore, frame rotti), cosi' si vede TUTTO quello che passa.
  void set_raw_log(bool on) { raw_log_ = on; ESP_LOGW("ps", "registro seriale completo %s", on ? "ACCESO" : "spento"); }
  // stato per la spia: 0 rosso (handshake non fatto), 1 giallo (acceso ma report fermi = freeze),
  // 2 verde (gruppo batteria da meno di 20 s). NB: a station spenta l'ESP e' senza corrente, quindi la spia e' proprio spenta.
  uint8_t link_state() {
    if (st_ == INIT) return 0;
    if (st_ == ONLINE && last_data_ms_ != 0 && (millis() - last_data_ms_) < 20000) return 2;
    return 1;
  }
  // per il sensore "Dati fermi": online? e da quanti ms non arriva il gruppo batteria
  bool online() { return st_ == ONLINE; }
  bool had_report() { return last_data_ms_ != 0; }   // ha gia' ricevuto almeno un rapporto con dati veri (anti falso-positivo al boot)
  uint32_t ms_since_report() { return millis() - last_data_ms_; }   // solo dati veri: gli echi delle scritture non contano

  void setup() { ESP_LOGI("ps", "emulatore modulo FC41D avviato (stato INIT)"); }

  // FINTO WIFI SPENTO E RIACCESO, a comando (pulsante in HA) o dal cane da guardia, in qualunque stato:
  // 0x02 "non connesso" per 3 s (era 10 s fino al 18/11), poi 0x03 "connesso al router", dopo 3 s lo stato vero (cloud) + query.
  void ricollega(const char *perche) {
    if (wifi_sim_ != 0) return;
    ESP_LOGW("ps", "RICOLLEGAMENTO (%s): finto WiFi spento 3s, poi router, poi cloud", perche);
    annuncio_ms_ = 0;   // se c'e' una pausa in corso, a chiuderla sara' il finto WiFi e non l'annuncio
    net_send_(0x02);
    wifi_sim_ = 1; wifi_sim_ms_ = millis();
  }

  // --- scrittura datapoint verso il mainboard (cmd 0x06) ---
  void write_bool(uint16_t id, bool on) {
    uint8_t d[6] = {(uint8_t)(id >> 8), (uint8_t) id, 0x01, 0x00, 0x01, (uint8_t)(on ? 1 : 0)};
    send_(0x06, d, 6);
    ESP_LOGI("ps", "WRITE bool id 0x%04X = %d", id, on);
  }
  void write_val(uint16_t id, int32_t val) {
    uint8_t d[9] = {(uint8_t)(id >> 8), (uint8_t) id, 0x02, 0x00, 0x04,
                    (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t) val};
    send_(0x06, d, 9);
    ESP_LOGI("ps", "WRITE int id 0x%04X = %d", id, val);
  }

  void loop() {
    while (available()) feed_((uint8_t) read());
    uint32_t now = millis();
    // un pezzo di frame rimasto a meta' (seriale muta da 3 s): non arrivera' piu', lo butto.
    // Era 500 ms: il 18/11 10:02 nel blocco grave il mainboard trasmetteva a scatti (~15 byte ogni ~1 s,
    // buchi di ~550 ms DENTRO lo stesso frame) e li buttavamo tutti, comprese le info-prodotto da 155 byte:
    // l'ESP restava per sempre in presentazione. Con 3 s i frame lenti arrivano interi.
    if (len_ != 0 && now - rx_ms_ > 3000) { if (len_ >= 2) rx_bad_++; junk_add_(buf_, len_); len_ = 0; }
    if (junk_n_ && now - rx_ms_ > 500) junk_flush_();
    if (rx_bad_ != 0 && now - rx_log_ms_ > 30000) {
      rx_log_ms_ = now;
      ESP_LOGW("ps", "seriale: %u frame scartati (checksum, lunghezza o frame a meta') negli ultimi 30s", (unsigned) rx_bad_);
      rx_bad_ = 0;
    }
    if (wifi_sim_ == 1 && now - wifi_sim_ms_ >= 3000) {   // dopo 3 s (era 10 s fino al 18/11) WiFi "riacceso": connesso al router
      net_send_(0x03);
      wifi_sim_ = 2; wifi_sim_ms_ = now;
    } else if (wifi_sim_ == 2 && now - wifi_sim_ms_ >= 3000) {         // ...e poi di nuovo il cloud
      net_send_(net_state_);
      query_(); t_q_ = now; hfr_due_ = true;
      if (st_ == HB_OK) send_q_(0x01, nullptr, 0, 0x01);              // presentazione ferma: richiedo le info-prodotto
      wifi_sim_ = 0; last_wake_ms_ = now;
    }
    // una pausa che non si chiude in 5 minuti la lascio al cane da guardia e alla prossima (non resto "in pausa" per sempre)
    if (pausa_ms_ != 0 && now - pausa_ms_ > 300000) {
      ESP_LOGW("ps", "PAUSA NON CHIUSA dopo 5 minuti");
      pausa_ms_ = 0; annuncio_ms_ = 0;
    }
    switch (st_) {
      case INIT:
        if (now - t_hb_ >= 1000) { t_hb_ = now; send_(0x00, nullptr, 0); }
        break;
      case HB_OK:
        // heartbeat a 15s; le info-prodotto (0x01) si chiedono a ogni risposta di heartbeat (vedi handle_),
        // con attesa della conferma e ritrasmissione: esattamente come il modulo vero.
        if (now - t_hb_ >= 15000) { t_hb_ = now; send_(0x00, nullptr, 0); }
        // il mainboard risponde all'heartbeat ma non alle info-prodotto (visto il 12/11 03:22, MCU inceppata):
        // dopo 60 s ferma qui provo il finto WiFi spento e riacceso, al massimo ogni 2 minuti
        if (chiedi_ && now - hb_ok_dal_ms_ > 60000 && now - last_wake_ms_ > 120000) ricollega("presentazione ferma da oltre 60s");
        break;
      case ONLINE: {
        if (online_since_ms_ == 0) online_since_ms_ = now;
        if (now - t_hb_ >= 15000) {
          t_hb_ = now;
          // Come il modulo vero ("long time have not receive HB, reboot"): 5 heartbeat di fila senza risposta
          // ne' report -> riparto da capo come un modulo appena acceso.
          if (hb_miss_ > 4) { restart_("5 heartbeat di fila senza risposta dal mainboard"); break; }
          if (hb_miss_ >= 2) ESP_LOGW("ps", "il mainboard non risponde all'heartbeat da %u invii", (unsigned) hb_miss_);
          hb_miss_++;
          send_(0x00, nullptr, 0);
        }
        // Stato rete (0x03): come il modulo vero lo mando SOLO quando cambia, in coda e con attesa della
        // conferma del mainboard. net_sent_ = 0xFF forza l'invio all'ingresso in ONLINE.
        if (wifi_sim_ == 0 && net_sent_ != net_state_) net_send_(net_state_);
        // CANE DA GUARDIA, come nell'integrazione (finto cloud): conta SOLO i rapporti col gruppo
        // batteria (DP 3; gli echi delle scritture e il solo gruppo della rete no). Se mancano da 25 s
        // (o intervallo query + 15 s, se piu' lungo; a stazione sana il buco massimo e' ~15 s) si interviene, ma solo in un momento di silenzio della seriale (1,5 s
        // senza byte dal mainboard, al massimo 30 s di attesa). L'intervento e' il "ricollegamento"
        // che il mainboard riconosce: finto WiFi spento (0x02) per 3 s, poi 0x03 "connesso al
        // router", dopo 3 s lo stato vero 0x04 + query + HFR. Dopo 3 interventi senza dati:
        // ripartenza completa, come il modulo vero che si riavvia.
        // (Tolti l'11/11-12/11: i cicli rapidi da 2 s, mai serviti, e la riscrittura HFR a orario.)
        if (wifi_sim_ == 0 && chiedi_) {   // con "Chiedi i dati" spento: nessun intervento, la lascio in pace
          uint32_t soglia = q_ms_ + 15000 > 25000 ? q_ms_ + 15000 : 25000;
          uint32_t ref = last_data_ms_ > online_since_ms_ ? last_data_ms_ : online_since_ms_;
          if (ref < last_wake_ms_) ref = last_wake_ms_;           // dopo un intervento si riconta da li'
          if (now - ref > soglia) {
            if (stall_due_ms_ == 0) stall_due_ms_ = now;
            bool quiet = now - rx_ms_ >= 1500;
            if (quiet || now - stall_due_ms_ >= 30000) {
              stall_due_ms_ = 0;
              if (wifi_tries_ >= 3) { restart_("3 ricollegamenti senza dati"); break; }
              wifi_tries_++;
              ESP_LOGW("ps", "WATCHDOG: nessun dato vero da %us (tentativo %u%s)",
                       (unsigned) ((now - ref) / 1000), (unsigned) wifi_tries_, quiet ? "" : ", senza silenzio");
              ricollega("watchdog");
            }
          } else {
            stall_due_ms_ = 0;
          }
        }
        if (wifi_sim_ != 0) break;   // durante il finto WiFi spento: niente HFR ne' query
        // ALTA FREQUENZA (DP 19), come il custode dell'integrazione: si scrive appena in linea o dopo
        // un intervento (hfr_due_), poi SOLO quando un rapporto del mainboard la mostra diversa dalla
        // scelta (di solito lo 0 quando scade, ogni ~30 s), mai piu' spesso di una volta ogni 5 s.
        if (hfr_due_ || (hfr_diverso_ && now - t_hfr_ >= 5000)) {   // anche con "Chiedi i dati" spento: e' la via del modulo vero per i dati veloci
          hfr_due_ = false; hfr_diverso_ = false; t_hfr_ = now;
          write_val(0x0013, hfr_mode_);
        }
        if (now - t_q_ >= q_ms_)   { t_q_ = now; query_(); }          // query, intervallo dallo slider
        break;
      }
    }
    tx_pump_(millis());
  }

 protected:
  enum St { INIT, HB_OK, ONLINE };
  struct Reg { sensor::Sensor *s; float scale; };   // (l'ultimo valore pubblicato lo tiene il sensore: s->state)
  St st_ = INIT;
  time::RealTimeClock *rtc_;
  std::map<uint16_t, Reg> num_;
  std::map<uint16_t, switch_::Switch *> sw_;
  std::map<uint16_t, select::Select *> sel_;
  std::map<uint16_t, number::Number *> numctl_;
  text_sensor::TextSensor *status_txt_ = nullptr;
  text_sensor::TextSensor *fault_txt_ = nullptr;
  text_sensor::TextSensor *auton_txt_ = nullptr;
  select::Select *screen_sel_ = nullptr;
  sensor::Sensor *en_pv_ = nullptr, *en_out_ = nullptr;   // energia pannelli / uscita (totali)
  float *en_pv_tot_ = nullptr, *en_out_tot_ = nullptr;
  uint8_t hfr_mode_ = 1;        // modo HFR mantenuto (scelta utente): 0 Infreq,1 LAN,2 WiFi,3 LAN+WiFi
  uint32_t q_ms_ = 5000;        // intervallo query 0x08 (slider utente)
  uint32_t online_since_ms_ = 0;// da quando siamo in ONLINE (0 = non ancora)
  uint32_t last_wake_ms_ = 0;   // fine dell'ultimo ricollegamento (watchdog, pulsante, pausa) o della ripartenza
  uint32_t t_2b_ms_ = 0;        // ultima domanda 0x2B del mainboard (per riconoscere la pausa dei 40 minuti)
  uint32_t pausa_ms_ = 0;       // inizio della pausa dei 40 minuti in corso (0 = nessuna pausa)
  uint32_t annuncio_ms_ = 0;    // quando ho annunciato 0x03=04 per chiudere la pausa (0 = non ancora)
  uint32_t stall_due_ms_ = 0;   // da quando il watchdog aspetta un momento di silenzio (0 = non aspetta)
  uint32_t hb_ok_dal_ms_ = 0;   // da quando la presentazione aspetta le info-prodotto
  uint8_t net_sent_ = 0xFF;     // ultimo stato rete trasmesso al mainboard (0xFF = ancora nessuno)
  uint32_t last_data_ms_ = 0;   // ultimo report col gruppo batteria (DP 3), per il watchdog di stallo
  uint8_t wifi_sim_ = 0;        // finto WiFi spento/riacceso: 0 fermo, 1 "spento" (0x02), 2 "riacceso" (0x03)
  uint32_t wifi_sim_ms_ = 0;
  uint8_t wifi_tries_ = 0;      // ricollegamenti fatti senza che tornassero i dati
  uint8_t hb_miss_ = 0;         // heartbeat mandati di fila senza risposta ne' report (come il contatore del modulo)
  bool hfr_due_ = true;         // HFR da scrivere subito (appena in linea, dopo ripartenza o intervento del watchdog)
  bool hfr_diverso_ = false;    // un rapporto ha mostrato il DP19 diverso dalla scelta (di solito 0: scaduto)
  bool raw_log_ = false;        // registro seriale completo (vedi set_raw_log)
  bool chiedi_ = true;          // manda la query 0x08 (interruttore "Chiedi i dati")
  uint8_t junk_[64]; uint8_t junk_n_ = 0;   // byte buttati via, in attesa di essere scritti nel registro

  // scrive un blocco di byte in esadecimale, 48 per riga (le righe lunghe il registro le taglia)
  void raw_dump_(const char *dir, const uint8_t *b, size_t n) {
    for (size_t o = 0; o < n; o += 48) {
      size_t k = n - o < 48 ? n - o : 48;
      ESP_LOGI("raw", "%s %u/%u: %s", dir, (unsigned) o, (unsigned) n, format_hex_pretty(b + o, k).c_str());
    }
  }
  void junk_add_(const uint8_t *b, size_t n) {
    if (!raw_log_) return;
    for (size_t i = 0; i < n; i++) {
      junk_[junk_n_++] = b[i];
      if (junk_n_ == sizeof(junk_)) junk_flush_();
    }
  }
  void junk_flush_() { if (junk_n_) { raw_dump_("SCARTO", junk_, junk_n_); junk_n_ = 0; } }

  // --- TRASMISSIONE COME IL MODULO VERO (dal dump: timer uart_tx a 50ms + coda con attesa di conferma) ---
  // Due strade: "immediata" (heartbeat, query 0x08, scritture 0x06, conferma 0x23, MAC) e "in coda"
  // (stato rete 0x03 e info-prodotto 0x01 con attesa della conferma: ritrasmissione ogni 1s per 3 volte;
  // risposte alle domande del mainboard senza attesa). Fra due frame passano SEMPRE almeno 50ms: mai
  // raffiche di frame attaccati.
  struct Tx { uint8_t cmd; uint8_t ack; uint8_t len; uint8_t d[40]; };
  static const uint8_t TXQ = 8;
  Tx qi_[TXQ]; uint8_t qi_n_ = 0;   // invii immediati
  Tx qw_[TXQ]; uint8_t qw_n_ = 0;   // coda (in testa puo' esserci un frame che aspetta la conferma)
  uint8_t tx_wait_ = 0xFF;          // comando di cui aspetto la conferma (0xFF = nessuno)
  uint8_t tx_tries_ = 0;
  uint32_t tx_sent_ms_ = 0, tx_last_ms_ = 0;

  bool push_(Tx *q, uint8_t &n, uint8_t cmd, const uint8_t *data, uint16_t dlen, uint8_t ack) {
    if (dlen > sizeof(q[0].d)) return false;
    for (uint8_t i = 0; i < n; i++)     // lo stesso identico frame e' gia' in attesa: non lo raddoppio
      if (q[i].cmd == cmd && q[i].len == dlen && (dlen == 0 || memcmp(q[i].d, data, dlen) == 0)) return true;
    if (n >= TXQ) {
      static uint32_t last_full = 0;
      if (millis() - last_full > 10000) { last_full = millis(); ESP_LOGW("ps", "coda di trasmissione piena: scartato il comando 0x%02X", cmd); }
      return false;
    }
    q[n].cmd = cmd; q[n].ack = ack; q[n].len = (uint8_t) dlen;
    if (dlen) memcpy(q[n].d, data, dlen);
    n++;
    return true;
  }
  static void del_(Tx *q, uint8_t &n, uint8_t at) { for (uint8_t i = at + 1; i < n; i++) q[i - 1] = q[i]; if (n > at) n--; }
  static void pop_(Tx *q, uint8_t &n) { del_(q, n, 0); }
  // RISPOSTE alle domande del mainboard: partono subito, come nel modulo vero. Tutto il resto (heartbeat,
  // info-prodotto, stato rete, query 0x08, scritture 0x06) e' iniziativa nostra.
  static bool risposta_(uint8_t cmd) {
    switch (cmd) {
      case 0x2D: case 0x90: case 0x1C: case 0x0C: case 0x0F: case 0x2B: case 0x35: case 0x24: case 0x23: return true;
      default: return false;
    }
  }
  // primo frame della coda che si puo' mandare adesso: una risposta sempre, il resto solo a via libera
  static int scegli_(const Tx *q, uint8_t n, bool libero) {
    for (uint8_t i = 0; i < n; i++) if (libero || risposta_(q[i].cmd)) return i;
    return -1;
  }
  static bool spontaneo_in_(const Tx *q, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) if (!risposta_(q[i].cmd)) return true;
    return false;
  }
  void send_(uint8_t cmd, const uint8_t *data, uint16_t dlen) { push_(qi_, qi_n_, cmd, data, dlen, 0xFF); }
  void send_q_(uint8_t cmd, const uint8_t *data, uint16_t dlen, uint8_t ack = 0xFF) { push_(qw_, qw_n_, cmd, data, dlen, ack); }
  void net_send_(uint8_t s) { send_q_(0x03, &s, 1, 0x03); net_sent_ = s; }   // stato rete: in coda, con conferma
  uint8_t rete_ora_() { return net_sent_ == 0xFF ? net_state_ : net_sent_; }   // lo stato annunciato adesso

  // MAI PARLARE SOPRA IL MAINBOARD (17/11): quello che mandiamo di nostra iniziativa parte solo quando la
  // seriale e' zitta da almeno 300 ms e non c'e' un frame a meta'. Se il mainboard non tace mai (per
  // esempio quando goccia byte durante un blocco) dopo 3 s si manda lo stesso, cosi' l'heartbeat non si
  // ferma. Le risposte alle sue domande invece partono subito, come nel modulo vero.
  // Motivo: nel blocco del 17/11 13:46 il mainboard ha mandato un frame con dentro la coda di un altro
  // (buffer di trasmissione sovrascritto) dopo raffiche da ~1 kB in cui la nostra query era caduta in mezzo;
  // stesso guasto documentato sui dimmer Tuya e risolto in ESPHome cosi' (PR esphome #1404 e #1886).
  static const uint32_t SILENZIO_MS = 300, ATTESA_MAX_MS = 3000;
  uint32_t attesa_dal_ = 0;          // da quando un nostro frame aspetta il silenzio (0 = nessuno aspetta)
  uint32_t rimandati_ = 0, forzati_ = 0, t_conto_ms_ = 0;

  void tx_pump_(uint32_t now) {
    if (now - t_conto_ms_ >= 600000) {                        // resoconto ogni 10 minuti
      if (rimandati_ || forzati_)
        ESP_LOGW("ps", "seriale: %u invii nostri rimandati perche' il mainboard trasmetteva, %u mandati lo stesso dopo 3 s (ultimi 10 min)",
                 (unsigned) rimandati_, (unsigned) forzati_);
      rimandati_ = 0; forzati_ = 0; t_conto_ms_ = now;
    }
    if (now - tx_last_ms_ < 50) return;                       // mai due frame a meno di 50ms
    bool zitta = len_ == 0 && now - rx_ms_ >= SILENZIO_MS;
    bool forzato = !zitta && attesa_dal_ != 0 && now - attesa_dal_ >= ATTESA_MAX_MS;
    bool libero = zitta || forzato;
    bool mandato = false, spont = false;
    int i = scegli_(qi_, qi_n_, libero);
    if (i >= 0) {
      spont = !risposta_(qi_[i].cmd);
      raw_send_(qi_[i]); del_(qi_, qi_n_, (uint8_t) i); mandato = true;
    } else if (qw_n_ > 0) {
      if (tx_wait_ != 0xFF) {                                 // la testa della coda aspetta la conferma
        if (now - tx_sent_ms_ >= 1000) {
          if (tx_tries_ >= 3) {
            ESP_LOGW("ps", "nessuna conferma dal mainboard al comando 0x%02X dopo 3 ritrasmissioni", tx_wait_);
            pop_(qw_, qw_n_); tx_wait_ = 0xFF;
          } else if (libero) {                                // anche la ritrasmissione solo a seriale zitta
            tx_tries_++; raw_send_(qw_[0]); tx_sent_ms_ = now; mandato = true; spont = true;
          }
        }
      } else if ((i = scegli_(qw_, qw_n_, libero)) >= 0) {
        spont = !risposta_(qw_[i].cmd);
        raw_send_(qw_[i]); mandato = true;
        if (i == 0 && qw_[0].ack != 0xFF) { tx_wait_ = qw_[0].ack; tx_tries_ = 0; tx_sent_ms_ = now; }
        else del_(qw_, qw_n_, (uint8_t) i);
      }
    }
    if (mandato) tx_last_ms_ = now;
    if (mandato && spont) { if (forzato) forzati_++; attesa_dal_ = 0; }
    // c'e' ancora un nostro frame che aspetta il silenzio? (la ritrasmissione in attesa conta anche lei)
    bool aspetta = spontaneo_in_(qi_, qi_n_) || (tx_wait_ == 0xFF && spontaneo_in_(qw_, qw_n_)) ||
                   (tx_wait_ != 0xFF && now - tx_sent_ms_ >= 1000);
    if (aspetta && attesa_dal_ == 0) { attesa_dal_ = now; if (!zitta) rimandati_++; }
    else if (!aspetta) attesa_dal_ = 0;
  }

  // Ripartenza completa: quello che al modulo vero succede quando si riavvia da solo.
  void restart_(const char *why) {
    ESP_LOGW("ps", "RIPARTENZA COMPLETA (%s): rifaccio heartbeat, info-prodotto, stato rete e query", why);
    st_ = INIT;
    online_since_ms_ = 0; net_sent_ = 0xFF; wifi_sim_ = 0; stall_due_ms_ = 0; pausa_ms_ = 0; annuncio_ms_ = 0;
    wifi_tries_ = 0; last_wake_ms_ = millis(); hb_miss_ = 0;
    qi_n_ = 0; qw_n_ = 0; tx_wait_ = 0xFF; t_hb_ = 0; attesa_dal_ = 0;
    // riparto pulito anche qui: niente dato "vecchio" che inganni il watchdog, HFR da riarmare subito
    // appena di nuovo ONLINE, e nessun pezzo di frame rimasto nel buffer di ricezione
    last_data_ms_ = 0; hfr_due_ = true; hfr_diverso_ = false; len_ = 0;
  }
  // Ricezione: il modulo vero accetta fino a 1017 byte di dati per frame (dal dump).
  static const size_t RX_MAX_DATA = 1017;
  uint8_t buf_[6 + RX_MAX_DATA + 1]; size_t len_ = 0;
  uint32_t rx_ms_ = 0, rx_log_ms_ = 0;   // ultimo byte ricevuto / ultima riga di avviso
  uint16_t rx_bad_ = 0;                  // frame scartati dall'ultimo avviso
  uint32_t t_hb_ = 0, t_q_ = 0, t_hfr_ = 0;
  uint8_t net_state_ = 0x04;
  // deviceKey / WiFi MAC of YOUR station (from the BLE advertisement or the label).
  // The MCU just echoes it back in the net-status reply; fill in yours.
  uint8_t mac_[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint16_t pub_ = 0;

  static uint8_t ck_(const uint8_t *f, size_t n) { uint8_t s = 0; for (size_t i = 0; i < n; i++) s += f[i]; return s; }

  // butta i primi n byte del buffer tenendo quelli dopo
  void drop_(size_t n, bool scarto = true) {
    if (scarto) junk_add_(buf_, n < len_ ? n : len_);
    if (n >= len_) { len_ = 0; return; }
    memmove(buf_, buf_ + n, len_ - n);
    len_ -= n;
  }

  // RICEZIONE CON RIAGGANCIO. Un frame vale solo se intestazione, lunghezza e checksum tornano. Se
  // qualcosa non torna (byte perso, disturbo, lunghezza assurda) NON butto tutto il buffer: scarto la
  // sola intestazione fasulla e cerco il prossimo 55 AA nei byte che ho gia' ricevuto, cosi' il frame
  // buono che segue non va perso insieme a quello rotto.
  void feed_(uint8_t c) {
    rx_ms_ = millis();
    if (len_ == 0 && c != 0x55) { junk_add_(&c, 1); return; }          // fuori frame: aspetto l'inizio
    if (len_ >= sizeof(buf_)) { drop_(2); rx_bad_++; }   // non puo' succedere (la lunghezza e' controllata), ma non sforo mai
    buf_[len_++] = c;
    for (;;) {
      size_t i = 0;
      while (i < len_ && buf_[i] != 0x55) i++;   // porto in testa il primo 55
      if (i) drop_(i);
      if (len_ < 2) return;
      if (buf_[1] != 0xAA) { drop_(1); continue; }
      if (len_ < 6) return;
      size_t plen = ((size_t) buf_[4] << 8) | buf_[5];
      if (plen > RX_MAX_DATA) { rx_bad_++; drop_(2); continue; }      // lunghezza impossibile: intestazione fasulla
      size_t tot = 6 + plen + 1;
      if (len_ < tot) return;                                         // frame non ancora completo
      if (ck_(buf_, tot - 1) != buf_[tot - 1]) { rx_bad_++; drop_(2); continue; }   // checksum errato: mi riaggancio
      if (raw_log_) { junk_flush_(); raw_dump_("RX", buf_, tot); }
      handle_(buf_, tot);
      drop_(tot, false);
    }
  }

  // scrittura fisica di un frame sulla seriale (la chiama solo tx_pump_, che rispetta i 50ms)
  void raw_send_(const Tx &t) {
    uint8_t h[6] = {0x55, 0xAA, 0x00, t.cmd, 0x00, t.len};
    uint8_t ck = ck_(h, 6);
    for (uint8_t i = 0; i < t.len; i++) ck += t.d[i];
    write_array(h, 6);
    if (t.len) write_array(t.d, t.len);
    write(ck);
    if (raw_log_) {
      uint8_t f[6 + sizeof(t.d) + 1];
      memcpy(f, h, 6); if (t.len) memcpy(f + 6, t.d, t.len); f[6 + t.len] = ck;
      raw_dump_("TX", f, 7 + t.len);
    }
  }

  static float rd_double_(const uint8_t *v) {
    uint64_t u = 0; for (int k = 0; k < 8; k++) u = (u << 8) | v[k];
    double d; memcpy(&d, &u, 8); return (float) d;
  }
  static int32_t rd_int_(const uint8_t *v, uint16_t ln) {
    int32_t x = 0; for (uint16_t k = 0; k < ln && k < 4; k++) x = (x << 8) | v[k];
    if (ln == 1 && (v[0] & 0x80)) x |= ~0xFF;
    if (ln == 2 && (v[0] & 0x80)) x |= ~0xFFFF;
    return x;
  }

  // testo e tendine: si pubblica solo se cambia (vedi sotto, "SI PUBBLICA SOLO CIO' CHE CAMBIA")
  static void pub_txt_(text_sensor::TextSensor *t, const std::string &v) {
    if (!t->has_state() || t->state != v) t->publish_state(v);
  }
  static void pub_sel_(select::Select *s, const char *opt) {
    const auto &o = s->traits.get_options();
    for (size_t i = 0; i < o.size(); i++)
      if (o[i] == opt) { auto c = s->active_index(); if (!c.has_value() || *c != i) s->publish_state(opt); return; }
  }

  void leaf_(uint16_t key, uint8_t ty, const uint8_t *v, uint16_t ln) {
    float val;
    if (ty == 0x11 && ln == 8) val = rd_double_(v);
    else if (ty == 0x02 || ty == 0x04 || ty == 0x01) val = (float) rd_int_(v, ln);
    else return;
    // HFR (DP 19): il mainboard dice a che valore e' adesso. Diverso dalla scelta: loop() lo rimette.
    if (key == 0x0013) { hfr_diverso_ = ((int) val != (int) hfr_mode_); return; }
    // energia dei 10 minuti (DP 16 pannelli, DP 17 uscita): si aggiunge al totale
    if ((key == 0x0010 || key == 0x0011) && ty == 0x11) {
      sensor::Sensor *s = key == 0x0010 ? en_pv_ : en_out_;
      float *t = key == 0x0010 ? en_pv_tot_ : en_out_tot_;
      if (s != nullptr && t != nullptr && val >= 0.0f && val < 10.0f) { *t += val; s->publish_state(*t); }
      pub_++; return;
    }
    // stato e guasto -> testo leggibile
    if (key == 0x000C && status_txt_ != nullptr) {
      static const char *S[] = {"Standby", "In carica", "In scarica", "Carica+Scarica", "Bypass"};
      int i = (int) val;
      pub_txt_(status_txt_, (i >= 0 && i < 5) ? std::string(S[i]) : std::to_string(i));
      pub_++; return;
    }
    if (key == 0x000D && fault_txt_ != nullptr) {
      int i = (int) val;
      if (i == 146) i = 10;   // E146 (fantasma, non dichiarato dal cloud) = E10: evita l'oscillazione
      if (i == 0) { pub_txt_(fault_txt_, "Normale"); pub_++; return; }
      // dizionario ufficiale dal pannello H5 del prodotto (p11tpn); 5/6/15 mantenuti
      // dall'osservazione sul campo (il pannello li marca "Unknown")
      const char *d = nullptr;
      switch (i) {
        case 1:   d = "Sovraccarico/corto uscita DC"; break;
        case 2:   d = "Sovratemperatura uscita DC"; break;
        case 3:   d = "Sovraccarico/corto uscita AC"; break;
        case 4:   d = "Sovratemperatura uscita AC"; break;
        case 5:   d = "Sottotensione uscita AC"; break;
        case 6:   d = "Sovratensione uscita AC"; break;
        case 7:   d = "Sovraccarico/corto uscita PD"; break;
        case 8:   d = "Sovraccarico/corto USB-A"; break;
        case 9:   d = "Sovratensione ingresso DC"; break;
        case 10:  d = "Sottotensione ingresso DC"; break;
        case 11:  d = "Sovratensione ingresso AC"; break;
        case 12:  d = "Sottotensione ingresso AC"; break;
        case 13:  d = "Sovratemperatura batteria"; break;
        case 14:  d = "Bassa temperatura batteria"; break;
        case 15:  d = "Sovratemp. batteria in carica/scarica"; break;
        case 22:  d = "Sovratemperatura uscita DC"; break;
        case 23:  d = "Bassa temperatura uscita DC"; break;
        case 24:  d = "Comunicazione uscita DC anomala"; break;
        case 25:  d = "Sovratemperatura MPPT"; break;
        case 28:  d = "Comunicazione MPPT-scheda anomala"; break;
        case 29:  d = "Err. calibrazione zero corrente BMS"; break;
        case 35:  d = "Protezione hardware MPPT"; break;
        case 101: d = "Protezione sovraccarica pacco"; break;
        case 102: d = "Protezione sovrascarica pacco"; break;
        case 103: d = "Protezione sovracorrente in carica"; break;
        case 104: d = "Protezione sovracorrente in scarica"; break;
        case 105: d = "Protezione sovratemp. in carica"; break;
        case 106: d = "Protezione sovratemp. in scarica"; break;
        case 107: d = "Protezione bassa temp. in carica"; break;
        case 108: d = "Protezione bassa temp. in scarica"; break;
        case 109: d = "Protezione corto pacco batteria"; break;
        case 110: d = "Delta tensione celle eccessivo"; break;
        case 111: d = "Bassa temperatura MOS"; break;
        case 112: d = "Precarica fallita"; break;
        case 113: d = "Sovratemperatura MOS"; break;
        case 138: d = "Sovracorrente accendisigari"; break;
        case 139: d = "Sovracorrente DC5525"; break;
        case 143: d = "Guasto sonda NTC MPPT"; break;
        case 144: d = "Corto uscita DC5525"; break;
        case 145: d = "Corto accendisigari"; break;
        case 180: d = "Comunicazione inverter anomala"; break;
        case 181: d = "Comunicazione BMS master anomala"; break;
        case 182: d = "Comunicazione MPPT anomala"; break;
        case 183: d = "Comunicazione BMS slave anomala"; break;
        case 184: d = "Anomalia FLASH"; break;
        case 185: d = "Sleep di sistema fallito"; break;
        case 186: d = "Differenza B+/P+ oltre 5V"; break;
        case 187: d = "Apertura uscita P+ anomala"; break;
        case 190: d = "Guasto inverter"; break;
        case 191: d = "Guasto BMS principale"; break;
        case 192: d = "Guasto BMS secondario"; break;
        case 193: d = "Guasto MPPT"; break;
        case 199: d = "Aggancio rete fallito (rete assente)"; break;
        case 200: d = "Sovratensione rete"; break;
        case 201: d = "Sottotensione rete"; break;
        case 202: d = "Frequenza rete troppo alta"; break;
        case 203: d = "Frequenza rete troppo bassa"; break;
        case 204: d = "Rete persa"; break;
        case 205: d = "Tensione rete persa"; break;
        case 206: d = "Frequenza rete persa"; break;
        case 207: d = "Protezione isola"; break;
        case 208: d = "Rete rilevata in piena potenza"; break;
        case 254: d = "Batteria fredda: PV in bypass al micro-inverter"; break;
        case 255: d = "Batteria piena: PV in bypass al micro-inverter"; break;
      }
      char b[64];
      if (d) snprintf(b, sizeof(b), "E%02d - %s", i, d);
      else   snprintf(b, sizeof(b), "E%02d", i);
      pub_txt_(fault_txt_, b);
      pub_++; return;
    }
    // spegnimento schermo (DP 38 = 0x0026): valori 3/10/0 minuti (non indici)
    if (key == 0x0026 && screen_sel_ != nullptr) {
      int i = (int) val;
      const char *o = (i == 3) ? "3 minuti" : (i == 10) ? "10 minuti" : (i == 0) ? "Mai" : nullptr;
      if (o != nullptr) { pub_sel_(screen_sel_, o); pub_++; }
      return;
    }
    // autonomia (DP 2 batteria = 0x0302): solo testo in giorni/ore/minuti
    if (key == 0x0302) {
      if (auton_txt_ != nullptr) {
        int t = (int) val, d = t / 1440, h = (t % 1440) / 60, mi = t % 60; char b[32];
        if (d > 0) snprintf(b, sizeof(b), "%dg %dh %dm", d, h, mi); else snprintf(b, sizeof(b), "%dh %dm", h, mi);
        pub_txt_(auton_txt_, b); pub_++;
      }
      return;
    }
    bool found = false;
    auto it = num_.find(key);
    // SI PUBBLICA SOLO CIO' CHE CAMBIA: sotto carica ogni rapporto porta ~40 valori e mandarli tutti
    // a HA ogni volta intasava il collegamento (l'ESP risultava irraggiungibile finche' si caricava).
    // HA tiene l'ultimo valore; alla riconnessione ESPHome rimanda comunque lo stato di tutto.
    if (it != num_.end()) {
      float nv = val * it->second.scale;
      if (!it->second.s->has_state() || it->second.s->state != nv) it->second.s->publish_state(nv);
      found = true;
    }
    auto sit = sw_.find(key);
    if (sit != sw_.end()) { if (sit->second->state != (val != 0)) sit->second->publish_state(val != 0); found = true; }
    auto slit = sel_.find(key);
    if (slit != sel_.end()) {
      const auto &opts = slit->second->traits.get_options();
      int idx = (int) val;
      if (idx >= 0 && idx < (int) opts.size()) {
        auto cur = slit->second->active_index();
        if (!cur.has_value() || *cur != (size_t) idx) slit->second->publish_state(opts[idx]);
        found = true;
      }
    }
    auto nit = numctl_.find(key);
    if (nit != numctl_.end()) { if (!nit->second->has_state() || nit->second->state != val) nit->second->publish_state(val); found = true; }
    if (found) pub_++;
    else ESP_LOGD("ps", "DP non mappato key 0x%04X ty 0x%02X = %.2f", key, ty, val);
  }

  // Il frame contiene il gruppo batteria (DP 3)? E' il segno dei dati veri: a stazione sana arriva a
  // ogni query; quando il mainboard si inceppa manda al piu' l'eco dell'HFR o il solo gruppo della rete.
  static bool ha_batteria_(const uint8_t *b, size_t n) {
    size_t i = 0;
    while (i + 5 <= n) {
      uint16_t id = (b[i] << 8) | b[i + 1];
      uint16_t ln = (b[i + 3] << 8) | b[i + 4];
      if (id == 0x0003) return true;
      i += 5 + ln;
    }
    return false;
  }

  void parse_(const uint8_t *b, size_t n, uint16_t parent) {
    size_t i = 0;
    while (i + 5 <= n) {
      uint16_t id = (b[i] << 8) | b[i + 1];
      uint8_t ty = b[i + 2];
      uint16_t ln = (b[i + 3] << 8) | b[i + 4];
      i += 5;
      if (i + ln > n) break;
      const uint8_t *v = b + i;
      // gruppo/struct: scendo. Negli ARRAY (pack 4, celle 42) l'elemento e' un altro struct
      // con id = indice (0,1,..): tengo l'id del GRUPPO gia' noto, non l'indice, se no i
      // sotto-campi collidono con le chiavi top-level (es. CellVoltage12/13 -> stato/guasto).
      if (ty == 0x12) parse_(v, ln, parent ? parent : id);
      else leaf_(parent ? (uint16_t)((parent << 8) | id) : id, ty, v, ln);
      i += ln;
    }
  }

  void handle_(uint8_t *f, size_t n) {
    // (il checksum e' gia' stato verificato da feed_)
    uint8_t cmd = f[3];
    uint16_t dlen = ((uint16_t)f[4] << 8) | f[5];
    // era la conferma che la coda stava aspettando: libero la coda (come __uart_stop_timeout_retrans)
    if (tx_wait_ != 0xFF && cmd == tx_wait_) { pop_(qw_, qw_n_); tx_wait_ = 0xFF; }
    switch (cmd) {
      case 0x00:   // risposta all'heartbeat: dato 00 = prima risposta dopo un riavvio del mainboard, 01 = le successive
        hb_miss_ = 0;
        if (st_ == INIT) { st_ = HB_OK; ESP_LOGI("ps", "mainboard VIVO -> product-info"); t_hb_ = millis(); hb_ok_dal_ms_ = millis(); }
        if (st_ != ONLINE) send_q_(0x01, nullptr, 0, 0x01);   // info-prodotto: in coda, con conferma e ritrasmissione
        else if (dlen >= 1 && f[6] == 0x00) {                 // il mainboard si e' riavviato: il modulo vero rimanda la query
          ESP_LOGW("ps", "il mainboard si e' riavviato (heartbeat con 00) -> query 0x08");
          send_(0x08, nullptr, 0);   // SEMPRE (come il modulo vero): lo stato completo, beep compreso
        }
        break;
      case 0x01: {
        if (dlen > 0) {   // cosa dichiara il mainboard: p, v, m, mt, new_dp, tslid...
          char js[161]; uint16_t l = dlen < 160 ? dlen : 160;
          memcpy(js, f + 6, l); js[l] = 0;
          ESP_LOGI("ps", "info-prodotto dal mainboard (%u byte): %s", dlen, js);
        }
        if (st_ != ONLINE) { st_ = ONLINE; ESP_LOGI("ps", "product-info -> ONLINE, query 0x08"); send_(0x08, nullptr, 0); net_sent_ = 0xFF; t_q_ = millis(); }   // 0x08 SEMPRE all'ingresso (come il modulo vero): l'unico modo di sapere il beep e le impostazioni che non sono nei giri spontanei
      } break;
      case 0x1C: send_time_(); break;
      case 0x2D: { uint8_t r[7]; r[0] = 0x00; memcpy(r + 1, mac_, 6); send_(0x2D, r, 7); } break;
      // Domande sulla rete: si risponde con lo stato ANNUNCIATO in quel momento (anche durante il finto
      // WiFi spento), come il modulo vero che risponde con quello reale.
      // PAUSA DEI ~40 MINUTI: ogni 4 domande 0x24 (una ogni ~606 s) il mainboard chiede 0x2B+0x35 ogni
      // secondo e non manda rapporti. Le risposte non contano (04 per 8 minuti di fila, 0x35 con 01 o 03:
      // pausa uguale); lasciata a se' NON finisce (18/11 11:53-12:01). Nei registri (16 pause su 16) finisce
      // solo quando riceve un nostro ANNUNCIO 0x03=04; lo 02 e lo 03 del finto WiFi non la chiudono. Il
      // modulo vero annuncia lo 0x03 solo al cambio (dump, thread ssid-m 0x62D80), per questo col FC41D
      // finiva solo alla riconnessione: e' il "blocco dei ~40 minuti" (2430 s = 4 x 606 s).
      // Cura (22/11): alla 2a 0x2B entro 3 s annuncio SUBITO 0x03=04, da solo (niente 02/03, niente HFR).
      // Se 5 s dopo l'annuncio la chiede ancora: finto WiFi completo come prima (max 1 ogni 2 minuti).
      // (23/11: alzato da 3 a 5 s, le pause chiuse dall'annuncio sono durate 0,65 / 2,4 / 2,8 s.)
      case 0x2B: {
        uint8_t r = rete_ora_(); send_q_(0x2B, &r, 1);
        uint32_t ora = millis();
        bool di_nuovo = t_2b_ms_ != 0 && ora - t_2b_ms_ < 3000;
        t_2b_ms_ = ora;
        if (!di_nuovo || st_ != ONLINE || wifi_sim_ != 0) break;   // SEMPRE, anche con "Chiedi i dati" spento
        if (pausa_ms_ == 0) {
          pausa_ms_ = ora; annuncio_ms_ = ora;
          ESP_LOGW("ps", "PAUSA DEI 40 MINUTI: il mainboard richiede 'come sta la rete' -> annuncio 0x03=04 (solo quello)");
          net_send_(net_state_);
        } else if (annuncio_ms_ != 0 && ora - annuncio_ms_ >= 5000 && ora - last_wake_ms_ > 120000) {
          ricollega("la pausa continua dopo l'annuncio 0x03=04");
        }
      } break;
      // 0x35 sotto-comando 05: 05 + flag. Dal dump (fn 0x647D4): bit 0 = WiFi collegato, bit 1 = chiave del
      // dispositivo presente e nessun reset in attesa, cioe' "registrato"; un FC41D accoppiato risponde 03.
      // Rispondiamo 01: il 12/11 l'avevo letto (male) come "in configurazione" e cambiato da 03 a 01, ma la
      // pausa dei 40 minuti c'e' con tutte e due le risposte, quindi qui non cambia niente.
      case 0x35: { uint8_t r[2] = {0x05, (uint8_t) (rete_ora_() >= 3 ? 0x01 : 0x00)}; send_q_(0x35, r, 2); } break;
      case 0x90: {   // "dammi l'IP WiFi": il modulo vero risponde {0x01, IP in ASCII}; {0x01,0x00} solo se non connesso
        // durante la pausa dei 40 minuti il mainboard non fa il suo giro 0x90/0x2D/0x1C: il primo 0x90 = pausa finita
        if (pausa_ms_ != 0) {
          uint32_t ora = millis();
          ESP_LOGW("ps", "PAUSA CHIUSA dopo %u ms (%s)", (unsigned) (ora - pausa_ms_),
                   annuncio_ms_ != 0 ? "bastato l'annuncio 0x03=04" : "servito il finto WiFi");
          pausa_ms_ = 0; annuncio_ms_ = 0;
        }
        uint8_t r[20]; r[0] = 0x01; r[1] = 0x00; uint16_t rl = 2;
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info;
        if (rete_ora_() >= 3 && nif != nullptr && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0) {
          char ip[16];
          esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
          uint16_t l = (uint16_t) strlen(ip);
          memcpy(r + 1, ip, l); rl = 1 + l;
        }
        send_q_(0x90, r, rl);
      } break;
      case 0x07: {
        pub_ = 0;
        hb_miss_ = 0;   // nel modulo vero anche un report azzera il contatore degli heartbeat persi
        if (ha_batteria_(f + 6, dlen)) { last_data_ms_ = millis(); wifi_tries_ = 0; }   // dati veri: watchdog a riposo
        parse_(f + 6, dlen, 0);
        // i report corti (conferme delle scritture, cambi di un solo datapoint) li scrivo per intero:
        // id(2) tipo(1) lunghezza(2) valore
        if (dlen < 50) ESP_LOGI("ps", "report 0x07 (%u byte) -> %u valori  [%s]", dlen, pub_, format_hex_pretty(f + 6, dlen).c_str());
        else ESP_LOGI("ps", "report 0x07 (%u byte) -> %u valori", dlen, pub_);
      } break;
      // --- comandi che il modulo FC41D vero gestisce (tabella dei 17 gestori, dal dump) ---
      case 0x22: {   // report SINCRONO: stesso formato dello 0x07, ma il mainboard ASPETTA la conferma 0x23 (1 = ricevuto)
        pub_ = 0;
        hb_miss_ = 0;
        if (ha_batteria_(f + 6, dlen)) { last_data_ms_ = millis(); wifi_tries_ = 0; }
        parse_(f + 6, dlen, 0);
        uint8_t ok = 0x01; send_(0x23, &ok, 1);
        ESP_LOGW("ps", "report SINCRONO 0x22 (%u byte) -> %u valori, confermato con 0x23", dlen, pub_);
      } break;
      case 0x82: {   // report sincrono nel formato alternativo: non lo interpreto, ma lo confermo come fa il modulo
        uint8_t ok = 0x01; send_(0x23, &ok, 1);
        ESP_LOGW("ps", "report SINCRONO 0x82 (%u byte): confermato con 0x23, non interpretato", dlen);
      } break;
      case 0x81:     // report asincrono nel formato alternativo: solo segnalato
        ESP_LOGW("ps", "report 0x81 (%u byte): formato alternativo, non interpretato", dlen);
        break;
      case 0x24: {   // potenza del segnale WiFi: 1 byte con segno (dBm)
        int8_t rssi = (wifi::global_wifi_component != nullptr) ? wifi::global_wifi_component->wifi_rssi() : 0;
        send_q_(0x24, (uint8_t *) &rssi, 1);
      } break;
      case 0x0C: send_gmt_(); break;   // ora GMT
      case 0x0F: {   // memoria libera del modulo: 4 byte big-endian
        uint32_t h = esp_get_free_heap_size();
        uint8_t r[4] = {(uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t) h};
        send_q_(0x0F, r, 4);
      } break;
      case 0x03: break;   // conferma del mainboard al nostro stato rete
      default: {     // qualunque altro comando: lo segnalo (al massimo ogni 5s per comando) per sapere cosa ci chiede
        static uint8_t last_unk = 0; static uint32_t last_unk_ms = 0;
        uint32_t nowu = millis();
        if (cmd != last_unk || nowu - last_unk_ms > 5000) {
          last_unk = cmd; last_unk_ms = nowu;
          ESP_LOGW("ps", "cmd dal mainboard NON gestito: 0x%02X (%u byte)", cmd, dlen);
        }
      } break;
    }
  }

  void send_gmt_() {   // risposta al 0x0C: {valido, anno-2000, mese, giorno, ora, minuti, secondi} in UTC
    uint8_t t[7] = {0};
    if (rtc_ != nullptr) {
      auto n = rtc_->utcnow();
      if (n.is_valid()) {
        t[0] = 0x01;
        t[1] = (uint8_t)((n.year - 2000) & 0xFF);
        t[2] = (uint8_t) n.month;
        t[3] = (uint8_t) n.day_of_month;
        t[4] = (uint8_t) n.hour;
        t[5] = (uint8_t) n.minute;
        t[6] = (uint8_t) n.second;
      }
    }
    send_q_(0x0C, t, 7);
  }

  void send_time_() {
    uint8_t t[8] = {0};
    if (rtc_ != nullptr) {
      auto n = rtc_->now();
      if (n.is_valid()) {
        t[0] = 0x01;
        t[1] = (uint8_t)((n.year - 2000) & 0xFF);
        t[2] = (uint8_t) n.month;
        t[3] = (uint8_t) n.day_of_month;
        t[4] = (uint8_t) n.hour;
        t[5] = (uint8_t) n.minute;
        t[6] = (uint8_t) n.second;
        uint8_t dow = n.day_of_week;
        t[7] = (dow == 1) ? 7 : (uint8_t)(dow - 1);
      }
    }
    send_q_(0x1C, t, 8);
  }
};

// puntatore globale: gli switch/select/number di ESPHome scrivono qui
inline PowerstationModule *g_ps = nullptr;
