
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

#define BUFFER_SIZE 516
#define TIMEOUT_SEC 5 // Ajustez selon les besoins
#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5
#define OP_OACK 6
#define OPTION_BIGFILE "bigfile"
#define MAX_RETRIES 3

// Prototypes des fonctions
void sendRRQAndWaitForResponse(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode, int blksize);
void sendFile(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode, int blksize);
int waitForAck(int sockfd, struct sockaddr_in *serverAddr, unsigned int expectedBlockNum);
int sendWithRetries(int sockfd, struct sockaddr_in *serverAddr, char *packet, int packetLen, unsigned int expectedBlockNum);


// La fonction principale
int main() {
    char serverIP[INET_ADDRSTRLEN];
    int serverPort;
    char filename[100];
    char mode[10];
    int operation;
    int blksize = 512; // Taille de bloc par défaut

    printf("Enter server IP: ");
    scanf("%s", serverIP);
    printf("Enter server port: ");
    scanf("%d", &serverPort);
    printf("Enter operation (1 for RRQ, 2 for WRQ): ");
    scanf("%d", &operation);
    printf("Enter filename: ");
    scanf("%s", filename);
    printf("Enter mode (octet or netascii): ");
    scanf("%s", mode);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Cannot create socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);
    inet_pton(AF_INET, serverIP, &serverAddr.sin_addr);

    if (operation == 1) {
        sendRRQAndWaitForResponse(sockfd, &serverAddr, filename, mode, blksize);
    } else if (operation == 2) {
        sendFile(sockfd, &serverAddr, filename, mode, blksize);
    } else {
        printf("Invalid operation.\n");
    }

    close(sockfd);
    return 0;
}

// Implémentations des fonctions sendRRQAndWaitForResponse, sendFile, waitForAck, sendWithRetries

void sendRRQAndWaitForResponse(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode, int blksize) {
    char buffer[BUFFER_SIZE];
    int len, recvLen;
    struct sockaddr_in fromAddr;
    socklen_t fromAddrLen = sizeof(fromAddr);
    unsigned int blockNum = 0;  // Initial block number for ACK

    // Construction de la requête RRQ avec l'option bigfile
    len = sprintf(buffer, "%c%c%s%c%s%c", 0, OP_RRQ, filename, 0, mode, 0);
    len += sprintf(buffer + len, "%s%c%d%c", OPTION_BIGFILE, 0, 1, 0); // Ajout de l'option bigfile

    // Envoi de la requête RRQ
    sendto(sockfd, buffer, len, 0, (struct sockaddr *)serverAddr, sizeof(*serverAddr));

    // Ouverture/Création du fichier où écrire les données reçues
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Failed to open file for writing");
        exit(EXIT_FAILURE);
    }

    // Boucle de réception des données
    while (1) {
        recvLen = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&fromAddr, &fromAddrLen);
        if (recvLen < 4) {
            perror("Packet received is too short");
            continue;
        }

        unsigned short opcode = buffer[1];
        unsigned short receivedBlock = ntohs(*(unsigned short *)(buffer + 2));

        // Vérification du premier paquet pour OACK
        if (opcode == OP_OACK) {
            // Envoi d'un ACK pour OACK
            buffer[0] = 0; buffer[1] = OP_ACK;
            buffer[2] = 0; buffer[3] = 0;
            sendto(sockfd, buffer, 4, 0, (struct sockaddr *)&fromAddr, fromAddrLen);
            continue;  // Attendre le premier bloc de données
        } else if (opcode == OP_DATA) {
            if (receivedBlock == (blockNum + 1) % 65536) {
                fwrite(buffer + 4, 1, recvLen - 4, file);  // Écriture des données dans le fichier
                blockNum = receivedBlock;  // Mise à jour du numéro de bloc

                // Envoi d'un ACK pour le bloc reçu
                buffer[0] = 0; buffer[1] = OP_ACK;
                buffer[2] = (blockNum >> 8) & 0xFF; buffer[3] = blockNum & 0xFF;
                sendto(sockfd, buffer, 4, 0, (struct sockaddr *)&fromAddr, fromAddrLen);

                if (recvLen < BUFFER_SIZE) {  // Si c'est le dernier bloc
                    printf("File transfer completed.\n");
                    break;
                }
            }
        } else if (opcode == OP_ERROR) {
            printf("Error packet received: %s\n", buffer + 4);
            break;
        }
    }

    fclose(file);
}

int waitForAck(int sockfd, struct sockaddr_in *serverAddr, unsigned int expectedBlockNum) {
    char ackBuffer[4];
    struct timeval tv;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;

    if (select(sockfd + 1, &readfds, NULL, NULL, &tv) > 0) {
        socklen_t addrLen = sizeof(struct sockaddr_in);
        if (recvfrom(sockfd, ackBuffer, sizeof(ackBuffer), 0, (struct sockaddr *)serverAddr, &addrLen) >= 4) {
            if (ackBuffer[1] == OP_ACK) {
                unsigned int blockNum = ((unsigned char)ackBuffer[2] << 8) | (unsigned char)ackBuffer[3];
                if (blockNum == expectedBlockNum) {
                    return 1;  // ACK correct reçu
                }
            }
        }
    }
    return 0;  // Timeout ou ACK incorrect
}

int sendWithRetries(int sockfd, struct sockaddr_in *serverAddr, char *packet, int packetLen, unsigned int expectedBlockNum) {
    int retries = 0;
    while (retries < MAX_RETRIES) {
        sendto(sockfd, packet, packetLen, 0, (struct sockaddr *)serverAddr, sizeof(*serverAddr));
        if (waitForAck(sockfd, serverAddr, expectedBlockNum)) {
            return 1;  // ACK reçu
        }
        retries++;
    }
    return 0;  // Échec après les tentatives
}

void sendFile(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode, int blksize) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Could not open file for reading");
        exit(EXIT_FAILURE);
    }

    // Envoi de la requête WRQ avec l'option bigfile si nécessaire
    char buffer[BUFFER_SIZE];
    int len = sprintf(buffer, "%c%c%s%c%s%c%s%c%d%c", 0, OP_WRQ, filename, 0, mode, 0, OPTION_BIGFILE, 0, 1, 0);
    sendto(sockfd, buffer, len, 0, (struct sockaddr *)serverAddr, sizeof(*serverAddr));

    // Attente de l'ACK pour la requête WRQ ou de l'OACK
    if (!waitForAck(sockfd, serverAddr, 0)) {
        printf("Timeout or no ACK for WRQ.\n");
        fclose(file);
        return;
    }

    // Envoi du fichier en blocs
    unsigned int blockNum = 1;
    size_t bytesRead;
    while ((bytesRead = fread(buffer + 4, 1, blksize, file)) > 0) {
        buffer[0] = 0; buffer[1] = OP_DATA;
        buffer[2] = (blockNum >> 8) & 0xFF; buffer[3] = blockNum & 0xFF;

        if (!sendWithRetries(sockfd, serverAddr, buffer, bytesRead + 4, blockNum)) {
            printf("Failed to send block %u.\n", blockNum);
            break;
        }

        blockNum = (blockNum + 1) % 65536; // Gestion du roll-over de numéro de bloc
    }

    fclose(file);
}


