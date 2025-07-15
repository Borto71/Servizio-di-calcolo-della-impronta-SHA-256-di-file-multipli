// collector_client.c – Client collettore con ordinamento per dimensione
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define FIFO_IN "/tmp/fifo_in"

typedef struct {
    char filepath[1024];
    char fifo_path[64];
    char hash[65];
    off_t filesize;
} RequestEntry;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <file1> <file2> ... <fileN>\n", argv[0]);
        return 1;
    }

    int num_files = argc - 1;
    RequestEntry *requests = malloc(num_files * sizeof(RequestEntry));
    if (!requests) {
        perror("malloc");
        return 1;
    }

    // Prepara richieste
    for (int i = 0; i < num_files; i++) {
        strncpy(requests[i].filepath, argv[i + 1], sizeof(requests[i].filepath));
        requests[i].filepath[sizeof(requests[i].filepath) - 1] = '\0';

        snprintf(requests[i].fifo_path, sizeof(requests[i].fifo_path), "/tmp/fifo_client_%d_%d", getpid(), i);
        if (mkfifo(requests[i].fifo_path, 0666) < 0) {
            perror("mkfifo");
            return 1;
        }

        struct stat st;
        if (stat(requests[i].filepath, &st) < 0) {
            perror("stat");
            return 1;
        }
        requests[i].filesize = st.st_size;

        // Invia richiesta al server
        int fd_out = open(FIFO_IN, O_WRONLY);
        if (fd_out < 0) {
            perror("open FIFO_IN");
            return 1;
        }

        char message[2048];
        snprintf(message, sizeof(message), "%s::%s", requests[i].filepath, requests[i].fifo_path);
        write(fd_out, message, strlen(message) + 1);
        close(fd_out);
    }

    // Riceve tutte le risposte
    for (int i = 0; i < num_files; i++) {
        int fd_in = open(requests[i].fifo_path, O_RDONLY);
        if (fd_in < 0) {
            perror("open fifo_client");
            continue;
        }

        ssize_t len = read(fd_in, requests[i].hash, sizeof(requests[i].hash));
        if (len <= 0) {
            strncpy(requests[i].hash, "ERRORE", sizeof(requests[i].hash));
        }
        close(fd_in);
        unlink(requests[i].fifo_path);
    }

    // Ordina per dimensione crescente
    for (int i = 0; i < num_files - 1; i++) {
        for (int j = i + 1; j < num_files; j++) {
            if (requests[i].filesize > requests[j].filesize) {
                RequestEntry tmp = requests[i];
                requests[i] = requests[j];
                requests[j] = tmp;
            }
        }
    }

    // Stampa risultati ordinati
    for (int i = 0; i < num_files; i++) {
        printf("%s (%ld byte):\nSHA-256: %s\n\n", requests[i].filepath, requests[i].filesize, requests[i].hash);
    }

    free(requests);
    return 0;
}
