#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>

#include "modelo.h"
#include "protocolo.h"

#define FIFO_DIR "./tmp"
#define FIFO_PEDIDOS FIFO_DIR "/taxi_pedidos"

// variável global modificada pelo handler de sinais
volatile sig_atomic_t running = 1;

static void loop_comandos(const char *user, const char *fifo_resp_path);
static void setup_directory(void);
static void handle_signal(int sig, siginfo_t *info, void *context);

int main(int argc, char **argv)
{
    setup_directory(); // cria ./tmp se ainda não existir

    if (argc != 2)
    {
        fprintf(stderr, "Uso: %s <username>\n", argv[0]);
        return 1;
    }
    const char *user = argv[1];

    // Instalar handlers de SIGINT e SIGPIPE
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    // Caminho da FIFO deste cliente
    char fifo_resp[256];
    snprintf(fifo_resp, sizeof(fifo_resp), FIFO_DIR "/taxi_cli_%s", user);

    // limpa e cria FIFO de resposta
    unlink(fifo_resp);
    if (mkfifo(fifo_resp, 0666) == -1)
    {
        perror("[CLI] mkfifo resposta");
        return 1;
    }

    // Enviar HELLO inicial ao controlador usando a estrutura Pedido
    int fd_out = open(FIFO_PEDIDOS, O_WRONLY);
    if (fd_out < 0)
    {
        perror("[CLI] controlador nao esta a correr?");
        unlink(fifo_resp);
        return 1;
    }

    Pedido hello;
    memset(&hello, 0, sizeof(hello));
    hello.tipo        = CMD_HELLO;
    hello.pid_cliente = getpid();
    strncpy(hello.user, user, sizeof(hello.user));
    hello.user[sizeof(hello.user) - 1] = '\0';
    strncpy(hello.fifo_resp, fifo_resp, sizeof(hello.fifo_resp));
    hello.fifo_resp[sizeof(hello.fifo_resp) - 1] = '\0';

    ssize_t n = write(fd_out, &hello, sizeof(hello));
    if (n != sizeof(hello))
    {
        perror("[CLI] erro ao enviar HELLO");
        close(fd_out);
        unlink(fifo_resp);
        return 1;
    }
    close(fd_out);

    printf("[CLI] HELLO enviado para o controlador.\n");

    // Entrar no loop de comandos (agendar / sair)
    loop_comandos(user, fifo_resp);

    // no fim, limpar FIFO
    unlink(fifo_resp);
    return 0;
}

static void loop_comandos(const char *user, const char *fifo_resp_path)
{
    char linha[256];

    while (running)
    {
        printf("comando> ");
        fflush(stdout);

        if (!fgets(linha, sizeof(linha), stdin))
        {
            // EOF
            break;
        }

        // tirar \n
        linha[strcspn(linha, "\n")] = '\0';

        if (linha[0] == '\0')
            continue;

        if (strcmp(linha, "agendar") == 0)
        {
            int hora, dist;
            char local[64];
            char buf[128];

            // Hora relativa
            printf("Indique em quantos segundos quer iniciar o serviço: ");
            if (!fgets(buf, sizeof(buf), stdin))
                break;
            if (sscanf(buf, "%d", &hora) != 1 || hora < 0)
            {
                printf("Hora inválida.\n");
                continue;
            }

            // Local
            printf("Indique o local: ");
            if (!fgets(local, sizeof(local), stdin))
                break;
            local[strcspn(local, "\n")] = '\0';

            // Distância
            printf("Indique a distância (km): ");
            if (!fgets(buf, sizeof(buf), stdin))
                break;
            if (sscanf(buf, "%d", &dist) != 1 || dist <= 0)
            {
                printf("Distância inválida.\n");
                continue;
            }

            // Preencher estrutura Pedido
            Pedido p;
            memset(&p, 0, sizeof(p));
            p.tipo        = CMD_AGENDAR;
            p.pid_cliente = getpid();
            strncpy(p.user, user, sizeof(p.user));
            p.user[sizeof(p.user) - 1] = '\0';
            strncpy(p.fifo_resp, fifo_resp_path, sizeof(p.fifo_resp));
            p.fifo_resp[sizeof(p.fifo_resp) - 1] = '\0';
            p.hora = hora;
            p.dist = dist;
            strncpy(p.local, local, sizeof(p.local));
            p.local[sizeof(p.local) - 1] = '\0';

            printf("[DEBUG CLI] vou enviar pedido: user=%s, hora=%d, local=%s, dist=%d\n",
                   p.user, p.hora, p.local, p.dist);

            // Abrir FIFO_PEDIDOS para escrever o pedido
            int fd_ped = open(FIFO_PEDIDOS, O_WRONLY);
            if (fd_ped == -1)
            {
                perror("[CLI] nao consegui abrir FIFO_PEDIDOS");
                continue;
            }

            ssize_t n = write(fd_ped, &p, sizeof(p));
            if (n == -1)
            {
                if (errno != EPIPE)
                    perror("[CLI] erro na escrita do pedido");
                close(fd_ped);
                continue;
            }
            if (n != sizeof(p))
            {
                printf("[CLI] aviso: escrevi apenas %zd bytes do pedido\n", n);
            }
            close(fd_ped);

            // Ler a resposta do controlador na FIFO do cliente
            int fd_resp = open(fifo_resp_path, O_RDONLY);
            if (fd_resp < 0)
            {
                perror("[CLI] nao consegui abrir fifo_resp para ler");
            }
            else
            {
                Resposta r;
                n = read(fd_resp, &r, sizeof(r));
                if (n == sizeof(r))
                {
                    if (r.ok)
                        printf("[CLI] Sucesso: %s", r.msg);
                    else
                        printf("[CLI] Erro: %s", r.msg);
                }
                else if (n == -1)
                {
                    if (errno != EINTR)
                        perror("[CLI] erro na leitura da resposta");
                }
                else if (n == 0)
                {
                    printf("[CLI] resposta: EOF (0 bytes)\n");
                }
                else
                {
                    printf("[CLI] resposta com tamanho inesperado (%zd bytes)\n", n);
                }
                close(fd_resp);
            }
        }
        else if (strcmp(linha, "sair") == 0)
        {
            printf("[CLI] a terminar.\n");
            break;
        }
        else
        {
            printf("[CLI] comando desconhecido. Use: agendar / sair\n");
        }
    }
}

static void setup_directory(void)
{
    // Tenta criar o diretório FIFO_DIR
    if (mkdir(FIFO_DIR, 0777) == -1)
    {
        if (errno == EEXIST)
        {
            // Já existe, tudo bem
        }
        else
        {
            perror("[CLI] Erro ao criar diretório FIFO_DIR");
        }
    }
}

static void handle_signal(int sig, siginfo_t *info, void *context)
{
    (void)info;
    (void)context;
    if (sig == SIGINT || sig == SIGPIPE)
    {
        running = 0;
    }
}
