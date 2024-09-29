#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h> // Pour struct timeval

#define BUFFER_SIZE 516
#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5
#define DEFAULT_TFTP_PORT 6969
#define MAX_FILES 100
#define TIMEOUT_SEC 60 // Timeout pour recvfrom en secondes
// #define MAX_RETRIES 3

typedef struct {
    int sockfd;
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen;
    char filename[100];
    char mode[10];
} ClientRequest;

typedef struct {
    char filename[100];
    pthread_mutex_t mutex;
} FileLock;

FileLock fileLocks[MAX_FILES];
int fileLockCount = 0;
pthread_mutex_t fileLocksMutex = PTHREAD_MUTEX_INITIALIZER;

// Prototypes des fonctions
void* handleRRQ(void* arg);
void* handleWRQ(void* arg);
FileLock* getFileLock(const char* filename);
void lockFile(FileLock* lock);
void unlockFile(FileLock* lock);
void sendError(int sockfd, struct sockaddr_in* clientAddr, socklen_t clientAddrLen, const char* errorMessage);
void sendACK(int sockfd, struct sockaddr_in* clientAddr, socklen_t clientAddrLen, int blockNum);

int main() {
    int sockfd;
    struct sockaddr_in serverAddr, clientAddr;
    char buffer[BUFFER_SIZE];
    socklen_t clientAddrLen = sizeof(clientAddr);
    char serverIP[INET_ADDRSTRLEN]; // Buffer pour l'adresse IP du serveur
    int serverPort; // Variable pour le port du serveur

    printf("Enter server IP address (or 'any' to listen on all interfaces): ");
    scanf("%s", serverIP);
    printf("Enter server port (or 0 to use default %d): ", DEFAULT_TFTP_PORT);
    scanf("%d", &serverPort);
    if (serverPort == 0) {
        serverPort = DEFAULT_TFTP_PORT; // Utilisation du port par défaut si 0 est saisi
    }

    // Initialisation du socket serveur
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(serverPort);

    if (bind(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    printf("TFTP Server running on port %d\n", serverPort);

    while (1) {
        int receivedBytes = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&clientAddr, &clientAddrLen);
        if (receivedBytes < 0) {
            perror("recvfrom failed");
            continue;
        }

        // Création d'une socket pour la session client
        int clientSockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (clientSockfd < 0) {
            perror("Failed to create socket for client session");
            continue;
        }

        // Préparation de la requête client
        ClientRequest* request = (ClientRequest*)malloc(sizeof(ClientRequest));
        if (!request) {
            perror("Failed to allocate memory for client request");
            close(clientSockfd);
            continue;
        }
        
        request->sockfd = clientSockfd;
        request->clientAddr = clientAddr;
        request->clientAddrLen = clientAddrLen;
        strncpy(request->filename, buffer + 2, sizeof(request->filename) - 1);
        // Assurez-vous de gérer correctement le mode ici
        // Déclaration du pointeur vers la fonction

        // Lancement du thread pour traiter la requête
        pthread_t thread;
// Déclaration du pointeur vers la fonction
void* (*handlerFunc)(void *) = NULL; // This declares a function pointer correctly.


// Affectation conditionnelle basée sur le type de requête
if (buffer[1] == OP_RRQ) {
    handlerFunc = handleRRQ;
} else if (buffer[1] == OP_WRQ) {
    handlerFunc = handleWRQ;
} else {
    // Gestion des requêtes non supportées
    sendError(clientSockfd, &clientAddr, clientAddrLen, "Unsupported request.");
    free(request);
    continue; // Passez à la prochaine itération de la boucle si la requête n'est pas supportée
}

// Création d'un thread pour gérer la requête avec la fonction appropriée
if (pthread_create(&thread, NULL, handlerFunc, (void*)request) != 0) {
    perror("Thread creation failed");
    close(clientSockfd);
    free(request);
} else {
    pthread_detach(thread);
}
    }

    close(sockfd);
    return 0;
}

// Fonction pour gérer les requêtes de lecture (RRQ)
void* handleRRQ(void* arg) {
    ClientRequest* request = (ClientRequest*)arg;
    FILE* file;
    char dataBuf[BUFFER_SIZE];
    int bytesRead, blockNum = 1;
    const int MAX_RETRIES = 5;  // Nombre maximal de tentatives de retransmission

    // Configurer le timeout pour recvfrom
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;  // Timeout en secondes
    tv.tv_usec = 0;          // Timeout en microsecondes
    if (setsockopt(request->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Error setting socket timeout");
        free(request);
        return NULL;
    }

    // Obtention du verrou pour le fichier demandé
    FileLock* fileLock = getFileLock(request->filename);
    if (!fileLock) {
        sendError(request->sockfd, &request->clientAddr, request->clientAddrLen, "Server error: file lock unavailable.");
        free(request);
        return NULL;
    }

    lockFile(fileLock);  // Verrouillage du fichier

    // Ouverture du fichier en mode lecture binaire
    file = fopen(request->filename, "rb");
    if (!file) {
        sendError(request->sockfd, &request->clientAddr, request->clientAddrLen, "File not found.");
        unlockFile(fileLock);  // Déverrouillage du fichier
        free(request);
        return NULL;
    }

    // Boucle de lecture et d'envoi du fichier par blocs
    while ((bytesRead = fread(dataBuf + 4, 1, 512, file)) > 0) {
        int attempts = 0;
        while (attempts < MAX_RETRIES) {
            // Préparation du paquet de données
            dataBuf[0] = 0; dataBuf[1] = OP_DATA;
            dataBuf[2] = (blockNum >> 8) & 0xFF; dataBuf[3] = blockNum & 0xFF;
            ssize_t sentBytes = sendto(request->sockfd, dataBuf, bytesRead + 4, 0,
                                       (struct sockaddr*)&request->clientAddr, request->clientAddrLen);
            if (sentBytes < 0) {
                perror("sendto failed");
                attempts++;
                continue;
            }

            // Attente de l'ACK correspondant avec gestion du timeout
            ssize_t rcvLen = recvfrom(request->sockfd, dataBuf, 4, 0, NULL, NULL);
            if (rcvLen < 0) {
                // Timeout ou erreur, on réessaie d'envoyer le paquet
                perror("recvfrom timed out or failed");
                attempts++;
            } else if (dataBuf[1] == OP_ACK && dataBuf[2] == ((blockNum >> 8) & 0xFF) && 
                       dataBuf[3] == (blockNum & 0xFF)) {
                blockNum++; // ACK reçu, on passe au bloc suivant
                break;
            } else {
                // Réponse inattendue, on réessaie d'envoyer le paquet
                attempts++;
            }
        }

        if (attempts >= MAX_RETRIES) {
            fprintf(stderr, "Max retries exceeded for block %d\n", blockNum);
            break;
        }

        if (bytesRead < 512) { // Si le dernier bloc est moins de 512, c'est la fin du fichier
            break;
        }
    }

    fclose(file);
    unlockFile(fileLock);  // Déverrouillage du fichier
    close(request->sockfd);
    free(request);
    return NULL;
}


void* handleWRQ(void* arg) {
    ClientRequest* request = (ClientRequest*)arg;
    char buffer[BUFFER_SIZE];
    FILE* file = NULL;
    int blockNum = 0, attempts = 0;
    const int MAX_RETRIES = 5; // Nombre maximal de tentatives de réception

    // Création d'une nouvelle socket pour isoler la session de communication
    int sessionSockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sessionSockfd < 0) {
        perror("Socket creation for WRQ session failed");
        free(request);
        return NULL;
    }

    // Configurer le timeout pour recvfrom
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC; // Timeout en secondes
    tv.tv_usec = 0; // Timeout en microsecondes
    setsockopt(sessionSockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Obtention du verrou pour le fichier demandé
    FileLock* fileLock = getFileLock(request->filename);
    if (!fileLock) {
        sendError(sessionSockfd, &request->clientAddr, request->clientAddrLen, "Server error: file lock unavailable.");
        close(sessionSockfd);
        free(request);
        return NULL;
    }

    lockFile(fileLock); // Verrouillage du fichier

    // Ouverture/Création du fichier pour écriture
    file = fopen(request->filename, "wb");
    if (!file) {
        sendError(sessionSockfd, &request->clientAddr, request->clientAddrLen, "Cannot open file for writing.");
        unlockFile(fileLock);
        close(sessionSockfd);
        free(request);
        return NULL;
    }

    // Envoi de l'ACK initial pour la requête WRQ
    sendACK(sessionSockfd, &request->clientAddr, request->clientAddrLen, blockNum);

    // Boucle de réception des blocs de données
    while (1) {
        ssize_t recvLen = recvfrom(sessionSockfd, buffer, BUFFER_SIZE, 0, NULL, NULL);
        if (recvLen < 0 && ++attempts < MAX_RETRIES) {
            perror("recvfrom timeout or error, retrying");
            sendACK(sessionSockfd, &request->clientAddr, request->clientAddrLen, blockNum); // Retransmission de l'ACK
            continue;
        } else if (attempts >= MAX_RETRIES) {
            fprintf(stderr, "Max retries exceeded for block %d\n", blockNum + 1);
            break;
        }

        if (buffer[1] == OP_DATA) {
            int receivedBlockNum = (buffer[2] << 8) | buffer[3];
            if (receivedBlockNum == blockNum + 1) {
                fwrite(buffer + 4, 1, recvLen - 4, file); // Écrire les données reçues
                blockNum++;
                sendACK(sessionSockfd, &request->clientAddr, request->clientAddrLen, blockNum);
                attempts = 0; // Réinitialiser les tentatives pour le prochain bloc
            }
        } else {
            fprintf(stderr, "Unexpected packet type\n");
            break;
        }

        if (recvLen < 516) { // Dernier bloc reçu
            printf("File transfer completed.\n");
            break;
        }
    }

    fclose(file);
    unlockFile(fileLock);
    close(sessionSockfd);
    free(request);
    return NULL;
}

// Fonction helper pour envoyer un ACK
void sendACK(int sockfd, struct sockaddr_in* clientAddr, socklen_t clientAddrLen, int blockNum) {
    char ackPacket[4];
    ackPacket[0] = 0;
    ackPacket[1] = OP_ACK;
    ackPacket[2] = (blockNum >> 8) & 0xFF;
    ackPacket[3] = blockNum & 0xFF;
    sendto(sockfd, ackPacket, sizeof(ackPacket), 0, (const struct sockaddr*)clientAddr, clientAddrLen);
}


FileLock* getFileLock(const char* filename) {
    pthread_mutex_lock(&fileLocksMutex);
    for (int i = 0; i < fileLockCount; ++i) {
        if (strcmp(fileLocks[i].filename, filename) == 0) {
            pthread_mutex_unlock(&fileLocksMutex);
            return &fileLocks[i];
        }
    }
    if (fileLockCount < MAX_FILES) {
        strncpy(fileLocks[fileLockCount].filename, filename, sizeof(fileLocks[fileLockCount].filename) - 1);
        pthread_mutex_init(&fileLocks[fileLockCount].mutex, NULL);
        FileLock* newLock = &fileLocks[fileLockCount++];
        pthread_mutex_unlock(&fileLocksMutex);
        return newLock;
    }
    pthread_mutex_unlock(&fileLocksMutex);
    return NULL; // Retourne NULL si le nombre maximal de fichiers est atteint
}


void lockFile(FileLock* lock) {
    pthread_mutex_lock(&lock->mutex);
}

void unlockFile(FileLock* lock) {
    pthread_mutex_unlock(&lock->mutex);
}

void sendError(int sockfd, struct sockaddr_in* clientAddr, socklen_t clientAddrLen, const char* errorMessage) {
    char buffer[BUFFER_SIZE];
    // Construction du paquet d'erreur
    buffer[0] = 0; // Byte zéro pour tous les paquets TFTP
    buffer[1] = OP_ERROR; // Opcode pour un paquet d'erreur
    buffer[2] = 0; // Code d'erreur 0 signifie "non défini"
    buffer[3] = 0;
    strncpy(buffer + 4, errorMessage, BUFFER_SIZE - 5); // Copie du message d'erreur
    buffer[BUFFER_SIZE - 1] = '\0'; // Assure que le message est null-terminé

    sendto(sockfd, buffer, strlen(buffer + 4) + 5, 0, (struct sockaddr*)clientAddr, clientAddrLen);
}