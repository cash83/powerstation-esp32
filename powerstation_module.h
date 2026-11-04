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
  void set_hfr(uint8_t m) { hfr_mode_ = m; }                              // modo HFR scelto dall'utente
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
    switch (st_) {
      case INIT:
        if (now - t_hb_ >= 1000) { t_hb_ = now; send_(0x00, nullptr, 0); }
        break;
      case HB_OK:
        if (now - t_hb_ >= 15000) { t_hb_ = now; send_(0x00, nullptr, 0); }
        if (now - t_pi_ >= 2000)  { t_pi_ = now; send_(0x01, nullptr, 0); }
        break;
      case ONLINE:
        if (now - t_hb_ >= 15000)  { t_hb_ = now; send_(0x00, nullptr, 0); }
        if (now - t_net_ >= 10000) { t_net_ = now; uint8_t s = net_state_; send_(0x03, &s, 1); }
        // Tieni viva l'ALTA FREQUENZA LAN (DP 19 = 1): la station torna a bassa frequenza da sola
        // dopo ~20s, quindi la ri-stimolo ogni 10s (come fa l'addon con cmd19+cmd17). Con l'HFR
        // acceso la station spinge TUTTO veloce da sola (batteria compresa), anche col 0x08 a 5s.
        if (now - t_hfr_ >= 10000) { t_hfr_ = now; write_val(0x0013, hfr_mode_); }  // ri-arma la TUA scelta HFR
        if (now - t_q_ >= q_ms_)   { t_q_ = now; send_(0x08, nullptr, 0); }          // query, intervallo dallo slider
        break;
    }
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
  uint8_t buf_[1200]; size_t len_ = 0;
  uint32_t t_hb_ = 0, t_pi_ = 0, t_net_ = 0, t_q_ = 0, t_hfr_ = 0;
  uint8_t net_state_ = 0x04;
  // deviceKey / WiFi MAC of YOUR station (from the BLE advertisement or the label).
  // The MCU just echoes it back in the net-status reply; fill in yours.
  uint8_t mac_[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint16_t pub_ = 0;

  void feed_(uint8_t c) {
    if (len_ == 0 && c != 0x55) return;
    if (len_ == 1 && c != 0xAA) { len_ = 0; if (c == 0x55) buf_[len_++] = 0x55; return; }
    if (len_ < sizeof(buf_)) buf_[len_++] = c;
    if (len_ >= 6) {
      size_t plen = ((size_t)buf_[4] << 8) | buf_[5];
      if (plen > sizeof(buf_) - 7) { len_ = 0; return; }
      if (len_ >= 6 + plen + 1) { handle_(buf_, 6 + plen + 1); len_ = 0; }
    }
  }

  static uint8_t ck_(const uint8_t *f, size_t n) { uint8_t s = 0; for (size_t i = 0; i < n; i++) s += f[i]; return s; }

  void send_(uint8_t cmd, const uint8_t *data, uint16_t dlen) {
    uint8_t h[6] = {0x55, 0xAA, 0x00, cmd, (uint8_t)(dlen >> 8), (uint8_t)(dlen & 0xFF)};
    uint8_t ck = ck_(h, 6);
    for (uint16_t i = 0; i < dlen; i++) ck += data[i];
    write_array(h, 6);
    if (dlen) write_array(data, dlen);
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
    if (ck_(f, n - 1) != f[n - 1]) return;
    uint8_t cmd = f[3];
    uint16_t dlen = ((uint16_t)f[4] << 8) | f[5];
    switch (cmd) {
      case 0x00:
        if (st_ == INIT) { st_ = HB_OK; ESP_LOGI("ps", "mainboard VIVO -> product-info"); send_(0x01, nullptr, 0); t_pi_ = millis(); t_hb_ = millis(); }
        break;
      case 0x01:
        if (st_ != ONLINE) { st_ = ONLINE; ESP_LOGI("ps", "product-info -> ONLINE, query 0x08"); send_(0x08, nullptr, 0); t_net_ = 0; t_q_ = 0; }
        break;
      case 0x1C: send_time_(); break;
      case 0x2D: { uint8_t r[7]; r[0] = 0x00; memcpy(r + 1, mac_, 6); send_(0x2D, r, 7); } break;
      case 0x2B: { uint8_t r = net_state_; send_(0x2B, &r, 1); } break;
      case 0x35: { uint8_t st = net_state_ >= 4 ? 3 : net_state_; uint8_t r[2] = {0x05, st}; send_(0x35, r, 2); } break;
      case 0x90: { uint8_t r[2] = {0x01, 0x00}; send_(0x90, r, 2); } break;
      case 0x07: {
        pub_ = 0;
        last_report_ms_ = millis();
        parse_(f + 6, dlen, 0);
        ESP_LOGI("ps", "report 0x07 (%u byte) -> %u valori", dlen, pub_);
      } break;
      default: break;
    }
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
    send_(0x1C, t, 8);
  }
};

// puntatore globale: gli switch/select/number di ESPHome scrivono qui
inline PowerstationModule *g_ps = nullptr;
