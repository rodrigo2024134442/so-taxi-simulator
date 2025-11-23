CC=gcc
CFLAGS=-Wall -Wextra -O2 -std=c17 -Iinclude -pthread

all: controlador cliente veiculo

controlador: src/controlador.c src/frota.c
	$(CC) $(CFLAGS) -o controlador src/controlador.c src/frota.c

cliente: src/cliente.c
	$(CC) $(CFLAGS) -o cliente src/cliente.c

veiculo: src/veiculo.c
	$(CC) $(CFLAGS) -o veiculo src/veiculo.c

clean:
	rm -f controlador cliente veiculo
