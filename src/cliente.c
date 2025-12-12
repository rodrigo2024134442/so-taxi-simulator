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

#include "taxis_protocol.h"
#include "cliente_ui.h"

#define FIFO_DIR "./tmp"
#define FIFO_PEDIDOS FIFO_DIR "/taxi_pedidos"
#define MAX_HORA_TICKS 24
#define MAX_DIST_KM 100

// variável global modificada pelo handler de sinais
// para controlar o loop principal
volatile sig_atomic_t running = 1;

static int cmd_agendar(ContextoCliente *ctx, const char *args);                  // agendar novo serviço
static int cmd_entrar(ContextoCliente *ctx, const char *args);                   // entrar no veículo
static int cmd_cancelar_pedido(ContextoCliente *ctx, const char *args);          // cancelar serviço
static int cmd_consultar(ContextoCliente *ctx, const char *args);                // consultar serviços
static int cmd_help(ContextoCliente *ctx, const char *args);                     // mostrar ajuda
static int cmd_sair(ContextoCliente *ctx, const char *args);                     // sair da aplicação
static void cancelar_tudo_ao_sair(const char *user, const char *fifo_resp_path); // cancelar todos os serviços ao sair
static void loop_comandos(const char *user, const char *fifo_resp_path);         // loop principal de comandos
static void handle_signal(int sig, siginfo_t *info, void *context);              // handler de sinais
static void setup_directory(void);                                               // cria diretório FIFO_DIR

// ----------------------------------------------------------------------
//  Função principal do cliente
// ----------------------------------------------------------------------
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
    // SA_RESTART impede que o fgets falhe quando recebes um sinal
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    // Caminho da FIFO deste cliente
    char fifo_resp[128];
    snprintf(fifo_resp, sizeof(fifo_resp), FIFO_DIR "/taxi_cli_%s", user);

    // Verificar se o FIFO já existe (outro cliente com o mesmo user?)
    if (access(fifo_resp, F_OK) == 0)
    {
        fprintf(stderr, "[ERRO] O utilizador '%s' já parece estar ligado!\n", user);
        fprintf(stderr, "Nota: Se o cliente anterior crashou, apaga o ficheiro manualmente:\n");
        fprintf(stderr, "      rm %s\n", fifo_resp);
        return 1; // Sai do programa para não estragar a outra sessão
    }
    // Criar FIFO de resposta
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

    // Quando o loop termina (seja por 'terminar' ou Ctrl+C), solicitar cancelamento de todos os serviços ativos
    printf("\n[CLI] A solicitar cancelamento de servicos ativos...\n");
    cancelar_tudo_ao_sair(user, fifo_resp);

    // no fim, limpar FIFO
    unlink(fifo_resp);
    return 0;
}
// ----------------------------------------------------------------------
// Loop principal que lê comandos do utilizador e envia pedidos ao controlador
// ----------------------------------------------------------------------
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

        // O select fica bloqueado aqui até haver atividade em stdin ou no FIFO
        int maxfd = (STDIN_FILENO > fd_resp) ? STDIN_FILENO : fd_resp;

        // MULTIPLEXAGEM DE I/O (SELECT)
        // Usamos select() para vigiar simultaneamente o teclado (stdin) e o FIFO.
        // Isto evita que o cliente bloqueie à espera de input do utilizador
        // e permite mostrar mensagens do servidor em tempo real.
        int atividade = select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if (atividade < 0)
        {
            if (errno == EINTR)
            {
                // Foi interrompido por um sinal (ex: Ctrl+C)
                // Se o handler mudou running para 0, saímos JÁ!
                if (running)
                    continue; // Se não, tentamos outra vez
                break;
            }
            perror("Erro no select");
            break;
        }

        // === CASO 1: MENSAGEM DO CONTROLADOR (Veículo chegou, etc) === //
        if (FD_ISSET(fd_resp, &readfds))
        {
            Resposta r;
            ssize_t n = read(fd_resp, &r, sizeof(r));
            // Se n=0, significa que o servidor fechou o pipe (EOF) ou crashou
            if (n == 0)
            {
                printf("\n[CRITICO] O servidor desligou-se. A encerrar o cliente.\n");
                break; // Sai do loop e termina
            }

            if (n == sizeof(r))
            {
                if (strcmp(r.msg, "SERVIDOR_ENCERRADO") == 0)
                {
                    printf("\n\n[AVISO DO SISTEMA] O servidor encerrou as operacoes.\n");
                    printf("Obrigado por utilizar o nosso serviço. Até breve!\n");
                    break; // Sai do loop e termina o cliente automaticamente
                }
                // Mostrar mensagem ao utilizador
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
            sscanf(linha, "%31s %223[^\n]", comando, args);

            // Executar comandos
            if (strcmp(comando, "agendar") == 0)
            {
                cmd_agendar(&ctx, args);
            }
            else if (strcmp(comando, "entrar") == 0)
            {
                cmd_entrar(&ctx, args);
            }
            else if (strcmp(comando, "ajuda") == 0 || strcmp(comando, "?") == 0 || strcmp(comando, "help") == 0 || strcmp(comando, "h") == 0)
            {
                cmd_help(&ctx, args);
            }
            else if (strcmp(comando, "cancelar") == 0)
            {
                cmd_cancelar_pedido(&ctx, args);
            }
            else if (strcmp(comando, "consultar") == 0)
            {
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

// ----------------------------------------------------------------------
// Comandos do cliente
// ----------------------------------------------------------------------
static int cmd_help(ContextoCliente *ctx, const char *args)
{
    (void)ctx;
    (void)args; // Evitar warnings de variáveis não usadas
    printf("\n=== Comandos Disponiveis ===\n");
    printf("  agendar <seg> <local> <km>  : Agenda uma nova viagem\n");
    printf("  entrar                      : Entra no taxi quando este chegar\n");
    printf("  consultar                   : Consulta o estado dos teus servicos\n");
    printf("  cancelar <id>               : Cancela o servico com o ID indicado (0 para todos)\n");
    printf("  ajuda (h, ?, help)          : Mostra esta ajuda\n");
    printf("  sair                        : Encerra a aplicacao\n");
    printf("============================\n");
    return 0;
}

// ----------------------------------------------------------------------
// Envia pedido de agendamento
// ----------------------------------------------------------------------
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
    // A resposta será lida no loop principal via FIFO de resposta
    return 0;
}

// ----------------------------------------------------------------------
// Envia pedido de cancelamento
// ----------------------------------------------------------------------
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

// ----------------------------------------------------------------------
// Envia pedido de consulta
// ----------------------------------------------------------------------
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

// ----------------------------------------------------------------------
// Envia pedido de sair
// ----------------------------------------------------------------------
static int cmd_sair(ContextoCliente *ctx, const char *args)
{
    (void)ctx;
    (void)args;
    printf("[CLI] A terminar aplicacao...\n");
    return 0; // Retorna 0 para parar o loop no main
}

// ----------------------------------------------------------------------
// Envia pedido de entrar no veículo
// ----------------------------------------------------------------------
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
    // A resposta será lida no loop principal via FIFO de resposta
    return 0;
}

// ----------------------------------------------------------------------
//  Trata pedido especial ao sair para cancelar todos os serviços deste user
// ----------------------------------------------------------------------
static void cancelar_tudo_ao_sair(const char *user, const char *fifo_resp)
{
    // Para não bloquear se o servidor estiver morto
    // abrimos o FIFO em O_RDWR
    int fd = open(FIFO_PEDIDOS, O_RDWR);
    if (fd == -1)
        return; // Controlador já morreu, não vale a pena

    Pedido p;
    memset(&p, 0, sizeof(p));
    p.tipo = CMD_CANCELAR;
    p.pid_cliente = getpid();
    snprintf(p.user, sizeof(p.user), "%s", user);
    snprintf(p.fifo_resp, sizeof(p.fifo_resp), "%s", fifo_resp);

    // ID 0 significa "TODOS" na nossa lógica
    p.dist = 0;

    write(fd, &p, sizeof(p));
    close(fd);
    // Não esperamos resposta porque estamos a sair
}

// ----------------------------------------------------------------------
// Configura o diretório FIFO_DIR
// ----------------------------------------------------------------------
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

// ----------------------------------------------------------------------
// Trata sinais como SIGINT e SIGPIPE
// ----------------------------------------------------------------------
static void handle_signal(int sig, siginfo_t *info, void *context)
{
    (void)context; // Não usado
    (void)info;    // Não usado

    // Variável estática para contar Ctrl+C (proteção contra saída acidental)
    static int ctrl_c_count = 0;

    // === CASO 1: INTERRUPÇÃO DE TECLADO (Ctrl+C) ===
    if (sig == SIGINT)
    {
        if (ctrl_c_count == 0)
        {
            // Primeiro toque: Aviso
            const char msg[] = "\n[SINAL] Recebi SIGINT. Pressiona novamente para sair.\n";
            write(STDOUT_FILENO, msg, sizeof(msg) - 1);
            ctrl_c_count++;
        }
        else
        {
            // Segundo toque: Sair
            const char msg[] = "\n[SINAL] A encerrar...\n";
            write(STDOUT_FILENO, msg, sizeof(msg) - 1);
            running = 0; // Termina o loop principal
        }
    }
    // === CASO 2: JANELA FECHADA (SIGHUP) ===
    else if (sig == SIGHUP)
    {
        // Se a janela fecha, morremos logo. Não há user para avisar.
        running = 0;
    }
    // === CASO 3: SERVIDOR DESAPARECEU (SIGPIPE) ===
    else if (sig == SIGPIPE)
    {
        // Se tentámos escrever no FIFO e falhou, o servidor morreu.
        // O cliente não serve para nada sem servidor
        const char msg[] = "\n[ERRO] Conexão perdida com o Controlador (SIGPIPE).\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        running = 0;
    }
}