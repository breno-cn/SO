#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <semaphore.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <bits/sigaction.h>
#include "message.h"

#define LIMIT 10
#define MAX_ID 99                                                   // Número máximo de processos que podem utilizar a biblioteca simultaneamente
#define MEM_SZ 4*1024*1024
#define BUFF_SZ 4096


typedef struct Message {
    int sender;                                                     // ID do Transmissor
    int receiver;                                                   // ID do receptor
    char content[30];                                               // Mensagem, em formato de string
} message;

typedef struct Msg_queue {
    int IF, FF;                                                     // Incício e fim da fila
    message buffer[BUFF_SZ];					                    // Vetor que implementa a fila de mensagens
    message async_buff;                                             // Buffer para receber mensagens asíncronas
    pid_t used_id[MAX_ID];                                          // ID's usados pelos processos, 0 se estiver vazio, pid caso contrário
    sem_t sem[MAX_ID];                                              // Lista de semáforos para cada processo, usado mas mensagens síncronas
    sem_t critical_sync;					                        // Semáforo usado para controlar acesso à regiões  síncronas
    sem_t critical_async;                                           // Semáforo das regiões asíncronas
} msg_queue;

message local_async_buff;                                           // Buffer local para receber mensagens asíncronas
msg_queue *msg;                                                     // Área compartilhada
msg_queue *msg_ptr = NULL;                                          // Ponteiro para shmem
int shmid;                                                          // ID da shmem
key_t key;                                                          // Chave de acesso
int first_init = 1;                                                 // Verifica se é a primeira inicialização, para não resetar dados da lib
int id;                                                             // ID do processo para enviar e receber mensagens
struct sigaction action;

void sig_handler(int signo) {
    local_async_buff.sender = msg_ptr->async_buff.sender;
    local_async_buff.receiver = msg_ptr->async_buff.receiver;
    strcpy(local_async_buff.content, msg_ptr->async_buff.content);
    sem_post(&(msg_ptr->critical_async)); 
}

int process_exist(pid_t pid) {
    for (int i = 0; i < MAX_ID; i++) {
        if (pid == msg_ptr->used_id[i])
            return 1;
    }
    return 0;
}

int process_awake(pid_t pid) {
    return kill(pid, 0) == 0;
}

int msg_init(int _id) {
    if (_id < 0 || _id > MAX_ID) {
        printf("ID invalido!\n");
        return 0;
    }
    
    // Inicializa a fila
    msg = NULL;

    key = 12345;           // Chave para acesso
    shmid = shmget(key, MEM_SZ, 0666|IPC_CREAT);
    if (shmid == -1) {
        printf("Falha na criação da area compartilhada!\n");
        return 0;
    }
    printf("shmid = %d\n", shmid);

    msg = shmat(shmid, NULL, 0);
    if (msg == (void *) -1) {
        printf("Alocacao do recurso falhou!\n");
        return 0;
    }

    // Inicialização do ponteiro
    msg_ptr = (msg_queue *) msg;
    if (first_init) {
		first_init = 0;
        printf("Primeira inicializacao...\n");
        msg_ptr->IF = 0;
        msg_ptr->FF = 0;

        // Inicialização dos semáforos e da tabela de processos
		printf("Incializacao dos semaforos...\n");
        for (int i = 0; i < MAX_ID; i++) {
            sem_init(&msg_ptr->sem[i], 1, 0);
            msg_ptr->used_id[i] = 0;
        }
		sem_init(&msg_ptr->critical_sync, 1, 1);
        sem_init(&msg_ptr->critical_async, 1, 1);
        sem_init(&msg_ptr->critial_awake, 1, 1);

        memset(&action, '\0', sizeof(action));
        action.sa_handler = &sig_handler;
        action.sa_flags = SA_SIGINFO;
        if (sigaction(SIGUSR1, &action, NULL) < 0) {
            printf("Erro na instalacao do sinal...\n");
            msg_exit();
            return 0;
        }
    }
    
    if (msg_ptr->used_id[_id] != 0) {
        printf("ID ja esta em uso!\n");
        msg_exit();
        return 0;
    }
    id = _id;
    msg_ptr->used_id[id] = getpid();
    local_async_buff.receiver = -1;                                                       // Inicializa mensagem asincrona
    return 1;
}

int msg_exit(void) {
    
    // Libera ID para ser usado novamente...
    msg_ptr->used_id[id] = 0;

    shmdt(msg_ptr);
    shmctl(shmid, IPC_RMID, NULL);
    printf("Recursos liberados...\n");
    return 1;   
}

int msg_full(void) {
    return msg_ptr->IF == (msg_ptr->FF + 1) % BUFF_SZ;
}

int msg_empty(void) {
    return msg_ptr->IF == msg_ptr->FF;
}

int sendA(int receiver, char *content) {
    if (!process_exist(msg_ptr->used_id[receiver])) {
        printf("O processo receptor nao existe!\n");
        return 0;
    }

    int i = 0;
    sem_wait(&(msg_ptr->critical_async));

    if (strcpy(msg_ptr->async_buff.content, content) == NULL) {
        sem_post(&(msg_ptr->critical_async));
        return 0;
    }

    msg_ptr->async_buff.sender = id;
    msg_ptr->async_buff.receiver = receiver;
    printf("Enviando sinal do processo %d para %d\n", getpid(), msg_ptr->used_id[receiver]);
    kill(msg_ptr->used_id[receiver], SIGUSR1);
    return 1;
}

int receiveA(int *sender, char *content) {
    if (local_async_buff.receiver != -1) {
        *sender = local_async_buff.sender;
        strcpy(content, local_async_buff.content);
        local_async_buff.receiver = -1;
        return 1;
    }

    return 0;
}

int sendS(int receiver, char *content) {

    if (msg_full() || !process_exist(msg_ptr->used_id[receiver])) {
        printf("Fila de mensagens cheia ou processo receptor nao existe!\n");
        return 0;
    }
	
    sem_wait(&(msg_ptr->critical_sync));

    msg_ptr->buffer[msg_ptr->FF].sender = id;
    msg_ptr->buffer[msg_ptr->FF].receiver = receiver;
    strcpy(msg_ptr->buffer[msg_ptr->FF].content, content);
    msg_ptr->FF = (msg_ptr->FF + 1) % BUFF_SZ;
	
    sem_post(&(msg_ptr->critical_sync));

    sem_wait(&(msg_ptr->sem[id]));

    return 1;
}

int receiveS(int *sender, char *content) {
    if (msg_empty()) {
        printf("Fila de mensagens esta vazia!\n");
        return 0;
    }

    while (msg_ptr->buffer[msg_ptr->IF].receiver != id);

    *sender = msg_ptr->buffer[msg_ptr->IF].sender;
    strcpy(content, msg_ptr->buffer[msg_ptr->IF].content);
    msg_ptr->IF = (msg_ptr->IF + 1) % BUFF_SZ;
	
    sem_post(&(msg_ptr->critical_sync));

    sem_post(&(msg_ptr->sem[*sender]));
    
    return 1;
}
