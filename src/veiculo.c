// src/veiculo.c
#define _GNU_SOURCE
#include <unistd.h> // write, sleep
#include <string.h> // strlen
#include <stdlib.h> // exit
#include <stdio.h>
#include <signal.h>

// Variável global para controlar o arranque
volatile sig_atomic_t passageiro_a_bordo = 0;
volatile sig_atomic_t running = 1;

// Função auxiliar para enviar mensagens ao controlador
static void send_msg(const char *s)
{
    // escreve a string no stdout
    write(STDOUT_FILENO, s, strlen(s));
}

void handle_sinais(int sig, siginfo_t *info, void *context)
{
    (void)context; // Não usamos o contexto do CPU

    // Capturar o valor enviado pelo Controlador (via sigqueue)
    // Se foi enviado via kill(), este valor será 0 ou lixo, mas não faz mal.
    int valor_recebido = info->si_value.sival_int;

    // Capturar o PID de quem mandou (segurança/debug)
    // pid_t sender_pid = info->si_pid;

    if (sig == SIGUSR1)
    {
        // SINAL DE ARRANQUE
        passageiro_a_bordo = 1;

        // Log seguro (usando write em vez de printf)
        const char msg[] = "VEICULO: Ordem de arranque recebida.\n";
        write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    }
    else if (sig == SIGINT)
    {
        // SINAL DE PARAGEM / CANCELAMENTO
        running = 0;

        // Podemos usar o valor para distinguir "Cancelado" de "Shutdown"
        if (valor_recebido == 1)
        {
            const char msg[] = "VEICULO: Cancelamento forcado pelo Controlador.\n";
            write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        }
        else
        {
            const char msg[] = "VEICULO: Ordem de paragem recebida.\n";
            write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        }
    }
    else if (sig == SIGTERM)
    {
        running = 0;
    }
}

int main(int argc, char **argv)
{

    if (argc != 4)
    {
        fprintf(stderr, "Uso: %s <id> <local> <distancia>\n", argv[0]);
        return 1;
    }

    int id_servico = atoi(argv[1]);
    char *local = argv[2];
    int dist_km = atoi(argv[3]);

    // Configurar os sinais
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handle_sinais;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL); // Sinal para "Entrar"
    sigaction(SIGINT, &sa, NULL);  // Sinal para "Cancelar"
    sigaction(SIGTERM, &sa, NULL); // Sinal para "Matar"

    // Buffer para construir a mensagem
    char buffer[128]; 
    sprintf(buffer, "VEICULO %d: Cheguei a %s. Aguardo cliente (dist=%d km)...\n", id_servico, local, dist_km);
    send_msg(buffer);

    // === ESPERA PELO CLIENTE (SINAL SIGUSR1) ===
    while (running && !passageiro_a_bordo)
    {
        pause(); // Adormece até receber um sinal
    }

    if (!running)
        return 0;

    // Loop de 10 a 100, incrementando de 10 em 10
    // === INICIO DA VIAGEM ===
    send_msg("INICIO\n"); // Avisa o controlador

    // Loop de 10 a 100, incrementando de 10 em 10
    for (int perc = 10; perc <= 100 && running; perc += 10)
    {
        sleep(1); // Simula o tempo a passar (1s por 10%)

        sprintf(buffer, "PERC %d\n", perc);
        send_msg(buffer);
    }

    if (running)
        send_msg("FIM OK\n");

    // termina normalmente
    return 0;
}
