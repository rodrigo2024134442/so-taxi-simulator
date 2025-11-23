#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>

#include "comunicacao.h"
#include "cliente_interno.h"

#define FIFO_DIR "./tmp"
#define FIFO_PEDIDOS FIFO_DIR "/taxi_pedidos"
#define MAX_HORA_TICKS 24
#define MAX_DIST_KM 100

// variável global modificada pelo handler de sinais
volatile sig_atomic_t running = 1;

static int cmd_agendar(ContextoCliente *ctx, const char *args);
static int cmd_entrar(ContextoCliente *ctx, const char *args);
static int cmd_cancelar_pedido(ContextoCliente *ctx, const char *args);
static int cmd_consultar(ContextoCliente *ctx, const char *args);
static int cmd_help(ContextoCliente *ctx, const char *args);
static int cmd_sair(ContextoCliente *ctx, const char *args);
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
    char fifo_resp[128];
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
    hello.tipo = CMD_HELLO;
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
    char comando[32];
    char args[224];
    ContextoCliente ctx;
    ctx.user = user;
    ctx.fifo_resp = fifo_resp_path;

    // 1. Abrir o FIFO de resposta e MANTER ABERTO
    // Usamos O_RDWR para evitar receber EOF imediato se ninguem estiver a escrever
    // (Isto é um truque comum em FIFOs para "manter a linha aberta")
    int fd_resp = open(fifo_resp_path, O_RDWR);
    if (fd_resp == -1)
    {
        perror("[CLI] Erro ao abrir FIFO de resposta");
        return;
    }

    printf("Bem-vindo %s! Podes escrever comandos.\n", user);

    while (running)
    {
        fd_set readfds;                 // conjunto de descritores para ler
        FD_ZERO(&readfds);              // limpar conjunto
        FD_SET(STDIN_FILENO, &readfds); // adicionar stdin ao conjunto
        FD_SET(fd_resp, &readfds);      // adicionar FIFO de resposta ao conjunto

        // O select fica bloqueado aqui até ALGUÉM falar
        int maxfd = (STDIN_FILENO > fd_resp) ? STDIN_FILENO : fd_resp;

        // MULTIPLEXAGEM DE I/O (SELECT)
        // Usamos select() para vigiar simultaneamente o teclado (stdin) e o FIFO.
        // Isto evita que o cliente bloqueie à espera de input do utilizador
        // e permite mostrar mensagens do servidor em tempo real.
        int atividade = select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if ((atividade < 0) && (errno != EINTR))
        {
            perror("Erro no select");
            break;
        }

        // === CASO 1: MENSAGEM DO CONTROLADOR (Veículo chegou, etc) === //
        if (FD_ISSET(fd_resp, &readfds))
        {
            Resposta r;
            ssize_t n = read(fd_resp, &r, sizeof(r));
            if (n == sizeof(r))
            {
                // Limpar a linha atual (cosmética) e imprimir a mensagem
                printf("\n[AVISO] %s\ncomando> ", r.msg);
                fflush(stdout);
            }
        }
        // === CASO 2: O UTILIZADOR ESCREVEU NO TECLADO === //
        if (FD_ISSET(STDIN_FILENO, &readfds))
        {
            if (!fgets(linha, sizeof(linha), stdin))
            {
                break; // EOF
            }

            // Remover o \n final
            linha[strcspn(linha, "\n")] = '\0';

            if (linha[0] == '\0')
            {
                printf("comando> ");
                fflush(stdout);
                continue; // linha vazia, pedir outro comando e
            }

            // Limpar buffers antes de ler
            memset(comando, 0, sizeof(comando));
            memset(args, 0, sizeof(args));

            // Tenta separar: comando + argumentos
            // %31s lê a primeira palavra
            // %223[^\n] lê o resto da linha até ao fim
            // int n =
            sscanf(linha, "%31s %223[^\n]", comando, args);

            /* if (n < 1)
                continue; // Linha vazia ou erro
 */
            // Executar comandos
            // NOTA: Como o FIFO já está aberto aqui (fd_resp),
            // as funções cmd_* NÃO devem tentar abrir o FIFO de leitura novamente
            // ou vão bloquear/conflituar.
            // TEMOS DE AJUSTAR AS FUNÇÕES cmd_agendar E cmd_entrar PARA SÓ ENVIAREM O PEDIDO.

            if (strcmp(comando, "agendar") == 0)
            {
                cmd_agendar(&ctx, args);
            }
            else if (strcmp(comando, "entrar") == 0)
            {
                cmd_entrar(&ctx, args);
            }
            else if (strcmp(comando, "h") == 0 || strcmp(comando, "help") == 0)
            {
                cmd_help(&ctx, args);
            }
            else if (strcmp(comando, "cancelar") == 0)
            {
                // Precisas de declarar cmd_cancelar_pedido no topo!
                cmd_cancelar_pedido(&ctx, args);
            }
            else if (strcmp(comando, "consultar") == 0)
            {
                // Precisas de declarar cmd_consultar no topo!
                cmd_consultar(&ctx, args);
            }
            else if (strcmp(comando, "sair") == 0)
            {
                if (cmd_sair(&ctx, args) == 0)
                {
                    break; // sair do loop
                }
            }
            else
            {
                printf("[CLI] Comando '%s' desconhecido. Use 'h' para ajuda.\n", comando);
                fflush(stdout);
            }
        }
    }
    close(fd_resp); // fechar FIFO de resposta
}

static int cmd_help(ContextoCliente *ctx, const char *args)
{
    (void)ctx;
    (void)args; // Evitar warnings de variáveis não usadas
    printf("\n=== Comandos Disponiveis ===\n");
    printf("  agendar <seg> <local> <km>  : Agenda uma nova viagem\n");
    printf("  entrar                      : Entra no taxi quando este chegar\n");
    printf("  sair                        : Encerra a aplicacao\n");
    printf("============================\n");
    return 0;
}

static int cmd_agendar(ContextoCliente *ctx, const char *args)
{
    int hora, dist;
    char local[64];

    // Tenta ler 3 variáveis da string de argumentos
    // O formato esperado é: <segundos> <local> <km>
    if (sscanf(args, "%d %63s %d", &hora, local, &dist) != 3)
    {
        printf("[CLI] Uso incorreto. Sintaxe: agendar <segundos> <local> <km>\n");
        return -1;
    }

    if (hora < 0 || dist <= 0)
    {
        printf("[CLI] Erro: Hora deve ser >= 0 e distancia > 0.\n");
        return -1;
    }

    // Preencher estrutura Pedido
    Pedido p;
    memset(&p, 0, sizeof(p));
    p.tipo = CMD_AGENDAR;
    p.pid_cliente = getpid();
    // O snprintf garante sempre que a string termina com \0, mesmo que tenha de cortar o texto.
    snprintf(p.user, sizeof(p.user), "%s", ctx->user);
    snprintf(p.fifo_resp, sizeof(p.fifo_resp), "%s", ctx->fifo_resp);

    p.hora = hora;
    p.dist = dist;
    snprintf(p.local, sizeof(p.local), "%s", local);

    // Enviar ao FIFO_PEDIDOS
    int fd = open(FIFO_PEDIDOS, O_WRONLY);
    if (fd == -1)
    {
        perror("[CLI] Erro ao abrir FIFO de pedidos");
        return -1;
    }
    write(fd, &p, sizeof(p));
    close(fd);

    printf("[CLI] Pedido enviado. Aguarde confirmacao...\n");
    // Aguardar resposta
    /*     int fd_resp = open(ctx->fifo_resp, O_RDONLY);
        if (fd_resp != -1)
        {
            Resposta r;
            if (read(fd_resp, &r, sizeof(r)) == sizeof(r))
            {
                if (r.ok)
                    printf("[CLI] Sucesso: %s\n", r.msg);
                else
                    printf("[CLI] Erro: %s\n", r.msg);
            }
            close(fd_resp);
        } */
    return 0;
}

// Envia pedido de cancelamento
static int cmd_cancelar_pedido(ContextoCliente *ctx, const char *args)
{
    int id;
    if (sscanf(args, "%d", &id) != 1)
    {
        printf("[CLI] Uso: cancelar <id_servico> (0 para todos)\n");
        return -1;
    }

    Pedido p;
    memset(&p, 0, sizeof(p));
    p.tipo = CMD_CANCELAR;
    p.pid_cliente = getpid();
    snprintf(p.user, sizeof(p.user), "%s", ctx->user);
    snprintf(p.fifo_resp, sizeof(p.fifo_resp), "%s", ctx->fifo_resp);

    // Reutilizamos o campo 'id_servico' da Resposta ou usamos 'hora' do Pedido?

    // O Pedido não tem campo ID específico, mas podemos usar o campo 'dist' ou 'hora'
    // como "transportadora" do ID, já que não são usados no cancelar.
    // Vamos usar p.dist para enviar o ID do serviço a cancelar.
    p.dist = id;

    int fd = open(FIFO_PEDIDOS, O_WRONLY);
    if (fd != -1)
    {
        write(fd, &p, sizeof(p));
        close(fd);
        printf("[CLI] Pedido de cancelamento enviado.\n");
    }
    else
    {
        perror("[CLI] Erro ao contactar controlador");
    }
    return 0;
}

// Envia pedido de consulta
static int cmd_consultar(ContextoCliente *ctx, const char *args)
{
    (void)args; // Sem argumentos

    Pedido p;
    memset(&p, 0, sizeof(p));
    p.tipo = CMD_CONSULTAR;
    p.pid_cliente = getpid();
    snprintf(p.user, sizeof(p.user), "%s", ctx->user);
    snprintf(p.fifo_resp, sizeof(p.fifo_resp), "%s", ctx->fifo_resp);

    int fd = open(FIFO_PEDIDOS, O_WRONLY);
    if (fd != -1)
    {
        write(fd, &p, sizeof(p));
        close(fd);
        printf("[CLI] Pedido de consulta enviado.\n");
    }
    return 0;
}

static int cmd_sair(ContextoCliente *ctx, const char *args)
{
    (void)ctx;
    (void)args;
    printf("[CLI] A terminar aplicacao...\n");
    return 0; // Retorna 0 para parar o loop no main (conforme o teu if)
}

static int cmd_entrar(ContextoCliente *ctx, const char *args)
{
    (void)args; // Não usamos argumentos para entrar, mas a assinatura pede

    Pedido p;
    memset(&p, 0, sizeof(p));
    p.tipo = CMD_ENTRAR;
    p.pid_cliente = getpid();
    snprintf(p.user, sizeof(p.user), "%s", ctx->user);
    snprintf(p.fifo_resp, sizeof(p.fifo_resp), "%s", ctx->fifo_resp);

    // Enviar pedido
    int fd = open(FIFO_PEDIDOS, O_WRONLY);
    if (fd == -1)
    {
        perror("[CLI] Erro ao contactar controlador");
        return -1;
    }
    write(fd, &p, sizeof(p));
    close(fd);

    /*    // Ler resposta
       int fd_resp = open(ctx->fifo_resp, O_RDONLY);
       if (fd_resp != -1)
       {
           Resposta r;
           if (read(fd_resp, &r, sizeof(r)) == sizeof(r))
           {
               printf("[CLI] %s\n", r.msg);
           }
           close(fd_resp);
       } */
    return 0;
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
