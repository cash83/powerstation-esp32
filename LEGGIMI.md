# Versione 2026-11-23 — in italiano

I file nella radice (`powerstation.yaml`, `powerstation_module.h`) sono la versione attuale.
La beta dell'11 novembre resta in [`beta-2026-11-11/`](beta-2026-11-11/), quella del 4 novembre è nella cronologia git.
Dettagli completi in inglese nel [README](README.md).

## Il blocco dei ~40 minuti: cos'era e come si chiude

- Ogni ~606 s la scheda chiede al modulo il segnale Wi-Fi (`0x24`). **Una volta su quattro** (≈ 2426 s, i "40 minuti")
  fa un *controllo della rete*: chiede `0x2B` ("com'è la rete?") e `0x35` **ogni secondo** e smette di mandare i dati.
- Rispondere giusto **non basta**: l'ESP ha risposto `0x2B = 04` ("collegato al cloud") per 8 minuti di fila e la pausa non è finita.
- La pausa finisce **solo quando il modulo annuncia da solo** lo stato della rete `0x03 = 04`:
  ```
  ESP    -> 55 AA 00 03 00 01 04 07   (annuncio non richiesto: "sono collegato al cloud")
  scheda -> 55 AA 03 03 00 00 05      (ricevuto) — i dati ripartono ~110 ms dopo
  ```
- Il modulo originale FC41D manda lo `0x03` **solo quando la sua connessione cambia** (visto nel dump del suo firmware):
  quindi sta zitto, la pausa non finisce e i dati tornano solo quando il modulo cade e si ricollega. Ecco il blocco.

**La cura** (`powerstation_module.h`, `case 0x2B`): alla seconda `0x2B` entro 3 s l'ESP annuncia subito `0x03 = 04`, e basta.
Se 5 s dopo l'annuncio la scheda chiede ancora, finto Wi-Fi spento e riacceso (al massimo una volta ogni 2 minuti).
Nel registro: `PAUSA DEI 40 MINUTI …` e poi `PAUSA CHIUSA dopo X ms (bastato l'annuncio 0x03=04)`.

## Le altre novità

1. Trasmissione come il modulo vero: almeno 50 ms fra due frame, info-prodotto e stato rete con attesa della conferma
   (3 ritrasmissioni), stato rete solo quando cambia, heartbeat ogni 15 s, ripartenza completa dopo 5 heartbeat senza risposta.
2. Risposta a tutti i comandi a cui risponde il modulo vero (`0x22`, `0x82`, `0x81`, `0x24`, `0x0C`, `0x1C`, `0x0F`, `0x90`, `0x2B`, `0x35`, `0x2D`).
3. Mai parlare sopra la scheda: quello che l'ESP manda di sua iniziativa aspetta 300 ms di silenzio sulla seriale (massimo 3 s);
   le risposte alle domande partono subito.
4. Ricezione robusta: checksum, risincronizzazione sul `55 AA` successivo, fino a 1017 byte per frame, frame a metà tenuto fino a 3 s,
   buffer della UART a 8192 byte.
5. Cane da guardia: se per 25 s non arriva il gruppo batteria, finto Wi-Fi spento (3 s) → router (3 s) → cloud + richiesta dati;
   dopo 3 tentativi, ripartenza completa.
6. Nuovi comandi in Home Assistant: **Chiedi i dati**, **Ricollega Wi-Fi**, **Registro seriale completo**
   (attenzione: il registro contiene la chiave del dispositivo, toglierla prima di condividerlo), **Dati fermi**.
7. `powerstation.yaml`: ora da Home Assistant invece di SNTP, Wi-Fi a 15 dB con `power_save_mode: light`, registro solo avvisi,
   impostazioni ricordate dopo il riavvio. `flash_chip: gd` è quello della scheda usata qui: **toglilo se la tua ha un altro chip**.

## Cosa è stato misurato

Una sola stazione, 22–23 novembre 2026: tutte le pause dei 40 minuti, dalla sera del 22 al pomeriggio del 23, chiuse dal solo annuncio
(0,41 / 0,6 / 0,65 / 0,96 / 1,3 / 2,4 / 2,8 / 3,1 s), il finto Wi-Fi di riserva non è mai servito, l'ESP non si è mai riavviato
e l'allarme "Dati fermi" è rimasto spento tutta la notte.

**Non risolto:** qualche volta (10 e 17–18 novembre) la scheda è finita in un blocco *profondo* che si è sbloccato solo spegnendo e
riaccendendo la stazione. È un problema diverso dalla pausa dei 40 minuti.

## Chiave del dispositivo

Nella copia pubblica `mac_[6]` è a zeri: funziona lo stesso (la scheda la rimanda indietro senza controllarla).
Come trovare la propria è spiegato nel [README](README.md#setting-mac_6-optional--the-device-key).
