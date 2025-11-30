#define _GNU_SOURCE // para obter definições de GNU extensions (strtok_r, etc)
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>

#include "taxis_protocol.h"
#include "frota.h"

#define FIFO_DIR "./tmp"
#define FIFO_PEDIDOS FIFO_DIR "/taxi_pedidos"

extern int tempo_simulado;              // tempo simulado atual
volatile sig_atomic_t running_ctrl = 1; // Controlador em execução

// Prototipos internos
// Visibilidade no Ligador (Linker).
// Usamos 'static' para limitar o escopo ao ficheiro atual (controlador.c)
// Evita conflitos de nomes com outras partes do programa.
static void processa_mensagem_veiculo(int id_servico, const char *msg); // processa msg veiculo
static void spawn_veiculo_para_servico(Servico *s);                     // lança veiculo
static int encontra_slot_livre_veiculo(void);                           // encontra slot livre em veiculos[]
static int notificar_cliente(const Servico *s, const char *texto);      // notifica cliente
static void registar_utilizador(const Pedido *p);                       // regista novo utilizador
static void tratar_pedido_agendar(const Pedido *p);                     // trata pedido AGENDAR
static void tratar_pedido_entrar(const Pedido *p);                      // trata pedido ENTRAR
static void tratar_pedido_cancelar_user(const Pedido *p);               // trata pedido CANCELAR
static void tratar_pedido_consultar(const Pedido *p);                   // trata pedido CONSULTAR
static void *thread_veiculos(void *arg);                                // Thread que lê telemetria
static void *thread_clientes(void *arg);                                // Thread de atendimento a clientes
static void *thread_relogio(void *arg);                                 // Thread do relógio
static void cmd_listar(void);                                           // Procedimento listar
static void cmd_frota(void);                                            // Procedimento frota
static void cmd_utiliz(void);                                           // Procedimento utiliz
static void cmd_cancelar(int id_servico);                               // Procedimento cancelar
static void cmd_terminar(void);                                         // Procedimento de encerramento
static void cmd_final(void);                                            // Procedimento finalização
static void handle_signal(int sig, siginfo_t *info, void *context);     // Handler para sinal SIGINT (Ctrl+C)

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

    // 1. Configuração inicial (diretórios, sinais, FIFOs)
    if (mkdir(FIFO_DIR, 0777) == -1 && errno != EEXIST)
    {
        perror("[CTRL] erro a criar diretório FIFO_DIR");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handle_signal;
    // SA_RESTART impede que o fgets falhe quando recebes um sinal
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGINT, &sa, NULL);  // Ctrl+C
    sigaction(SIGHUP, &sa, NULL);  // Hangup do remote
    sigaction(SIGPIPE, &sa, NULL); // Ignora erros de escrita em pipes quebrados

    if (access(FIFO_PEDIDOS, F_OK) == 0)
    {
        fprintf(stderr, "\n[ERRO CRÍTICO] O Controlador já parece estar em execução!\n");
        fprintf(stderr, "Motivo: O ficheiro '%s' já existe.\n", FIFO_PEDIDOS);
        fprintf(stderr, "Solução:\n");
        fprintf(stderr, " 1. Se tens outro terminal aberto com o controlador, fecha-o.\n");
        fprintf(stderr, " 2. Se o controlador anterior crashou, apaga o ficheiro manualmente:\n");
        fprintf(stderr, "    rm %s\n\n", FIFO_PEDIDOS);
        return 1;
    }

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
    system("clear");
    // printf("[CTRL] Sistema pronto. Aceitar comandos de admin (listar (l), frota (f), utiliz (u), km, hora (h), cancelar <id>, terminar)\n");
    printf("\n\033[1;32m[OK] SISTEMA INICIADO E PRONTO.\033[0m\n\n");

    printf("\033[1m--- Gestão ---\033[0m\n");
    printf("  %-15s %s\n", "listar (l)", "-> Ver agendamentos");
    printf("  %-15s %s\n", "frota (f)", "-> Estado dos veículos");
    printf("  %-15s %s\n", "utiliz (u)", "-> Utilizadores online");

    printf("\n\033[1m--- Estatísticas ---\033[0m\n");
    printf("  %-15s %s\n", "km", "-> Total KMs percorridos");
    printf("  %-15s %s\n", "hora (h)", "-> Relógio do sistema");

    printf("\n\033[1m--- Controlo ---\033[0m\n");
    printf("  %-15s %s\n", "cancelar <id>", "-> Anular serviço");
    printf("  %-15s %s\n", "terminar", "-> Desligar servidor");

    fflush(stdout);
    // 3. Loop Principal (Administrador - STDIN)
    char linha[256];
    while (running_ctrl)
    {
        printf("admin> ");
        fflush(stdout); // Opcional: Prompt

        if (!fgets(linha, sizeof(linha), stdin))
        {
            // Se o fgets falhou porque foi interrompido por um sinal (EINTR)
            if (errno == EINTR)
            {
                // Se running_ctrl ainda é 1, foi o 1º toque (Aviso).
                // Limpamos o erro e continuamos o loop para pedir input outra vez.
                if (running_ctrl)
                {
                    clearerr(stdin); // Limpa a flag de erro do stdin
                    continue;
                }
            }
            // Se chegou aqui, é para sair mesmo (2º toque ou EOF real)
            break;
        }
        linha[strcspn(linha, "\n")] = '\0';

        if (strcmp(linha, "listar") == 0 || strcmp(linha, "l") == 0)
        {
            cmd_listar();
        }
        else if (strcmp(linha, "frota") == 0 || strcmp(linha, "f") == 0)
        {
            cmd_frota();
        }
        else if (strcmp(linha, "utiliz") == 0 || strcmp(linha, "u") == 0)
        {
            cmd_utiliz();
        }
        else if (strcmp(linha, "terminar") == 0 || strcmp(linha, "sair") == 0)
        {
            cmd_terminar();
        }
        else if (strcmp(linha, "km") == 0)
        {
            printf("Total acumulado da frota: %d km\n", km_total);
        }
        else if (strcmp(linha, "hora") == 0 || strcmp(linha, "h") == 0)
        {
            printf("Tempo simulado: %d ticks (hora %2dh)\n", tempo_simulado, tempo_simulado % 24);
        }
        else if (strcmp(linha, "ajuda") == 0 || strcmp(linha, "?") == 0 || strcmp(linha, "help") == 0)
        {
            printf("+------------+------------+------------+----------------+\n");
            printf("| GESTÃO     | MONITORIZA | ESTATISTICA| SISTEMA        |\n");
            printf("+------------+------------+------------+----------------+\n");
            printf("| listar (l) | frota (f)  | km         | cancelar <id>  |\n");
            printf("| utiliz (u) |            | hora (h)   | help (?)       |\n");
            printf("|            |            |            | terminar/sair  |\n");
            printf("+------------+------------+------------+----------------+\n");
        }
        else if (strncmp(linha, "cancelar", 8) == 0)
        {
            int id_cancelar = 0;
            // Tenta ler "cancelar 1", "cancelar 2", etc.
            if (sscanf(linha, "cancelar %d", &id_cancelar) == 1)
            {
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

    // === QUANDO SAI DO LOOP ===
    // Isto executa quer tenha sido 'terminar', quer tenha sido 'Ctrl+C' (2x)
    cmd_final();

    return 0;
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

    // Lógica Genérica: Se não conseguirmos avisar o cliente, ABORTAMOS TUDO.
    int cliente_vivo = 1;

    // 1. CHEGADA (Veículo -> Controlador -> Cliente)
    // 1. O veículo avisa que chegou
    if (strncmp(msg, "VEICULO", 7) == 0)
    {
        // Vamos avisar o cliente!
        printf("[CTRL] Veículo chegou ao cliente %s (Serviço %d)\n", s->user, s->id);
        cliente_vivo = notificar_cliente(s, "O seu táxi chegou! Escreva 'entrar'.");
    }
    // 2. Início da viagem (quando recebe SIGUSR1 e arranca)
    else if (strncmp(msg, "INICIO", 6) == 0)
    {
        s->estado = S_EM_CURSO;
        s->perc_concl = 0;
        printf("[CTRL] Servico %d iniciou viagem.\n", s->id);
        // Avisar cliente que arrancou
        cliente_vivo = notificar_cliente(s, "Viagem iniciada!");
    }
    // 3. PROGRESSO (PERC)
    else if (strncmp(msg, "PERC ", 5) == 0)
    {
        int perc = atoi(msg + 5);
        s->perc_concl = perc;
        // Criar mensagem para o cliente: "Viagem: 10%..."
        char msg_cli[64];
        snprintf(msg_cli, sizeof(msg_cli), "Viagem em curso: %d%%", perc);
        cliente_vivo = notificar_cliente(s, msg_cli);
    }
    // 4. Fim da viagem
    else if (strncmp(msg, "FIM", 3) == 0)
    {
        s->estado = S_TERMINADO;
        s->perc_concl = 100;
        // === CONTABILIZAR QUILÓMETROS ===
        km_total += s->dist_km; // <--- AQUI está a resposta aos KMs

        printf("[CTRL] Serviço %d concluído. Total frota: %d Km.\n", s->id, km_total);
        // Avisar cliente que a viagem terminou
        char msg_cli[128];
        snprintf(msg_cli, sizeof(msg_cli), "Viagem terminada! Total: %d km. Obrigado.", s->dist_km);
        notificar_cliente(s, msg_cli);
        // No fim não interessa se o cliente está vivo ou não, o serviço acabou.
    }
    // !=== CLIENTE DESAPARECEU === //
    // *Se o cliente não estiver a receber mensagens, cancelamos o serviço
    // *(pode ter fechado o cliente ou perdido a ligação)
    if (cliente_vivo == 0 && s->estado != S_TERMINADO && s->estado != S_CANCELADO)
    {
        printf("[CTRL] ALERTA: Cliente %s desapareceu! A cancelar servico %d.\n", s->user, s->id);

        // 1. Mandar parar o veículo imediatamente
        if (s->pid_veiculo != -1)
        {

            union sigval val;
            val.sival_int = 1; // 1 = Ordem de Cancelamento
            sigqueue(s->pid_veiculo, SIGINT, val);
        }
        // 2. Marcar como terminado/cancelado para libertar o sistema
        s->estado = S_CANCELADO;

        // 3. === NOVO: REMOVER UTILIZADOR DA LISTA ===
        // Como já temos o mutex trancado, podemos mexer na lista users diretamente
        for (int i = 0; i < MAX_USERS; i++)
        {
            if (users[i].ativo && strcmp(users[i].nome, s->user) == 0)
            {
                users[i].ativo = 0; // Liberta o slot
                printf("[CTRL] Utilizador %s removido da lista de ativos.\n", s->user);
                break;
            }
        }
    }
    pthread_mutex_unlock(&mux_servicos);
}

// ----------------------------------------------------------------------
//  Lança um processo veiculo e liga-o a um pipe de telemetria
// ----------------------------------------------------------------------
static void spawn_veiculo_para_servico(Servico *s)
{
    // 1. Garantir que há lugar ANTES de gastar recursos (Pipe/Fork)
    int id = encontra_slot_livre_veiculo();
    if (id < 0)
    {
        printf("[CTRL] Erro critico: Tentativa de spawn sem slot livre!\n");
        return;
    }

    int p[2];
    // close(p[0]);
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

        // Copia todas as configurações (variáveis de ambiente) que o Pai (Controlador) tem e entrega-as ao Filho (Veículo).
        // A função execve é a chamada ao sistema "pura" e não faz nada automaticamente em termos de ambiente.
        // Se usássemos execv, o ambiente seria herdado automaticamente.
        //* (Página 1, ponto 9)
        //*"No que respeita à manipulação de recursos do sistema, deve ser dada prioridade
        //* ao uso de chamadas ao sistema operativo face ao uso de funções biblioteca".
        //! usa um vector (argv) para passar os argumentos
        execv("./veiculo", argv);

        // Se chegar aqui, deu erro
        perror("[FILHO] erro no execve");
        _exit(1);
    }

    // PAI (controlador)
    close(p[1]); // só vamos ler

    veiculos[id].ativo = 1;
    veiculos[id].pid = pid;
    veiculos[id].fd_telemetria = p[0];
    veiculos[id].id_servico = s->id;

    s->pid_veiculo = pid;
    s->estado = S_EM_CURSO;
    s->perc_concl = 0;

    pthread_t th;
    pthread_create(&th, NULL, thread_veiculos, (void *)(intptr_t)id);
    // =================================================================================
    // DECISÃO DE IMPLEMENTAÇÃO: Gestão de Threads (Detach vs Join)
    // ---------------------------------------------------------------------------------
    // Utilizamos pthread_detach() em vez de pthread_join() por três motivos:
    //
    // 1. NÃO BLOQUEANTE: O 'join' bloquearia o Controlador até a viagem terminar
    //    (o que pode demorar vários segundos). O Controlador precisa de continuar
    //    livre para atualizar o relógio e atender outros clientes imediatamente.
    //
    // 2. CONCORRÊNCIA: Permite que a thread de telemetria corra em paralelo
    //    ("fire-and-forget") enquanto o processo principal gere outros eventos.
    //
    // 3. LIMPEZA AUTOMÁTICA: Informa o SO que deve libertar os recursos da thread
    //    automaticamente assim que ela terminar, sem intervenção do pai.
    // =================================================================================
    pthread_detach(th); // Não precisamos de fazer pthread_join mais tarde

    printf("[CTRL] veiculo para servico id=%d lançado (idx=%d)\n", s->id, id);
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
// Função auxiliar para notificar o cliente
// ----------------------------------------------------------------------
static int notificar_cliente(const Servico *s, const char *texto)
{
    char fifo_cli[128];
    // Reconstrói o nome do FIFO do cliente: ./tmp/taxi_cli_username
    snprintf(fifo_cli, sizeof(fifo_cli), "%s/taxi_cli_%s", FIFO_DIR, s->user);

    // Abre em modo NONBLOCK para o controlador não bloquear se o cliente não estiver lá
    // Se o open falhar com ENXIO, significa que ninguém está a ler do outro lado
    int fd = open(fifo_cli, O_RDWR);
    if (fd == -1)
        return 0; // Cliente não está a ouvir

    Resposta r;
    // Limpa a estrutura para evitar lixo de memória
    memset(&r, 0, sizeof(r));
    r.ok = 1;
    r.id_servico = s->id;
    snprintf(r.msg, sizeof(r.msg), "%s", texto);

    // Se o write falhar (ex: pipe quebrou a meio), também detetamos
    if (write(fd, &r, sizeof(r)) == -1)
    {
        perror("[CTRL] Erro ao escrever para cliente (pipe quebrado)");
        close(fd);
        return 0;
    }

    close(fd);
    return 1;
}

// ----------------------------------------------------------------------
//  Regista um novo utilizador ou atualiza um existente
// ----------------------------------------------------------------------
static void registar_utilizador(const Pedido *p)
{
    pthread_mutex_lock(&mux_servicos); // Usamos o mesmo mutex para proteger dados globais

    // 1. PROCURAR EXISTENTE
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (users[i].ativo && strcmp(users[i].nome, p->user) == 0)
        {
            // Atualizamos os dados de conexão
            users[i].pid_cliente = p->pid_cliente;
            snprintf(users[i].fifo_cli, sizeof(users[i].fifo_cli), "%s", p->fifo_resp);
            printf("[CTRL] Utilizador %s RECONECTADO (PID atualizado: %d)\n",
                   users[i].nome, users[i].pid_cliente);

            pthread_mutex_unlock(&mux_servicos);
            return;
        }
    }

    // 2. SE NÃO EXISTE, CRIAR NOVO
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
            servicos[i].estado != S_TERMINADO &&
            servicos[i].estado != S_CANCELADO)
        {
            s = &servicos[i];
            break; // Encontrou
        }
    }

    if (s != NULL)
    {
        // === AQUI ESTÁ A MAGIA ===
        // Envia o sinal SIGUSR1 ao processo veículo
        union sigval val;
        val.sival_int = 0;
        if (sigqueue(s->pid_veiculo, SIGUSR1, val) == 0)
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

    // 1. VALIDAÇÃO DO ID
    /*    if (id_alvo <= 0 || id_alvo > n_servicos)
       {
           // Enviar erro ao cliente
           int fd = open(p->fifo_resp, O_WRONLY | O_NONBLOCK);
           if (fd != -1)
           {
               Resposta r;
               r.ok = 0;
               r.id_servico = id_alvo;
               snprintf(r.msg, sizeof(r.msg), "Erro: Servico %d nao encontrado.", id_alvo);
               write(fd, &r, sizeof(r));
               close(fd);
           }
           pthread_mutex_unlock(&mux_servicos);
           return;
       } */
    // 2. CANCELAR TUDO (id_alvo == 0)
    if (id_alvo == 0)
    {
        printf("[CTRL] Recebido pedido 'CANCELAR TUDO' do user %s\n", p->user);
        int cancelados = 0;

        for (int i = 0; i < n_servicos; i++)
        {
            Servico *s = &servicos[i];

            // Verifica se é do user E se está ativo
            if (strcmp(s->user, p->user) == 0 &&
                (s->estado == S_AGENDADO || s->estado == S_EM_CURSO))
            {
                // Parar veículo se estiver a andar
                if (s->estado == S_EM_CURSO && s->pid_veiculo != -1)
                {
                    union sigval val;
                    val.sival_int = 1;
                    sigqueue(s->pid_veiculo, SIGINT, val);
                }

                s->estado = S_CANCELADO;
                cancelados++;
            }
        }
        printf("[CTRL] Total de servicos cancelados para %s: %d\n", p->user, cancelados);
    }
    // 3. CANCELAR SERVIÇO ESPECÍFICO
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
                notificar_cliente(s, "Servico cancelado com sucesso.");
            }
            else if (s->estado == S_EM_CURSO)
            {
                // Se já tem carro, parar o carro
                if (s->pid_veiculo != -1)
                {
                    // kill(s->pid_veiculo, SIGINT);
                    union sigval val;
                    val.sival_int = 1; // 1 = Ordem de Cancelamento
                    sigqueue(s->pid_veiculo, SIGINT, val);
                }
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
            // === O UTILIZADOR NÃO É O DONO (Tentativa inválida) ===
            printf("[CTRL] User %s tentou cancelar servico %d (pertence a %s).\n",
                   p->user, id_alvo, s->user);

            // Enviar mensagem técnica genérica ao atacante/cliente enganado
            // Usamos p->fifo_resp porque queremos responder a QUEM PEDIU,
            // e não ao dono do serviço (s->user).
            int fd = open(p->fifo_resp, O_WRONLY);
            if (fd != -1)
            {
                Resposta r;
                r.ok = 0;
                r.id_servico = id_alvo;
                // Mensagem técnica que não revela detalhes
                snprintf(r.msg, sizeof(r.msg), "Erro: Servico nao encontrado ou sem permissao.");

                write(fd, &r, sizeof(r));
                close(fd);
            }
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
//  Thread que lê telemetria do veiculo
// ----------------------------------------------------------------------
static void *thread_veiculos(void *arg)
{
    int id = (int)(intptr_t)arg; // índice em veiculos[]
    int fd = veiculos[id].fd_telemetria;
    int id_servico = veiculos[id].id_servico;
    int pid_veiculo = veiculos[id].pid;

    char buf[256];
    ssize_t n;
    // !MONITORIZAÇÃO DEDICADA
    // Cada veículo tem uma thread exclusiva para ler o seu pipe.
    // Usamos strtok_r para garantir que processamos mensagens completas,
    // mesmo que cheguem "coladas" (ex: PERC 100\nFIM OK\n).
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        // !=== Processar linha a linha === //
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
    veiculos[id].ativo = 0;

    // === O VEÍCULO DESLIGOU O PIPE (ACABOU) ===

    close(fd);              // Fechar o nosso lado do pipe
    veiculos[id].ativo = 0; // Libertar o slot

    // === ELIMINAR O ZOMBIE ===
    // O waitpid recolhe o status do filho e remove-o da tabela de processos.
    // Usamos o PID que guardámos no início da função.
    waitpid(pid_veiculo, NULL, 0);

    printf("[CTRL] veiculo idx=%d terminou\n", id);
    return NULL;
}

// ----------------------------------------------------------------------
//  Thread de relógio: incrementa tempo_simulado e lança serviços
// ----------------------------------------------------------------------
static void *thread_relogio(void *arg)
{
    (void)arg; // evita warning

    while (running_ctrl)
    {
        sleep(1);
        tempo_simulado++;

        pthread_mutex_lock(&mux_servicos);
        // varre os serviços à espera
        for (int i = 0; i < n_servicos; i++)
        {
            Servico *s = &servicos[i];
            // 1. Verifica se já passou a hora (<=) E se ainda não foi feito (S_AGENDADO)
            if (s->estado == S_AGENDADO && s->hora <= tempo_simulado)
            {
                // 2. Verifica se temos carro para ele
                if (encontra_slot_livre_veiculo() != -1)
                {
                    printf("[RELOGIO] hora=%d (agendada=%d), lançar servico id=%d\n",
                           tempo_simulado, s->hora, s->id);
                    // O spawn vai marcar o veículo como ocupado imediatamente
                    spawn_veiculo_para_servico(s);
                }
                else
                {
                    // Na próxima volta do while (segundo seguinte), voltamos a tentar!
                    printf("[RELOGIO] Servico %d em espera (sem veiculos disponiveis)...\n", s->id);
                }
            }
        }
        pthread_mutex_unlock(&mux_servicos);
    }

    return NULL;
}

// ----------------------------------------------------------------------
// Loop principal de pedidos: lê estruturas Pedido do FIFO_PEDIDOS
// ----------------------------------------------------------------------
static void *thread_clientes(void *arg)
{
    (void)arg;
    int fd_read = open(FIFO_PEDIDOS, O_RDONLY);
    if (fd_read == -1)
    {
        perror("[CTRL] nao consegui abrir FIFO_PEDIDOS para leitura");
        return NULL;
    }

    // *=================================================================================
    // *TÉCNICA DO "DUMMY WRITER" (O Pé na Porta)
    // *---------------------------------------------------------------------------------
    // Abrimos o FIFO também em modo de ESCRITA (O_WRONLY), mas nunca vamos usar este fd.
    //
    //! PORQUÊ?
    // O comportamento padrão do read() num FIFO é retornar 0 (EOF - End Of File)
    // assim que NÃO houver nenhum processo com o FIFO aberto para escrita.
    //
    //! O PROBLEMA SEM ISTO:
    // Se todos os clientes se desligarem, o número de escritores cai para 0.
    // O read() retornaria 0 instantaneamente e repetidamente.
    // O ciclo while entraria em "Busy Waiting" (loop infinito), consumindo 100% de CPU.
    //
    //! A SOLUÇÃO:
    // Ao mantermos este 'fd_dummy' aberto, o Kernel vê que existe sempre pelo menos
    // um escritor ativo (nós próprios). Logo, quando os clientes saem, o read()
    // não retorna 0; em vez disso, fica BLOQUEADO à espera de novos dados.
    // Isto poupa CPU e mantém a lógica do servidor correta.
    // *=================================================================================
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
        else
        {
            printf("[CTRL] pedido desconhecido do tipo %d\n", p.tipo);
        }
    }

    close(fd_read);
    if (fd_dummy != -1)
        close(fd_dummy);
    return NULL;
}

// ----------------------------------------------------------------------
//  Mostrar lista de serviços agendados
// ----------------------------------------------------------------------
static void cmd_listar(void)
{
    pthread_mutex_lock(&mux_servicos);
    printf("=== SERVICOS AGENDADOS ===\n");
    if (n_servicos == 0)
    {
        printf("Lista vazia.\n");
    }

    for (int i = 0; i < n_servicos; i++)
    {
        // Traduzir o estado (int) para Texto (string)
        char estado_str[20];
        switch (servicos[i].estado)
        {
        case S_AGENDADO:
            strcpy(estado_str, "AGENDADO");
            break;
        case S_EM_CURSO:
            strcpy(estado_str, "EM CURSO");
            break;
        case S_TERMINADO:
            strcpy(estado_str, "TERMINADO");
            break;
        case S_CANCELADO:
            strcpy(estado_str, "CANCELADO");
            break;
        default:
            strcpy(estado_str, "???");
            break;
        }

        printf("ID: %-3d | User: %-10s | Estado: %-10s | Hora: %-4d | Local: %s\n",
               servicos[i].id,
               servicos[i].user,
               estado_str, // <--- Agora mostra texto!
               servicos[i].hora,
               servicos[i].local);
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
    printf("=== HISTORICO DE UTILIZADORES (%d) ===\n", n_users);
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
        printf("Servico %d cancelado (ainda nao tinha iniciado).\n", id);
        notificar_cliente(s, "O seu agendamento foi cancelado pelo administrador.");
    }
    else if (s->estado == S_EM_CURSO)
    {
        // Se ja esta a andar, temos de parar o veiculo!
        if (s->pid_veiculo != -1)
        {
            // Envia SIGINT ao processo veiculo para ele terminar

            union sigval val;
            val.sival_int = 1; // 1 = Ordem de Cancelamento
            sigqueue(s->pid_veiculo, SIGINT, val);
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

    // 2. Loop de Espera INTELIGENTE (Deteta clientes mortos)

    while (1)
    {
        int ativos = 0;

        pthread_mutex_lock(&mux_servicos);
        for (int i = 0; i < n_servicos; i++)
        {
            Servico *s = &servicos[i];
            // Verifica apenas se os serviços estão ativos
            if (s->estado == S_AGENDADO || s->estado == S_EM_CURSO)
            {
                // === A MUDANÇA CRÍTICA AQUI ===
                // Tentamos avisar o cliente. Se a função retornar 0, o cliente morreu.
                int cliente_vivo = notificar_cliente(s, "O servidor esta a encerrar. Aguarde...");

                if (cliente_vivo)
                {
                    // Cliente respondeu, temos de esperar por ele
                    ativos++;
                }
                else
                {
                    // Cliente MORREU (Pipe quebrado).
                    // Não podemos ficar à espera dele. Forçamos o cancelamento!
                    printf("[CTRL] Cliente do servico %d desapareceu. A forcar cancelamento.\n", s->id);

                    if (s->estado == S_EM_CURSO && s->pid_veiculo != -1)
                    {
                        // kill(s->pid_veiculo, SIGINT); // Pára o táxi
                        union sigval val;
                        val.sival_int = 1; // 1 = Ordem de Cancelamento
                        sigqueue(s->pid_veiculo, SIGINT, val);
                    }
                    s->estado = S_CANCELADO; // Remove da lista de ativos
                }
            }
        }
        pthread_mutex_unlock(&mux_servicos);

        // Se não há serviços ativos, saímos do loop
        if (ativos == 0)
        {
            break; // Não há mais táxis na estrada
        }

        printf("[CTRL] A aguardar conclusao de %d servicos ativos...\n", ativos);
        sleep(2); // Verifica a cada 2 segundos
    }

    /*  // 3. Notificar todos os clientes que o sistema vai encerrar
     printf("[CTRL] A notificar clientes do encerramento...\n");

     pthread_mutex_lock(&mux_servicos);

     for (int i = 0; i < MAX_USERS; i++)
     {
         // Só enviamos a quem está ativo na lista de utilizadores
         if (users[i].ativo)
         {
             // Usamos o FIFO que guardámos quando ele fez login (HELLO)
             int fd = open(users[i].fifo_cli, O_RDWR);
             if (fd != -1)
             {
                 Resposta r;
                 memset(&r, 0, sizeof(r));
                 r.ok = 1;
                 r.id_servico = -1;

                 // Esta é a mensagem chave que o cliente vai reconhecer para se desligar
                 snprintf(r.msg, sizeof(r.msg), "SERVIDOR_ENCERRADO");

                 write(fd, &r, sizeof(r));
                 close(fd);
                 printf("[CTRL] Notificado user: %s\n", users[i].nome);
             }
         }
     }

     pthread_mutex_unlock(&mux_servicos);

     printf("[CTRL] Todos os servicos terminaram. A desligar sistema.\n");
  */

    // 3. Fim da espera -> Mandar o main sair do loop
    running_ctrl = 0;
}

// ----------------------------------------------------------------------
//  Limpeza final de recursos antes de sair
// ----------------------------------------------------------------------
static void cmd_final(void)
{
    printf("\n[CTRL] === A realizar limpeza final de recursos ===\n");

    // 1. Notificar todos os clientes ligados que vamos fechar
    pthread_mutex_lock(&mux_servicos);
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (users[i].ativo)
        {
            // Tenta enviar a mensagem final
            int fd = open(users[i].fifo_cli, O_RDWR); // O_RDWR para não bloquear
            if (fd != -1)
            {
                Resposta r;
                memset(&r, 0, sizeof(r));
                r.ok = 1;
                r.id_servico = -1;
                snprintf(r.msg, sizeof(r.msg), "SERVIDOR_ENCERRADO");
                write(fd, &r, sizeof(r));
                close(fd);
                printf("[CTRL] Notificado user: %s\n", users[i].nome);
            }
        }
    }

    // 2. Garantir que nenhum veículo fica vivo (Safety Net)
    for (int i = 0; i < n_servicos; i++)
    {
        if (servicos[i].pid_veiculo != -1 && servicos[i].estado == S_EM_CURSO)
        {
            printf("[CTRL] A cancelar veículo residual PID %d\n", servicos[i].pid_veiculo);

            // Usamos sigqueue para manter o padrão, ou kill simples
            union sigval val;
            val.sival_int = 1;
            sigqueue(servicos[i].pid_veiculo, SIGINT, val);
        }
    }
    pthread_mutex_unlock(&mux_servicos);

    // 3. Apagar o FIFO do servidor
    unlink(FIFO_PEDIDOS);
    printf("[CTRL] FIFO apagado. Sistema encerrado.\n");
}

// ----------------------------------------------------------------------
//  Manipulador de sinal SIGINT/SIGHUP/SIGPIPE
// ----------------------------------------------------------------------
static void handle_signal(int sig, siginfo_t *info, void *context)
{
    (void)context; // Não usado
    (void)info;    // Não usado

    // Variável estática para contar Ctrl+C (apenas para SIGINT)
    static int ctrl_c_count = 0;

    // === CASO 1: INTERRUPÇÃO DE TECLADO (Ctrl+C) ===
    if (sig == SIGINT)
    {
        if (ctrl_c_count == 0)
        {
            const char aviso[] = "\n[SINAL] Recebi SIGINT. Pressiona novamente para sair.\n";
            write(STDOUT_FILENO, aviso, sizeof(aviso) - 1);
            ctrl_c_count++;
        }
        else
        {
            const char fim[] = "\n[SINAL] 2º sinal recebido. A encerrar...\n";
            write(STDOUT_FILENO, fim, sizeof(fim) - 1);
            running_ctrl = 0; // Quebra o loop principal
        }
    }
    // === CASO 2: PERDA DE TERMINAL (Janela fechada/SSH caiu) ===
    else if (sig == SIGHUP)
    {
        // Aqui não pedimos confirmação porque o utilizador já não está lá!
        const char hup_msg[] = "\n[SIGHUP] Terminal fechado. Encerrar imediatamente.\n";

        // Tentamos escrever (pode falhar se o stdout já tiver ido, mas tentamos)
        write(STDOUT_FILENO, hup_msg, sizeof(hup_msg) - 1);

        running_ctrl = 0; // Shutdown imediato
    }
    // === CASO 3: PIPE QUEBRADO (Cliente morreu) ===
    else if (sig == SIGPIPE)
    {
        // Apenas ignoramos ou logamos erro, mas NÃO matamos o servidor
        // (O servidor deve continuar a correr para os outros clientes)
        const char pipe_msg[] = "[AVISO] SIGPIPE detetado (escrita falhada).\n";
        write(STDERR_FILENO, pipe_msg, sizeof(pipe_msg) - 1);

        // Não fazemos mais nada; a lógica de deteção de clientes mortos
        // está implementada na função notificar_cliente().
    }
}