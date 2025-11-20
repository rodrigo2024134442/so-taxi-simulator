#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>

#include "modelo.h"
#include "protocolo.h"

#define FIFO_DIR "./tmp"
#define FIFO_PEDIDOS FIFO_DIR "/taxi_pedidos"

int tempo_simulado = 0;
extern char **environ;   // usado no execve do veiculo

// Prototipos internos
static void cria_servico_teste(void);
static void processa_mensagem_veiculo(int id_servico, const char *msg);
static void spawn_veiculo_para_servico(Servico *s);
static int  encontra_slot_livre_veiculo(void);
static void *thread_telemetria(void *arg);
static void *thread_relogio(void *arg);
static void loop_pedidos(void);
static void tratar_pedido_agendar(const Pedido *p);

int main(void)
{
    printf("[CTRL] main começou\n");

    // Garante que o diretório dos FIFOs existe
    if (mkdir(FIFO_DIR, 0777) == -1 && errno != EEXIST)
    {
        perror("[CTRL] erro a criar diretório FIFO_DIR");
        return 1;
    }

    // (Re)criar FIFO de pedidos
    unlink(FIFO_PEDIDOS);
    if (mkfifo(FIFO_PEDIDOS, 0666) == -1)
    {
        perror("[CTRL] mkfifo FIFO_PEDIDOS");
        return 1;
    }
    printf("[CTRL] FIFO de pedidos criado em %s\n", FIFO_PEDIDOS);

    // Serviços de teste (podem ser removidos mais tarde)
    cria_servico_teste();

    // Thread de relógio
    pthread_t th_clock;
    pthread_create(&th_clock, NULL, thread_relogio, NULL);

    // Fica a atender pedidos (HELLO/AGENDAR) neste thread
    loop_pedidos();

    printf("[CTRL] a terminar\n");
    unlink(FIFO_PEDIDOS);
    return 0;
}

// ----------------------------------------------------------------------
//  Inicializa alguns serviços de teste (ana/miguel) só para demo
// ----------------------------------------------------------------------
static void cria_servico_teste(void)
{
    // Serviço 1
    Servico *s = &servicos[0];
    s->id = 1;
    strcpy(s->user, "ana");
    s->hora = 3;
    strcpy(s->local, "ISEC");
    s->dist_km = 3;
    s->estado = S_AGENDADO;
    s->pid_veiculo = -1;
    s->perc_concl = 0;

    // Serviço 2
    Servico *t = &servicos[1];
    t->id = 2;
    strcpy(t->user, "miguel");
    t->hora = 3; // mesma hora → arrancam os dois!
    strcpy(t->local, "Coimbra");
    t->dist_km = 5;
    t->estado = S_AGENDADO;
    t->pid_veiculo = -1;
    t->perc_concl = 0;

    n_servicos = 2;
}

// ----------------------------------------------------------------------
//  Processa mensagens vindas do veiculo (via pipe)
// ----------------------------------------------------------------------
static void processa_mensagem_veiculo(int id_servico, const char *msg)
{
    pthread_mutex_lock(&mux_servicos);

    Servico *s = NULL;

    if (id_servico >= 1 && id_servico <= n_servicos)
        s = &servicos[id_servico - 1];

    if (!s)
    {
        pthread_mutex_unlock(&mux_servicos);
        return;
    }

    if (strncmp(msg, "INICIO", 6) == 0)
    {
        s->estado = S_EM_EXEC;
        s->perc_concl = 0;
    }
    else if (strncmp(msg, "PERC ", 5) == 0)
    {
        int perc = atoi(msg + 5);
        s->perc_concl = perc;
    }
    else if (strncmp(msg, "FIM", 3) == 0)
    {
        s->estado = S_TERMINADO;
        s->perc_concl = 100;
    }

    printf("[CTRL] servico %d (%s)-> perc = %d%%\n",
           s->id, s->user, s->perc_concl);

    pthread_mutex_unlock(&mux_servicos);
}

// ----------------------------------------------------------------------
//  Lança um processo veiculo e liga-o a um pipe de telemetria
// ----------------------------------------------------------------------
static void spawn_veiculo_para_servico(Servico *s)
{
    int p[2];

    if (pipe(p) == -1)
    {
        perror("[CTRL] pipe");
        return;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("[CTRL] fork");
        close(p[0]);
        close(p[1]);
        return;
    }

    if (pid == 0)
    {
        // FILHO (veiculo)
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);

        const char *m = "[FILHO] antes do execve\n";
        write(STDERR_FILENO, m, strlen(m));

        char *argv[] = {"veiculo", NULL};
        execve("./veiculo", argv, environ);
        _exit(1);
    }

    // PAI (controlador)
    close(p[1]); // só vamos ler

    int idx = encontra_slot_livre_veiculo();
    if (idx < 0)
    {
        // não há slot livre -> fecha pipe e (idealmente) mata o veiculo
        close(p[0]);
        return;
    }

    veiculos[idx].ativo         = 1;
    veiculos[idx].pid           = pid;
    veiculos[idx].fd_telemetria = p[0];
    veiculos[idx].id_servico    = s->id;

    s->pid_veiculo = pid;
    s->estado      = S_EM_EXEC;
    s->perc_concl  = 0;

    pthread_t th;
    pthread_create(&th, NULL, thread_telemetria, (void *)(intptr_t)idx);
    pthread_detach(th); // não precisamos de dar join mais tarde

    printf("[CTRL] veiculo para servico id=%d lançado (idx=%d)\n",
           s->id, idx);
}

// ----------------------------------------------------------------------
//  Procura um slot livre no array de veiculos[]
// ----------------------------------------------------------------------
static int encontra_slot_livre_veiculo(void)
{
    for (int i = 0; i < MAX_VEICULOS; i++)
    {
        if (!veiculos[i].ativo)
        {
            return i;
        }
    }
    return -1; // não há slot livre
}

// ----------------------------------------------------------------------
//  Thread que lê telemetria do veiculo
// ----------------------------------------------------------------------
static void *thread_telemetria(void *arg)
{
    int idx        = (int)(intptr_t)arg; // índice em veiculos[]
    int fd         = veiculos[idx].fd_telemetria;
    int id_servico = veiculos[idx].id_servico;

    char buf[256];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        processa_mensagem_veiculo(id_servico, buf);
    }

    close(fd);
    veiculos[idx].ativo = 0;

    printf("[CTRL] veiculo idx=%d terminou\n", idx);
    return NULL;
}

// ----------------------------------------------------------------------
//  Thread de relógio: incrementa tempo_simulado e lança serviços
// ----------------------------------------------------------------------
static void *thread_relogio(void *arg)
{
    (void)arg; // evita warning

    while (1)
    {
        sleep(1);
        tempo_simulado++;

        pthread_mutex_lock(&mux_servicos);
        // varre os serviços à espera
        for (int i = 0; i < n_servicos; i++)
        {
            Servico *s = &servicos[i];

            if (s->estado == S_AGENDADO &&
                s->hora == tempo_simulado)
            {
                printf("[RELOGIO] hora=%d, lançar servico id=%d\n",
                       tempo_simulado, s->id);
                spawn_veiculo_para_servico(s);
            }
        }
        pthread_mutex_unlock(&mux_servicos);
    }

    return NULL;
}

// ----------------------------------------------------------------------
//  Trata pedido AGENDAR vindo do cliente
// ----------------------------------------------------------------------
static void tratar_pedido_agendar(const Pedido *p)
{
    Resposta r;

    pthread_mutex_lock(&mux_servicos);

    if (n_servicos >= MAX_SERVICOS)
    {
        pthread_mutex_unlock(&mux_servicos);
        r.ok         = 0;
        r.id_servico = -1;
        snprintf(r.msg, sizeof(r.msg),
                 "Nao ha mais servicos disponiveis.\n");
    }
    else
    {
        Servico *s = &servicos[n_servicos];
        s->id = n_servicos + 1;

        strncpy(s->user, p->user, sizeof(s->user));
        s->user[sizeof(s->user) - 1] = '\0';

        // p->hora é atraso relativo em relação ao tempo_simulado atual
        s->hora = tempo_simulado + p->hora;

        strncpy(s->local, p->local, sizeof(s->local));
        s->local[sizeof(s->local) - 1] = '\0';

        s->dist_km     = p->dist;
        s->estado      = S_AGENDADO;
        s->pid_veiculo = -1;
        s->perc_concl  = 0;

        n_servicos++;

        pthread_mutex_unlock(&mux_servicos);

        r.ok         = 1;
        r.id_servico = s->id;
        snprintf(r.msg, sizeof(r.msg),
                 "Servico %d agendado para daqui a %d s (hora_sim=%d), local=%s, dist=%d km\n",
                 s->id, p->hora, s->hora, s->local, s->dist_km);

        printf("[CTRL] agendado serviço id=%d para user=%s, daqui a %d s (hora_sim=%d), local=%s, dist=%d km\n",
               s->id, s->user, p->hora, s->hora, s->local, s->dist_km);
    }

    // Enviar Resposta na FIFO do cliente
    int fd_cli = open(p->fifo_resp, O_WRONLY);
    if (fd_cli == -1)
    {
        perror("[CTRL] nao consegui abrir fifo_resp do cliente");
        return;
    }

    ssize_t n = write(fd_cli, &r, sizeof(r));
    if (n != sizeof(r))
    {
        perror("[CTRL] erro a escrever Resposta no fifo_resp");
    }
    close(fd_cli);
}

// ----------------------------------------------------------------------
//  Loop principal de pedidos: lê estruturas Pedido do FIFO_PEDIDOS
// ----------------------------------------------------------------------
static void loop_pedidos(void)
{
    int fd_read = open(FIFO_PEDIDOS, O_RDONLY);
    if (fd_read == -1)
    {
        perror("[CTRL] nao consegui abrir FIFO_PEDIDOS para leitura");
        return;
    }

    // abrir um descritor dummy em escrita para o FIFO não fechar
    int fd_dummy = open(FIFO_PEDIDOS, O_WRONLY);
    if (fd_dummy == -1)
    {
        perror("[CTRL] aviso: nao consegui abrir FIFO_PEDIDOS em escrita");
        // não saímos; mas o read pode dar 0 se ninguém estiver a escrever
    }

    printf("[CTRL] loop_pedidos a aguardar pedidos...\n");

    while (1)
    {
        Pedido p;
        ssize_t n = read(fd_read, &p, sizeof(p));
        if (n == -1)
        {
            if (errno == EINTR)
                continue;
            perror("[CTRL] erro na leitura de FIFO_PEDIDOS");
            break;
        }
        if (n == 0)
        {
            // EOF no FIFO (todos os escritores fecharam)
            printf("[CTRL] read devolveu 0 (EOF no FIFO)\n");
            sleep(1);
            continue;
        }
        if (n != sizeof(p))
        {
            printf("[CTRL] tamanho inesperado de Pedido (%zd bytes)\n", n);
            continue;
        }

        if (p.tipo == CMD_HELLO)
        {
            printf("[CTRL] recebi HELLO de user=%s (pid=%d, fifo=%s)\n",
                   p.user, (int)p.pid_cliente, p.fifo_resp);

            // opcional: responder com uma mensagem simples
            Resposta r;
            r.ok         = 1;
            r.id_servico = 0;
            snprintf(r.msg, sizeof(r.msg), "Bem-vindo, %s!\n", p.user);

            int fd_cli = open(p.fifo_resp, O_WRONLY);
            if (fd_cli != -1)
            {
                write(fd_cli, &r, sizeof(r));
                close(fd_cli);
            }
        }
        else if (p.tipo == CMD_AGENDAR)
        {
            tratar_pedido_agendar(&p);
        }
        else
        {
            printf("[CTRL] pedido desconhecido do tipo %d\n", p.tipo);
        }
    }

    close(fd_read);
    if (fd_dummy != -1)
        close(fd_dummy);
}
