#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define TKN_NORMAL      0
#define TKN_REDIR_IN    1
#define TKN_REDIR_OUT   2
#define TKN_PIPE        3
#define TKN_BG          4
#define TKN_EOL         5
#define TKN_EOF         6
#define TKN_NONE        7
#define TKN_ERR         8

#define CLEN 16
#define TKN_LEN 50
#define CONT_LEN 16
#define NARGS 16

struct token_container {
    char *token;
    int token_type;
} tkn_cont[CONT_LEN+1];

void init_tkn_cont(struct token_container *p);
int get_last_token_type(struct token_container *p);
void print_tkn_cont(struct token_container *p);
void print_av(char *av[]);
void close_all_pfd(int pfd[][2], int ax1len);
int count_pipe_num(struct token_container *p);
int getargs(int *ac, char *av[], char *lbuf);
int gettoken(char *token, int len);
void sighandle(int signal);

int main() {
    int ac;
    char *av[NARGS+1];
    char *fname_av[NARGS+1];
    char lbuf[CLEN];
    char dlm[] = " \n";
    char *tok;

    int pid;
    int ret;
    int status;
    extern char **environ;

    int pfd[CONT_LEN][2];
    int tty_fd = open("/dev/tty", O_RDWR);

    char prompt[TKN_LEN] = {0};

    strcpy(prompt, "mysh$");
    signal(SIGINT, SIG_IGN);
    signal(SIGCHLD, sighandle);
    while (true) {
        struct token_container *p;
        int i, j, cmd_idx, cmd_num, token_type, last_token_type, pipe_num;
        char *first_token[TKN_LEN] = {0};
        int has_bg = false;

        printf("%s ", prompt);

        init_tkn_cont(tkn_cont);
        i = 0;
        p = tkn_cont;
        while (true) {
            for (; ; i++, p++) {
                if (i > CONT_LEN-1) {
                    p--;
                    p->token_type = TKN_ERR;
                    fprintf(stderr, "Too many tokens\n");
                    break;
                }
                p->token = (char *)malloc(sizeof(char)*TKN_LEN);
                token_type = gettoken(p->token, TKN_LEN); 
                if (token_type == TKN_EOF) {
                    fprintf(stderr, "EOF\n");
                    return 0;
                } else if (token_type == TKN_EOL) {
                    free(p->token);
                    p->token = NULL;
                    break;
                } else {
                    p->token_type = token_type;
                }
            }
            last_token_type = get_last_token_type(tkn_cont);
            if (last_token_type == TKN_REDIR_IN
                    || last_token_type == TKN_REDIR_OUT
                    || last_token_type == TKN_PIPE) {
                printf("> ");
                continue;
            } else {
                break;
            }
        }
        if (last_token_type == TKN_NONE || last_token_type == TKN_ERR) {
            continue;   // wait for user's input again
        }

//         print_tkn_cont(tkn_cont);   // DEBUG

        // init pfd
        for (i = 0; i < CONT_LEN; i++) {
            pipe(pfd[i]);
        }

        cmd_num = count_pipe_num(tkn_cont) + 1;
        for (cmd_idx = 0, p = tkn_cont; cmd_idx < cmd_num; cmd_idx++) {
            int fd;
            char fname[TKN_LEN] = {0};
            int has_pipe_in = false;
            int has_pipe_out = false;
            int has_redir_in = false;
            int has_redir_out = false;
            int pid = -1;
            has_bg = false;
            struct token_container *tmp_p;

            if (p->token_type != TKN_NORMAL) {
                fprintf(stderr, "Invalid input\n");
                break;
            }
            if(getargs(&ac, av, p->token) == 1) {
                break;
            }
//             print_av(av);   // DEBUG

            // special commands
            if (strcmp(av[0], "exit") == 0) {
                exit(0);      // exit mysh
            } else if (strcmp(av[0], "cd") == 0) {
                if (chdir(av[1]) < 0) {
                    fprintf(stderr, "chdir error\n");
                }
                break;
            } else if (strcmp(av[0], "chprompt") == 0) {
                if (av[1] != NULL) {
                    strcpy(prompt, av[1]);
                }
                break;
            }


            tmp_p = p;
            if (cmd_idx != 0 && (--tmp_p)->token_type == TKN_PIPE) {
                has_pipe_in = true;
            }
            tmp_p = p + 1;
            if (tmp_p->token_type == TKN_PIPE) {
                has_pipe_out = true;
            } else if (tmp_p->token_type == TKN_REDIR_IN) {
                if(getargs(&ac, fname_av, (++tmp_p)->token) == 1) {
                    fprintf(stderr, "filename parsing error.\n");
                    break;
                }
                strcpy(fname, fname_av[0]);
                has_redir_in = true;
                if ((++tmp_p)->token_type == TKN_PIPE) {
                    has_pipe_out = true;
                }
            } else if (tmp_p->token_type == TKN_REDIR_OUT) {
                if(getargs(&ac, fname_av, (++tmp_p)->token) == 1) {
                    fprintf(stderr, "filename parsing error.\n");
                    break;
                }
                strcpy(fname, fname_av[0]);
                has_redir_out = true;
            } else if (tmp_p->token_type == TKN_BG) {
                has_bg = true;
            }


            // child process
            if ((pid = fork()) == 0) {
                signal(SIGINT, SIG_DFL);
                if (has_redir_out) {
                    fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    close(1);
                    dup(fd);
                    close(fd);
                }
                if (has_redir_in) {
                    fd = open(fname, O_RDONLY, 0644);
                    close(0);
                    dup(fd);
                    close(fd);
                }

                if (has_pipe_in & has_pipe_out) {
                    // pipe_in_out
                    close(0);
                    dup(pfd[cmd_idx-1][0]);
                    close(1);
                    dup(pfd[cmd_idx][1]);
                    close_all_pfd(pfd, CONT_LEN);
                } else if (has_pipe_in && !has_pipe_out) {
                    // pipe_in
                    close(0);
                    dup(pfd[cmd_idx-1][0]);
                    close_all_pfd(pfd, CONT_LEN);
                } else if (!has_pipe_in && has_pipe_out) {
                    // pipe_out
                    close(1);
                    dup(pfd[cmd_idx][1]);
                    close_all_pfd(pfd, CONT_LEN);
                } else if (!(has_pipe_in || has_pipe_out)) {
                    // normal
                }

                if(execvp(av[0], av) < 0) {
                    fprintf(stderr, "execvp error\n");
                    break;
                }
            }

            // parent process
//             if (tcsetpgrp(tty_fd, pid) < 0) {
//                 fprintf(stderr, "setting process group ID error\n");
//             }
            if (has_redir_in && has_pipe_out) {
                p += 4;
            } else if (has_pipe_out) {
                p += 2;
            }
            if (p->token == NULL) {
                break;
            } 
        }

        // parent process
        pipe_num = count_pipe_num(tkn_cont);
        for (j = 0; j < pipe_num + 1; j++) {
            close_all_pfd(pfd, CONT_LEN);
            if (!(j == pipe_num-1 && has_bg)) {
                wait(&status);
            }
        }
    }
    return 0;
}

void init_tkn_cont(struct token_container *p) {
    int i;
    for (i = 0; i < CONT_LEN+1; i++, p++) {
        free(p->token);
        p->token = NULL;
        p->token_type = TKN_NONE;
    }
    return;
}

int get_last_token_type(struct token_container *p) {
    int token_type = TKN_NONE;
    for (; p->token != NULL; p++) {
        token_type = p->token_type;
    }
    return token_type;
}

void print_tkn_cont(struct token_container *p) {
    int i;
    for (i = 0; p->token != NULL; i++, p++) {
        printf("DEBUG: ");
        switch (p->token_type) {
            case TKN_NORMAL:
                printf("TKN_NORMAL:    %s\n", p->token);
                break;
            case TKN_REDIR_IN:
                printf("TKN_REDIR_IN:  %s\n", p->token);
                break;
            case TKN_REDIR_OUT:
                printf("TKN_REDIR_OUT: %s\n", p->token);
                break;
            case TKN_PIPE:
                printf("TKN_PIPE:      %s\n", p->token);
                break;
            case TKN_BG:
                printf("TKN_BG:        %s\n", p->token);
                break;
            case TKN_EOL:
                printf("TKN_EOL\n");
                break;
            case TKN_NONE:
                printf("TKN_NONE\n");
                break;
            default:
                printf("other token\n");
                break;
        }
    }
    printf("DEBUG: %d tokens\n", i);
    return;
}

void print_av(char *av[]) {
    int i;
    for (i = 0; av[i] != NULL; i++) {
        printf("DEBUG: av%d: %s\n", i, av[i]);
    }
    return;
}

void close_all_pfd(int pfd[][2], int ax1len) {
    int i;
    for (i = 0; i < ax1len; i++) {
        close(pfd[i][0]);
        close(pfd[i][1]);
    }
    return;
}

int count_pipe_num(struct token_container *p) {
    int pipe_num = 0;
    for (; p->token != NULL ; p++) {
        if (p->token_type == TKN_PIPE) {
            pipe_num++;
        }
    }
    return pipe_num;
}

int getargs(int *ac, char *av[], char *lbuf) {
    char dlm[] = " ";
    char *tok;

    for (*ac = 0, tok = strtok(lbuf, dlm); tok != NULL; (*ac)++, tok = strtok(NULL, dlm)) {
        if (*ac > NARGS-1) {
            fprintf(stderr, "Too many arguments.\n");
            return 1;
        }
        av[*ac] = tok;
    }
    av[*ac] = NULL;
    return 0;
}

int gettoken(char *token, int len) {
    int i, c;
    int last_token_type = TKN_NONE;
    int seen_non_space_char = false;

    for (i = 0; ; i++) {
        if (i > len-1) {
            fprintf(stderr, "Too long input.\n");
//             while (c != EOF) {
//                 c = getc(stdin);
//             }
            return TKN_ERR;
        }
        c = getc(stdin);
        switch (c) {
            case '<':
                if (last_token_type == TKN_NORMAL) {
                    ungetc(c, stdin);
                    token[i] = '\0';
                    return TKN_NORMAL;
                } else {
                    strcpy(token, "<");
                    return TKN_REDIR_IN;
                }

            case '>':
                if (last_token_type == TKN_NORMAL) {
                    ungetc(c, stdin);
                    token[i] = '\0';
                    return TKN_NORMAL;
                } else {
                    strcpy(token, ">");
                    return TKN_REDIR_OUT;
                }

            case '|':
                if (last_token_type == TKN_NORMAL) {
                    ungetc(c, stdin);
                    token[i] = '\0';
                    return TKN_NORMAL;
                } else {
                    strcpy(token, "|");
                    return TKN_PIPE;
                }

            case '&':
                if (last_token_type == TKN_NORMAL) {
                    ungetc(c, stdin);
                    token[i] = '\0';
                    return TKN_NORMAL;
                } else {
                    strcpy(token, "&");
                    return TKN_BG;
                }

            case '\n':
                if (last_token_type == TKN_NORMAL) {
                    ungetc(c, stdin);
                    token[i] = '\0';
                    return TKN_NORMAL;
                } else {
                    strcpy(token, "\n");
                    return TKN_EOL;
                }

            case EOF:
                if (last_token_type == TKN_NORMAL) {
                    ungetc(c, stdin);
                    token[i] = '\0';
                    return TKN_NORMAL;
                } else {
                    free(token);
                    token = NULL;
                    return TKN_EOF;
                }

            default:
                if ((c == ' ' || c == '\t') && !seen_non_space_char) {
                    i--;    // discard input and continue
                } else {
                    seen_non_space_char = true;
                    token[i] = c;
                    last_token_type = TKN_NORMAL;
                }
//                 break;
        }
    }
}

void sighandle(int signal) {
    int state;
    if (signal == SIGCHLD) {
        waitpid(-1, &state, WNOHANG);
    }
    return;
}
