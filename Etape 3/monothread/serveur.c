#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/file.h> 

#define BUFFER_SIZE 516
#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5
#define TFTP_PORT 66
#define TIMEOUT_SEC 5
// Prototypes for functions that handle RRQ and WRQ
void handleRRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode);
void handleWRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode);

int main() {
     int sockfd;
    struct sockaddr_in serverAddr, clientAddr;
    char buffer[BUFFER_SIZE];
    char serverIP[INET_ADDRSTRLEN];  // Buffer for the IP address
    int serverPort;                  // Variable for the server port
    socklen_t clientAddrLen = sizeof(clientAddr);

    // Ask for server IP and port from the user
    printf("Enter server IP address: ");
    scanf("%s", serverIP);
    printf("Enter server port (default is %d): ", TFTP_PORT);
    scanf("%d", &serverPort);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);
    if (inet_pton(AF_INET, serverIP, &serverAddr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address/ Address not supported \n");
        exit(EXIT_FAILURE);
    }

    if (bind(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    printf("TFTP Server started on %s:%d...\n", serverIP, serverPort);

     // Configuration de select()
    fd_set master_fds, read_fds;
    FD_ZERO(&master_fds);
    FD_SET(sockfd, &master_fds);
    int fdmax = sockfd;

    while (1) {
        read_fds = master_fds;
        if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
            perror("select");
            exit(4);
        }

        if (FD_ISSET(sockfd, &read_fds)) {
            int receivedBytes = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
                                         (struct sockaddr *)&clientAddr, &clientAddrLen);
            if (receivedBytes < 0) {
                perror("recvfrom failed");
                continue;
            }

            // Traitement des requêtes
            int opcode = buffer[1];
            char *filename = buffer + 2;
            char *mode = filename + strlen(filename) + 1;

            switch (opcode) {
                case OP_RRQ:
                    handleRRQ(sockfd, &clientAddr, clientAddrLen, filename, mode);
                    break;
                case OP_WRQ:
                    handleWRQ(sockfd, &clientAddr, clientAddrLen, filename, mode);
                    break;
                default:
                    fprintf(stderr, "Unsupported request. Only RRQ and WRQ are supported.\n");
                    break;
            }
        }
    }

    close(sockfd);
    return 0;
}


void sendError(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, int errorCode, const char *errorMessage) {
    char buffer[BUFFER_SIZE];
    int messageLength = sprintf(buffer, "%c%c%c%c%s%c", 0, OP_ERROR, 0, errorCode, errorMessage, 0);
    sendto(sockfd, buffer, messageLength + 1, 0, (struct sockaddr *)clientAddr, clientAddrLen);
}

void sendACK(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, int blockNum) {
    char ackPacket[4];
    ackPacket[0] = 0;
    ackPacket[1] = OP_ACK;
    ackPacket[2] = (blockNum >> 8) & 0xFF;
    ackPacket[3] = blockNum & 0xFF;
    sendto(sockfd, ackPacket, 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);
}

// Implement the handleRRQ function to handle read requests
void handleRRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode) {
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        perror("File not found or cannot be opened");
        sendError(sockfd, clientAddr, clientAddrLen, 1, "File not found");
        return;
    }

    // Verrouillage du fichier en lecture
    if (flock(fileno(file), LOCK_SH) == -1) { // LOCK_SH pour un verrou partagé (lecture)
        perror("flock failed");
        fclose(file);
        return;
    }

    char dataBuffer[BUFFER_SIZE];
    int bytesRead, blockNum = 1;
    fd_set readfds;
    struct timeval tv;

    do {
        bytesRead = fread(dataBuffer + 4, 1, 512, file);
        if (ferror(file)) {
            sendError(sockfd, clientAddr, clientAddrLen, 0, "Error reading the file");
            break;
        }

        dataBuffer[0] = 0;
        dataBuffer[1] = OP_DATA;
        dataBuffer[2] = (blockNum >> 8) & 0xFF;
        dataBuffer[3] = blockNum & 0xFF;
        
        sendto(sockfd, dataBuffer, bytesRead + 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;

        int ackReceived = 0;
        while (!ackReceived) {
            int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
            if (rv > 0) {
                int len = recvfrom(sockfd, dataBuffer, BUFFER_SIZE, 0, (struct sockaddr *)clientAddr, &clientAddrLen);
                if (len >= 4 && dataBuffer[1] == OP_ACK) {
                    int ackBlockNum = (dataBuffer[2] << 8) | dataBuffer[3];
                    if (ackBlockNum == blockNum) {
                        ackReceived = 1;
                    }
                }
            } else if (rv == 0) {
                // Timeout occurred, retransmit the DATA packet
                sendto(sockfd, dataBuffer, bytesRead + 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);
            } else {
                // Error occurred
                perror("Error or timeout on ACK reception");
                break;
            }
        }

        blockNum++;
    } while (bytesRead == 512); // Continue if last read was a full block

    flock(fileno(file), LOCK_UN); // Déverrouillage du fichier
    fclose(file);
}

// Implement the handleWRQ function to handle write requests

void handleWRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode) {
    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        perror("Cannot open file");
        sendError(sockfd, clientAddr, clientAddrLen, 1, "Could not open file for writing");
        return;
    }

    // Verrouillage du fichier en écriture
    if (flock(fileno(file), LOCK_EX) == -1) { // LOCK_EX pour un verrou exclusif (écriture)
        perror("flock failed");
        fclose(file);
        return;
    }

    int blockNum = 0;
    char buffer[BUFFER_SIZE];

    // Envoi du premier ACK pour confirmer la réception de la requête WRQ
    sendACK(sockfd, clientAddr, clientAddrLen, 0);

    fd_set readfds;
    struct timeval tv;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;

        int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        if (rv < 0) {
            perror("Select error");
            break;
        } else if (rv == 0) {
            printf("Timeout waiting for data packet\n");
            break; // Timeout atteint sans recevoir de paquet DATA
        }

        int recvLen = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)clientAddr, &clientAddrLen);
        if (recvLen < 4) { // Vérifie que le paquet est suffisamment grand pour contenir un en-tête
            sendError(sockfd, clientAddr, clientAddrLen, 0, "Received packet is too short");
            break;
        }

        int opcode = buffer[1];
        int receivedBlockNum = (buffer[2] << 8) + buffer[3];

        if (opcode != OP_DATA) {
            sendError(sockfd, clientAddr, clientAddrLen, 0, "Expected DATA packet");
            break;
        }

        if (receivedBlockNum == blockNum + 1) {
            size_t writtenBytes = fwrite(buffer + 4, 1, recvLen - 4, file);
            if (writtenBytes < (size_t)(recvLen - 4)) {
                sendError(sockfd, clientAddr, clientAddrLen, 0, "Failed to write data to file");
                break;
            }

            sendACK(sockfd, clientAddr, clientAddrLen, receivedBlockNum);
            blockNum = receivedBlockNum; // Mise à jour du numéro de bloc attendu pour le prochain paquet

            if (recvLen < 512) {
                printf("Last data packet received\n");
                break; // Si la longueur du paquet DATA est < 512, c'est le dernier paquet
            }
        } else {
            // Si le numéro de bloc ne correspond pas à celui attendu, ignorez le paquet ou traitez l'erreur
            printf("Unexpected block number received: %d\n", receivedBlockNum);
        }
    }

    flock(fileno(file), LOCK_UN); // Déverrouillage du fichier
    fclose(file);
}