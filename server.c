#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>

// Nomi FIFO per comunicazione client-server
#define FIFO_IN  "/tmp/fifo_in"

#define MAX_MSG_SIZE 1024
#define MAX_CACHE_SIZE 100
#define MAX_QUEUE      100
#define MAX_THREADS    4   // Limite massimo di thread worker simultanei

// Struttura per la cache dei risultati (filepath -> hash calcolato)
typedef struct {
    char filepath[1024];
    char hash_string[65];
} CacheEntry;

static CacheEntry cache[MAX_CACHE_SIZE];
static int cache_size = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Struttura per rappresentare una richiesta nella coda (filepath + fifo client + dimensione file)
typedef struct {
    char request_str[1024];  // "filepath::fifo_client"
    off_t filesize;
} Request;

static Request request_queue[MAX_QUEUE];
static int queue_size = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;

// Struttura per tracciare elaborazioni in corso
typedef struct {
    char filepath[1024];
    int done;
    int wait_count;
    char hash_string[65];
    pthread_cond_t cond;
} InProgressEntry;

static InProgressEntry in_progress_list[MAX_QUEUE];
static int in_progress_count = 0;

// Controllo dei thread attivi
static pthread_mutex_t thread_count_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thread_available = PTHREAD_COND_INITIALIZER;
static int active_threads = 0;

// Prototipi
void digest_file(const char *filename, uint8_t *hash);
void cache_insert_unlocked(const char* path, const char* hash);
int cache_lookup(const char* path, char* hash_out);
int find_in_progress_index(const char* path);
void enqueue_request(const char* request_str, off_t filesize);
int dequeue_request(Request* out);
void* handle_request(void* arg);
void* dispatcher_thread(void* arg);


/**
 * Calcola l'hash SHA-256 del file specificato.
 * @param filename Percorso del file di cui calcolare l'hash.
 * @param hash Buffer di 32 byte dove verrà scritto il digest binario risultante.
 */
void digest_file(const char *filename, uint8_t *hash) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    // Apre il file in sola lettura
    int file = open(filename, O_RDONLY);
    if (file == -1) {
        perror("open file");
        return;  // Se il file non si apre, esce (errore gestito dal chiamante)
    }

    // Legge il file a blocchi e aggiorna il contesto SHA256
    unsigned char buffer[1024];
    ssize_t bytes;
    while ((bytes = read(file, buffer, sizeof(buffer))) > 0) {
        SHA256_Update(&ctx, buffer, bytes);
    }
    close(file);

    // Completa il calcolo dell'hash (digest finale)
    SHA256_Final(hash, &ctx);
}

/**
 * Inserisce un risultato (filepath-hash) nella cache.
 * ATTENZIONE: va chiamata con cache_mutex già bloccato (versione "unlocked").
 */
void cache_insert_unlocked(const char* path, const char* hash) {
    // Evita duplicati: controlla se il percorso è già presente
    for (int i = 0; i < cache_size; ++i) {
        if (strcmp(cache[i].filepath, path) == 0) {
            return;  // già in cache, non inserisce duplicato
        }
    }
    if (cache_size < MAX_CACHE_SIZE) {
        // Aggiunge una nuova voce in cache
        strncpy(cache[cache_size].filepath, path, sizeof(cache[cache_size].filepath));
        cache[cache_size].filepath[sizeof(cache[cache_size].filepath) - 1] = '\0';
        strncpy(cache[cache_size].hash_string, hash, sizeof(cache[cache_size].hash_string));
        cache[cache_size].hash_string[64] = '\0';
        printf("[DEBUG] Inserita in cache: %s → %s (Totale cache: %d)\n", path, hash, cache_size + 1);
        cache_size++;
    } else {
        // Cache piena: in questa implementazione non aggiungiamo nuove voci se la cache ha raggiunto la capacità massima.
        // (Opzionalmente si potrebbe rimuovere la voce meno recente o usare un criterio FIFO/LRU)
    }
}

/**
 * Cerca nella cache l'hash di un determinato file.
 * @param path Percorso del file da cercare.
 * @param hash_out Buffer (65 char) in cui, se trovato, verrà copiato l'hash esadecimale.
 * @return 1 se trovato (hash_out valorizzato), 0 se non presente in cache.
 */
int cache_lookup(const char* path, char* hash_out) {
    pthread_mutex_lock(&cache_mutex);
    for (int i = 0; i < cache_size; i++) {
        if (strcmp(cache[i].filepath, path) == 0) {
            printf("[DEBUG] Trovato in cache: %s → %s\n", cache[i].filepath, cache[i].hash_string);
            // Copia l'hash trovato e rilascia il mutex
            strcpy(hash_out, cache[i].hash_string);
            pthread_mutex_unlock(&cache_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&cache_mutex);
    return 0;  // non trovato
}

/**
 * Trova l'indice di un file nella lista in_progress.
 * @return Indice dell'entry se il file è presente *e ancora in corso* (done==0), altrimenti -1.
 */
int find_in_progress_index(const char* path) {
    for (int i = 0; i < in_progress_count; ++i) {
        if (strcmp(in_progress_list[i].filepath, path) == 0 && in_progress_list[i].done == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * Inserisce una nuova richiesta nella coda, ordinandola in base alla dimensione del file.
 * @param request_str Stringa "filepath::fifo_client" della richiesta.
 * @param filesize Dimensione del file (per ordinamento).
 */
void enqueue_request(const char* request_str, off_t filesize) {
    pthread_mutex_lock(&queue_mutex);

    if (queue_size >= MAX_QUEUE) {
        fprintf(stderr, "Coda delle richieste piena. Richiesta scartata.\n");
        pthread_mutex_unlock(&queue_mutex);
        return;
    }

    int i = queue_size;
    while (i > 0) {
        if (request_queue[i - 1].filesize > filesize) {
            // Sposta a destra se il file precedente è più grande
            request_queue[i] = request_queue[i - 1];
            i--;
        } else if (request_queue[i - 1].filesize == filesize &&
                   strcmp(request_queue[i - 1].request_str, request_str) > 0) {
            // Sposta a destra se le dimensioni sono uguali e ordine alfabetico maggiore
            request_queue[i] = request_queue[i - 1];
            i--;
        } else {
            break;
        }
    }

    strncpy(request_queue[i].request_str, request_str, sizeof(request_queue[i].request_str));
    request_queue[i].request_str[sizeof(request_queue[i].request_str) - 1] = '\0';
    request_queue[i].filesize = filesize;
    queue_size++;
    printf("[DEBUG] Richiesta accodata: %s (size: %ld). Coda attuale: %d\n", request_str, filesize, queue_size);

    // DEBUG opzionale:
    // printf("Inserita richiesta: %s (size: %ld) in posizione %d\n", request_str, filesize, i);

    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
}


/**
 * Estrae la prossima richiesta dalla coda (bloccante se la coda è vuota).
 * @param out Puntatore a Request dove memorizzare i dati estratti.
 * @return 1 se una richiesta è stata estratta correttamente.
 */
int dequeue_request(Request* out) {
    pthread_mutex_lock(&queue_mutex);
    // Attende finché la coda è vuota
    while (queue_size == 0) {
        pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }
    // Estrae la prima richiesta (più piccola)
    *out = request_queue[0];
    printf("[DEBUG] Richiesta estratta: %s (size: %ld). Coda residua: %d\n", out->request_str, out->filesize, queue_size);
    // Shift a sinistra delle restanti richieste in coda
    for (int i = 1; i < queue_size; i++) {
        request_queue[i - 1] = request_queue[i];
    }
    queue_size--;
    pthread_mutex_unlock(&queue_mutex);
    return 1;
}

/**
 * Gestisce l'elaborazione di una singola richiesta (funzione eseguita da ogni thread worker).
 * Riceve in ingresso una stringa allocata dinamicamente con formato "filepath::fifo_client".
 */
void* handle_request(void* arg) {
    char* input = (char*)arg;
    // Divide la stringa di input in "filepath" e "fifo_path" usando il separatore "::"
    char* sep = strstr(input, "::");
    if (!sep) {
        fprintf(stderr, "Richiesta malformata: %s\n", input);
        free(input);
        pthread_mutex_lock(&thread_count_mutex);
        active_threads--;
        pthread_cond_signal(&thread_available);
        pthread_mutex_unlock(&thread_count_mutex);
        return NULL;
    }
    *sep = '\0';
    char* filepath = input;
    char* fifo_path = sep + 2;

    char hash_string[65] = "";
    int need_compute = 0;

    pthread_mutex_lock(&cache_mutex);
    // 1. Controlla la cache per vedere se l'hash è già disponibile
    for (int i = 0; i < cache_size; ++i) {
        if (strcmp(cache[i].filepath, filepath) == 0) {
            strcpy(hash_string, cache[i].hash_string);
            need_compute = 0;
            printf("[DEBUG] [CACHE_HIT] %s servito dalla cache: %s\n", filepath, hash_string);
            break;
        }
    }
    if (hash_string[0] == '\0') {
        // Non trovato in cache
        need_compute = 1;
    }

    if (need_compute) {
        // 2. Controlla se un thread sta già calcolando l'hash di questo file
        int idx = find_in_progress_index(filepath);
        if (idx != -1) {
            printf("[DEBUG] [WAIT_ON_OTHER] Attendo hash per %s da altro thread...\n", filepath);
            in_progress_list[idx].wait_count++;
            while (in_progress_list[idx].done == 0) {
                pthread_cond_wait(&in_progress_list[idx].cond, &cache_mutex);
            }
            // Risultato pronto: copia l'hash calcolato dal primo thread
            strcpy(hash_string, in_progress_list[idx].hash_string);
            printf("[DEBUG] [WAIT_ON_OTHER_DONE] Hash per %s ricevuto da altro thread: %s\n", filepath, hash_string);
            in_progress_list[idx].wait_count--;
            if (in_progress_list[idx].done == 1 && in_progress_list[idx].wait_count == 0) {
                pthread_cond_destroy(&in_progress_list[idx].cond);
                in_progress_list[idx] = in_progress_list[in_progress_count - 1];
                in_progress_count--;
            }
            pthread_mutex_unlock(&cache_mutex);
        } else {
            // 3. Nessun altro sta elaborando questo file: preparati a calcolarlo
            if (in_progress_count >= MAX_QUEUE) {
                for (int j = 0; j < in_progress_count; ++j) {
                    if (in_progress_list[j].done == 1) {
                        pthread_cond_destroy(&in_progress_list[j].cond);
                        in_progress_list[j] = in_progress_list[in_progress_count - 1];
                        in_progress_count--;
                        j--;
                    }
                }
                if (in_progress_count >= MAX_QUEUE) {
                    fprintf(stderr, "Troppe richieste in elaborazione: impossibile gestire %s\n", filepath);
                    pthread_mutex_unlock(&cache_mutex);
                    free(input);
                    pthread_mutex_lock(&thread_count_mutex);
                    active_threads--;
                    pthread_cond_signal(&thread_available);
                    pthread_mutex_unlock(&thread_count_mutex);
                    return NULL;
                }
            }
            int new_idx = in_progress_count++;
            strncpy(in_progress_list[new_idx].filepath, filepath, sizeof(in_progress_list[new_idx].filepath));
            in_progress_list[new_idx].filepath[sizeof(in_progress_list[new_idx].filepath) - 1] = '\0';
            in_progress_list[new_idx].done = 0;
            in_progress_list[new_idx].wait_count = 0;
            pthread_cond_init(&in_progress_list[new_idx].cond, NULL);
            pthread_mutex_unlock(&cache_mutex);

            // --- Calcolo effettivo ---
            printf("[DEBUG] [HASH_CALC] Calcolo hash per %s...\n", filepath);
            uint8_t hash[32];
            digest_file(filepath, hash);
            for (int i = 0; i < 32; ++i) {
                sprintf(hash_string + (i * 2), "%02x", hash[i]);
            }
            hash_string[64] = '\0';

            // Rientra in sezione critica per aggiornare la cache e lo stato condiviso
            pthread_mutex_lock(&cache_mutex);
            cache_insert_unlocked(filepath, hash_string);
            int comp_idx = -1;
            for (int j = 0; j < in_progress_count; ++j) {
                if (strcmp(in_progress_list[j].filepath, filepath) == 0 && in_progress_list[j].done == 0) {
                    comp_idx = j;
                    break;
                }
            }
            if (comp_idx != -1) {
                in_progress_list[comp_idx].done = 1;
                strcpy(in_progress_list[comp_idx].hash_string, hash_string);
                if (in_progress_list[comp_idx].wait_count > 0) {
                    pthread_cond_broadcast(&in_progress_list[comp_idx].cond);
                } else {
                    pthread_cond_destroy(&in_progress_list[comp_idx].cond);
                    in_progress_list[comp_idx] = in_progress_list[in_progress_count - 1];
                    in_progress_count--;
                }
            }
            pthread_mutex_unlock(&cache_mutex);
            printf("[DEBUG] [HASH_CALC_DONE] Hash calcolato per %s: %s\n", filepath, hash_string);
        }
    } else {
        // Risultato trovato in cache
        pthread_mutex_unlock(&cache_mutex);
    }

    int fd_out = open(fifo_path, O_WRONLY);
    if (fd_out < 0) {
        perror("open FIFO client per risposta");
        free(input);
        pthread_mutex_lock(&thread_count_mutex);
        active_threads--;
        pthread_cond_signal(&thread_available);
        pthread_mutex_unlock(&thread_count_mutex);
        return NULL;
    }
    write(fd_out, hash_string, strlen(hash_string) + 1);
    close(fd_out);

    free(input);
    pthread_mutex_lock(&thread_count_mutex);
    active_threads--;
    pthread_cond_signal(&thread_available);
    pthread_mutex_unlock(&thread_count_mutex);
    return NULL;
}

/**
 * Thread dispatcher: estrae richieste dalla coda e crea i thread worker rispettando il limite MAX_THREADS.
 */
void* dispatcher_thread(void* arg) {
    char buffer[MAX_MSG_SIZE];
    // Apri FIFO in lettura/scrittura per evitare blocchi
    int fd_in = open(FIFO_IN, O_RDWR);
    if (fd_in < 0) {
        perror("open FIFO_IN");
        exit(1);
    }

    while (1) {
        ssize_t len = read(fd_in, buffer, sizeof(buffer) - 1);
        if (len <= 0) continue;
        buffer[len] = '\0';

        char *ptr = buffer;
        while (ptr < buffer + len) {
            size_t rem = strlen(ptr);
            if (rem == 0) { ptr++; continue; }

            char *sep = strstr(ptr, "::");
            if (!sep) {
                fprintf(stderr, "Richiesta malformata: %s\n", ptr);
                break;
            }
            *sep = '\0';
            char *filepath   = ptr;
            char *fifo_client = sep + 2;

            printf("[DEBUG] Letto messaggio: %s → %s\n", filepath, fifo_client);

            // Costruisci stringa di richiesta completa
            char combined[MAX_MSG_SIZE];
            snprintf(combined, sizeof(combined), "%s::%s", filepath, fifo_client);

            // Ottieni dimensione del file
            struct stat st;
            if (stat(filepath, &st) < 0) {
                perror("stat file");
                ptr += rem + 1;
                continue;
            }

            // Enqueue richiesta
            enqueue_request(combined, st.st_size);

            // Dispatch: aspetta slot thread
            pthread_mutex_lock(&thread_count_mutex);
            while (active_threads >= MAX_THREADS) {
                pthread_cond_wait(&thread_available, &thread_count_mutex);
            }
            active_threads++;
            pthread_mutex_unlock(&thread_count_mutex);

            // Prendi richiesta dalla coda
            Request req;
            if (dequeue_request(&req)) {
                // Duplica la stringa per il worker
                char *arg = strdup(req.request_str);
                pthread_t tid;
                pthread_create(&tid, NULL, handle_request, arg);
                pthread_detach(tid);
            }

            ptr += rem + 1;
        }
    }

    close(fd_in);
    return NULL;
}

int main() {
    unlink(FIFO_IN);
    if (mkfifo(FIFO_IN, 0666) < 0) {
        perror("mkfifo");
        return 1;
    }

    printf("Server in ascolto su %s...\n", FIFO_IN);

    pthread_t dispatcher;
    pthread_create(&dispatcher, NULL, dispatcher_thread, NULL);
    pthread_join(dispatcher, NULL);

    unlink(FIFO_IN);
    return 0;
}
