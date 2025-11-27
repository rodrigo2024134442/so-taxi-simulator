# 🚖 Sistema de Gestão de Táxis (Simulador)

Este projeto consiste na simulação de uma plataforma de gestão de uma frota de táxis autónomos, desenvolvido em **Linguagem C** para ambiente **Linux**.

O sistema utiliza mecanismos de **Sistemas Operativos** (IPC, concorrência, sinais) para gerir a comunicação entre múltiplos clientes, um **controlador** central e processos independentes para cada **veículo**.

---

## 📋 Funcionalidades

- **Arquitetura Cliente-Servidor**
  - Múltiplos clientes podem ligar-se em simultâneo ao controlador.
  - Comunicação feita através de **Named Pipes (FIFOs)**.

- **Gestão de Frota**
  - O controlador gere uma frota limitada de veículos.
  - Atribui serviços conforme a disponibilidade e o tempo simulado.

- **Simulação de Veículos**
  - Cada táxi é um **processo independente** (`fork`/`execve`).
  - Envia telemetria (estado da viagem) para o controlador através de um **pipe anónimo**.

- **Concorrência**
  - Uso de **POSIX Threads** no controlador (por exemplo, relógio e telemetria).
  - O cliente pode recorrer a **I/O multiplexado (`select`)** para manter a interface responsiva.

- **Gestão de Sinais**
  - Tratamento robusto de sinais (`SIGINT`, `SIGPIPE`, eventualmente `SIGUSR1`, `SIGHUP`).
  - Suporte a **encerramento gracioso** (*graceful shutdown*), evitando deixar FIFOs “pendurados”.

- **Configuração Dinâmica**
  - Tamanho da frota configurável através de variável de ambiente (por exemplo, `NVEICULOS`).
  - Valores por defeito definidos no código (ex.: 10 veículos).

---

## 🛠️ Tecnologias e Conceitos Usados

- **Linguagem C**
  - Standard C11/C17 com extensões GNU (flags tipo `-std=c11 -Wall -Wextra -pthread`).

- **Named Pipes (FIFOs)**
  - Canal de comunicação **Cliente ↔ Controlador**.
  - Ex.: FIFO global de pedidos `./tmp/taxi_pedidos` e FIFOs individuais por cliente.

- **Pipes Anónimos**
  - Canal de telemetria **Veículo → Controlador**.
  - Ligação do `stdout` do processo `veiculo` ao controlador.

- **Sinais (Signals)**
  - `SIGINT`, `SIGPIPE`, e sinais adicionais para controlo/encerramento.
  - Handlers específicos para evitar crash e permitir limpeza de recursos.

- **Multithreading**
  - `pthread` para:
    - Thread de relógio (tempo simulado).
    - Threads de leitura de telemetria dos veículos.
  - `pthread_mutex_t` para proteger estruturas partilhadas (serviços, frota).

- **I/O Multiplexing (opcional)**
  - `select()` no cliente para gerir, por exemplo:
    - Entrada do utilizador (stdin).
    - Mensagens recebidas do controlador.

---

## 🚀 Como Compilar e Executar

### Pré-requisitos

- **GCC** (GNU Compiler Collection)
- **make**
- Ambiente **Linux**

### 1. Compilar

Na pasta do projeto:

```bash
make
```

Serão gerados os seguintes executáveis:

- `controlador`
- `cliente`
- `veiculo`

### 2. Executar o Controlador

O controlador deve ser iniciado **em primeiro lugar**.

Opcionalmente, podes definir o número máximo de veículos via variável de ambiente (exemplo):

```bash
# Exemplo: limitar a frota a 5 veículos
export NVEICULOS=5

./controlador
```

Se não definires, será usado o valor por defeito (ex.: 10 veículos).

### 3. Executar Clientes

Em terminais separados, podes lançar múltiplos clientes.  
Cada cliente deve ter um **username** único:

```bash
./cliente joao
```

Noutro terminal:

```bash
./cliente ana
```

Cada cliente cria a sua própria FIFO de resposta dentro de `./tmp/`.

---

## 📖 Guia de Comandos

### 👤 Comandos do Cliente

Na consola do **cliente**, os comandos podem ser, por exemplo:

| Comando                          | Descrição                                                                | Exemplo                    |
|----------------------------------|--------------------------------------------------------------------------|----------------------------|
| `agendar <hora> <local> <dist>`  | Solicita uma viagem (hora em *ticks*/segundos, local, distância em km). | `agendar 5 ISEC 10`        |
| `entrar`                         | Simula a entrada no táxi (após notificação de chegada).                 | `entrar`                   |
| `consultar`                      | Lista o estado das viagens associadas ao utilizador.                    | `consultar`                |
| `cancelar <id>`                  | Cancela um serviço agendado com o ID indicado.                          | `cancelar 1`               |
| `sair`                           | Encerra o cliente (se possível, isto é, se não estiver em viagem).      | `sair`                     |
| `help` / `h`                     | Mostra a lista de comandos disponíveis.                                 | `help`                     |

> **Nota:** No código atual, podes começar com `agendar`, `sair` e `help`, e ir adicionando os restantes comandos (`consultar`, `cancelar`, `entrar`) à medida que implementas o protocolo.

---

### 👮 Comandos do Administrador (Controlador)

No terminal onde o **`./controlador`** está a correr, o administrador pode ter comandos próprios, por exemplo:

| Comando         | Descrição                                                                 |
|-----------------|---------------------------------------------------------------------------|
| `listar`        | Mostra todos os serviços agendados e respetivo estado.                    |
| `frota`         | Mostra veículos em movimento e percentagem de conclusão.                  |
| `utiliz`        | Lista utilizadores ligados e respetivos PIDs.                             |
| `km`            | Exibe o total de quilómetros percorridos pela frota.                      |
| `hora`          | Mostra o tempo simulado atual (em ticks).                                 |
| `cancelar <id>` | Força o cancelamento de um serviço (e pára o veículo associado).          |
| `terminar`      | Inicia o encerramento gracioso (recusa novos pedidos e encerra o sistema).|

> Por exemplo, ao receber `terminar`, o controlador pode colocar o sistema em modo “Sistema a encerrar. Novos pedidos serão rejeitados.” e esperar que os serviços em curso terminem.

---

## 📂 Estrutura do Projeto

Exemplo de organização típica:

```text
.
├── src/
│   ├── controlador.c    # Lógica do servidor, threads, gestão da frota e serviços
│   ├── cliente.c        # Interface de linha de comandos (comandos do utilizador)
│   ├── veiculo.c        # Processo filho que simula o táxi (telemetria)
│   └── modelo.c         # Implementação das estruturas de dados partilhadas
├── include/
│   ├── modelo.h         # Definição de Utilizador, Servico, Veiculo, variáveis globais
│   └── protocolo.h      # Protocolo de mensagens (Pedido, Resposta, enums de comandos)
├── tmp/                 # Diretório criado automaticamente para as FIFOs
├── Makefile             # Script de compilação
└── README.md            # Este ficheiro
```

---

## 📝 Autor

Trabalho realizado no âmbito da unidade curricular de **Sistemas Operativos**.

Este simulador tem como objetivo praticar:

- comunicação interprocessos (IPC),
- sincronização e concorrência,
- gestão de processos e sinais,

num cenário mais próximo de uma aplicação real (gestão de táxis/autocarros/serviços distribuídos), mas simplificado para fins académicos.
