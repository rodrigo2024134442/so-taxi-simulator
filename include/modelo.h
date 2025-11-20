// include/modelo.h
#ifndef MODELO_H
#define MODELO_H

#include <sys/types.h>   // para pid_t
#include <pthread.h>

// ===== LIMITES GERAIS =====
#define MAX_USERS     30
#define MAX_SERVICOS  100
#define MAX_VEICULOS  10

// ===== UTILIZADORES =====
typedef enum {
    U_AGUARDA = 0,
    U_EM_VIAGEM = 1
} EstadoUser;

typedef struct {
    char nome[32];
    char fifo_cli[128];   // FIFO controlador->cliente
    EstadoUser estado;
    int ativo;            // 0/1
} Utilizador;

// ===== SERVIÇOS =====
typedef enum {
    S_AGENDADO,
    S_EM_EXEC,
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

extern int km_total; 

#endif
