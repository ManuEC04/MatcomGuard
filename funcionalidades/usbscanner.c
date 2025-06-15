#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <errno.h>

#define MEDIA_PATH "/media/manuel"
#define MAX_DEVICES 16
#define EVENT_BUF_LEN (1024 * (sizeof(struct inotify_event) + 512))
#define MAX_WATCHES 1024

typedef struct WatchMap {
    int wd;
    char path[512];
    struct WatchMap *next;
} WatchMap;

typedef struct MoveCookie {
    uint32_t cookie;
    char from_path[1024];
    struct MoveCookie *next;
} MoveCookie;

typedef struct MonitorThread {
    char mountpoint[256];
    pthread_t thread;
    int running;
    int *global_running; // apuntador a variable global para control Q
    struct MonitorThread *next;
    int inotify_fd;
    WatchMap *watches;
    MoveCookie *move_cookies;
} MonitorThread;

MonitorThread *threads = NULL;
int global_running = 1;

void notify(const char *summary, const char *body) {
    printf("%s: %s\n", summary, body);
    char cmd[768];
    snprintf(cmd, sizeof(cmd), "notify-send '%s' '%s'", summary, body);
    system(cmd);
}

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

void set_nonblocking(int enable) {
    static struct termios oldt, newt;
    if (enable) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    } else tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

MonitorThread* find_thread(const char *mountpoint) {
    MonitorThread *t = threads;
    while (t) {
        if (strcmp(t->mountpoint, mountpoint) == 0) return t;
        t = t->next;
    }
    return NULL;
}

void remove_thread(const char *mountpoint) {
    MonitorThread *prev = NULL, *curr = threads;
    while (curr) {
        if (strcmp(curr->mountpoint, mountpoint) == 0) {
            curr->running = 0; // Señal al hilo para terminar
            pthread_join(curr->thread, NULL);
            // Liberar watches
            while (curr->watches) {
                WatchMap *tmp = curr->watches;
                curr->watches = curr->watches->next;
                free(tmp);
            }
            // Liberar move_cookies
            while (curr->move_cookies) {
                MoveCookie *tmp = curr->move_cookies;
                curr->move_cookies = curr->move_cookies->next;
                free(tmp);
            }
            if (curr->inotify_fd > 0) close(curr->inotify_fd);
            if (prev) prev->next = curr->next;
            else threads = curr->next;
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// Añade un watch y lo guarda en la lista
int add_watch_recursive(MonitorThread *mt, const char *path);

int add_single_watch(MonitorThread *mt, const char *path) {
    int wd = inotify_add_watch(mt->inotify_fd, path,
        IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);

    if (wd < 0) return -1;

    WatchMap *wm = malloc(sizeof(WatchMap));
    wm->wd = wd;
    strncpy(wm->path, path, sizeof(wm->path));
    wm->next = mt->watches;
    mt->watches = wm;
    return wd;
}

int add_watch_recursive(MonitorThread *mt, const char *base_path) {
    add_single_watch(mt, base_path);
    DIR *dir = opendir(base_path);
    if (!dir) return -1;
    struct dirent *entry;
    char path[512];
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            add_watch_recursive(mt, path);
        }
    }
    closedir(dir);
    return 0;
}

const char* wd_to_path(MonitorThread *mt, int wd) {
    WatchMap *wm = mt->watches;
    while (wm) {
        if (wm->wd == wd) return wm->path;
        wm = wm->next;
    }
    return NULL;
}

// Remueve el watch y lo elimina de la lista
void remove_watch(MonitorThread *mt, int wd) {
    WatchMap *prev = NULL, *curr = mt->watches;
    while (curr) {
        if (curr->wd == wd) {
            inotify_rm_watch(mt->inotify_fd, curr->wd);
            if (prev) prev->next = curr->next;
            else mt->watches = curr->next;
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// --- Renombrado/movido: manejo de cookies ---
MoveCookie* pop_move_cookie(MonitorThread *mt, uint32_t cookie) {
    MoveCookie *prev = NULL, *curr = mt->move_cookies;
    while (curr) {
        if (curr->cookie == cookie) {
            if (prev) prev->next = curr->next;
            else mt->move_cookies = curr->next;
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }
    return NULL;
}

void push_move_cookie(MonitorThread *mt, uint32_t cookie, const char *from_path) {
    MoveCookie *mc = malloc(sizeof(MoveCookie));
    mc->cookie = cookie;
    strncpy(mc->from_path, from_path, sizeof(mc->from_path));
    mc->next = mt->move_cookies;
    mt->move_cookies = mc;
}

// Hilo que monitorea una memoria específica usando inotify recursivo
void* monitor_memory(void *arg) {
    MonitorThread *self = (MonitorThread*)arg;
    char mountpoint[256];
    strcpy(mountpoint, self->mountpoint);

    self->inotify_fd = inotify_init1(IN_NONBLOCK);
    self->move_cookies = NULL;
    if (self->inotify_fd < 0) {
        perror("inotify_init1");
        return NULL;
    }

    add_watch_recursive(self, mountpoint);

    char start_msg[300];
    snprintf(start_msg, sizeof(start_msg), "Monitoreando en tiempo real %s...", mountpoint);
    notify("USB", start_msg);

    char buf[EVENT_BUF_LEN];
    while (self->running && *(self->global_running)) {
        int length = read(self->inotify_fd, buf, sizeof(buf));
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100 * 1000); // 100ms
                continue;
            } else {
                perror("read inotify");
                break;
            }
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event*)&buf[i];
            const char *base = wd_to_path(self, event->wd);
            char fullpath[1024] = "";
            if (base && event->len > 0)
                snprintf(fullpath, sizeof(fullpath), "%s/%s", base, event->name);
            else if (base)
                snprintf(fullpath, sizeof(fullpath), "%s", base);

            if (event->mask & IN_CREATE) {
                if (event->mask & IN_ISDIR) {
                    char msg[700];
                    snprintf(msg, sizeof(msg), "[CREADA carpeta] %s", fullpath);
                    notify(mountpoint, msg);
                    add_watch_recursive(self, fullpath); // Nuevo dir: monitorea recursivamente
                } else {
                    char msg[700];
                    snprintf(msg, sizeof(msg), "[CREADO archivo] %s", fullpath);
                    notify(mountpoint, msg);
                }
            }
            if (event->mask & IN_DELETE) {
                char msg[700];
                snprintf(msg, sizeof(msg), "[ELIMINADO] %s", fullpath);
                notify(mountpoint, msg);
            }
            if (event->mask & IN_MODIFY) {
                char msg[700];
                snprintf(msg, sizeof(msg), "[MODIFICADO] %s", fullpath);
                notify(mountpoint, msg);
            }
            if (event->mask & IN_ATTRIB) {
                char msg[700];
                snprintf(msg, sizeof(msg), "[ATRIBUTO cambiado] %s", fullpath);
                notify(mountpoint, msg);
            }
            // RENOMBRADO/MOVIDO: emparejar MOVED_FROM/MOVED_TO por cookie
            if (event->mask & IN_MOVED_FROM) {
                if (event->len > 0) {
                    push_move_cookie(self, event->cookie, fullpath);
                    // Espera el MOVED_TO para notificar
                }
            }
            if (event->mask & IN_MOVED_TO) {
                if (event->len > 0) {
                    MoveCookie *mc = pop_move_cookie(self, event->cookie);
                    if (mc) {
                        char msg[1200];
                        // Analiza ruta padre
                        char *from_dir = strdup(mc->from_path);
                        char *to_dir   = strdup(fullpath);
                        char *from_base = strrchr(from_dir, '/');
                        char *to_base   = strrchr(to_dir, '/');
                        if (from_base) *from_base = 0;
                        if (to_base)   *to_base   = 0;
                        if (strcmp(from_dir, to_dir) == 0) {
                            snprintf(msg, sizeof(msg), "[RENOMBRADO] %s → %s", mc->from_path, fullpath);
                        } else {
                            snprintf(msg, sizeof(msg), "[MOVIDO] %s → %s", mc->from_path, fullpath);
                        }
                        notify(mountpoint, msg);
                        free(mc);
                        free(from_dir);
                        free(to_dir);
                    } else {
                        // Es un archivo movido externo (no sabemos el origen)
                        char msg[700];
                        snprintf(msg, sizeof(msg), "[CREADO (moved)] %s", fullpath);
                        notify(mountpoint, msg);
                    }
                }
            }
            if ((event->mask & IN_DELETE_SELF) || (event->mask & IN_MOVE_SELF)) {
                // Carpeta eliminada/renombrada: remueve watch
                remove_watch(self, event->wd);
            }
            i += sizeof(struct inotify_event) + event->len;
        }
    }

    // Limpieza
    while (self->watches) {
        WatchMap *tmp = self->watches;
        self->watches = self->watches->next;
        inotify_rm_watch(self->inotify_fd, tmp->wd);
        free(tmp);
    }
    // Limpieza de cookies
    while (self->move_cookies) {
        MoveCookie *tmp = self->move_cookies;
        self->move_cookies = self->move_cookies->next;
        free(tmp);
    }
    if (self->inotify_fd > 0) close(self->inotify_fd);

    char stop_msg[300];
    snprintf(stop_msg, sizeof(stop_msg), "Deteniendo monitoreo de %s.", mountpoint);
    notify("USB", stop_msg);
    return NULL;
}

void start_monitor_thread(const char *mountpoint) {
    if (find_thread(mountpoint)) return; // Ya monitorizada
    MonitorThread *t = malloc(sizeof(MonitorThread));
    strcpy(t->mountpoint, mountpoint);
    t->running = 1;
    t->watches = NULL;
    t->move_cookies = NULL;
    t->inotify_fd = 0;
    t->global_running = &global_running;
    t->next = threads;
    threads = t;
    pthread_create(&t->thread, NULL, monitor_memory, t);
}

void stop_all_threads() {
    while (threads) {
        remove_thread(threads->mountpoint);
    }
}

void usb_scan() {
    global_running = 1; // <-- ¡Esta línea es la clave!
    char devices[MAX_DEVICES][256] = {0};
    int prev_count = list_devices(devices);

    printf("Escaneando dispositivos USB en %s. Presiona Q para salir.\n", MEDIA_PATH);

    int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);
    set_nonblocking(1);

    for (int i = 0; i < prev_count; i++) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Nueva memoria USB detectada: %s", devices[i]);
        notify("USB", msg);
        start_monitor_thread(devices[i]);
    }

    while (global_running) {
        int c = getchar();
        if (c == 'Q' || c == 'q') {
            global_running = 0;
            break;
        }

        char curr_devices[MAX_DEVICES][256] = {0};
        int curr_count = list_devices(curr_devices);

        // Detectar nuevas memorias y lanzar hilos
        for (int i = 0; i < curr_count; i++) {
            int found = 0;
            for (int j = 0; j < prev_count; j++) {
                if (strcmp(curr_devices[i], devices[j]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                char msg[300];
                snprintf(msg, sizeof(msg), "Nueva memoria USB detectada: %s", curr_devices[i]);
                notify("USB", msg);
                start_monitor_thread(curr_devices[i]);
            }
        }
        // Detectar memorias retiradas y terminar hilo
        for (int i = 0; i < prev_count; i++) {
            int found = 0;
            for (int j = 0; j < curr_count; j++) {
                if (strcmp(devices[i], curr_devices[j]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                char msg[300];
                snprintf(msg, sizeof(msg), "Memoria USB retirada: %s", devices[i]);
                notify("USB", msg);
                remove_thread(devices[i]);
            }
        }
        memcpy(devices, curr_devices, sizeof(devices));
        prev_count = curr_count;
        usleep(200 * 1000); // 200ms para respuesta rápida
    }

    set_nonblocking(0);
    fcntl(STDIN_FILENO, F_SETFL, old_flags);

    stop_all_threads();
    printf("Programa terminado.\n");
}
