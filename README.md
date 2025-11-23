# 🚖 Sistema de Gestão de Táxis (Simulador)

Este projeto consiste na simulação de uma plataforma de gestão de uma frota de táxis autónomos, desenvolvido em **Linguagem C** para ambiente **Linux**.

O sistema utiliza mecanismos avançados de **Sistemas Operativos** (IPC, Concorrência, Sinais) para gerir a comunicação entre múltiplos clientes, um controlador central e processos independentes para cada veículo.

## 📋 Funcionalidades

* **Arquitetura Cliente-Servidor:** Múltiplos clientes podem ligar-se simultaneamente via *Named Pipes* (FIFOs).
* **Gestão de Frota:** O controlador gere uma frota limitada de veículos, atribuindo serviços conforme a disponibilidade.
* **Simulação de Veículos:** Cada táxi é um processo independente (`fork`/`exec`) que reporta telemetria em tempo real.
* **Concorrência:** Uso de **Threads** (POSIX Threads) no servidor e **I/O Multiplexing** (`select`) no cliente para operações não bloqueantes.
* **Gestão de Sinais:** Tratamento robusto de sinais (`SIGINT`, `SIGPIPE`, `SIGHUP`) para garantir estabilidade e encerramento gracioso (*Graceful Shutdown*).
* **Configuração Dinâmica:** Tamanho da frota configurável via variável de ambiente.

## 🛠️ Tecnologias e Conceitos Usados

* **Linguagem C** (Standard C11/C17 com extensões GNU)
* **Named Pipes (FIFOs):** Comunicação Cliente ↔ Controlador.
* **Anonymous Pipes:** Telemetria Veículo → Controlador.
* **Sinais (Signals):** Controlo de fluxo (`SIGUSR1`, `SIGINT`).
* **Multithreading:** `pthread` e `mutex` para gestão concorrente no servidor.
* **I/O Multiplexing:** `select()` para interface de cliente responsiva.

---

## 🚀 Como Compilar e Executar

### Pré-requisitos
* GCC (GNU Compiler Collection)
* Make
* Ambiente Linux

### 1. Compilação
O projeto inclui um `Makefile`. Para compilar todos os componentes, basta executar na raiz do projeto:

```bash
make
