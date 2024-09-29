#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

#define BUFFER_SIZE 516
#define TIMEOUT_SEC 5 // Timeout de 5 secondes pour attendre les réponses
#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5

// Déclaration des fonctions pour envoyer des requêtes RRQ et WRQ
void sendRRQAndWaitForResponse(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode);
void sendFile(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode);

// Fonction principale
int main() {
    char serverIP[INET_ADDRSTRLEN];
    int serverPort;
    char filename[100];
    char mode[10];
    int operation;

    // Saisie des informations par l'utilisateur
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

    // Création du socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Cannot create socket");
        exit(EXIT_FAILURE);
    }

    // Configuration de l'adresse du serveur
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);
    if (inet_pton(AF_INET, serverIP, &serverAddr.sin_addr) <= 0) {
        perror("inet_pton error");
        exit(EXIT_FAILURE);
    }

    // Traitement en fonction de l'opération choisie
    if (operation == 1) {
        sendRRQAndWaitForResponse(sockfd, &serverAddr, filename, mode);
    } else if (operation == 2) {
        sendFile(sockfd, &serverAddr, filename, mode);
    } else {
        printf("Invalid operation.\n");
    }

    close(sockfd); // Fermeture du socket
    return 0;
}


// Implémentation de sendFile

void sendFile(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode) {
    char buffer[BUFFER_SIZE];
    FILE *file;
    int bytesRead, block = 0;
    int ackBlockNum;
    socklen_t addrLen = sizeof(*serverAddr);

    // Ouverture du fichier à envoyer
    if ((file = fopen(filename, "rb")) == NULL) {
        perror("Cannot open file");
        exit(EXIT_FAILURE);
    }

    // Envoi de la requête WRQ
    int len = sprintf(buffer, "%c%c%s%c%s%c", 0, OP_WRQ, filename, 0, mode, 0);
    sendto(sockfd, buffer, len, 0, (const struct sockaddr *)serverAddr, addrLen);

    // Attente de l'ACK pour WRQ
    recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)serverAddr, &addrLen);
    if (buffer[1] == OP_ACK) {
        ackBlockNum = (buffer[2] << 8) + buffer[3];
        if (ackBlockNum != 0) {
            printf("Invalid ACK number for WRQ.\n");
            fclose(file);
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    } else {
        printf("Did not receive expected ACK for WRQ.\n");
        fclose(file);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Envoi des blocs de données
    do {
        bytesRead = fread(buffer + 4, 1, 512, file);
        block++;
        buffer[0] = 0; buffer[1] = OP_DATA;
        buffer[2] = (block >> 8) & 0xFF; buffer[3] = block & 0xFF;

        sendto(sockfd, buffer, bytesRead + 4, 0, (const struct sockaddr *)serverAddr, addrLen);
        
        // Attente de l'ACK pour chaque bloc de données
        recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)serverAddr, &addrLen);
        if (buffer[1] != OP_ACK || ((buffer[2] << 8) + buffer[3]) != block) {
            printf("ACK error or block number mismatch.\n");
            fclose(file);
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    } while (bytesRead == 512); // Continue jusqu'à ce que le dernier bloc soit envoyé

    fclose(file);
    printf("File transfer complete.\n");
}

void sendRRQAndWaitForResponse(int sockfd, struct sockaddr_in *serverAddr, const char *filename, const char *mode) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in fromAddr;
    socklen_t fromAddrLen = sizeof(fromAddr);
    int len, recvLen, retryCount = 0;
    unsigned short block = 1;

    // Envoi de la requête RRQ
    len = sprintf(buffer, "%c%c%s%c%s%c", 0, OP_RRQ, filename, 0, mode, 0);
    sendto(sockfd, buffer, len, 0, (const struct sockaddr *)serverAddr, sizeof(*serverAddr));

    // Ouverture/Création du fichier où écrire les données reçues
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Failed to open file for writing");
        exit(EXIT_FAILURE);
    }

    fd_set readfds;
    struct timeval tv;

    // Boucle de réception des données
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        // Configuration du timeout
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;

        int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        if (rv == -1) {
            perror("select error");
            break;
        } else if (rv == 0) {
            printf("Timeout occurred! No data received.\n");
            if (++retryCount > 3) { // Limite de tentatives de retransmission atteinte
                printf("Failed to receive file: maximum retries exceeded.\n");
                break;
            }
            // Retransmission du dernier ACK
            buffer[0] = 0; buffer[1] = OP_ACK;
            buffer[2] = (block - 1) >> 8; buffer[3] = (block - 1) & 0xFF;
            sendto(sockfd, buffer, 4, 0, (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
        } else {
            // Réception du paquet de données
            recvLen = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&fromAddr, &fromAddrLen);
            if (recvLen < 4) { // Vérification de la longueur minimale d'un paquet TFTP
                printf("Received packet is too short.\n");
                break;
            }

            int opcode = buffer[1];
            unsigned short receivedBlock = (buffer[2] << 8) | buffer[3];
            
            if (opcode == OP_ERROR) {
                printf("Error received: %s\n", &buffer[4]);
                break;
            } else if (opcode == OP_DATA && receivedBlock == block) {
                // Écriture des données dans le fichier
                fwrite(buffer + 4, 1, recvLen - 4, file);
                // Envoi de l'ACK
                buffer[0] = 0; buffer[1] = OP_ACK;
                buffer[2] = receivedBlock >> 8; buffer[3] = receivedBlock & 0xFF;
                sendto(sockfd, buffer, 4, 0, (const struct sockaddr *)&fromAddr, fromAddrLen);
                
                if (recvLen < BUFFER_SIZE) { // Dernier bloc de données reçu
                    printf("File transfer completed.\n");
                    break;
                }
                block++; // Préparation pour le prochain bloc
            }
        }
    }

    fclose(file); // Fermeture du fichier
}

