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

volatile sig_atomic_t ctrl_c_pressed = 0;
volatile sig_atomic_t pediu_sair = 0;
volatile sig_atomic_t pode_sair = 0;
static int confirmar_saida_segura(ContextoCliente *ctx);

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

    int fd_resp = open(fifo_resp_path, O_RDWR);
    if (fd_resp == -1)
    {
        perror("[CLI] Erro ao abrir FIFO de resposta");
        return;
    }

    printf("Bem-vindo %s! Podes escrever comandos.\n", user);

    while (running)
    {
        //  VERIFICAR SE CTRL+C FOI PREMIDO
        if (ctrl_c_pressed)
        {
            if (confirmar_saida_segura(&ctx))
            {
                break;  // Sair do loop
            }
            ctrl_c_pressed = 0;  // Reset
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(fd_resp, &readfds);

        int maxfd = (STDIN_FILENO > fd_resp) ? STDIN_FILENO : fd_resp;

        //  SELECT COM TIMEOUT PARA PERMITIR VERIFICAR ctrl_c_pressed
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int atividade = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if ((atividade < 0) && (errno != EINTR))
        {
            perror("Erro no select");
            break;
        }

        // === CASO 1: MENSAGEM DO CONTROLADOR === //
        if (FD_ISSET(fd_resp, &readfds))
        {
            Resposta r;
            ssize_t n = read(fd_resp, &r, sizeof(r));
            if (n == sizeof(r))
            {
                printf("\n[AVISO] %s\n", r.msg);

                // SE CLIENTE PEDIU SAIR E RECEBE RESPOSTA
                if (pediu_sair)
                {
                    pode_sair = r.ok;  // 1 = pode sair, 0 = não pode
                }

                printf("comando> ");
                fflush(stdout);
            }
        }

        // === CASO 2: O UTILIZADOR ESCREVEU NO TECLADO === //
        if (FD_ISSET(STDIN_FILENO, &readfds))
        {
            if (!fgets(linha, sizeof(linha), stdin))
            {
                break;
            }

            linha[strcspn(linha, "\n")] = '\0';

            if (linha[0] == '\0')
            {
                printf("comando> ");
                fflush(stdout);
                continue;
            }

            memset(comando, 0, sizeof(comando));
            memset(args, 0, sizeof(args));

            int n = sscanf(linha, "%31s %223[^\n]", comando, args);
            if (n < 1)
                continue;

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
                cmd_sair(&ctx, args);
            }
            else
            {
                printf("[CLI] Comando '%s' desconhecido. Use 'h' para ajuda.\n", comando);
                fflush(stdout);
            }
        }
    }
    close(fd_resp);
}

//Confirmar saída segura
static int confirmar_saida_segura(ContextoCliente *ctx)
{
    printf("\n[AVISO] Pressione Ctrl+C novamente para confirmar saida (3 segundos).\n");
    printf("Ou escreva um comando para continuar.\n");

    // Dar 3 segundos para confirmar
    ctrl_c_pressed = 0;
    sleep(3);

    if (ctrl_c_pressed)
    {
        // Utilizador premiu Ctrl+C novamente dentro de 3s
        printf("\n[CLI] A terminar de forma segura...\n");
        
        // Notificar o controlador que o cliente vai sair
        Pedido p;
        memset(&p, 0, sizeof(p));
        p.tipo = CMD_SAIR;
        p.pid_cliente = getpid();
        snprintf(p.user, sizeof(p.user), "%s", ctx->user);
        snprintf(p.fifo_resp, sizeof(p.fifo_resp), "%s", ctx->fifo_resp);

        int fd = open(FIFO_PEDIDOS, O_WRONLY);
        if (fd != -1)
        {
            write(fd, &p, sizeof(p));
            close(fd);
        }

        return 1;  // Sinal para sair
    }
    else
    {
        printf("[CLI] Saida cancelada. Continua...\n");
        printf("comando> ");
        fflush(stdout);
        return 0;  // Continuar
    }
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

//cmd_sair
static int cmd_sair(ContextoCliente *ctx, const char *args)
{
    (void)args;

    // Enviar pedido CMD_SAIR ao controlador
    Pedido p;
    memset(&p, 0, sizeof(p));
    p.tipo = CMD_SAIR;
    p.pid_cliente = getpid();
    snprintf(p.user, sizeof(p.user), "%s", ctx->user);
    snprintf(p.fifo_resp, sizeof(p.fifo_resp), "%s", ctx->fifo_resp);

    int fd = open(FIFO_PEDIDOS, O_WRONLY);
    if (fd == -1)
    {
        perror("[CLI] Erro ao contactar controlador");
        return -1;
    }
    write(fd, &p, sizeof(p));
    close(fd);

    printf("[CLI] A verificar se podes sair...\n");
    pediu_sair = 1;
    pode_sair = 0;

    // Bloquear até ter resposta (5 segundos timeout)
    for (int tentativas = 0; tentativas < 50; tentativas++)
    {
        if (pode_sair)
        {
            printf("[CLI] Podes sair com seguranca.\n");
            pediu_sair = 0;
            running = 0;  // Sinal para sair do loop
            return 0;
        }
        usleep(100000);  // 100ms
    }

    printf("[CLI] Timeout na resposta do controlador ou tens viagem ativa.\n");
    pediu_sair = 0;
    pode_sair = 0;
    return -1;
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

//handle_signal
static void handle_signal(int sig, siginfo_t *info, void *context)
{
    (void)info;
    (void)context;
    if (sig == SIGINT)
    {
        ctrl_c_pressed = 1;  // ← NÃO seta running = 0 aqui
    }
    else if (sig == SIGPIPE)
    {
        // Ignorar SIGPIPE
    }
}
