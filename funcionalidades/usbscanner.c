#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include "usbscanner.h"

#define MEDIA_PATH "/media/manuel"
#define SLEEP_TIME 1000000 // 1 segundo
#define MAX_DEVICES 16

// Estructura para guardar información de archivos
typedef struct FileInfo {
    char path[512];
    time_t mtime;
    int is_dir; // 1 si es carpeta, 0 si es archivo
    struct FileInfo *next;
} FileInfo;

// Lista enlazada para guardar el estado de archivos
FileInfo* scan_files(const char *base_path, FileInfo *list) {
    DIR *dir = opendir(base_path);
    if (!dir) return list;
    struct dirent *entry;
    char path[512];
    struct stat st;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
        if (stat(path, &st) == 0) {
            FileInfo *fi = malloc(sizeof(FileInfo));
            strcpy(fi->path, path);
            fi->mtime = st.st_mtime;
            fi->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            fi->next = list;
            list = fi;
            if (fi->is_dir) {
                list = scan_files(path, list); // Recursivo para subdirectorios
            }
        }
    }
    closedir(dir);
    return list;
}

// Libera la lista enlazada
void free_file_list(FileInfo *list) {
    while (list) {
        FileInfo *tmp = list;
        list = list->next;
        free(tmp);
    }
}

// Busca un archivo en la lista
FileInfo* find_file(FileInfo *list, const char *path) {
    while (list) {
        if (strcmp(list->path, path) == 0) return list;
        list = list->next;
    }
    return NULL;
}

// Hilo para monitorear cambios en un dispositivo
void* monitor_device(void *arg) {
    char *device_path = (char*)arg;
    printf("Iniciando monitoreo en %s\n", device_path);
    FileInfo *prev = scan_files(device_path, NULL);

    while (1) {
        sleep(1);
        FileInfo *curr = scan_files(device_path, NULL);

        // Detectar archivos nuevos y modificados
        for (FileInfo *f = curr; f; f = f->next) {
            FileInfo *old = find_file(prev, f->path);
            if (!old) {
                printf("[ALERTA] Archivo creado: %s\n", f->path);
            } else if (old->mtime != f->mtime) {
                printf("[ALERTA] Archivo modificado: %s\n", f->path);
            }
        }
        // Detectar archivos eliminados o renombrados
        for (FileInfo *f = prev; f; f = f->next) {
            if (!find_file(curr, f->path)) {
                printf("[ALERTA] Archivo eliminado o modificado: %s\n", f->path);
            }
        }
        free_file_list(prev);
        prev = curr;
    }
    free_file_list(prev);
    free(device_path);
    return NULL;
}

// Cuenta dispositivos y llena el arreglo con sus nombres
int list_devices(char devices[MAX_DEVICES][256]) {
    DIR *dir = opendir(MEDIA_PATH);
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            snprintf(devices[count], 256, "%s/%s", MEDIA_PATH, entry->d_name);
            count++;
            if (count >= MAX_DEVICES) break;
        }
    }
    closedir(dir);
    return count;
}

// Terminal sin canon para leer teclas sin Enter
void set_nonblocking(int enable) {
    static struct termios oldt, newt;
    if (enable) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    } else tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

// Escanea dispositivos USB y lanza hilos para monitorear cada uno
void usb_scan() {
    char devices[MAX_DEVICES][256] = {0};
    int prev_count = list_devices(devices);
    pthread_t threads[MAX_DEVICES] = {0};
    int monitoring[MAX_DEVICES] = {0};

    printf("Escaneando dispositivos USB en %s. Presiona Q para salir.\n", MEDIA_PATH);

    int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);
    set_nonblocking(1);

    while (1) {
        int c = getchar();
        if (c == 'Q' || c == 'q') break;

        char curr_devices[MAX_DEVICES][256] = {0};
        int curr_count = list_devices(curr_devices);

        // Detectar nuevos dispositivos y lanzar hilos
        for (int i = 0; i < curr_count; i++) {
            int found = 0;
            for (int j = 0; j < prev_count; j++) {
                if (strcmp(curr_devices[i], devices[j]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found && !monitoring[i]) {
                printf("¡Nueva memoria USB detectada: %s!\n", curr_devices[i]);
                char *dev_path = strdup(curr_devices[i]);
                pthread_create(&threads[i], NULL, monitor_device, dev_path);
                monitoring[i] = 1;
            }
        }
        // Detectar dispositivos retirados
        for (int i = 0; i < prev_count; i++) {
            int found = 0;
            for (int j = 0; j < curr_count; j++) {
                if (strcmp(devices[i], curr_devices[j]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                printf("Una memoria USB fue retirada: %s\n", devices[i]);
            }
        }
        memcpy(devices, curr_devices, sizeof(devices));
        prev_count = curr_count;
        usleep(SLEEP_TIME);
    }

    set_nonblocking(0);
    fcntl(STDIN_FILENO, F_SETFL, old_flags);
    printf("Programa terminado.\n");
}