#include "port_scanner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         // close()
#include <arpa/inet.h>      // inet_pton()
#include <netinet/in.h>     // sockaddr_in
#include <sys/socket.h>     // socket(), connect()
#include <sys/time.h>       // timeval

#define TIMEOUT_SEC 1       // Timeout para conexión (segundos)

typedef struct {
    int port;
    const char *service;
} port_service_t;

static port_service_t common_services[] = {
    {21, "FTP"},
    {22, "SSH"},
    {23, "Telnet"},
    {25, "SMTP"},
    {53, "DNS"},
    {80, "HTTP"},
    {110, "POP3"},
    {143, "IMAP"},
    {443, "HTTPS"},
    {3306, "MySQL"},
    {3389, "RDP"},
    {5900, "VNC"},
    {6379, "Redis"},
    {8080, "HTTP-Alt"},
};

#define COMMON_SERVICES_COUNT (sizeof(common_services) / sizeof(common_services[0]))

static int scan_port(const char *ip, int port) {
    int sockfd;
    struct sockaddr_in target_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return 0;
    }

    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &target_addr.sin_addr) <= 0) {
        close(sockfd);
        return 0;
    }

    struct timeval timeout = {TIMEOUT_SEC, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int result = connect(sockfd, (struct sockaddr *)&target_addr, sizeof(target_addr));
    close(sockfd);

    return result == 0;
}

static const char* get_service_name(int port) {
    for (int i = 0; i < COMMON_SERVICES_COUNT; i++) {
        if (common_services[i].port == port) {
            return common_services[i].service;
        }
    }
    return NULL;
}

void port_scan(void) {
    int start_port, end_port;

    // Solicitar puerto inicial
    printf("Introduce puerto inicial (1-65535): ");
    if (scanf("%d", &start_port) != 1) {
        fprintf(stderr, "Entrada inválida para puerto inicial.\n");
        return;
    }

    // Solicitar puerto final
    printf("Introduce puerto final (1-65535): ");
    if (scanf("%d", &end_port) != 1) {
        fprintf(stderr, "Entrada inválida para puerto final.\n");
        return;
    }

    // Validar rango de puertos
    if (start_port < 1 || start_port > 65535 || end_port < 1 || end_port > 65535) {
        fprintf(stderr, "Los puertos deben estar entre 1 y 65535.\n");
        return;
    }

    if (start_port > end_port) {
        fprintf(stderr, "El puerto inicial no puede ser mayor que el final.\n");
        return;
    }

    const char *target_ip = "127.0.0.1";

    printf("\nEscaneando puertos TCP en %s del %d al %d...\n\n", target_ip, start_port, end_port);

    for (int port = start_port; port <= end_port; port++) {
        if (scan_port(target_ip, port)) {
            const char* service = get_service_name(port);
            if (service) {
                printf(" [+] Puerto %d abierto (%s)\n", port, service);
            } else {
                printf(" [+] Puerto %d abierto (Servicio no común - posible puerta secreta!)\n", port);
            }
        }
    }

    printf("\nEscaneo finalizado.\n");
}