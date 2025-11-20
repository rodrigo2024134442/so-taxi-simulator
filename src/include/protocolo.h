// include/protocolo.h
#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <sys/types.h> // para pid_t

// === Tipo de pedido que vai pelo FIFO controlador <-> cliente ===
typedef enum
{
    CMD_HELLO = 1,  // cliente a dizer "sou o user X, este é o meu FIFO"
    CMD_AGENDAR = 2 // pedido de agendamento de serviço
    // (mais tarde: CMD_CANCELAR, CMD_CONSULTAR, etc.)
} TipoPedido;

// Estrutura genérica de pedido (por enquanto cobre HELLO e AGENDAR)
typedef struct
{
    TipoPedido tipo; // CMD_HELLO ou CMD_AGENDAR

    pid_t pid_cliente;   // PID do processo cliente (opcional, mas útil)
    char user[32];       // username
    char fifo_resp[128]; // caminho da FIFO de resposta

    // Campos usados em CMD_AGENDAR:
    int hora;       // atraso relativo (segundos até iniciar)
    char local[64]; // destino
    int dist;       // distância em km
} Pedido;

// Estrutura de resposta do controlador para o cliente
typedef struct
{
    int ok;         // 1 = sucesso, 0 = erro
    int id_servico; // p.ex. id atribuído ao serviço agendado
    char msg[128];  // mensagem para mostrar ao utilizador
} Resposta;

extern int tempo_simulado;

#endif
