/**
	@file main.c
   
	Ficheiro com a nossa implementação do processador de Notebooks descrito no enunciado.
*/

#include <fcntl.h> 
#include <unistd.h> 
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/stat.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>


// Como cada processo tem dois pipes, assim sabemos qual o seu indice
#define PIPE_READ(x) ((x)*2)
#define PIPE_WRITE(x) ((x)*2 + 1)

/** Path para a pasta temporária com os ficheiros auxiliares */
#define TMP_PATH "/tmp/notebook/"
/** String com o delimitador superior dos resultados */
#define SUP_DELIM ">>>"
/** String com o delimitador inferior dos resultados */
#define INF_DELIM "<<<"

/** 
* @brief Estrutura para o parser do ficheiro
*/
typedef struct cmd{
	char* text; /**< Texto do ficheiro antes de cada comando */
	char** cmd; /**< Comando */ 
	char* full_cmd; /**< Linha de comando tal como está no ficheiro .nb */
	int* needs_me; /**< Lista de comandos que precisam do output deste comando */
	int needs_me_len; /**< Número de comandos que precisam do output deste comando */
	int my_input_id; /**< ID do comando para o input deste */
}*Comando;


/**
* @brief Função para inicializar uma variavel do tipo cmd
*
* @return nova estrutura Comando
*/
Comando initComando(){
	Comando new = malloc(sizeof(struct cmd));
	new->text = NULL;
	new->cmd = NULL;
	new->full_cmd = NULL;
	new->needs_me = NULL;
	new->needs_me_len = 0;
	new->my_input_id = -1;

	return new;
}


/** Variável global com os pids dos filhos */
int* son_pids = NULL;
/** Número de filhos inicial */
int son_pids_len = 0;
/** Descritor do ficheiro do stderr para o pai o fechar */
int des_error;



/**
* @brief Função que cria um filho que verifica se ocorreu algum erro durante a execução do programa
*/
int* criaFilhoSentinela(){
	int n_read;
	char aux;

	int f_error = open("/tmp/notebook/error.txt", O_CREAT | O_RDWR, 0666);
	if(f_error<0) exit(-1);
	
	des_error = f_error;

	dup2(f_error, 2);
	
	int check_error = fork();

	if(check_error == 0){
		while((n_read = read(f_error, &aux, 1)) == 0);

		if(n_read > 0){ // Quando conseguiu ler algo no ficheiro
			kill(getppid(), SIGINT); 
			exit(0);
		} 
		else exit(0);
	}

	int* result = malloc(sizeof(int) * 2);
	result[0] = check_error;
	result[1] = f_error;
	
	return result;
}

/**
* @brief Função que remove os ficheiros auxiliares criados durante a execução do programa
* 
* @param n_cmds Número total de comandos
*/
void removeFicheirosAuxiliares(int n_cmds){
	for(int i = 0; i < n_cmds; i++){
		char output_file[100];
		sprintf(output_file, "%scomando%d.txt", TMP_PATH, i);
		unlink(output_file);
	}
	unlink("/tmp/notebook/error.txt");
	unlink("resultado.nb"); // no caso de esta funcao ser usada pela interrupt este ficheiro deve ser eliminado
}

	
/**
* @brief Função que permite cancelar o programa através do Ctrl-C ou ocorrencia de erro
*/
void interrupt(int x){
	for(int i = 0; i < son_pids_len; i++){
		kill(son_pids[i], SIGKILL);
	}
	removeFicheirosAuxiliares(son_pids_len);
	exit(-1);
}


/**
* @brief Função que duplica uma string para outra nova
*
* @param string String a duplicar 
* @param n_chars Número de caracteres da String
*
* @return Nova string duplicada
*/

char* mystrndup(char* string, int n_chars){
  char* result;
  int len = strlen(string);

  if(n_chars < len) len = n_chars;

  result = malloc(len + 1);
  memcpy (result, string, len);
  result[len] = '\0';

  return result;
}


/**
* @brief Funcao para ver se o processo que faz redirecionamento do output pode fechar um descritor ou nao
* Funciona como um elem, isto é, verifica se um determinado N está contido numa lista de N's
*
* @param needs_me Lista de Comandos que precisam do output de comando em questão
* @param needs_me_len Tamanho da lista needs_me
* @param n ID do comando a verificar
*
* @return 0 se puder fechar (o ID não está na lista), 1 caso contrário
*/
int elem(int* needs_me, int needs_me_len, int n){
	for(int i=0; i<needs_me_len; i++){
		if(n == needs_me[i]) return 1;
	}

    return 0;
}



// AUX PARSER



/**
* @brief Função que permite verificar qual o ID do comando de que certo comando precisa
*
* @param str Linha de comando a analisar
*
* @return ID do comando de que depende
*/

int getDependentNumber(char* str){
	char* aux = malloc(strlen(str));
	int i;

	if(str[1] == ' ') return -1; // Para diferenciar do comando nº 0
	if(str[1] == '|') return 1;  
	
	for(i = 1;  str[i] != '|'; i++){
		aux[i-1] = str[i];
	}
	aux[i] = '\0';

	return atoi(aux);
}


/**
* @brief Função que permite analisar uma linha de código, tratando os argumentos
*
* @param cmd_line Linha de comando a analisar
*
* @return Lista de Strings com o comando e os seus argumentos separados
*/
char** transformCmdLine(char* cmd_line){
	int n_args = 0, n, i , j, first_alpha;

	for(i = 0; !isalpha(cmd_line[i]); i++);
	
	first_alpha = i;	
		
	for(i = 0; cmd_line[i]!='\0'; i++){					
		if(cmd_line[i] == ' ') n_args++;
	}

	char** cmd_args = malloc(sizeof(char*) * (n_args+1));
	cmd_args[n_args] = NULL; // terminar o array a null para efeitos de exec
	
	char* token = strtok (cmd_line + first_alpha," ");
	i=0;
  	while(token != NULL){
   		cmd_args[i++] = strdup(token);
    	token = strtok (NULL, " ");
  	}

  	return cmd_args;
}

/**
* @brief Função que permite adicionar um inteiro a um array
*
* @param v Array
* @param n Tamanho do Array
* @param x Elemento que se pretende introduzir no Array
*
* @return Array com o inteiro já inserido
*/
int* addToArray(int* v, int n, int x){
	int* aux = malloc((n+1)*sizeof(int));

	for(int i = 0; i < n; i++){
		aux[i] = v[i];
	}

	aux[n] = x;
	
	return aux;
}



// PARSER



/**
* @brief Função que faz o parser do ficheiro, carregando os dados para a estrutura por nós definida
*
* @param cmds Apontador para a estrutura para a qual queremos carregar a informação
* @param file String com o conteúdo do ficheiro
* @param fileSize Tamanho do ficheiro (em Bytes)
*/
void loadCmds(Comando* cmds, char* file, int fileSize){
	int cmd_id = 0;
	int i = 0;
	char* line = malloc(sizeof(char)*fileSize);

	while(file[i] != '\0'){
		// Analisa linha a linha - Sabemos que estamos a iniciar uma linha nova no inicio do ciclo	
		int j;

		// Lê uma linha
		for(j = 0; file[i] != '\0' && file[i] != '\n'; i++, j++) line[j] = file[i];
		i++; // para avancar \n

		if(j==0) break;
		line[j] = '\0';			


		// É texto
		if((line[0]) != '$'){ 
			// Se tiver encontrado um resultado deve ignorar
			if(!strcmp(line, SUP_DELIM)){
				do{ 
					j=0; 
					while(file[i] != '\0' && file[i] != '\n') line[j++] = file[i++];
					line[j]='\0'; i++;
				}while(strcmp(line, INF_DELIM) != 0 && i < fileSize);

			}
			else{ // Caso tenha encontrado uma linha de texto guarda-a
				if(cmds[cmd_id]->text != NULL){
					char* new_line = malloc(strlen(cmds[cmd_id]->text) + strlen(line) + 3);
					strcat(new_line, cmds[cmd_id]->text);
					strcat(new_line, "\n");
					strcat(new_line, line);

					cmds[cmd_id]->text = new_line;
				}
				else cmds[cmd_id]->text = strdup(line);
				
			}
		}
		// É comando
		else if(line[0] == '$'){ 
			cmds[cmd_id]->full_cmd = strdup(line);			
			cmds[cmd_id]->cmd = transformCmdLine(line);
			
			int x = getDependentNumber(line);
			if(x >= 0) cmds[cmd_id]->my_input_id = cmd_id - x;

			if(x != -1){
				cmds[cmd_id - x]->needs_me = addToArray(cmds[cmd_id - x]->needs_me, cmds[cmd_id - x]->needs_me_len, cmd_id);
				cmds[cmd_id - x]->needs_me_len++;
			}
			cmd_id++;
		}
	} 
}


/**
* @brief Função que nos permite saber qual o número de comandos existente num ficheiro
*
* @param file String com todo o conteúdo do ficheiro
*
* @return Número de comandos existente
*/
int getNumOfCmds(char* file){
	char buffer[50]; 
	int id1, id2, pd1[2], pd2[2], numCmds;

	pipe(pd1);
	pipe(pd2);

	id1 = fork();

	if(id1==0){
		dup2(pd1[1], 1); // Redirecionar stdout para pipe escrita 

		close(pd1[0]);
		close(pd2[1]);
		close(pd2[0]);

		execlp("fgrep", "fgrep", "$", file, NULL);
		_exit(-1);
	}
	else{
		id2 = fork();

		if(id2 == 0){
			dup2(pd1[0], 0); // le resultado da execucao do outro filho
			dup2(pd2[1], 1); // manda resultado ao pai

			close(pd1[1]);
			close(pd2[0]);

			execlp("wc", "wc", "-l", NULL);
			_exit(-1);
		}
	}

	close(pd1[0]);
	close(pd1[1]);
	close(pd2[1]);

	for(int i = 0; i < 2; i++){
		wait(NULL);
	}

	int n_read = read(pd2[0], buffer, 50);

	if(n_read < 0){
		printf("Erro\n");
	}

	close(pd2[0]);

	sscanf(buffer, "%d", &numCmds);

	return numCmds;
}


// MAIN



/**
* @brief Função main que realiza o parser do ficheiro e o seu processamento
*
* @param argc Número de argumentos
*
* @return argv Lista de argumentos
*/  
int main (int argc, char* argv[]){
	signal(SIGINT, interrupt);

	if(argc == 1){
		printf("Especifique um ficheiro\n");
		return -1;
	}

	int nFilhoExec = 0, nFilhosCriados = 0;
	char *path = argv[1];	
	char *buffer;
	int n_cmds = getNumOfCmds(argv[1]);

	// Inicializa estrutura com size = n_cmds
	Comando cmds[n_cmds];
	for(int i=0; i<n_cmds; i++)
		cmds[i] = initComando();


	// Abrir ficheiro
	int file = open(path, O_RDWR); //abrir ficheiro
	int fileSize = 0;

	if(file < 0){
		printf("Não existe o ficheiro\n");
		return -1;
	}

	// Ver tamanho do ficheiro
	struct stat st; 
	if (stat(path, &st) != -1) {
		fileSize = st.st_size; // em bytes
		buffer = malloc(fileSize+1);
	}

	// Carregar ficheiro para string Buffer
	while(read(file, buffer, fileSize) > 0);
	buffer[fileSize] = '\0';

	close(file);	

	int* error = criaFilhoSentinela();
	int check_error = error[0];
	int f_error = error[1];

	// Parser do ficheiro para a estrutura
	loadCmds(cmds, buffer, fileSize);

	// Cria 2*n_cmds pipes
	int pipeArray[n_cmds*2][2];
	for(int i=0; i < n_cmds*2; i++) pipe(pipeArray[i]);
	
	// Inicia execução
	for(nFilhoExec = 0; nFilhoExec < n_cmds; nFilhoExec++){
		Comando to_exec = cmds[nFilhoExec];
		
		if(!to_exec->cmd) break; 
		
		int id = fork();

		// Adiciona o pid do filho ao array global que guarda todos os pids dos filhos
		son_pids = addToArray(son_pids, son_pids_len, id);
		son_pids_len++;

		if(id == 0){
			for(int i=0; i<n_cmds; i++){
				close(pipeArray[PIPE_READ(i)][1]);
				close(pipeArray[PIPE_WRITE(i)][0]);

				if(i != nFilhoExec){ 
					close(pipeArray[PIPE_READ(i)][0]);
					close(pipeArray[PIPE_WRITE(i)][1]);
				}					
			}

			if(to_exec->my_input_id == -1) // teste se precisa de resultado de outro comando
				close(pipeArray[PIPE_READ(nFilhoExec)][0]);  // se nao precisar fecha
			else 
				dup2(pipeArray[PIPE_READ(nFilhoExec)][0], 0);

			if ((to_exec->needs_me_len) == 0) {
				char output_file[100];
				sprintf(output_file, "%scomando%d.txt", TMP_PATH, nFilhoExec);
				int output_des = open(output_file, O_RDWR | O_CREAT, 0666);

				dup2(output_des, 1);

				close(pipeArray[PIPE_WRITE(nFilhoExec)][1]);
			}

			else dup2(pipeArray[PIPE_WRITE(nFilhoExec)][1], 1);

			execvp(to_exec->cmd[0], to_exec->cmd);
			exit(-1);
		}
		else{
			nFilhosCriados++;
			if (to_exec->needs_me_len > 0){
				id = fork();
				if(id == 0){
					char output_file[100];
					sprintf(output_file, "%scomando%d.txt", TMP_PATH, nFilhoExec);
					int output_des = open(output_file, O_RDWR | O_CREAT, 0666);	
					int to_send, n_read;
					char* result_buffer = malloc(1024 * sizeof(char));
					
					for(int i=0; i<n_cmds; i++){
						if(i != nFilhoExec) close(pipeArray[PIPE_WRITE(i)][0]);							
						if(i != nFilhoExec && elem(to_exec->needs_me, to_exec->needs_me_len, i) == 0)						
							close(pipeArray[PIPE_READ(i)][1]); // fechar descritor de escrita porque nao vai ser preciso enviar resultado a este
					
						close(pipeArray[PIPE_WRITE(i)][1]);
						close(pipeArray[PIPE_READ(i)][0]);

					}
					close(pipeArray[PIPE_READ(nFilhoExec)][1]);

 					while((n_read = read(pipeArray[PIPE_WRITE(nFilhoExec)][0], result_buffer, 1024)) > 0){
							for(int i = 0; i < to_exec->needs_me_len; i++){
								to_send = to_exec->needs_me[i];
								write(pipeArray[PIPE_READ(to_send)][1], result_buffer, n_read);
							}
							write(output_des, result_buffer, n_read);
					}	

					for(int i = 0; i < to_exec->needs_me_len; i++){ // fechar os descritores para escrita
						to_send = to_exec->needs_me[i];
						close(pipeArray[PIPE_READ(to_send)][1]);
					}

					close(pipeArray[PIPE_WRITE(nFilhoExec)][0]);
					close(output_des);
					exit(0);		
				}
				else nFilhosCriados++;
			}
		}
	}
	

	
	for(int i=0; i<n_cmds; i++){
		close(pipeArray[PIPE_WRITE(i)][0]);
		close(pipeArray[PIPE_WRITE(i)][1]);
		close(pipeArray[PIPE_READ(i)][0]);
		close(pipeArray[PIPE_READ(i)][1]);
	}


	for(int i = 0; i<nFilhosCriados;  i++){
		wait(NULL);
	}
	kill(check_error, SIGKILL);

	char aux;
	if(read(f_error, &aux, 1) != 0) interrupt(1);
	else close(f_error);

	// Reescrita no ficheiro
	char *result_file = "result.nb";
	int n_read;
	int fid = open(result_file, O_CREAT | O_WRONLY, 0666);
	for (int i = 0; i < n_cmds; i++) {
		write(fid, cmds[i]->text, strlen(cmds[i]->text));
		write(fid, "\n", 1);
		write(fid, cmds[i]->full_cmd, strlen(cmds[i]->full_cmd));
		write(fid, "\n", 1);

		char res_buf;
		char output_file[100];
		sprintf(output_file, "%scomando%d.txt", TMP_PATH, i);

		int output_src = open(output_file, O_RDONLY);

		write(fid, SUP_DELIM, 3);
		write(fid, "\n", 1);
		while((n_read = read(output_src, &res_buf, 1)) > 0){
			write(fid, &res_buf, n_read);
		}
		write(fid, INF_DELIM, 3);
		write(fid, "\n",1);

		close(output_src);
	}
	close(fid);

	// Substituir .nb original pelo novo com os resultados
	rename("result.nb", argv[1]);

	removeFicheirosAuxiliares(nFilhoExec);
	
	return 0;
}