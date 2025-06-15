#include "process_scanner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>

#define CPU_THRESHOLD 50.0   // % de CPU
#define MEM_THRESHOLD 50.0   // % de memoria
#define ALERT_SECONDS 10
#define MAX_PROCESSES 4096

typedef struct {
    int pid;
    char name[256];
    double cpu_usage;
    double mem_usage;
    int over_threshold_seconds;
    int alerted;
} ProcessAlertInfo;

typedef struct {
    int pid;
    char name[256];
    unsigned long utime;
    unsigned long stime;
    unsigned long vsize;
    unsigned long rss;
    double cpu_usage;
    double mem_usage;
} ProcessInfo;

// Configura la terminal para lectura no bloqueante
static void set_nonblocking(int enable) {
    static struct termios oldt, newt;
    static int oldf;
    if (enable) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        fcntl(STDIN_FILENO, F_SETFL, oldf);
    }
}

static unsigned long get_total_cpu_time() {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;
    char line[512];
    unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
    fgets(line, sizeof(line), fp);
    sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    fclose(fp);
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

static unsigned long get_total_mem() {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0;
    char line[256];
    unsigned long mem_total = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) break;
    }
    fclose(fp);
    return mem_total;
}

static int is_number(const char *str) {
    while (*str) {
        if (!isdigit(*str)) return 0;
        str++;
    }
    return 1;
}

static void scan_processes(unsigned long cpu_diff, unsigned long total_mem, ProcessAlertInfo *alerts, int *alert_count) {
    DIR *dir = opendir("/proc");
    struct dirent *entry;

    if (!dir) {
        perror("opendir /proc");
        return;
    }

    printf("%-8s %-25s %-10s %-10s\n", "PID", "NAME", "CPU(%)", "MEM(%)");

    while ((entry = readdir(dir)) != NULL) {
        if (!is_number(entry->d_name)) continue;
        int pid = atoi(entry->d_name);
        char stat_path[256];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);

        FILE *fp = fopen(stat_path, "r");
        if (!fp) continue;

        ProcessInfo pinfo = {0};
        pinfo.pid = pid;

        char comm[256];
        char state;
        unsigned long utime, stime, vsize;
        long rss;
        int scanned = fscanf(fp, "%d %255s %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %lu %ld",
                             &pinfo.pid, comm, &state, &utime, &stime, &vsize, &rss);
        fclose(fp);
        if (scanned < 7) continue;

        strncpy(pinfo.name, comm, sizeof(pinfo.name));
        pinfo.utime = utime;
        pinfo.stime = stime;
        pinfo.vsize = vsize;
        pinfo.rss = rss;

        // Calcular uso de memoria
        pinfo.mem_usage = (double)(rss * sysconf(_SC_PAGESIZE)) / (total_mem * 1024) * 100.0;

        // Calcular uso de CPU (simplificado, solo para ejemplo)
        pinfo.cpu_usage = ((double)(utime + stime) / cpu_diff) * 100.0;

        // Buscar si el proceso ya está en la lista de alertas
        int found = 0;
        for (int i = 0; i < *alert_count; ++i) {
            if (alerts[i].pid == pinfo.pid) {
                found = 1;
                // Si sigue sobre el umbral, incrementar el contador
                if (pinfo.cpu_usage > CPU_THRESHOLD && pinfo.mem_usage > MEM_THRESHOLD) {
                    alerts[i].over_threshold_seconds++;
                } else {
                    alerts[i].over_threshold_seconds = 0;
                    alerts[i].alerted = 0;
                }
                alerts[i].cpu_usage = pinfo.cpu_usage;
                alerts[i].mem_usage = pinfo.mem_usage;
                strncpy(alerts[i].name, pinfo.name, sizeof(alerts[i].name));
                break;
            }
        }
        // Si no está, agregarlo si corresponde
        if (!found && *alert_count < MAX_PROCESSES) {
            alerts[*alert_count].pid = pinfo.pid;
            strncpy(alerts[*alert_count].name, pinfo.name, sizeof(alerts[*alert_count].name));
            alerts[*alert_count].cpu_usage = pinfo.cpu_usage;
            alerts[*alert_count].mem_usage = pinfo.mem_usage;
            if (pinfo.cpu_usage > CPU_THRESHOLD && pinfo.mem_usage > MEM_THRESHOLD) {
                alerts[*alert_count].over_threshold_seconds = 1;
            } else {
                alerts[*alert_count].over_threshold_seconds = 0;
            }
            alerts[*alert_count].alerted = 0;
            (*alert_count)++;
        }

        // Mostrar información y alerta si corresponde
        int alert_this = 0;
        for (int i = 0; i < *alert_count; ++i) {
            if (alerts[i].pid == pinfo.pid &&
                alerts[i].over_threshold_seconds * 0.5 >= ALERT_SECONDS && // 0.5s por ciclo
                !alerts[i].alerted) {
                alert_this = 1;
                alerts[i].alerted = 1;
                break;
            }
        }

        if (alert_this) {
            printf("%-8d %-25s %-10.2f %-10.2f <-- ALERTA: >50%% CPU y RAM por más de 10s\n",
                   pinfo.pid, pinfo.name, pinfo.cpu_usage, pinfo.mem_usage);
        } else if (pinfo.cpu_usage > CPU_THRESHOLD || pinfo.mem_usage > MEM_THRESHOLD) {
            printf("%-8d %-25s %-10.2f %-10.2f <-- Sobre umbral\n",
                   pinfo.pid, pinfo.name, pinfo.cpu_usage, pinfo.mem_usage);
        } else {
            printf("%-8d %-25s %-10.2f %-10.2f\n",
                   pinfo.pid, pinfo.name, pinfo.cpu_usage, pinfo.mem_usage);
        }
    }
    closedir(dir);
}

void process_scan() {
    set_nonblocking(1);
    printf("Monitoreando procesos en tiempo real. Presione Q para salir.\n");
    ProcessAlertInfo alerts[MAX_PROCESSES] = {0};
    int alert_count = 0;
    int quit = 0;
    while (!quit) {
        unsigned long total_mem = get_total_mem();
        unsigned long total_cpu_1 = get_total_cpu_time();
        usleep(500000); // 0.5 segundos
        unsigned long total_cpu_2 = get_total_cpu_time();
        unsigned long cpu_diff = total_cpu_2 - total_cpu_1;

        printf("\033[2J\033[H"); // Limpiar pantalla
        scan_processes(cpu_diff, total_mem, alerts, &alert_count);

        // Verificar si se presionó 'q' o 'Q'
        int c = getchar();
        if (c == 'q' || c == 'Q') {
            quit = 1;
        }
    }
    set_nonblocking(0);
}
