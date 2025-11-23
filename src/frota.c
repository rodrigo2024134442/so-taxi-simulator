// src/modelo.c
#include "frota.h"

// definição REAL das variáveis globais
Utilizador users[MAX_USERS];
int n_users = 0;

Servico servicos[MAX_SERVICOS];
int n_servicos = 0;

Veiculo veiculos[MAX_VEICULOS];
int max_veiculos_ativos = MAX_VEICULOS;

int km_total = 0;

int tempo_simulado = 0;

/* definição real do mutex */
pthread_mutex_t mux_servicos = PTHREAD_MUTEX_INITIALIZER;

/* Bandeira de encerramento */
int em_encerramento = 0; // 0 = Normal, 1 = A fechar (não aceita novos pedidos)