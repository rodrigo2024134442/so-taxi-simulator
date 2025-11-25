// src/modelo.c
#include "frota.h"

// definição REAL das variáveis globais
FilaEspera fila_espera[MAX_FILA_ESPERA];
int fila_head = 0;
int fila_count = 0;

Utilizador users[MAX_USERS];
int n_users = 0;

Servico servicos[MAX_SERVICOS];
int n_servicos = 0;

pthread_mutex_t mux_servicos = PTHREAD_MUTEX_INITIALIZER;

Veiculo veiculos[MAX_VEICULOS];
int max_veiculos_ativos = MAX_VEICULOS;

int km_total = 0;
int em_encerramento = 0; // 0 = Normal, 1 = A fechar (não aceita novos pedidos)

// definir tempo_simulado 
int tempo_simulado = 0;