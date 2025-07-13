// SERVER.C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <openssl/sha.h>

#define SERVER_FIFO "/tmp/server_fifo"

void digest_file(const char *filename, uint8_t *hash); // dichiarazione funzione SHA256

// Struttura richiesta ricevuta
typedef struct {
    char filepath[256];     // percorso del file da leggere
    char client_fifo[256];  // percorso FIFO privata del client
} request_t;

// Thread handler per ogni richiesta
void *handle_request(void *arg) {
    request_t req = *(request_t *)arg;
    free(arg);

    uint8_t hash[32];
    digest_file(req.filepath, hash);

    char char_hash[65];
    for (int i = 0; i < 32; i++)
        sprintf(char_hash + (i * 2), "%02x", hash[i]);
    char_hash[64] = '\0';

    // Scrive il risultato sulla FIFO privata del client
    int fd = open(req.client_fifo, O_WRONLY);
    if (fd >= 0) {
        write(fd, char_hash, sizeof(char_hash));
        close(fd);
    } else {
        perror("Errore apertura FIFO client");
    }

    return NULL;
}

int main() {
    // Creazione FIFO server
    mkfifo(SERVER_FIFO, 0666);
    printf("[SERVER] In ascolto su %s...\n", SERVER_FIFO);

    int fd = open(SERVER_FIFO, O_RDONLY);
    while (1) {
        request_t *req = malloc(sizeof(request_t));
        if (read(fd, req, sizeof(request_t)) > 0) {
            pthread_t tid;
            pthread_create(&tid, NULL, handle_request, req);
            pthread_detach(tid); // evitiamo di dover fare join
        }
    }

    close(fd);
    unlink(SERVER_FIFO);
    return 0;
}
