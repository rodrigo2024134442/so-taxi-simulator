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

#include "comunicacao.h"
#include "frota.h"

#define FIFO_DIR "./tmp"
#define FIFO_PEDIDOS FIFO_DIR "/taxi_pedidos"

extern int tempo_simulado;              // tempo simulado atual
extern char **environ;                  // usado no execve do veiculo, para aceder e passar as Variáveis de Ambiente do sistema operativo.
volatile sig_atomic_t running_ctrl = 1; // Controlador a correr

// Prototipos internos so pertencentes ao controlador
static void cria_servico_teste(void) __attribute__((unused));           // só para demo
static void processa_mensagem_veiculo(int id_servico, const char *msg); // processa msg veiculo
static void spawn_veiculo_para_servico(Servico *s);                     // lança veiculo
static void notificar_cliente(const Servico *s, const char *texto);     // notifica cliente
static int encontra_slot_livre_veiculo(void);                           // encontra slot livre em veiculos[]
static void *thread_telemetria(void *arg);                              // Thread que lê telemetria
static void *thread_relogio(void *arg);                                 // Thread do relógio
static void registar_utilizador(const Pedido *p);                       // regista novo utilizador
static void loop_pedidos(void);                                         // Loop de atendimento a pedidos
static void tratar_pedido_agendar(const Pedido *p);                     // trata pedido AGENDAR
static void tratar_pedido_entrar(const Pedido *p);                      // trata pedido ENTRAR
static void tratar_pedido_cancelar_user(const Pedido *p);               // trata pedido CANCELAR
static void tratar_pedido_consultar(const Pedido *p);                   // trata pedido CONSULTAR
static void *thread_clientes(void *arg);                                // Thread de atendimento a clientes
static int conta_servicos_ativos(void);                                 // Conta serviços em execução
static void cmd_listar(void);                                           // Procedimento listar
static void cmd_frota(void);                                            // Procedimento frota
static void cmd_utiliz(void);                                           // Procedimento utiliz
static void cmd_cancelar(int id_servico);                               // Procedimento cancelar
static void cmd_terminar(void);                                         // Procedimento de encerramento
void handle_sigint(int sig);                                            // Handler para sinal SIGINT (Ctrl+C)

int main(void)
{
    printf("[CTRL] main começou. PID=%d\n", getpid());

    // Verificar variável de ambiente NVEICULOS
    char *env_n = getenv("NVEICULOS");
    if (env_n)
    {
        int n = atoi(env_n);
        // Validação de segurança: deve ser > 0 e <= ao tamanho físico do array
        if (n > 0 && n <= MAX_VEICULOS)
        {
            max_veiculos_ativos = n;
            printf("[CTRL] Configuração NVEICULOS aceite: %d veiculos.\n", n);
        }
        else
        {
            printf("[CTRL] Aviso: NVEICULOS inválido (%d). A usar defeito: %d\n", n, MAX_VEICULOS);
        }
    }
    else
    {
        printf("[CTRL] NVEICULOS não definido. A usar defeito: %d\n", max_veiculos_ativos);
    }

    // 1. Configuração Inicial (Diretorios, Sinais, FIFOs)
    if (mkdir(FIFO_DIR, 0777) == -1 && errno != EEXIST)
    {
        perror("[CTRL] erro a criar diretório FIFO_DIR");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);  // Ctrl+C
    sigaction(SIGHUP, &sa, NULL);  // Hangup do remote
    sigaction(SIGTERM, &sa, NULL); // Kill default
    signal(SIGPIPE, SIG_IGN);      // Ignora erros de escrita em pipes quebrados

    // (Re)criar FIFO de pedidos
    // unlink(FIFO_PEDIDOS);

    if (mkfifo(FIFO_PEDIDOS, 0666) == -1)
    {
        perror("[CTRL] mkfifo FIFO_PEDIDOS");
        return 1;
    }
    printf("[CTRL] FIFO de pedidos criado em %s\n", FIFO_PEDIDOS);

    // 2. Lançar Threads Auxiliares
    pthread_t th_clock, th_clientes;
    // Thread do Relógio (Tempo e Lançamento de Veículos)
    pthread_create(&th_clock, NULL, thread_relogio, NULL);
    // Thread de Atendimento a Clientes (comandos administrativos)
    pthread_create(&th_clientes, NULL, thread_clientes, NULL);

    printf("[CTRL] Sistema pronto. Aceitar comandos de admin (listar, frota, ...)\n");

    // 3. Loop Principal (Administrador - STDIN)
    char linha[256];
    while (running_ctrl)
    {
        // printf("admin> "); fflush(stdout); // Opcional: Prompt

        if (!fgets(linha, sizeof(linha), stdin))
            break;
        linha[strcspn(linha, "\n")] = '\0';

        if (strcmp(linha, "listar") == 0)
        {
            cmd_listar();
        }
        else if (strcmp(linha, "frota") == 0)
        {
            cmd_frota();
        }
        else if (strcmp(linha, "utiliz") == 0)
        {
            cmd_utiliz();
        }
        else if (strcmp(linha, "terminar") == 0)
        {
            cmd_terminar();
        }
        else if (strcmp(linha, "km") == 0)
        {
            printf("Total acumulado da frota: %d km\n", km_total);
        }
        else if (strcmp(linha, "hora") == 0)
        {
            printf("Tempo simulado: %d ticks\n", tempo_simulado);
        }
        else if (strcmp(linha, "ajuda") == 0)
        {
            printf("Comandos: listar, frota, utiliz, km, hora, cancelar <id>, terminar\n");
        }
        else if (strncmp(linha, "cancelar", 8) == 0)
        {
            int id_cancelar = 0;
            // Tenta ler "cancelar 1", "cancelar 2", etc.
            if (sscanf(linha, "cancelar %d", &id_cancelar) == 1)
            {
                // Precisas de declarar esta função nova no topo!
                cmd_cancelar(id_cancelar);
            }
            else
            {
                printf("Uso: cancelar <id_servico>\n");
            }
        }
        else if (strlen(linha) > 0)
        {
            printf("Comando desconhecido.\n");
        }
    }

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
    s->na_fila = 0; // inicializar flag

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
    t->na_fila = 0;

    n_servicos = 2;
}

// ----------------------------------------------------------------------
//  Processa mensagens vindas do veiculo (via pipe)
// ----------------------------------------------------------------------
static void processa_mensagem_veiculo(int id_servico, const char *msg)
{
    pthread_mutex_lock(&mux_servicos);

    Servico *s = NULL;
    // Encontrar o serviço pelo ID
    if (id_servico >= 1 && id_servico <= n_servicos)
        s = &servicos[id_servico - 1];

    if (!s)
    {
        pthread_mutex_unlock(&mux_servicos);
        return;
    }
    // --- Lógica de Mensagens ---

    // 1. CHEGADA (Veículo -> Controlador -> Cliente)
    // 1. O veículo avisa que chegou
    if (strncmp(msg, "VEICULO", 7) == 0)
    {
        // Vamos avisar o cliente!
        printf("[CTRL] Veiculo chegou ao cliente %s (Servico %d)\n", s->user, s->id);
        notificar_cliente(s, "O seu Taxi chegou! Escreva 'entrar'.");
    }
    // 2. INICIO DA VIAGEM (quando recebe SIGUSR1 e arranca)
    else if (strncmp(msg, "INICIO", 6) == 0)
    {
        s->estado = S_EM_CURSO;
        s->perc_concl = 0;
        printf("[CTRL] Servico %d iniciou viagem.\n", s->id);
        // Avisar cliente que arrancou
        notificar_cliente(s, "Viagem iniciada!");
    }
    // 3. PROGRESSO (PERC)
    else if (strncmp(msg, "PERC ", 5) == 0)
    {
        int perc = atoi(msg + 5);
        s->perc_concl = perc;
        // Criar mensagem para o cliente: "Viagem: 10%..."
        char msg_cli[64];
        snprintf(msg_cli, sizeof(msg_cli), "Viagem em curso: %d%%", perc);
        notificar_cliente(s, msg_cli);
    }
    // 4. Fim da viagem
    else if (strncmp(msg, "FIM", 3) == 0)
    {
        s->estado = S_TERMINADO;
        s->perc_concl = 100;
        // === CONTABILIZAR QUILÓMETROS ===
        km_total += s->dist_km; // <--- AQUI está a resposta aos KMs

        printf("[CTRL] Servico %d concluido. Total frota: %d Km.\n", s->id, km_total);
        // Avisar cliente que a viagem terminou
        char msg_cli[128];
        snprintf(msg_cli, sizeof(msg_cli), "Viagem terminada! Total: %d Km. Obrigado.", s->dist_km);
        notificar_cliente(s, msg_cli);
    }

    /* printf("[CTRL] servico %d (%s)-> perc = %d%%\n",
           s->id, s->user, s->perc_concl); */

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

    // CRIAR PROCESSO FILHO
    // O filho herda os descritores de ficheiro. Usamos dup2 para
    // redirecionar o stdout do filho para o pipe de escrita.
    // Assim, tudo o que o veículo fizer 'printf' vai parar ao pipe de leitura do pai.
    if (pid == 0)
    {
        // FILHO (veiculo)
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);

        const char *m = "[FILHO] antes do execve\n";
        write(STDERR_FILENO, m, strlen(m));

        /* char *argv[] = {"veiculo", NULL};
        execve("./veiculo", argv, environ); */
        // Preparar argumentos para o execve
        char str_id[20];
        char str_dist[20];

        // Converter inteiros para string
        sprintf(str_id, "%d", s->id);
        sprintf(str_dist, "%d", s->dist_km);

        // O formato será: ./veiculo <ID> <LOCAL> <DISTANCIA>
        char *argv[] = {
            "veiculo", // argv[0]
            str_id,    // argv[1]
            s->local,  // argv[2]
            str_dist,  // argv[3]
            NULL       // Sentinela
        };

        execve("./veiculo", argv, environ);

        // Se chegar aqui, deu erro
        perror("[FILHO] erro no execve");

        _exit(1);
    }

    close(p[1]); // só vamos ler

    int idx = encontra_slot_livre_veiculo();
    if (idx < 0)
    {
        kill(pid, SIGTERM);
        close(p[0]);
        
        // ✅ NOTIFICAR CLIENTE
        // (Opcional, ou marcar serviço para retry)
        printf("[CTRL] ERRO: Sem slots para servico id=%d (PID filho: %d terminado).\n",
               s->id, pid);
        return;
    }

    veiculos[idx].ativo = 1;
    veiculos[idx].pid = pid;
    veiculos[idx].fd_telemetria = p[0];
    veiculos[idx].id_servico = s->id;

    s->pid_veiculo = pid;
    s->estado = S_EM_CURSO;
    s->perc_concl = 0;

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
    for (int i = 0; i < max_veiculos_ativos; i++)
    {
        if (!veiculos[i].ativo)
        {
            return i;
        }
    }
    return -1; // não há slot livre
}

// ----------------------------------------------------------------------
// Encontrar o veículo certo desse utilizador e enviar o sinal.
// ----------------------------------------------------------------------
static void tratar_pedido_entrar(const Pedido *p)
{
    Resposta r;
    r.ok = 0;
    r.id_servico = -1;
    strcpy(r.msg, "Nenhum veiculo a espera.");

    pthread_mutex_lock(&mux_servicos);

    // Procurar um serviço deste utilizador que tenha um veículo atribuído (pid != -1)
    // e que ainda não esteja terminado.
    Servico *s = NULL;
    for (int i = 0; i < n_servicos; i++)
    {
        // Verifica se o serviço pertence ao user E se tem veículo lançado
        if (strcmp(servicos[i].user, p->user) == 0 &&
            servicos[i].pid_veiculo != -1 &&
            servicos[i].estado != S_TERMINADO)
        {
            s = &servicos[i];
            break; // Encontrou
        }
    }

    if (s != NULL)
    {
        // === AQUI ESTÁ A MAGIA ===
        // Envia o sinal SIGUSR1 ao processo veículo
        if (kill(s->pid_veiculo, SIGUSR1) == 0)
        {
            r.ok = 1;
            r.id_servico = s->id;
            snprintf(r.msg, sizeof(r.msg), "Entrou no veiculo! Viagem vai iniciar.");
            printf("[CTRL] Sinal SIGUSR1 enviado ao veiculo PID=%d (User=%s)\n",
                   s->pid_veiculo, p->user);
        }
        else
        {
            perror("[CTRL] Falha ao enviar sinal ao veiculo");
            snprintf(r.msg, sizeof(r.msg), "Erro ao contactar veiculo.");
        }
    }

    pthread_mutex_unlock(&mux_servicos);

    // Enviar resposta ao cliente
    int fd_cli = open(p->fifo_resp, O_WRONLY);
    if (fd_cli != -1)
    {
        write(fd_cli, &r, sizeof(r));
        close(fd_cli);
    }
}

// ----------------------------------------------------------------------
//  Trata pedido AGENDAR vindo do cliente
// ----------------------------------------------------------------------
static void tratar_pedido_agendar(const Pedido *p)
{
    Resposta r;

    if (em_encerramento)
    {
        r.ok = 0;
        r.id_servico = -1;
        snprintf(r.msg, sizeof(r.msg), "Sistema a encerrar. Novos pedidos rejeitados.");

        // Avisar o cliente
        int fd_cli = open(p->fifo_resp, O_WRONLY);
        if (fd_cli != -1)
        {
            write(fd_cli, &r, sizeof(r));
            close(fd_cli);
        }
        return; // Sai da função imediatamente
    }

    pthread_mutex_lock(&mux_servicos);

    if (n_servicos >= MAX_SERVICOS)
    {
        pthread_mutex_unlock(&mux_servicos);
        r.ok = 0;
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

        s->dist_km = p->dist;
        s->estado = S_AGENDADO;
        s->pid_veiculo = -1;
        s->perc_concl = 0;
        s->na_fila = 0; // flag seta ou nao na fila de espera

        n_servicos++;

        pthread_mutex_unlock(&mux_servicos);

        r.ok = 1;
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
// Trata o pedido "cancelar <id>" vindo do Cliente
// ----------------------------------------------------------------------
static void tratar_pedido_cancelar_user(const Pedido *p)
{
    // O ID vem no campo p->dist (conforme definido no cliente)
    int id_alvo = p->dist;

    pthread_mutex_lock(&mux_servicos);
    if (id_alvo == 0)
    {
        // Cancelar TODOS deste user (Opcional do enunciado)
        // Podes implementar um loop aqui se quiseres
    }
    else if (id_alvo > 0 && id_alvo <= n_servicos)
    {
        Servico *s = &servicos[id_alvo - 1];

        // VERIFICAÇÃO DE SEGURANÇA: O serviço pertence a este user?
        if (strcmp(s->user, p->user) == 0)
        {
            // Logica de cancelamento (semelhante ao admin)
            if (s->estado == S_AGENDADO)
            {
                s->estado = S_CANCELADO;
                s->na_fila = 0;
                printf("Servico %d cancelado (ainda nao tinha iniciado).\n", id_alvo);  // ← MUDAR 'id' para 'id_alvo'
                notificar_cliente(s, "O seu agendamento foi cancelado.");
            }
            else if (s->estado == S_EM_CURSO)
            {
                // Se já tem carro, parar o carro
                if (s->pid_veiculo != -1)
                    kill(s->pid_veiculo, SIGINT);
                s->estado = S_CANCELADO;
                notificar_cliente(s, "Viagem interrompida e cancelada.");
            }
            else
            {
                notificar_cliente(s, "Nao foi possivel cancelar (ja terminou/cancelado).");
            }
        }
        else
        {
            // Tentar cancelar serviço de outro user
            printf("[CTRL] User %s tentou cancelar servico %d que nao lhe pertence.\n", p->user, id_alvo);
        }
    }
    pthread_mutex_unlock(&mux_servicos);
}

// ----------------------------------------------------------------------
// Função que trata o pedido "consultar"
// ----------------------------------------------------------------------
static void tratar_pedido_consultar(const Pedido *p)
{
    pthread_mutex_lock(&mux_servicos);

    int encontrou = 0;
    char fifo_dest[128];
    snprintf(fifo_dest, sizeof(fifo_dest), "%s", p->fifo_resp);

    int fd = open(fifo_dest, O_WRONLY | O_NONBLOCK);
    if (fd == -1)
    {
        pthread_mutex_unlock(&mux_servicos);
        return;
    }

    for (int i = 0; i < n_servicos; i++)
    {
        // Mostra apenas os serviços deste utilizador
        if (strcmp(servicos[i].user, p->user) == 0)
        {
            Resposta r;
            r.ok = 1;
            r.id_servico = servicos[i].id;

            // Formata estado
            char estado_str[20];
            if (servicos[i].estado == S_AGENDADO)
                strcpy(estado_str, "AGENDADO");
            else if (servicos[i].estado == S_EM_CURSO)
                strcpy(estado_str, "EM VIAGEM");
            else if (servicos[i].estado == S_TERMINADO)
                strcpy(estado_str, "TERMINADO");
            else
                strcpy(estado_str, "CANCELADO");

            snprintf(r.msg, sizeof(r.msg), "Servico %d [%s]: %s (%d km)",
                     servicos[i].id, estado_str, servicos[i].local, servicos[i].dist_km);

            write(fd, &r, sizeof(r));
            encontrou++;
        }
    }

    if (encontrou == 0)
    {
        Resposta r;
        r.ok = 1;
        r.id_servico = 0;
        strcpy(r.msg, "Nenhum historico de servicos.");
        write(fd, &r, sizeof(r));
    }
    else
    {
        // Opcional: Mandar mensagem de fim de lista
        Resposta r;
        r.ok = 1;
        r.id_servico = 0;
        strcpy(r.msg, "--- Fim da lista ---");
        write(fd, &r, sizeof(r));
    }

    close(fd);
    pthread_mutex_unlock(&mux_servicos);
}
// ----------------------------------------------------------------------
// Função auxiliar para notificar o cliente
// ----------------------------------------------------------------------
static void notificar_cliente(const Servico *s, const char *texto)
{
    char fifo_cli[128];
    // Reconstrói o nome do FIFO do cliente: ./tmp/taxi_cli_username
    snprintf(fifo_cli, sizeof(fifo_cli), "%s/taxi_cli_%s", FIFO_DIR, s->user);

    // Abre em modo NONBLOCK para o controlador não bloquear se o cliente não estiver lá
    int fd = open(fifo_cli, O_WRONLY | O_NONBLOCK);
    if (fd != -1)
    {
        Resposta r;
        memset(&r, 0, sizeof(r));
        r.ok = 1;
        r.id_servico = s->id;
        snprintf(r.msg, sizeof(r.msg), "%s", texto);

        write(fd, &r, sizeof(r));
        close(fd);
    }
}

// ----------------------------------------------------------------------
//  Thread que lê telemetria do veiculo
// ----------------------------------------------------------------------
static void *thread_telemetria(void *arg)
{
    int idx = (int)(intptr_t)arg; // índice em veiculos[]
    int fd = veiculos[idx].fd_telemetria;
    int id_servico = veiculos[idx].id_servico;

    char buf[256];
    ssize_t n;
    // MONITORIZAÇÃO DEDICADA
    // Cada veículo tem uma thread exclusiva para ler o seu pipe.
    // Usamos strtok_r para garantir que processamos mensagens completas,
    // mesmo que cheguem "coladas" (ex: PERC 100\nFIM OK\n).
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        // === Processar linha a linha ===
        // Se o buffer tiver "PERC 100\nFIM OK\n", o strtok parte em dois
        char *saveptr;
        char *token = strtok_r(buf, "\n", &saveptr);

        while (token != NULL)
        {
            processa_mensagem_veiculo(id_servico, token);
            token = strtok_r(NULL, "\n", &saveptr);
        }
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
    (void)arg;

    while (running_ctrl)
    {
        sleep(1);
        tempo_simulado++;

        pthread_mutex_lock(&mux_servicos);

        // 1️ ADICIONAR SERVIÇOS À FILA quando a hora chega
        for (int i = 0; i < n_servicos; i++)
        {
            Servico *s = &servicos[i];

            if (s->estado == S_AGENDADO &&
                s->hora <= tempo_simulado &&
                s->na_fila == 0)  // ainda não na fila
            {
                // Se houver slot livre, lançar já; caso contrário, entrar na fila
                if (encontra_slot_livre_veiculo() >= 0)
                {
                    printf("[RELOGIO] hora=%d, lançar servico id=%d (slot disponível)\n",
                           tempo_simulado, s->id);
                    spawn_veiculo_para_servico(s);
                }
                else
                {
                    // Entrar na fila de espera (só se não houver slot)
                    if (fila_count < MAX_FILA_ESPERA)
                    {
                        // ← CORREÇÃO: usar buffer circular
                        int pos = (fila_head + fila_count) % MAX_FILA_ESPERA;
                        fila_espera[pos].id_servico = s->id;
                        fila_espera[pos].tempo_agendado = s->hora;
                        fila_count++;
                        s->na_fila = 1;
                        printf("[RELOGIO] Sem slots: Servico %d entrou na fila de espera (posicao: %d)\n",
                               s->id, fila_count);
                    }
                    else
                    {
                        // fila cheia: log e manter S_AGENDADO
                        printf("[RELOGIO] Fila de espera cheia. Servico %d nao pode ser enfileirado.\n", s->id);
                    }
                }
            }
        }

        // 2️ PROCESSAR FILA (LIFO - Last In, First Out)
        while (fila_count > 0 && encontra_slot_livre_veiculo() >= 0)
        {
            // Índice do último elemento inserido (tail)
            int tail = fila_head + fila_count - 1;
            if (tail >= MAX_FILA_ESPERA)
                tail -= MAX_FILA_ESPERA;

            // Em thread_relogio(), antes de spawn - PROTEÇÃO
            int id_servico = fila_espera[tail].id_servico;
            if (id_servico < 0 || id_servico > n_servicos)
                continue; // entrada inválida

            Servico *s = &servicos[id_servico - 1];
            if (s->estado == S_CANCELADO)
            {
                printf("[RELOGIO] Servico %d ja cancelado. Removendo da fila.\n", id_servico);
                fila_count--;
                continue;
            }

            printf("[RELOGIO] Processando (LIFO) servico %d da fila\n", id_servico);

            // Marca que sai da fila antes de spawn (evita duplicados)
            s->na_fila = 0;

            spawn_veiculo_para_servico(s);

            // Remover do topo da pilha (LIFO)
            fila_count--;
            if (fila_count == 0)
                fila_head = 0; // reset para simplificar
            // fila_head inalterado para LIFO
        }

        pthread_mutex_unlock(&mux_servicos);
    }

    return NULL;
}

// ----------------------------------------------------------------------
// Loop principal de pedidos: lê estruturas Pedido do FIFO_PEDIDOS
// ----------------------------------------------------------------------
static void *thread_clientes(void *arg) // Mudámos o nome para ser mais claro
{
    (void)arg;
    loop_pedidos(); // Chama a tua função original (ou move o código para aqui)
    return NULL;
}

// ----------------------------------------------------------------------
//  Regista um novo utilizador na lista global 'users'
// ----------------------------------------------------------------------
static void registar_utilizador(const Pedido *p)
{
    pthread_mutex_lock(&mux_servicos); // Usamos o mesmo mutex para proteger dados globais

    // Verificar se já existe
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (users[i].ativo && strcmp(users[i].nome, p->user) == 0)
        {
            // Já existe, atualiza PID e sai
            users[i].pid_cliente = p->pid_cliente;

            pthread_mutex_unlock(&mux_servicos);
            return;
        }
    }

    // Adicionar novo
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (!users[i].ativo)
        {
            users[i].ativo = 1;

            snprintf(users[i].nome, sizeof(users[i].nome), "%s", p->user);
            snprintf(users[i].fifo_cli, sizeof(users[i].fifo_cli), "%s", p->fifo_resp);

            users[i].pid_cliente = p->pid_cliente;
            users[i].estado = U_AGUARDA;
            n_users++;
            printf("[CTRL] Novo utilizador registado: %s (Total: %d)\n", p->user, n_users);
            break;
        }
    }
    pthread_mutex_unlock(&mux_servicos);
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

    while (running_ctrl)
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

            registar_utilizador(&p);

            // opcional: responder com uma mensagem simples
            Resposta r;
            r.ok = 1;
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
        else if (p.tipo == CMD_ENTRAR)
        {
            printf("[CTRL] Recebido pedido ENTRAR de %s\n", p.user);
            tratar_pedido_entrar(&p);
        }
        else if (p.tipo == CMD_CANCELAR)
        {
            tratar_pedido_cancelar_user(&p);
        }
        else if (p.tipo == CMD_CONSULTAR)
        {
            tratar_pedido_consultar(&p);
        }
        else if (p.tipo == CMD_SAIR)
        {
            tratar_pedido_sair(&p);
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

// ----------------------------------------------------------------------
//  Mostrar lista de serviços agendados
// ----------------------------------------------------------------------
static void cmd_listar(void)
{
    pthread_mutex_lock(&mux_servicos);
    printf("=== SERVICOS AGENDADOS ===\n");
    for (int i = 0; i < n_servicos; i++)
    {
        printf("ID: %d | User: %s | Estado: %d | Hora: %d | Local: %s\n",
               servicos[i].id, servicos[i].user, servicos[i].estado,
               servicos[i].hora, servicos[i].local);
    }
    pthread_mutex_unlock(&mux_servicos);
}

// ----------------------------------------------------------------------
//  Mostrar estado da frota (veículos em execução)
// ----------------------------------------------------------------------
static void cmd_frota(void)
{
    pthread_mutex_lock(&mux_servicos);
    printf("=== ESTADO DA FROTA ===\n");
    int ativos = 0;
    for (int i = 0; i < n_servicos; i++)
    {
        if (servicos[i].estado == S_EM_CURSO)
        {
            printf("Veiculo Servico %d (%s) -> %d%% concluido\n",
                   servicos[i].id, servicos[i].user, servicos[i].perc_concl);
            ativos++;
        }
    }
    if (ativos == 0)
        printf("Nenhum veiculo em movimento.\n");
    pthread_mutex_unlock(&mux_servicos);
}

// ----------------------------------------------------------------------
//  Mostrar lista de utilizadores registados
// ----------------------------------------------------------------------
static void cmd_utiliz(void)
{
    pthread_mutex_lock(&mux_servicos);
    printf("=== UTILIZADORES ONLINE (%d) ===\n", n_users);
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (users[i].ativo)
        {
            printf("- User: %s | PID: %d | FIFO: %s\n",
                   users[i].nome, users[i].pid_cliente, users[i].fifo_cli);
        }
    }
    pthread_mutex_unlock(&mux_servicos);
}

// ----------------------------------------------------------------------
//  Cancelar um serviço pelo ID
// ----------------------------------------------------------------------
static void cmd_cancelar(int id)
{
    pthread_mutex_lock(&mux_servicos);

    if (id <= 0 || id > n_servicos)
    {
        printf("ID de servico invalido.\n");
        pthread_mutex_unlock(&mux_servicos);
        return;
    }

    Servico *s = &servicos[id - 1];

    if (s->estado == S_TERMINADO || s->estado == S_CANCELADO)
    {
        printf("O servico %d ja se encontra terminado/cancelado.\n", id);
    }
    else if (s->estado == S_AGENDADO)
    {
        // Se ainda não começou, basta mudar o estado
        s->estado = S_CANCELADO;
        s->na_fila = 0;  // ← ADICIONAR
        printf("Servico %d cancelado (ainda nao tinha iniciado).\n", id);
        notificar_cliente(s, "O seu agendamento foi cancelado pelo administrador.");
    }
    else if (s->estado == S_EM_CURSO)
    {
        // Se ja esta a andar, temos de parar o veiculo!
        if (s->pid_veiculo != -1)
        {
            // Envia SIGINT ao processo veiculo para ele terminar
            kill(s->pid_veiculo, SIGINT);
            printf("Sinal de cancelamento enviado ao veiculo PID=%d.\n", s->pid_veiculo);
        }
        s->estado = S_CANCELADO;
        notificar_cliente(s, "A sua viagem foi cancelada pelo administrador.");
    }

    pthread_mutex_unlock(&mux_servicos);
}

// ----------------------------------------------------------------------
//  Procedimento de encerramento do sistema
// ----------------------------------------------------------------------
static void cmd_terminar(void)
{
    printf("[CTRL] === Inicio do Procedimento de Encerramento ===\n");

    // 1. Bloquear novos agendamentos
    em_encerramento = 1;
    printf("[CTRL] Novos pedidos serao rejeitados.\n");

    // 2. Loop de Espera (Drenagem)
    while (1)
    {
        int ativos = conta_servicos_ativos();
        if (ativos == 0)
        {
            break; // Não há mais táxis na estrada
        }

        printf("[CTRL] A aguardar conclusao de %d servicos ativos...\n", ativos);
        sleep(2); // Verifica a cada 2 segundos
    }

    // 3. Notificação final (Opcional) e alteração da flag principal
    printf("[CTRL] Todos os servicos terminaram. A desligar sistema.\n");

    // Isto vai fazer o while(running_ctrl) do main parar
    running_ctrl = 0;

    /*   // 3. Mandar os Clientes embora (Opcional )
      pthread_mutex_lock(&mux_servicos);
      for (int i = 0; i < n_servicos; i++) {
           // Tenta enviar uma ultima mensagem a dizer "ADEUS"
           notificar_cliente(&servicos[i], "O sistema encerrou. Ate logo!");
      }
      pthread_mutex_unlock(&mux_servicos);     */
}

// ----------------------------------------------------------------------
//  Conta serviços ativos (agendados ou em execução)
// ----------------------------------------------------------------------
static int conta_servicos_ativos(void)
{
    int count = 0;
    pthread_mutex_lock(&mux_servicos);
    for (int i = 0; i < n_servicos; i++)
    {
        if (servicos[i].estado == S_AGENDADO || servicos[i].estado == S_EM_CURSO)
        {
            count++;
        }
    }
    pthread_mutex_unlock(&mux_servicos);
    return count;
}

// ----------------------------------------------------------------------
//  Manipulador de sinal SIGINT/SIGHUP/SIGPIPE
// ----------------------------------------------------------------------
void handle_sigint(int sig)
{
    (void)sig;
    running_ctrl = 0;
}

// Tratar pedido CMD_SAIR
static void tratar_pedido_sair(Pedido *p)
{
    pthread_mutex_lock(&mux_servicos);

    // Procurar se o utilizador tem um serviço ativo (EM_CURSO)
    int tem_servico_ativo = 0;
    int id_servico_ativo = -1;
    
    for (int i = 0; i < n_servicos; i++)
    {
        Servico *s = &servicos[i];
        if (strcmp(s->user, p->user) == 0 && s->estado == S_EM_CURSO)
        {
            tem_servico_ativo = 1;
            id_servico_ativo = s->id;
            break;
        }
    }

    pthread_mutex_unlock(&mux_servicos);

    // Enviar resposta via FIFO do cliente
    int fd_resp = open(p->fifo_resp, O_WRONLY);
    if (fd_resp == -1)
    {
        printf("[CTRL] Erro ao abrir FIFO de resposta para %s\n", p->user);
        return;
    }

    Resposta r;
    memset(&r, 0, sizeof(r));

    if (tem_servico_ativo)
    {
        r.ok = 0;  //  Não pode sair
        snprintf(r.msg, sizeof(r.msg),
                 "[BLOQUEADO] Nao podes sair! Tens a viagem %d em curso. Aguarda a finalizacao.",
                 id_servico_ativo);
        printf("[CTRL] Cliente %s tem viagem ativa (ID=%d). Saida bloqueada.\n",
               p->user, id_servico_ativo);
    }
    else
    {
        r.ok = 1;  //  Pode sair
        snprintf(r.msg, sizeof(r.msg),
                 "[OK] Podes sair com seguranca.");
        
        // Marcar utilizador como inativo
        pthread_mutex_lock(&mux_servicos);
        for (int i = 0; i < n_users; i++)
        {
            if (strcmp(users[i].nome, p->user) == 0)
            {
                users[i].ativo = 0;
                printf("[CTRL] Utilizador %s marcado como inativo.\n", p->user);
                break;
            }
        }
        pthread_mutex_unlock(&mux_servicos);
    }

    write(fd_resp, &r, sizeof(r));
    close(fd_resp);

    printf("[CTRL] Resposta CMD_SAIR enviada para %s (ok=%d)\n", p->user, r.ok);
}
