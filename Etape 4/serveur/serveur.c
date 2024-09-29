#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

#define BUFFER_SIZE 516
#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5
#define TFTP_PORT 66
#define TIMEOUT_SEC 5
#define OP_OACK 6 // Ajoutez cette ligne si OP_OACK n'est pas déjà défini
#define OPTION_BIGFILE "bigfile"
#define MAX_RETRIES 5

// Prototypes for functions that handle RRQ and WRQ
void handleRRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode);
// Correction dans la définition de la fonction handleWRQ
void handleWRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char* filename, const char* mode);

void sendError(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, int errorCode, const char *errorMsg);
int waitForAck(int sockfd, struct sockaddr_in *clientAddr, socklen_t *clientAddrLen, unsigned int expectedBlockNum);
// Déclaration de sendACK (ajoutez-la au début du fichier ou dans un fichier d'en-tête inclus)
void sendACK(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, unsigned int blockNum);



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

// Implement the handleRRQ function to handle read requests
// Implement the handleRRQ function to handle read requests with bigfile support
void handleRRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode) {
    FILE *file;
    char buffer[BUFFER_SIZE];
    unsigned int blockNum = 1; // Commence à 1
    size_t bytesRead = 0;

    file = fopen(filename, "rb");
    if (!file) {
        sendError(sockfd, clientAddr, clientAddrLen, 1, "File not found");
        return;
    }
    // Avant la boucle d'envoi du fichier
if (strcmp(mode, "netascii") == 0) {
    // Conversion de '\n' à '\r\n' pour l'envoi
    int c;
    bytesRead = 0;
    while (bytesRead < 512 && (c = fgetc(file)) != EOF) {
        if (c == '\n') {
            if (bytesRead < 511) { // Assurez-vous d'avoir de l'espace pour 2 caractères
                buffer[4 + bytesRead++] = '\r';
            }
            buffer[4 + bytesRead++] = '\n';
        } else {
            buffer[4 + bytesRead++] = c;
        }
    }
} else {
    // Mode octet, lecture directe
    bytesRead = fread(buffer + 4, 1, 512, file);
}

    // Traitement selon le mode de transfert
    int netascii = (strcmp(mode, "netascii") == 0);

    while (1) {
        if (netascii) {
            int c, prevC = EOF;
            bytesRead = 0;
            while (bytesRead < 512) {
                c = fgetc(file);
                if (c == '\n' && prevC != '\r') {
                    buffer[4 + bytesRead++] = '\r';
                }
                if (c == EOF) break;
                buffer[4 + bytesRead++] = c;
                prevC = c;
            }
        } else {
            bytesRead = fread(buffer + 4, 1, 512, file);
        }

        // Préparation du paquet DATA
        buffer[0] = 0;
        buffer[1] = OP_DATA;
        buffer[2] = (blockNum >> 8) & 0xFF;
        buffer[3] = blockNum & 0xFF;

        // Envoi du paquet DATA et attente d'ACK
        int ackReceived = 0, retries = 0;
        while (!ackReceived && retries < MAX_RETRIES) {
            sendto(sockfd, buffer, bytesRead + 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);
            ackReceived = waitForAck(sockfd, clientAddr, &clientAddrLen, blockNum);
            retries++;
        }

        if (!ackReceived) {
            sendError(sockfd, clientAddr, clientAddrLen, 0, "Max retries reached, transfer aborted");
            break;
        }

        if (bytesRead < 512) { // Dernier bloc
            break;
        }

        blockNum = (blockNum == 65535) ? 1 : blockNum + 1; // Gestion du roll-over
    }

    fclose(file);
}


void handleWRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char* filename, const char* mode) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        sendError(sockfd, clientAddr, clientAddrLen, 2, "Cannot open file for writing");
        return;
    }

    unsigned int blockNum = 0;
    sendACK(sockfd, clientAddr, clientAddrLen, blockNum);

    char buffer[BUFFER_SIZE];
    int lastPacketSize = 0;

    while (1) {
        struct sockaddr_in fromAddr;
        socklen_t fromAddrLen = sizeof(fromAddr);
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;

        int retval = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        if (retval > 0) {
            ssize_t receivedBytes = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&fromAddr, &fromAddrLen);
            if (receivedBytes < 0) {
                perror("recvfrom failed");
                break;
            }

            if (buffer[1] == OP_DATA) {
                unsigned int receivedBlockNum = ((unsigned char)buffer[2] << 8) + (unsigned char)buffer[3];
                if (receivedBlockNum == (blockNum + 1)) {
                    fwrite(buffer + 4, 1, receivedBytes - 4, file);
                    blockNum = receivedBlockNum;
                    sendACK(sockfd, clientAddr, clientAddrLen, blockNum);

                    if (receivedBytes < BUFFER_SIZE) {
                        lastPacketSize = 1;
                        break;
                    }
                }
            } else if (buffer[1] == OP_ERROR) {
                fprintf(stderr, "Error packet received\n");
                break;
            }
        } else if (retval == 0) {
            printf("Timeout waiting for block %u\n", blockNum + 1);
            sendACK(sockfd, clientAddr, clientAddrLen, blockNum);
        } else {
            perror("Select error");
            break;
        }
    }

    fclose(file);
}


int waitForAck(int sockfd, struct sockaddr_in *clientAddr, socklen_t *clientAddrLen, unsigned int expectedBlockNum) {
    char buffer[4]; // Taille suffisante pour un paquet ACK
    struct timeval tv;
    fd_set readfds;

    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    // Configurer le timeout
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;

    // Attendre la réponse
    if (select(sockfd + 1, &readfds, NULL, NULL, &tv) > 0) {
        // Un paquet est arrivé, vérifier s'il s'agit d'un ACK
        if (recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)clientAddr, clientAddrLen) >= 4) {
            // Vérifier l'opcode et le numéro de bloc
            if (buffer[1] == OP_ACK) {
                unsigned int blockNum = ((unsigned char)buffer[2] << 8) | (unsigned char)buffer[3];
                if (blockNum == expectedBlockNum) {
                    return 1; // C'est l'ACK attendu
                }
            }
        }
    }

    return 0; // Timeout ou ACK incorrect
}




void sendError(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, int errorCode, const char *errorMsg) {
    char buffer[BUFFER_SIZE];
    int errorMsgLength = strlen(errorMsg) + 1; // Inclure le byte null de fin

    // Construire le paquet d'erreur
    buffer[0] = 0; // Byte zéro pour TFTP
    buffer[1] = OP_ERROR; // Opcode pour un message d'erreur
    buffer[2] = (errorCode >> 8) & 0xFF; // Byte de poids fort du code d'erreur
    buffer[3] = errorCode & 0xFF; // Byte de poids faible du code d'erreur
    strcpy(buffer + 4, errorMsg); // Copier le message d'erreur

    // Envoyer le paquet d'erreur
    sendto(sockfd, buffer, errorMsgLength + 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);
}

void sendACK(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, unsigned int blockNum) {
    unsigned char ackPacket[4]; // Paquet ACK a une taille fixe de 4 octets

    // Remplir le paquet ACK avec les valeurs appropriées
    ackPacket[0] = 0;                // Le premier octet est toujours zéro
    ackPacket[1] = OP_ACK;           // Le second octet est le code d'opération pour ACK
    ackPacket[2] = (blockNum >> 8) & 0xFF;  // Byte de poids fort du numéro de bloc
    ackPacket[3] = blockNum & 0xFF;         // Byte de poids faible du numéro de bloc

    // Envoyer le paquet ACK au client
    if (sendto(sockfd, ackPacket, sizeof(ackPacket), 0, (struct sockaddr *)clientAddr, clientAddrLen) < 0) {
        perror("sendACK failed");
        exit(EXIT_FAILURE);
    }
}





