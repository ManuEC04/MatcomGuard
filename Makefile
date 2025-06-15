CC = gcc
CFLAGS = -Wall
OBJ = main.o funcionalidades/usbscanner.o funcionalidades/process_scan.o funcionalidades/port_scanner.o

matcom_guard: $(OBJ)
	$(CC) $(CFLAGS) -o matcom_guard $(OBJ)

main.o: main.c funcionalidades/usbscanner.h funcionalidades/process_scan.h funcionalidades/port_scanner.h
	$(CC) $(CFLAGS) -c main.c

funcionalidades/usbscanner.o: funcionalidades/usbscanner.c funcionalidades/usbscanner.h
	$(CC) $(CFLAGS) -c funcionalidades/usbscanner.c -o funcionalidades/usbscanner.o

funcionalidades/process_scan.o: funcionalidades/process_scan.c funcionalidades/process_scan.h
	$(CC) $(CFLAGS) -c funcionalidades/process_scan.c -o funcionalidades/process_scan.o

funcionalidades/port_scanner.o: funcionalidades/port_scanner.c funcionalidades/port_scanner.h
	$(CC) $(CFLAGS) -c funcionalidades/port_scanner.c -o funcionalidades/port_scanner.o

clean:
	rm -f *.o funcionalidades/*.o matcom_guard