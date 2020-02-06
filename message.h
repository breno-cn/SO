void sig_handler(int signo);
int msg_init(int _id);
int msg_exit(void);
int msg_full(void);
int msg_empty(void);
int sendA(int receiver, char *content);
int receiveA(int *sender, char *content);
int sendS(int receiver, char *content);
int receiveS(int *sender, char *content);
