# Calcolo Distribuito SHA-256 su File (Client/Server)

## Descrizione

Questo progetto, sviluppato come esercizio per il corso di **Sistemi Operativi**, implementa un sistema **client-server** per il calcolo concorrente dell’hash crittografico **SHA-256** su file arbitrari, utilizzando il linguaggio C e le API POSIX.

Il client invia richieste di calcolo hash di uno o più file, il server riceve le richieste tramite una FIFO pubblica, elabora ogni richiesta sfruttando la concorrenza tramite thread, memorizza i risultati in una cache interna e risponde tramite FIFO temporanee dedicate.

---

## Funzionalità

- Calcolo hash SHA-256 di file arbitrari tramite libreria OpenSSL.
- Comunicazione client-server tramite FIFO/named pipe POSIX.
- Gestione concorrente delle richieste tramite thread (pthread).
- Caching dei risultati per evitare ricalcoli ridondanti.
- Ordinamento delle risposte per dimensione del file.
- Gestione robusta di richieste concorrenti e duplicati.

---

## Architettura

### Componenti

- **Client** (`collector_client.c`):  
  - Prepara richieste per ciascun file passato come argomento.
  - Crea una FIFO di risposta univoca per ogni richiesta.
  - Invia al server la richiesta composta da percorso file e nome FIFO risposta.
  - Attende il risultato sulla propria FIFO, lo raccoglie, lo ordina per dimensione file e lo stampa.

- **Server** (`server.c`):  
  - Riceve richieste tramite una FIFO pubblica (`/tmp/fifo_in`).
  - Inserisce le richieste in una coda ordinata per dimensione file.
  - Gestisce un pool di thread lavoratori per l’elaborazione concorrente (limite configurabile).
  - Implementa una **cache** dei risultati già calcolati e una struttura per tracciare i calcoli in corso (evita duplicazioni di lavoro).
  - Risponde al client scrivendo l’hash nella FIFO di risposta dedicata.

---

## Dettagli Tecnici

- **FIFO/Nomi temporanei**: Ogni client crea una FIFO personale per ricevere la risposta del server, garantendo separazione e gestione di più richieste parallele.
- **SHA-256 con OpenSSL**: Uso delle API `SHA256_Init`, `SHA256_Update`, `SHA256_Final` per processare i file in blocchi.
- **Ordinamento richieste**: La coda delle richieste nel server è ordinata per dimensione file (priorità a file più piccoli).
- **Concorrenza & Sincronizzazione**: Uso di mutex, variabili di condizione e controllo del numero massimo di thread.
- **Cache**: Gli hash già calcolati sono memorizzati in cache per ottimizzare le prestazioni e ridurre i tempi di risposta.
- **Gestione richieste duplicate**: Un’unica elaborazione per richieste concorrenti sullo stesso file, tutti i client ricevono lo stesso risultato senza ricalcoli.

---

## Utilizzo

### Compilazione

```bash
gcc -o collector_client collector_client.c -lpthread
gcc -o server server.c -lpthread -lssl -lcrypto
```

### Esecuzione

1. **Avviare il server**
   ```bash
   ./server
   ```

2. **Inviare richieste dal client**
   ```bash
   ./collector_client file1.txt file2.txt file3.txt
   ```

   - Il client stamperà a schermo, ordinati per dimensione, i percorsi dei file con rispettiva size (in byte) e hash SHA-256.

---

## Testing

Sono disponibili script di test automatico (`run_tests.sh`) che verificano:

- Calcolo hash singolo e multiplo
- Ordinamento delle risposte
- Gestione file inesistenti
- Efficienza della cache
- Robustezza sotto carico concorrente

Dopo ogni test viene eseguita una pulizia automatica delle FIFO e dei file temporanei.

---

## Dipendenze

- POSIX (Linux/Unix-like)
- Libreria OpenSSL (per SHA-256)
- pthread

---

## Note e Possibili Estensioni

- La dimensione massima di cache e di thread è configurabile.
- Possibili sviluppi: politica di rimpiazzo LRU per la cache, distribuzione su più server, gestione remota via socket TCP/IP.

---

## Autore

Mattia Bortolaso  
15 luglio 2025

---

> **Nota:** Questo progetto è un esempio didattico per la gestione di concorrenza, comunicazione interprocesso e ottimizzazione tramite caching in ambiente POSIX.
