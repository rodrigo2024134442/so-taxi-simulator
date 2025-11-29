# ==============================================================================
# CONFIGURAÇÃO GERAL
# ==============================================================================
CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu17 -Iinclude -pthread

# ==============================================================================
# ALVOS (TARGETS)
# ==============================================================================

all: controlador cliente veiculo

# ------------------------------------------------------------------------------
# 1. CONTROLADOR (Depende de controlador.o e frota.o)
# ------------------------------------------------------------------------------
# Linkagem: Junta os objetos para criar o executável
controlador: src/controlador.o src/frota.o
	$(CC) $(CFLAGS) -o controlador src/controlador.o src/frota.o

# Compilação: Cria o objeto controlador.o
src/controlador.o: src/controlador.c include/taxis_protocol.h include/frota.h
	$(CC) $(CFLAGS) -c src/controlador.c -o src/controlador.o

# Compilação: Cria o objeto frota.o
src/frota.o: src/frota.c include/frota.h
	$(CC) $(CFLAGS) -c src/frota.c -o src/frota.o

# ------------------------------------------------------------------------------
# 2. CLIENTE
# ------------------------------------------------------------------------------
cliente: src/cliente.o
	$(CC) $(CFLAGS) -o cliente src/cliente.o

src/cliente.o: src/cliente.c include/taxis_protocol.h include/cliente_ui.h
	$(CC) $(CFLAGS) -c src/cliente.c -o src/cliente.o

# ------------------------------------------------------------------------------
# 3. VEÍCULO
# ------------------------------------------------------------------------------
veiculo: src/veiculo.o
	$(CC) $(CFLAGS) -o veiculo src/veiculo.o

src/veiculo.o: src/veiculo.c
	$(CC) $(CFLAGS) -c src/veiculo.c -o src/veiculo.o

# ==============================================================================
# LIMPEZA E ENTREGA
# ==============================================================================

clean:
	# Apaga os executáveis
	rm -f controlador cliente veiculo
	# Apaga os ficheiros objeto (.o) na pasta src
	rm -f src/*.o
	
	# Apaga os FIFOs temporários (opcional)
	#rm -f tmp/*





