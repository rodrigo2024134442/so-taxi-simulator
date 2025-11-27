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

// Handler para receber ordens do Controlador
void handle_sinais(int sig)
{
    if (sig == SIGUSR1)
    {
        passageiro_a_bordo = 1; // O controlador disse "o cliente entrou"
    }
    else if (sig == SIGINT || sig == SIGTERM)
    {
        running = 0; // Ordem para terminar
    }
}

int main(int argc, char **argv)
{
    // por agora, ignoramos os argumentos
    /*   (void)argc;
      (void)argv;

      send_msg("TESTE\n");
      sleep(1);
   */
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
    sa.sa_handler = handle_sinais;
    sigaction(SIGUSR1, &sa, NULL); // Sinal para "Entrar"
    sigaction(SIGINT, &sa, NULL);  // Sinal para "Matar"

        
    char buffer[128]; // Buffer para construir a mensagem
    sprintf(buffer, "VEICULO %d: Cheguei a %s. Aguardo cliente (dist=%d km)...\n",
    id_servico, local, dist_km);
    send_msg(buffer);
   
    /* char buf_chegada[64];
    sprintf(buf_chegada, "CHEGUEI %d\n", id_servico); // "CHEGUEI" é a palavra-chave
    send_msg(buf_chegada); */

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
