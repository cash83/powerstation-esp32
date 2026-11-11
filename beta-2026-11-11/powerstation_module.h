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
// ONLINE -> stato rete 0x04 (cloud) + una query 0x08 (poi nudge ogni 30s) -> report 0x07.
// Legge i 0x07 (set veloce PV/AC/rete/DC ogni ~30s + set lento batteria/temp ogni ~2-3 min) e li
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
  // stato per la spia: 0 rosso (handshake non fatto), 1 giallo (acceso ma report fermi = freeze),
  // 2 verde (dati freschi). NB: a station spenta l'ESP e' senza corrente, quindi la spia e' proprio spenta.
  uint8_t link_state() {
    if (st_ == INIT) return 0;
    if (st_ == ONLINE && (millis() - last_report_ms_) < 8000) return 2;
    return 1;
  }
  // per il monitoraggio del blocco: online? e da quanti ms non arriva un report 0x07
  bool online() { return st_ == ONLINE; }
  bool had_report() { return last_report_ms_ != 0; }   // ha gia' ricevuto almeno un report (anti falso-positivo al boot)
  uint32_t ms_since_report() { return millis() - last_report_ms_; }

  void setup() { ESP_LOGI("ps", "emulatore modulo FC41D avviato (stato INIT)"); }

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
    // un pezzo di frame rimasto a meta' (seriale muta da mezzo secondo): non arrivera' piu', lo butto
    if (len_ != 0 && now - rx_ms_ > 500) { if (len_ >= 2) rx_bad_++; len_ = 0; }
    if (rx_bad_ != 0 && now - rx_log_ms_ > 30000) {
      rx_log_ms_ = now;
      ESP_LOGW("ps", "seriale: %u frame scartati (checksum, lunghezza o frame a meta') negli ultimi 30s", (unsigned) rx_bad_);
      rx_bad_ = 0;
    }
    switch (st_) {
      case INIT:
        if (now - t_hb_ >= 1000) { t_hb_ = now; send_(0x00, nullptr, 0); }
        break;
      case HB_OK:
        // heartbeat a 15s; le info-prodotto (0x01) si chiedono a ogni risposta di heartbeat (vedi handle_),
        // con attesa della conferma e ritrasmissione: esattamente come il modulo vero.
        if (now - t_hb_ >= 15000) { t_hb_ = now; send_(0x00, nullptr, 0); }
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
        bool wd_busy = (net_cycle_ms_ != 0 || wifi_sim_ != 0);
        if (!wd_busy && net_sent_ != net_state_) net_send_(net_state_);
        // WATCHDOG STALLO: siamo ONLINE ma dal mainboard non arrivano piu' i set di dati (report 0x07 "veri",
        // >=50 byte: gli echi da 9 byte delle scritture non contano) da 30s. Scaletta, un intervento ogni 30s:
        //   1) e 2) CICLO RAPIDO: 0x03=0x02 "non connesso", dopo 2s di nuovo lo stato vero + query 0x08;
        //   3) e 4) finto WiFi SPENTO E RIACCESO, come quando si spegneva il WiFi del router col modulo
        //      originale: 0x02 tenuto 10s in silenzio (niente query ne' HFR, come un modulo senza rete),
        //      poi 0x03 "connesso al router", dopo 3s lo stato vero 0x04 "cloud" + query 0x08 (ogni 2 min);
        //   5) RIPARTENZA COMPLETA da capo (heartbeat, info-prodotto, stato rete, query), poi la scaletta
        //      ricomincia.
        uint32_t stall_ms = 30000;
        if (q_ms_ + 3000 > stall_ms) stall_ms = q_ms_ + 3000;   // solo per non scattare a vuoto con lo slider oltre i 27s
        uint32_t ref = last_data_ms_ > online_since_ms_ ? last_data_ms_ : online_since_ms_;
        if (!wd_busy && now - ref > stall_ms && now - last_wake_ms_ > wd_cool_ms_) {
          last_wake_ms_ = now;
          if (wd_fail_ < 2) {
            wd_fail_++; wd_cool_ms_ = 30000;
            net_send_(0x02);
            net_cycle_ms_ = now;
            ESP_LOGW("ps", "WATCHDOG: nessun dato da %us -> ciclo rapido stato-rete (tentativo %u)", (unsigned) ((now - ref) / 1000), (unsigned) wd_fail_);
          } else if (wifi_tries_ < 2) {
            wifi_tries_++; wd_cool_ms_ = 120000;
            net_send_(0x02);
            wifi_sim_ = 1; wifi_sim_ms_ = now;
            ESP_LOGW("ps", "WATCHDOG: nessun dato da %us -> finto WiFi spento (0x02) per 10s (tentativo %u)", (unsigned) ((now - ref) / 1000), (unsigned) wifi_tries_);
          } else {
            restart_("watchdog: nessun dato dopo cicli rapidi e finto WiFi");
            break;
          }
        }
        if (net_cycle_ms_ != 0 && now - net_cycle_ms_ >= 2000) {          // fine ciclo rapido
          net_cycle_ms_ = 0;
          net_send_(net_state_);
          send_(0x08, nullptr, 0); t_q_ = now; hfr_due_ = true;
        }
        if (wifi_sim_ == 1 && now - wifi_sim_ms_ >= 10000) {               // WiFi "riacceso": connesso al router
          net_send_(0x03);
          wifi_sim_ = 2; wifi_sim_ms_ = now;
          ESP_LOGW("ps", "WATCHDOG: finto WiFi riacceso (0x03), tra 3s stato cloud + query");
        } else if (wifi_sim_ == 2 && now - wifi_sim_ms_ >= 3000) {         // ...e poi di nuovo lo stato vero
          net_send_(net_state_);
          send_(0x08, nullptr, 0); t_q_ = now; hfr_due_ = true;
          wifi_sim_ = 0; last_wake_ms_ = now;
        }
        if (wifi_sim_ != 0) break;   // durante il finto WiFi spento: niente HFR ne' query
        // Tieni viva l'ALTA FREQUENZA LAN (DP 19 = 1): la station torna a bassa frequenza da sola
        // dopo ~20s, quindi la ri-stimolo ogni 10s (come fa l'addon con cmd19+cmd17). Con l'HFR
        // acceso la station spinge TUTTO veloce da sola (batteria compresa), anche col 0x08 a 5s.
        // In piu' (misurato l'11/11): l'HFR dura ~30s da quando viene acceso e riscrivere 1 mentre e' gia' 1
        // NON lo allunga; quando scade il mainboard lo annuncia da solo con un report DP19 = 0. Appena
        // arriva quell'annuncio lo riaccendo subito (al massimo una volta ogni 3s), cosi' non resta spento
        // fino alla prossima riscrittura. Le riscritture a +10/+20/+30s erano tutte a vuoto (log dell'11/11:
        // scadenza a 30,3s esatti, 7 volte su 7), quindi restano solo: la prima accensione appena in linea
        // o dopo un intervento del watchdog (hfr_due_), e una rete di sicurezza a 45s che scatta soltanto
        // se un annuncio del mainboard va perso.
        if (hfr_zero_ && hfr_mode_ != 0 && now - t_hfr_ev_ >= 3000) {
          hfr_zero_ = false; t_hfr_ev_ = now; t_hfr_ = now;
          ESP_LOGI("ps", "il mainboard ha spento l'HFR (DP19=0) -> lo riaccendo subito");
          write_val(0x0013, hfr_mode_);
        }
        if (hfr_due_ || now - t_hfr_ >= 45000) { hfr_due_ = false; t_hfr_ = now; write_val(0x0013, hfr_mode_); }  // la TUA scelta HFR
        if (now - t_q_ >= q_ms_)   { t_q_ = now; send_(0x08, nullptr, 0); }          // query, intervallo dallo slider
        break;
      }
    }
    tx_pump_(millis());
  }

 protected:
  enum St { INIT, HB_OK, ONLINE };
  struct Reg { sensor::Sensor *s; float scale; };
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
  uint8_t hfr_mode_ = 1;        // modo HFR mantenuto (scelta utente): 0 Infreq,1 LAN,2 WiFi,3 LAN+WiFi
  uint32_t q_ms_ = 5000;        // intervallo query 0x08 (slider utente)
  uint32_t last_report_ms_ = 0; // ultimo report 0x07 ricevuto (per la spia di stato)
  uint32_t online_since_ms_ = 0;// da quando siamo in ONLINE (0 = non ancora)
  uint32_t last_wake_ms_ = 0;   // ultimo intervento del watchdog di stallo
  uint32_t net_cycle_ms_ = 0;   // ciclo stato-rete in corso: a +2s manda 0x03=0x04
  uint8_t net_sent_ = 0xFF;     // ultimo stato rete trasmesso al mainboard (0xFF = ancora nessuno)
  uint32_t last_data_ms_ = 0;   // ultimo report 0x07 "vero" (>=50 byte), per il watchdog di stallo
  uint32_t wd_cool_ms_ = 30000; // pausa minima fra due interventi del watchdog
  uint8_t wd_fail_ = 0;         // cicli rapidi fatti senza che tornassero i dati
  uint8_t wifi_sim_ = 0;        // finto WiFi spento/riacceso: 0 fermo, 1 "spento" (0x02), 2 "riacceso" (0x03)
  uint32_t wifi_sim_ms_ = 0;
  uint8_t wifi_tries_ = 0;      // finti WiFi spento/riacceso fatti senza che tornassero i dati
  uint8_t hb_miss_ = 0;         // heartbeat mandati di fila senza risposta ne' report (come il contatore del modulo)
  bool hfr_due_ = true;         // HFR da scrivere subito (appena in linea, dopo ripartenza o intervento del watchdog)
  bool hfr_zero_ = false;       // il mainboard ha annunciato DP19 = 0 (HFR spento da lui) e non l'ho ancora riacceso
  uint32_t t_hfr_ev_ = 0;       // ultima riaccensione dell'HFR fatta su annuncio del mainboard

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
  static void pop_(Tx *q, uint8_t &n) { for (uint8_t i = 1; i < n; i++) q[i - 1] = q[i]; if (n) n--; }
  void send_(uint8_t cmd, const uint8_t *data, uint16_t dlen) { push_(qi_, qi_n_, cmd, data, dlen, 0xFF); }
  void send_q_(uint8_t cmd, const uint8_t *data, uint16_t dlen, uint8_t ack = 0xFF) { push_(qw_, qw_n_, cmd, data, dlen, ack); }
  void net_send_(uint8_t s) { send_q_(0x03, &s, 1, 0x03); net_sent_ = s; }   // stato rete: in coda, con conferma

  void tx_pump_(uint32_t now) {
    if (now - tx_last_ms_ < 50) return;                       // mai due frame a meno di 50ms
    if (qi_n_ > 0) { raw_send_(qi_[0]); pop_(qi_, qi_n_); tx_last_ms_ = now; return; }
    if (qw_n_ == 0) return;
    if (tx_wait_ != 0xFF) {                                   // la testa della coda aspetta la conferma
      if (now - tx_sent_ms_ < 1000) return;
      if (tx_tries_ >= 3) {
        ESP_LOGW("ps", "nessuna conferma dal mainboard al comando 0x%02X dopo 3 ritrasmissioni", tx_wait_);
        pop_(qw_, qw_n_); tx_wait_ = 0xFF;
        return;
      }
      tx_tries_++; raw_send_(qw_[0]); tx_sent_ms_ = now; tx_last_ms_ = now;
      return;
    }
    raw_send_(qw_[0]); tx_last_ms_ = now;
    if (qw_[0].ack != 0xFF) { tx_wait_ = qw_[0].ack; tx_tries_ = 0; tx_sent_ms_ = now; }
    else pop_(qw_, qw_n_);
  }

  // Ripartenza completa: quello che al modulo vero succede quando si riavvia da solo.
  void restart_(const char *why) {
    ESP_LOGW("ps", "RIPARTENZA COMPLETA (%s): rifaccio heartbeat, info-prodotto, stato rete e query", why);
    st_ = INIT;
    online_since_ms_ = 0; net_sent_ = 0xFF; net_cycle_ms_ = 0; wifi_sim_ = 0;
    wd_fail_ = 0; wifi_tries_ = 0; wd_cool_ms_ = 30000; last_wake_ms_ = millis(); hb_miss_ = 0;
    qi_n_ = 0; qw_n_ = 0; tx_wait_ = 0xFF; t_hb_ = 0;
    // riparto pulito anche qui: niente dato "vecchio" che inganni il watchdog, HFR da riarmare subito
    // appena di nuovo ONLINE, e nessun pezzo di frame rimasto nel buffer di ricezione
    last_data_ms_ = 0; hfr_due_ = true; hfr_zero_ = false; len_ = 0;
  }
  // Ricezione: il modulo vero accetta fino a 1017 byte di dati per frame (dal dump).
  static const size_t RX_MAX_DATA = 1017;
  uint8_t buf_[6 + RX_MAX_DATA + 1]; size_t len_ = 0;
  uint32_t rx_ms_ = 0, rx_log_ms_ = 0;   // ultimo byte ricevuto / ultima riga di avviso
  uint16_t rx_bad_ = 0;                  // frame scartati dall'ultimo avviso
  uint32_t t_hb_ = 0, t_pi_ = 0, t_net_ = 0, t_q_ = 0, t_hfr_ = 0;
  uint8_t net_state_ = 0x04;
  // deviceKey / WiFi MAC of YOUR station (from the BLE advertisement or the label).
  // The MCU just echoes it back in the net-status reply; fill in yours.
  uint8_t mac_[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint16_t pub_ = 0;

  static uint8_t ck_(const uint8_t *f, size_t n) { uint8_t s = 0; for (size_t i = 0; i < n; i++) s += f[i]; return s; }

  // butta i primi n byte del buffer tenendo quelli dopo
  void drop_(size_t n) {
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
    if (len_ == 0 && c != 0x55) return;          // fuori frame: aspetto l'inizio
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
      handle_(buf_, tot);
      drop_(tot);
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

  void leaf_(uint16_t key, uint8_t ty, const uint8_t *v, uint16_t ln) {
    float val;
    if (ty == 0x11 && ln == 8) val = rd_double_(v);
    else if (ty == 0x02 || ty == 0x04 || ty == 0x01) val = (float) rd_int_(v, ln);
    else return;
    // HFR (DP 19): il mainboard dice a che valore e' adesso. 0 = l'ha spento lui: loop() lo riaccende.
    if (key == 0x0013) { hfr_zero_ = ((int) val == 0); return; }
    // stato e guasto -> testo leggibile
    if (key == 0x000C && status_txt_ != nullptr) {
      static const char *S[] = {"Standby", "In carica", "In scarica", "Carica+Scarica", "Bypass"};
      int i = (int) val;
      if (i >= 0 && i < 5) status_txt_->publish_state(S[i]); else status_txt_->publish_state(std::to_string(i));
      pub_++; return;
    }
    if (key == 0x000D && fault_txt_ != nullptr) {
      int i = (int) val;
      if (i == 146) i = 10;   // E146 (fantasma, non dichiarato dal cloud) = E10: evita l'oscillazione
      if (i == 0) { fault_txt_->publish_state("Normale"); pub_++; return; }
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
      fault_txt_->publish_state(b);
      pub_++; return;
    }
    // spegnimento schermo (DP 38 = 0x0026): valori 3/10/0 minuti (non indici)
    if (key == 0x0026 && screen_sel_ != nullptr) {
      int i = (int) val;
      const char *o = (i == 3) ? "3 minuti" : (i == 10) ? "10 minuti" : (i == 0) ? "Mai" : nullptr;
      if (o != nullptr) { screen_sel_->publish_state(o); pub_++; }
      return;
    }
    // autonomia (DP 2 batteria = 0x0302): solo testo in giorni/ore/minuti
    if (key == 0x0302) {
      if (auton_txt_ != nullptr) {
        int t = (int) val, d = t / 1440, h = (t % 1440) / 60, mi = t % 60; char b[32];
        if (d > 0) snprintf(b, sizeof(b), "%dg %dh %dm", d, h, mi); else snprintf(b, sizeof(b), "%dh %dm", h, mi);
        auton_txt_->publish_state(b); pub_++;
      }
      return;
    }
    bool found = false;
    auto it = num_.find(key);
    if (it != num_.end()) { it->second.s->publish_state(val * it->second.scale); found = true; }
    auto sit = sw_.find(key);
    if (sit != sw_.end()) { sit->second->publish_state(val != 0); found = true; }
    auto slit = sel_.find(key);
    if (slit != sel_.end()) {
      const auto &opts = slit->second->traits.get_options();
      int idx = (int) val;
      if (idx >= 0 && idx < (int) opts.size()) { slit->second->publish_state(opts[idx]); found = true; }
    }
    auto nit = numctl_.find(key);
    if (nit != numctl_.end()) { nit->second->publish_state(val); found = true; }
    if (found) pub_++;
    else ESP_LOGD("ps", "DP non mappato key 0x%04X ty 0x%02X = %.2f", key, ty, val);
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
        if (st_ == INIT) { st_ = HB_OK; ESP_LOGI("ps", "mainboard VIVO -> product-info"); t_hb_ = millis(); }
        if (st_ != ONLINE) send_q_(0x01, nullptr, 0, 0x01);   // info-prodotto: in coda, con conferma e ritrasmissione
        else if (dlen >= 1 && f[6] == 0x00) {                 // il mainboard si e' riavviato: il modulo vero rimanda la query
          ESP_LOGW("ps", "il mainboard si e' riavviato (heartbeat con 00) -> query 0x08");
          send_(0x08, nullptr, 0);
        }
        break;
      case 0x01: {
        if (dlen > 0) {   // cosa dichiara il mainboard: p, v, m, mt, new_dp, tslid...
          char js[161]; uint16_t l = dlen < 160 ? dlen : 160;
          memcpy(js, f + 6, l); js[l] = 0;
          ESP_LOGI("ps", "info-prodotto dal mainboard (%u byte): %s", dlen, js);
        }
        if (st_ != ONLINE) { st_ = ONLINE; ESP_LOGI("ps", "product-info -> ONLINE, query 0x08"); send_(0x08, nullptr, 0); net_sent_ = 0xFF; t_q_ = millis(); }
      } break;
      case 0x1C: send_time_(); break;
      case 0x2D: { uint8_t r[7]; r[0] = 0x00; memcpy(r + 1, mac_, 6); send_(0x2D, r, 7); } break;
      case 0x2B: { uint8_t r = net_state_; send_q_(0x2B, &r, 1); } break;
      case 0x35: { uint8_t st = net_state_ >= 4 ? 3 : net_state_; uint8_t r[2] = {0x05, st}; send_q_(0x35, r, 2); } break;
      case 0x90: {   // "dammi l'IP WiFi": il modulo vero risponde {0x01, IP in ASCII}; {0x01,0x00} solo se non connesso
        uint8_t r[20]; r[0] = 0x01; r[1] = 0x00; uint16_t rl = 2;
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info;
        if (nif != nullptr && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0) {
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
        last_report_ms_ = millis();
        if (dlen >= 50) { last_data_ms_ = last_report_ms_; wd_fail_ = 0; wifi_tries_ = 0; wd_cool_ms_ = 30000; }   // dati veri: watchdog a riposo
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
        last_report_ms_ = millis();
        if (dlen >= 50) { last_data_ms_ = last_report_ms_; wd_fail_ = 0; wifi_tries_ = 0; wd_cool_ms_ = 30000; }
        parse_(f + 6, dlen, 0);
        uint8_t ok = 0x01; send_(0x23, &ok, 1);
        ESP_LOGW("ps", "report SINCRONO 0x22 (%u byte) -> %u valori, confermato con 0x23", dlen, pub_);
      } break;
      case 0x82: {   // report sincrono nel formato alternativo: non lo interpreto, ma lo confermo come fa il modulo
        last_report_ms_ = millis();
        uint8_t ok = 0x01; send_(0x23, &ok, 1);
        ESP_LOGW("ps", "report SINCRONO 0x82 (%u byte): confermato con 0x23, non interpretato", dlen);
      } break;
      case 0x81:     // report asincrono nel formato alternativo: solo segnalato
        last_report_ms_ = millis();
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
