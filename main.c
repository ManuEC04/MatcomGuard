// main.c
#include <stdio.h>
#include <stdlib.h>
#include "funcionalidades/usbscanner.h"
#include "funcionalidades/process_scanner.h"

int main() {
    int opcion;

    while (1) {
        printf("========== MATCOM GUARD ==========\n");
        printf("1. Escáner USB\n");
        printf("2. Monitorear procesos\n");
        printf("3. Escáner de puertos\n");
        printf("0. Salir\n");
        printf("==================================\n");
        printf("Seleccione una opción: ");
        scanf("%d", &opcion);

        switch (opcion) {
            case 1:
                usb_scan();
                break;
            case 2:
                process_scan();
                break;
            case 3:
                printf("Funcionalidad aún no implementada.\n");
                break;
            case 0:
                printf("Saliendo de MATCOM GUARD...\n");
                return 0;
            default:
                printf("Opción inválida.\n");
        }

        printf("\nPresione Enter para continuar...");
        getchar(); // limpiar salto de línea de scanf
        getchar(); // esperar Enter del usuario
    }

    return 0;
}