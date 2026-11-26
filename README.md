# Taxi Fleet Simulator

Simulation of a platform that manages a fleet of autonomous taxis, written in **C for Linux** using operating system mechanisms: IPC, concurrency and signals.
Developed for **Operating Systems** at ISEC - Polytechnic University of Coimbra (2025/26).

## Architecture

| Program | Role |
|---|---|
| `controlador` | Central server: manages the fleet, schedules trips and tracks simulated time |
| `cliente` | User console: books, checks and cancels trips (many clients at once) |
| `veiculo` | One process per taxi, launched by the controller, sending trip telemetry |

## Features

- Client-server communication through **named pipes (FIFOs)**: one shared request FIFO plus one reply FIFO per client
- Each vehicle is its own process (**fork / execve**), sending telemetry to the controller through an **anonymous pipe**
- **POSIX threads** in the controller (simulated clock, telemetry readers) with **mutexes** protecting shared data
- **I/O multiplexing** with `select()` so the client stays responsive
- **Signal handling** (SIGINT, SIGPIPE, SIGUSR1...) with graceful shutdown that cleans up FIFOs
- Fleet size set through the `NVEICULOS` environment variable (default: 10)

## Tech

C11, Linux, POSIX (FIFOs, pipes, fork/exec, pthreads, signals, select), Make

## Build and run

```
make
./controlador          # optional: export NVEICULOS=5 first
./cliente <username>   # in other terminals, one per user
```

Client commands: `agendar <time> <place> <km>`, `consultar`, `cancelar <id>`, `entrar`, `sair`, `help`.
Admin commands (controller): `listar`, `frota`, `utiliz`, `km`, `hora`, `cancelar <id>`, `terminar`.

## Authors

- Rodrigo Costa ([@rodrigo2024134442](https://github.com/rodrigo2024134442))
- [@gilberto2017068731](https://github.com/gilberto2017068731)