# Beta 11/11/2026 — watchdog di stallo, seriale fedele al modulo, HFR riacceso su annuncio

Questa cartella è una **copia**: i file nella radice del repository non sono stati toccati e
restano la versione che ha girato dal 4 al 10 novembre 2026. Usa questa se vuoi le modifiche qui sotto.

Per installarla copia `powerstation.yaml` e `powerstation_module.h` di questa cartella sopra quelli
della cartella di ESPHome e carica il firmware come al solito.
Poi imposta `mac_[6]` in `powerstation_module.h` con la chiave del tuo apparecchio, come spiegato nel README principale (lasciarla a zero funziona lo stesso).

## Cosa cambia

**1. Trasmissione come il modulo originale** (ricavata dall'analisi del firmware del modulo originale)
- Mai due frame attaccati: almeno 50 ms fra uno e l'altro.
- Info-prodotto (`0x01`) e stato rete (`0x03`) aspettano la conferma del mainboard e vengono
  ritrasmessi fino a 3 volte, a 1 s di distanza.
- Lo stato rete parte solo quando cambia, non a orologio.

**2. Risposta a tutti i comandi a cui risponde il modulo originale**
`0x22` report sincrono (confermato con `0x23`), `0x82`, `0x81`, `0x24` segnale Wi-Fi, `0x0C` ora GMT,
`0x1C` ora locale, `0x0F` memoria libera, `0x90` indirizzo IP, `0x2B`, `0x35`, `0x2D`.
Qualunque altro comando del mainboard finisce nel log come avviso.

**3. Heartbeat come il modulo originale**
Ogni 15 s una volta in linea. Dopo 5 heartbeat di fila senza risposta e senza report si rifà tutta
la sequenza di avvio (heartbeat, info-prodotto, stato rete, query): è quello che fa il modulo
originale riavviandosi.

**4. Watchdog sui dati fermi**
Se in linea non arriva nessun report vero (50 byte o più) per 30 s:
1. due volte: ciclo rapido dello stato rete (`0x02` "non connesso", dopo 2 s lo stato vero più una query);
2. due volte: finto Wi-Fi spento e riacceso (`0x02` tenuto 10 s in silenzio, poi `0x03`, dopo 3 s lo
   stato vero più una query);
3. ripartenza completa della sequenza di avvio.

**5. Ricezione che si riaggancia**
Il checksum si controlla mentre si riceve. Se intestazione, lunghezza o checksum non tornano si
scarta solo l'intestazione sbagliata e si cerca il prossimo `55 AA` nei byte già arrivati, così il
frame buono che segue non va perso. Lunghezza massima dei dati 1017 byte, come nel modulo
originale. Un frame rimasto a metà per più di 0,5 s viene scartato. I frame scartati vengono
contati e scritti nel log (al massimo una riga ogni 30 s).

**6. Alta frequenza (HFR, datapoint 19) riaccesa quando il mainboard annuncia di averla spenta**
Misurato sul filo:
- il mainboard spegne l'HFR **30,3 s dopo l'accensione** (13 volte su 13);
- riscrivere 1 mentre è già 1 **non** lo allunga;
- quando scade, il mainboard lo annuncia con un report corto: datapoint 19 = 0.

Il firmware vecchio riscriveva l'HFR ogni 10 s: scritture tutte a vuoto, e l'HFR restava spento fino
a 10 s ogni 30. Ora l'HFR si scrive appena in linea e poi solo quando il mainboard annuncia lo zero
(al massimo una volta ogni 3 s), con una rete di sicurezza a 45 s nel caso un annuncio vada perso.
Una scrittura ogni 30 s invece di tre o quattro, e l'HFR resta spento solo per qualche decimo di secondo.

**7. Log**
I report corti (sotto i 50 byte) vengono scritti con il contenuto in esadecimale:
`id (2 byte) · tipo (1) · lunghezza (2) · valore`.

**8. `powerstation.yaml`**
- Lo slider "Intervallo aggiornamento" viene applicato già all'avvio (prima solo dopo averlo toccato).
- Wi-Fi `output_power: 12dB`.
- `esp32 > framework > advanced > flash_chip: gd`. Vale per il chip di memoria della scheda usata
  qui (lo suggerisce ESPHome nel log di avvio per liberare IRAM). **Togli queste due righe se la tua
  scheda ha un chip diverso.**

## Cosa è stato misurato e cosa no

- Con lo slider a 7 s: un gruppo completo di report per ogni query, batteria compresa, nessun buco,
  nessun avviso, nessun frame scartato.
- Sono catture di log di pochi minuti l'una, su un solo apparecchio, nella notte dell'11 novembre
  2026. Questa versione **non** ha ancora girato per giorni come quella nella radice.
- Il 10 novembre il mainboard era finito in uno stato degradato (serviva la porta del modulo circa
  una volta al minuto). L'ha sbloccato solo lo spegnimento e la riaccensione della stazione, con lo
  stesso firmware. La causa non è nota; il watchdog serve a riprendersi da soli se ricapita, ma da
  allora non è più successo, quindi gli ultimi gradini del watchdog non sono provati sul campo.
