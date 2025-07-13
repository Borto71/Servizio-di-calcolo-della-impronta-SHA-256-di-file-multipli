// CLIENT.C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SERVER_FIFO "/tmp/server_fifo"

typedef struct {
    char filepath[256];
    char client_fifo[256];
} request_t;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <percorso_file>\n", argv[0]);
        return 1;
    }

    // Creazione FIFO privata
    char client_fifo[256];
    sprintf(client_fifo, "/tmp/client_fifo_%d", getpid());
    mkfifo(client_fifo, 0666);

    // Composizione della richiesta
    request_t req;
    strcpy(req.filepath, argv[1]);
    strcpy(req.client_fifo, client_fifo);

    // Invio al server
    int server_fd = open(SERVER_FIFO, O_WRONLY);
    if (server_fd < 0) {
        perror("Errore apertura FIFO server");
        unlink(client_fifo);
        return 1;
    }
    write(server_fd, &req, sizeof(req));
    close(server_fd);

    // Attesa risposta
    int client_fd = open(client_fifo, O_RDONLY);
    char hash[65];
    if (read(client_fd, hash, sizeof(hash)) > 0) {
        printf("SHA-256 di '%s':\n%s\n", argv[1], hash);
    } else {
        printf("Errore nella lettura della risposta dal server.\n");
    }

    close(client_fd);
    unlink(client_fifo);
    return 0;
}
