// src/veiculo.c
#include <unistd.h> // write, sleep
#include <string.h> // strlen
#include <stdlib.h> // exit
#include <stdio.h>

#include "modelo.h"

static void send_msg(const char *s)
{
    // escreve a string no stdout
    write(STDOUT_FILENO, s, strlen(s));
}

int main(int argc, char **argv)
{
    // por agora, ignoramos os argumentos
    (void)argc;
    (void)argv;

    send_msg("TESTE\n");
    sleep(1);

    char buffer[20]; // Buffer para construir a mensagem

    // Loop de 10 a 100, incrementando de 10 em 10
    for (int perc = 10; perc <= 90; perc += 10)
    {
        // Formata a string "PERC XX\n" onde XX é o valor de 'perc'
        sprintf(buffer, "PERC %d\n", perc);

        // Envia a mensagem
        send_msg(buffer);

        // Espera 1 segundo
        sleep(1);
    }

    send_msg("FIM OK\n");

    // termina normalmente
    return 0;
}
