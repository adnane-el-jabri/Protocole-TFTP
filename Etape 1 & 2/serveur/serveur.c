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

    while (1) {
        int receivedBytes = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
                                     (struct sockaddr *)&clientAddr, &clientAddrLen);
        if (receivedBytes < 0) {
            perror("recvfrom failed");
            break;
        }

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

    close(sockfd);
    return 0;
}

// Implement the handleRRQ function to handle read requests
void handleRRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode) {
    FILE *file;
    char buffer[BUFFER_SIZE];
    int bytesRead, blockNum = 1;

    file = fopen(filename, "rb");
    if (file == NULL) {
        perror("File not found or cannot be opened");
        // Send error packet to client
        // Format: | 0x00 | 0x05 | ErrorCode | ErrorMessage | 0x00 |
        sprintf(buffer, "%c%c%c%cFile not found%c", 0, OP_ERROR, 0, 1, 0);
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)clientAddr, clientAddrLen);
        return;
    }

    do {
        // Prepare the DATA packet
        // Format: | 0x00 | 0x03 | Block # | Data |
        bytesRead = fread(buffer + 4, 1, 512, file);
        buffer[0] = 0;
        buffer[1] = OP_DATA;
        buffer[2] = (blockNum >> 8) & 0xFF;
        buffer[3] = blockNum & 0xFF;

        // Send the DATA packet
        sendto(sockfd, buffer, bytesRead + 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);

        // Wait for ACK
        // Expected ACK format: | 0x00 | 0x04 | Block # |
        int ackReceived = 0;
        while (!ackReceived) {
            fd_set readfds;
            struct timeval tv;
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);
            tv.tv_sec = TIMEOUT_SEC;
            tv.tv_usec = 0;
            
            int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
            if (rv > 0) {
                int len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)clientAddr, &clientAddrLen);
                if (len >= 4 && buffer[1] == OP_ACK) {
                    int ackBlockNum = (buffer[2] << 8) | buffer[3];
                    if (ackBlockNum == blockNum) {
                        ackReceived = 1;
                    }
                }
            } else if (rv == 0) {
                // Timeout occurred, retransmit the DATA packet
                sendto(sockfd, buffer, bytesRead + 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);
            } else {
                // Error occurred
                perror("Error receiving ACK");
                break;
            }
        }

        blockNum++;
    } while (bytesRead == 512);  // The last packet must be less than 512 bytes

    fclose(file);
}

// Implement the handleWRQ function to handle write requests

void handleWRQ(int sockfd, struct sockaddr_in *clientAddr, socklen_t clientAddrLen, const char *filename, const char *mode) {
    FILE *file;
    char buffer[BUFFER_SIZE];
    int blockNum = 0;
    int expectedBlockNum = 1;
    int writeComplete = 0;

    // Open file for writing
    file = fopen(filename, "wb");
    if (file == NULL) {
        perror("Cannot create file");
        // Send error packet to client
        sprintf(buffer, "%c%c%c%cCould not open file%c", 0, OP_ERROR, 0, 2, 0);
        sendto(sockfd, buffer, strlen(buffer) + 1, 0, (struct sockaddr *)clientAddr, clientAddrLen);
        return;
    }

    // Send initial ACK for WRQ
    buffer[0] = 0;
    buffer[1] = OP_ACK;
    buffer[2] = 0;
    buffer[3] = 0;
    sendto(sockfd, buffer, 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);

    while (!writeComplete) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;

        // Wait for DATA packet
        int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        if (rv > 0) {
            int len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)clientAddr, &clientAddrLen);
            if (len < 4) {
                // Malformed packet
                break;
            }
            int opcode = buffer[1];
            blockNum = (buffer[2] << 8) | buffer[3];

            if (opcode == OP_DATA && blockNum == expectedBlockNum) {
                // Write block to file
                fwrite(buffer + 4, 1, len - 4, file);

                // Send ACK
                buffer[1] = OP_ACK;
                sendto(sockfd, buffer, 4, 0, (struct sockaddr *)clientAddr, clientAddrLen);

                if (len < BUFFER_SIZE) {
                    // Last block of data
                    writeComplete = 1;
                }

                expectedBlockNum++;
            } else if (opcode == OP_ERROR) {
                // Handle error
                break;
            }
        } else if (rv == 0) {
            // Timeout occurred
            printf("Timeout occurred while waiting for data block %d\n", expectedBlockNum);
            break;
        } else {
            // Error occurred
            perror("Error waiting for data");
            break;
        }
    }

    fclose(file);
}