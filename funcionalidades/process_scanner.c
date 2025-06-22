#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>

#define CPU_THRESHOLD 80.0
#define MEM_THRESHOLD 30.0
#define ALERT_SECONDS 15
#define MAX_PROCESSES 4096

typedef struct {
    int pid;
    char name[256];
    unsigned long prev_total;
    int cpu_consec;
    int mem_consec;
} ProcInfo;

volatile sig_atomic_t running = 1;

// Thread que detecta si se presiona 'q'
void *key_listener(void *arg) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    int ch;
    while (running) {
        ch = getchar();
        if (ch == 'q' || ch == 'Q') {
            running = 0;
            break;
        }
        usleep(100000);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return NULL;
}

// Verifica si una cadena es numérica
int isnum(const char *s) {
    while (*s) { if (!isdigit(*s++)) return 0; }
    return 1;
}

// Lee tiempo de CPU y nombre de un proceso
int read_stat(int pid, unsigned long *t, char *name) {
    char buf[1024], path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);

    char *s = strchr(buf, '('), *e = strrchr(buf, ')');
    if (!s || !e) return 0;

    size_t len = e - s - 1;
    strncpy(name, s + 1, len);
    name[len] = '\0';

    unsigned long ut, st;
    if (sscanf(e + 2, "%*c %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &ut, &st) != 2)
        return 0;

    *t = ut + st;
    return 1;
}

// Calcula porcentaje de RAM usada por un proceso
double get_process_mem_percent(int pid, unsigned long total_kb) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long vmrss_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %lu kB", &vmrss_kb) == 1) break;
    }
    fclose(f);
    return (double)vmrss_kb / total_kb * 100.0;
}

// Función principal llamada externamente
void process_scan() {
    running = 1;  // reinicia el estado para permitir nuevas ejecuciones

    ProcInfo arr[MAX_PROCESSES];
    int n = 0;
    long clk = sysconf(_SC_CLK_TCK);
    unsigned long total_mem_kb = 0;

    // Obtener memoria total
    {
        FILE *f = fopen("/proc/meminfo", "r");
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %lu kB", &total_mem_kb) == 1)
                break;
        }
        fclose(f);
    }

    // Lanzar hilo que escucha la tecla 'q'
    pthread_t key_thread;
    pthread_create(&key_thread, NULL, key_listener, NULL);

    printf("🔍 Monitoreo iniciado: Presione 'q' para finalizar ejecución\n");

    while (running) {
        sleep(1);
        DIR *d = opendir("/proc");
        if (!d) {
            perror("opendir");
            return;
        }

        struct dirent *e;
        while ((e = readdir(d))) {
            if (!isnum(e->d_name)) continue;

            int pid = atoi(e->d_name);
            unsigned long total;
            char name[256];
            if (!read_stat(pid, &total, name)) continue;

            double mem = get_process_mem_percent(pid, total_mem_kb);

            // Buscar si el proceso ya existe en la lista
            int i;
            for (i = 0; i < n; i++)
                if (arr[i].pid == pid) break;

            // Si no existe, agregarlo
            if (i == n && n < MAX_PROCESSES) {
                arr[n].pid = pid;
                strcpy(arr[n].name, name);
                arr[n].prev_total = total;
                arr[n].cpu_consec = 0;
                arr[n].mem_consec = (mem > MEM_THRESHOLD) ? 1 : 0;
                n++;
            }

            if (i == n) continue;  // ya no hay espacio

            unsigned long diff = total - arr[i].prev_total;
            arr[i].prev_total = total;
            double cpu = ((double)diff / clk) * 100.0;

            // Comprobar umbral de CPU
            if (cpu > CPU_THRESHOLD) {
                arr[i].cpu_consec++;
                if (arr[i].cpu_consec == ALERT_SECONDS) {
                    printf("🚨 ALERTA CPU: PID %d (%s) uso > %.1f%% CPU durante %d segundos consecutivos\n",
                           pid, name, CPU_THRESHOLD, ALERT_SECONDS);
                }
            } else {
                arr[i].cpu_consec = 0;
            }

            // Comprobar umbral de RAM
            if (mem > MEM_THRESHOLD) {
                arr[i].mem_consec++;
                if (arr[i].mem_consec == ALERT_SECONDS) {
                    printf("🚨 ALERTA MEMORIA: PID %d (%s) uso > %.1f%% MEM durante %d segundos consecutivos\n",
                           pid, name, MEM_THRESHOLD, ALERT_SECONDS);
                }
            } else {
                arr[i].mem_consec = 0;
            }
        }

        closedir(d);
    }

    pthread_join(key_thread, NULL);
    printf("\n✅ Monitoreo finalizado por el usuario (tecla 'q')\n");
}
