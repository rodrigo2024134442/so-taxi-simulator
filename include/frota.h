// include/modelo.h
#ifndef FROTA_H
#define FROTA_H

#include <sys/types.h>   // para pid_t
#include <pthread.h>

// ===== LIMITES GERAIS =====
#define MAX_USERS     30
#define MAX_SERVICOS  100
#define MAX_VEICULOS  10
#define MAX_FILA_ESPERA 100

// ===== UTILIZADORES =====
typedef enum {
    U_AGUARDA = 0,
    U_EM_VIAGEM = 1
} EstadoUser;

typedef struct {
    char nome[32];
    char fifo_cli[128];   // FIFO controlador->cliente
    pid_t pid_cliente;    // PID do processo cliente    
    EstadoUser estado;
    int ativo;            // 0/1
} Utilizador;

// ===== SERVIÇOS =====
// definir tipo FilaEspera antes de usar
typedef struct {
    int id_servico;
    int tempo_agendado;  // Quando foi agendado
} FilaEspera;

// declarar a fila e índices como externes (definir em .c)
extern FilaEspera fila_espera[MAX_FILA_ESPERA];
extern int fila_head;  // Próximo a processar
extern int fila_count; // Quantos estão na fila

typedef enum {
    S_AGENDADO,
    S_EM_CURSO,
    S_TERMINADO,
    S_CANCELADO
} EstadoServico;

typedef struct {
    int   id;
    char  user[32];       // username do dono
    int   hora;           // tempo simulado em que começa
    char  local[64];
    int   dist_km;
    EstadoServico estado;
    pid_t pid_veiculo;    // -1 se ainda não lançado
    int   perc_concl;     // 0..100
    int   na_fila;        // 0/1 - indica se já entrou na fila de espera
} Servico;

// ===== VEÍCULOS (processos em execução) =====
typedef struct {
    int   ativo;          // 0/1
    pid_t pid;            // PID do processo veiculo
    int   fd_telemetria;  // descritor de leitura do pipe
    int   id_servico;     // serviço associado
} Veiculo;

// ===== VARIÁVEIS GLOBAIS (apenas declaradas) =====
extern Utilizador users[MAX_USERS];
extern int n_users;

extern Servico servicos[MAX_SERVICOS];
extern int n_servicos;

extern pthread_mutex_t mux_servicos;

extern Veiculo veiculos[MAX_VEICULOS];
extern int max_veiculos_ativos;

extern int km_total; 

extern int em_encerramento;

#endif
