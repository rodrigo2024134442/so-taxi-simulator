// src/modelo.c
#include "modelo.h"

// definição REAL das variáveis globais
Utilizador users[MAX_USERS];
int n_users = 0;

Servico servicos[MAX_SERVICOS];
int n_servicos = 0;

Veiculo veiculos[MAX_VEICULOS];

int km_total = 0; 

int tempo_ = 0;

/* definição real do mutex */
pthread_mutex_t mux_servicos = PTHREAD_MUTEX_INITIALIZER;